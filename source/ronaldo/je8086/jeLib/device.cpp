#include "device.h"

#include "je8086.h"

#include <cstdlib>
#include "jeThread.h"
#include "jePipeline.h"
#include "synthLib/midiToSysex.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#endif

namespace
{
#ifdef __APPLE__
	/* Diagnostics for the iOS build: an AUv3 has no console, so this goes to a
	 * file in the extension's own container, which devicectl can copy off. */
	void jeDiag(const char* _fmt, ...)
	{
		static FILE* f = [] () -> FILE*
		{
			const char* home = getenv("HOME");
			if (!home) return nullptr;
			std::string path = std::string(home) + "/Documents/je_diag.txt";
			FILE* h = fopen(path.c_str(), "w");
			if (h) setvbuf(h, nullptr, _IOLBF, 0);
			return h;
		}();
		if (!f) return;
		static std::mutex m;
		std::scoped_lock lock(m);
		va_list args;
		va_start(args, _fmt);
		vfprintf(f, _fmt, args);
		va_end(args);
		fputc('\n', f);
		fflush(f);

		/* Also to stderr, so a run whose file never gets copied off the device
		 * still leaves its numbers in the console capture. Losing a 25-minute
		 * soak to a missed copy step is not a mistake worth making twice. */
		va_start(args, _fmt);
		vfprintf(stderr, _fmt, args);
		va_end(args);
		fputc('\n', stderr);
	}
#else
	inline void jeDiag(const char*, ...) {}
#endif

#ifdef __APPLE__
	/* NSProcessInfo.thermalState: 0 nominal, 1 fair, 2 serious, 3 critical.
	 * The engine's own render loop slows by a third after ~30 s of sustained
	 * load, long after the notes stop, which is what thermal throttling and
	 * efficiency-core demotion both look like from inside. This separates them:
	 * if the state climbs as ns/sample climbs, it is heat. Reached through the
	 * ObjC runtime so this stays a .cpp. */
	long jeThermalState()
	{
		static Class cls = objc_getClass("NSProcessInfo");
		if (!cls) return -1;
		static id pi = reinterpret_cast<id(*)(Class, SEL)>(objc_msgSend)(cls, sel_registerName("processInfo"));
		if (!pi) return -1;
		return reinterpret_cast<long(*)(id, SEL)>(objc_msgSend)(pi, sel_registerName("thermalState"));
	}
#endif

	inline float dspWordToFloat(const uint32_t _d)
	{
		constexpr float scale = 1.0f / 8388608.0f;
		const auto signExtended = static_cast<int32_t>(_d << 8) >> 8;
		return static_cast<float>(signExtended) * scale;
	}
}

namespace jeLib
{
	/* The pipeline hands one sample over for every sample rendered, taken this
	 * many samples late. It only has to cover what is in flight; two is enough,
	 * and a constant delay is what makes the output bit-exact against serial. */
	static constexpr int64_t g_pipelineDelaySamples = 2;

	constexpr uint8_t g_paramPageMasterVolume = 6;
	constexpr uint8_t g_paramIndexMasterVolume = 0;

	Device::Device(const synthLib::DeviceCreateParams& _params) : synthLib::Device(_params)
	{
		const auto ramDataFilename = _params.homePath.empty() ? "ram_dump.bin" : _params.homePath + "/roms/ram_dump.bin";
		m_je8086.reset(new Je8086(_params.romData, ramDataFilename));

		if (m_je8086->hasDoneFactoryReset())
		{
			m_je8086.reset();
			m_je8086.reset(new Je8086(_params.romData, ramDataFilename));
		}

		/* Opt in to the parallel ASIC pipeline (see jePipeline.h). Off unless asked
		 * for, and only worth asking for where one core cannot render the chain in
		 * real time -- an SBC, not a desktop. Measured on a Pi 4 through a CLAP
		 * host: 0.7x real time without it, which is unusable; 1.8x with it.
		 *
		 * The count arrives through DeviceCreateParams, which the plugin fills in
		 * from its own settings. It has to be decided here, before the engine
		 * thread exists, because the pipeline delivers audio on a fixed delay that
		 * forms part of the latency we report. */
		if (_params.dspThreads > 1)
		{
			std::vector<int> bounds;
			for (uint32_t i = 1; i < _params.dspThreads && i < 4; ++i)
				bounds.push_back(static_cast<int>(i));
			jeDiag("[je] dspThreads=%u -> pipeline with %zu stage boundaries, delay %lld",
			       _params.dspThreads, bounds.size(), (long long)g_pipelineDelaySamples);
			m_je8086->requestParallelPipeline(bounds, {}, g_pipelineDelaySamples);
		}
		/* Local override for benchmarking and the Move build, which know the
		 * hardware and do not go through the plugin settings. Not upstreamed. */
		/* Without the JIT the ESP cores are interpreted at roughly 15x the cost,
		 * and one thread renders at about a third of real time -- the audio
		 * thread then blocks on an empty ring and the host sees a pegged DSP
		 * meter and silence. The pipeline is not a tuning option in that build,
		 * it is the only way the device runs at all, so ask for it here rather
		 * than depend on a plugin setting reaching us. */
		/* JE_BOUNDS is the standalone benchmark's own fork/thread pipeline, which
		 * lives outside Device; JE_NO_AUTO_PIPELINE is the explicit opt-out.
		 * Building a second pipeline underneath either of them deadlocks. */
		else if (jeLib::devices::jeEspInterp() && !getenv("JE_PIPELINE")
		         && !getenv("JE_BOUNDS") && !getenv("JE_NO_AUTO_PIPELINE"))
		{
			jeDiag("[je] interpreted ESP and no thread count from the host -> forcing the 4-stage pipeline");
			m_je8086->requestParallelPipeline({1, 2, 3}, {}, g_pipelineDelaySamples);
		}
		else if (const char* bounds = getenv("JE_PIPELINE"))
		{
			const auto parse = [](const char* _s)
			{
				std::vector<int> v;
				if (!_s) return v;
				int n = 0; bool any = false;
				for (const char* c = _s;; ++c)
				{
					if (*c >= '0' && *c <= '9') { n = n * 10 + (*c - '0'); any = true; }
					else { if (any) v.push_back(n); n = 0; any = false; if (!*c) break; }
				}
				return v;
			};
			const auto latency = getenv("JE_PIPELINE_LATENCY") ? atoll(getenv("JE_PIPELINE_LATENCY")) : 64;
			m_je8086->requestParallelPipeline(parse(bounds), parse(getenv("JE_PIPELINE_CORES")), latency);
		}

		jeDiag("[je] dspThreads from host = %u", _params.dspThreads);

		jeDiag("[je] esp jit: %s", jeLib::devices::jeEspInterp() ? "OFF (interpreted)" : "on");

#ifdef __APPLE__
		/* How many PERFORMANCE cores are there really? M-series iPads are binned
		 * -- some ship 3 P-cores, not 4 -- and the pipeline must not run more hot
		 * threads than the P cluster has, or one stage is permanently on an E
		 * core or preempting the host's audio thread. */
		{
			auto sysctlInt = [](const char* _name) -> int
			{
				int v = 0; size_t sz = sizeof(v);
				return sysctlbyname(_name, &v, &sz, nullptr, 0) == 0 ? v : -1;
			};
			jeDiag("[je] cores: %d total, %d performance, %d efficiency",
			       sysctlInt("hw.physicalcpu"),
			       sysctlInt("hw.perflevel0.physicalcpu"),
			       sysctlInt("hw.perflevel1.physicalcpu"));
		}
#endif

		m_thread.reset(new JeThread(*m_je8086));

		m_paramChangedListener.set(m_sysexRemote.evParamChanged, [this](const uint8_t _page, const uint8_t _index, const int32_t& _value)
		{
			onParamChanged(_page, _index, _value);
		});

		m_buttonChangedListener.set(m_sysexRemote.evButtonChanged, [this](const uint32_t _buttonIndex, const bool _pressed)
		{
			m_je8086->setButton(static_cast<devices::SwitchType>(_buttonIndex), _pressed);
		});

		// inform UI about default master volume
		createMasterVolumeMessage(m_midiOut);
	}

	Device::~Device()
	{
		m_thread.reset();
		m_je8086.reset();
	}

	float Device::getSamplerate() const
	{
		return 88200.0f;
	}

	bool Device::isValid() const
	{
		return true;
	}

	bool Device::getState(std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		std::vector<synthLib::SMidiEvent> results;

		if (!m_state.createSystemDump(results.emplace_back(synthLib::MidiEventSource::Device)))
			results.pop_back();

		createMasterVolumeMessage(results);

		if (!m_state.createTempPerformanceDumps(results))
			return false;

		for (const auto& result : results)
			_state.insert(_state.end(), result.sysex.begin(), result.sysex.end());
		return true;
	}

	bool Device::setState(const std::vector<uint8_t>& _state, synthLib::StateType _type)
	{
		if (_state.empty())
			return false;

		synthLib::SysexBuffer stateBuf(_state.begin(), _state.end());
		synthLib::SysexBufferList messages;
		synthLib::MidiToSysex::splitMultipleSysex(messages, stateBuf);

		if (messages.empty())
			return false;

		m_masterVolume = -1.0f;

		for (auto& message : messages)
		{
			synthLib::SMidiEvent e(synthLib::MidiEventSource::Host);
			e.sysex = std::move(message);

			// let the state receive it directly, the reason is that a frozen plugin is never processed and if the DSP
			// is never processed, the state will be lost
			m_state.receive(e.sysex);

			if (!m_sysexRemote.receive(e.sysex))
				m_midiIn.emplace_back(e);
		}

		// if master volume was not part of the state, set it to 1.0f to keep compatibility with older versions that did not store it
		if (m_masterVolume < 0)
			m_masterVolume = 1.0f;

		// feed master volume to the UI directly because there is no request message for it
		createMasterVolumeMessage(m_midiOut);

		return true;
	}

	uint32_t Device::getChannelCountIn()
	{
		return 2;
	}

	uint32_t Device::getChannelCountOut()
	{
		return 2;
	}

	bool Device::setDspClockPercent(uint32_t _percent)
	{
		return false;
	}

	uint32_t Device::getDspClockPercent() const
	{
		return 100;
	}

	uint64_t Device::getDspClockHz() const
	{
		return 88'000'000;
	}

	uint32_t Device::getMaxDspThreads() const
	{
		return 4;	// H8S+ASIC0 | ASIC1 | ASIC2 | ASIC3
	}

	uint32_t Device::getInternalLatencyMidiToOutput() const
	{
		// 4.5 ms, plus the pipeline's fixed delivery delay when it is running.
		return static_cast<uint32_t>(getSamplerate() * 4.5f / 1000.0f) + pipelineDelay();
	}

	uint32_t Device::getInternalLatencyInputToOutput() const
	{
		/* Only our own delay is reported here. Whatever the audio path costs
		 * without the pipeline is unchanged and unmeasured, so it stays 0. */
		return pipelineDelay();
	}

	uint32_t Device::pipelineDelay() const
	{
		return m_je8086 && m_je8086->hasParallelPipeline() ? static_cast<uint32_t>(g_pipelineDelaySamples) : 0;
	}

	void Device::readMidiOut(std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		if (_midiOut.empty())
			std::swap(m_midiOut, _midiOut);
		else
			_midiOut.insert(_midiOut.end(), m_midiOut.begin(), m_midiOut.end());

		m_state.receive(_midiOut);
	}

	void Device::processAudio(const synthLib::TAudioInputs& _inputs, const synthLib::TAudioOutputs& _outputs, const size_t _samples)
	{
		/* We are on the host's audio thread here, and it is the only place we ever
		 * see how the host schedules it. The pipeline's workers mirror it one step
		 * below; without that the host's realtime thread blocks on SCHED_OTHER
		 * workers, which is priority inversion and sounds exactly like the plugin
		 * being too slow. No-op when there is no pipeline or no realtime host. */
		pipelineAdoptHostSchedule();

#ifdef __APPLE__
		/* Self-test load. The measurement that matters is the engine under
		 * VOICES, and on iOS that otherwise needs a person holding a chord in a
		 * host. With JE_SELFTEST_CHORD set, the standalone plays one to itself
		 * two seconds after boot and holds it, so launching the app is enough to
		 * produce a loaded figure in the diagnostics. Diagnostic build only. */
		{
			static const bool selfTest = getenv("JE_SELFTEST_CHORD") != nullptr;
			if (selfTest && !m_selfTestDone)
			{
				m_selfTestSamples += _samples;
				if (m_selfTestSamples > static_cast<uint64_t>(getSamplerate() * 2.0f))
				{
					for (const uint8_t note : { 48, 52, 55, 60, 64, 67 })
					{
						synthLib::SMidiEvent ev(synthLib::MidiEventSource::Host);
						ev.a = 0x90; ev.b = note; ev.c = 100;
						m_midiIn.push_back(ev);
					}
					m_selfTestDone = true;
					jeDiag("[je] self-test: six-note chord held from here on");
				}
			}
		}
#endif

		m_thread->processSamples(static_cast<uint32_t>(_samples), getExtraLatencySamples(), m_midiIn, m_midiOut);
		m_midiIn.clear();

		auto& sampleBuffer = m_thread->getSampleBuffer();

		const auto availBefore = sampleBuffer.size();

		float peak = 0.0f;

		/* NEVER block the host's audio thread. pop_front() waits when the ring is
		 * empty, so an engine that falls behind does not merely go quiet -- it
		 * stalls the host's whole render graph, and in AUM nothing else made a
		 * sound until the app was force-quit. Underrun into silence instead and
		 * say so in the diagnostics. */
		const auto usable = std::min(availBefore, _samples);

		for (size_t i=0; i<usable; ++i)
		{
			const auto s = sampleBuffer.pop_front();

			_outputs[0][i] = dspWordToFloat(s.first) * m_masterVolume;
			_outputs[1][i] = dspWordToFloat(s.second) * m_masterVolume;

			peak = std::max(peak, std::abs(_outputs[0][i]));
		}

		for (size_t i=usable; i<_samples; ++i)
		{
			_outputs[0][i] = 0.0f;
			_outputs[1][i] = 0.0f;
		}

		m_diagUnderrun += _samples - usable;

		/* Starvation and silence look identical from the outside, so report both.
		 * Counting refills per CALL misses everything the engine produced between
		 * callbacks -- the ring depth across the whole interval is the only
		 * honest measure, so produced = consumed + (depth now - depth then). */
		{
			const auto availAfter = sampleBuffer.size();
			m_diagRequested += _samples;
			m_diagPeak = std::max(m_diagPeak, peak);
			m_diagMinAvail = std::min(m_diagMinAvail, availBefore);
			m_diagCalls++;

			const auto now = std::chrono::steady_clock::now();
			if (m_diagT0 == std::chrono::steady_clock::time_point{})
			{
				m_diagT0 = now;
				m_diagAvailMark = availAfter;
			}
			const auto secs = std::chrono::duration<double>(now - m_diagT0).count();
			if (secs >= 1.0)
			{
				const double nsPerSample = m_thread->takeNsPerSample();
				const auto produced = static_cast<int64_t>(m_diagRequested)
				                    + static_cast<int64_t>(availAfter) - static_cast<int64_t>(m_diagAvailMark);
				jeDiag("[je] %.2fs: block %zu, host wanted %llu/s, engine made %lld/s (%.2fx of demand), ring min %zu now %zu, peak %.4f, underrun %llu, lat %u/%u, pending %zu, carry %u, dropped %llu, %.0f ns/sample (%.2fx capacity), thermal %ld",
				       secs, _samples,
				       (unsigned long long)(static_cast<double>(m_diagRequested) / secs),
				       (long long)(static_cast<double>(produced) / secs),
				       m_diagRequested ? static_cast<double>(produced) / static_cast<double>(m_diagRequested) : 0.0,
				       m_diagMinAvail, availAfter, m_diagPeak, (unsigned long long)m_diagUnderrun,
				       m_thread->getCurrentLatency(), getExtraLatencySamples(), m_thread->getPendingJobs(), m_thread->getCarry(), (unsigned long long)m_thread->getDropped(),
				       nsPerSample, nsPerSample > 0.0 ? (1e9 / getSamplerate()) / nsPerSample : 0.0, jeThermalState());
				m_diagT0 = now;
				m_diagAvailMark = availAfter;
				m_diagRequested = m_diagCalls = m_diagUnderrun = 0;
				m_diagPeak = 0.0f;
				m_diagMinAvail = ~size_t(0);
			}
		}
	}

	bool Device::sendMidi(const synthLib::SMidiEvent& _ev, std::vector<synthLib::SMidiEvent>& _response)
	{
		if (!m_sysexRemote.receive(_ev.sysex))
			m_midiIn.emplace_back(_ev);
		m_state.receive(_ev);
		return true;
	}

	void Device::onParamChanged(uint8_t/* _page*/, uint8_t/* _index*/, const int32_t _value)
	{
		m_masterVolume = static_cast<float>(_value) * 0.01f;
	}

	void Device::createMasterVolumeMessage(std::vector<synthLib::SMidiEvent>& _messages) const
	{
		SysexRemoteControl::sendSysexParameter(_messages, g_paramPageMasterVolume, g_paramIndexMasterVolume, static_cast<int32_t>(m_masterVolume * 100.0f));
	}
}
