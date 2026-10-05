#include "core/World.h"
#include "core/Exceptions.h"
#include <algorithm>

namespace core
{
	bool World::validateAccessPanel(uint32_t index, uint32_t level, uint32_t x,
		AccessPanelGeometry geometry, uint32_t ignored, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = std::move(message); return false; };
		if (!AccessPanel::geometryIsValid(geometry)) return reject("Access panel dimensions must be finite in [0,1], with Y offset + height <= 1");
		if (index >= mSectors.size() || !mSectors[index] || !isLocationLike(mSectors[index]->getType()))
			return reject("Access panels require a Room, Corridor, or Facade");
		auto sector = mSectors[index];
		if (level >= sector->getLevelsHigh() || x < sector->getCellX() || x > sector->getCellX1())
			return reject("Access panel cell is outside its Location");
		auto y = sector->getCellY() + level;
		auto const& cell = mLayers[sector->getLayerIndex()]->getCellDefinition(x, y);
		if (cell.sectorIndex != index || (cell.floorType != CellFloorType::Ground && cell.floorType != CellFloorType::Walkway))
			return reject("Access panels require walkable Floor or Walkway support");
		if (cell.accessPanel != ~0u && cell.accessPanel != ignored)
			return reject("The cell already owns an Access panel");
		AccessPanel proposed(x, y, level, geometry);
		Vector2 min, max; proposed.getFullShape(min, max);
		if (geometry.width == 0 || geometry.height == 0) return true;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			if (i == ignored) continue;
			auto object = sector->getObject(i);
			if (!object) continue;
			auto type = object->getObjectType();
			if (type != SectorObjectType::InteractionPoint && type != SectorObjectType::Door
				&& type != SectorObjectType::BulkheadDoor && !isWindowAperture(type)
				&& type != SectorObjectType::AccessPanel) continue;
			Vector2 otherMin, otherMax;
			object->_getObject()->getFullShape(otherMin, otherMax);
			if (otherMin.x < otherMax.x && otherMin.y < otherMax.y
				&& min.x < otherMax.x && max.x > otherMin.x && min.y < otherMax.y && max.y > otherMin.y)
				return reject("Access panel overlaps a fixed wall object");
		}
		return true;
	}

	bool World::canAddAccessPanel(uint32_t sector, uint32_t level, uint32_t x,
		AccessPanelGeometry geometry, std::string* diagnostic) const
	{
		return validateAccessPanel(sector, level, x, geometry, ~0u, diagnostic);
	}

	World::CreateObjectResult World::addAccessPanel(uint32_t index, uint32_t level,
		uint32_t x, AccessPanelGeometry geometry)
	{
		std::string diagnostic;
		if (!canAddAccessPanel(index, level, x, geometry, &diagnostic)) throw WorldException(this, diagnostic);
		beginStructuralEdit("addAccessPanel");
		auto sector = mSectors[index];
		auto y = sector->getCellY() + level;
		auto object = std::make_shared<AccessPanelSectorObject>(sector, x, y, level, geometry);
		auto objectIndex = sector->addSectorObject(object);
		mLayers[sector->getLayerIndex()]->getCellDefinition(x, y).accessPanel = objectIndex;
		ConstructionRecord record{ConstructionType::AccessPanel};
		record.a = index; record.b = level; record.c = x - sector->getCellX();
		record.x = geometry.width; record.y = geometry.height; record.z = geometry.yOffset;
		recordConstruction(record);
		return {objectIndex, SectorObjectType::AccessPanel, sector};
	}

	bool World::configureAccessPanel(uint32_t index, uint32_t objectIndex,
		AccessPanelGeometry geometry, std::string* diagnostic)
	{
		if (index >= mSectors.size() || objectIndex >= mSectors[index]->getNumObjects()) return false;
		auto object = std::dynamic_pointer_cast<AccessPanelSectorObject>(mSectors[index]->getObject(objectIndex));
		if (!object) return false;
		auto panel = std::static_pointer_cast<AccessPanel>(object->_getObject());
		if (!validateAccessPanel(index, panel->getLevelOffset(), object->getCellX(), geometry, objectIndex, diagnostic)) return false;
		if (panel->getGeometry() == geometry) return false;
		if (mBuildFinished && !mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause simulation before editing Access panels";
			return false;
		}
		// Geometry does not alter the floor approach or traversal topology.
		panel->configure(object->getCellY(), geometry);
		ConstructionRecord record{ConstructionType::ConfigureAccessPanel};
		record.a = index; record.b = panel->getLevelOffset(); record.c = object->getCellX() - mSectors[index]->getCellX();
		record.x = geometry.width; record.y = geometry.height; record.z = geometry.yOffset;
		recordConstruction(record); modify();
		return true;
	}

	bool World::removeAccessPanel(uint32_t index, uint32_t objectIndex)
	{
		if (index >= mSectors.size() || objectIndex >= mSectors[index]->getNumObjects()) return false;
		auto sector = mSectors[index];
		auto object = std::dynamic_pointer_cast<AccessPanelSectorObject>(sector->getObject(objectIndex));
		if (!object) return false;
		beginStructuralEdit("removeAccessPanel");
		mLayers[sector->getLayerIndex()]->getCellDefinition(object->getCellX(), object->getCellY()).accessPanel = ~0u;
		sector->removeSectorObject(objectIndex);
		ConstructionRecord record{ConstructionType::RemoveAccessPanel};
		record.a = index; record.b = object->getPanel()->getLevelOffset(); record.c = object->getCellX() - sector->getCellX();
		recordConstruction(record);
		if (!mDeserializingConstruction) finishBuild();
		return true;
	}
}
