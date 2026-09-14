#include "n2xhardware.h"

#include <chrono>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <sstream>

#include "n2xromloader.h"
#include "dsp56kBase/threadtools.h"
#include "synthLib/deviceException.h"

namespace n2x
{
	constexpr uint32_t g_syncEsaiFrameRate = 16;
	constexpr uint32_t g_syncHaltDspEsaiThreshold = 32;

	static_assert((g_syncEsaiFrameRate & (g_syncEsaiFrameRate - 1)) == 0, "esai frame sync rate must be power of two");
	static_assert(g_syncHaltDspEsaiThreshold >= g_syncEsaiFrameRate * 2, "esai DSP halt threshold must be greater than two times the sync rate");

	Rom initRom(const std::vector<uint8_t>& _romData, const std::string& _romName)
	{
		if(_romData.empty())
			return RomLoader::findROM();
		Rom rom(_romData, _romName);
		if(rom.isValid())
			return rom;
		return RomLoader::findROM();
	}

	Hardware::Hardware(const std::vector<uint8_t>& _romData, const std::string& _romName)
		: m_rom(initRom(_romData, _romName))
		, m_uc(*this, m_rom)
		, m_dspA(*this, m_uc.getHdi08A(), 0)
		, m_dspB(*this, m_uc.getHdi08B(), 1)
		, m_samplerateInv(1.0 / g_samplerate)
		, m_semDspAtoB(2)
	{
		if(!m_rom.isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing, "No firmware found, expected firmware .bin with a size of " + std::to_string(Rom::MySize) + " bytes");

		m_dspA.getPeriph().getEsai().setCallback([this](dsp56k::Audio*){ onEsaiCallbackA(); });
		m_dspB.getPeriph().getEsai().setCallback([this](dsp56k::Audio*){ onEsaiCallbackB(); });

		m_ucThread.reset(new std::thread([this]
		{
			ucThreadFunc();
		}));

		while(!m_bootFinished)
			processAudio(8,8);
		m_midiOffsetCounter = 0;
	}

	Hardware::~Hardware()
	{
		m_destroy = true;

		while(m_destroy)
			processAudio(8,64);

		m_dspA.terminate();
		m_dspB.terminate();

		m_esaiFrameIndex = 0;
		m_esaiLatency = 0;

		while(!m_dspA.getDSPThread().runThread() || !m_dspB.getDSPThread().runThread())
		{
			// DSP A waits for space to push to DSP B
			m_semDspAtoB.notify();

			if(m_dspB.getPeriph().getEsai().getAudioInputs().full())
				m_dspB.getPeriph().getEsai().getAudioInputs().pop_front();

			// DSP B waits for ESAI rate limiting and for DSP A to provide audio data
			m_haltDSPSem.notify(999999);
			if(m_dspA.getPeriph().getEsai().getAudioOutputs().empty())
				m_dspA.getPeriph().getEsai().getAudioOutputs().push_back({});
		}

		m_ucThread->join();
	}

	bool Hardware::isValid() const
	{
		return m_rom.isValid();
	}

	void Hardware::processUC()
	{
		if(m_remainingUcCycles <= 0)
			syncUCtoDSP();

		const auto deltaCycles = m_uc.exec();

		if(m_esaiFrameIndex > 0)
			m_remainingUcCycles -= static_cast<int64_t>(deltaCycles);
	}

	void Hardware::processAudio(uint32_t _frames, const uint32_t _latency)
	{
#if N2X_AUDIO_TRACE
		using TraceClock = std::chrono::steady_clock;

		const auto traceCallbackBegin = TraceClock::now();
		const auto traceFrames = _frames;
#endif

		getMidi().process(_frames);

		ensureBufferSize(_frames);

		dsp56k::TWord* outputs[12]{nullptr};
		outputs[1] = &m_audioOutputs[0].front();
		outputs[0] = &m_audioOutputs[1].front();
		outputs[3] = &m_audioOutputs[2].front();
		outputs[2] = &m_audioOutputs[3].front();
		outputs[4] = m_dummyOutput.data();
		outputs[5] = m_dummyOutput.data();
		outputs[6] = m_dummyOutput.data();
		outputs[7] = m_dummyOutput.data();
		outputs[8] = m_dummyOutput.data();
		outputs[9] = m_dummyOutput.data();
		outputs[10] = m_dummyOutput.data();
		outputs[11] = m_dummyOutput.data();

		auto& esaiB = m_dspB.getPeriph().getEsai();

#if N2X_AUDIO_TRACE
		{
			// sample the cushion BEFORE consuming anything from it
			const auto depth = static_cast<uint32_t>(esaiB.getAudioOutputs().size());
			m_waitStats.ringDepthSum += depth;
			m_waitStats.ringDepthMin = std::min(m_waitStats.ringDepthMin, depth);
			m_waitStats.ringDepthMax = std::max(m_waitStats.ringDepthMax, depth);
			m_waitStats.latencyFrames = _latency;
		}
#endif

//		LOG("B out " << esaiB.getAudioOutputs().size() << ", A out " << esaiA.getAudioOutputs().size() << ", B in " << esaiB.getAudioInputs().size());

		while (_frames)
		{
			const auto processCount = std::min(_frames, static_cast<uint32_t>(64));
			_frames -= processCount;

			advanceSamples(processCount, _latency);

			const auto requiredSize = processCount > 8 ? processCount - 8 : 0;

#if N2X_AUDIO_TRACE
			++m_waitStats.chunks;
#endif

			if(esaiB.getAudioOutputs().size() < requiredSize)
			{
				// reduce thread contention by waiting for output buffer to be full enough to let us grab the data without entering the read mutex too often

#if N2X_AUDIO_TRACE
				// clock read is on the blocking path only; the fast path pays a branch
				const auto traceWaitBegin = TraceClock::now();

				// how many frames were actually missing when we decided to wait
				const auto available = esaiB.getAudioOutputs().size();
				const auto shortfall = requiredSize > available ? requiredSize - available : 0;
				const auto expectedUsec = static_cast<double>(shortfall) * 1'000'000.0 / static_cast<double>(g_samplerate);
#endif

				std::unique_lock uLock(m_requestedFramesAvailableMutex);
				m_requestedFrames = requiredSize;
				m_requestedFramesAvailableCv.wait(uLock, [&]()
				{
					if(esaiB.getAudioOutputs().size() < requiredSize)
						return false;
					m_requestedFrames = 0;
					return true;
				});

#if N2X_AUDIO_TRACE
				const auto waitUsec = std::chrono::duration<double, std::micro>(TraceClock::now() - traceWaitBegin).count();

				const auto excessUsec = waitUsec - expectedUsec;

				++m_waitStats.chunksBlocked;
				m_waitStats.waitUsecTotal += waitUsec;
				m_waitStats.waitUsecMax = std::max(m_waitStats.waitUsecMax, waitUsec);
				m_waitStats.waitExpectedUsecTotal += expectedUsec;
				m_waitStats.waitExcessUsecTotal += excessUsec;
				m_waitStats.waitExcessUsecMax = std::max(m_waitStats.waitExcessUsecMax, excessUsec);
#endif
			}

			// read output of DSP B to regular audio output
			esaiB.processAudioOutputInterleaved(outputs, processCount);

			outputs[0] += processCount;
			outputs[1] += processCount;
			outputs[2] += processCount;
			outputs[3] += processCount;
		}

#if N2X_AUDIO_TRACE
		reportAudioWaitStats(traceFrames, std::chrono::duration<double, std::micro>(TraceClock::now() - traceCallbackBegin).count());
#endif
	}

#if N2X_AUDIO_TRACE
	void Hardware::reportAudioWaitStats(const uint32_t _hostBlockFrames, const double _busyUsec)
	{
		auto& s = m_waitStats;

		++s.callbacks;
		s.busyUsecTotal += _busyUsec;
		s.framesSinceReport += _hostBlockFrames;
		s.hostBlockMin = std::min(s.hostBlockMin, _hostBlockFrames);
		s.hostBlockMax = std::max(s.hostBlockMax, _hostBlockFrames);

		// ~2 s of output between reports, so the log stays readable while playing
		constexpr uint64_t kStatsIntervalFrames = 2 * g_samplerate;

		if(s.framesSinceReport < kStatsIntervalFrames)
			return;

		// Budget is the wall time the host block is WORTH. Anything at or above
		// 100% is an overrun: the callback took longer than the audio it returned.
		const auto audioUsec = static_cast<double>(s.framesSinceReport) * 1'000'000.0 / static_cast<double>(g_samplerate);
		const auto loadPercent = 100.0 * s.busyUsecTotal / audioUsec;
		const auto waitPercent = s.busyUsecTotal > 0.0 ? 100.0 * s.waitUsecTotal / s.busyUsecTotal : 0.0;
		const auto blockedPercent = s.chunks ? 100.0 * static_cast<double>(s.chunksBlocked) / static_cast<double>(s.chunks) : 0.0;
		const auto waitAvgUsec = s.chunksBlocked ? s.waitUsecTotal / static_cast<double>(s.chunksBlocked) : 0.0;
		const auto expAvgUsec = s.chunksBlocked ? s.waitExpectedUsecTotal / static_cast<double>(s.chunksBlocked) : 0.0;
		const auto excessAvgUsec = s.chunksBlocked ? s.waitExcessUsecTotal / static_cast<double>(s.chunksBlocked) : 0.0;

		std::stringstream line;
		line << "audio: blocks=" << s.hostBlockMin << ".." << s.hostBlockMax
			<< " callbacks=" << s.callbacks
			<< " load=" << static_cast<int>(loadPercent + 0.5) << "%"
			<< " | chunks=" << s.chunks
			<< " blocked=" << s.chunksBlocked
			<< " (" << static_cast<int>(blockedPercent + 0.5) << "%)"
			<< " | wait avg=" << static_cast<int>(waitAvgUsec + 0.5) << "us"
			<< " max=" << static_cast<int>(s.waitUsecMax) << "us"
			<< " =" << static_cast<int>(waitPercent + 0.5) << "% of callback time"
			<< " | EXPECTED avg=" << static_cast<int>(expAvgUsec + 0.5) << "us"
			<< " EXCESS avg=" << static_cast<int>(excessAvgUsec) << "us"
			<< " max=" << static_cast<int>(s.waitExcessUsecMax) << "us"
			<< " | ring=" << s.ringDepthMin << ".." << s.ringDepthMax
			<< " avg=" << (s.callbacks ? static_cast<uint32_t>(s.ringDepthSum / s.callbacks) : 0)
			<< " (latency=" << s.latencyFrames << ")"
			<< " | dspA=" << m_dspA.getDSPThread().getCurrentMips()
			<< " dspB=" << m_dspB.getDSPThread().getCurrentMips() << " MIPS";

		LOG(line.str());

		/* An AUv3 runs as its OWN extension process on iOS, so its stdout never
		 * reaches a --console launch of the host. Append to the container as well;
		 * that file can be pulled afterwards with
		 *   xcrun devicectl device copy from --domain-type appDataContainer \
		 *     --domain-identifier <appex bundle id> --source Documents/<name>
		 * which is the only way to see the numbers that actually matter -- the
		 * standalone's are the easy case and were never the question. */
		if(const auto* home = std::getenv("HOME"))
		{
			const std::string path = std::string(home) + "/Documents/n2x_audio_stats.log";
			if(auto* f = fopen(path.c_str(), "a"))
			{
				// the file is appended to across runs; mark where each one starts so
				// a 64-frame session is never read as part of a 256-frame one
				static bool s_sessionBannerWritten = false;
				if(!s_sessionBannerWritten)
				{
					s_sessionBannerWritten = true;
					fprintf(f, "=== session start, pid %d ===\n", static_cast<int>(getpid()));
				}
				fprintf(f, "%s\n", line.str().c_str());
				fclose(f);
			}
		}

		s = AudioWaitStats{};
	}
#endif
	
	void Hardware::processAudio(const synthLib::TAudioOutputs& _outputs, const uint32_t _frames, const uint32_t _latency)
	{
		processAudio(_frames, _latency);

		for(size_t i=0; i<_frames; ++i)
		{
			_outputs[0][i] = dsp56k::dsp2sample<float>(m_audioOutputs[0][i]);
			_outputs[1][i] = dsp56k::dsp2sample<float>(m_audioOutputs[1][i]);
			_outputs[2][i] = dsp56k::dsp2sample<float>(m_audioOutputs[2][i]);
			_outputs[3][i] = dsp56k::dsp2sample<float>(m_audioOutputs[3][i]);
		}
	}

	bool Hardware::sendMidi(const synthLib::SMidiEvent& _ev)
	{
		m_midiIn.push_back(_ev);
		return true;
	}

	void Hardware::notifyBootFinished()
	{
		m_bootFinished = true;
	}

	void Hardware::ensureBufferSize(const uint32_t _frames)
	{
		if(m_dummyInput.size() >= _frames)
			return;

		m_dummyInput.resize(_frames, 0);
		m_dummyOutput.resize(_frames, 0);

		for (auto& audioOutput : m_audioOutputs)
			audioOutput.resize(_frames, 0);

		m_dspAtoBBuffer.resize(_frames * 4);
	}

	void Hardware::onEsaiCallbackA()
	{
		// forward DSP A output to DSP B input
		const auto out = m_dspA.getPeriph().getEsai().getAudioOutputs().pop_front();

		dsp56k::Audio::RxFrame in;
		in.resize(out.size());

		in[0] = dsp56k::Audio::RxSlot{out[0][0]};
		in[1] = dsp56k::Audio::RxSlot{out[1][0]};
		in[2] = dsp56k::Audio::RxSlot{out[2][0]};
		in[3] = dsp56k::Audio::RxSlot{out[3][0]};

		m_dspB.getPeriph().getEsai().getAudioInputs().push_back(in);

		m_semDspAtoB.wait();
	}

	void Hardware::processMidiInput()
	{
		++m_midiOffsetCounter;

		while(!m_midiIn.empty())
		{
			const auto& e = m_midiIn.front();

			if(e.offset > m_midiOffsetCounter)
				break;

			getMidi().write(e);
			m_midiIn.pop_front();
		}
	}

	void Hardware::onEsaiCallbackB()
	{
		m_semDspAtoB.notify();

		++m_esaiFrameIndex;

		processMidiInput();

		if((m_esaiFrameIndex & (g_syncEsaiFrameRate-1)) == 0)
			m_esaiFrameAddedCv.notify_one();

		m_requestedFramesAvailableMutex.lock();

		if(m_requestedFrames && m_dspB.getPeriph().getEsai().getAudioOutputs().size() >= m_requestedFrames)
		{
			m_requestedFramesAvailableMutex.unlock();
			m_requestedFramesAvailableCv.notify_one();
		}
		else
		{
			m_requestedFramesAvailableMutex.unlock();
		}

		m_haltDSPSem.wait(1);
	}

	void Hardware::syncUCtoDSP()
	{
		assert(m_remainingUcCycles <= 0);

		// we can only use ESAI to clock the uc once it has been enabled
		if(m_esaiFrameIndex <= 0)
			return;

		if(m_esaiFrameIndex == m_lastEsaiFrameIndex)
		{
			resumeDSPs();
			std::unique_lock uLock(m_esaiFrameAddedMutex);
			m_esaiFrameAddedCv.wait(uLock, [this]{return m_esaiFrameIndex > m_lastEsaiFrameIndex;});
		}

		const auto esaiFrameIndex = m_esaiFrameIndex;
		const auto esaiDelta = esaiFrameIndex - m_lastEsaiFrameIndex;

		const auto ucClock = m_uc.getSim().getSystemClockHz();
		const double ucCyclesPerFrame = static_cast<double>(ucClock) * m_samplerateInv;

		// if the UC consumed more cycles than it was allowed to, remove them from remaining cycles
		m_remainingUcCyclesD += static_cast<double>(m_remainingUcCycles);

		// add cycles for the ESAI time that has passed
		m_remainingUcCyclesD += ucCyclesPerFrame * static_cast<double>(esaiDelta);

		// set new remaining cycle count
		m_remainingUcCycles = static_cast<int64_t>(m_remainingUcCyclesD);

		// and consume them
		m_remainingUcCyclesD -= static_cast<double>(m_remainingUcCycles);

		if(esaiDelta > g_syncHaltDspEsaiThreshold)
			haltDSPs();

		m_lastEsaiFrameIndex = esaiFrameIndex;
	}

	void Hardware::ucThreadFunc()
	{
		dsp56k::ThreadTools::setCurrentThreadName("MC68331");
		dsp56k::ThreadTools::setCurrentThreadPriority(dsp56k::ThreadPriority::Highest);

		while(!m_destroy)
		{
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
			processUC();
		}
		resumeDSPs();
		m_destroy = false;
	}

	void Hardware::advanceSamples(const uint32_t _samples, const uint32_t _latency)
	{
		// if the latency was higher first but now is lower, we might report < 0 samples. In this case we
		// cannot notify but have to wait for another sample block until we can notify again

		const auto latencyDiff = static_cast<int>(_latency) - static_cast<int>(m_esaiLatency);
		m_esaiLatency = _latency;

		const auto notifyCount = static_cast<int>(_samples) + latencyDiff + m_dspNotifyCorrection;

		if (notifyCount > 0)
		{
			m_haltDSPSem.notify(notifyCount);
			m_dspNotifyCorrection = 0;
		}
		else
		{
			m_dspNotifyCorrection = notifyCount;
		}
	}

	void Hardware::haltDSPs()
	{
		if(m_dspHalted)
			return;
		m_dspHalted = true;
//		LOG("Halt");
		m_dspA.getHaltDSP().haltDSP();
		m_dspB.getHaltDSP().haltDSP();
	}

	void Hardware::resumeDSPs()
	{
		if(!m_dspHalted)
			return;
		m_dspHalted = false;
//		LOG("Resume");
		m_dspA.getHaltDSP().resumeDSP();
		m_dspB.getHaltDSP().resumeDSP();
	}

	bool Hardware::getButtonState(const ButtonType _type) const
	{
		return m_uc.getFrontPanel().getButtonState(_type);
	}

	void Hardware::setButtonState(const ButtonType _type, const bool _pressed)
	{
		m_uc.getFrontPanel().setButtonState(_type, _pressed);
	}

	uint8_t Hardware::getKnobPosition(KnobType _knob) const
	{
		return m_uc.getFrontPanel().getKnobPosition(_knob);
	}

	void Hardware::setKnobPosition(KnobType _knob, uint8_t _value)
	{
		return m_uc.getFrontPanel().setKnobPosition(_knob, _value);
	}
}
