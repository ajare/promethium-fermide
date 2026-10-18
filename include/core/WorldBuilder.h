#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/Window.h"
#include "core/World.h"

namespace core
{
	// Owns the SectorObject authoring behaviour on behalf of the World
	// that owns it (ADR 0022).
	//
	// This is the first leaf of the WorldBuilder seam: the SectorObject
	// family - Walkway, ForceBridge, Window, BoothWindow, and LightSwitch
	// authoring. Each add is judged before anything is written, so a
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

		explicit WorldBuilder(World& world);

		WorldBuilder(WorldBuilder const&) = delete;
		WorldBuilder& operator=(WorldBuilder const&) = delete;

		virtual ~WorldBuilder() = default;

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
