#include "core/World.h"
#include "core/Exceptions.h"
#include "core/StaircaseTransit.h"
#include "core/Agent.h"
#include "core/ExtensibleObject.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{
	DeviceOperationId World::pressDumbwaiterLanding(DumbwaiterId id, uint32_t stop)
	{
		DeviceCommand command;
		command.type = DeviceCommandType::PressDumbwaiterLanding;
		command.dumbwaiter = id;
		command.stopIndex = stop;
		return submitDeviceCommand(command);
	}

	InteractionRequestId World::requestDumbwaiterLanding(DumbwaiterId id, uint32_t stop, AgentId actor)
	{
		auto unit = lookupDumbwaiter(id);
		return unit && stop < 2 ? requestInteraction(unit->getLandingButton(stop), actor) : InteractionRequestId{};
	}

	bool World::hasDumbwaiters() const
	{
		return std::any_of(mSectors.begin(), mSectors.end(), [](auto const& sector)
		{ return sector->getType() == SectorType::Dumbwaiter; });
	}

	std::shared_ptr<const Dumbwaiter> World::lookupDumbwaiter(DumbwaiterId id) const
	{
		for (auto const& sector : mSectors)
			if (auto unit = std::dynamic_pointer_cast<const Dumbwaiter>(sector); unit && unit->getId() == id)
				return unit;
		return nullptr;
	}

	bool World::canAddDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
		CreateDumbwaiterOptions const& options, std::string* diagnostic) const
	{
		auto refuse = [&](char const* message) { if (diagnostic) *diagnostic = message; return false; };
		if (options.initialStop > 1 || !std::isfinite(options.travelSeconds)
			|| options.travelSeconds < 0.1f || options.travelSeconds > 60.0f)
			return refuse("Dumbwaiter requires initial lower/upper Stop and finite travel time in 0.1–60 seconds");
		if (layer == 0 || layer >= getLayerCount() || x >= mCellsWide
			|| mLevelsHigh < 2 || y >= mLevelsHigh - 1)
			return refuse("Dumbwaiter requires a bounded 1x2 shaft behind its landing Layer");
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto const& shaft = mLayers[layer]->getCellDefinition(x, y + stop);
			auto const& landing = mLayers[layer - 1]->getCellDefinition(x, y + stop);
			if (shaft.occupied()) return refuse("Dumbwaiter shaft footprint is occupied");
			if (!landing.occupied() || !isLocationLike(mSectors[landing.sectorIndex]->getType()))
				return refuse("Dumbwaiter landings require Rooms, Corridors, or Facades");
			if (landing.floorType != CellFloorType::Ground && landing.floorType != CellFloorType::Walkway)
				return refuse("Dumbwaiter landings require permanent walkable support");
			if (landing.hasObject() || landing.bulkheadIndices[0] != ~0u || landing.bulkheadIndices[1] != ~0u)
				return refuse("Dumbwaiter landing aperture conflicts with an existing object or threshold");
		}
		if (diagnostic) diagnostic->clear();
		return true;
	}

	DumbwaiterId World::addDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
		CreateDumbwaiterOptions const& options)
	{
		std::string diagnostic;
		if (!canAddDumbwaiter(layer, y, x, options, &diagnostic)) throw WorldException(this, diagnostic);
		if (mNextDumbwaiterId == 0 || mNextDumbwaiterId == std::numeric_limits<uint64_t>::max())
			throw WorldException(this, "Dumbwaiter identity space exhausted");
		return createDumbwaiter(layer, y, x, options, DumbwaiterId{mNextDumbwaiterId});
	}

	DumbwaiterId World::createDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
		CreateDumbwaiterOptions const& options, DumbwaiterId id)
	{
		std::string diagnostic;
		if (!id || id.value == std::numeric_limits<uint64_t>::max() || lookupDumbwaiter(id))
			throw WorldException(this, "Invalid or duplicate Dumbwaiter identity");
		if (!canAddDumbwaiter(layer, y, x, options, &diagnostic)) throw WorldException(this, diagnostic);
		beginStructuralEdit("addDumbwaiter");
		std::vector<TransitStop> stops;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto landing = mSectors[mLayers[layer - 1]->getCellDefinition(x, y + stop).sectorIndex];
			stops.push_back({landing, int(x) - int(landing->getCellX()), int(y + stop) - int(landing->getCellY())});
		}
		auto index = uint32_t(mSectors.size());
		auto unit = std::make_shared<Dumbwaiter>(id, index, layer, x, y, options.initialStop, options.travelSeconds, stops);
		mSectors.push_back(unit);
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto& shaft = mLayers[layer]->getCellDefinition(x, y + stop);
			shaft.sectorIndex = index;
			shaft.floorType = CellFloorType::None;
			auto created = createWindow(layer - 1, x, y + stop, 1, 1, nullptr, true);
			auto booth = std::static_pointer_cast<BoothWindow>(
				std::static_pointer_cast<WindowSectorObject>(created.sector->_getObject(created.index))->getWindow());
			booth->setState(stop == options.initialStop ? Window::State::Open : Window::State::Closed);
			booth->mDumbwaiterOwner = id;
			booth->mDeviceId = BoothWindowId{mNextBoothWindowId++};
			mBoothWindows.emplace(booth->mDeviceId, booth);
			unit->mApertures[stop] = booth;
			DeviceCommand press;
			press.type = DeviceCommandType::PressDumbwaiterLanding;
			press.dumbwaiter = id; press.stopIndex = stop;
			unit->mLandingButtons[stop] = createInteractionPoint(
				stop == 0 ? "Dumbwaiter lower landing" : "Dumbwaiter upper landing",
				SectorId{uint64_t(stops[stop].sector->getIndex()) + 1},
				{float(x) + 0.5f, float(y + stop)}, 0.25f, 0.0f,
				{{press, InteractionBindingRequirement::Required}});
			mInteractionPoints.find(unit->mLandingButtons[stop])->mDumbwaiterOwner = id;
			auto& landing = mLayers[layer - 1]->getCellDefinition(x, y + stop);
			landing.sectorObjectType = SectorObjectType::BoothWindow;
			landing.sectorObjectIndex = created.index;
		}
		mNextDumbwaiterId = std::max(mNextDumbwaiterId, id.value + 1);
		ConstructionRecord record{ConstructionType::Dumbwaiter};
		record.layer = layer; record.a = y; record.b = x;
		record.c = options.initialStop; record.x = options.travelSeconds; record.dumbwaiterId = id;
		recordConstruction(std::move(record));
		modify();
		return id;
	}

	bool World::configureDumbwaiter(DumbwaiterId id, CreateDumbwaiterOptions const& options)
	{
		if (options.initialStop > 1 || !std::isfinite(options.travelSeconds)
			|| options.travelSeconds < 0.1f || options.travelSeconds > 60.0f)
			throw WorldException(this, "Invalid Dumbwaiter configuration");
		if (!mSimulationPaused) throw WorldException(this, "Dumbwaiter configuration requires pauseSimulation()");
		auto records = mConstructionRecords;
		for (auto& record : records)
			if (record.type == ConstructionType::Dumbwaiter && record.dumbwaiterId == id)
			{
				if (record.c == options.initialStop && record.x == options.travelSeconds) return false;
				// Configuration is device-local, not a whole-World replay. In
				// particular it must not reset unrelated Agents or moving devices.
				auto unit = std::const_pointer_cast<Dumbwaiter>(lookupDumbwaiter(id));
				unit->mInitialStop = options.initialStop;
				unit->mTravelSeconds = options.travelSeconds;
				mSimulationCoordinator.resetDumbwaiter(*unit);
				record.c = options.initialStop; record.x = options.travelSeconds;
				mConstructionRecords = std::move(records);
				modify();
				return true;
			}
		return false;
	}

	bool World::removeDumbwaiter(DumbwaiterId id)
	{
		auto unit = lookupDumbwaiter(id);
		if (!unit) return false;
		if (!mSimulationPaused) throw WorldException(this, "Dumbwaiter deletion requires pauseSimulation()");
		auto removedIndex = unit->getIndex();
		std::vector<ConstructionRecord> records;
		for (auto record : mConstructionRecords)
		{
			if (record.type == ConstructionType::Dumbwaiter && record.dumbwaiterId == id)
			{
				// Keep front-Location object slots stable after removing owned apertures.
				for (uint32_t stop = 0; stop < 2; ++stop)
				{
					ConstructionRecord tombstone{ConstructionType::ObjectTombstone};
					tombstone.a = unit->getStop(stop).sector->getIndex();
					if (tombstone.a > removedIndex) --tombstone.a;
					records.push_back(std::move(tombstone));
				}
				continue;
			}
			switch (record.type)
			{
			case ConstructionType::LightSwitch: case ConstructionType::ForceBridge:
			case ConstructionType::SectorLadder: case ConstructionType::PlatformLift:
			case ConstructionType::Walkway: case ConstructionType::Marker:
			case ConstructionType::RemoveWall: case ConstructionType::RemoveMarker:
			case ConstructionType::ObjectTombstone:
				if (record.a > removedIndex) --record.a;
				break;
			default: break;
			}
			records.push_back(std::move(record));
		}
		// Validate the authored result before mutation, but do not replay the
		// live World: that would reset unrelated devices and their operations.
		auto candidate = makeCandidateWorld();
		candidate->mDeserializingConstruction = true;
		for (auto const& record : records) candidate->applyConstructionRecord(record);
		candidate->finishBuild();
		beginStructuralEdit("removeDumbwaiter");
		mSimulationCoordinator.resetDumbwaiter(*std::const_pointer_cast<Dumbwaiter>(unit));
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto booth = unit->getAperture(stop);
			mBoothWindows.erase(booth->getDeviceId());
			auto point = mInteractionPoints.find(unit->getLandingButton(stop));
			point->mDumbwaiterOwner = {};
			removeInteractionPoint(unit->getLandingButton(stop));
			auto landing = std::const_pointer_cast<Sector>(unit->getStop(stop).sector);
			for (auto& object : landing->mObjects)
				if (auto window = std::dynamic_pointer_cast<WindowSectorObject>(object);
					window && window->getWindow() == booth) object.reset();
			auto& front = mLayers[unit->getLayerIndex() - 1]->getCellDefinition(unit->getCellX(), unit->getCellY() + stop);
			front.sectorObjectType = SectorObjectType::None;
			front.sectorObjectIndex = ~0u;
			mLayers[unit->getLayerIndex()]->getCellDefinition(unit->getCellX(), unit->getCellY() + stop) = {};
		}
		auto remap = [removedIndex](SectorId& sector)
		{
			if (sector.value == uint64_t(removedIndex) + 1) sector = {};
			else if (sector.value > uint64_t(removedIndex) + 1) --sector.value;
		};
		for (auto& layer : mLayers)
			for (uint32_t y = 0; y < mLevelsHigh; ++y)
				for (uint32_t x = 0; x < mCellsWide; ++x)
				{
					auto& cell = layer->getCellDefinition(x, y);
					if (cell.occupied() && cell.sectorIndex > removedIndex) --cell.sectorIndex;
				}
		mSectors.erase(mSectors.begin() + removedIndex);
		for (uint32_t index = removedIndex; index < mSectors.size(); ++index)
		{
			mSectors[index]->mIndex = index;
			if (auto stairs = std::dynamic_pointer_cast<StaircaseTransit>(mSectors[index]))
				stairs->getStaircase()->mSectorIndex = index;
		}
		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			(void)pointId; remap(point->mSector);
			for (auto& binding : point->mBindings) remap(binding.command.target);
		}
		for (auto const& [operationId, operation] : mDeviceOperations.entries())
		{ (void)operationId; remap(operation->mCommand.target); }
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
		{
			(void)resourceId; remap(resource->mLiftSector); remap(resource->mLadderSector);
			for (auto& stop : resource->mLiftStops) remap(stop.locationSector);
			for (auto& door : resource->mShuttleDoors) remap(door.locationSector);
			for (auto& lane : resource->mQueueLanes) remap(lane.sector);
			for (auto& sector : resource->mDoorRouteObservationKey.sectors) remap(sector);
			if (resource->mExtensible)
			{
				std::set<SectorId> controls;
				for (auto sector : resource->mExtensible->mExtensionControlSectors) { remap(sector); controls.insert(sector); }
				resource->mExtensible->mExtensionControlSectors = std::move(controls);
			}
		}
		for (auto& [agentId, intent] : mPausedPathIntents)
		{ (void)agentId; remap(intent.destinationSector); }
		for (auto& [agentId, goal] : mMovementGoals)
		{
			(void)agentId; remap(goal.sector);
			if (goal.fallbackIntent) remap(goal.fallbackIntent->destinationSector);
		}
		for (auto const& [agentId, agent] : mAgents.entries())
		{
			(void)agentId;
			std::map<uint32_t, DeviceCondition> conditions;
			for (auto const& [index, condition] : agent->mRememberedEscalatorConditions)
				conditions.emplace(index > removedIndex ? index - 1 : index, condition);
			agent->mRememberedEscalatorConditions = std::move(conditions);
		}
		for (auto& control : mPhysicalControlPlacements)
			if (control.sectorIndex > removedIndex) --control.sectorIndex;
		auto removedRecord = std::find_if(mConstructionRecords.begin(), mConstructionRecords.end(),
			[id](auto const& record) { return record.type == ConstructionType::Dumbwaiter && record.dumbwaiterId == id; });
		auto recordIndex = size_t(removedRecord - mConstructionRecords.begin());
		for (auto& [point, requirement] : mAuthoredControlRequirements)
		{ (void)point; if (requirement.constructionRecord > recordIndex) ++requirement.constructionRecord; }
		mConstructionRecords = std::move(records);
		finishBuild();
		return true;
	}
}
