#include "jePluginProcessor.h"

#include "jeLib/jePipeline.h"

#include "jeController.h"
#include "jePluginEditorState.h"

// ReSharper disable once CppUnusedIncludeDirective
#include "BinaryData.h"
#include "jeLib/device.h"
#include "jeLib/romloader.h"
#include "jucePluginLib/processorPropertiesInit.h"

#include "synthLib/deviceException.h"

namespace jeJucePlugin
{
	class Controller;

	AudioPluginAudioProcessor::AudioPluginAudioProcessor() :
	    Processor(BusesProperties()
	                   .withOutput("Out AB", juce::AudioChannelSet::stereo(), true)
	                   .withInput("Input", juce::AudioChannelSet::stereo(), true)
		, {}, pluginLib::initProcessorProperties())
	{
		m_roms = jeLib::RomLoader::findROMs();

		// default to keyboard for now
		m_selectedRom = std::numeric_limits<size_t>::max();
		for (size_t i=0; i<m_roms.size(); ++i)
		{
			if (m_roms[i].getDeviceType() != jeLib::DeviceType::Keyboard)
				continue;
			m_selectedRom = i;
			break;
		}

		// FIXME clear rom list if there is no keyboard rom to prevent that the rack rom is used
		if (m_selectedRom == std::numeric_limits<size_t>::max())
		{
			m_roms.clear();
			m_selectedRom = 0;
		}

		getController();
#if JUCE_IOS
		/* The engine may run ahead of the host by latencyBlocks host blocks, and
		 * the default of 1 gives a four-stage pipeline no runway at all: AUM
		 * calls us with 58-sample blocks, so the stages are interrupted and
		 * resynchronised 750 times a second and never reach steady state. The
		 * same session at a 2048-sample buffer drops from a pegged DSP meter to
		 * 25%, which is the runway talking, not the engine's speed.
		 *
		 * 8 blocks is ~2048 samples at the 256 a Bluetooth output forces on us.
		 * It should be computed from the block size once that is known rather
		 * than assumed here. */
		const auto defaultLatencyBlocks = 8;
#else
		const auto defaultLatencyBlocks = static_cast<int>(getPlugin().getLatencyBlocks());
#endif
		const auto latencyBlocks = getConfig().getIntValue("latencyBlocks", defaultLatencyBlocks);
		Processor::setLatencyBlocks(latencyBlocks);

		/* Read before the device exists: the thread count is applied at creation
		 * because it fixes the reported latency and cannot change while running. */
#if JUCE_IOS
		/* iOS cannot JIT, so the ESP cores are interpreted (~15x the cost) and
		 * one thread renders at a fraction of real time -- silence and a pegged
		 * DSP meter. The four-stage pipeline is what makes it playable, and the
		 * AUv3 has no reachable settings page, so it is the DEFAULT here. */
		constexpr int defaultDspThreads = 4;
#else
		constexpr int defaultDspThreads = 0;
#endif
		pluginLib::Processor::setDspThreads(static_cast<uint32_t>(getConfig().getIntValue("dspThreads", defaultDspThreads)));
	}

	AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
	{
		destroyEditorState();
	}

	void AudioPluginAudioProcessor::audioWorkgroupContextChanged(const juce::AudioWorkgroup& _workgroup)
	{
		m_audioWorkgroup = _workgroup;

		/* Type-erased so jeLib stays free of JUCE. The token has to live for as
		 * long as the thread stays in the workgroup, so it is thread_local: each
		 * stage thread joins with its own, and rejoining with the same token
		 * replaces the previous membership rather than stacking. */
		jeLib::pipelineSetWorkgroupJoiner([this]
		{
			thread_local juce::WorkgroupToken token;
			m_audioWorkgroup.join(token);
		});
	}

	jucePluginEditorLib::PluginEditorState* AudioPluginAudioProcessor::createEditorState()
	{
		return new PluginEditorState(*this);
	}

	synthLib::Device* AudioPluginAudioProcessor::createDevice()
	{
		const auto& rom = getSelectedRom();

		auto* errorMsg = "A firmware rom (a single 512k .bin file or multiple .mid files) is required, but was not found.";

		if (!rom.isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing, errorMsg);

		synthLib::DeviceCreateParams params;

		params.romData = rom.getData();
		params.romName = rom.getName();
		params.homePath = getDataFolder();
		params.dspThreads = getDspThreads();

		auto* d = new jeLib::Device(params);
		if(!d->isValid())
			throw synthLib::DeviceException(synthLib::DeviceError::FirmwareMissing, errorMsg);
		return d;
	}

	void AudioPluginAudioProcessor::getRemoteDeviceParams(synthLib::DeviceCreateParams& _params) const
	{
		Processor::getRemoteDeviceParams(_params);

		auto rom = jeLib::RomLoader::findROM();

		if (rom.isValid())
		{
			auto data = rom.getData();
			_params.romData.assign(data.begin(), data.end());
			_params.romName = rom.getName();
		}
	}

	pluginLib::Controller* AudioPluginAudioProcessor::createController()
	{
		return new jeJucePlugin::Controller(*this);
	}

	const jeLib::Rom& AudioPluginAudioProcessor::getSelectedRom() const
	{
		if (m_roms.empty())
		{
			static jeLib::Rom emptyRom;
			return emptyRom;
		}
		if (getSelectedRomIndex() >= m_roms.size())
			return m_roms.back();
		return m_roms[m_selectedRom];
	}
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new jeJucePlugin::AudioPluginAudioProcessor();
}
