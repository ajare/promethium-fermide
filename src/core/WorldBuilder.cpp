#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/WorldBuilder.h"

#include "core/Background.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Button.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Facade.h"
#include "core/Layer.h"
#include "core/LiftTransit.h"
#include "core/Location.h"
#include "core/PhysicalControlPlacement.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/CellDefinition.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/ForceBridge.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	WorldBuilder::WorldBuilder(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Sectors: Room / Corridor / Facade / Background
	// ----------------------------------------------------------------------

	uint32_t WorldBuilder::createLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor)
	{
		mWorld.invalidateSimulationSnapshot();
		auto sectorIndex = (uint32_t)mWorld.mSectors.size();

		auto location = make_shared<Location>(name, type, layerIndex, sectorIndex, x, y, cellsWide, levelsHigh, topLevelHeight, ~0u, isCorridor);

		mWorld.mSectors.push_back(location);
		return sectorIndex;
	}

	uint32_t WorldBuilder::addLocation(string const& name, SectorType type, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, bool isCorridor)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::addLocation({}, {}, {}, {}, {}, {}, {} {})", name, getSectorTypeString(type), layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight);

		mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);
		mWorld.validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);

		// Create sector
		auto sectorIndex = createLocation(name, type, layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight, isCorridor);

		// Set layers
		auto layer = mWorld.getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorIndex = sectorIndex;
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		return sectorIndex;
	}

	uint32_t WorldBuilder::addCorridor(uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh)
	{
		mWorld.invalidateSimulationSnapshot();
		return addCorridor(0, y, x, cellsWide, levelsHigh);
	}

	uint32_t WorldBuilder::addCorridor(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh)
	{
		mWorld.invalidateSimulationSnapshot();
		string const caller = format("World::addCorridor({}, {}, {}, {}, {})", layerIndex, y, x, cellsWide, levelsHigh);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		mWorld.validateLayer(caller, layerIndex);
		// Minimum (1,1). A zero-sized Location would pass the bounds checks by covering
		// nothing, leaving a Sector no cell references: invisible, unselectable, and
		// unreachable forever (ticket #64).
		if (cellsWide == 0)
			throw WorldException(&mWorld, format("{} - a Corridor must be at least one cell wide", caller));
		if (levelsHigh == 0)
			throw WorldException(&mWorld, format("{} - a Corridor must be at least one level high", caller));
		mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);
		mWorld.validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, true);
		mWorld.beginStructuralEdit("addCorridor");
		auto const result = addLocation("Corridor", SectorType::Location, layerIndex, x, y, cellsWide, levelsHigh, CORE_CORRIDOR_HEIGHT, true);
		mWorld.reflowAllPhysicalControls();
		World::ConstructionRecord record{ World::ConstructionType::Corridor };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		mWorld.recordConstruction(std::move(record));
		return result;
	}

	uint32_t WorldBuilder::addRoom(string const& name, uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight)
	{
		mWorld.invalidateSimulationSnapshot();
		string const caller = format("World::addRoom({}, {}, {}, {}, {}, {}, {})", name, layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight);
		// Every rejecting check runs before beginStructuralEdit() so a refused
		// call stays a true no-op: no modified flag, no topology invalidation
		// (ticket #93).
		// Written as a negated in-range test so a NaN topLevelHeight is rejected
		// too: NaN fails both comparisons, so the plain < / > pair would let it
		// through (ticket #55).
		if (!(topLevelHeight >= CORE_ROOM_MIN_HEIGHT && topLevelHeight <= CORE_ROOM_MAX_HEIGHT))
		{
			throw WorldException(&mWorld, format("{} - topLevelHeight={} is out of range", caller, topLevelHeight));
		}
		// Minimum (1,1), the same invariant Background and Facade enforce: a
		// zero-sized Room would pass the bounds checks by covering nothing,
		// leaving a Sector no cell references (ticket #64).
		if (cellsWide == 0)
			throw WorldException(&mWorld, format("{} - a Room must be at least one cell wide", caller));
		if (levelsHigh == 0)
			throw WorldException(&mWorld, format("{} - a Room must be at least one level high", caller));

		mWorld.validateLayer(caller, layerIndex);
		mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);
		mWorld.validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, true);
		mWorld.beginStructuralEdit("addRoom");

		auto const result = addLocation(name, SectorType::Location, layerIndex, x, y, cellsWide, levelsHigh, topLevelHeight, false);
		mWorld.reflowAllPhysicalControls();
		World::ConstructionRecord record{ World::ConstructionType::Room };
		record.name = name;
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = levelsHigh;
		record.x = topLevelHeight;
		mWorld.recordConstruction(std::move(record));
		return result;
	}

	bool WorldBuilder::canAddBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("World::addBackground({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, levelsHigh);

		try
		{
			mWorld.validateLayer(caller, layerIndex);

			// Minimum (1,1). A zero-sized block would pass the bounds checks by covering
			// nothing, which is not a Background worth authoring.
			if (cellsWide == 0)
				throw WorldException(&mWorld, format("{} - a Background must be at least one cell wide", caller));
			if (levelsHigh == 0)
				throw WorldException(&mWorld, format("{} - a Background must be at least one level high", caller));

			mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);

			// Every cell must be unoccupied on the Background's own Layer, which is the
			// same rule a Location plays by. Any Layer is legal, front-most and back-most
			// included: a "back layers only" rule would re-introduce exactly the
			// Fore/Back special-casing that ADR 0002 removed.
			mWorld.validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t WorldBuilder::addBackground(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, BackgroundColour const& colour)
	{
		mWorld.invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddBackground(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic))
			throw WorldException(&mWorld, diagnostic);

		mWorld.validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, false);
		mWorld.beginStructuralEdit("addBackground");

		auto sectorIndex = (uint32_t)mWorld.mSectors.size();
		mWorld.mSectors.push_back(make_shared<Background>("Background", layerIndex, sectorIndex,
			x, y, cellsWide, levelsHigh, colour));

		auto layer = mWorld.getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Background owns no walkable floor and hosts no object, so its cells
				// carry no floor and no SectorObject.
				cellDef.floorType = Background::cellFloorType();
				cellDef.sectorObjectType = SectorObjectType::None;
			}
		}

		mWorld.reflowAllPhysicalControls();
		World::ConstructionRecord record{ World::ConstructionType::Background };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		// The whole Background appearance fits one integer, so no new record field is
		// needed to persist its colour.
		record.f = packBackgroundColour(colour);
		mWorld.recordConstruction(std::move(record));

		return sectorIndex;
	}

	bool WorldBuilder::canAddFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();

		string caller = format("World::addFacade({}, {}, {}, {}, {})",
			layerIndex, y, x, cellsWide, levelsHigh);

		try
		{
			mWorld.validateLayer(caller, layerIndex);

			// A Facade is placed exactly as a Room is: minimum (1,1), inside the
			// World bounds, and on cells unoccupied on its own Layer.
			if (cellsWide == 0)
				throw WorldException(&mWorld, format("{} - a Facade must be at least one cell wide", caller));
			if (levelsHigh == 0)
				throw WorldException(&mWorld, format("{} - a Facade must be at least one level high", caller));
			// Same negated in-range test as addRoom: a NaN topLevelHeight must
			// not sail through the < / > pair (ticket #55).
			if (!(topLevelHeight >= CORE_ROOM_MIN_HEIGHT && topLevelHeight <= CORE_ROOM_MAX_HEIGHT))
				throw WorldException(&mWorld, format("{} - topLevelHeight={} is out of range", caller, topLevelHeight));

			mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);
			mWorld.validateLayerSpace(caller, layerIndex, x, y, cellsWide, levelsHigh);
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		return true;
	}

	uint32_t WorldBuilder::addFacade(uint32_t layerIndex, uint32_t y, uint32_t x, uint32_t cellsWide,
		uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		mWorld.invalidateSimulationSnapshot();
		return addFacade(Facade::defaultName(), layerIndex, y, x, cellsWide, levelsHigh,
			topLevelHeight, colour);
	}

	uint32_t WorldBuilder::addFacade(std::string const& name, uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, BackgroundColour const& colour)
	{
		mWorld.invalidateSimulationSnapshot();
		string diagnostic;
		if (!canAddFacade(layerIndex, y, x, cellsWide, levelsHigh, topLevelHeight, &diagnostic))
			throw WorldException(&mWorld, diagnostic);

		mWorld.validatePhysicalControlSectorCreation(layerIndex, x, y, cellsWide, levelsHigh, false);
		mWorld.beginStructuralEdit("addFacade");

		auto sectorIndex = (uint32_t)mWorld.mSectors.size();
		mWorld.mSectors.push_back(make_shared<Facade>(name, layerIndex, sectorIndex,
			x, y, cellsWide, levelsHigh, topLevelHeight, colour));

		auto layer = mWorld.getLayer(layerIndex);
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);
				cellDef.sectorIndex = sectorIndex;
				// A Facade owns walkable floor exactly as a Location does: ground
				// on the bottom level, upper levels reached by Walkways.
				cellDef.floorType = iy == y ? CellFloorType::Ground : CellFloorType::None;
			}
		}

		mWorld.reflowAllPhysicalControls();
		World::ConstructionRecord record{ World::ConstructionType::Facade };
		record.name = name;
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = cellsWide; record.d = levelsHigh;
		record.x = topLevelHeight;
		// The Facade reuses Background's colour packing, so the editor and the
		// headless checks round-trip the same arithmetic (ADR 0003).
		record.f = packBackgroundColour(colour);
		mWorld.recordConstruction(std::move(record));

		return sectorIndex;
	}

	// ----------------------------------------------------------------------
	// Low-level creation seams
	// ----------------------------------------------------------------------

	World::CreateObjectResult WorldBuilder::createWindow(uint32_t layerIndex, uint32_t x,
		uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, uint32_t* vertexIdentifier,
		bool boothWindow)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::createWindow({}, {}, {}, {}, {})", layerIndex, x, y, cellsWide, levelsHigh);

		// A Window is authored on the front Layer of the pair it crosses.  The Layer
		// behind it is only absent when a map written before the back-most Layer rule
		// is replayed: authoring such a Window is refused by canAddSectorWindow(), and a
		// Layer deletion removes one it would strand there.
		bool const hasBack = layerIndex + 1 < mWorld.getLayerCount();

		mWorld.validateCellOccupied(caller, layerIndex, x, y);
		if (hasBack)
			mWorld.validateCellOccupied(caller, layerBehind(layerIndex), x, y);

		// Find sectors that the Window is connecting, if any.
		shared_ptr<Sector> foreSector{ nullptr };
		shared_ptr<Sector> backSector{ nullptr };

		foreSector = mWorld._getSector(mWorld.mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		if (hasBack)
		{
			auto const& cellDef1 = mWorld.mLayers[layerBehind(layerIndex)]->getCellDefinition(x, y);

			// The back Sector is the Sector behind the Window's first cell.  A Window
			// may span several Backgrounds (#36), so for such a Window this is the
			// first Background in the span and is non-authoritative; the cell grid
			// holds the truth about what the whole aperture looks into.
			backSector = cellDef1.sectorIndex != ~0u ? mWorld._getSector(cellDef1.sectorIndex) : nullptr;
		}

		// Create window in fore Location and add to back
		auto windowIndex = foreSector->createWindow(foreSector, backSector, x, y, cellsWide, levelsHigh, vertexIdentifier, boothWindow);

		if (backSector)
		{
			backSector->addWindow(dynamic_pointer_cast<WindowSectorObject>(foreSector->_getObject(windowIndex)));
		}

		return {
			windowIndex,
			boothWindow ? SectorObjectType::BoothWindow : SectorObjectType::Window,
			foreSector
		};
	}

	World::CreateObjectResult WorldBuilder::createWalkway(uint32_t layerIndex, uint32_t x,
		uint32_t y, uint32_t* vertexIdentifier)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::createWalkway({}, {}, {})", layerIndex, x, y);

		mWorld.validateCellOccupied(caller, layerIndex, x, y);

		// Create Walkway
		auto layer = mWorld.getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = mWorld._getSector(cellDef.sectorIndex);

		return {
			sector->createWalkway(sector, x, y, vertexIdentifier),
			SectorObjectType::Walkway,
			sector
		};
	}

	World::CreateObjectResult WorldBuilder::createForceBridge(uint32_t layerIndex, uint32_t x,
		uint32_t y, CreateForceBridgeOptions const& options)
	{
		mWorld.invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(options.fromSide);

		string caller = format("World::createForceBridge({}, {}, {}, {}, {}, {})", layerIndex, x, y, options.width, options.fromSide, options.startExtended);

		mWorld.validateCellOccupied(caller, layerIndex, x, y);

		// Create ForceBridge
		auto layer = mWorld.getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = mWorld._getSector(cellDef.sectorIndex);

		return {
			sector->createForceBridge(sector, x, y, options.width, options.fromSide, options.extensible, options.startExtended),
			SectorObjectType::ForceBridge,
			sector
		};
	}

	World::CreateObjectResult WorldBuilder::createForceBridgeButton(
		shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide,
		int side, uint32_t flags, uint32_t* index)
	{
		mWorld.invalidateSimulationSnapshot();
		auto demand = mWorld.insetControlDemand(sector, physicalControl::OwnerType::ForceBridge,
			{ sector->getLayerIndex(), x, y, cellsWide, 1 }, y, side);
		auto obj = mWorld.createPhysicalControl("ForceBridge button", sector->getLayerIndex(), y, demand, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	// ----------------------------------------------------------------------
	// Window and BoothWindow
	// ----------------------------------------------------------------------

	bool WorldBuilder::canAddSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh, string* diagnostic) const
	{
		string caller = format("World::addSectorWindow({}, {}, {}, {})", layerIndex, y, x, cellsWide);
		try
		{
			mWorld.validateLayer(caller, layerIndex);
			mWorld.validateBounds(caller, x, y, cellsWide, levelsHigh);

			// A Window joins its own Layer to the Layer directly behind it.  The back-most
			// Layer has nothing behind it, so a Window there would cross no threshold and
			// could never take part in the Graph.  Maps written before the rule can still
			// replay one from that Layer - see createWindow() - but none may be authored.
			if (!mWorld.mDeserializingConstruction && isBackMostLayer(layerIndex, mWorld.getLayerCount()))
				throw WorldException(&mWorld, format(
					"{} - a Window needs a Layer behind it, and Layer {} is the back-most Layer",
					caller, layerIndex));

			vector<uint32_t> requiredLayers{ layerIndex };
			if (layerIndex + 1 < mWorld.getLayerCount()) requiredLayers.push_back(layerBehind(layerIndex));

			// The front Layer of the pair keeps the strict one-Sector rule: a Window
			// whose own wall straddles two Sectors has no coherent frame.  The Layer
			// behind may span several Backgrounds, but may not mix one with a Room.
			for (auto requiredLayer : requiredLayers)
				mWorld.validateSpaceOnlyInOneSector(caller, requiredLayer, x, y, cellsWide, levelsHigh,
					requiredLayer != layerIndex);

			for (auto requiredLayer : requiredLayers)
			{
				auto layer = mWorld.getLayer(requiredLayer);
				// A back-side Window may span several Backgrounds/Locations.
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto owner = layer->getCellDefinition(ix, iy).sectorIndex;
						if (owner != ~0u) mWorld.validatePanelWallRectangle(owner,
							{float(x) + CORE_WINDOW_X_INSET, float(y) + CORE_WINDOW_Y_OFFSET},
							{float(x + cellsWide) - CORE_WINDOW_X_INSET,
								float(y + levelsHigh - 1) + CORE_WINDOW_Y_OFFSET + CORE_WINDOW_HEIGHT});
					}
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& cellDef = layer->getCellDefinition(ix, iy);
						if (cellDef.sectorIndex == ~0u)
							throw WorldException(&mWorld, format("{} - Layer {} cell at {},{} is not occupied.",
								caller, requiredLayer, ix, iy));
						if (requiredLayer == layerIndex)
							mWorld.validateObjectAllowedInSector(caller, SectorObjectType::Window, cellDef.sectorIndex);
						else
							mWorld.validateObjectAllowedInSectorAsLookTarget(caller, SectorObjectType::Window, cellDef.sectorIndex);
						if (requiredLayer == layerIndex
							&& (cellDef.hasObject() || !cellDef.markers.empty()))
							throw WorldException(&mWorld, format("{} - another object occupies cell at {},{}",
								caller, ix, iy));
					}
			}
		}
		catch (Exception const& error)
		{
			if (diagnostic) *diagnostic = error.getMessage();
			return false;
		}
		catch (exception const& error)
		{
			if (diagnostic) *diagnostic = error.what();
			return false;
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint32_t WorldBuilder::addSectorWindow(uint32_t layerIndex, uint32_t y, uint32_t x,
		uint32_t cellsWide, uint32_t levelsHigh)
	{
		mWorld.invalidateSimulationSnapshot();
		return addSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, {}).window.index;
	}

	World::CreateWindowResult WorldBuilder::addSectorWindow(uint32_t layerIndex, uint32_t y,
		uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options)
	{
		return addWindowAperture(layerIndex, y, x, cellsWide, levelsHigh, options, false);
	}

	World::CreateWindowResult WorldBuilder::addWindowAperture(uint32_t layerIndex, uint32_t y,
		uint32_t x, uint32_t cellsWide, uint32_t levelsHigh, CreateWindowOptions const& options,
		bool boothWindow)
	{
		string diagnostic;
		if (boothWindow && (options.traversable || options.style != Window::Style::Clear
			|| (options.initialState != Window::State::Open && options.initialState != Window::State::Closed)))
			throw WorldException(&mWorld, "BoothWindow supports only non-traversable Open or Closed shutters");
		if (!(boothWindow ? canAddBoothWindow(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic)
			: canAddSectorWindow(layerIndex, y, x, cellsWide, levelsHigh, &diagnostic)))
			throw WorldException(&mWorld, diagnostic);
		if (options.traversable)
		{
			// A Window may look into a Background, but a traversable Window would admit
			// Agents into the Sector behind it, and a Background is seen through, never
			// entered: it owns no walkable floor and takes no part in traversal.
			if (layerIndex + 1 < mWorld.getLayerCount())
			{
				auto const backLayer = layerBehind(layerIndex);
				for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
					for (uint32_t ix = x; ix < x + cellsWide; ++ix)
					{
						auto const& backCell = mWorld.mLayers[backLayer]->getCellDefinition(ix, iy);
						if (backCell.sectorIndex != ~0u
							&& mWorld.mSectors[backCell.sectorIndex]->getType() == SectorType::Background)
							throw WorldException(&mWorld, format(
								"A traversable Window cannot cross into the Background at {},{}: a Background can be looked into, but never entered",
								ix, iy));
					}
			}
		}
		mWorld.beginStructuralEdit("addSectorWindow");
		auto layer = mWorld.getLayer(layerIndex);

		// Create window
		auto createdWindow = createWindow(layerIndex, x, y, cellsWide, levelsHigh, nullptr, boothWindow);
		auto windowIndex = createdWindow.index;
		auto windowObjType = createdWindow.type;
		auto windowSector = createdWindow.sector;
		auto windowObject = dynamic_pointer_cast<WindowSectorObject>(windowSector->_getObject(windowIndex));
		auto window = windowObject->getWindow();
		window->setState(options.initialState, options.style);
		if (boothWindow)
		{
			auto booth = static_pointer_cast<BoothWindow>(window);
			if (mWorld.mNextBoothWindowId == 0) throw overflow_error("BoothWindow device identity space is exhausted");
			booth->mDeviceId = BoothWindowId{ mWorld.mNextBoothWindowId++ };
			mWorld.mBoothWindows.emplace(booth->mDeviceId, booth);
			DeviceCommand toggle;
			toggle.type = DeviceCommandType::ToggleBoothWindow;
			toggle.boothWindow = booth->mDeviceId;
			booth->mPanel = mWorld.createInteractionPoint("BoothWindow back-side panel",
				SectorId{ static_cast<uint64_t>(booth->getBackSector()->getIndex()) + 1 },
				{ static_cast<float>(x) + 0.5f, static_cast<float>(y) }, 0.25f,
				mWorld.getFixedTimestep(), {{ toggle, InteractionBindingRequirement::Required }});
			mWorld.mInteractionPoints.find(booth->mPanel)->mBoothWindowOwner = booth->mDeviceId;
		}
		TraversalResourceId traversalResource;
		if (options.traversable && window->getFrontSector() && window->getBackSector())
		{
			traversalResource = mWorld.createWindowTraversalResource(format("Window at {},{}", x, y), window);
			window->configureTraversal(true, traversalResource);
		}

		// Set layers
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto& cellDef = layer->getCellDefinition(ix, iy);

				cellDef.sectorObjectIndex = windowIndex;
				cellDef.sectorObjectType = windowObjType;
			}

		World::ConstructionRecord record{ boothWindow ? World::ConstructionType::BoothWindow : World::ConstructionType::Window };
		record.a = layerIndex; record.b = y; record.c = x; record.d = cellsWide; record.e = levelsHigh;
		record.p = options.traversable;
		record.i = static_cast<int32_t>(options.initialState);
		record.j = static_cast<int32_t>(options.style);
		mWorld.recordConstruction(std::move(record));
		return { { windowIndex, windowObjType, windowSector }, window, traversalResource };
	}

	bool WorldBuilder::canAddBoothWindow(uint32_t layer, uint32_t y, uint32_t x,
		uint32_t width, uint32_t height, string* diagnostic) const
	{
		auto reject = [&](string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (width != 1 || height != 1) return reject("BoothWindow requires a fixed one-cell-wide, one-Level-high footprint");
		if (layer >= mWorld.getLayerCount() || layer + 1 >= mWorld.getLayerCount())
			return reject("BoothWindow needs an adjacent Layer behind it");
		if (x >= mWorld.mCellsWide || y >= mWorld.mLevelsHigh) return reject("BoothWindow position is outside the World");
		for (auto side : { layer, layer + 1 })
		{
			auto const& cell = mWorld.mLayers[side]->getCellDefinition(x, y);
			if (cell.sectorIndex == ~0u || !isLocationLike(mWorld.mSectors[cell.sectorIndex]->getType()))
				return reject(format("BoothWindow requires a Room, Corridor, or Facade on Layer {}", side));
			if (cell.floorType == CellFloorType::None || cell.floorType == CellFloorType::ForceBridge)
				return reject(format("BoothWindow requires a walkable approach on Layer {} at Level {}", side, y));
		}
		return canAddSectorWindow(layer, y, x, width, height, diagnostic);
	}

	World::CreateWindowResult WorldBuilder::addBoothWindow(uint32_t layer, uint32_t y,
		uint32_t x, Window::State state)
	{
		return addWindowAperture(layer, y, x, 1, 1, { false, state, Window::Style::Clear }, true);
	}

	// ----------------------------------------------------------------------
	// Light switch
	// ----------------------------------------------------------------------

	World::CreateObjectResult WorldBuilder::addSectorLightSwitch(uint32_t sectorIndex, uint32_t xOffset)
	{
		mWorld.invalidateSimulationSnapshot();
		auto sector = mWorld._getSector(sectorIndex);
		if (!isLocationLike(sector->getType()) || xOffset >= sector->getCellsWide())
			throw WorldException(&mWorld, "Light switch requires an authored Location host cell");
		physicalControl::Demand demand;
		demand.owner = { physicalControl::OwnerType::LocationLightSwitch,
			{ sector->getLayerIndex(), sector->getCellX() + xOffset, sector->getCellY(), 1, 1 },
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { 0, sector->getCellX() + xOffset, sector->getCellY() } };
		(void)mWorld.planPhysicalControls(sector->getLayerIndex(), sectorIndex, sector->getCellY(), &demand);
		mWorld.beginStructuralEdit("addSectorLightSwitch");
		auto ctrl = _createSectorButton("Lightswitch", sector, xOffset, sector->getCellY(), CORE_BUTTON_F_AUTO_REENABLE);

		auto object = ctrl.sector->_getObject(ctrl.index);
		DeviceCommand command;
		command.type = DeviceCommandType::SetSectorLights;
		command.target = SectorId{ (uint64_t)sectorIndex + 1 };
		command.desiredState = !sector->areLightsOn();
		mWorld.createPhysicalControlInteractionPoint("Light switch", ctrl,
			(float)object->getCellY(), 0.15f, mWorld.getFixedTimestep(),
			{ { command, InteractionBindingRequirement::Required } });
		World::ConstructionRecord record{ World::ConstructionType::LightSwitch };
		record.a = sectorIndex; record.b = xOffset;
		mWorld.recordConstruction(std::move(record));
		return ctrl;
	}

	// ----------------------------------------------------------------------
	// Walkway
	// ----------------------------------------------------------------------

	bool WorldBuilder::canAddSectorWalkway(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (sectorIndex >= mWorld.mSectors.size()) return reject("Walkway Room does not exist");
		auto location = dynamic_pointer_cast<const Location>(mWorld.mSectors[sectorIndex]);
		if (!location) return reject("Walkways require a Location");
		if (levelIndex == 0) return reject("Walkways must be placed above the Location's ground floor");
		if (levelIndex >= location->getLevelsHigh() || xOffset >= location->getCellsWide())
			return reject("Walkway position is outside the Location");

		auto const& cellDef = mWorld.mLayers[location->getLayerIndex()]->getCellDefinition(
			location->getCellX() + xOffset, location->getCellY() + levelIndex);
		if (cellDef.floorType == CellFloorType::Ground)
			return reject("Walkways cannot replace a ground floor");
		if (cellDef.floorType == CellFloorType::Walkway)
			return reject("A Walkway already occupies this cell");
		if (cellDef.floorType == CellFloorType::ForceBridge)
			return reject("A Force Bridge already occupies this cell");
		if (cellDef.floorType != CellFloorType::None)
			return reject("The destination floor cell is occupied");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateObjectResult WorldBuilder::addSectorWalkway(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::addSectorWalkway({}, {}, {})", sectorIndex, levelIndex, xOffset);
		string diagnostic;
		if (!canAddSectorWalkway(sectorIndex, levelIndex, xOffset, &diagnostic))
			throw WorldException(&mWorld, format("{} - {}", caller, diagnostic));

		// Once editing an already-built document, replay the authored structure so
		// every Ladder in this column can shorten to the newly nearest Walkway.
		if (mWorld.mBuildFinished && !mWorld.mDeserializingConstruction)
		{
			// The detached replay below validates all affected control families,
			// including new Ladder endpoints and Platform Stops. Do not dirty the
			// live topology before that validation succeeds.
			if (!mWorld.mSimulationPaused)
				throw WorldException(&mWorld, "addSectorWalkway is a structural edit and requires pauseSimulation() before it can run");
			auto room = mWorld._getSector(sectorIndex);
			for (uint32_t i = 0; i < room->getNumObjects(); ++i)
			{
				auto ladderObject = dynamic_pointer_cast<LadderSectorObject>(room->getObject(i));
				if (ladderObject && ladderObject->getCellX() == room->getCellX() + xOffset)
				{
					auto ladder = ladderObject->getLadder();
					auto base = ladderObject->getCellY() - room->getCellY();
					auto top = base + ladder->getLevelsHigh() - 1;
					if (levelIndex > base && levelIndex < top && mWorld.roomLadderIsActive(ladder))
						throw WorldException(&mWorld, "A Room Ladder cannot be resized while it is in use");
				}
				auto liftObject = dynamic_pointer_cast<LiftSectorObject>(room->getObject(i));
				if (liftObject && liftObject->getCellX() == room->getCellX() + xOffset
					&& mWorld.platformLiftIsActive(liftObject->getLift()))
					throw WorldException(&mWorld, "The PlatformLift shaft cannot be changed while it is in use");
			}
			auto records = mWorld.mConstructionRecords;
			World::ConstructionRecord record{ World::ConstructionType::Walkway };
			record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
			// A PlatformLift validates authored landing Walkways while replaying, so
			// keep a newly added Walkway before PlatformLifts in the same Room.
			auto beforeLift = find_if(records.begin(), records.end(), [&](auto const& candidate)
				{ return candidate.type == World::ConstructionType::PlatformLift && candidate.a == sectorIndex; });
			records.insert(beforeLift, record);
			if (!mWorld.normalizeRoomLadderRecords(records, diagnostic))
				throw WorldException(&mWorld, diagnostic);
			mWorld.rebuildFromConstructionRecords(std::move(records));
			auto rebuilt = mWorld._getSector(sectorIndex);
			for (uint32_t i = 0; i < rebuilt->getNumObjects(); ++i)
			{
				auto object = rebuilt->getObject(i);
				if (object && object->getObjectType() == SectorObjectType::Walkway
					&& object->getCellX() == rebuilt->getCellX() + xOffset
					&& object->getCellY() == rebuilt->getCellY() + levelIndex)
					return { i, SectorObjectType::Walkway, rebuilt };
			}
			throw WorldException(&mWorld, "Could not locate the added Walkway");
		}
		mWorld.beginStructuralEdit("addSectorWalkway");

		auto sector = mWorld._getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = mWorld.getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + xOffset,
			sector->getCellY() + levelIndex);
		auto createdWalkway = createWalkway(layerIndex,
			sector->getCellX() + xOffset, sector->getCellY() + levelIndex);

		cellDef.floorIndex = createdWalkway.index;
		cellDef.floorType = CellFloorType::Walkway;
		World::ConstructionRecord record{ World::ConstructionType::Walkway };
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset;
		mWorld.recordConstruction(std::move(record));
		return createdWalkway;
	}

	// ----------------------------------------------------------------------
	// Force bridge
	// ----------------------------------------------------------------------

	World::CreateForceBridgeResult WorldBuilder::addSectorForceBridge(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset)
	{
		mWorld.invalidateSimulationSnapshot();
		return addSectorForceBridge(sectorIndex, levelIndex, xOffset, CreateForceBridgeOptions{});
	}

	bool WorldBuilder::canAddSectorForceBridge(uint32_t sectorIndex, uint32_t levelIndex,
		uint32_t xOffset, CreateForceBridgeOptions const& options, string* diagnostic) const
	{
		auto reject = [&](string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (options.fromSide != CORE_SIDE_LEFT && options.fromSide != CORE_SIDE_RIGHT)
			return reject("Force Bridge extension side is invalid");
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			return reject(format("Force Bridge width must be [1,{}]", CORE_FORCEBRIDGE_MAX_SIZE));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			return reject("An extensible Force Bridge requires one or two controls");
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			return reject("A non-extensible Force Bridge must be permanently extended and have no controls");
		if (sectorIndex >= mWorld.mSectors.size()) return reject("Force Bridge Room does not exist");
		auto room = dynamic_pointer_cast<const Location>(mWorld.mSectors[sectorIndex]);
		if (!room || room->isCorridor()) return reject("Force Bridges can only be placed in Rooms");
		if (levelIndex >= room->getLevelsHigh() || xOffset == 0
			|| (uint64_t)xOffset + options.width >= room->getCellsWide())
			return reject("The Force Bridge and both supports must remain inside its Room");
		auto layer = mWorld.mLayers[room->getLayerIndex()];
		uint32_t x = room->getCellX() + xOffset, y = room->getCellY() + levelIndex;
		for (uint32_t ix = x; ix < x + options.width; ++ix)
			if (layer->getCellDefinition(ix, y).floorType != CellFloorType::None)
				return reject("The Force Bridge span must be clear air");
		auto supported = [](CellDefinition const& cell)
			{ return cell.floorType == CellFloorType::Ground || cell.floorType == CellFloorType::Walkway; };
		if (!supported(layer->getCellDefinition(x - 1, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its left");
		if (!supported(layer->getCellDefinition(x + options.width, y)))
			return reject("The Force Bridge requires Ground or a Walkway on its right");
		try
		{
			mWorld.validateSectorForceBridgeOptions("World::canAddSectorForceBridge", options);
			vector<physicalControl::Demand> demands;
			for (uint32_t i = 0; i < options.controlCount; ++i)
				demands.push_back(mWorld.insetControlDemand(room, physicalControl::OwnerType::ForceBridge,
					{ room->getLayerIndex(), x, y, options.width, 1 }, y,
					i == 0 ? options.fromSide : 1 - options.fromSide));
			mWorld.validatePhysicalControlAdditions(demands);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::CreateForceBridgeResult WorldBuilder::addSectorForceBridge(uint32_t sectorIndex,
		uint32_t levelIndex, uint32_t xOffset, CreateForceBridgeOptions const& options)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::addSectorForceBridge({}, {}, {}, {}, {}, {})", sectorIndex, levelIndex, xOffset, options.width, options.fromSide, options.startExtended);
		string diagnostic;
		if (!canAddSectorForceBridge(sectorIndex, levelIndex, xOffset, options, &diagnostic))
			throw WorldException(&mWorld, format("{} - {}", caller, diagnostic));
		mWorld.beginStructuralEdit("addSectorForceBridge");
		ASSERT_SIDE_OK(options.fromSide);

		auto sector = mWorld._getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		uint32_t x = sector->getCellX() + xOffset;
		uint32_t y = sector->getCellY() + levelIndex;
		auto layer = mWorld.getLayer(layerIndex);

		// Create and mark every cell in the authored span as one floor object.
		auto fbObject = createForceBridge(layerIndex, x, y, options);
		for (uint32_t ix = x; ix < x + options.width; ++ix)
		{
			auto& cellDef = layer->getCellDefinition(ix, y);
			cellDef.floorIndex = fbObject.index;
			cellDef.floorType = CellFloorType::ForceBridge;
		}

		auto forceBridge = dynamic_pointer_cast<ForceBridgeSectorObject>(
			fbObject.sector->_getObject(fbObject.index))->getForceBridge();
		auto traversalResource = mWorld.createForceBridgeTraversalResource("Force bridge", forceBridge);
		forceBridge->configureTraversal(traversalResource);
		forceBridge->mInitiallyBroken = forceBridge->mBroken = options.extensible && options.initiallyBroken;
		auto const halfAgentWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		mWorld.configureForceBridgeQueueLanes(traversalResource,
			SectorId{ (uint64_t)sector->getIndex() + 1 },
			{ Vector2{ (float)x - halfAgentWidth, (float)y },
				Vector2{ (float)(x + options.width) + halfAgentWidth, (float)y } });

		// See if a physical control is needed
		CreateObjectResult createdControls[2];

		if (options.extensible)
		{
			if (options.controlCount > 0) forceBridge->addPreparationSide(options.fromSide);
			if (options.controlCount > 1) forceBridge->addPreparationSide(1 - options.fromSide);
			if (x == sector->getCellX0() && (x + options.width - 1) == sector->getCellX1())
			{
				throw WorldException(&mWorld, format("{} - No space to place Force Bridge controls", caller));
			}

			if (options.controlCount > 0)
			{
				createdControls[0] = createForceBridgeButton(fbObject.sector, x, y, options.width, options.fromSide, CORE_BUTTON_F_AUTO_REENABLE);
				DeviceCommand command{ DeviceCommandType::ToggleExtendedState, {}, false, traversalResource };
				auto point = mWorld.createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[0], (float)y, CORE_RESOURCE_SLOT_WIDTH * 0.5f + 0.001f,
					mWorld.getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				if (auto interaction = mWorld.mInteractionPoints.find(point))
					for (auto permission : options.controlPermissionRequirements[options.fromSide])
						interaction->mPermissionRequirement.set(permission.value - 1);
				mWorld.addTraversalControl(traversalResource, point);
			}
			if (options.controlCount > 1)
			{
				createdControls[1] = createForceBridgeButton(fbObject.sector, x, y, options.width, 1 - options.fromSide, CORE_BUTTON_F_AUTO_REENABLE);
				DeviceCommand command{ DeviceCommandType::ToggleExtendedState, {}, false, traversalResource };
				auto point = mWorld.createPhysicalControlInteractionPoint("Force bridge extension control",
					createdControls[1], (float)y, CORE_RESOURCE_SLOT_WIDTH * 0.5f + 0.001f,
					mWorld.getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
				if (auto interaction = mWorld.mInteractionPoints.find(point))
					for (auto permission : options.controlPermissionRequirements[1 - options.fromSide])
						interaction->mPermissionRequirement.set(permission.value - 1);
				mWorld.addTraversalControl(traversalResource, point);
			}
		}
		// Keep the aggregate at three authored object slots (bridge plus two
		// controls) so changing control count cannot shift unrelated object indices.
		for (uint32_t i = options.controlCount; i < 2; ++i)
			sector->addSectorObject(nullptr);

		CreateForceBridgeResult result{
			fbObject,
			{ createdControls[0], createdControls[1] },
			traversalResource
		};
		World::ConstructionRecord record{ World::ConstructionType::ForceBridge };
		record.a = sectorIndex; record.b = levelIndex; record.c = xOffset; record.d = options.width;
		record.i = options.fromSide; record.p = options.extensible; record.q = options.startExtended;
		record.e = options.controlCount;
		record.initiallyBroken = options.extensible && options.initiallyBroken;
		for (size_t side = 0; side < 2; ++side)
			for (auto permission : options.controlPermissionRequirements[side])
				record.controlPermissionRequirements[side].push_back(
					static_cast<uint32_t>(permission.value));
		mWorld.recordConstruction(std::move(record));
		return result;
	}

	// ----------------------------------------------------------------------
	// Physical control button seams
	// ----------------------------------------------------------------------

	WorldBuilder::CreateObjectResult WorldBuilder::_createSectorButton(string const& name, shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t flags, uint32_t* index)
	{
		mWorld.invalidateSimulationSnapshot();
		uint32_t buttonX = sector->getCellX() + x;
		physicalControl::Demand demand;
		demand.owner = { physicalControl::OwnerType::LocationLightSwitch,
			{ sector->getLayerIndex(), buttonX, y, 1, 1 },
			{ sector->getLayerIndex(), sector->getCellX(), sector->getCellY(),
				sector->getCellsWide(), sector->getLevelsHigh() }, { 0, buttonX, y } };
		auto const& obj = mWorld.createPhysicalControl(name, sector->getLayerIndex(), y, demand, flags);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	WorldBuilder::CreateObjectResult WorldBuilder::_createDoorButton(shared_ptr<const Sector> sector, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t flags, uint32_t* index, uint32_t approachSide)
	{
		mWorld.invalidateSimulationSnapshot();
		auto demand = mWorld.doorControlDemand(sector, x, y, cellsWide, approachSide);
		auto obj = mWorld.createPhysicalControl("Door button", sector->getLayerIndex(), y, demand, flags);
		if (index) *index = obj.index;
		return obj;
	}

	WorldBuilder::CreateObjectResult WorldBuilder::_createBulkheadDoorButton(shared_ptr<const Sector> sector, uint32_t y, int side, uint32_t* index)
	{
		mWorld.invalidateSimulationSnapshot();
		uint32_t thresholdX = side == CORE_SIDE_LEFT ? sector->getCellX1() + 1 : sector->getCellX0();
		auto demand = mWorld.insetControlDemand(sector, physicalControl::OwnerType::BulkheadDoor,
			{ sector->getLayerIndex(), thresholdX, y, 2, 1 }, y, side);
		auto obj = mWorld.createPhysicalControl("BulkheadDoor button", sector->getLayerIndex(), y, demand, 0);

		if (index)
		{
			*index = obj.index;
		}

		return obj;
	}

	// ----------------------------------------------------------------------
	// Ordinary Doors
	// ----------------------------------------------------------------------

	WorldBuilder::CreateObjectResult WorldBuilder::createDoor(uint32_t layerIndex, uint32_t x, uint32_t y,
		uint32_t cellsWide, Door::Height height, uint32_t* vertexIdentifier)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::createDoor({}, {}, {}, {})", layerIndex, x, y, cellsWide);

		// A Door is authored on the front Layer of its pair and opens into the Layer
		// directly behind it.
		auto const backLayer = layerBehind(layerIndex);

		mWorld.validateCellOccupied(caller, layerIndex, x, y);
		mWorld.validateCellOccupied(caller, backLayer, x, y);
		// A threshold is owned by the Layer where it is authored. Objects on the
		// destination Layer coexist with it and do not block its placement.
		mWorld.validateCellHasNoDoor(caller, layerIndex, x, y);

		// Find Sectors that the Door is connecting.
		auto foreSector = mWorld._getSector(mWorld.mLayers[layerIndex]->getCellDefinition(x, y).sectorIndex);
		auto backSector = mWorld._getSector(mWorld.mLayers[backLayer]->getCellDefinition(x, y).sectorIndex);

		// A Door's front Sector is a Location or a Facade: a Facade follows
		// the Room hosting rule (ADR 0003, ticket #48).
		assert(isLocationLike(foreSector->getType()));

		// Create door in Fore Location and add to Back.
		uint32_t doorIndex = foreSector->createDoor(foreSector, backSector, x, y, cellsWide, height, vertexIdentifier);
		backSector->addDoor(dynamic_pointer_cast<DoorSectorObject>(foreSector->_getObject(doorIndex)));

		return {
			doorIndex,
			SectorObjectType::Door,
			foreSector
		};
	}

	WorldBuilder::CreateDoorResult WorldBuilder::_addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, bool controlsAreExternallyBound)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::addSectorDoor({}, {}, {}, {})", layerIndex, y, x, options.width);

		auto const cellsWide = options.width;
		constexpr uint32_t levelsHigh = 1;

		// The placement preflight is the sole home of the rejecting checks. It has
		// proved every cell of the rectangle is occupied inside one Sector on each
		// Layer of the pair, so the Door's two Sectors come straight from the
		// authored cell.
		mWorld.validateSectorDoorPlacement(caller, layerIndex, y, x, options, controlsAreExternallyBound);

		auto const backLayer = layerBehind(layerIndex);
		auto frontLayer = mWorld.getLayer(layerIndex);
		auto behindLayer = mWorld.getLayer(backLayer);
		shared_ptr<Sector> sectors[2] = {
			mWorld._getSector(frontLayer->getCellDefinition(x, y).sectorIndex),
			mWorld._getSector(behindLayer->getCellDefinition(x, y).sectorIndex)
		};

		// Create door, making sure we add it to the other Location as well
		// If there is a button, then a stateful button will control a stateless door - the state
		// can only be in one object.
		auto doorObject = createDoor(layerIndex, x, y, cellsWide, options.height);
		auto doorSectorObject = dynamic_pointer_cast<DoorSectorObject>(doorObject.sector->_getObject(doorObject.index));
		auto door = doorSectorObject->getDoor();
		door->setOpenStyle(options.openStyle);
		door->setSpeedOverride(options.speedOverride);
		door->setHeightScale(options.heightScale);
		// Generated transport Doors use the same construction helper but are not
		// independently breakable in the ordinary Door slice.
		door->mBreakable = isLocationLike(sectors[0]->getType())
			&& isLocationLike(sectors[1]->getType());
		door->mInitiallyBroken = door->mBroken = door->mBreakable && options.initiallyBroken;
		door->mBrokenOpenPercentage = options.brokenOpenPercentage;
		if (door->mBroken) door->mOpenPct = options.brokenOpenPercentage;
		auto traversalResource = mWorld.createDoorTraversalResource(
			format("Door at {},{}", x, y), door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		if (options.crossingLanes != 0)
		{
			mWorld.configureDoorCrossingLanes(traversalResource, options.crossingLanes);
		}

		// Door approaches are explicit resource geometry. Seed each side at the
		// threshold, which Door placement already proved has traversable floor.
		// Queue positions are expanded below only across contiguous usable floor;
		// an unrelated gap elsewhere in a multi-level Room must not invalidate the Door.
		auto const threshold = Vector2{ x + cellsWide * 0.5f, (float)y };
		for (auto const& sector : sectors)
			mWorld.configureDoorQueueLane(traversalResource,
				SectorId{ (uint64_t)sector->getIndex() + 1 }, threshold,
				Vector2::UNIT_X, 0.0f);

		// Each side owns an independent centre-first queue. Expand left and right
		// until that direction reaches either the Sector boundary or a floor gap.
		auto queueResource = mWorld.mTraversalResources.find(traversalResource);
		auto const halfWidth = CORE_RESOURCE_SLOT_WIDTH * 0.5f;
		for (auto& lane : queueResource->mQueueLanes)
		{
			auto sector = mWorld.mSectors[(size_t)lane.sector.value - 1];
			auto positionFits = [&](float positionX)
			{
				if (positionX - halfWidth < sector->getCellX0() - 0.001f
					|| positionX + halfWidth > sector->getCellX1() + 1.0f + 0.001f
					|| threshold.y < sector->getCellY0() - 0.001f
					|| threshold.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT > sector->getCellY1() + 1.0f + 0.001f)
					return false;
				auto const cellX = min(sector->getCellX1(), (uint32_t)floor(positionX));
				auto const cellY = min(sector->getCellY1(), (uint32_t)floor(threshold.y));
				return mWorld.mLayers[sector->getLayerIndex()]
					->getCellDefinition(cellX, cellY).isTraversableOnFoot();
			};

			vector<Vector2> positions{ threshold };
			bool scanLeft = true, scanRight = true;
			for (uint32_t step = 1; scanLeft || scanRight; ++step)
			{
				auto const distance = step * (float)CORE_RESOURCE_QUEUE_SLOT_PITCH;
				auto const left = threshold.x - distance;
				auto const right = threshold.x + distance;
				if (scanLeft)
				{
					scanLeft = positionFits(left);
					if (scanLeft) positions.push_back({ left, threshold.y });
				}
				if (scanRight)
				{
					scanRight = positionFits(right);
					if (scanRight) positions.push_back({ right, threshold.y });
				}
			}
			lane.origin = threshold;
			lane.direction = Vector2::UNIT_X;
			lane.extent = positions.empty() ? 0.0f : max(abs(positions.back().x - threshold.x),
				abs(positions.front().x - threshold.x));
			lane.positions = std::move(positions);
			lane.positionOwners.assign(lane.positions.size(), {});
		}

		// Only the authored Layer carries the Door in its cell grid. The shared
		// Door remains registered in the destination Sector for rendering and
		// traversal, without replacing that Layer's own object occupancy.
		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
		{
			auto& cellDef0 = frontLayer->getCellDefinition(ix, iy);
			cellDef0.sectorObjectIndex = doorObject.index;
			cellDef0.sectorObjectType = doorObject.type;
		}

		// Bind physical controls to typed device commands
		CreateObjectResult createdControls[2];

		for (int i = 0; i < 2; ++i)
		{
			if (options.controls[i])
			{
				auto buttonObject = _createDoorButton(sectors[i], x, y, cellsWide, CORE_BUTTON_F_AUTO_REENABLE, &createdControls[i].index, (uint32_t)i);
				createdControls[i].type = SectorObjectType::InteractionPoint;
				createdControls[i].sector = sectors[i];
				// A Door Button renders like its Door: solid on the authored Layer,
				// an outline from every other Layer.
				static_pointer_cast<Button>(
					buttonObject.sector->getObject(buttonObject.index)->_getObject())
					->_setThresholdLayer(layerIndex);

				if (!controlsAreExternallyBound)
				{
					DeviceCommand command;
					command.type = DeviceCommandType::OpenDoor;
					command.desiredState = true;
					command.traversalResource = traversalResource;
					auto point = mWorld.createPhysicalControlInteractionPoint("Door button",
						createdControls[i], (float)y, CORE_RESOURCE_SLOT_STANDING_HEIGHT * 0.4f,
						mWorld.getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
					auto interaction = mWorld.mInteractionPoints.find(point);
					for (auto permission : options.controlPermissionRequirements[i])
					{
						interaction->mPermissionRequirement.set(permission.value - 1);
					}
					mWorld.addTraversalControl(traversalResource, point);
				}
			}
		}


		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}

	bool WorldBuilder::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x, string* diagnostic) const
	{
		uint32_t liftX, liftWidth;
		if (mWorld.getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			CreateDoorOptions options;
			options.width = liftWidth;
			return canAddCorridorDoor(layerIndex, y, liftX, options, diagnostic);
		}
		return canAddCorridorDoor(layerIndex, y, x, CreateDoorOptions{}, diagnostic);
	}

	bool WorldBuilder::canAddCorridorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		// A Door is authored on the front Layer of the pair it crosses.
		if (layerIndex + 1 >= mWorld.getLayerCount())
			return reject("A Door needs a Layer directly behind the Layer it is authored on");
		auto const backLayer = layerBehind(layerIndex);
		constexpr uint32_t levelsHigh = 1;
		// Door authoring reserves the final column as the world boundary.
		if (options.width == 0 || x >= mWorld.mCellsWide || options.width > mWorld.mCellsWide - x
			|| x + options.width >= mWorld.mCellsWide || y + levelsHigh > mWorld.mLevelsHigh)
			return reject("Door position is outside the world");
		try
		{
			string const caller = "World::canAddCorridorDoor";
			mWorld.validateSectorDoorOptions(caller, options);
			uint32_t liftX, liftWidth;
			bool const liftLanding = mWorld.getLiftLandingGeometry(backLayer, y, x, liftX, liftWidth);
			if (liftLanding && options.heightScale) return reject("Transport-owned Doors refuse Height scale");
			if (liftLanding && (x != liftX || options.width != liftWidth))
				return reject(format("Lift landing doors must start at {} and be {} cells wide", liftX, liftWidth));
			if (options.activationMode != DoorActivationMode::RemoteControlled
				&& (options.controls[0] || options.controls[1]))
				return reject("Physical controls require a remote-controlled Door");
			mWorld.validateBounds(caller, x, y, options.width, levelsHigh);
			// The whole Door rectangle stays inside one Sector on each Layer of its pair:
			// a tall opening that straddles a Sector boundary on the Layer behind would
			// join two Sectors with one threshold.
			mWorld.validateSpaceOnlyInOneSector(caller, layerIndex, x, y, options.width, levelsHigh);
			mWorld.validateSpaceOnlyInOneSector(caller, backLayer, x, y, options.width, levelsHigh);
			shared_ptr<const Sector> sectors[2];
			for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
				for (uint32_t ix = x; ix < x + options.width; ++ix)
			{
				auto const& foreCell = mWorld.mLayers[layerIndex]->getCellDefinition(ix, iy);
				auto const& backCell = mWorld.mLayers[backLayer]->getCellDefinition(ix, iy);
				if (!foreCell.occupied()) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (!backCell.occupied()) return reject("A Room, Corridor, Facade or Lift on the Layer behind is required here");
				sectors[0] = mWorld.mSectors[foreCell.sectorIndex];
				sectors[1] = mWorld.mSectors[backCell.sectorIndex];
				auto fore = dynamic_pointer_cast<const Location>(sectors[0]);
				auto back = dynamic_pointer_cast<const Location>(sectors[1]);
				if (!fore) return reject("A Door must be placed in a Room, Corridor or Facade on the Layer where it is authored");
				if (options.height == Door::Height::Tall
					&& (fore->getType() != SectorType::Location || fore->isCorridor()))
					return reject("A tall Door is only available in a Room");
				if (liftLanding)
				{
					if (sectors[1]->getType() != SectorType::Lift)
						return reject("The complete lift width must overlap one corridor");
				}
				else if (!back) return reject("A Room, Corridor, Facade on the Layer behind is required here");
				// Only the authored Layer owns the Door's cell occupancy. Objects and
				// Markers on the destination Layer remain independent.
				if (foreCell.hasObject() || !foreCell.markers.empty())
					return reject("Another object blocks Door placement");
				// Only the threshold level carries the walking floor.  The levels above it
				// are the opening's headroom, so they need no floor of their own.
				if (iy == y && (!foreCell.isTraversableOnFoot() || !backCell.isTraversableOnFoot()))
					return reject("Door placement requires a traversable floor on both layers");
				// A Walkway or other floor above the threshold level runs through the
				// opening, so the Door cannot cover it.
				if (iy != y && (foreCell.floorType != CellFloorType::None
					|| backCell.floorType != CellFloorType::None))
					return reject("A Door cannot open through a Walkway above its threshold");
				mWorld.validateObjectAllowedInSector(caller, SectorObjectType::Door, foreCell.sectorIndex);
				mWorld.validateObjectAllowedInSector(caller, SectorObjectType::Door, backCell.sectorIndex);
			}
			if (!liftLanding)
				for (auto sector : sectors)
					mWorld.validatePanelWallRectangle(sector->getIndex(), {float(x) + CORE_DOOR_X_INSET, float(y)},
						{float(x + options.width) - CORE_DOOR_X_INSET, float(y) + Door::effectiveHeight(options.height, options.heightScale)});
			if (liftLanding)
			{
				auto const lift = dynamic_pointer_cast<const LiftTransit>(sectors[1]);
				for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
				{
					auto const& existing = lift->getStop(stop);
					if ((uint32_t)((int)existing.sector->getCellY() + existing.sectorOffsetY) == y)
						return reject("This lift already has a stop on this level");
				}
				auto demand = mWorld.doorControlDemand(sectors[0], x, y, options.width, 0);
				(void)mWorld.planPhysicalControls(layerIndex, sectors[0]->getIndex(), y, &demand);
			}
			else for (uint32_t side = 0; side < 2; ++side)
			{
				if (!options.controls[side]) continue;
				auto demand = mWorld.doorControlDemand(sectors[side], x, y, options.width, side);
				(void)mWorld.planPhysicalControls(sectors[side]->getLayerIndex(), sectors[side]->getIndex(), y, &demand);
			}
		}
		catch (std::exception const& error)
		{
			return reject(error.what());
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	WorldBuilder::CreateDoorResult WorldBuilder::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x)
	{
		mWorld.invalidateSimulationSnapshot();
		return addSectorDoor(layerIndex, y, x, CreateDoorOptions{});
	}

	WorldBuilder::CreateDoorResult WorldBuilder::addSectorDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		CreateDoorOptions const& options)
	{
		string const caller = format("World::addSectorDoor({}, {}, {}, {})", layerIndex, y, x, options.width);
		// Every rejecting check runs before beginStructuralEdit() so a refused call
		// stays a true no-op: no SectorObjects, traversal resources, events, or
		// construction records, no modified flag, and no dirty topology
		// (ticket #196).
		mWorld.validateSectorDoorOptions(caller, options);
		uint32_t liftX, liftWidth;
		if (mWorld.getLiftLandingGeometry(layerBehind(layerIndex), y, x, liftX, liftWidth))
		{
			if (options.heightScale) throw WorldException(&mWorld, "Transport-owned Doors refuse Height scale");
			string diagnostic;
			CreateDoorOptions normalized;
			normalized.width = liftWidth;
			if (!canAddCorridorDoor(layerIndex, y, liftX, normalized, &diagnostic))
				throw WorldException(&mWorld, diagnostic);
			auto const liftIndex = mWorld.mLayers[layerBehind(layerIndex)]->getCellDefinition(liftX, y).sectorIndex;
			auto lift = dynamic_pointer_cast<LiftTransit>(mWorld._getSector(liftIndex));
			auto idlePlan = mWorld.planResizeLift(liftIndex, lift->getCellX(), lift->getCellY(),
				lift->getCellsWide(), lift->getLevelsHigh());
			if (!idlePlan.valid) throw WorldException(&mWorld, idlePlan.diagnostic);
			idlePlan.stopOffsets.clear();
			for (uint32_t stop = 0; stop < lift->getNumStops(); ++stop)
			{
				auto const& value = lift->getStop(stop);
				idlePlan.stopOffsets.push_back((uint32_t)((int)value.sector->getCellY()
					+ value.sectorOffsetY - (int)lift->getCellY()));
			}
			idlePlan.stopOffsets.push_back(y - lift->getCellY());
			sort(idlePlan.stopOffsets.begin(), idlePlan.stopOffsets.end());
			vector<World::ConstructionRecord> records;
			if (!mWorld.prepareLiftEdit(idlePlan, records, diagnostic)) throw WorldException(&mWorld, diagnostic);
			// The plan is complete and every authored record is built, so the edit
			// can commit without a later refusal stranding a dirty World.
			mWorld.beginStructuralEdit("addSectorDoor");
			mWorld.rebuildFromConstructionRecords(std::move(records));
			auto const& cell = mWorld.mLayers[layerIndex]->getCellDefinition(liftX, y);
			auto sector = mWorld._getSector(cell.sectorIndex);
			CreateObjectResult doorResult{ cell.sectorObjectIndex, SectorObjectType::Door, sector };
			auto doorObject = dynamic_pointer_cast<DoorSectorObject>(sector->_getObject(cell.sectorObjectIndex));
			return { doorResult, {}, doorObject->getDoor()->getTraversalResourceId() };
		}
		// Complete option and placement preflight, still before any mutation.
		mWorld.validateSectorDoorPlacement(caller, layerIndex, y, x, options, false);
		mWorld.beginStructuralEdit("addSectorDoor");
		auto result = _addSectorDoor(layerIndex, y, x, options);
		World::ConstructionRecord record{ World::ConstructionType::Door };
		record.layer = layerIndex;
		record.a = y; record.b = x; record.c = options.width; record.d = options.crossingLanes;
		record.e = static_cast<uint32_t>(options.height);
		record.p = options.controls[0]; record.q = options.controls[1];
		record.i = static_cast<int32_t>(options.activationMode); record.x = options.holdOpenSeconds;
		record.j = static_cast<int32_t>(options.openStyle);
		record.initiallyBroken = options.initiallyBroken;
		record.brokenOpenPercentage = options.brokenOpenPercentage;
		record.doorSpeed = options.speedOverride;
		record.doorHeightScale = options.heightScale;
		for (size_t side = 0; side < 2; ++side)
			for (auto permission : options.controlPermissionRequirements[side])
				record.controlPermissionRequirements[side].push_back(
					static_cast<uint32_t>(permission.value));
		mWorld.recordConstruction(std::move(record));
		return result;
	}

	void WorldBuilder::addSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex)
	{
		mWorld.invalidateSimulationSnapshot();
		if (sectorIndex >= mWorld.mSectors.size() || !mWorld.mSectors[sectorIndex]
			|| objectIndex >= mWorld.mSectors[sectorIndex]->getNumObjects())
		{
			throw WorldException(&mWorld, "The selected Door no longer exists");
		}

		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mWorld.mSectors[sectorIndex]->getObject(objectIndex));
		if (doorObject && (mWorld.isLiftOwnedDoor(doorObject) || mWorld.isShuttleOwnedDoor(doorObject)))
			throw WorldException(&mWorld, "Transport-owned Doors are read-only; their call button is managed by the transport");
		if (!doorObject)
		{
			throw WorldException(&mWorld, "The selected object is not a Door");
		}
		auto door = doorObject->getDoor();
		auto sector = mWorld.mSectors[sectorIndex];
		// A Door owns one Sector in each Layer of the pair it crosses. The selected
		// Sector may sit on either side of that pair; giving the Door a Button
		// always gives it Buttons on both sides.
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector)
		{
			throw WorldException(&mWorld, "The selected Door does not belong to this Sector");
		}
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;

		auto source = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[&](World::ConstructionRecord const& record)
			{
				return record.type == World::ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mWorld.mConstructionRecords.end())
		{
			throw WorldException(&mWorld, "This Door does not support an added Door Button");
		}
		if (source->p && source->q)
		{
			throw WorldException(&mWorld, "This Door already has Door Buttons on both sides");
		}
		// The edit is all-or-nothing: every side that still lacks a Button must
		// have space for one before either side is created.
		shared_ptr<const Sector> sides[2];
		sides[0] = door->getFrontSector();
		sides[1] = door->getBackSector();
		bool const hasButton[2] = { source->p, source->q };
		for (uint32_t side = 0; side < 2; ++side)
		{
			if (hasButton[side]) continue;
			auto demand = mWorld.doorControlDemand(sides[side], doorObject->getCellX(),
				doorObject->getCellY(), door->getCellsWide(), side);
			(void)mWorld.planPhysicalControls(sides[side]->getLayerIndex(), sides[side]->getIndex(),
				doorObject->getCellY(), &demand);
		}

		mWorld.beginStructuralEdit("addSectorDoorButton");

		for (uint32_t side = 0; side < 2; ++side)
		{
			if (hasButton[side]) continue;
			auto control = _createDoorButton(sides[side], doorObject->getCellX(),
				doorObject->getCellY(), door->getCellsWide(), CORE_BUTTON_F_AUTO_REENABLE, nullptr, side);
			// A Door Button renders like its Door: solid on the Layer the Door was
			// authored on, an outline from every other Layer.
			auto button = static_pointer_cast<Button>(
				control.sector->getObject(control.index)->_getObject());
			button->_setThresholdLayer(door->getFrontLayer());
			DeviceCommand command;
			command.type = DeviceCommandType::OpenDoor;
			command.desiredState = true;
			command.traversalResource = door->getTraversalResourceId();
			auto point = mWorld.createPhysicalControlInteractionPoint("Door button", control,
				(float)doorObject->getCellY(), CORE_RESOURCE_SLOT_STANDING_HEIGHT * 0.4f,
				mWorld.getFixedTimestep(), { { command, InteractionBindingRequirement::Required } });
			auto interaction = mWorld.mInteractionPoints.find(point);
			for (auto permission : source->values)
			{
				interaction->mPermissionRequirement.set(permission - 1);
				source->controlPermissionRequirements[side].push_back(permission);
			}
			if (!mWorld.addTraversalControl(door->getTraversalResourceId(), point))
			{
				throw WorldException(&mWorld, "Could not bind the Door Button to its Door");
			}
		}

		// A Door with a physical open control uses remote-controlled preparation.
		auto resource = mWorld.mTraversalResources.find(door->getTraversalResourceId());
		resource->mDoorActivationMode = DoorActivationMode::RemoteControlled;
		door->configureTraversal(DoorActivationMode::RemoteControlled,
			door->getTraversalResourceId(), door->mHoldOpenTime);
		// Record the mode the Door had before its first editor-added Button so
		// removal can restore it. Doors loaded with existing Buttons have no such
		// history; removing those Buttons falls back to manual activation.
		if (!source->p && !source->q)
		{
			source->preButtonActivationMode = source->i;
		}
		source->i = static_cast<int32_t>(DoorActivationMode::RemoteControlled);
		source->p = true;
		source->q = true;
		// The direct interaction no longer exists. Its access intent now belongs
		// independently to each generated control.
		source->values.clear();
		door->mPermissionRequirement.reset();
	}

	bool WorldBuilder::canAddSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		if (sectorIndex >= mWorld.mSectors.size() || !mWorld.mSectors[sectorIndex]
			|| objectIndex >= mWorld.mSectors[sectorIndex]->getNumObjects()) return false;
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mWorld.mSectors[sectorIndex]->getObject(objectIndex));
		if (!doorObject || mWorld.isLiftOwnedDoor(doorObject) || mWorld.isShuttleOwnedDoor(doorObject)) return false;
		auto door = doorObject->getDoor();
		auto sector = mWorld.mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector) return false;
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto source = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[&](World::ConstructionRecord const& record)
			{
				return record.type == World::ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		if (source == mWorld.mConstructionRecords.end() || (source->p && source->q)) return false;
		try
		{
			shared_ptr<const Sector> sides[] = { door->getFrontSector(), door->getBackSector() };
			for (uint32_t side = 0; side < 2; ++side)
			{
				if (side == 0 ? source->p : source->q) continue;
				auto demand = mWorld.doorControlDemand(sides[side], doorObject->getCellX(),
					doorObject->getCellY(), door->getCellsWide(), side);
				(void)mWorld.planPhysicalControls(sides[side]->getLayerIndex(), sides[side]->getIndex(),
					doorObject->getCellY(), &demand);
			}
		}
		catch (exception const&) { return false; }
		return true;
	}

	bool WorldBuilder::canRemoveSectorDoorButton(uint32_t sectorIndex, uint32_t objectIndex) const
	{
		if (sectorIndex >= mWorld.mSectors.size() || !mWorld.mSectors[sectorIndex]
			|| objectIndex >= mWorld.mSectors[sectorIndex]->getNumObjects()) return false;
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mWorld.mSectors[sectorIndex]->getObject(objectIndex));
		if (!doorObject || mWorld.isLiftOwnedDoor(doorObject) || mWorld.isShuttleOwnedDoor(doorObject)) return false;
		auto door = doorObject->getDoor();
		auto sector = mWorld.mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector) return false;
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto source = find_if(mWorld.mConstructionRecords.cbegin(), mWorld.mConstructionRecords.cend(),
			[&](World::ConstructionRecord const& record)
			{
				return record.type == World::ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorObject->getCellY()
					&& record.b == doorObject->getCellX()
					&& record.c == door->getCellsWide();
			});
		return source != mWorld.mConstructionRecords.cend() && (source->p || source->q);
	}

	shared_ptr<const DoorSectorObject> WorldBuilder::removeSectorDoorButton(uint32_t sectorIndex,
		uint32_t objectIndex, optional<vector<AccessPermissionId>> resultingRequirement)
	{
		mWorld.invalidateSimulationSnapshot();
		if (!mWorld.mSimulationPaused)
			throw WorldException(&mWorld, "Removing a Door Button requires the simulation to be paused");
		if (sectorIndex >= mWorld.mSectors.size() || !mWorld.mSectors[sectorIndex]
			|| objectIndex >= mWorld.mSectors[sectorIndex]->getNumObjects())
		{
			throw WorldException(&mWorld, "The selected Door no longer exists");
		}
		auto doorObject = dynamic_pointer_cast<DoorSectorObject>(
			mWorld.mSectors[sectorIndex]->getObject(objectIndex));
		if (doorObject && (mWorld.isLiftOwnedDoor(doorObject) || mWorld.isShuttleOwnedDoor(doorObject)))
			throw WorldException(&mWorld, "Transport-owned Doors are read-only; their call button is managed by the transport");
		if (!doorObject)
		{
			throw WorldException(&mWorld, "The selected object is not a Door");
		}
		auto door = doorObject->getDoor();
		auto sector = mWorld.mSectors[sectorIndex];
		bool const isFrontSide = door->getFrontSector() == sector;
		if (!isFrontSide && door->getBackSector() != sector)
		{
			throw WorldException(&mWorld, "The selected Door does not belong to this Sector");
		}
		auto const doorLayer = isFrontSide ? sector->getLayerIndex() : sector->getLayerIndex() - 1;
		auto const doorX = doorObject->getCellX();
		auto const doorY = doorObject->getCellY();
		auto const doorWidth = door->getCellsWide();
		auto source = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[&](World::ConstructionRecord const& record)
			{
				return record.type == World::ConstructionType::Door
					&& record.layer == doorLayer
					&& record.a == doorY
					&& record.b == doorX
					&& record.c == doorWidth;
			});
		if (source == mWorld.mConstructionRecords.end())
		{
			throw WorldException(&mWorld, "This Door does not support an added Door Button");
		}
		if (!source->p && !source->q)
		{
			throw WorldException(&mWorld, "This Door has no Door Buttons to remove");
		}

		// Collapse two independent operations only when their requirements agree.
		// A caller resolving a conflict must supply the resulting all-of set;
		// omitting it refuses before the rebuild, leaving the Door untouched.
		array<vector<AccessPermissionId>, 2> sideRequirements;
		auto resource = mWorld.mTraversalResources.find(door->getTraversalResourceId());
		for (auto pointId : resource->mControls)
		{
			auto point = mWorld.mInteractionPoints.find(pointId);
			if (!point) continue;
			size_t side = point->mSector == SectorId{ static_cast<uint64_t>(door->getBackSector()->getIndex()) + 1 } ? 1 : 0;
			for (size_t bit = 0; bit < AccessPermission::Capacity; ++bit)
				if (point->mPermissionRequirement.test(bit))
					sideRequirements[side].push_back(AccessPermissionId{ bit + 1 });
		}
		vector<AccessPermissionId> collapsed;
		if (resultingRequirement) collapsed = *resultingRequirement;
		else if (sideRequirements[0] == sideRequirements[1]) collapsed = sideRequirements[0];
		else throw WorldException(&mWorld,
			"Door controls have different permission requirements; choose the resulting manual Door requirement");
		bitset<AccessPermission::Capacity> seenPermissions;
		for (auto permission : collapsed)
		{
			if (!mWorld.lookupAccessPermission(permission))
				throw WorldException(&mWorld, "The resulting Door requirement references an unknown Access permission");
			if (seenPermissions.test(permission.value - 1))
				throw WorldException(&mWorld, "The resulting Door requirement contains a duplicate Access permission");
			seenPermissions.set(permission.value - 1);
		}

		// The record is the authored source of truth: clearing its control flags
		// and restoring the pre-Button activation mode, then replaying, removes
		// both Buttons, their InteractionPoints, and their traversal bindings.
		vector<World::ConstructionRecord> records = mWorld.mConstructionRecords;
		for (auto& record : records)
		{
			if (record.type == World::ConstructionType::Door && record.layer == doorLayer
				&& record.a == doorY && record.b == doorX && record.c == doorWidth)
			{
				record.p = false;
				record.q = false;
				record.i = record.preButtonActivationMode >= 0
					? record.preButtonActivationMode
					: static_cast<int32_t>(DoorActivationMode::Manual);
				record.preButtonActivationMode = -1;
				record.values.clear();
				if (record.i == static_cast<int32_t>(DoorActivationMode::Manual))
					for (auto permission : collapsed)
						record.values.push_back(static_cast<uint32_t>(permission.value));
				record.controlPermissionRequirements = {};
			}
		}
		mWorld.rebuildFromConstructionRecords(std::move(records));

		// The rebuild replaced every object, so re-resolve the Door on its front
		// Layer and hand the fresh object back for the caller's selection.
		auto const& cell = mWorld.mLayers[doorLayer]->getCellDefinition(doorX, doorY);
		if (cell.sectorObjectType == SectorObjectType::Door && cell.sectorObjectIndex != ~0u)
		{
			auto rebuilt = dynamic_pointer_cast<DoorSectorObject>(
				mWorld._getSector(cell.sectorIndex)->_getObject(cell.sectorObjectIndex));
			if (rebuilt) return rebuilt;
		}
		return nullptr;
	}

	// ----------------------------------------------------------------------
	// Bulkhead Doors
	// ----------------------------------------------------------------------

	WorldBuilder::CreateObjectResult WorldBuilder::createBulkheadDoor(uint32_t layerIndex, uint32_t x, uint32_t y, int side)
	{
		mWorld.invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(side);

		string caller = format("World::createBulkheadDoor({}, {}, {}, {})", layerIndex, x, y, side);
		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		mWorld.validateCellOccupied(caller, layerIndex, cx0, y);
		mWorld.validateCellOccupied(caller, layerIndex, cx1, y);

		// Find locations that the Door is connecting.
		shared_ptr<Sector> leftSector{ nullptr };
		shared_ptr<Sector> rightSector{ nullptr };

		auto layer = mWorld.getLayer(layerIndex);

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		leftSector = mWorld._getSector(cellDef0.sectorIndex);
		rightSector = mWorld._getSector(cellDef1.sectorIndex);

		// Create bulkhead door in one location and add to the other
		auto doorIndex = leftSector->createBulkheadDoor(leftSector, rightSector, y - leftSector->getCellY(), CORE_SIDE_LEFT);

		rightSector->addBulkheadDoor(dynamic_pointer_cast<BulkheadDoorSectorObject>(leftSector->_getObject(doorIndex)), y - rightSector->getCellY(), CORE_SIDE_RIGHT);
		
		return {
			doorIndex,
			SectorObjectType::BulkheadDoor,
			leftSector
		};
	}

	bool WorldBuilder::canAddSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (layerIndex >= mWorld.mLayers.size() || y >= mWorld.mLevelsHigh
			|| (side != CORE_SIDE_LEFT && side != CORE_SIDE_RIGHT))
			return reject("Bulkhead Door position is outside the world");
		uint32_t thresholdX;
		if (side == CORE_SIDE_LEFT)
		{
			if (x == 0 || x >= mWorld.mCellsWide) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x;
		}
		else
		{
			if (x >= mWorld.mCellsWide - 1) return reject("Bulkhead Doors require a cell on each side");
			thresholdX = x + 1;
		}
		if (!isFiniteTiming(options.holdOpenSeconds))
			return reject("Bulkhead Door hold-open time must be finite and non-negative");
		if (!Door::speedIsValid(options.speedOverride))
			return reject("Bulkhead Door speed must be finite and positive");
		if (!Door::brokenOpenPercentageIsValid(options.brokenOpenPercentage))
			return reject("Bulkhead Door broken open percentage must be a finite value from 0 to 1");
		if (!isfinite(options.automaticSensorDistance)
			|| options.automaticSensorDistance < 0.0f)
			return reject("Bulkhead Door automatic sensor distance must be finite and non-negative");
		if (options.crossingLanes != 1)
			return reject("Bulkhead Doors support exactly one crossing lane");
		if (options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
			return reject("Physical controls require a remote-controlled Bulkhead Door");
		if (!mWorld.mDeserializingConstruction)
			for (auto const& requirement : options.controlPermissionRequirements)
				for (auto permission : requirement)
					if (!mWorld.lookupAccessPermission(permission))
						return reject("A Bulkhead Door control requirement references an unknown Access permission");
		try
		{
			auto const& left = mWorld.mLayers[layerIndex]->getCellDefinition(thresholdX - 1, y);
			auto const& right = mWorld.mLayers[layerIndex]->getCellDefinition(thresholdX, y);
			if (!left.occupied() || !right.occupied())
				return reject("Bulkhead Doors require a Location on each side");
			if (left.sectorIndex == right.sectorIndex)
				return reject("Bulkhead Doors must connect two distinct Locations");
			if (!dynamic_pointer_cast<const Location>(mWorld.mSectors[left.sectorIndex])
				|| !dynamic_pointer_cast<const Location>(mWorld.mSectors[right.sectorIndex]))
				return reject("Bulkhead Doors can only connect Locations");
			// A Bulkhead Door is set into a pair of wall ends. A Facade has no
			// wall ends, so there is nowhere for one to go (ADR 0003).
			if (mWorld.mSectors[left.sectorIndex]->getType() == SectorType::Facade
				|| mWorld.mSectors[right.sectorIndex]->getType() == SectorType::Facade)
				return reject("A Bulkhead Door cannot connect a Facade: a Facade has no wall ends");
			if (!left.isTraversableOnFoot() || !right.isTraversableOnFoot())
				return reject("Bulkhead Door placement requires a traversable floor on both sides");
			if (left.bulkheadIndices[CORE_SIDE_RIGHT] != ~0u
				|| right.bulkheadIndices[CORE_SIDE_LEFT] != ~0u)
				return reject("A Bulkhead Door already occupies this boundary");
			if (left.sectorObjectType == SectorObjectType::Door
				|| isWindowAperture(left.sectorObjectType)
				|| right.sectorObjectType == SectorObjectType::Door
				|| isWindowAperture(right.sectorObjectType))
				return reject("Another object blocks Bulkhead Door placement");
			mWorld.validateObjectAllowedInSector("World::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, left.sectorIndex);
			mWorld.validateObjectAllowedInSector("World::canAddSectorBulkheadDoor",
				SectorObjectType::BulkheadDoor, right.sectorIndex);
			for (auto owner : {left.sectorIndex, right.sectorIndex})
				mWorld.validatePanelWallRectangle(owner,
					{float(thresholdX) - CORE_BULKHEAD_DOOR_WIDTH * 0.5f, float(y)},
					{float(thresholdX) + CORE_BULKHEAD_DOOR_WIDTH * 0.5f, float(y) + CORE_DOOR_HEIGHT});
			vector<physicalControl::Demand> demands;
			for (int controlSide = 0; controlSide < CORE_NUM_SIDES; ++controlSide)
				if (options.controls[controlSide])
					demands.push_back(mWorld.insetControlDemand(mWorld.mSectors[controlSide == CORE_SIDE_LEFT ? left.sectorIndex : right.sectorIndex],
						physicalControl::OwnerType::BulkheadDoor, { layerIndex, thresholdX, y, 2, 1 }, y, controlSide));
			if (demands.empty()) mWorld.validatePhysicalControlBoundary(layerIndex, y, thresholdX);
			else mWorld.validatePhysicalControlAdditions(demands, thresholdX);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	WorldBuilder::CreateBulkheadDoorResult WorldBuilder::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y,
		uint32_t x, int side)
	{
		mWorld.invalidateSimulationSnapshot();
		return addSectorBulkheadDoor(layerIndex, y, x, side, CreateBulkheadDoorOptions{});
	}

	WorldBuilder::CreateBulkheadDoorResult WorldBuilder::addSectorBulkheadDoor(uint32_t layerIndex, uint32_t y, uint32_t x,
		int side, CreateBulkheadDoorOptions const& options)
	{
		mWorld.invalidateSimulationSnapshot();
		ASSERT_SIDE_OK(side);

		string caller = format("World::addSectorBulkheadDoor({}, {}, {}, {})", layerIndex, y, x, side);
		string diagnostic;
		// The option and placement preflight runs before beginStructuralEdit() so a
		// refused Bulkhead Door add is a true no-op and no non-finite timing
		// reaches the tick conversion (#198).
		if (!canAddSectorBulkheadDoor(layerIndex, y, x, side, options, &diagnostic))
			throw WorldException(&mWorld, format("{} - {}", caller, diagnostic));

		mWorld.beginStructuralEdit("addSectorBulkheadDoor");

		// Get locations on either side.
		auto layer = mWorld.getLayer(layerIndex);

		uint32_t cx0, cx1;

		if (side == CORE_SIDE_LEFT)
		{
			cx0 = x - 1;
			cx1 = x;
		}
		else
		{
			cx0 = x;
			cx1 = x + 1;
		}

		auto& cellDef0 = layer->getCellDefinition(cx0, y);
		auto& cellDef1 = layer->getCellDefinition(cx1, y);

		// Check that there are no Doors or Windows in cells X and X-1, as there won't
		// be space for them.
		if (cellDef0.sectorObjectType == SectorObjectType::Door || isWindowAperture(cellDef0.sectorObjectType))
		{
			throw WorldException(&mWorld, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx0, y));
		}
		if (cellDef1.sectorObjectType == SectorObjectType::Door || isWindowAperture(cellDef1.sectorObjectType))
		{
			throw WorldException(&mWorld, format("{} - cell at {}, {} has an object blocking the Bulkhead door", caller, cx1, y));
		}

		mWorld.validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef0.sectorIndex);
		mWorld.validateObjectAllowedInSector(caller, SectorObjectType::BulkheadDoor, cellDef1.sectorIndex);
		
		// Create Bulkhead door
		auto doorObject = createBulkheadDoor(layerIndex, x, y, side);

		// Update cells
		cellDef0.bulkheadIndices[CORE_SIDE_RIGHT] = doorObject.index;
		cellDef1.bulkheadIndices[CORE_SIDE_LEFT] = doorObject.index;
		mWorld.reflowAllPhysicalControls();

		// Add buttons: place two, one of each side.
		auto sector0 = mWorld._getSector(cellDef0.sectorIndex);
		auto sector1 = mWorld._getSector(cellDef1.sectorIndex);

		auto dooSectorObject = doorObject.sector->_getObject(doorObject.index);
		auto door = dynamic_pointer_cast<BulkheadDoorSectorObject>(dooSectorObject)->getDoor();

		auto traversalResource = mWorld.createDoorTraversalResource(format("Bulkhead door at {},{}", x, y),
			door, options.activationMode, options.holdOpenSeconds);
		door->configureTraversal(options.activationMode, traversalResource, options.holdOpenSeconds);
		door->setAutomaticSensorDistance(options.automaticSensorDistance);
		door->setSpeedOverride(options.speedOverride);
		door->mBreakable = true;
		door->mInitiallyBroken = door->mBroken = options.initiallyBroken;
		door->mBrokenOpenPercentage = options.brokenOpenPercentage;
		if (door->mBroken) door->mOpenPct = options.brokenOpenPercentage;
		mWorld.configureDoorCrossingLanes(traversalResource, options.crossingLanes);

		// Same-layer geometry gets explicit approaches on opposite sides of the
		// threshold; no Y/layer heuristic participates in authorization.
		auto threshold = Vector2{ (float)cx1, (float)y };
		auto leftOrigin = threshold - Vector2::UNIT_X * CORE_RESOURCE_QUEUE_SLOT_PITCH;
		auto rightOrigin = threshold + Vector2::UNIT_X * CORE_RESOURCE_QUEUE_SLOT_PITCH;
		mWorld.configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector0->getIndex() + 1 },
			leftOrigin, Vector2::NEGATIVE_UNIT_X,
			max(0.0f, leftOrigin.x - (sector0->getCellX0() + CORE_RESOURCE_SLOT_WIDTH * 0.5f)));
		mWorld.configureDoorQueueLane(traversalResource, SectorId{ (uint64_t)sector1->getIndex() + 1 },
			rightOrigin, Vector2::UNIT_X,
			max(0.0f, (sector1->getCellX1() + 1.0f - CORE_RESOURCE_SLOT_WIDTH * 0.5f) - rightOrigin.x));

		CreateObjectResult createdControls[2];

		for (int i = 0; i < CORE_NUM_SIDES; ++i)
		{
			if (!options.controls[i]) continue;
			auto sector = i == CORE_SIDE_LEFT ? sector0 : sector1;
			createdControls[i] = _createBulkheadDoorButton(sector, y, i);
			if (options.activationMode == DoorActivationMode::RemoteControlled)
			{
				DeviceCommand command;
				command.type = DeviceCommandType::OpenDoor;
				command.desiredState = true;
				command.traversalResource = traversalResource;
				auto point = mWorld.createPhysicalControlInteractionPoint("Bulkhead door button",
					createdControls[i], (float)y, 0.15f, mWorld.getFixedTimestep(),
					{ { command, InteractionBindingRequirement::Required } });
				auto interaction = mWorld.mInteractionPoints.find(point);
				for (auto permission : options.controlPermissionRequirements[i])
				{
					interaction->mPermissionRequirement.set(permission.value - 1);
				}
				mWorld.addTraversalControl(traversalResource, point);
			}
		}
		World::ConstructionRecord record{ World::ConstructionType::BulkheadDoor };
		record.a = layerIndex; record.b = y; record.c = x; record.i = side;
		record.p = options.controls[0]; record.q = options.controls[1];
		record.j = static_cast<int32_t>(options.activationMode);
		record.x = options.holdOpenSeconds; record.d = options.crossingLanes;
		record.y = options.automaticSensorDistance;
		record.initiallyBroken = options.initiallyBroken;
		record.brokenOpenPercentage = options.brokenOpenPercentage;
		record.doorSpeed = options.speedOverride;
		for (size_t controlSide = 0; controlSide < 2; ++controlSide)
			for (auto permission : options.controlPermissionRequirements[controlSide])
				record.controlPermissionRequirements[controlSide].push_back(
					static_cast<uint32_t>(permission.value));
		mWorld.recordConstruction(std::move(record));
		return { doorObject, { createdControls[0], createdControls[1] }, traversalResource };
	}
}
