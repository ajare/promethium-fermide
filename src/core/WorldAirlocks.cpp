#include <cmath>

#include "core/World.h"
#include "core/AirlockTransit.h"
#include "core/Exceptions.h"
#include "core/Button.h"
#include "core/Agent.h"

namespace core
{
	bool World::canAddAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width,
		float seconds, std::string* diagnostic) const
	{
		auto reject = [&](std::string message) {
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (!std::isfinite(seconds) || seconds < 1.0f || seconds > 10.0f)
			return reject("Airlock cycle duration must be finite and between 1 and 10 seconds");
		if (layer >= getLayerCount() || y >= getLevelsHigh() || width == 0
			|| x == 0 || (uint64_t)x + width >= getCellsWide())
			return reject("Airlock requires a positive whole-cell width and two adjoining ends on one Level");
		auto grid = getLayer(layer);
		for (uint32_t cell = x; cell < x + width; ++cell)
			if (grid->getCellDefinition(cell, y).occupied())
				return reject("Airlock chamber cells must be empty; existing Locations are never carved");
		for (int side = 0; side < 2; ++side)
		{
			auto endX = side == CORE_SIDE_LEFT ? x - 1 : x + width;
			auto const& cell = grid->getCellDefinition(endX, y);
			if (!cell.occupied()) return reject("Each Airlock end must adjoin a Room or Corridor");
			auto sector = getSector(cell.sectorIndex);
			if (sector->getType() != SectorType::Location)
				return reject("Airlocks connect only Rooms and Corridors on their own Layer");
			if ((side == CORE_SIDE_LEFT && sector->getCellX1() != endX)
				|| (side == CORE_SIDE_RIGHT && sector->getCellX0() != endX)
				|| !cell.isTraversableOnFoot())
				return reject("Airlock neighbours must have adjoining walkable ends");
			auto end = sector->getEndType(y - sector->getCellY(), 1 - side);
			if (end == SectorEndType::BulkheadDoor || cell.bulkheadIndices[1 - side] != ~0u
				|| cell.hasObject())
				return reject("An object or control blocks the Airlock entrance");
		}
		try
		{
			std::vector<physicalControl::Demand> demands;
			for (int side = 0; side < CORE_NUM_SIDES; ++side)
			{
				auto endX = side == CORE_SIDE_LEFT ? x - 1 : x + width;
				demands.push_back(insetControlDemand(getSector(grid->getCellDefinition(endX, y).sectorIndex),
					physicalControl::OwnerType::Airlock, { layer, x, y, width, 1 }, y, side));
			}
			// The left threshold borders currently empty cells; only the right
			// threshold can block an existing offset-zero candidate.
			validatePhysicalControlAdditions(demands, x + width);
		}
		catch (Exception const& error) { return reject(error.getMessage()); }
		catch (std::exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint32_t World::addAirlock(uint32_t layer, uint32_t y, uint32_t x, uint32_t width, float seconds)
	{
		std::string diagnostic;
		if (!canAddAirlock(layer, y, x, width, seconds, &diagnostic))
			throw WorldException(this, diagnostic);
		beginStructuralEdit("addAirlock");
		auto grid = getLayer(layer);
		std::vector<TransitStop> stops;
		std::array<SectorEndType, 2> previous;
		for (int side = 0; side < 2; ++side)
		{
			auto endX = side == CORE_SIDE_LEFT ? x - 1 : x + width;
			auto neighbour = _getSector(grid->getCellDefinition(endX, y).sectorIndex);
			previous[side] = neighbour->getEndType(y - neighbour->getCellY(), 1 - side);
			stops.push_back({ neighbour, (int)(endX - neighbour->getCellX()),
				(int)(y - neighbour->getCellY()) });
		}
		auto index = (uint32_t)mSectors.size();
		auto chamber = std::make_shared<AirlockTransit>(index, layer, x, y, width, seconds, stops, previous);
		mSectors.push_back(chamber);
		auto resourceId = mTraversalResources.add(std::unique_ptr<TraversalResource>(new TraversalResource("Airlock journey")));
		auto resource = mTraversalResources.find(resourceId);
		resource->mAirlock = chamber;
		resource->mCapacity = width;
		resource->mOccupants.resize(width);
		resource->mAdmissionReservations.resize(width);
		resource->mCrossingOwners.resize(1);
		for (uint32_t cell = 0; cell < width; ++cell)
			resource->mCapacityPositions.push_back({ cell + 0.5f, 0.0f });
		chamber->mTraversalResource = resourceId;
		for (uint32_t cell = x; cell < x + width; ++cell)
			grid->getCellDefinition(cell, y).sectorIndex = index;
		for (int side = 0; side < 2; ++side)
		{
			auto thresholdX = side == CORE_SIDE_LEFT ? x : x + width;
			auto created = createBulkheadDoor(layer, thresholdX, y, CORE_SIDE_LEFT);
			grid->getCellDefinition(thresholdX - 1, y).bulkheadIndices[CORE_SIDE_RIGHT] = created.index;
			// The right-side lookup is only used by placement preflight.
			grid->getCellDefinition(thresholdX, y).bulkheadIndices[CORE_SIDE_LEFT] = created.index;
			auto object = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(created.sector->getObject(created.index));
			auto door = object->getDoor();
			door->configureTraversal(DoorActivationMode::Unavailable, {}, CORE_DOOR_STAY_OPEN_TIME);
			door->mAirlockOwned = true;
			chamber->mDoors[side] = door;
			// Both thresholds share the Airlock authority, never an ordinary Door resource.
			auto neighbour = _getSector(stops[side].sector->getIndex());
			neighbour->setEndType(y - neighbour->getCellY(), 1 - side, SectorEndType::None);
			auto& lane = resource->mQueueLanes[side];
			lane.sector = SectorId{ (uint64_t)neighbour->getIndex() + 1 };
			lane.origin = { side == CORE_SIDE_LEFT ? x - 0.3f : x + width + 0.3f, (float)y };
			for (uint32_t cell = 0; cell < neighbour->getCellsWide(); ++cell)
				lane.positions.push_back({ side == CORE_SIDE_LEFT ? x - 0.5f - cell : x + width + 0.5f + cell, (float)y });
			lane.positionOwners.resize(lane.positions.size());
			auto demand = insetControlDemand(neighbour, physicalControl::OwnerType::Airlock,
				{ layer, x, y, width, 1 }, y, side);
			auto control = createPhysicalControl("Airlock outside button", layer,
				y, demand, CORE_BUTTON_F_AUTO_REENABLE);
			DeviceCommand command;
			command.type = DeviceCommandType::RequestAirlock;
			command.target = SectorId{ (uint64_t)index + 1 }; command.stopIndex = side;
			command.traversalResource = resourceId;
			chamber->mControls[side] = createPhysicalControlInteractionPoint("Airlock outside button",
				control, (float)y, 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
		}
		// Older documents allocated a third InteractionPoint for the internal
		// button. Keep that ID unused so subsequent controls retain their saved
		// permission references, without retaining a button or interaction.
		if (!mInteractionPoints.exhausted())
			mInteractionPoints.restoreNextId(mInteractionPoints.nextId() + 1);
		resource->mControls.assign(chamber->mControls.begin(), chamber->mControls.end());
		ConstructionRecord record{ ConstructionType::Airlock };
		record.layer = layer; record.a = y; record.b = x; record.c = width; record.x = seconds;
		record.p = previous[0] == SectorEndType::None; record.q = previous[1] == SectorEndType::None;
		recordConstruction(std::move(record));
		return index;
	}

	bool World::canAgentEnterAirlock(TraversalResourceId id, SectorId approach,
		AgentId agentId, bool locallyObserved) const
	{
		auto resource = mTraversalResources.find(id);
		auto actor = mAgents.find(agentId);
		if (!resource || !resource->mAirlock || !actor) return false;
		auto const& chamber = *resource->mAirlock;
		int side = approach == resource->mQueueLanes[0].sector ? 0 : 1;
		if (approach != resource->mQueueLanes[side].sector
			|| !canAgentAccessLocation(*chamber.getStop(1 - side).sector, *actor)) return false;
		auto control = mInteractionPoints.find(chamber.getControl(side));
		if (!control) return false;
		if (missingInteractionPermissions(*control, *actor).empty()) return true;
		// Unauthorized operation is never allowed. Non-adhering Agents may use
		// a locally usable entrance, but the allocator still owns every permit.
		return locallyObserved && !actor->getEffectivePermissionAdherence().value
			&& chamber.mActiveSide == side && !chamber.mClosing
			&& chamber.mDoors[side]->isOpen() && chamber.mDoors[1 - side]->isClosed()
			&& chamber.isCycleComplete() && resource->mAirlockEntrySide == side;
	}

	bool World::setAirlockCycleSeconds(uint32_t index, float seconds)
	{
		if (!mSimulationPaused || !std::isfinite(seconds) || seconds < 1 || seconds > 10
			|| index >= mSectors.size()) return false;
		auto chamber = std::dynamic_pointer_cast<AirlockTransit>(mSectors[index]);
		if (!chamber) return false;
		for (auto& record : mConstructionRecords)
			if (record.type == ConstructionType::Airlock && record.layer == chamber->getLayerIndex()
				&& record.a == chamber->getCellY() && record.b == chamber->getCellX())
			{
				record.x = seconds; chamber->mCycleSeconds = seconds; markModified();
				invalidateSimulationSnapshot(); return true;
			}
		return false;
	}

	bool World::isAirlockOwnedObject(std::shared_ptr<const SectorObject> const& object) const
	{
		if (!object) return false;
		if (auto bulkhead = std::dynamic_pointer_cast<const BulkheadDoorSectorObject>(object))
			return bulkhead->getDoor()->isAirlockOwned();
		if (auto button = std::dynamic_pointer_cast<Button>(object->_getObject()))
			for (auto const& sector : mSectors)
				if (auto chamber = std::dynamic_pointer_cast<AirlockTransit>(sector))
					for (auto point : chamber->mControls)
						if (point && point == button->getInteractionPointId()) return true;
		return false;
	}
}
