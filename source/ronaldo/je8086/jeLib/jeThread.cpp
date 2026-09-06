#include "jeThread.h"

#ifdef __APPLE__
#include <pthread.h>
#endif

#include "je8086.h"

#include "dsp56kBase/threadtools.h"

namespace jeLib
{
	JeThread::JeThread(Je8086& _je8086) : m_je8086(_je8086)
	{
		m_thread.reset(new std::thread([this]() { threadFunc(); }));
	}

	JeThread::~JeThread()
	{
		m_exit = true;
		m_pendingJobs.push_back(ProcessJob());
		m_thread->join();
		m_thread.reset();
	}

	void JeThread::processSamples(const uint32_t _count, uint32_t _requiredLatency, std::vector<synthLib::SMidiEvent>& _midiIn, std::vector<synthLib::SMidiEvent>& _midiOut)
	{
		ProcessJob job;
		{
			std::lock_guard lock(m_mutex);
			if (!m_jobPool.empty())
			{
				job = std::move(m_jobPool.back());
				m_jobPool.pop_back();
			}
		}

		for (auto& e : _midiIn)
		{
			const auto offset = e.offset + _requiredLatency + m_inSampleOffset;
			job.midiEvents.emplace_back(offset, std::move(e));
		}

		_midiIn.clear();

		job.samplesToProcess = 0;

		m_inSampleOffset += _count;

		// add latency by allowing the DSP to process more samples
		while (_requiredLatency > m_currentLatency)
		{
			++job.samplesToProcess;
			++m_currentLatency;
		}

		for (size_t i=0; i<_count; ++i)
		{
			// remove latency by omitting new processing requests
			if (m_currentLatency > _requiredLatency)
				--m_currentLatency;
			else
				++job.samplesToProcess;
		}

		if (m_currentLatency == 0 && m_pendingJobs.empty())
		{
			processJob(job);
			m_jobPool.push_back(std::move(job));
		}
		else
		{
			/* NEVER block the host's audio thread. m_pendingJobs is a 32-deep
			 * BLOCKING ring: once the engine falls behind enough to fill it,
			 * push_back() stalls the caller inside process(), the host's callback
			 * rate collapses, and the backlog can then never drain -- which is
			 * self-sustaining, so the plugin plays cleanly and then breaks up for
			 * good. Measured on an iPad in AUM as `pending 32` pinned forever with
			 * the host down from 88200 to 26000 samples/s.
			 *
			 * Fold the work into a carry instead and hand it over when there is
			 * room. The engine renders the same samples and the same MIDI, just in
			 * larger chunks, and the audio thread always returns. */
			if (!m_hasCarry)
			{
				m_carry = std::move(job);
				m_hasCarry = true;
			}
			else
			{
				m_carry.samplesToProcess += job.samplesToProcess;
				m_carry.midiEvents.insert(m_carry.midiEvents.end(),
					std::make_move_iterator(job.midiEvents.begin()),
					std::make_move_iterator(job.midiEvents.end()));
			}

			if (!m_pendingJobs.full())
			{
				m_pendingJobs.push_back(std::move(m_carry));
				m_carry.samplesToProcess = 0;
				m_carry.midiEvents.clear();
				m_hasCarry = false;
			}
		}

		{
			std::lock_guard lock(m_mutex);

			if (_midiOut.empty())
			{
				std::swap(_midiOut, m_midiOutput);
			}
			else
			{
				_midiOut.insert(_midiOut.end(), m_midiOutput.begin(), m_midiOutput.end());
				m_midiOutput.clear();
			}
		}
	}

	void JeThread::threadFunc()
	{
		dsp56k::ThreadTools::setCurrentThreadName("JE8086");
		dsp56k::ThreadTools::setCurrentThreadPriority(dsp56k::ThreadPriority::Highest);
#ifdef __APPLE__
		/* On Apple silicon the QoS class, not the priority, is what keeps a
		 * thread off the efficiency cores. The engine thread is what the host's
		 * audio thread waits on, so it belongs with the interactive work. */
		pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

		while (!m_exit)
		{
			auto job = m_pendingJobs.pop_front();

			if (m_exit)
				break;

			processJob(job);

			std::lock_guard lock(m_mutex);
			m_jobPool.push_back(std::move(job));
		}
	}

	void JeThread::processJob(ProcessJob& _job)
	{
		/* Capacity probe. "engine made" in the diagnostics is demand-limited by
		 * construction -- it can only ever equal what the host asked for -- so it
		 * cannot answer "how fast could this engine go". Timing the render loop
		 * can: ns per sample, measured where the work actually happens, and
		 * directly comparable between the standalone and the AUv3. */
		const auto t0 = std::chrono::steady_clock::now();
		const auto samplesThisJob = _job.samplesToProcess;
		struct Timer {
			JeThread& t; const std::chrono::steady_clock::time_point& t0; const uint32_t n;
			~Timer() {
				if (!n) return;
				const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - t0).count();
				t.m_renderNs.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
				t.m_renderSamples.fetch_add(n, std::memory_order_relaxed);
			}
		} timer{*this, t0, samplesThisJob};

		if (m_tempMidiIn.empty())
		{
			std::swap(m_tempMidiIn, _job.midiEvents);
		}
		else
		{
			m_tempMidiIn.insert(m_tempMidiIn.end(), _job.midiEvents.begin(), _job.midiEvents.end());
			_job.midiEvents.clear();
		}

		for (uint32_t i=0; i<_job.samplesToProcess; ++i)
		{
			for(auto it = m_tempMidiIn.begin(); it != m_tempMidiIn.end();)
			{
				auto& e = *it;

				if (e.first <= m_processedSampleOffset)
				{
					m_je8086.addMidiEvent(e.second);
					it = m_tempMidiIn.erase(it);
				}
				else
				{
					++it;
				}
			}

			while (m_je8086.getSampleBuffer().empty())
				m_je8086.step();

			m_audioOut.push_back(m_je8086.getSampleBuffer().front());
			m_je8086.clearSampleBuffer();

			++m_processedSampleOffset;

			m_je8086.readMidiOut(m_tempMidiOut);

			if (!m_tempMidiOut.empty())
			{
				std::lock_guard lock(m_mutex);
				if (m_midiOutput.empty())
				{
					std::swap(m_midiOutput, m_tempMidiOut);
				}
				else
				{
					m_midiOutput.insert(m_midiOutput.end(), m_tempMidiOut.begin(), m_tempMidiOut.end());
					m_tempMidiOut.clear();
				}
			}
		}

		_job.samplesToProcess = 0;
	}
}
