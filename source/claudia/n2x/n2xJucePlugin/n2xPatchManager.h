#pragma once

#include "jucePluginEditorLib/patchmanager/patchmanager.h"

#include <string>
#include <vector>

namespace n2xJucePlugin
{
	class Editor;
	class Controller;

	class PatchManager : public jucePluginEditorLib::patchManager::PatchManager
	{
	public:
		PatchManager(Editor& _editor, Rml::Element* _root);
		~PatchManager() override;

		// PatchManager overrides
		bool requestPatchForPart(pluginLib::patchDB::Data& _data, uint32_t _part, uint64_t _userData) override;
		bool loadRomData(pluginLib::patchDB::DataList& _results, uint32_t _bank, uint32_t _program) override;
		pluginLib::patchDB::PatchPtr initializePatch(pluginLib::patchDB::Data&& _sysex, const std::string& _defaultPatchName) override;
		pluginLib::patchDB::Data applyModifications(const pluginLib::patchDB::PatchPtr& _patch, const pluginLib::FileType& _fileType, pluginLib::ExportType _exportType) const override;
		uint32_t getCurrentPart() const override;
		bool activatePatch(const pluginLib::patchDB::PatchPtr& _patch, uint32_t _part) override;
		bool parseFileData(pluginLib::patchDB::DataList& _results, const pluginLib::patchDB::Data& _data, const std::string& _filename) override;

		static std::string getPatchName(const pluginLib::patchDB::Data& _sysex, const std::string& _defaultPatchName = {});
		static bool isValidPatchDump(const pluginLib::patchDB::Data& _sysex);

	private:
		/* The Nord Lead 2x exposes no ROM patches of its own: its patch memory is
		 * flash, which starts uninitialized under emulation, and the firmware image
		 * carries no preset area. So "Factory" here means bank dumps shipped
		 * alongside the ROM (Clavia's Factory Program Library: bank0..3.syx plus
		 * Perf0.syx), split back into their individual single and multi dumps.
		 *
		 * Keyed per FILE, not per sysex bank byte: bank0.syx and bank3.syx both
		 * declare SingleDumpBankB, so keying on the bank would merge two distinct
		 * banks into one source with duplicate program numbers. The bank field is
		 * our own index into m_factoryBanks. */
		struct FactoryBank
		{
			std::string name;
			std::vector<pluginLib::patchDB::Data> patches;
		};

		const std::vector<FactoryBank>& factoryBanks();
		std::vector<FactoryBank> m_factoryBanks;
		bool m_factoryBanksScanned = false;

		Editor& m_editor;
		Controller& m_controller;
	};
}
