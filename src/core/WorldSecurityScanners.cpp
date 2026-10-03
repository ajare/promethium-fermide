#include "core/World.h"
#include "core/SecurityScannerTransit.h"
#include "core/Exceptions.h"

namespace core
{
	bool World::canAddSecurityScanner(uint32_t layer, uint32_t y, uint32_t x,
		uint32_t width, std::string* diagnostic) const
	{
		// Same footprint and walkable Room/Corridor ends as an Airlock; no controls.
		std::string reason;
		bool valid = canAddAirlock(layer, y, x, width, 3.0f, &reason);
		for (size_t pos = 0; (pos = reason.find("Airlock", pos)) != std::string::npos;)
		{
			reason.replace(pos, 7, "Security scanner");
			pos += 16;
		}
		if (diagnostic) *diagnostic = std::move(reason);
		return valid;
	}

	uint32_t World::addSecurityScanner(uint32_t layer, uint32_t y, uint32_t x,
		uint32_t width, bool leftToRight)
	{
		std::string diagnostic;
		if (!canAddSecurityScanner(layer, y, x, width, &diagnostic))
			throw WorldException(this, diagnostic);
		beginStructuralEdit("addSecurityScanner");
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
		auto chamber = std::make_shared<SecurityScannerTransit>(index, layer, x, y,
			width, leftToRight, stops, previous);
		mSectors.push_back(chamber);
		auto resourceId = mTraversalResources.add(std::unique_ptr<TraversalResource>(new TraversalResource("Security scanner journey")));
		auto resource = mTraversalResources.find(resourceId);
		resource->mSecurityScanner = chamber;
		resource->mCapacity = 1;
		resource->mOccupants.resize(1);
		resource->mAdmissionReservations.resize(1);
		resource->mCrossingOwners.resize(1);
		resource->mCapacityPositions.push_back({ width * 0.5f, 0.0f });
		chamber->mTraversalResource = resourceId;
		for (uint32_t cell = x; cell < x + width; ++cell)
			grid->getCellDefinition(cell, y).sectorIndex = index;
		for (int side = 0; side < 2; ++side)
		{
			auto thresholdX = side == CORE_SIDE_LEFT ? x : x + width;
			auto created = createBulkheadDoor(layer, thresholdX, y, CORE_SIDE_LEFT);
			grid->getCellDefinition(thresholdX - 1, y).bulkheadIndices[CORE_SIDE_RIGHT] = created.index;
			grid->getCellDefinition(thresholdX, y).bulkheadIndices[CORE_SIDE_LEFT] = created.index;
			auto object = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(created.sector->getObject(created.index));
			auto door = object->getDoor();
			door->configureTraversal(DoorActivationMode::Unavailable, {}, CORE_DOOR_STAY_OPEN_TIME);
			door->setAutomaticSensorDistance(chamber->getSensorDistance());
			door->mSecurityScannerOwned = true;
			chamber->mDoors[side] = door;
			auto neighbour = _getSector(stops[side].sector->getIndex());
			neighbour->setEndType(y - neighbour->getCellY(), 1 - side, SectorEndType::None);
			auto& lane = resource->mQueueLanes[side];
			lane.sector = SectorId{ (uint64_t)neighbour->getIndex() + 1 };
			lane.origin = { side == CORE_SIDE_LEFT ? x - 0.3f : x + width + 0.3f, (float)y };
			for (uint32_t cell = 0; cell < neighbour->getCellsWide(); ++cell)
				lane.positions.push_back({ side == CORE_SIDE_LEFT ? x - 0.5f - cell : x + width + 0.5f + cell, (float)y });
			lane.positionOwners.resize(lane.positions.size());
		}
		ConstructionRecord record{ ConstructionType::SecurityScanner };
		record.layer = layer; record.a = y; record.b = x; record.c = width;
		record.d = leftToRight ? 1 : 0;
		record.x = chamber->getPreDelaySeconds(); record.y = chamber->getScanSeconds();
		record.z = chamber->getPostPauseSeconds();
		record.p = previous[0] == SectorEndType::None; record.q = previous[1] == SectorEndType::None;
		recordConstruction(std::move(record));
		return index;
	}

	bool World::setSecurityScannerConfiguration(uint32_t index, float sensor, float pre, float scan, float post)
	{
		if (!mSimulationPaused || !SecurityScannerTransit::validConfiguration(sensor, pre, scan, post)
			|| index >= mSectors.size()) return false;
		auto chamber = std::dynamic_pointer_cast<SecurityScannerTransit>(mSectors[index]);
		if (!chamber) return false;
		for (auto& record : mConstructionRecords)
			if (record.type == ConstructionType::SecurityScanner && record.layer == chamber->getLayerIndex()
				&& record.a == chamber->getCellY() && record.b == chamber->getCellX())
			{
				record.scannerSensorDistance = chamber->mSensorDistance = sensor;
				record.x = chamber->mPreDelaySeconds = pre;
				record.y = chamber->mScanSeconds = scan;
				record.z = chamber->mPostPauseSeconds = post;
				for (auto const& door : chamber->mDoors) door->setAutomaticSensorDistance(sensor);
				markModified(); invalidateSimulationSnapshot(); return true;
			}
		return false;
	}

	bool World::isChamberOwnedObject(std::shared_ptr<const SectorObject> const& object) const
	{
		if (auto bulkhead = std::dynamic_pointer_cast<const BulkheadDoorSectorObject>(object))
			return bulkhead->getDoor()->isChamberOwned();
		return isAirlockOwnedObject(object);
	}
}
