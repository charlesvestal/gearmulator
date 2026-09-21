#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "audioTypes.h"
#include "deviceTypes.h"

#include "midiTypes.h"
#include "buildconfig.h"
#include "midiTranslator.h"

#include "baseLib/md5.h"

namespace synthLib
{
	struct DeviceCreateParams
	{
		float preferredSamplerate = 0.0f;
		float hostSamplerate = 0.0f;
		std::string romName;
		std::vector<uint8_t> romData;
		baseLib::MD5 romHash;
		uint32_t customData = 0;
		std::string homePath;
		// 0 or 1 = render on one thread. Higher asks a device that supports it to
		// spread its DSP over that many threads; see Device::getMaxDspThreads().
		uint32_t dspThreads = 0;
	};

	class Device
	{
	public:
		Device(const DeviceCreateParams& _params);
		Device(const Device&) = delete;
		Device(Device&&) = delete;

		virtual ~Device();

		Device& operator = (const Device&) = delete;
		Device& operator = (Device&&) = delete;

		virtual void process(const TAudioInputs& _inputs, const TAudioOutputs& _outputs, size_t _size, const std::vector<SMidiEvent>& _midiIn, std::vector<SMidiEvent>& _midiOut);

		void setExtraLatencySamples(uint32_t _size);

		/* Upper bound on the runway, in samples. The runway is otherwise
		 * latencyBlocks x the host's MAXIMUM expected block, which on iOS is far
		 * larger than the blocks actually delivered -- an AUv3 running 64-frame
		 * callbacks was asking for 16384 samples (186 ms) at the smallest block
		 * count available. A device that knows how much headroom it has can say
		 * so directly. 0 = no additional limit. */
		void setMaxExtraLatencySamples(const uint32_t _max) { m_maxExtraLatency = _max; setExtraLatencySamples(m_requestedExtraLatency); }
		uint32_t getExtraLatencySamples() const { return m_extraLatency; }

		virtual uint32_t getInternalLatencyMidiToOutput() const { return 0; }
		virtual uint32_t getInternalLatencyInputToOutput() const { return 0; }

		virtual void getSupportedSamplerates(std::vector<float>& _dst) const
		{
			_dst.push_back(getSamplerate());
		}
		virtual float getSamplerate() const = 0;
		// Rates selected by firmware during processing, rather than by the host.
		// Declare these so the plugin can prepare conversion filters in advance.
		// Every rate this device can switch to *while running*, i.e. because its own firmware
		// changed the clock - a front panel menu selecting 44.1 vs 48 kHz, say. Nothing here means
		// "my rate never moves", which is true of every device that picks a rate at construction.
		//
		// Declaring them is what makes such a change cheap. Plugin::process() notices the new rate
		// and hands it to the message thread (see applyPendingDeviceSamplerate), which switches the
		// resampler over; a rate listed here already has its converters built and prewarmed, so the
		// switch allocates nothing. A rate that is not listed has to build them on the spot.
		//
		// Note the rate itself is reported by getSamplerate(): this list only says which values it
		// may take, so keep the two in step.
		virtual void getDynamicSamplerates(std::vector<float>& _dst) const {}
		virtual void getPreferredSamplerates(std::vector<float>& _dst) const
		{
			return getSupportedSamplerates(_dst);
		}

		bool isSamplerateSupported(const float& _samplerate) const;

		virtual bool setSamplerate(float _samplerate);

		float getDeviceSamplerate(float _preferredDeviceSamplerate, float _hostSamplerate) const;
		float getDeviceSamplerateForHostSamplerate(float _hostSamplerate) const;

		auto& getDeviceCreateParams() { return m_createParams; }
		const auto& getDeviceCreateParams() const { return m_createParams; }

		virtual bool isValid() const = 0;

#if SYNTHLIB_DEMO_MODE == 0
		virtual bool getState(std::vector<uint8_t>& _state, StateType _type) = 0;
		virtual bool setState(const std::vector<uint8_t>& _state, StateType _type) = 0;
		virtual bool setStateFromUnknownCustomData(const std::vector<uint8_t> &_state) { return false; }
#endif

		virtual uint32_t getChannelCountIn() = 0;
		virtual uint32_t getChannelCountOut() = 0;

		virtual bool setDspClockPercent(uint32_t _percent = 100) = 0;
		virtual uint32_t getDspClockPercent() const = 0;
		virtual uint64_t getDspClockHz() const = 0;
		virtual bool canModifyDspClock() const { return false; }

		/* How many threads this device can spread its DSP over. 1 means it renders
		 * on the calling thread only, which is the default and true of every device
		 * that has not opted in. The count is chosen at construction
		 * (DeviceCreateParams::dspThreads) because it fixes the reported latency. */
		virtual uint32_t getMaxDspThreads() const { return 1; }

		/* Offline rendering: the host is producing audio faster than real time -- a
		 * bounce, or a DAW freezing a track -- and the caller is NOT a realtime
		 * thread. Every mechanism that trades audio for meeting a deadline is wrong
		 * here, because there is no deadline: dropping a backlog or filling an
		 * underrun with silence just corrupts the render. A device that has such a
		 * mechanism overrides this and turns it off while the flag is set, so the
		 * host waits for the engine instead.
		 *
		 * Set from AudioProcessor::setNonRealtime(), which can be called before the
		 * device exists, so Plugin remembers it and applies it to whatever device it
		 * is given. */
		virtual void setNonRealtime(const bool _nonRealtime) { m_nonRealtime = _nonRealtime; }
		bool isNonRealtime() const { return m_nonRealtime; }

		auto& getMidiTranslator() { return m_midiTranslator; }

		// DSPBridge server entry points. The server runs devices created by plugin libraries, and on Windows each of
		// them has a heap of its own, so neither side may grow or free memory that the other side allocated. The server
		// only passes data to read, and what these return lives in the device and stays valid until the next call of the
		// same function.
		// They are virtual so that the server runs the code of the library that created the device. Never make them
		// final: the compiler could then call the server's own copy of them.
		virtual const std::vector<SMidiEvent>& bridgeProcess(const TAudioInputs& _inputs, const TAudioOutputs& _outputs, size_t _size, const std::vector<SMidiEvent>& _midiIn);
#if SYNTHLIB_DEMO_MODE == 0
		virtual const std::vector<uint8_t>& bridgeGetState(StateType _type);
#endif
		virtual const std::vector<float>& bridgeGetSupportedSamplerates();
		virtual const std::vector<float>& bridgeGetPreferredSamplerates();

	protected:
		bool m_nonRealtime = false;

		virtual void readMidiOut(std::vector<SMidiEvent>& _midiOut) = 0;
		virtual void processAudio(const TAudioInputs& _inputs, const TAudioOutputs& _outputs, size_t _samples) = 0;
		virtual bool sendMidi(const SMidiEvent& _ev, std::vector<SMidiEvent>& _response) = 0;

		// Host transport start / stop / seek. This is a marker, not MIDI: it carries no
		// status or data bytes, so it must never be handed to sendMidi, where a device
		// pushes an event's bytes into its emulated UART - a lone 0x00 there completes the
		// firmware's running-status message and fakes a Program Change. Devices that rate
		// limit their MIDI input override this to pass the generation on; for everyone
		// else it is a no-op.
		virtual void onTransportDiscontinuity(const SMidiEvent& /*_ev*/) {}

		void dummyProcess(uint32_t _numSamples);

	private:
		DeviceCreateParams m_createParams;
		std::vector<SMidiEvent> m_midiIn;

		uint32_t m_extraLatency = 0;
		uint32_t m_maxExtraLatency = 0;
		uint32_t m_requestedExtraLatency = 0;

		MidiTranslator m_midiTranslator;
		std::vector<SMidiEvent> m_translatorOut;

		std::vector<SMidiEvent> m_bridgeMidiOut;
		std::vector<uint8_t> m_bridgeState;
		std::vector<float> m_bridgeSupportedSamplerates;
		std::vector<float> m_bridgePreferredSamplerates;
	};
}
