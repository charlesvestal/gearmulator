/* How fast is the DSP56300 emulator with the JIT turned off?
 *
 * iOS forbids JIT: asmjit can map an executable page, but the kernel kills the
 * process when generated code is executed. JE-8086 reaches real time there on
 * an interpreted ESP, and every other synth in this tree is DSP56300-based,
 * where dsp56kEmu already ships DSP::execInterpreter() selected at build time
 * by DSP56K_FORCE_INTERPRETER. Whether Osirus/Vavra/Xenia/NodalRed2x can follow
 * is one number per synth: the interpreter's real-time factor.
 *
 * Build the tree twice, with and without -DDSP56K_FORCE_INTERPRETER=1, and
 * compare. Cost is very nearly linear in emulated cycles, so the emulated core
 * clock -- which comes from each firmware's own PLL setup and cannot be read
 * off the EXTAL constant in the source -- is what separates these synths.
 *
 * Two things a real-time factor cannot see, and which the numbers are worthless
 * without: whether the thing is still making a sound, and whether an
 * underclocked DSP is quietly dropping voices. Hence the peak/rms line.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "baseLib/filesystem.h"
#include "dsp56kEmu/dspconfig.h"
#include "synthLib/device.h"

/* Only ONE of these is ever defined: see the CMakeLists for why the synth libs
 * cannot share a binary. */
#if BENCH_HAS_VIRUS
#include "virusLib/device.h"
#include "virusLib/romloader.h"
#endif
#if BENCH_HAS_XT
#include "xtLib/xtDevice.h"
#include "xtLib/xtRomLoader.h"
#endif
#if BENCH_HAS_MQ
#include "mqLib/device.h"
#include "mqLib/romloader.h"
#endif
#if BENCH_HAS_N2X
#include "n2xLib/n2xdevice.h"
#include "n2xLib/n2xromloader.h"
#endif

namespace
{
	std::unique_ptr<synthLib::Device> createDevice(const std::string& _synth, const std::string& _rom)
	{
		synthLib::DeviceCreateParams params;
		params.preferredSamplerate = 48000.0f;
		params.hostSamplerate = 48000.0f;

#if BENCH_HAS_VIRUS
		if(_synth == "virus" || _synth == "virusA" || _synth == "virusB" || _synth == "virusTI")
		{
			/* Virus A firmware is a PAIR of .mid files, not a .bin, so it can
			 * only be loaded through ROMLoader -- which is also what picks the
			 * model out of the files. Naming a .bin keeps the direct path. */
			const auto model = _synth == "virusA"  ? virusLib::DeviceModel::A
			                 : _synth == "virusB"  ? virusLib::DeviceModel::B
			                 : _synth == "virusTI" ? virusLib::DeviceModel::TI2
			                 : virusLib::DeviceModel::C;

			if(_rom.empty())
			{
				const auto rom = virusLib::ROMLoader::findROM(model);
				if(!rom.isValid())
				{
					fprintf(stderr, "no Virus ROM for that model in the current directory\n");
					return {};
				}
				params.romData = rom.getRomFileData();
				params.romName = rom.getFilename();
				params.customData = static_cast<uint32_t>(rom.getModel());
			}
			else
			{
				if(!baseLib::filesystem::readFile(params.romData, _rom) || params.romData.empty())
				{
					fprintf(stderr, "failed to read ROM '%s'\n", _rom.c_str());
					return {};
				}
				params.romName = _rom;
				/* The device model rides in customData, which defaults to 0,
				 * i.e. Virus A. Handed a Virus C ROM under that, the firmware
				 * clocks itself at 36 MHz and walks into a DEBUG instruction
				 * rather than booting, so process() never returns. */
				params.customData = static_cast<uint32_t>(model);
			}
			return std::make_unique<virusLib::Device>(params);
		}
#endif
#if BENCH_HAS_XT
		if(_synth == "xt")
		{
			/* The XT ships as two 128k EPROM halves that the loader interleaves
			 * into one 256k image, so this takes a DIRECTORY, not a file. */
			const auto rom = xt::RomLoader::findROM();
			if(!rom.isValid())
			{
				fprintf(stderr, "no XT ROM found in the current directory\n");
				return {};
			}
			params.romData = rom.getData();
			params.romName = rom.getFilename();
			return std::make_unique<xt::Device>(params);
		}
#endif
#if BENCH_HAS_MQ
		if(_synth == "mq")
		{
			const auto rom = mqLib::RomLoader::findROM();
			if(!rom.isValid())
			{
				fprintf(stderr, "no microQ ROM found in the current directory\n");
				return {};
			}
			params.romData = rom.getData();
			params.romName = rom.getFilename();
			return std::make_unique<mqLib::Device>(params);
		}
#endif
#if BENCH_HAS_N2X
		if(_synth == "n2x")
		{
			const auto rom = n2x::RomLoader::findROM();
			if(!rom.isValid())
			{
				fprintf(stderr, "no Nord Lead 2x ROM found in the current directory\n");
				return {};
			}
			params.romData = rom.data();
			params.romName = rom.getFilename();
			return std::make_unique<n2x::Device>(params);
		}
#endif
		fprintf(stderr, "unknown or unbuilt synth '%s'\n", _synth.c_str());
		return {};
	}
}

int main(int _argc, char* _argv[])
{
	if(_argc < 2)
	{
		fprintf(stderr, "usage: %s <virus|virusA|virusB|virusTI|xt|mq|n2x> [rom] [seconds] [voices] [dspClockPercent] [repeats]\n"
		                "  virus takes a ROM path; xt/mq/n2x find theirs in the current directory\n", _argv[0]);
		return 1;
	}

	setvbuf(stdout, nullptr, _IONBF, 0);

	const std::string synth = _argv[1];
	const std::string rom = _argc > 2 ? _argv[2] : std::string();
	const double seconds = _argc > 3 ? atof(_argv[3]) : 5.0;
	const auto voices = static_cast<uint8_t>(_argc > 4 ? atoi(_argv[4]) : 0);
	const auto clockPercent = static_cast<uint32_t>(_argc > 5 ? atoi(_argv[5]) : 100);
	const auto repeats = static_cast<uint32_t>(_argc > 6 ? atoi(_argv[6]) : 3);

	std::unique_ptr<synthLib::Device> device;
	try
	{
		device = createDevice(synth, rom);
	}
	catch(const std::exception& _e)
	{
		fprintf(stderr, "device creation failed: %s\n", _e.what());
		return 1;
	}

	if(!device)
		return 1;

	if(!device->isValid())
	{
		fprintf(stderr, "device invalid, the ROM was not accepted\n");
		return 1;
	}

	/* Every DSP56300 device here answers canModifyDspClock() -- the Virus as
	 * well as both Waldorfs -- and cost is near-linear in emulated cycles, so
	 * underclocking is the polyphony-for-CPU trade an interpreted build needs,
	 * using a shipped feature rather than a hack. NodalRed2x does not offer it. */
	if(clockPercent != 100)
	{
		/* Ask even when canModifyDspClock() says no, and report what happens.
		 * NodalRed2x implements setDspClockPercent() for both its DSPs yet
		 * never overrides canModifyDspClock(), which looks like an oversight
		 * hiding a usable lever. It is not: the call is accepted, the reported
		 * clock stays at 100%, and the render gets SLOWER (0.91x -> 0.44x at
		 * 50%), because on that device the ESAI clock sets the output rate --
		 * so a lower clock means more emulated work per second of audio, not
		 * less. The false is doing its job. */
		if(!device->canModifyDspClock())
			fprintf(stderr, "note: %s reports canModifyDspClock()==false; trying anyway\n", synth.c_str());
		if(!device->setDspClockPercent(clockPercent))
		{
			fprintf(stderr, "this device would not take a DSP clock of %u%%\n", clockPercent);
			return 1;
		}
	}

	const auto samplerate = device->getSamplerate();
	const auto* mode = dsp56k::g_jitSupported ? "JIT" : "INTERPRETER";

	printf("%s: %s, %.0f Hz, DSP clock %llu Hz", synth.c_str(), mode, samplerate,
		static_cast<unsigned long long>(device->getDspClockHz()));
	if(device->canModifyDspClock())
		printf(" @ %u%%", device->getDspClockPercent());
	printf(", rendering %.1f s\n", seconds);

	constexpr size_t blockSize = 256;

	/* Back EVERY channel the device declares, not just a stereo pair. The XT
	 * writes more than two and segfaults on the null pointers a partly filled
	 * array leaves behind. */
	const auto channelsOut = std::max(2u, device->getChannelCountOut());
	const auto channelsIn = std::max(2u, device->getChannelCountIn());

	std::vector<std::vector<float>> outBuffers(channelsOut, std::vector<float>(blockSize));
	std::vector<std::vector<float>> inBuffers(channelsIn, std::vector<float>(blockSize));

	synthLib::TAudioInputs inputs{};
	synthLib::TAudioOutputs outputs{};
	for(uint32_t i=0; i<channelsIn && i<inputs.size(); ++i)
		inputs[i] = inBuffers[i].data();
	for(uint32_t i=0; i<channelsOut && i<outputs.size(); ++i)
		outputs[i] = outBuffers[i].data();

	const auto& outL = outBuffers[0];
	const std::vector<synthLib::SMidiEvent> midiIn;
	std::vector<synthLib::SMidiEvent> midiOut;

	/* Boot the firmware and let the JIT compile before the clock starts, so
	 * what is timed is steady-state rendering and not one-off cost. */
	for(int i=0; i<400; ++i)
	{
		device->process(inputs, outputs, blockSize, midiIn, midiOut);
		midiOut.clear();
	}

	std::vector<synthLib::SMidiEvent> notesOn, notesOff;
	for(uint8_t i=0; i<voices; ++i)
	{
		const auto note = static_cast<uint8_t>(48 + i * 3);
		synthLib::SMidiEvent on(synthLib::MidiEventSource::Host);
		on.a = 0x90; on.b = note; on.c = 100;
		notesOn.push_back(on);
		synthLib::SMidiEvent off(synthLib::MidiEventSource::Host);
		off.a = 0x80; off.b = note; off.c = 0;
		notesOff.push_back(off);
	}

	float peak = 0.0f;
	double sumSq = 0.0;
	size_t sumN = 0;

	/* Take the FASTEST of several passes, not one timing. An interpreted pass
	 * takes seconds, so on a machine that is doing anything else it collides
	 * with that work and the figure swings by 4x -- 4 held voices read 0.32x,
	 * 0.52x, 0.77x, 0.95x, 1.34x and 1.38x on one box at load average 12, in
	 * that order, which is not something a single sample can be trusted to
	 * represent. The minimum is the closest estimate of the undisturbed cost.
	 * Prefer an idle machine anyway; this only stops a busy one lying. */
	const auto total = static_cast<size_t>(seconds * samplerate);
	double elapsed = 0.0;

	for(uint32_t pass=0; pass<std::max(1u, repeats); ++pass)
	{
		peak = 0.0f;
		sumSq = 0.0;
		sumN = 0;

		/* Retrigger every pass. A held note decays, and a decayed voice is
		 * cheaper, so without this the FASTEST pass is simply the one where the
		 * sound has died -- the peak/rms line read SILENT while the real-time
		 * factor looked like the best result yet. */
		if(!notesOn.empty())
		{
			device->process(inputs, outputs, blockSize, notesOff, midiOut);
			midiOut.clear();
			device->process(inputs, outputs, blockSize, notesOn, midiOut);
			midiOut.clear();
			for(int i=0; i<200; ++i)	// untimed: let the voices reach steady state
			{
				device->process(inputs, outputs, blockSize, midiIn, midiOut);
				midiOut.clear();
			}
		}

		const auto start = std::chrono::steady_clock::now();
		for(size_t done=0; done<total; done+=blockSize)
		{
			device->process(inputs, outputs, blockSize, midiIn, midiOut);
			midiOut.clear();
			for(size_t i=0; i<blockSize; ++i)
			{
				peak = std::max(peak, std::abs(outL[i]));
				sumSq += static_cast<double>(outL[i]) * outL[i];
				++sumN;
			}
		}
		const auto e = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		if(pass == 0 || e < elapsed)
			elapsed = e;
	}

	printf("%s: %s, %u voices, %u%% clock: %.2f s wall (best of %u) for %.1f s audio => %.3fx real-time\n",
		synth.c_str(), mode, voices, device->canModifyDspClock() ? device->getDspClockPercent() : 100u,
		elapsed, std::max(1u, repeats), seconds, seconds / elapsed);
	printf("peak %.4f, rms %.4f (%s)\n", peak, sumN ? std::sqrt(sumSq / static_cast<double>(sumN)) : 0.0,
		peak > 0.0001f ? "AUDIO" : "SILENT");

	return 0;
}
