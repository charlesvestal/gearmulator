#include "parameter.h"

#include "controller.h"

#if TUS_PARAM_DIAGNOSTICS
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#endif

namespace pluginLib
{
	namespace
	{
		std::set<Parameter*> g_aliveParameters;
	}

#if TUS_PARAM_DIAGNOSTICS
	/* Where a parameter change came from and how far it got, for working out why a
	 * host's automation does not reach the synth. An AUv3 has no console, so this
	 * goes to a file in the extension's own container that devicectl can copy off.
	 *
	 * OFF unless TUS_PARAM_DIAGNOSTICS is defined at build time
	 * (EXTRA_CXX_FLAGS=-DTUS_PARAM_DIAGNOSTICS=1). It writes from whatever thread
	 * the change arrived on, the audio thread included, so it is a debugging build
	 * only -- never ship it. */
	void paramDiag(const char* _fmt, ...)
	{
		static FILE* f = [] () -> FILE*
		{
			const char* home = getenv("HOME");
			if (!home) return nullptr;
			const std::string path = std::string(home) + "/Documents/tus_param_diag.txt";
			FILE* h = fopen(path.c_str(), "w");
			if (h) setvbuf(h, nullptr, _IOLBF, 0);
			return h;
		}();
		if (!f) return;

		static std::mutex m;
		const std::scoped_lock lock(m);

		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		fprintf(f, "%lld ", static_cast<long long>(ms % 1000000));

		va_list args;
		va_start(args, _fmt);
		vfprintf(f, _fmt, args);
		va_end(args);
		fputc('\n', f);
		fflush(f);
	}
#else
	void paramDiag(const char*, ...) {}
#endif

	Parameter::Parameter(Controller& _controller, const Description& _desc, const uint8_t _partNum, const int _uniqueId, const PartFormatter& _partFormatter)
		: juce::RangedAudioParameter(genId(_desc, _partNum, _uniqueId), _partFormatter(_partNum, _desc.isNonPartSensitive()) + " " + _desc.displayName)
		, m_controller(_controller)
		, m_desc(_desc)
		, m_part(_partNum)
		, m_uniqueId(_uniqueId)
	{
		m_range.start = static_cast<float>(m_desc.range.getStart());
		m_range.end = static_cast<float>(m_desc.range.getEnd());
		m_range.interval = m_desc.step ? static_cast<float>(m_desc.step) : (m_desc.isDiscrete || m_desc.isBool ? 1.0f : 0.0f);

		m_value.setValue(m_range.start);
		m_value.addListener(this);

		g_aliveParameters.insert(this);
    }

	Parameter::~Parameter()
	{
		g_aliveParameters.erase(this);
	}

	void Parameter::valueChanged(juce::Value&)
    {
		sendToSynth(m_lastValueOrigin);
		onValueChanged(this);
	}

    void Parameter::setDerivedValue(const int _value)
    {
		const int newValue = clampValue(_value);

		if (newValue == m_lastValue)
			return;

		m_lastValue = newValue;
		m_lastValueOrigin = Origin::Derived;

		m_value.setValue(newValue);
	}

    void Parameter::sendToSynth(const Origin _origin)
    {
		const float floatValue = m_value.getValue();
		const auto value = juce::roundToInt(floatValue);

		jassert(m_range.getRange().contains(floatValue) || m_range.end == floatValue);

		if (value == m_lastValue)
		{
			paramDiag("  sendToSynth %s part=%u value=%d SKIPPED (unchanged)", m_desc.name.c_str(), m_part, value);
			return;
		}

		// ignore initial update
		if (m_lastValue != -1)
		{
			if(m_rateLimit)
			{
				paramDiag("  sendToSynth %s part=%u value=%d DELAYED (rateLimit=%u)", m_desc.name.c_str(), m_part, value, m_rateLimit);
				sendParameterChangeDelayed(value, _origin);
			}
			else
			{
				paramDiag("  sendToSynth %s part=%u value=%d NOW", m_desc.name.c_str(), m_part, value);
				sendParameterChangeNow(value, _origin);
			}
		}
		else
		{
			paramDiag("  sendToSynth %s part=%u value=%d SKIPPED (initial update)", m_desc.name.c_str(), m_part, value);
		}

		m_lastValue = value;
    }

	void Parameter::sendParameterChangeNow(const ParamValue _value, const Origin _origin)
	{
		m_lastSendTime = milliseconds();
		m_controller.sendParameterChange(*this, _value, _origin);
	}

    uint64_t Parameter::milliseconds()
    {
		const auto t = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
		return t.count();
    }

	void Parameter::scheduleTimer(const uint64_t _delayMs)
	{
		/* juce::Timer::callAfterDelay from the AUDIO thread is the thing to watch
		 * here: host automation arrives there, and a timer started off the message
		 * thread is not something JUCE promises will fire. */
		paramDiag("  scheduleTimer %s part=%u delay=%llums msgThread=%d",
			m_desc.name.c_str(), m_part, static_cast<unsigned long long>(_delayMs),
			juce::MessageManager::existsAndIsCurrentThread() ? 1 : 0);

		juce::Timer::callAfterDelay(static_cast<int>(_delayMs), [this]
		{
			if (g_aliveParameters.count(this))
			{
				paramDiag("  timer fired %s part=%u", m_desc.name.c_str(), m_part);
				sendPendingParameterChange();
			}
		});
	}

	void Parameter::sendParameterChangeDelayed(const ParamValue _value, Origin _origin)
	{
		const auto now = milliseconds();
		const auto elapsed = now - m_lastSendTime;

		if(elapsed >= m_rateLimit)
		{
			m_pendingChange.store(PendingChange{}, std::memory_order_release);
			sendParameterChangeNow(_value, _origin);
		}
		else
		{
			PendingChange next{};
			next.value = _value;
			next.origin = static_cast<uint8_t>(_origin);
			next.hasPending = true;

			const auto prev = m_pendingChange.exchange(next, std::memory_order_acq_rel);
			if (!prev.hasPending)
				scheduleTimer(m_rateLimit - elapsed);
		}
    }

	void Parameter::sendPendingParameterChange()
	{
		const auto snap = m_pendingChange.load(std::memory_order_acquire);
		if (!snap.hasPending)
			return;

		// Guard against juce::Timer firing too early
		if(m_rateLimit)
		{
			const auto elapsed = milliseconds() - m_lastSendTime;
			if(elapsed < m_rateLimit)
			{
				scheduleTimer(m_rateLimit - elapsed);
				return;
			}
		}

		// Clear only if nothing newer was written by the audio thread in the
		// meantime. On a CAS failure the newer state already has hasPending=1,
		// so we reschedule to pick it up on the next tick.
		PendingChange expected = snap;
		if (m_pendingChange.compare_exchange_strong(expected, PendingChange{},
		                                             std::memory_order_acq_rel))
		{
			sendParameterChangeNow(snap.value, static_cast<Origin>(snap.origin));
		}
		else
		{
			scheduleTimer(m_rateLimit ? m_rateLimit : 1);
		}
	}

    int Parameter::clampValue(const int _value) const
    {
		return juce::roundToInt(m_range.getRange().clipValue(static_cast<float>(_value)));
    }

    void Parameter::setValueNotifyingHost(const float _value, const Origin _origin)
    {
		ScopedChangeGesture g(*this, _origin);
		setUnnormalizedValue(juce::roundToInt(convertFrom0to1(_value)), _origin);
		notifyHost(_value);
	}

    void Parameter::setUnnormalizedValueNotifyingHost(const float _value, const Origin _origin)
    {
		ScopedChangeGesture g(*this, _origin);
		setUnnormalizedValue(juce::roundToInt(_value), _origin);
		notifyHost(convertTo0to1(_value));
    }

    void Parameter::setUnnormalizedValueNotifyingHost(const int _value, const Origin _origin)
    {
		ScopedChangeGesture g(*this, _origin);
		setUnnormalizedValue(_value, _origin);
		notifyHost(convertTo0to1(static_cast<float>(_value)));
    }

    void Parameter::setRateLimitMilliseconds(const uint32_t _ms)
    {
	    m_rateLimit = _ms;
    }

    void Parameter::setLinkState(const ParameterLinkType _type)
    {
		const auto prev = m_linkType;
		m_linkType = static_cast<ParameterLinkType>(m_linkType | _type);
		if(m_linkType != prev)
			onLinkStateChanged(this, m_linkType);
    }

    void Parameter::clearLinkState(const ParameterLinkType _type)
    {
		const auto prev = m_linkType;
		m_linkType = static_cast<ParameterLinkType>(m_linkType & ~_type);
		if(m_linkType != prev)
			onLinkStateChanged(this, m_linkType);
    }

    void Parameter::pushChangeGesture()
    {
		if (!getDescription().isPublic)
			return;
		if(!m_changeGestureCount)
			beginChangeGesture();
		++m_changeGestureCount;
    }

    void Parameter::popChangeGesture()
    {
		if (!getDescription().isPublic)
			return;
		
		assert(m_changeGestureCount > 0);
		--m_changeGestureCount;

		if(!m_changeGestureCount)
		{
			// Flush any pending rate-limited parameter value when gesture ends
			sendPendingParameterChange();
			endChangeGesture();
		}
	}

    bool Parameter::requiresGesture(Origin _origin)
    {
	    switch (_origin)
	    {
	    case Origin::Unknown:
	    case Origin::Ui:
			return true;
	    case Origin::Derived:
	    case Origin::PresetChange:
	    case Origin::Midi:
	    case Origin::HostAutomation:
		default:
			return false;
	    }
    }

    bool Parameter::isMetaParameter() const
    {
	    return !m_derivedParameters.empty();
    }

    void Parameter::setValue(const float _newValue)
	{
		// some plugin formats (VST3 for example) bounce back immediately, skip this, we don't
		// want it and VST2 doesn't do it either so why does Juce for VST3?
		// It's not the host, it's the Juce VST3 implementation
		if(m_notifyingHost)
		{
			paramDiag("setValue  %s part=%u norm=%.4f DROPPED (notifyingHost)",
				m_desc.name.c_str(), m_part, _newValue);
			return;
		}

		paramDiag("setValue  %s part=%u norm=%.4f -> %d  msgThread=%d",
			m_desc.name.c_str(), m_part, _newValue,
			juce::roundToInt(convertFrom0to1(_newValue)),
			juce::MessageManager::existsAndIsCurrentThread() ? 1 : 0);

		setUnnormalizedValue(juce::roundToInt(convertFrom0to1(_newValue)), Origin::HostAutomation);
	}

    void Parameter::setUnnormalizedValue(const int _newValue, const Origin _origin)
    {
		if (m_changingDerivedValues)
			return;

		m_lastValueOrigin = _origin;
		m_value.setValue(clampValue(_newValue));

		if(_origin != Origin::Derived)
			sendToSynth(_origin);

		forwardToDerived(_newValue);
    }

    void Parameter::setValueFromSynth(const int _newValue, const Origin _origin)
	{
		const auto clampedValue = clampValue(_newValue);

		// we do not want to send an excessive amount of value changes to the host if a preset is
		// changed, we use updateHostDisplay() (see caller) to inform the host to read all
		// parameters again instead
		const auto notifyHost = _origin != Origin::PresetChange;

		if (clampedValue != m_lastValue)
		{
			m_lastValue = clampedValue;
			m_lastValueOrigin = _origin;

			if (notifyHost && getDescription().isPublic)
			{
				setUnnormalizedValueNotifyingHost(clampedValue, _origin);
			}
			else
			{
				m_value.setValue(clampedValue);
			}
		}

		forwardToDerived(_newValue);
	}

    void Parameter::forwardToDerived(const int _newValue)
    {
		if (m_changingDerivedValues)
			return;

		m_changingDerivedValues = true;

		for (const auto& p : m_derivedParameters)
			p->setDerivedValue(_newValue);

		m_changingDerivedValues = false;
    }

    void Parameter::notifyHost(const float _value)
    {
		m_notifyingHost = true;
		sendValueChangedMessageToListeners(_value);
		m_notifyingHost = false;
    }

    juce::ParameterID Parameter::genId(const Description& d, const int part, const int uniqueId)
	{
		juce::String s;

		if(uniqueId > 0)
			s = juce::String::formatted("%d_%d_%d_%d", static_cast<int>(d.page), part, d.index, uniqueId);
		else
			s = juce::String::formatted("%d_%d_%d", static_cast<int>(d.page), part, d.index);

		if (d.version > 0)
			return { s, d.version };
		return { s };
	}

	float Parameter::getValueForText(const juce::String& _text) const
	{
		auto res = m_desc.valueList.textToValue(std::string(_text.getCharPointer()));
		if(m_desc.range.getStart() < 0)
			res += m_desc.range.getStart();
		return convertTo0to1(static_cast<float>(res));
	}

	ParamValue Parameter::getDefault() const
	{
		if(m_desc.defaultValue != Description::NoDefaultValue)
			return m_desc.defaultValue;
		return 0;
	}

	juce::String Parameter::getText(const float _normalisedValue, int _i) const
	{
		const auto v = convertFrom0to1(_normalisedValue);
		return m_desc.valueList.valueToText(juce::roundToInt(v) - std::min(0, m_desc.range.getStart()));
	}

	void Parameter::setLocked(const bool _locked)
	{
		if(m_isLocked == _locked)
			return;

		m_isLocked = _locked;

		onLockedChanged(this, m_isLocked);
	}

	void Parameter::addDerivedParameter(Parameter* _param)
	{
		if (_param == this)
			return;

		for (auto* p : m_derivedParameters)
		{
			_param->m_derivedParameters.insert(p);
			p->m_derivedParameters.insert(_param);
		}

		m_derivedParameters.insert(_param);
		_param->m_derivedParameters.insert(this);
	}

	Parameter::ScopedChangeGesture::ScopedChangeGesture(Parameter& _p, const Origin _origin) : m_parameter(_p), m_origin(_origin)
    {
		if(_p.getDescription().isPublic && requiresGesture(_origin))
		{
			// beginChangeGesture/endChangeGesture must only be called on the
			// message thread. Every caller that passes Origin::Ui or Origin::Unknown
			// is expected to run there; an audio-thread call here would race the
			// host's parameter subscribers and can crash some DAWs.
			jassert(juce::MessageManager::existsAndIsCurrentThread());
		    _p.pushChangeGesture();
		}
    }

    Parameter::ScopedChangeGesture::~ScopedChangeGesture()
    {
		if(m_parameter.getDescription().isPublic && requiresGesture(m_origin))
		{
			jassert(juce::MessageManager::existsAndIsCurrentThread());
		    m_parameter.popChangeGesture();
		}
    }
}
