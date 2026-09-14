#pragma once

#include "n2xdsp.h"
#include "n2xmc.h"
#include "n2xrom.h"

#include "synthLib/audioTypes.h"
#include "synthLib/midiTypes.h"

namespace n2x
{
	class Hardware
	{
	public:
		using AudioOutputs = std::array<std::vector<dsp56k::TWord>, 4>;
		Hardware(const std::vector<uint8_t>& _romData = {}, const std::string& _romName = {});
		~Hardware();

		bool isValid() const;

		void processUC();

		Microcontroller& getUC() {return m_uc; }

		const auto& getAudioOutputs() const { return m_audioOutputs; }

		void processAudio(uint32_t _frames, uint32_t _latency);

		auto& getDSPA() { return m_dspA; }
		auto& getDSPB() { return m_dspB; }

		auto& getMidi() { return m_uc.getMidi(); }

		void haltDSPs();
		void resumeDSPs();
		bool requestingHaltDSPs() const { return m_dspHalted; }

		bool getButtonState(ButtonType _type) const;
		void setButtonState(ButtonType _type, bool _pressed);

		uint8_t getKnobPosition(KnobType _knob) const;
		void setKnobPosition(KnobType _knob, uint8_t _value);

		void processAudio(const synthLib::TAudioOutputs& _outputs, uint32_t _frames, uint32_t _latency);
		bool sendMidi(const synthLib::SMidiEvent& _ev);
		void notifyBootFinished();

		const std::string& getRomFilename() const { return m_rom.getFilename(); }

	private:
		void ensureBufferSize(uint32_t _frames);
		void onEsaiCallbackA();
		void processMidiInput();
		void onEsaiCallbackB();
		void syncUCtoDSP();
		void ucThreadFunc();
		void advanceSamples(uint32_t _samples, uint32_t _latency);

		Rom m_rom;
		Microcontroller m_uc;
		DSP m_dspA;
		DSP m_dspB;

		std::vector<dsp56k::TWord> m_dummyInput;
		std::vector<dsp56k::TWord> m_dummyOutput;
		std::vector<dsp56k::TWord> m_dspAtoBBuffer;

		AudioOutputs m_audioOutputs;

		// timing
		const double m_samplerateInv;
		uint32_t m_esaiFrameIndex = 0;
		uint32_t m_lastEsaiFrameIndex = 0;
		int64_t m_remainingUcCycles = 0;
		double m_remainingUcCyclesD = 0;
		std::mutex m_esaiFrameAddedMutex;
		dsp56k::ConditionVariable m_esaiFrameAddedCv;
		std::mutex m_requestedFramesAvailableMutex;
		dsp56k::ConditionVariable m_requestedFramesAvailableCv;
		size_t m_requestedFrames = 0;

/* Audio-path instrumentation. OFF by default: it does iostream and a fopen/
 * fprintf on the AUDIO THREAD once every 2s, which is fine for measuring and not
 * for shipping. Build with -DN2X_AUDIO_TRACE=1 to turn it back on.
 *
 * This is what found the realtime-window bug (see threadtools.cpp): the ESAI
 * output ring is pinned at its full cushion when healthy and drains to almost
 * nothing when the DSP workers lose CPU, which is visible here long before it is
 * audible. Worth keeping. */
#ifndef N2X_AUDIO_TRACE
#define N2X_AUDIO_TRACE 0
#endif

		/* Instrumentation for the producer/consumer handshake in processAudio().
		 * The audio callback consumes in 64-frame chunks and BLOCKS on the CV per
		 * chunk until the DSP threads have produced, so at 98.2 kHz internal a
		 * 512-frame host buffer means eight waits per callback. If the DSP workers
		 * are being scheduled late, it shows up here as wall time the host's render
		 * thread spent blocked -- which is the difference between "the emulation is
		 * too slow" and "the emulation is fast but not scheduled in time".
		 *
		 * The clock is only read on the path that actually blocks, so the common
		 * case costs a branch. Totals are dumped every kStatsIntervalFrames of
		 * output and then reset. */
#if N2X_AUDIO_TRACE
		struct AudioWaitStats
		{
			uint64_t callbacks = 0;
			uint64_t chunks = 0;
			uint64_t chunksBlocked = 0;
			uint64_t framesSinceReport = 0;
			double waitUsecTotal = 0.0;
			double waitUsecMax = 0.0;
			/* The DSPs produce at exactly real time, so a consumer asking for
			 * frames that do not exist yet MUST wait roughly the time it takes to
			 * make them: shortfall / g_samplerate. That inherent cost and a late
			 * worker thread both look like "callback blocked", so measure the
			 * EXPECTED wait beside the actual one -- the excess is the part that
			 * scheduling has to answer for. */
			/* Depth of the ESAI output ring at callback ENTRY. This is the cushion
			 * that actually exists, as opposed to the one the extra-latency setting
			 * asks for. At a 256-frame host buffer the configured cushion is ~5.3 ms
			 * (~520 device frames); if this sits near zero instead, the latency never
			 * materialises as standing audio and the consumer rides the production
			 * edge -- which is what the 80%-of-callback wait figures imply. */
			uint64_t ringDepthSum = 0;
			uint32_t ringDepthMin = 0xffffffff;
			uint32_t ringDepthMax = 0;
			uint32_t latencyFrames = 0;

			double waitExpectedUsecTotal = 0.0;
			double waitExcessUsecTotal = 0.0;
			double waitExcessUsecMax = 0.0;
			double busyUsecTotal = 0.0;	// wall time in processAudio, waiting included
			uint32_t hostBlockMin = 0xffffffff;
			uint32_t hostBlockMax = 0;
		};

		AudioWaitStats m_waitStats;

		void reportAudioWaitStats(uint32_t _hostBlockFrames, double _busyUsec);
#endif
		bool m_dspHalted = false;
		dsp56k::SpscSemaphore m_semDspAtoB;

		dsp56k::RingBuffer<dsp56k::Audio::RxFrame, 4, true> m_dspAtoBbuf;

		std::unique_ptr<std::thread> m_ucThread;
		bool m_destroy = false;

		// Midi
		dsp56k::RingBuffer<synthLib::SMidiEvent, 16384, true> m_midiIn;
		uint32_t m_midiOffsetCounter = 0;

		// DSP slowdown
		uint32_t m_esaiLatency = 0;
		int32_t m_dspNotifyCorrection = 0;
		dsp56k::SpscSemaphoreWithCount m_haltDSPSem;

		bool m_bootFinished = false;
	};
}
