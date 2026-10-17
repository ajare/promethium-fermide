#pragma once

#include "core/StationSystems.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

// Shared per-type settings editors for authored Station configuration
// entries (#652). The Station Systems form tab and the Flow inspector
// render one entry's full settings through this seam against the same
// StationSystemsSettings draft — neither holds a copy, and field edits
// still commit through the caller's pending-edit → commit-on-field-release
// path. New type = descriptor entry + editor function (#638 story 43).
namespace station_editor
{
	// Renders one store's settings widgets, mutating the entry in place.
	// `label` prefixes every widget ("Store 0"), so each surface addresses
	// the same entry with its own naming. `removeLabel` renders the row's
	// Remove button when non-empty; the editor never erases anything —
	// removal stays deferred to the caller until the row has finished
	// rendering, because erasing the element mid-row would destroy the
	// bound reference (and, for a compound store, its nested composition)
	// while the rest of the row still reads it (#557). Returns true when
	// the Remove button was pressed.
	bool renderStoreEntry([[maybe_unused]] core::StationSystemsSettings& draft,
		core::ResourceStoreSettings& store,
		std::string const& label, std::string const& removeLabel);

	// Renders one transfer's settings widgets, mutating the entry in
	// place. Same label and deferred-removal contract as renderStoreEntry.
	bool renderTransferEntry([[maybe_unused]] core::StationSystemsSettings& draft,
		core::ResourceTransferSettings& transfer,
		std::string const& label, std::string const& removeLabel);

	// Full list sections: the deferred-removal loop over every entry plus
	// the Add button. Section headings stay with the calling surface.
	void renderStoreList(core::StationSystemsSettings& draft);
	void renderTransferList(core::StationSystemsSettings& draft);

	// #647: the power and conversion chain entry editors. Same label and
	// deferred-removal contract as renderStoreEntry; the Flow inspector
	// and the Station Systems form tab render one entry's full settings
	// through these against the same draft.
	bool renderEquipmentLoadEntry(core::StationSystemsSettings& draft,
		core::EquipmentLoadSettings& load,
		std::string const& label, std::string const& removeLabel);
	bool renderBatteryEntry(core::StationSystemsSettings& draft,
		core::BatterySettings& battery,
		std::string const& label, std::string const& removeLabel);
	bool renderGeneratorEntry(core::StationSystemsSettings& draft,
		core::GeneratorSettings& generator,
		std::string const& label, std::string const& removeLabel);
	bool renderFuelCellEntry(core::StationSystemsSettings& draft,
		core::FuelCellSettings& cell,
		std::string const& label, std::string const& removeLabel);
	bool renderSabatierReactorEntry(core::StationSystemsSettings& draft,
		core::SabatierReactorSettings& reactor,
		std::string const& label, std::string const& removeLabel);
	bool renderMethaneProcessorEntry(core::StationSystemsSettings& draft,
		core::MethaneProcessorSettings& processor,
		std::string const& label, std::string const& removeLabel);
	// #656: the thermal reservoir entry editor. Same label and
	// deferred-removal contract as renderStoreEntry; the Flow inspector
	// and the Station Systems form tab render one reservoir's full
	// settings through this against the same draft.
	bool renderThermalReservoirEntry(core::StationSystemsSettings& draft,
		core::ThermalReservoirSettings& reservoir,
		std::string const& label, std::string const& removeLabel);
	// #657: the radiator entry editor. Same label and deferred-removal
	// contract as renderStoreEntry; the Flow inspector and the Station
	// Systems form tab render one radiator's full settings through this
	// against the same draft. The host reservoir field is the same
	// reservoirId the Flow host connector writes.
	bool renderRadiatorEntry(core::StationSystemsSettings& draft,
		core::RadiatorSettings& radiator,
		std::string const& label, std::string const& removeLabel);
	void renderEquipmentLoadList(core::StationSystemsSettings& draft);
	void renderBatteryList(core::StationSystemsSettings& draft);
	void renderGeneratorList(core::StationSystemsSettings& draft);
	void renderFuelCellList(core::StationSystemsSettings& draft);
	void renderSabatierList(core::StationSystemsSettings& draft);
	void renderMethaneProcessorList(core::StationSystemsSettings& draft);
	void renderThermalReservoirList(core::StationSystemsSettings& draft);
	void renderRadiatorList(core::StationSystemsSettings& draft);
	bool renderCoolantLoopEntry(core::StationSystemsSettings& draft,
		core::CoolantLoopSettings& loop,
		std::string const& label, std::string const& removeLabel);

	bool renderThermalBufferEntry(core::StationSystemsSettings& draft,
		core::ThermalBufferSettings& buffer,
		std::string const& label, std::string const& removeLabel);

	bool renderChillerEntry(core::StationSystemsSettings& draft,
		core::ChillerSettings& chiller,
		std::string const& label, std::string const& removeLabel);

	// Descriptor registry keyed by the Flow node type name. A type absent
	// here has no extracted editor yet; the index addresses the entry in
	// the draft vector the type owns. The registered renderer presents
	// fields only — no Remove button — because removal belongs to the
	// surface that owns the list and its deferred-removal loop.
	struct StationEntryEditor
	{
		std::string_view nodeType;
		bool (*renderEntry)(core::StationSystemsSettings& draft, size_t index);
	};
	std::span<StationEntryEditor const> stationEntryEditors();
	StationEntryEditor const* findStationEntryEditor(std::string_view nodeType);
}
