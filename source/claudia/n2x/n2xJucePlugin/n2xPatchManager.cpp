#include "n2xPatchManager.h"

#include "n2xController.h"
#include "n2xEditor.h"
#include "n2xFileType.h"

#include "juce_cryptography/hashing/juce_MD5.h"

#include "n2xLib/n2xmiditypes.h"

#include "dsp56kBase/logging.h"

#include "synthLib/midiToSysex.h"
#include "synthLib/romLoader.h"
#include "baseLib/filesystem.h"

#include <algorithm>
#include <set>

namespace n2xJucePlugin
{
	constexpr char g_performancePrefixes[] = {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'L' };

	static constexpr std::initializer_list<jucePluginEditorLib::patchManager::GroupType> g_groupTypes =
	{
		jucePluginEditorLib::patchManager::GroupType::Favourites,
		// Rom datasources map to Factory (patchmanager/types.cpp); without the
		// group here they would load and have nowhere to be shown.
		jucePluginEditorLib::patchManager::GroupType::Factory,
		jucePluginEditorLib::patchManager::GroupType::MidiBanks,
		jucePluginEditorLib::patchManager::GroupType::LocalStorage,
		jucePluginEditorLib::patchManager::GroupType::DataSources,
	};

	PatchManager::PatchManager(Editor& _editor, Rml::Element* _root)
	: jucePluginEditorLib::patchManager::PatchManager(_editor, _root, g_groupTypes)
	, m_editor(_editor)
	, m_controller(_editor.getN2xController())
	{
		setTagTypeName(pluginLib::patchDB::TagType::CustomA, "Patch Type");
		startLoaderThread();
		addGroupTreeItemForTag(pluginLib::patchDB::TagType::CustomA);

		// _save is false: this is derived from files on disk, so persisting it
		// would leave the DB carrying a stale copy once a file changes or goes away.
		const auto& banks = factoryBanks();

		for (uint32_t i = 0; i < static_cast<uint32_t>(banks.size()); ++i)
		{
			pluginLib::patchDB::DataSource ds;
			ds.type = pluginLib::patchDB::SourceType::Rom;
			ds.origin = pluginLib::patchDB::DataSourceOrigin::Manual;
			ds.bank = i;
			ds.name = banks[i].name;
			addDataSource(ds, false);
		}
	}

	PatchManager::~PatchManager()
	{
		stopLoaderThread();
	}

	bool PatchManager::requestPatchForPart(pluginLib::patchDB::Data& _data, const uint32_t _part, const uint64_t _userData)
	{
		if(_userData)
			_data = m_controller.createMultiDump(n2x::SysexByte::MultiDumpBankA, 0);
		else
			_data = m_controller.createSingleDump(n2x::SysexByte::SingleDumpBankA, 0, static_cast<uint8_t>(_part));
		return !_data.empty();
	}

	const std::vector<PatchManager::FactoryBank>& PatchManager::factoryBanks()
	{
		if(m_factoryBanksScanned)
			return m_factoryBanks;
		m_factoryBanksScanned = true;

		// Identified by content, not by name. The firmware ROM is a .bin, so it is
		// never a candidate here; the size window spans a program bank (~24 KB) and
		// a performance bank (~72 KB) in either container.
		std::vector<std::string> files;

		for (const auto& ext : {".syx", ".mid"})
		{
			auto found = synthLib::RomLoader::findFiles(ext, 8 * 1024, 256 * 1024);
			files.insert(files.end(), found.begin(), found.end());
		}

		std::sort(files.begin(), files.end());

		for (const auto& file : files)
		{
			std::vector<uint8_t> data;
			if(!baseLib::filesystem::readFile(data, file) || data.empty())
				continue;

			// The flag is _isMidiFileData, and it is NOT cosmetic: a .mid wraps each
			// message in a varlen length, a .syx is the raw stream. Passing true for
			// a .syx sends it down the MIDI-file branch, which reads a length that
			// is not there and finds nothing -- silently, since an empty scan is
			// indistinguishable from "no bank files present".
			// hasExtension() lowercases both sides; getExtension() does not, so a
			// .SYX would otherwise be taken for a MIDI file.
			const auto isMidiFile = !baseLib::filesystem::hasExtension(file, ".syx");

			std::vector<std::vector<uint8_t>> messages;
			synthLib::MidiToSysex::splitMultipleSysex(messages, data, isMidiFile);

			FactoryBank bank;

			for (auto& m : messages)
			{
				// isValidPatchDump() checks the dump SIZE first, so Clavia's ten
				// trailing 1063-byte blocks (four singles' worth of data under a
				// single-dump type, at programs 99..108) are rejected here, as are
				// the out-of-range program numbers they carry.
				if(isValidPatchDump(m))
					bank.patches.emplace_back(std::move(m));
			}

			if(bank.patches.empty())
			{
				LOG("Factory bank: no valid dumps in " << file << " (" << messages.size() << " sysex messages)");
				continue;
			}

			bank.name = baseLib::filesystem::stripExtension(baseLib::filesystem::getFilenameWithoutPath(file));

			LOG("Factory bank '" << bank.name << "': " << bank.patches.size() << " dumps from " << file);
			m_factoryBanks.emplace_back(std::move(bank));
		}

		return m_factoryBanks;
	}

	bool PatchManager::loadRomData(pluginLib::patchDB::DataList& _results, const uint32_t _bank, uint32_t /*_program*/)
	{
		// The whole bank is asked for at once (db.cpp passes g_invalidProgram).
		const auto& banks = factoryBanks();

		if(_bank >= banks.size())
			return false;

		for (const auto& p : banks[_bank].patches)
			_results.push_back(p);

		return !_results.empty();
	}

	pluginLib::patchDB::PatchPtr PatchManager::initializePatch(pluginLib::patchDB::Data&& _sysex, const std::string& _defaultPatchName)
	{
		if(!isValidPatchDump(_sysex))
			return {};

		const auto bank = _sysex[n2x::SysexIndex::IdxMsgType];
		const auto program = _sysex[n2x::SysexIndex::IdxMsgSpec];
		const auto isSingle = n2x::State::isSingleDump(_sysex);

		auto p = std::make_shared<pluginLib::patchDB::Patch>();

		p->tags.add(pluginLib::patchDB::TagType::CustomA, isSingle ? "Program" : "Performance");

		if(isSingle)
		{
			const auto distRmSync = n2x::State::getSingleParam(_sysex, n2x::SingleParam::Distortion, 0);
			if(distRmSync & 1)
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Sync");
			if(distRmSync & 2)
				p->tags.add(pluginLib::patchDB::TagType::Tag, "RingMod");
			if(distRmSync & (1<<4))
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Distortion");

			if(n2x::State::getSingleParam(_sysex, n2x::SingleParam::Unison, 0))
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Unison");

			const auto voiceMode = n2x::State::getSingleParam(_sysex, n2x::SingleParam::VoiceMode, 0);
			if(voiceMode == 2)
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Poly");
			else if(voiceMode == 1)
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Legato");
			else
				p->tags.add(pluginLib::patchDB::TagType::Tag, "Mono");
		}

		p->name = getPatchName(_sysex, _defaultPatchName);
		p->sysex = std::move(_sysex);
		p->program = program;
		p->bank = bank;

		const juce::MD5 md5(p->sysex.data() + n2x::g_sysexHeaderSize, p->sysex.size() - n2x::g_sysexContainerSize);
		static_assert(sizeof(juce::MD5) >= sizeof(pluginLib::patchDB::PatchHash));
		memcpy(p->hash.data(), md5.getChecksumDataArray(), std::size(p->hash));

		return p;
	}

	pluginLib::patchDB::Data PatchManager::applyModifications(const pluginLib::patchDB::PatchPtr& _patch, const pluginLib::FileType& _fileType, pluginLib::ExportType _exportType) const
	{
		auto d = n2x::State::stripPatchName(_patch->sysex);

		d[n2x::SysexIndex::IdxMsgType] = static_cast<uint8_t>(_patch->bank);
		d[n2x::SysexIndex::IdxMsgSpec] = static_cast<uint8_t>(_patch->program);

		if (_fileType == fileType::g_nl2 || _exportType != pluginLib::ExportType::File)
		{
			auto name = _patch->getName();

			if(name.size() > n2x::g_nameLength)
				name = name.substr(0, n2x::g_nameLength);
			while(name.size() < n2x::g_nameLength)
				name.push_back(' ');

			d.pop_back();
			d.insert(d.end(), name.begin(), name.end());
			d.push_back(0xf7);
		}

		return d;
	}

	uint32_t PatchManager::getCurrentPart() const
	{
		return m_controller.getCurrentPart();
	}

	bool PatchManager::activatePatch(const pluginLib::patchDB::PatchPtr& _patch, const uint32_t _part)
	{
		if(!m_controller.activatePatch(_patch->sysex, _part))
			return false;

		m_editor.onPatchActivated(_patch, _part);
		return true;
	}

	bool PatchManager::parseFileData(pluginLib::patchDB::DataList& _results, const pluginLib::patchDB::Data& _data, const std::string& _filename)
	{
		return jucePluginEditorLib::patchManager::PatchManager::parseFileData(_results, _data, _filename);
	}

	std::string PatchManager::getPatchName(const pluginLib::patchDB::Data& _sysex, const std::string& _defaultPatchName/* = {}*/)
	{
		if(!isValidPatchDump(_sysex))
			return _defaultPatchName;

		{
			const auto nameFromDump = n2x::State::extractPatchName(_sysex);
			if(!nameFromDump.empty())
				return nameFromDump;
		}

		if (!_defaultPatchName.empty())
			return _defaultPatchName;

		const auto isSingle = n2x::State::isSingleDump(_sysex);

		const auto bank = _sysex[n2x::SysexIndex::IdxMsgType];
		const auto program = _sysex[n2x::SysexIndex::IdxMsgSpec];

		char name[128]{0};

		auto getBankChar = [&]() -> char
		{
			if(isSingle)
			{
				if(bank == n2x::SingleDumpBankEditBuffer)
					return 'e';
				return static_cast<char>('0' + bank - n2x::SysexByte::SingleDumpBankA);
			}
			if(bank == n2x::MultiDumpBankEditBuffer)
				return 'e';

			return static_cast<char>('0' + bank - n2x::SysexByte::MultiDumpBankA);
		};

		if(isSingle)
		{
			(void)snprintf(name, sizeof(name), "%c.%02d", getBankChar(), program);
		}
		else
		{
			(void)snprintf(name, sizeof(name), "%c.%c%01d", getBankChar(), g_performancePrefixes[(program/10)%std::size(g_performancePrefixes)], program % 10);
		}

		return name;
	}

	bool PatchManager::isValidPatchDump(const pluginLib::patchDB::Data& _sysex)
	{
		const auto isSingle = n2x::State::isSingleDump(_sysex);
		const auto isMulti = n2x::State::isMultiDump(_sysex);

		if(!isSingle && !isMulti)
			return false;

		const auto deviceId = _sysex[n2x::SysexIndex::IdxDevice];
		const auto bank = _sysex[n2x::SysexIndex::IdxMsgType];
		const auto program = _sysex[n2x::SysexIndex::IdxMsgSpec];

		if(deviceId > 15)
			return false;

		if(program > n2x::g_programsPerBank)
			return false;

		if(isSingle && (bank < n2x::SysexByte::SingleDumpBankEditBuffer || bank > (n2x::SysexByte::SingleDumpBankEditBuffer + n2x::g_singleBankCount)))
			return false;

		if(isMulti && (bank < n2x::SysexByte::MultiDumpBankEditBuffer || bank > (n2x::SysexByte::MultiDumpBankEditBuffer + n2x::g_multiBankCount)))
			return false;

		return true;
	}
}
