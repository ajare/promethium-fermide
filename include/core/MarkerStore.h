#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Marker.h"
#include "core/World.h"

namespace core
{
	// Owns the behaviour of the World's Marker family on behalf of the
	// World that owns it (ADR 0022).
	//
	// Markers are authored World data, not simulation state (ADR 0006):
	// each is a named, stable World-local identity carried by a
	// MarkerSectorObject inside a Sector, with an ID allocated once from
	// the World's `mNextMarkerId` high-water mark and never reused. The
	// store owns no state: the ID counter and every Marker stay World-owned
	// (ADR 0001); the store reaches them, the construction-record seam,
	// the pause gate, and the topology/cell machinery through the World it
	// was given, following the SimulationCoordinator, PlacementValidator,
	// AgentGroupRegistry, PermissionRequirementStore, and AgentTypeCatalog
	// pattern.
	//
	// This is the Marker behaviour moved out of World: ID allocation,
	// creation and validated restoration, placement judging, name-
	// uniqueness judging and deterministic generated names, lookup and
	// mutable access, rename with its authored-record and Furniture-
	// destination upkeep, pathing-property edits behind the pause gate,
	// and removal with its cell deregistration. Every operation is judged
	// before anything is written, so a refusal leaves the World exactly
	// as it was found.
	//
	// `markerHasNoBehaviourReferences` stays on World: it couples to the
	// behaviour runtime's assignment configuration and is not part of the
	// marker-record family. WorldSerialization.cpp and construction-record
	// replay are untouched - behaviour moved out, state did not.
	//
	// World keeps ownership of every entity registry (ADR 0001) and
	// remains the facade (design pattern) through which callers reach
	// these operations; no caller outside World names this class.
	class MarkerStore
	{
	public:

		explicit MarkerStore(World& world);

		MarkerStore(MarkerStore const&) = delete;
		MarkerStore& operator=(MarkerStore const&) = delete;

		virtual ~MarkerStore() = default;

		// ------------------------------------------------------------------
		// Identity and lookup
		// ------------------------------------------------------------------

		// True when a Marker other than `except` already carries exactly
		// this (already-trimmed) name. Inspects Sector objects directly so
		// repeated insertion stays linear per scan rather than cubic.
		bool markerNameTaken(std::string const& trimmed,
			MarkerId except = MarkerId{}) const;

		// The next deterministic "Marker N" name that no existing Marker
		// holds. Throws when the suffix space is exhausted.
		std::string nextGeneratedMarkerName() const;

		// The mutable Marker for a World-issued ID, or nullptr.
		std::shared_ptr<Marker> mutableMarker(MarkerId id) const;

		// The const Marker for a World-issued ID, or nullptr.
		std::shared_ptr<const Marker> lookupMarker(MarkerId id) const;

		// All Marker IDs the World currently holds, in ascending order.
		std::vector<MarkerId> getMarkerIds() const;

		// ------------------------------------------------------------------
		// Placement validation
		// ------------------------------------------------------------------

		// Read-only placement check for a Marker at (levelIndex, xOffset)
		// within a Sector: the Sector must admit Markers, the position must
		// be inside it and finite, the cell must carry a Ground or Walkway
		// floor, and no existing Marker may sit at the same spot unless
		// `allowCoincident` (used by Furniture-owned placement).
		bool canAddSectorMarker(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
			std::string* diagnostic = nullptr) const;
		bool canAddSectorMarkerImpl(uint32_t sectorIndex, uint32_t levelIndex, float xOffset,
			std::string* diagnostic, bool allowCoincident) const;

		// ------------------------------------------------------------------
		// Creation
		// ------------------------------------------------------------------

		// Constructs the Marker object in its cell. Low-level seam used by
		// restoration; callers go through the add/remove family.
		World::CreateObjectResult createMarker(uint32_t layerIndex, uint32_t x,
			uint32_t y, float xOffset, MarkerId id, std::string name,
			MarkerProperties properties, uint32_t* vertexIdentifier = nullptr);

		// Editor placement with a generated unique name.
		World::CreateObjectResult addSectorMarker(uint32_t sectorIndex,
			uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier = nullptr);

		// Explicit naming follows the same validation as rename.
		World::CreateObjectResult addSectorMarker(uint32_t sectorIndex,
			uint32_t levelIndex, float xOffset, std::string const& name,
			uint32_t* vertexIdentifier = nullptr);

		// Restoration of an already-identified Marker: validates identity,
		// name, position, and properties before mutating, then records the
		// authored construction record.
		World::CreateObjectResult addSectorMarkerRestored(uint32_t sectorIndex,
			uint32_t levelIndex, float xOffset, MarkerId id, std::string name,
			MarkerProperties properties = 0, uint32_t* vertexIdentifier = nullptr,
			bool allowCoincident = false);

		// ------------------------------------------------------------------
		// Editing
		// ------------------------------------------------------------------

		bool canRenameMarker(MarkerId id, std::string const& name,
			std::string* diagnostic = nullptr) const;
		bool renameMarker(MarkerId id, std::string const& name,
			std::string* diagnostic = nullptr);

		// Properties which affect routing can only be edited while a built
		// World is paused.
		bool setMarkerProperties(MarkerId id, MarkerProperties properties,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Removal
		// ------------------------------------------------------------------

		// Judged without touching anything: the object must exist, be a
		// Marker, not be Furniture-owned, and carry no behaviour-reference
		// refusal (judged by World, which owns the behaviour coupling).
		bool canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
			std::string* diagnostic = nullptr) const;
		bool removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
			std::string* diagnostic = nullptr);

	private:

		// The store owns nothing: it reads and edits the World's Marker
		// state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
