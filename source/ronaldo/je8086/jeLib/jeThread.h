#pragma once

#include <atomic>
#include <chrono>
#include <deque>

#include "baseLib/semaphore.h"

#include "dsp56kBase/ringbuffer.h"

#include "synthLib/midiTypes.h"

namespace jeLib
{
	class Je8086;

	class JeThread
	{
	public:
		using SampleFrame = std::pair<int32_t, int32_t>; // left, right

		JeThread(Je8086& _je8086);
		~JeThread();

		void processSamples(uint32_t _count, uint32_t _requiredLatency, std::vector<synthLib::SMidiEvent>& _midiIn, std::vector<synthLib::SMidiEvent>& _midiOut);

		auto& getSampleBuffer() { return m_audioOut; }

		/* Diagnostics: "the runway was spent and never rebuilt" and "the engine is
		 * behind and the jobs are piling up" both look like an empty ring from the
		 * audio thread, and they need opposite fixes. These tell them apart. */
		uint32_t getCurrentLatency() const { return m_currentLatency; }
		size_t getPendingJobs() const { return m_pendingJobs.size(); }
		uint32_t getCarry() const { return m_hasCarry ? m_carry.samplesToProcess : 0; }

		/* ns of engine time per rendered sample, since the last call. 11337 ns is
		 * exactly real time at 88.2 kHz, so below that is headroom and above it
		 * is a deficit -- whatever the host happens to be asking for. */
		double takeNsPerSample()
		{
			const auto ns = m_renderNs.exchange(0, std::memory_order_relaxed);
			const auto n  = m_renderSamples.exchange(0, std::memory_order_relaxed);
			return n ? static_cast<double>(ns) / static_cast<double>(n) : 0.0;
		}

	private:
		using MidiEvent = std::pair<uint64_t, synthLib::SMidiEvent>;
		struct ProcessJob
		{
			uint32_t samplesToProcess = 0;
			std::vector<MidiEvent> midiEvents;
		};

		void threadFunc();
		void processJob(ProcessJob& _job);

		Je8086& m_je8086;

		std::unique_ptr<std::thread> m_thread;

		bool m_exit = false;

		uint32_t m_currentLatency = 0;

		dsp56k::RingBuffer<SampleFrame, 16384, true> m_audioOut;

		std::vector<synthLib::SMidiEvent> m_midiOutput;

		std::mutex m_mutex;

		uint64_t m_inSampleOffset = 0;

		/* Work that did not fit in m_pendingJobs, accumulated rather than blocking
		 * the audio thread. See processSamples(). */
		std::atomic<uint64_t> m_renderNs{0}, m_renderSamples{0};

		ProcessJob m_carry;
		bool m_hasCarry = false;

		std::vector<ProcessJob> m_jobPool;
		dsp56k::RingBuffer<ProcessJob, 32, true> m_pendingJobs;

		uint64_t m_processedSampleOffset = 0;
		std::vector<synthLib::SMidiEvent> m_tempMidiOut;
		std::vector<MidiEvent> m_tempMidiIn;
	};
}
