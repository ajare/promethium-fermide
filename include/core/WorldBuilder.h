#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Window.h"
#include "core/World.h"

namespace core
{
	// Owns the Sector and SectorObject authoring behaviour on behalf of
	// the World that owns it (ADR 0022).
	//
	// This is the WorldBuilder seam: the Sector family - Room, Corridor,
	// Facade, and Background authoring, the Door family - ordinary Door
	// and Bulkhead Door authoring with their Buttons, plus the
	// SectorObject family - Walkway, ForceBridge, Window, BoothWindow,
	// and LightSwitch authoring. Each add is judged before anything is written, so a
	// refusal leaves the World exactly as it was found; each accepted add
	// records its construction record so replay reproduces it verbatim.
	// The Walkway add carries its detached-replay path: once the document
	// is built, the column is rebuilt from records so every Ladder in it
	// can shorten to the newly nearest Walkway.
	//
	// The builder owns no state: the Sectors, Layers, cells, BoothWindow
	// registry, interaction points, and construction records stay
	// World-owned (ADR 0001); the builder reaches them, the pause and
	// structural-edit gates, and the physical-control machinery through
	// the World it was given, following the SimulationCoordinator,
	// PlacementValidator, MarkerStore, and TraversalResourceRegistry
	// pattern. Placement validation goes through the World's validator
	// seams, traversal-resource creation through the traversal resource
	// registry, and Marker placement validation through the marker store
	// (#672, #675, #679).
	//
	// WorldSerialization.cpp and construction-record replay are
	// untouched - behaviour moved out, state did not. World keeps
	// ownership of every entity registry (ADR 0001) and remains the
	// facade (design pattern) through which callers reach these
	// operations; no caller outside World names this class.
	class WorldBuilder
	{
	public:

		// The option and result types are World's authored creation types;
		// the builder produces them without owning them.
		using CreateObjectResult = World::CreateObjectResult;
		using CreateWindowOptions = World::CreateWindowOptions;
		using CreateWindowResult = World::CreateWindowResult;
		using CreateForceBridgeOptions = World::CreateForceBridgeOptions;
		using CreateForceBridgeResult = World::CreateForceBridgeResult;
		using CreateDoorOptions = World::CreateDoorOptions;
		using CreateDoorResult = World::CreateDoorResult;
		using CreateBulkheadDoorOptions = World::CreateBulkheadDoorOptions;
		using CreateBulkheadDoorResult = World::CreateBulkheadDoorResult;

		explicit WorldBuilder(World& world);

		WorldBuilder(WorldBuilder const&) = delete;
		WorldBuilder& operator=(WorldBuilder const&) = delete;

		virtual ~WorldBuilder() = default;

		// ------------------------------------------------------------------
		// Sectors: Room / Corridor / Facade / Background
		// ------------------------------------------------------------------

		// The raw Location creation seam: appends the Sector to the World's
		// registry. The cells are stamped by addLocation(); nothing here
		// validates - callers come through the judged add family.
		uint32_t createLocation(std::string const& name, SectorType type,
			uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			uint32_t levelsHigh, float topLevelHeight, bool isCorridor);

		// The validated Location add: bounds and layer-space are judged
		// before anything is written; on acceptance the Location is created
		// and its cells stamped (ground floor on the bottom level).
		uint32_t addLocation(std::string const& name, SectorType type,
			uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide,
			uint32_t levelsHigh, float topLevelHeight, bool isCorridor);

		// A Corridor is a Location, so it may sit on any Layer. The
		// Layer-less form keeps the front-most Layer as its default.
		uint32_t addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide,
			uint32_t levelsHigh);
		uint32_t addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh);

		uint32_t addRoom(std::string const& name, uint32_t layerIndex,
			uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh,
			float topLevelHeight);

		// A Background is a non-occupiable Sector: it takes space on its
		// own Layer and nothing else. It may sit on any Layer, front-most
		// and back-most included (ADR 0002).
		bool canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh,
			std::string* diagnostic = nullptr) const;
		uint32_t addBackground(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh,
			BackgroundColour const& colour);

		// A Facade is an occupiable Location with its perimeter walls all
		// open by construction; placement follows the Room rule (ADR 0003).
		// The named form is the persistence form.
		bool canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight,
			std::string* diagnostic = nullptr) const;
		uint32_t addFacade(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight,
			BackgroundColour const& colour);
		uint32_t addFacade(std::string const& name, uint32_t layerIndex,
			uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh,
			float topLevelHeight, BackgroundColour const& colour);

		// ------------------------------------------------------------------
		// Low-level creation seams
		// ------------------------------------------------------------------

		// Construct the object in its cell(s). These are the seams the
		// validated add family and the World's own builders (Dumbwaiter)
		// use; callers outside World go through the add family.
		CreateObjectResult createWindow(uint32_t layerIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier = nullptr,
			bool boothWindow = false);

		CreateObjectResult createWalkway(uint32_t layerIndex, uint32_t x, uint32_t y,
			uint32_t* vertexIdentifier = nullptr);

		CreateObjectResult createForceBridge(uint32_t layerIndex, uint32_t x, uint32_t y,
			CreateForceBridgeOptions const& options);

		// ------------------------------------------------------------------
		// Window and BoothWindow
		// ------------------------------------------------------------------

		// A Window needs the Layer directly behind the Layer it is authored
		// on, so the back-most Layer can never take a new one.
		bool canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide = 1, uint32_t levelsHigh = 1,
			std::string* diagnostic = nullptr) const;

		uint32_t addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh);

		CreateWindowResult addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
			uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options);

		// The shared aperture path for Windows and BoothWindows: judged by the
		// matching canAdd*, then built, wired, and recorded.
		CreateWindowResult addWindowAperture(uint32_t layer, uint32_t y, uint32_t x,
			uint32_t width, uint32_t height, CreateWindowOptions const& options,
			bool boothWindow);

		bool canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
			uint32_t width = 1, uint32_t height = 1, std::string* diagnostic = nullptr) const;

		CreateWindowResult addBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
			Window::State state);

		// ------------------------------------------------------------------
		// Ordinary Doors
		// ------------------------------------------------------------------

		// A Door is authored on the front Layer of its pair and opens into
		// the Layer directly behind it. This is the raw creation seam the
		// judged add family and the World's transport builders use.
		CreateObjectResult createDoor(uint32_t layerIndex, uint32_t x, uint32_t y,
			uint32_t cellsWide, Door::Height height = Door::Height::Regular,
			uint32_t* vertexIdentifier = nullptr);

		// The complete Door build: placement preflight, threshold creation,
		// traversal resource, queue lanes, and optional per-side Buttons.
		// The lift and shuttle builders call it with
		// controlsAreExternallyBound so their own binding path owns the
		// Button InteractionPoints.
		CreateDoorResult _addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			CreateDoorOptions const& options, bool controlsAreExternallyBound = false);

		// The judged ordinary Door add: every rejecting check runs before
		// any mutation so a refusal leaves the World exactly as found.
		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			std::string* diagnostic = nullptr) const;
		bool canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			CreateDoorOptions const& options, std::string* diagnostic = nullptr) const;

		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x);
		CreateDoorResult addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			CreateDoorOptions const& options);

		// Gives the selected Door physical open controls on both sides of
		// the threshold; the missing side(s) gain one, a side that lacks a
		// Button but has no space for one refuses before anything changes.
		void addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex);
		bool canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;

		// Removes every Door Button from the selected Door and returns the
		// rebuilt Door object; the pre-Button activation mode recorded in
		// the Door's construction record is restored.
		bool canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const;
		std::shared_ptr<const DoorSectorObject> removeSectorDoorButton(
			uint32_t sectorIndex, uint32_t objectIndex,
			std::optional<std::vector<AccessPermissionId>> resultingRequirement = std::nullopt);

		// ------------------------------------------------------------------
		// Bulkhead Doors
		// ------------------------------------------------------------------

		// A Bulkhead Door sits in the boundary between two adjacent
		// Locations on one Layer; this is the raw creation seam.
		CreateObjectResult createBulkheadDoor(uint32_t layerIndex, uint32_t x,
			uint32_t y, int side);

		bool canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
			int side, CreateBulkheadDoorOptions const& options,
			std::string* diagnostic = nullptr) const;
		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex,
			uint32_t y, uint32_t x, int side);
		CreateBulkheadDoorResult addSectorBulkheadDoor(uint32_t layerIndex,
			uint32_t y, uint32_t x, int side, CreateBulkheadDoorOptions const& options);

		// ------------------------------------------------------------------
		// Physical control button seams
		// ------------------------------------------------------------------

		// The unjudged button creation seams shared by the add families
		// above and the World's own builders (light switch, transports).
		CreateObjectResult _createSectorButton(std::string const& name,
			std::shared_ptr<const Sector> sector, uint32_t x, uint32_t y,
			uint32_t flags, uint32_t* index = nullptr);
		CreateObjectResult _createDoorButton(std::shared_ptr<const Sector> sector,
			uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags,
			uint32_t* index = nullptr, uint32_t approachSide = 0);
		CreateObjectResult _createBulkheadDoorButton(std::shared_ptr<const Sector> sector,
			uint32_t y, int side, uint32_t* index = nullptr);

		// ------------------------------------------------------------------
		// Light switch
		// ------------------------------------------------------------------

		CreateObjectResult addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset);

		// ------------------------------------------------------------------
		// Walkway
		// ------------------------------------------------------------------

		bool canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset, std::string* diagnostic = nullptr) const;

		CreateObjectResult addSectorWalkway(uint32_t sectorIndex,
			uint32_t levelIndex, uint32_t xOffset);

		// ------------------------------------------------------------------
		// Force bridge
		// ------------------------------------------------------------------

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex,
			uint32_t levelIndex, uint32_t xOffset);

		CreateForceBridgeResult addSectorForceBridge(uint32_t sectorIndex,
			uint32_t levelIndex, uint32_t xOffset, CreateForceBridgeOptions const& options);

		bool canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
			uint32_t xOffset, CreateForceBridgeOptions const& options,
			std::string* diagnostic = nullptr) const;

	private:

		// The extension control button for a Force Bridge add; placed from
		// the bridge's canonical physical-control demand.
		CreateObjectResult createForceBridgeButton(std::shared_ptr<const Sector> sector,
			uint32_t x, uint32_t y, uint32_t cellsWide, int side, uint32_t flags,
			uint32_t* index = nullptr);

		// The builder owns nothing: it reads and edits the World's Sector,
		// Layer, and registry state through this reference (ADR 0001/0022).
		World& mWorld;
	};
}
