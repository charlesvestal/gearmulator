#pragma once

#include "jeLib/rom.h"

#include "jucePluginEditorLib/pluginProcessor.h"

namespace jeJucePlugin
{
	class AudioPluginAudioProcessor : public jucePluginEditorLib::Processor
	{
	public:
	    AudioPluginAudioProcessor();
	    ~AudioPluginAudioProcessor() override;

	    jucePluginEditorLib::PluginEditorState* createEditorState() override;
	    synthLib::Device* createDevice() override;
		void getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const override;

	    pluginLib::Controller* createController() override;

		/* The host publishes its audio workgroup here. The pipeline's stage
		 * threads have to join it: they are helpers for the same render cycle,
		 * and on Apple silicon a workgroup is the only thing that reliably keeps
		 * them on performance cores. */
		void audioWorkgroupContextChanged(const juce::AudioWorkgroup& _workgroup) override;

		const jeLib::Rom& getSelectedRom() const;
		const auto& getRoms() const { return m_roms; }
		size_t getSelectedRomIndex() const { return m_selectedRom; }

    private:
		juce::AudioWorkgroup m_audioWorkgroup;
		std::vector<jeLib::Rom> m_roms;
		size_t m_selectedRom = 0;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
	};
}
