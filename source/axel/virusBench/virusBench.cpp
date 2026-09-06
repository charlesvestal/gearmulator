/* How fast is the DSP56300 emulator with the JIT turned off?
 *
 * iOS forbids JIT: asmjit can map an executable page, but the kernel kills the
 * process when generated code is executed. JE-8086 reaches real time there on
 * an interpreted ESP, and every other synth in this tree is DSP56300-based,
 * where dsp56kEmu already ships DSP::execInterpreter() selected at build time
 * by DSP56K_FORCE_INTERPRETER. Whether Osirus/Vavra/Xenia can follow is one
 * number: the interpreter's real-time factor.
 *
 * Times a fixed span of audio through virusLib::Device and reports it. Build
 * the tree twice, with and without -DDSP56K_FORCE_INTERPRETER=1, and compare.
 * The DSP runs its loop whether or not a note is held, so an idle device is a
 * fair measure of emulation throughput -- there is no note to play here.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "virusLib/device.h"

#include "baseLib/filesystem.h"
#include "dsp56kEmu/dspconfig.h"

int main(int _argc, char* _argv[])
{
	if(_argc < 2)
	{
		fprintf(stderr, "usage: %s <rom.bin> [seconds] [voices] [dspClockPercent]\n", _argv[0]);
		return 1;
	}

	const double seconds = _argc > 2 ? atof(_argv[2]) : 5.0;
	const auto voices = static_cast<uint8_t>(_argc > 3 ? atoi(_argv[3]) : 0);
	const auto clockPercent = static_cast<uint32_t>(_argc > 4 ? atoi(_argv[4]) : 100);

	setvbuf(stdout, nullptr, _IONBF, 0);

	/* The device model rides in customData, and DeviceCreateParams defaults it
	 * to 0 -- Virus A. Hand a Virus C ROM to that and the firmware clocks
	 * itself at 36 MHz and walks into a DEBUG instruction instead of booting,
	 * so process() never returns. */
	synthLib::DeviceCreateParams params;
	if(!baseLib::filesystem::readFile(params.romData, _argv[1]) || params.romData.empty())
	{
		fprintf(stderr, "failed to read ROM '%s'\n", _argv[1]);
		return 1;
	}
	params.romName = _argv[1];
	params.customData = static_cast<uint32_t>(virusLib::DeviceModel::C);
	params.preferredSamplerate = 48000.0f;
	params.hostSamplerate = 48000.0f;
	printf("ROM %s, %zu bytes\n", _argv[1], params.romData.size());

	std::unique_ptr<virusLib::Device> device;
	try
	{
		device.reset(new virusLib::Device(params));
	}
	catch(const std::exception& _e)
	{
		fprintf(stderr, "device creation failed: %s\n", _e.what());
		return 1;
	}

	if(!device->isValid())
	{
		fprintf(stderr, "device invalid, the ROM was not accepted\n");
		return 1;
	}

	/* Every DSP56300 device here answers canModifyDspClock() -- the Virus too,
	 * not just the Waldorfs -- and the emulator's cost is very nearly linear in
	 * emulated cycles. Underclocking is therefore the polyphony-for-CPU trade
	 * an interpreted build needs, and it is a shipped feature, not a hack. */
	if(clockPercent != 100 && !device->setDspClockPercent(clockPercent))
	{
		fprintf(stderr, "this device would not take a DSP clock of %u%%\n", clockPercent);
		return 1;
	}

	const auto samplerate = device->getSamplerate();
	printf("%s, %.0f Hz, DSP clock %u%% (%llu Hz), rendering %.1f s\n", dsp56k::g_jitSupported ? "JIT" : "INTERPRETER",
		samplerate, device->getDspClockPercent(), static_cast<unsigned long long>(device->getDspClockHz()), seconds);

	constexpr size_t blockSize = 256;
	std::vector<float> outL(blockSize), outR(blockSize), inL(blockSize), inR(blockSize);
	const synthLib::TAudioInputs inputs{inL.data(), inR.data()};
	const synthLib::TAudioOutputs outputs{outL.data(), outR.data()};
	const std::vector<synthLib::SMidiEvent> midiIn;
	std::vector<synthLib::SMidiEvent> midiOut;

	/* Boot the firmware and let the JIT compile before the clock starts, so
	 * what is timed is steady-state rendering and not one-off cost. */
	for(int i=0; i<400; ++i)
	{
		device->process(inputs, outputs, blockSize, midiIn, midiOut);
		midiOut.clear();
		if((i % 100) == 0)
			printf("warmup %d/400\n", i);
	}
	printf("warmup done\n");

	/* A DSP56300 executes a fixed number of cycles per sample whatever the
	 * patch is doing, so held notes should cost nothing extra -- but only if
	 * the firmware's idle path is not a cheap-to-emulate WAIT. Pass a note
	 * count to check that rather than assume it. */
	std::vector<synthLib::SMidiEvent> notes;
	for(uint8_t i=0; i<voices; ++i)
	{
		synthLib::SMidiEvent on(synthLib::MidiEventSource::Host);
		on.a = 0x90; on.b = static_cast<uint8_t>(48 + i * 3); on.c = 100;
		notes.push_back(on);
	}
	if(!notes.empty())
	{
		device->process(inputs, outputs, blockSize, notes, midiOut);
		midiOut.clear();
		for(int i=0; i<200; ++i)	// let the voices reach steady state
		{
			device->process(inputs, outputs, blockSize, midiIn, midiOut);
			midiOut.clear();
		}
	}
	printf("%u voices held\n", voices);

	float peak = 0.0f;
	double sumSq = 0.0;
	size_t sumN = 0;

	const auto total = static_cast<size_t>(seconds * samplerate);
	const auto start = std::chrono::steady_clock::now();
	for(size_t done=0; done<total; done+=blockSize)
	{
		device->process(inputs, outputs, blockSize, midiIn, midiOut);
		midiOut.clear();
		/* An underclocked DSP that has gone silent is not a faster synth, and
		 * a real-time factor cannot tell the difference. */
		for(size_t i=0; i<blockSize; ++i)
		{
			peak = std::max(peak, std::abs(outL[i]));
			sumSq += static_cast<double>(outL[i]) * outL[i];
			++sumN;
		}
	}
	const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	printf("%s, %u voices, %u%% clock: %.2f s wall for %.1f s audio => %.3fx real-time\n",
		dsp56k::g_jitSupported ? "JIT" : "INTERPRETER", voices, device->getDspClockPercent(), elapsed, seconds, seconds / elapsed);

	printf("peak %.4f, rms %.4f (%s)\n", peak, sumN ? std::sqrt(sumSq / static_cast<double>(sumN)) : 0.0,
		peak > 0.0001f ? "AUDIO" : "SILENT");

	return 0;
}
