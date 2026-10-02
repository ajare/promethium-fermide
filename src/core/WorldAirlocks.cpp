#include <cmath>

#include "core/World.h"
#include "core/AirlockTransit.h"
#include "core/Exceptions.h"
#include "core/Button.h"

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
				|| cell.hasObject() || cell.controls[1 - side] != ~0u)
				return reject("An object or control blocks the Airlock entrance");
		}
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
		for (uint32_t cell = x; cell < x + width; ++cell)
			grid->getCellDefinition(cell, y).sectorIndex = index;
		for (int side = 0; side < 2; ++side)
		{
			auto thresholdX = side == CORE_SIDE_LEFT ? x : x + width;
			auto created = createBulkheadDoor(layer, thresholdX, y, CORE_SIDE_LEFT);
			auto object = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(created.sector->getObject(created.index));
			auto door = object->getDoor();
			door->configureTraversal(DoorActivationMode::Unavailable, {}, CORE_DOOR_STAY_OPEN_TIME);
			door->mAirlockOwned = true;
			chamber->mDoors[side] = door;
			// Rendered as owned Bulkheads, but never registered as ordinary traversal
			// resources or graph thresholds. The neighbour's authored wall is open.
			auto neighbour = _getSector(stops[side].sector->getIndex());
			neighbour->setEndType(y - neighbour->getCellY(), 1 - side, SectorEndType::None);
			auto control = createPhysicalControl("Airlock outside button", layer,
				side == CORE_SIDE_LEFT ? x - 1 : x + width, y, 1 - side, CORE_BUTTON_F_AUTO_REENABLE);
			DeviceCommand command;
			command.type = DeviceCommandType::RequestAirlock;
			command.target = SectorId{ (uint64_t)index + 1 }; command.stopIndex = side;
			chamber->mControls[side] = createPhysicalControlInteractionPoint("Airlock outside button",
				control, (float)y, 0.15f, getFixedTimestep(),
				{ { command, InteractionBindingRequirement::Required } });
		}
		auto internal = createPhysicalControl("Airlock internal button", layer, x + (width - 1) / 2,
			y, CORE_SIDE_MIDDLE, CORE_BUTTON_F_AUTO_REENABLE);
		DeviceCommand command;
		command.type = DeviceCommandType::RequestAirlock;
		command.target = SectorId{ (uint64_t)index + 1 }; command.stopIndex = 2;
		chamber->mControls[2] = createPhysicalControlInteractionPoint("Airlock internal button",
			internal, (float)y, 0.15f, getFixedTimestep(),
			{ { command, InteractionBindingRequirement::Required } });
		ConstructionRecord record{ ConstructionType::Airlock };
		record.layer = layer; record.a = y; record.b = x; record.c = width; record.x = seconds;
		record.p = previous[0] == SectorEndType::None; record.q = previous[1] == SectorEndType::None;
		recordConstruction(std::move(record));
		return index;
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
