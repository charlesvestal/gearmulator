#include "jePipeline.h"

#include "je8086.h"
#include "je8086devices.h"

#include "baseLib/os.h"

#include <atomic>
#include <cstring>

#ifdef __linux__
#include <sched.h>
#endif

namespace jeLib
{
	namespace
	{
		inline void pinCore(const int _core)
		{
#ifdef __linux__
			if (_core < 0) return;
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(_core, &set);
			sched_setaffinity(0, sizeof(set), &set);
#else
			(void)_core;
#endif
		}

		/* Spin, then yield. The stages are sample-locked and a boundary is only
		 * ever a few samples apart, so sleeping here costs more than it saves --
		 * but a bare spin burns a core, so hint the CPU on every turn. */
		template<typename Pred, typename Stop>
		inline bool spinWait(Pred _pred, Stop _stop)
		{
			while (!_pred())
			{
				if (_stop()) return false;
				std::this_thread::yield();
			}
			return true;
		}
	}

	struct JePipeline::Impl
	{
		struct Handoff { int32_t gram[HandoffCount]; };
		struct Audio { int32_t left, right; };
		struct UcWrite { uint8_t asic, val; uint16_t addr; uint32_t sample; };

		struct Stage
		{
			int lo = 0, hi = 0;
			std::atomic<bool> ready{false};

			Handoff gramRing[RingCapacity];
			std::atomic<int> gramWrite{0}, gramRead{0};

			UcWrite ucRing[UcRingCap];
			std::atomic<int> ucWrite{0}, ucRead{0};

			std::atomic<int64_t> samplesProduced{0};
		};

		Stage stage[MaxStages];
		std::thread threads[MaxStages];
		std::atomic<bool> shutdown{false};

		Audio audioRing[RingCapacity];
		std::atomic<int> audioWrite{0}, audioRead{0};

		uint8_t readback[4][4] = {};

		std::atomic<int64_t> drained{0};
		int64_t delivered = 0;	// caller's thread only

		static int avail(const std::atomic<int>& _w, const std::atomic<int>& _r)
		{
			int d = _w.load(std::memory_order_acquire) - _r.load(std::memory_order_relaxed);
			if (d < 0) d += RingCapacity * 2;
			return d;
		}
	};

	JePipeline::JePipeline(Je8086& _je, const std::vector<int>& _bounds, const std::vector<int>& _cores)
		: m_impl(new Impl()), m_je(_je)
	{
		if (_bounds.empty() || _bounds.size() >= MaxStages)
			return;
		for (size_t i = 0; i < _bounds.size(); ++i)
		{
			if (_bounds[i] < 1 || _bounds[i] > 3) return;
			if (i && _bounds[i] <= _bounds[i - 1]) return;
		}

		m_numStages = static_cast<int>(_bounds.size()) + 1;

		auto& impl = *m_impl;
		impl.stage[0].lo = 0;
		impl.stage[0].hi = _bounds[0];
		for (int s = 1; s < m_numStages; ++s)
		{
			impl.stage[s].lo = _bounds[s - 1];
			impl.stage[s].hi = (s < static_cast<int>(_bounds.size())) ? _bounds[s] : 4;
		}

		auto core = [&](const int s) { return s < static_cast<int>(_cores.size()) ? _cores[s] : -1; };

		installParentHooks(_bounds[0]);
		pinCore(core(0));

		for (int s = 1; s < m_numStages; ++s)
			impl.threads[s] = std::thread([this, s, c = core(s)] { stageMain(s, c); });

		/* Stages only need their thread_local state in place before the first
		 * handoff arrives, but waiting here keeps a failure at construction. */
		for (int s = 1; s < m_numStages; ++s)
			while (!impl.stage[s].ready.load(std::memory_order_acquire))
				std::this_thread::yield();

		m_valid = true;
	}

	JePipeline::~JePipeline()
	{
		auto& impl = *m_impl;
		impl.shutdown.store(true, std::memory_order_release);
		for (int s = 1; s < m_numStages; ++s)
			if (impl.threads[s].joinable())
				impl.threads[s].join();

		// leave the emulator in serial mode
		devices::g_je_parallel_mode = 0;
		devices::g_je_stage_lo = 0;
		devices::g_je_stage_hi = 4;
		devices::g_je_gram_produce = nullptr;
		devices::g_je_gram_consume = nullptr;
		devices::g_je_uc_write_forward = nullptr;
		devices::g_je_parent_dummy_audio = true;
	}

	/* Hooks for the caller's thread: it owns ASICs [0, firstBound), publishes
	 * that boundary's handoff to stage 1, and forwards H8S register writes aimed
	 * at ASICs it does not own. */
	void JePipeline::installParentHooks(const int _firstBound)
	{
		auto& impl = *m_impl;

		devices::g_je_split_asic = _firstBound;
		devices::g_je_parallel_mode = 1;
		devices::g_je_stage_lo = 0;
		devices::g_je_stage_hi = _firstBound;
		devices::g_je_parent_dummy_audio = false;	// we drain the stages' real audio ourselves

		auto* next = &impl.stage[1];
		devices::g_je_gram_produce = [this, next](const int32_t* _gram)
		{
			auto& impl2 = *m_impl;
			if (!spinWait([&] { return Impl::avail(next->gramWrite, next->gramRead) < RingCapacity - 1; },
			              [&] { return impl2.shutdown.load(std::memory_order_relaxed); }))
				return;
			const int wi = next->gramWrite.load(std::memory_order_relaxed) & RingMask;
			std::memcpy(next->gramRing[wi].gram, _gram, sizeof(int32_t) * HandoffCount);
			next->gramWrite.store((next->gramWrite.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
			                      std::memory_order_release);
			impl2.stage[0].samplesProduced.fetch_add(1, std::memory_order_release);
		};

		/* The H8S programs ASICs the stages own; the write must land on the same
		 * sample there as here, so it carries the parent's sample index. */
		devices::g_je_uc_write_forward = [this](const int _asic, const uint32_t _addr, const uint8_t _val)
		{
			auto& impl2 = *m_impl;
			for (int s = 1; s < m_numStages; ++s)
			{
				auto& st = impl2.stage[s];
				if (_asic < st.lo || _asic >= st.hi)
					continue;
				const int wi = st.ucWrite.load(std::memory_order_relaxed) % UcRingCap;
				st.ucRing[wi] = { static_cast<uint8_t>(_asic), _val, static_cast<uint16_t>(_addr),
				                  static_cast<uint32_t>(impl2.stage[0].samplesProduced.load(std::memory_order_relaxed)) };
				st.ucWrite.store((st.ucWrite.load(std::memory_order_relaxed) + 1) % UcRingCap,
				                 std::memory_order_release);
				break;
			}
		};
	}

	void JePipeline::stageMain(const int _stage, const int _core)
	{
		auto& impl = *m_impl;
		auto& st = impl.stage[_stage];
		auto& prev = impl.stage[_stage - 1];
		const bool isLast = (_stage + 1) >= m_numStages;
		const int lo = st.lo, hi = st.hi;

		pinCore(_core);
		baseLib::setFlushDenormalsToZero();	// FPCR is per thread

		devices::g_je_parallel_mode = 2;
		devices::g_je_stage_lo = lo;
		devices::g_je_stage_hi = hi;

		devices::g_je_gram_consume = [this, &st](int32_t* _gram) -> bool
		{
			auto& impl2 = *m_impl;
			if (!spinWait([&] { return Impl::avail(st.gramWrite, st.gramRead) >= 1; },
			              [&] { return impl2.shutdown.load(std::memory_order_relaxed); }))
				return false;
			const int ri = st.gramRead.load(std::memory_order_relaxed) & RingMask;
			std::memcpy(_gram, st.gramRing[ri].gram, sizeof(int32_t) * HandoffCount);
			st.gramRead.store((st.gramRead.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
			                  std::memory_order_release);
			return true;
		};

		if (!isLast)
		{
			auto* next = &impl.stage[_stage + 1];
			devices::g_je_gram_produce = [this, next, &st](const int32_t* _gram)
			{
				auto& impl2 = *m_impl;
				if (!spinWait([&] { return Impl::avail(next->gramWrite, next->gramRead) < RingCapacity - 1; },
				              [&] { return impl2.shutdown.load(std::memory_order_relaxed); }))
					return;
				const int wi = next->gramWrite.load(std::memory_order_relaxed) & RingMask;
				std::memcpy(next->gramRing[wi].gram, _gram, sizeof(int32_t) * HandoffCount);
				next->gramWrite.store((next->gramWrite.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
				                      std::memory_order_release);
				st.samplesProduced.fetch_add(1, std::memory_order_release);
			};
		}
		else
		{
			/* Stage-scoped, not MultiAsic::setPostSample: the object is shared, so
			 * installing it there would also catch the caller's dummy postSample. */
			devices::g_je_stage_audio_out = [this, &st](const int32_t _l, const int32_t _r)
			{
				auto& impl2 = *m_impl;
				if (!spinWait([&] { return Impl::avail(impl2.audioWrite, impl2.audioRead) < RingCapacity - 1; },
				              [&] { return impl2.shutdown.load(std::memory_order_relaxed); }))
					return;
				const int wi = impl2.audioWrite.load(std::memory_order_relaxed) & RingMask;
				impl2.audioRing[wi] = { _l, _r };
				impl2.audioWrite.store((impl2.audioWrite.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
				                       std::memory_order_release);
				st.samplesProduced.fetch_add(1, std::memory_order_release);
			};
		}

		st.ready.store(true, std::memory_order_release);

		auto& asics = m_je.getAsics();
		uint32_t sample = 0;

		while (!impl.shutdown.load(std::memory_order_relaxed))
		{
			/* Wait for our input handoff AND for the previous stage to have
			 * published this sample, so forwarded register writes stamped with it
			 * have all arrived. */
			if (!spinWait([&] { return Impl::avail(st.gramWrite, st.gramRead) >= 1 &&
			                           prev.samplesProduced.load(std::memory_order_acquire) > static_cast<int64_t>(sample); },
			              [&] { return impl.shutdown.load(std::memory_order_relaxed); }))
				break;

			while (st.ucRead.load(std::memory_order_relaxed) != st.ucWrite.load(std::memory_order_acquire))
			{
				const int ri = st.ucRead.load(std::memory_order_relaxed) % UcRingCap;
				const auto w = st.ucRing[ri];
				if (w.sample > sample)
					break;
				st.ucRead.store((ri + 1) % UcRingCap, std::memory_order_release);
				asics.applyUcWrite(w.asic, w.addr, w.val);
			}

			if (!asics.processSampleChild())
				break;
			++sample;

			for (int a = lo; a < hi; ++a)
				asics.getReadback(a, impl.readback[a]);

		}
	}

	void JePipeline::drainAudio(const std::function<void(int32_t, int32_t)>& _sink, const bool _waitForOne)
	{
		auto& impl = *m_impl;
		if (_waitForOne)
		{
			if (!spinWait([&] { return Impl::avail(impl.audioWrite, impl.audioRead) > 0; },
			              [&] { return impl.shutdown.load(std::memory_order_relaxed); }))
				return;
		}
		while (Impl::avail(impl.audioWrite, impl.audioRead) > 0)
		{
			const int ri = impl.audioRead.load(std::memory_order_relaxed) & RingMask;
			_sink(impl.audioRing[ri].left, impl.audioRing[ri].right);
			impl.drained.fetch_add(1, std::memory_order_relaxed);
			impl.audioRead.store((impl.audioRead.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
			                     std::memory_order_release);
		}
	}

	int64_t JePipeline::inFlight() const
	{
		auto& impl = *m_impl;
		return impl.stage[0].samplesProduced.load(std::memory_order_acquire) -
		       impl.drained.load(std::memory_order_relaxed);
	}

	void JePipeline::pump(const std::function<void(int32_t, int32_t)>& _sink, const int64_t _window)
	{
		auto& impl = *m_impl;
		drainAudio(_sink);
		while (inFlight() >= _window)
		{
			if (!spinWait([&] { return Impl::avail(impl.audioWrite, impl.audioRead) > 0; },
			              [&] { return impl.shutdown.load(std::memory_order_relaxed); }))
				return;
			drainAudio(_sink);
		}
	}

	void JePipeline::deliver(const std::function<void(int32_t, int32_t)>& _sink, const int64_t _latency)
	{
		auto& impl = *m_impl;
		const int64_t produced = impl.stage[0].samplesProduced.load(std::memory_order_acquire);

		while (impl.delivered < produced)
		{
			if (impl.delivered < _latency)
			{
				_sink(0, 0);	// pipeline still filling
			}
			else
			{
				if (!spinWait([&] { return Impl::avail(impl.audioWrite, impl.audioRead) > 0; },
				              [&] { return impl.shutdown.load(std::memory_order_relaxed); }))
					return;
				const int ri = impl.audioRead.load(std::memory_order_relaxed) & RingMask;
				_sink(impl.audioRing[ri].left, impl.audioRing[ri].right);
				impl.audioRead.store((impl.audioRead.load(std::memory_order_relaxed) + 1) % (RingCapacity * 2),
				                     std::memory_order_release);
				impl.drained.fetch_add(1, std::memory_order_relaxed);
			}
			++impl.delivered;
		}
	}

	void JePipeline::refreshParentReadbacks()
	{
		auto& impl = *m_impl;
		auto& asics = m_je.getAsics();
		for (int a = devices::g_je_split_asic; a < 4; ++a)
			asics.setReadback(a, impl.readback[a]);
	}
}
