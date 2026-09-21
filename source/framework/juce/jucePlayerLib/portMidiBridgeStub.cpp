#include "portMidiBridge.h"

// No portmidi on iOS: porttime/ptmacosx_mach.c includes CoreAudio/HostTime.h,
// which the iOS SDK does not ship. A plugin takes its MIDI from the host, so the
// bridge is dead weight there rather than a missing feature --
// virtualPortsSupported() is false on iOS, so nothing ever starts it. Defined
// anyway so that every call site compiles and links unchanged.

namespace jucePlayer
{
	PortMidiBridge::PortMidiBridge(MidiInputCallback _midiInputCallback, std::string _name, const uint8_t _portCount)
		: juce::Thread("PortMidiBridge")
		, m_midiInputCallback(std::move(_midiInputCallback))
		, m_name(std::move(_name))
		, m_portCount(_portCount)
	{
	}

	PortMidiBridge::~PortMidiBridge() = default;

	bool PortMidiBridge::isOwnVirtualPortName(const juce::String&) { return false; }

	void PortMidiBridge::setEnabled(bool) {}
	void PortMidiBridge::enqueueOutput(const synthLib::SMidiEvent&) {}
	void PortMidiBridge::run() {}
}
