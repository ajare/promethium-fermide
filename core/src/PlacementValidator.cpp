#include <algorithm>
#include <cmath>
#include <format>
#include <set>
#include <stdexcept>
#include <utility>

#include "core/PlacementValidator.h"

#include "core/Defines.h"
#include "core/Exceptions.h"
#include "core/Agent.h"
#include "core/Door.h"
#include "core/Edge.h"
#include "core/ForceBridge.h"
#include "core/Graph.h"
#include "core/Ladder.h"
#include "core/Layer.h"
#include "core/Lift.h"
#include "core/Location.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SectorType.h"
#include "core/Shuttle.h"
#include "core/SimulationCoordinator.h"
#include "core/Stairwell.h"
#include "core/Window.h"


namespace core
{
	using namespace std;

	PlacementValidator::PlacementValidator(World& world)
		: mWorld(world)
	{
	}

	bool PlacementValidator::dimensionsAreSupported(uint32_t cellsWide, uint32_t levelsHigh,
		uint32_t layerCount, string* diagnostic)
	{
		if (diagnostic) diagnostic->clear();

		auto const reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};

		if (cellsWide == 0 || levelsHigh == 0)
		{
			return reject("World dimensions must be positive");
		}

		if (levelsHigh > CORE_MAX_LEVELS)
			return reject(format("World Level count exceeds the {} Level limit", CORE_MAX_LEVELS));

		if (layerCount < 2 || layerCount > CORE_MAX_LAYERS)
		{
			return reject(format("World layer count {} is out of range", layerCount));
		}

		// One CellDefinition per (x, level) per Layer. Evaluating the product in
		// 64 bits keeps an overflowing pair from wrapping back under the limit.
		uint64_t const cellsPerLayer =
			static_cast<uint64_t>(cellsWide) * levelsHigh;
		uint64_t const totalCells = cellsPerLayer * layerCount;
		if (totalCells > CORE_MAX_WORLD_CELLS)
		{
			return reject(format(
				"World dimensions {} x {} over {} layers exceed the {} cell limit",
				cellsWide, levelsHigh, layerCount, CORE_MAX_WORLD_CELLS));
		}

		return true;
	}

	void PlacementValidator::validateCellOccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = mWorld.getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.occupied())
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} is not occupied.", caller, x, y));
		}
	}

	void PlacementValidator::validateCellUnoccupied(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = mWorld.getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.occupied())
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} is occupied.", caller, x, y));
		}
	}

	void PlacementValidator::validateCellIsInSector(string const& caller, uint32_t x, uint32_t y, shared_ptr<const Sector> sector) const
	{
		auto layer = mWorld.getLayer(sector->getLayerIndex());

		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sectorIndex = sector->getIndex();

		if (cellDef.sectorIndex != sectorIndex)
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} is not in sector {}.", caller, x, y, sectorIndex));
		}
	}

	void PlacementValidator::validateCellHasNoObject(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = mWorld.getLayer(layerIndex);

		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.hasObject())
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} has an object.", caller, x, y));
		}
	}

	void PlacementValidator::validateCellHasNoDoor(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = mWorld.getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (cellDef.sectorObjectType == SectorObjectType::Door)
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} has a door.", caller, x, y));
		}
	}

	void PlacementValidator::validateCellTraversableOnFoot(string const& caller, string const& desiredObject, uint32_t layerIndex, uint32_t x, uint32_t y) const
	{
		auto layer = mWorld.getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);

		if (!cellDef.isTraversableOnFoot())
		{
			throw WorldException(&mWorld, format("{} - cell at {},{} is not traversable, which blocks {} being placed", caller, x, y, desiredObject));
		}
	}

	void PlacementValidator::validateLayer(string const& caller, uint32_t layerIndex) const
	{
		if (layerIndex >= mWorld.mLayers.size())
		{
			throw WorldException(&mWorld, format("{} - layerIndex={} is out of bounds", caller, layerIndex));
		}
	}

	void PlacementValidator::validateBounds(string const& caller, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		if (x >= mWorld.mCellsWide)
		{
			throw WorldException(&mWorld, format("{} - x={} is out of bounds", caller, x));
		}

		if (cellsWide > mWorld.mCellsWide - x)
		{
			throw WorldException(&mWorld, format("{} - cellsWide={} is out of bounds", caller, cellsWide));
		}

		if (y >= mWorld.mLevelsHigh)
		{
			throw WorldException(&mWorld, format("{} - y={} is out of bounds", caller, y));
		}

		if (levelsHigh > mWorld.mLevelsHigh - y)
		{
			throw WorldException(&mWorld, format("{} - levelsHigh={} is out of bounds", caller, levelsHigh));
		}
	}

	void PlacementValidator::validateLayerSpace(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh) const
	{
		auto layer = mWorld.getLayer(layerIndex);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				validateCellUnoccupied(caller, layerIndex, ix, iy);
			}
		}
	}

	void PlacementValidator::validateObjectAllowedInSector(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = mWorld.getSector(sectorIndex);

		if (!sector->sectorSupportsObjectType(type))
		{
			throw WorldException(&mWorld, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void PlacementValidator::validateObjectAllowedInSectorAsLookTarget(string const& caller, SectorObjectType type, uint32_t sectorIndex) const
	{
		auto sector = mWorld.getSector(sectorIndex);

		if (!sector->sectorSupportsObjectAsLookTarget(type))
		{
			throw WorldException(&mWorld, format("{} - Sector type '{}' does not support SectorObject type '{}'", caller, getSectorTypeString(sector->getType()), getSectorObjectTypeString(type)));
		}
	}

	void PlacementValidator::validateSpaceOnlyInOneSector(string const& caller, uint32_t layerIndex, uint32_t x, uint32_t y, uint32_t cellsWide, uint32_t levelsHigh, bool allowAllBackgroundSpan) const
	{
		auto layer = mWorld.getLayer(layerIndex);

		// A Window's back Layer is the one place where several Sectors behind one
		// aperture make sense: Backgrounds are seen, never entered, so a wide Window
		// looking across the seam between two of them still sees an unbroken view.
		// Half room-interior and half sky is a different matter - there should be a
		// wall where the room ends - so a mixed span is refused outright.
		if (allowAllBackgroundSpan)
		{
			bool seesBackground = false;
			bool seesOccupiedOther = false;
			for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
				for (uint32_t ix = x; ix < x + cellsWide; ++ix)
				{
					auto const& cellDef = layer->getCellDefinition(ix, iy);
					if (cellDef.sectorIndex == ~0u) continue;
					if (mWorld.mSectors[cellDef.sectorIndex]->getType() == SectorType::Background)
						seesBackground = true;
					else
						seesOccupiedOther = true;
				}
			if (seesBackground && !seesOccupiedOther) return;
			if (seesBackground && seesOccupiedOther)
				throw WorldException(&mWorld, format(
					"{} - bounds {},{} -> {},{} mix a Background with a Location or Transit on Layer {}; a Window cannot look half into a room and half into a Background, since there should be a wall where the room ends",
					caller, x, y, x + cellsWide, y + levelsHigh, layerIndex));
		}

		auto sectorIndex = layer->getCellDefinition(x, y).sectorIndex;

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
		{
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef = layer->getCellDefinition(ix, iy);

				if (cellDef.sectorIndex != sectorIndex)
				{
					throw WorldException(&mWorld, format("{} - bounds {},{} -> {},{} cross multiple Sectors", caller, x, y, x + cellsWide, y + levelsHigh));
				}
			}
		}
	}

	void PlacementValidator::validateSectorDoorOptions(string const& caller, CreateDoorOptions const& options) const
	{
		if (!Door::heightScaleIsValid(options.heightScale)
			|| (options.heightScale && options.height != Door::Height::Regular))
			throw WorldException(&mWorld, caller + ": Height scale requires a Regular Door and a finite value from 0.1 to 1.0");
		if (!Door::speedIsValid(options.speedOverride))
			throw WorldException(&mWorld, caller + ": Door speed must be finite and positive");
		if (!Door::brokenOpenPercentageIsValid(options.brokenOpenPercentage))
			throw WorldException(&mWorld, caller + ": Broken open percentage must be a finite value from 0 to 1");
		// A zero-width Door would cover no cell: its placement loop runs zero times,
		// leaving the Door's two Sectors unset for the queue configuration to
		// dereference. A crossing-lane count above the usable threshold width is not
		// buildable either, and until now it was only caught after the Door's
		// objects and traversal resource existed. Both are refused here, before any
		// object or resource exists (ticket #196).
		if (options.width == 0)
			throw WorldException(&mWorld, format("{} - a Door must be at least one cell wide", caller));
		if (options.crossingLanes > options.width)
			throw WorldException(&mWorld, format(
				"{} - Door crossing lane count {} exceeds the usable threshold width {}",
				caller, options.crossingLanes, options.width));
		if (options.height != Door::Height::Regular && options.height != Door::Height::Tall)
			throw WorldException(&mWorld, format("{} - unknown Door height", caller));
		// Finite as well as non-negative: NaN slips past every `< 0.0f` range
		// check and infinity is non-negative, and either would reach the
		// float-to-tick conversion of the Door's traversal resource (#198).
		if (!isFiniteTiming(options.holdOpenSeconds))
		{
			throw WorldException(&mWorld, format("{} - Door hold-open time must be finite and non-negative.", caller));
		}
		if (!mWorld.mDeserializingConstruction)
			for (auto const& requirement : options.controlPermissionRequirements)
				for (auto permission : requirement)
					if (!mWorld.lookupAccessPermission(permission))
						throw WorldException(&mWorld, format("{} - a Door control requirement references an unknown Access permission", caller));
	}

	void PlacementValidator::validateSectorDoorPlacement(string const& caller, uint32_t layerIndex, uint32_t y,
		uint32_t x, CreateDoorOptions const& options, bool controlsAreExternallyBound) const
	{
		auto const cellsWide = options.width;

		// A Door is authored on the front Layer of the pair it crosses.
		validateLayer(caller, layerIndex);
		if (layerIndex + 1 >= mWorld.getLayerCount())
		{
			throw WorldException(&mWorld,
				format("{} - a Door needs a Layer directly behind the Layer it is authored on", caller));
		}
		auto const backLayer = layerBehind(layerIndex);

		validateSectorDoorOptions(caller, options);
		if (!controlsAreExternallyBound && options.activationMode != DoorActivationMode::RemoteControlled
			&& (options.controls[0] || options.controls[1]))
		{
			throw WorldException(&mWorld,
				format("{} - physical controls require remote-controlled activation", caller));
		}
		constexpr uint32_t levelsHigh = 1;
		validateBounds(caller, x, y, cellsWide, levelsHigh);
		validateSpaceOnlyInOneSector(caller, layerIndex, x, y, cellsWide, levelsHigh);
		validateSpaceOnlyInOneSector(caller, backLayer, x, y, cellsWide, levelsHigh);

		auto const frontLayer = mWorld.getLayer(layerIndex);
		auto const behindLayer = mWorld.getLayer(backLayer);

		for (uint32_t iy = y; iy < y + levelsHigh; ++iy)
			for (uint32_t ix = x; ix < x + cellsWide; ++ix)
			{
				auto const& cellDef0 = frontLayer->getCellDefinition(ix, iy);
				auto const& cellDef1 = behindLayer->getCellDefinition(ix, iy);

				if (cellDef0.sectorIndex == ~0u)
				{
					throw WorldException(&mWorld, format("{} - front Layer cell at {},{} is not occupied.", caller, ix, iy));
				}

				if (options.height == Door::Height::Tall)
				{
					auto const room = dynamic_pointer_cast<const Location>(mWorld.getSector(cellDef0.sectorIndex));
					if (!room || room->getType() != SectorType::Location || room->isCorridor())
						throw WorldException(&mWorld, format("{} - a tall Door is only available in a Room", caller));
				}

				if (cellDef1.sectorIndex == ~0u)
				{
					throw WorldException(&mWorld, format("{} - back Layer cell at {},{} is not occupied.", caller, ix, iy));
				}

				if (cellDef0.hasObject() || !cellDef0.markers.empty())
				{
					throw WorldException(&mWorld, format(
						"{} - another object occupies front Layer cell at {},{}", caller, ix, iy));
				}

				// The levels above the threshold are the opening's headroom: a Walkway or
				// other floor there runs through the opening.
				if (iy != y && (cellDef0.floorType != CellFloorType::None
					|| cellDef1.floorType != CellFloorType::None))
				{
					throw WorldException(&mWorld, format(
						"{} - a Door cannot open through a Walkway above its threshold", caller));
				}

				validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef0.sectorIndex);
				validateObjectAllowedInSector(caller, SectorObjectType::Door, cellDef1.sectorIndex);
			}
		if (options.heightScale && (!isLocationLike(mWorld.getSector(frontLayer->getCellDefinition(x, y).sectorIndex)->getType())
			|| !isLocationLike(mWorld.getSector(behindLayer->getCellDefinition(x, y).sectorIndex)->getType())))
			throw WorldException(&mWorld, caller + ": Height scale is only available for ordinary Doors between Locations");
		for (auto layer : {layerIndex, backLayer})
			mWorld.validatePanelWallRectangle(mWorld.mLayers[layer]->getCellDefinition(x, y).sectorIndex,
				{float(x) + CORE_DOOR_X_INSET, float(y)},
				{float(x + cellsWide) - CORE_DOOR_X_INSET, float(y)
					+ Door::effectiveHeight(options.height, options.heightScale)});
		if (!controlsAreExternallyBound)
			for (uint32_t side = 0; side < 2; ++side)
			{
				if (!options.controls[side]) continue;
				auto layer = side == 0 ? layerIndex : backLayer;
				auto sector = mWorld.mSectors[mWorld.mLayers[layer]->getCellDefinition(x, y).sectorIndex];
				auto demand = mWorld.doorControlDemand(sector, x, y, cellsWide, side);
				(void)mWorld.planPhysicalControls(layer, sector->getIndex(), y, &demand);
			}
	}

	void PlacementValidator::validateSectorForceBridgeOptions(string const& caller, CreateForceBridgeOptions const& options) const
	{
		if (options.width == 0 || options.width > CORE_FORCEBRIDGE_MAX_SIZE)
			throw WorldException(&mWorld, format("{} - ForceBridge width must be [1,{}], not {}",
				caller, CORE_FORCEBRIDGE_MAX_SIZE, options.width));
		if (options.extensible && (options.controlCount < 1 || options.controlCount > 2))
			throw WorldException(&mWorld, format("{} - Physical control count must be [1,2] for a controlled ForceBridge, not {}", caller, options.controlCount));
		if (!options.extensible && (options.controlCount != 0 || !options.startExtended))
			throw WorldException(&mWorld, format("{} - A non-extensible ForceBridge must be permanently extended and have no controls", caller));
		for (size_t side = 0; side < 2; ++side)
		{
			bool const hasControl = options.extensible && (options.controlCount > 1
				|| (options.controlCount == 1 && static_cast<int>(side) == options.fromSide));
			if (!hasControl && !options.controlPermissionRequirements[side].empty())
				throw WorldException(&mWorld, format("{} - A Force Bridge permission requirement requires a control on that side", caller));
			set<AccessPermissionId> seen;
			for (auto permission : options.controlPermissionRequirements[side])
				if (!permission || permission.value > AccessPermission::Capacity
					|| (!mWorld.mDeserializingConstruction && !mWorld.lookupAccessPermission(permission))
					|| !seen.insert(permission).second)
					throw WorldException(&mWorld, format("{} - Invalid or duplicate Access permission requirement", caller));
		}
	}

	void PlacementValidator::validateSectorLadderOptions(string const& caller, CreateLadderOptions const& options) const
	{
		if (options.directionalBatchLimit == 0)
		{
			throw WorldException(&mWorld, format("{} - Ladder directional batch limit must be positive.", caller));
		}
		for (auto const& requirement : options.controlPermissionRequirements)
		{
			if (!options.extensible && !requirement.empty())
				throw WorldException(&mWorld, format("{} - A Ladder permission requirement requires an extensible Ladder", caller));
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || permission.value > AccessPermission::Capacity
					|| (!mWorld.mDeserializingConstruction && !mWorld.lookupAccessPermission(permission))
					|| !seen.insert(permission).second)
					throw WorldException(&mWorld, format("{} - Invalid or duplicate Access permission requirement", caller));
		}
	}

	void PlacementValidator::validateLiftOptions(string const& caller, CreateLiftOptions const& options) const
	{
		if (options.stopOffsets.size() > 1000 || (!options.stopShortNames.empty()
			&& (options.stopShortNames.size() != options.stopOffsets.size()
				|| !all_of(options.stopShortNames.begin(), options.stopShortNames.end(), World::isValidTransportStopShortName))))
			throw WorldException(&mWorld, caller + ": Stop short names require 1–3 characters A–Z or 0–9, parallel to at most 1000 Stops");
		for (float speed : options.doorSpeeds)
			if (!std::isfinite(speed) || speed < 0.0f) throw WorldException(&mWorld, caller + ": Invalid landing Door speed");
		if (options.cellsWide == 0)
		{
			throw WorldException(&mWorld, format("{} - Lift width must be positive.", caller));
		}
		if (options.stopOffsets.size() < 2)
		{
			throw WorldException(&mWorld, format("{} - Lift must have at least 2 stops.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
		{
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
			{
				throw WorldException(&mWorld, format("{} - Lift stop offsets must be strictly increasing; stop {} ({}) is not above stop {} ({}).",
					caller, i, options.stopOffsets[i], i - 1, options.stopOffsets[i - 1]));
			}
		}
		if (options.capacity == 0)
		{
			throw WorldException(&mWorld, format("{} - Lift capacity must be positive.", caller));
		}
		if (options.initialStop >= options.stopOffsets.size())
		{
			throw WorldException(&mWorld, format("{} - Lift initial stop is out of range.", caller));
		}
		auto representablePositions = (uint32_t)floor((float)options.cellsWide / CORE_RESOURCE_SLOT_WIDTH);
		if (options.capacity > representablePositions)
		{
			throw WorldException(&mWorld, format("{} - Lift capacity {} exceeds {} representable interior standing positions.",
				caller, options.capacity, representablePositions));
		}
		// Finite as well as ordered: a NaN dwell or boarding window makes every
		// comparison false, and positive infinity stays non-negative (#198).
		if (!isFiniteTiming(options.minimumDwellSeconds)
			|| !isFiniteTiming(options.maximumBoardingSeconds)
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
		{
			throw WorldException(&mWorld, format("{} - Lift timing requires finite values with 0 <= minimum dwell <= maximum boarding time.", caller));
		}
		if (!options.landingControlPermissionRequirements.empty()
			&& options.landingControlPermissionRequirements.size() != options.stopOffsets.size())
			throw WorldException(&mWorld, format("{} - Lift landing requirements must be parallel to its stops.", caller));
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || !seen.insert(permission).second
					|| (!mWorld.mDeserializingConstruction && !mWorld.lookupAccessPermission(permission)))
					throw WorldException(&mWorld, format("{} - Invalid or duplicate Lift landing Access permission requirement.", caller));
		}
	}

	void PlacementValidator::validateShuttleOptions(string const& caller, CreateShuttleOptions const& options) const
	{
		if (options.stopOffsets.size() > 1000 || (!options.stopShortNames.empty()
			&& (options.stopShortNames.size() != options.stopOffsets.size()
				|| !all_of(options.stopShortNames.begin(), options.stopShortNames.end(), World::isValidTransportStopShortName))))
			throw WorldException(&mWorld, caller + ": Stop short names require 1–3 characters A–Z or 0–9, parallel to at most 1000 Stops");
		for (float speed : options.doorSpeeds)
			if (!std::isfinite(speed) || speed < 0.0f) throw WorldException(&mWorld, caller + ": Invalid landing Door speed");
		if (options.numCars == 0)
			throw WorldException(&mWorld, format("{} - Shuttle must have at least one carriage.", caller));
		if (options.carWidth < 3 || options.carWidth > 5)
		{
			throw WorldException(&mWorld, format("{} - Shuttle car width must be between 3 and 5.", caller));
		}
		if (options.doorMask == 0 || (options.doorMask >> options.carWidth) != 0)
		{
			throw WorldException(&mWorld, format("{} - Shuttle carriage door layout must select at least one cell and remain within the carriage width.", caller));
		}

		if (options.stopOffsets.size() < 2)
			throw WorldException(&mWorld, format("{} - Shuttle must have at least two stops.", caller));
		if (options.initialStop >= (uint32_t)options.stopOffsets.size())
		{
			throw WorldException(&mWorld, format("{} - Shuttle initialStop parameter out of bounds.", caller));
		}
		for (size_t i = 1; i < options.stopOffsets.size(); ++i)
			if (options.stopOffsets[i] <= options.stopOffsets[i - 1])
				throw WorldException(&mWorld, format("{} - Shuttle stop offsets must be strictly increasing.", caller));
		auto const representablePositions = maximumShuttleCarriageCapacity(options.carWidth);
		if (options.capacity == 0 || options.capacity > representablePositions)
			throw WorldException(&mWorld, format("{} - Shuttle capacity cannot be represented by buffered carriage standing positions.", caller));
		// Finite as well as ordered: a NaN dwell or boarding window makes every
		// comparison false, and positive infinity stays non-negative (#198).
		if (!isFiniteTiming(options.minimumDwellSeconds)
			|| !isFiniteTiming(options.maximumBoardingSeconds)
			|| options.maximumBoardingSeconds < options.minimumDwellSeconds)
			throw WorldException(&mWorld, format("{} - Shuttle timing requires finite values with 0 <= minimum dwell <= maximum boarding time.", caller));
		auto const landingSlots = options.stopOffsets.size() * options.numCars
			* SimulationCoordinator::shuttleDoorOffsets(options.carWidth, options.doorMask).size();
		if (!options.landingControlPermissionRequirements.empty()
			&& options.landingControlPermissionRequirements.size() != landingSlots)
			throw WorldException(&mWorld, format("{} - Shuttle landing requirements must use its fixed landing grid.", caller));
		for (auto const& requirement : options.landingControlPermissionRequirements)
		{
			set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!permission || !seen.insert(permission).second
					|| (!mWorld.mDeserializingConstruction && !mWorld.lookupAccessPermission(permission)))
					throw WorldException(&mWorld, format("{} - Invalid or duplicate Shuttle landing Access permission requirement.", caller));
		}
	}

	bool PlacementValidator::validateStaircaseEndpoint(uint32_t layerIndex, uint32_t x, uint32_t y, bool upperEndpoint,
		int /*riseSide*/, string& diagnostic) const
	{
		auto const& cell = mWorld.mLayers[layerIndex]->getCellDefinition(x, y);
		if (!cell.occupied())
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		auto location = dynamic_pointer_cast<const Location>(mWorld.mSectors[cell.sectorIndex]);
		if (!location)
		{
			diagnostic = format("A landing Location is required at {},{}", x, y);
			return false;
		}
		// Lower landings and Corridor landings require ordinary walkable floor.
		// An upper Room landing may instead terminate at an open side wall whose
		// adjacent Location supplies the walkable landing floor.
		if (!upperEndpoint || location->isCorridor())
		{
			if (cell.isTraversableOnFoot()) return true;
			diagnostic = format("Traversable landing floor is required at {},{}", x, y);
			return false;
		}

		auto const roomLevel = y - location->getCellY();
		auto openLandingAt = [&](int wallSide)
		{
			uint32_t const boundaryX = wallSide == CORE_SIDE_LEFT
				? location->getCellX0() : location->getCellX1();
			if (x != boundaryX || location->getEndType(roomLevel, wallSide) != SectorEndType::None)
				return false;
			int const adjacentX = wallSide == CORE_SIDE_LEFT ? (int)x - 1 : (int)x + 1;
			if (adjacentX < 0 || adjacentX >= (int)mWorld.mCellsWide) return false;
			auto const& adjacent = mWorld.mLayers[layerIndex]
				->getCellDefinition((uint32_t)adjacentX, y);
			if (!adjacent.occupied() || !adjacent.isTraversableOnFoot()) return false;
			auto adjacentLocation = dynamic_pointer_cast<const Location>(mWorld.mSectors[adjacent.sectorIndex]);
			if (!adjacentLocation || y < adjacentLocation->getCellY()) return false;
			auto const adjacentLevel = y - adjacentLocation->getCellY();
			return adjacentLevel < adjacentLocation->getLevelsHigh()
				&& adjacentLocation->getEndType(adjacentLevel, 1 - wallSide) == SectorEndType::None;
		};
		if (openLandingAt(CORE_SIDE_LEFT) || openLandingAt(CORE_SIDE_RIGHT)) return true;
		diagnostic = format("The upper Staircase endpoint at {},{} must meet an open Room wall with adjacent traversable floor", x, y);
		return false;
	}

	void PlacementValidator::validateTraversalTopology(Graph const& graph) const
	{
		auto validSector = [&](SectorId id) { return id && id.value <= mWorld.mSectors.size(); };
		auto require = [&](bool condition, string const& diagnostic)
		{
			if (!condition) throw WorldException(&mWorld, diagnostic);
		};
		require(mWorld.mTraversalRequests.entries().empty() && mWorld.mTraversalPermits.entries().empty(),
			"Traversal requests and permits must be drained before topology replacement");

		for (auto const& edge : graph.getEdges())
		{
			auto id = edge->getTraversalResourceId();
			bool requiresAuthority = edge->getType() == EdgeType::Door
				|| edge->getType() == EdgeType::BulkheadDoor || edge->getType() == EdgeType::Window
				|| edge->getType() == EdgeType::ForceBridge || edge->getType() == EdgeType::Ladder
				|| edge->getType() == EdgeType::LadderMount || edge->getType() == EdgeType::Lift
				|| edge->getType() == EdgeType::LiftMount || edge->getType() == EdgeType::Shuttle
				|| edge->getType() == EdgeType::ShuttleMount;
			require(!requiresAuthority || id,
				format("Edge {} ({}) has no traversal authority", edge->getId(), edge->getDescription()));
			if (!id) continue; // Explicit immediate-permit policy.
			auto resource = mWorld.mTraversalResources.find(id);
			require(resource != nullptr, format("Edge {} references removed traversal resource {}",
				edge->getId(), id.value));
			bool compatible = edge->getType() == EdgeType::Door || edge->getType() == EdgeType::BulkheadDoor
				? resource->mDoor != nullptr || resource->mAirlock != nullptr || resource->mSecurityScanner != nullptr
				: edge->getType() == EdgeType::Window ? resource->mWindow != nullptr
				: edge->getType() == EdgeType::ForceBridge ? resource->mForceBridge != nullptr
				: edge->getType() == EdgeType::Ladder || edge->getType() == EdgeType::LadderMount
					? resource->mLadder != nullptr
				: edge->getType() == EdgeType::Stairwell || edge->getType() == EdgeType::StairwellMount
					? resource->mStairwell != nullptr
				: edge->getType() == EdgeType::Lift || edge->getType() == EdgeType::LiftMount
					? resource->mLift != nullptr
				: edge->getType() == EdgeType::Shuttle || edge->getType() == EdgeType::ShuttleMount
					? resource->mShuttle != nullptr : true;
			require(compatible, format("Edge {} references an incompatible traversal resource {}",
				edge->getId(), id.value));
		}

		for (auto const& [id, resourcePtr] : mWorld.mTraversalResources.entries())
		{
			auto const& resource = *resourcePtr;
			if (resource.mDoor || resource.mForceBridge)
			{
				if (resource.mDoor) require(resource.mDoor->getTraversalResourceId() == id,
					format("Door resource {} is not the door's sole configured authority", id.value));
				auto const maximumCrossingLanes = resource.mDoor
					? resource.mDoor->getCellsWide() : 1u;
				require(!resource.mCrossingOwners.empty()
					&& resource.mCrossingOwners.size() <= maximumCrossingLanes,
					format("Queued crossing resource {} has invalid crossing-lane geometry", id.value));
				set<SectorId> approachSectors;
				for (auto const& lane : resource.mQueueLanes)
				{
					if (!lane.sector) continue;
					require(validSector(lane.sector)
						&& (!resource.mDoor || approachSectors.insert(lane.sector).second),
						format("Queued crossing resource {} has invalid approach sectors", id.value));
					require(lane.positions.size() == lane.positionOwners.size() && !lane.positions.empty(),
						format("Queued crossing resource {} has invalid queue-position storage", id.value));
					auto sector = mWorld.mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& position.x - CORE_RESOURCE_SLOT_WIDTH * 0.5f >= sector->getCellX0() - 0.001f
							&& position.x + CORE_RESOURCE_SLOT_WIDTH * 0.5f <= sector->getCellX1() + 1.001f
							&& position.y >= sector->getCellY0() - 0.001f
							&& position.y + CORE_RESOURCE_SLOT_STANDING_HEIGHT <= sector->getCellY1() + 1.001f,
							format("Queued crossing resource {} has a queue position outside its approach sector", id.value));
				}
			}
			if (resource.mOpenPlatformLift)
			{
				require(resource.mQueueLanes.size() == resource.mLiftStops.size(),
					format("Platform-lift resource {} does not have one queue lane per stop", id.value));
				for (uint32_t stop = 0; stop < resource.mQueueLanes.size(); ++stop)
				{
					auto const& lane = resource.mQueueLanes[stop];
					require(lane.sector == resource.mLiftSector && validSector(lane.sector)
						&& lane.positions.size() == lane.positionOwners.size()
						&& !lane.positions.empty(),
						format("Platform-lift resource {} has invalid queue geometry at stop {}",
							id.value, stop));
					auto sector = mWorld.mSectors[(size_t)lane.sector.value - 1];
					for (auto const& position : lane.positions)
						require(isfinite(position.x) && isfinite(position.y)
							&& abs(position.y - resource.mLiftStops[stop].globalPosition) <= 0.001f
							&& position.x - CORE_RESOURCE_SLOT_WIDTH * 0.5f
								>= sector->getCellX0() - 0.001f
							&& position.x + CORE_RESOURCE_SLOT_WIDTH * 0.5f
								<= sector->getCellX1() + 1.001f,
							format("Platform-lift resource {} has an invalid queue position at stop {}",
								id.value, stop));
				}
			}
			if (resource.mWindow)
				require(resource.mWindow->getTraversalResourceId() == id,
					format("Window resource {} is not the window's configured authority", id.value));
			if (resource.mLadder)
				require(resource.mLadder->getTraversalResourceId() == id,
					format("Ladder resource {} is not the ladder's configured authority", id.value));
			if (resource.mStairwell)
				require(resource.mStairwell->getTraversalResourceId() == id,
					format("Stairwell resource {} is not the stairwell's configured authority", id.value));
			if (resource.mForceBridge)
				require(resource.mForceBridge->getTraversalResourceId() == id,
					format("Force-bridge resource {} is not the bridge's configured authority", id.value));
			if (resource.mLift)
				require(resource.mLift->getTraversalResourceId() == id,
					format("Lift resource {} is not the lift's configured authority", id.value));
			if (resource.mShuttle)
				require(resource.mShuttle->getTraversalResourceId() == id,
					format("Shuttle resource {} is not the shuttle's configured authority", id.value));

			if (resource.mCapacity)
			{
				require(resource.mCapacityPositions.size() == resource.mCapacity
					&& resource.mOccupants.size() == resource.mCapacity
					&& resource.mAdmissionReservations.size() == resource.mCapacity,
					format("Traversal resource {} has inconsistent capacity positions", id.value));
				for (uint32_t i = 0; i < resource.mCapacityPositions.size(); ++i)
				{
					auto const& position = resource.mCapacityPositions[i];
					require(isfinite(position.x) && isfinite(position.y),
						format("Traversal resource {} has a non-finite capacity position", id.value));
					require(!resource.mOccupants[i] || mWorld.mAgents.find(resource.mOccupants[i]),
						format("Traversal resource {} contains a removed manifest occupant", id.value));
					require(!resource.mAdmissionReservations[i]
						|| mWorld.mTraversalRequests.find(resource.mAdmissionReservations[i]),
						format("Traversal resource {} contains a stale admission reservation", id.value));
					for (uint32_t j = 0; j < i; ++j)
						require(position.distanceTo(resource.mCapacityPositions[j]) > 0.001f,
							format("Traversal resource {} has overlapping capacity positions", id.value));
				}
			}
			for (auto requestId : resource.mAdmissionQueue)
				require(mWorld.mTraversalRequests.find(requestId) != nullptr,
					format("Traversal resource {} contains a stale admission queue entry", id.value));
			for (auto owner : resource.mVirtualBoundaryOwners)
				require(!owner || mWorld.mTraversalRequests.find(owner),
					format("Traversal resource {} contains a stale boundary owner", id.value));
			for (auto const& [leaseId, lease] : resource.mOpenLeases)
			{
				(void)leaseId;
				require(lease.kind == DoorOpenLeaseKind::ExternalHoldOpen || (lease.request
					&& mWorld.mTraversalRequests.find(lease.request)),
					format("Traversal resource {} contains a stale open lease", id.value));
			}

			for (auto controlId : resource.mControls)
			{
				auto control = mWorld.mInteractionPoints.find(controlId);
				require(control != nullptr, format("Traversal resource {} references removed control {}",
					id.value, controlId.value));
				auto expected = resource.mLiftCoordinator ? resource.mLiftCoordinator : id;
				require(any_of(control->mBindings.begin(), control->mBindings.end(), [&](auto const& binding)
					{ return binding.command.traversalResource == expected; }),
					format("Control {} does not target traversal resource {}", controlId.value, expected.value));
				if (resource.mDoor)
					require(any_of(resource.mQueueLanes.begin(), resource.mQueueLanes.end(),
						[&](auto const& lane) { return lane.sector == control->mSector; }),
						format("Control {} is unreachable from resource {} approaches", controlId.value, id.value));
				if (resource.mLift || resource.mShuttle)
					require(control->mSector == resource.mLiftSector,
						format("Transport selector {} is outside resource {}", controlId.value, id.value));
			}

			if (resource.mLift || resource.mShuttle)
			{
				require(resource.mLiftStops.size() >= 2 && validSector(resource.mLiftSector),
					format("Transport resource {} has invalid stops or transit sector", id.value));
				for (uint32_t stop = 0; stop < resource.mLiftStops.size(); ++stop)
				{
					auto const& value = resource.mLiftStops[stop];
					require(validSector(value.locationSector) && isfinite(value.globalPosition)
						&& (stop == 0 || value.globalPosition > resource.mLiftStops[stop - 1].globalPosition),
						format("Transport resource {} has invalid stop {} geometry", id.value, stop));
					if (!resource.mOpenPlatformLift)
					{
						auto landing = mWorld.mTraversalResources.find(value.landingResource);
						require(landing && landing->mDoor && landing->mLiftCoordinator == id
							&& landing->mLiftStopIndex == stop,
							format("Transport resource {} has invalid landing-door mapping at stop {}", id.value, stop));
					}
					auto call = mWorld.mInteractionPoints.find(value.callControl);
					require(call && call->mSector == value.locationSector
						&& any_of(call->mBindings.begin(), call->mBindings.end(), [&](auto const& binding)
							{ return binding.command.traversalResource == id
								&& binding.command.stopIndex == stop; }),
						format("Transport resource {} has an invalid landing control at stop {}", id.value, stop));
				}
				if (resource.mShuttle)
					for (auto const& door : resource.mShuttleDoors)
					{
						auto landing = mWorld.mTraversalResources.find(door.landingResource);
						require(door.stopIndex < resource.mLiftStops.size()
							&& door.carriageIndex < resource.mShuttleCarriages.size()
							&& validSector(door.locationSector) && landing && landing->mDoor
							&& landing->mLiftCoordinator == id && landing->mLiftStopIndex == door.stopIndex,
							format("Shuttle resource {} has an invalid carriage-door mapping", id.value));
					}
			}
		}

		for (auto const& [pointId, point] : mWorld.mInteractionPoints.entries())
		{
			require(validSector(point->mSector),
				format("Interaction point {} has an invalid sector", pointId.value));
			for (auto const& binding : point->mBindings)
				if (binding.command.type == DeviceCommandType::RequestAirlock)
					require(validSector(binding.command.target)
						&& mWorld.mSectors[binding.command.target.value - 1]->getType() == SectorType::Airlock
						&& binding.command.stopIndex < 2,
						format("Interaction point {} targets a removed Airlock or invalid entry side", pointId.value));
				else if (binding.command.type == DeviceCommandType::ToggleBoothWindow
					|| binding.command.type == DeviceCommandType::SetBoothWindowState)
					require(bool(mWorld.lookupBoothWindow(binding.command.boothWindow)),
						format("Interaction point {} targets a removed BoothWindow", pointId.value));
				else if (binding.command.type == DeviceCommandType::PressDumbwaiterLanding)
					require(mWorld.lookupDumbwaiter(binding.command.dumbwaiter) && binding.command.stopIndex < 2,
						format("Interaction point {} targets a removed Dumbwaiter", pointId.value));
				else if (binding.command.type == DeviceCommandType::SetAccessPanelState)
					require(bool(mWorld.lookupAccessPanel(binding.command.accessPanel)),
						format("Interaction point {} targets a removed Access panel", pointId.value));
				else if (binding.command.type != DeviceCommandType::SetSectorLights)
					require(mWorld.mTraversalResources.find(binding.command.traversalResource) != nullptr,
						format("Interaction point {} targets removed traversal resource {}",
							pointId.value, binding.command.traversalResource.value));
		}
	}

	void PlacementValidator::validateAgentLocationPlacement(Sector const& sector, Agent const& agent,
		std::optional<float> floorY) const
	{
		mWorld.validateAgentLocationPermissions(sector, mWorld.effectiveAccessGrants(agent), agent.getName());
		if (!agent.requiredPoseFor(sector, floorY))
			throw invalid_argument(format("Agent '{}' has no fitting supported movement pose in '{}'",
				agent.getName(), sector.getName()));
	}
}
