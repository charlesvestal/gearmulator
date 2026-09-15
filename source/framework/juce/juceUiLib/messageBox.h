#pragma once

#include <functional>
#include <string>
#include <cstdint>

namespace juce
{
	class Component;
}

namespace genericUI
{
	class MessageBox
	{
	public:
		enum class Icon : uint8_t
		{
		    None,
		    Question,
		    Warning,
		    Info,
		};

		enum class Result : uint8_t
		{
			Yes = 0,
			No = 1,
			Ok = Yes,
			Cancel = No
		};

		using Callback = std::function<void(Result)>;

		/* _associatedComponent is not optional in practice on iOS: a native alert is
		 * a UIAlertController and must be presented FROM a view controller, which JUCE
		 * finds via this component. Passed null it has nothing to present from, so the
		 * alert never appears and the callback never fires -- silently. That is what
		 * made "show advanced options" impossible to tick, and with it the DSP clock it
		 * gates: switching the option OFF takes an early return with no dialog, so only
		 * turning it ON was broken. */
		static void showYesNo(Icon _icon, const std::string& _header, const std::string& _message, Callback _callback, juce::Component* _associatedComponent = nullptr);
		static void showOkCancel(Icon _icon, const std::string& _header, const std::string& _message, Callback _callback, juce::Component* _associatedComponent = nullptr);
		static void showOk(Icon _icon, const std::string& _header, const std::string& _message, juce::Component* _associatedComponent = nullptr);
		static void showOk(Icon _icon, const std::string& _header, const std::string& _message, juce::Component* _associatedComponent, std::function<void()> _callback);
		static void showOk(Icon _icon, const std::string& _header, const std::string& _message, std::function<void()> _callback);
	};
}
