#include "core/World.h"
#include "core/Exceptions.h"
#include "core/MobilityProfile.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace core
{
	void World::reserveAccessPanelIdentitiesFrom(World const& previous)
	{
		if (!mSectors.empty() || mBuildFinished || previous.mNextAccessPanelId <= 1) return;
		mNextAccessPanelId = std::max(mNextAccessPanelId, previous.mNextAccessPanelId);
		mInteractionPoints.restoreNextId(previous.mInteractionPoints.nextId());
		mInteractionRequests.restoreNextId(previous.mInteractionRequests.nextId());
		mDeviceOperations.restoreNextId(previous.mDeviceOperations.nextId());
	}

	std::shared_ptr<const AccessPanel> World::lookupAccessPanel(AccessPanelId id) const
	{
		auto found = mAccessPanels.find(id);
		return found == mAccessPanels.end() ? nullptr : found->second.lock();
	}

	bool World::canRequestAccessPanel(AccessPanelId id, AccessPanel::Action action, AgentId actorId) const
	{
		auto panel = lookupAccessPanel(id);
		auto actor = mAgents.find(actorId);
		if (!panel || !actor || !actor->isActive() || agentForbidsButtons(actor)) return false;
		auto actions = panel->getActions();
		if (std::find(actions.begin(), actions.end(), action) == actions.end()) return false;
		auto point = mInteractionPoints.find(panel->getControl(action));
		return point && actor->getSector() == mSectors[point->getSector().value - 1].get()
			&& std::abs(actor->getGlobalPosition().y - point->getPosition().y) < 0.001f
			&& (actor->getState() == Agent::State::Idle || actor->getState() == Agent::State::WaitingForTraversal);
	}

	InteractionRequestId World::requestAccessPanel(AccessPanelId id, AccessPanel::Action action, AgentId actor)
	{
		if (!canRequestAccessPanel(id, action, actor)) return {};
		return requestInteraction(lookupAccessPanel(id)->getControl(action), actor);
	}

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

	void World::validatePanelWallRectangle(uint32_t sectorIndex, Vector2 min, Vector2 max) const
	{
		if (min.x >= max.x || min.y >= max.y) return;
		auto sector = mSectors[sectorIndex];
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = std::dynamic_pointer_cast<const AccessPanelSectorObject>(sector->getObject(i));
			if (!object) continue;
			Vector2 panelMin, panelMax; object->getPanel()->getFullShape(panelMin, panelMax);
			if (panelMin.x < panelMax.x && panelMin.y < panelMax.y
				&& min.x < panelMax.x && max.x > panelMin.x && min.y < panelMax.y && max.y > panelMin.y)
				throw WorldException(this, "Fixed wall object overlaps an Access panel");
		}
	}

	void World::retireRemovedAccessPanelRecords(std::vector<ConstructionRecord>& records)
	{
		// Historical removed panels impose no support/geometry constraint on an
		// edit. Keep their object slots so subsequent authored indices stay valid.
		std::map<std::tuple<uint32_t,uint32_t,uint32_t>, std::vector<size_t>> lifetimes;
		std::vector<bool> discard(records.size());
		for (size_t i = 0; i < records.size(); ++i)
		{
			auto& record = records[i];
			if (record.type != ConstructionType::AccessPanel && record.type != ConstructionType::ConfigureAccessPanel
				&& record.type != ConstructionType::RemoveAccessPanel) continue;
			auto key = std::tuple{record.a,record.b,record.c};
			if (record.type == ConstructionType::AccessPanel) lifetimes[key] = {i};
			else if (record.type == ConstructionType::ConfigureAccessPanel) lifetimes[key].push_back(i);
			else if (auto found = lifetimes.find(key); found != lifetimes.end() && !found->second.empty())
			{
				auto& placement = records[found->second.front()];
				ConstructionRecord tombstone{ConstructionType::ObjectTombstone}; tombstone.a = placement.a;
				placement = std::move(tombstone);
				for (size_t j = 1; j < found->second.size(); ++j) discard[found->second[j]] = true;
				// Removing a tail panel originally trimmed its empty object slots.
				// Replay that trim as well as the placement slot, so later indices
				// are identical whether the removed panel was at the tail or not.
				ConstructionRecord trim{ConstructionType::ObjectTombstone}; trim.a = record.a; trim.p = true;
				record = std::move(trim); lifetimes.erase(found);
			}
		}
		std::vector<ConstructionRecord> retained;
		for (size_t i = 0; i < records.size(); ++i) if (!discard[i]) retained.push_back(std::move(records[i]));
		records = std::move(retained);
	}

	void World::validateRetainedAccessPanels() const
	{
		for (auto const& sector : mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = std::dynamic_pointer_cast<const AccessPanelSectorObject>(sector->getObject(i));
				if (!object) continue;
				std::string diagnostic;
				auto panel = object->getPanel();
				if (!validateAccessPanel(sector->getIndex(), panel->getLevelOffset(), object->getCellX(),
					panel->getGeometry(), i, &diagnostic)) throw WorldException(this, diagnostic);
			}
		}
	}

	bool World::canAddAccessPanel(uint32_t sector, uint32_t level, uint32_t x,
		AccessPanelGeometry geometry, std::string* diagnostic) const
	{
		return validateAccessPanel(sector, level, x, geometry, ~0u, diagnostic);
	}

	World::CreateObjectResult World::addAccessPanel(uint32_t index, uint32_t level,
		uint32_t x, AccessPanelGeometry geometry, std::optional<float> speed)
	{
		if (!AccessPanel::speedIsValid(speed)) throw WorldException(this, "Access panel speed must be finite and positive");
		std::string diagnostic;
		if (!canAddAccessPanel(index, level, x, geometry, &diagnostic)) throw WorldException(this, diagnostic);
		beginStructuralEdit("addAccessPanel");
		auto sector = mSectors[index];
		auto y = sector->getCellY() + level;
		auto object = std::make_shared<AccessPanelSectorObject>(sector, x, y, level, geometry);
		auto panel = std::static_pointer_cast<AccessPanel>(object->_getObject());
		panel->mSpeedOverride = speed;
		panel->mId = AccessPanelId{mNextAccessPanelId++};
		mAccessPanels.emplace(panel->mId, panel);
		for (auto action : {AccessPanel::Action::Open, AccessPanel::Action::Close})
		{
			DeviceCommand command;
			command.type = DeviceCommandType::SetAccessPanelState;
			command.accessPanel = panel->mId;
			command.desiredState = action == AccessPanel::Action::Open;
			auto control = createInteractionPoint(command.desiredState ? "Open Access panel" : "Close Access panel",
				SectorId{uint64_t(index) + 1}, {float(x) + 0.5f, float(y)}, 0.25f, 0,
				{{command, InteractionBindingRequirement::Required}});
			mInteractionPoints.find(control)->mAccessPanelOwner = panel->mId;
			(command.desiredState ? panel->mOpenControl : panel->mCloseControl) = control;
		}
		auto objectIndex = sector->addSectorObject(object);
		mLayers[sector->getLayerIndex()]->getCellDefinition(x, y).accessPanel = objectIndex;
		ConstructionRecord record{ConstructionType::AccessPanel};
		record.a = index; record.b = level; record.c = x - sector->getCellX();
		record.x = geometry.width; record.y = geometry.height; record.z = geometry.yOffset;
		record.accessPanelSpeed = speed;
		recordConstruction(record);
		return {objectIndex, SectorObjectType::AccessPanel, sector};
	}

	bool World::configureAccessPanel(uint32_t index, uint32_t objectIndex,
		AccessPanelGeometry geometry, std::string* diagnostic, std::optional<float> speed)
	{
		if (!AccessPanel::speedIsValid(speed))
		{
			if (diagnostic) *diagnostic = "Access panel speed must be finite and positive";
			return false;
		}
		if (index >= mSectors.size() || objectIndex >= mSectors[index]->getNumObjects()) return false;
		auto object = std::dynamic_pointer_cast<AccessPanelSectorObject>(mSectors[index]->getObject(objectIndex));
		if (!object) return false;
		auto panel = std::static_pointer_cast<AccessPanel>(object->_getObject());
		if (!validateAccessPanel(index, panel->getLevelOffset(), object->getCellX(), geometry, objectIndex, diagnostic)) return false;
		if (panel->getGeometry() == geometry && panel->getSpeedOverride() == speed) return false;
		if (mBuildFinished && !mSimulationPaused)
		{
			if (diagnostic) *diagnostic = "Pause simulation before editing Access panels";
			return false;
		}
		// Geometry does not alter the floor approach or traversal topology.
		panel->configure(object->getCellY(), geometry);
		panel->mSpeedOverride = speed;
		ConstructionRecord record{ConstructionType::ConfigureAccessPanel};
		record.a = index; record.b = panel->getLevelOffset(); record.c = object->getCellX() - mSectors[index]->getCellX();
		record.x = geometry.width; record.y = geometry.height; record.z = geometry.yOffset;
		record.accessPanelSpeed = speed;
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
		auto panel = object->getPanel();
		std::vector<InteractionPointId> controls;
		for (auto const& [id, point] : mInteractionPoints.entries())
			if (std::any_of(point->mBindings.begin(), point->mBindings.end(), [&](auto const& binding)
				{ return binding.command.type == DeviceCommandType::SetAccessPanelState && binding.command.accessPanel == panel->getId(); }))
				controls.push_back(id);
		for (auto control : controls)
		{
			mInteractionPoints.find(control)->mAccessPanelOwner = {};
			removeInteractionPoint(control);
		}
		std::vector<DeviceOperationId> operations;
		for (auto const& [id, operation] : mDeviceOperations.entries())
			if (operation->mCommand.type == DeviceCommandType::SetAccessPanelState
				&& operation->mCommand.accessPanel == panel->getId()) operations.push_back(id);
		for (auto id : operations) removeDeviceOperation(id);
		mAccessPanels.erase(panel->getId());
		mLayers[sector->getLayerIndex()]->getCellDefinition(object->getCellX(), object->getCellY()).accessPanel = ~0u;
		sector->removeSectorObject(objectIndex);
		ConstructionRecord record{ConstructionType::RemoveAccessPanel};
		record.a = index; record.b = object->getPanel()->getLevelOffset(); record.c = object->getCellX() - sector->getCellX();
		recordConstruction(record);
		if (!mDeserializingConstruction) finishBuild();
		return true;
	}
}
