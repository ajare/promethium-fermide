#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "core/Furniture.h"
#include "core/World.h"

namespace core
{
	// Owns the Furniture instance, catalogue-reference, and usable-point
	// resolution behaviour on behalf of the World that owns it (ADR 0022).
	//
	// Furniture is catalogue-backed: a loaded `.furniture.lua` catalogue
	// (an application Resource, ADR 0010) supplies the definitions, and
	// each placed instance is a World-owned record borrowing its geometry,
	// usable points, and owned Marker destinations from the catalogue it
	// was placed with. This collaborator moves the behaviour around that
	// model: catalogue attach with its Resource-name and replacement rules;
	// the paused reload transaction that reconciles every saved instance
	// against a replacement package before installing it; placement with
	// its footprint, support, name, and identity-space validation; the
	// candidate-replay edit and removal transactions; the support
	// diagnostic; construction-record restoration with its destination
	// reconciliation; and the single-pass Marker -> instance/definition/
	// destination/usable-point/Sector resolution (#627).
	//
	// The state itself - `mFurniture`, `mFurnitureCatalogue`,
	// `mFurnitureCatalogueResourceName`, and the `mNextFurnitureId`
	// allocation - stays with the World (ADR 0001). WorldSerialization.cpp
	// and construction-record replay are untouched: behaviour moved out,
	// state did not.
	//
	// Furniture *use* - the Agent-side Action transaction, finishing, and
	// pose restoration - stays with World's action machinery; the pose-fit
	// and use-eligibility checks that read live Agent capabilities remain
	// there too. This store manages the instances, the catalogue
	// reference, and the resolution the use side reads.
	//
	// World keeps ownership of every entity registry (ADR 0001) and remains
	// the facade (design pattern) through which callers reach these
	// operations; no caller outside World names this class. The store owns
	// no state: it reaches the World's registries, the pause gate, the
	// construction records, and the coordinator through the World it was
	// given, following the SimulationCoordinator, PlacementValidator,
	// AgentGroupRegistry, PermissionRequirementStore, AgentTypeCatalog,
	// MarkerStore, TraversalResourceRegistry, and DeviceConditionStore
	// pattern.
	class FurnitureStore
	{
	public:

		explicit FurnitureStore(World& world);

		FurnitureStore(FurnitureStore const&) = delete;
		FurnitureStore& operator=(FurnitureStore const&) = delete;

		virtual ~FurnitureStore() = default;

		// ------------------------------------------------------------------
		// Catalogue reference
		// ------------------------------------------------------------------

		// Install a catalogue under its Resource name. A non-empty instance
		// list pins the catalogue: replacement is refused while any instance
		// uses it.
		void attachFurnitureCatalogue(std::string resourceName,
			std::shared_ptr<const FurnitureCatalogue> catalogue);

		// Paused reload of the installed package. Every lifecycle is
		// reconciled - all current users finish against the old package
		// before the replacement is installed, and a layout change replays
		// the construction records against it, retaining request
		// identities. Any refusal leaves the old catalogue installed.
		bool reloadFurnitureCatalogue(std::filesystem::path const& path,
			std::string* diagnostic = nullptr);

		// ------------------------------------------------------------------
		// Instance management
		// ------------------------------------------------------------------

		// True when the Marker is owned by some instance's destination.
		bool isFurnitureMarker(MarkerId id) const;

		// Judged without touching anything: catalogue, definition, sector
		// kind, floor alignment, support, overlap, names, identity space,
		// and the pause gate.
		bool canPlaceFurniture(uint32_t sectorIndex, std::string const& key,
			float x, float y, std::string const& name, std::string* diagnostic = nullptr,
			int localDepth = 0) const;
		uint64_t placeFurniture(uint32_t sector, std::string const& key,
			float x, float y, std::string const& name, int localDepth = 0);

		// Edit and removal run as candidate-replay transactions: the
		// amended record list must rebuild cleanly (with replay pose
		// validation) before the live World is rebuilt from it.
		bool canEditFurniture(uint64_t id, float x, float y,
			std::string const& name, std::string* diagnostic = nullptr,
			std::optional<int> localDepth = std::nullopt) const;
		bool editFurniture(uint64_t id, float x, float y,
			std::string const& name, std::string* diagnostic = nullptr,
			std::optional<int> localDepth = std::nullopt);
		bool canRemoveFurniture(uint64_t id, std::string* diagnostic = nullptr) const;
		bool removeFurniture(uint64_t id, std::string* diagnostic = nullptr);

		// The refusal text (empty when nothing rests there) naming the
		// instances a floor removal at this cell would leave unsupported.
		std::string furnitureSupportDiagnostic(uint32_t sector, uint32_t x,
			uint32_t y) const;

		// Construction-record restoration: validates the saved destination
		// set against the catalogue's usable points, restores the owned
		// Markers, and appends the instance. Throws on any mismatch.
		void restoreFurniture(World::ConstructionRecord const& record);

		// ------------------------------------------------------------------
		// Usable-point resolution
		// ------------------------------------------------------------------

		// One pass over the instances resolves the whole Marker ->
		// instance, definition, destination, usable-point support and
		// Sector chain (#627). The result borrows from the current World
		// and catalogue and lives only inside the calling evaluation. An
		// explicit catalogue serves live finishing, which keeps the
		// catalogue its use began with even while a replacement is being
		// installed; absent means the currently installed catalogue.
		World::ResolvedUsablePoint resolveUsablePoint(MarkerId marker,
			std::shared_ptr<const FurnitureCatalogue> const& catalogue = nullptr) const;

		// The instance owning this destination Marker, or nullptr.
		FurnitureInstance const* furnitureForMarker(MarkerId marker) const;

	private:

		// The store owns nothing: it reads and edits the World's Furniture
		// state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
