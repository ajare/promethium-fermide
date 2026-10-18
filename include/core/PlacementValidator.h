#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "core/SectorObjectType.h"
#include "core/World.h"


namespace core
{
	class Graph;

	// Validates placement and authoring requests on behalf of the World that
	// owns it (ADR 0022).
	//
	// This is the read-only validation family moved out of World: bounds and
	// Layer checks, cell occupancy/traversability, Sector object admission,
	// per-resource creation options, staircase endpoints, traversal-topology
	// consistency, and Agent location placement. Every check is read-only over
	// the World's sectors, Layers, and graph; it throws (or reports via a
	// diagnostic out-parameter) and never mutates state.
	//
	// World keeps ownership of every entity registry (ADR 0001) and remains
	// the facade (design pattern) through which callers reach these checks;
	// no caller outside World names the validator. The validator owns no
	// state: it reaches the World's registries and helper seams through the
	// World it was given, and calls back through the World facade for
	// machinery which has not moved out of World yet (access panels,
	// physical-control planning, Agent location permissions).
	//
	// This is the first proof-of-pattern leaf of the ADR 0022 decomposition,
	// following the SimulationCoordinator pattern: friend access to World,
	// World-owned instance, World forwards each entry point.
	class PlacementValidator
	{
	public:

		// The option types are World's authored creation options; the validator
		// checks them without owning them.
		using CreateDoorOptions = World::CreateDoorOptions;
		using CreateForceBridgeOptions = World::CreateForceBridgeOptions;
		using CreateLadderOptions = World::CreateLadderOptions;
		using CreateLiftOptions = World::CreateLiftOptions;
		using CreateShuttleOptions = World::CreateShuttleOptions;

		explicit PlacementValidator(World& world);

		PlacementValidator(PlacementValidator const&) = delete;
		PlacementValidator& operator=(PlacementValidator const&) = delete;

		virtual ~PlacementValidator() = default;

		// ------------------------------------------------------------------
		// Cell and Layer checks
		// ------------------------------------------------------------------

		void validateCellOccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellUnoccupied(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellIsInSector(std::string const& caller, uint32_t x, uint32_t y, std::shared_ptr<const Sector> sector) const;

		void validateCellHasNoObject(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellHasNoDoor(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateCellTraversableOnFoot(std::string const& caller, std::string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const;

		void validateLayer(std::string const& caller, uint32_t layerIndex) const;

		void validateBounds(std::string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		void validateLayerSpace(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const;

		// ------------------------------------------------------------------
		// Sector object admission
		// ------------------------------------------------------------------

		void validateObjectAllowedInSector(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		void validateObjectAllowedInSectorAsLookTarget(std::string const& caller, SectorObjectType type, uint32_t sectorIndex) const;

		// The span must fall inside a single Sector, because crossing a Sector
		// boundary breaks the traversal geometry the caller is about to build.
		// The one relaxation is allowAllBackgroundSpan, used for the Layer
		// behind a Window: there a span of nothing but Backgrounds may cover
		// several Background Sectors, since a Background takes no part in
		// traversal and what lies behind an aperture is read from the cell
		// grid. A span that mixes a Background with any other Sector is still
		// refused.
		void validateSpaceOnlyInOneSector(std::string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, bool allowAllBackgroundSpan = false) const;

		// ------------------------------------------------------------------
		// Per-resource creation option checks
		// ------------------------------------------------------------------

		void validateSectorDoorOptions(std::string const& caller, CreateDoorOptions const& options) const;

		void validateSectorDoorPlacement(std::string const& caller, uint32_t layerIndex, uint32_t y,
			uint32_t x, CreateDoorOptions const& options, bool controlsAreExternallyBound) const;

		void validateSectorForceBridgeOptions(std::string const& caller, CreateForceBridgeOptions const& options) const;

		void validateSectorLadderOptions(std::string const& caller, CreateLadderOptions const& options) const;

		void validateLiftOptions(std::string const& caller, CreateLiftOptions const& options) const;

		void validateShuttleOptions(std::string const& caller, CreateShuttleOptions const& options) const;

		// ------------------------------------------------------------------
		// Staircase, topology, and Agent placement checks
		// ------------------------------------------------------------------

		bool validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
			int riseSide, std::string& diagnostic) const;

		void validateTraversalTopology(Graph const& graph) const;

		void validateAgentLocationPlacement(Sector const& sector, Agent const& agent,
			std::optional<float> floorY = std::nullopt) const;

		// ------------------------------------------------------------------
		// World dimension support
		// ------------------------------------------------------------------

		// A World allocates one CellDefinition per (x, level) on each Layer, so
		// dimensions are accepted only when the total across `layerCount`
		// Layers stays within CORE_MAX_WORLD_CELLS. The product is evaluated in
		// 64 bits so an overflowing pair is refused rather than wrapping to a
		// short allocation (#184). Returns false and fills `diagnostic` when
		// the size is unsupported; the editor and loader use the same rule as
		// the World constructor.
		static bool dimensionsAreSupported(uint32_t cellsWide, uint32_t levelsHigh,
			uint32_t layerCount, std::string* diagnostic = nullptr);

	private:

		// The validator owns nothing: it reads the World's registries and
		// helper seams through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
