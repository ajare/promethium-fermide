#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/WorldBuilder.h"

#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/Layer.h"
#include "core/Location.h"
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
		auto ctrl = mWorld._createSectorButton("Lightswitch", sector, xOffset, sector->getCellY(), CORE_BUTTON_F_AUTO_REENABLE);

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
}
