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
