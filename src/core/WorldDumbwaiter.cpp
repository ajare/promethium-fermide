#include "core/World.h"
#include "core/Exceptions.h"
#include "core/StaircaseTransit.h"
#include "core/Agent.h"
#include "core/ExtensibleObject.h"
#include "core/Button.h"
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

	bool World::isDumbwaiterOwnedControl(std::shared_ptr<const SectorObject> const& object) const
	{
		auto button = object ? std::dynamic_pointer_cast<const Button>(object->_getObject()) : nullptr;
		auto point = button ? mInteractionPoints.find(button->getInteractionPointId()) : nullptr;
		return point && bool(point->mDumbwaiterOwner);
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
		return preflightDumbwaiter(layer, y, x, options, {}, diagnostic);
	}

	bool World::preflightDumbwaiter(uint32_t layer, uint32_t y, uint32_t x,
		CreateDumbwaiterOptions const& options, DumbwaiterId ignored, std::string* diagnostic) const
	{
		auto source = lookupDumbwaiter(ignored);
		auto refuse = [&](char const* message) { if (diagnostic) *diagnostic = message; return false; };
		if (options.initialStop > 1 || !std::isfinite(options.travelSeconds)
			|| options.travelSeconds < 0.1f || options.travelSeconds > 60.0f)
			return refuse("Dumbwaiter requires initial lower/upper Stop and finite travel time in 0.1–60 seconds");
		for (auto const& requirement : options.landingPermissionRequirements)
		{
			std::set<AccessPermissionId> seen;
			for (auto permission : requirement)
				if (!lookupAccessPermission(permission) || !seen.insert(permission).second)
					return refuse("Unknown or duplicate Dumbwaiter landing Permission reference");
		}
		if (layer == 0 || layer >= getLayerCount() || x >= mCellsWide
			|| mLevelsHigh < 2 || y >= mLevelsHigh - 1)
			return refuse("Dumbwaiter requires a bounded 1x2 shaft behind its landing Layer");
		bool right = false;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto const& landing = mLayers[layer - 1]->getCellDefinition(x, y + stop);
			if (!landing.occupied() || !isLocationLike(mSectors[landing.sectorIndex]->getType()))
				return refuse("Dumbwaiter landings require Rooms, Corridors, or Facades");
			auto sector = mSectors[landing.sectorIndex];
			if (sector->getCellsWide() < 2)
				return refuse("Dumbwaiter landing Sectors must be at least two cells wide");
			right = right || x == sector->getCellX();
		}
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto const& shaft = mLayers[layer]->getCellDefinition(x, y + stop);
			auto const& landing = mLayers[layer - 1]->getCellDefinition(x, y + stop);
			if (shaft.occupied() && (!source || shaft.sectorIndex != source->getIndex()))
				return refuse("Dumbwaiter shaft footprint is occupied");
			if (!landing.occupied() || !isLocationLike(mSectors[landing.sectorIndex]->getType()))
				return refuse("Dumbwaiter landings require Rooms, Corridors, or Facades");
			auto sector = mSectors[landing.sectorIndex];
			if (right && x == sector->getCellX1())
				return refuse("Dumbwaiter landing Buttons require a shared interior cell border");
			auto controlIndex = landing.controls[right ? CORE_SIDE_RIGHT : CORE_SIDE_LEFT];
			if (controlIndex != ~0u)
			{
				auto button = std::dynamic_pointer_cast<const Button>(sector->getObject(controlIndex)->_getObject());
				auto point = button ? mInteractionPoints.find(button->getInteractionPointId()) : nullptr;
				if (!source || !point || point->mDumbwaiterOwner != ignored)
					return refuse("Dumbwaiter landing Button conflicts with an existing control");
			}
			if (landing.floorType != CellFloorType::Ground && landing.floorType != CellFloorType::Walkway)
				return refuse("Dumbwaiter landings require permanent walkable support");
			bool ownAperture = source && layer == source->getLayerIndex() && x == source->getCellX()
				&& y + stop >= source->getCellY() && y + stop < source->getCellY() + 2;
			if ((landing.hasObject() && !ownAperture) || landing.bulkheadIndices[0] != ~0u || landing.bulkheadIndices[1] != ~0u)
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
		beginStructuralEdit("addDumbwaiter", true);
		std::vector<TransitStop> stops;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto landing = mSectors[mLayers[layer - 1]->getCellDefinition(x, y + stop).sectorIndex];
			stops.push_back({landing, int(x) - int(landing->getCellX()), int(y + stop) - int(landing->getCellY())});
		}
		auto index = uint32_t(mSectors.size());
		auto unit = std::make_shared<Dumbwaiter>(id, index, layer, x, y, options.initialStop, options.travelSeconds, stops);
		mSectors.push_back(unit);
		attachDumbwaiter(unit);
		mNextDumbwaiterId = std::max(mNextDumbwaiterId, id.value + 1);
		ConstructionRecord record{ConstructionType::Dumbwaiter};
		record.layer = layer; record.a = y; record.b = x;
		record.c = options.initialStop; record.x = options.travelSeconds; record.dumbwaiterId = id;
		for (uint32_t stop = 0; stop < 2; ++stop)
			for (auto permission : options.landingPermissionRequirements[stop])
			{
				mInteractionPoints.find(unit->getLandingButton(stop))->mPermissionRequirement.set(permission.value - 1);
				record.controlPermissionRequirements[stop].push_back(static_cast<uint32_t>(permission.value));
			}
		recordConstruction(std::move(record));
		modify();
		return id;
	}

	void World::attachDumbwaiter(std::shared_ptr<Dumbwaiter> const& unit)
	{
		auto layer = unit->getLayerIndex(), x = unit->getCellX(), y = unit->getCellY();
		auto id = unit->getId();
		int side = (x == unit->getStop(0).sector->getCellX() || x == unit->getStop(1).sector->getCellX())
			? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto& shaft = mLayers[layer]->getCellDefinition(x, y + stop);
			shaft.sectorIndex = unit->getIndex();
			shaft.floorType = CellFloorType::None;
			auto created = createWindow(layer - 1, x, y + stop, 1, 1, nullptr, true);
			auto booth = std::static_pointer_cast<BoothWindow>(
				std::static_pointer_cast<WindowSectorObject>(created.sector->_getObject(created.index))->getWindow());
			booth->setState(stop == unit->getInitialStop() ? Window::State::Open : Window::State::Closed);
			booth->mDumbwaiterOwner = id;
			booth->mDeviceId = BoothWindowId{mNextBoothWindowId++};
			mBoothWindows.emplace(booth->mDeviceId, booth);
			unit->mApertures[stop] = booth;
			DeviceCommand press;
			press.type = DeviceCommandType::PressDumbwaiterLanding;
			press.dumbwaiter = id; press.stopIndex = stop;
			auto name = stop == 0 ? "Dumbwaiter lower landing" : "Dumbwaiter upper landing";
			auto control = createPhysicalControl(name, layer - 1, x, y + stop, side, 0);
			unit->mLandingButtons[stop] = createPhysicalControlInteractionPoint(name, control,
				float(y + stop), 0.25f, 0.0f, {{press, InteractionBindingRequirement::Required}});
			mInteractionPoints.find(unit->mLandingButtons[stop])->mDumbwaiterOwner = id;
			auto& landing = mLayers[layer - 1]->getCellDefinition(x, y + stop);
			landing.sectorObjectType = SectorObjectType::BoothWindow;
			landing.sectorObjectIndex = created.index;
		}
	}

	void World::detachDumbwaiter(std::shared_ptr<const Dumbwaiter> const& unit)
	{
		mSimulationCoordinator.resetDumbwaiter(*std::const_pointer_cast<Dumbwaiter>(unit));
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto booth = unit->getAperture(stop);
			mBoothWindows.erase(booth->getDeviceId());
			auto point = mInteractionPoints.find(unit->getLandingButton(stop));
			point->mDumbwaiterOwner = {};
			removeInteractionPoint(unit->getLandingButton(stop));
			auto landing = std::const_pointer_cast<Sector>(unit->getStop(stop).sector);
			auto& cell = mLayers[unit->getLayerIndex() - 1]->getCellDefinition(unit->getCellX(), unit->getCellY() + stop);
			for (auto& index : cell.controls)
				if (index != ~0u)
				{
					auto object = landing->mObjects[index];
					auto button = object ? std::dynamic_pointer_cast<Button>(object->_getObject()) : nullptr;
					if (button && button->getInteractionPointId() == unit->getLandingButton(stop))
					{
						auto removed = index;
						landing->mObjects[index].reset(); index = ~0u;
						std::erase_if(mPhysicalControlPlacements, [&](auto const& placement) {
							return placement.sectorIndex == landing->getIndex() && placement.objectIndex == removed;
						});
					}
				}
			for (auto& object : landing->mObjects)
				if (auto window = std::dynamic_pointer_cast<WindowSectorObject>(object);
					window && window->getWindow() == booth) object.reset();
			auto& front = mLayers[unit->getLayerIndex() - 1]->getCellDefinition(unit->getCellX(), unit->getCellY() + stop);
			front.sectorObjectType = SectorObjectType::None;
			front.sectorObjectIndex = ~0u;
			mLayers[unit->getLayerIndex()]->getCellDefinition(unit->getCellX(), unit->getCellY() + stop) = {};
		}
	}

	World::DumbwaiterMovePlan World::planMoveDumbwaiter(DumbwaiterId id, uint32_t layer, uint32_t y, uint32_t x) const
	{
		DumbwaiterMovePlan plan;
		plan.id = id; plan.layer = layer; plan.y = y; plan.x = x;
		auto unit = lookupDumbwaiter(id);
		if (!unit) plan.diagnostic = "Unknown Dumbwaiter";
		else if (!mSimulationPaused && mBuildFinished) plan.diagnostic = "Dumbwaiter movement requires pauseSimulation()";
		else plan.valid = preflightDumbwaiter(layer, y, x,
			{unit->getInitialStop(), unit->getTravelSeconds()}, id, &plan.diagnostic);
		return plan;
	}

	bool World::applyDumbwaiterMove(DumbwaiterMovePlan const& requested)
	{
		// Recheck public plans; stale or forged plans may never cancel accepted work.
		auto plan = planMoveDumbwaiter(requested.id, requested.layer, requested.y, requested.x);
		if (!plan.valid) throw WorldException(this, plan.diagnostic);
		auto old = lookupDumbwaiter(plan.id);
		if (old->getLayerIndex() == plan.layer && old->getCellY() == plan.y && old->getCellX() == plan.x) return false;
		std::array<std::bitset<256>, 2> requirements;
		for (uint32_t stop = 0; stop < 2; ++stop)
			requirements[stop] = mInteractionPoints.find(old->getLandingButton(stop))->mPermissionRequirement;
		beginStructuralEdit("moveDumbwaiter", true);
		detachDumbwaiter(old);
		std::vector<TransitStop> stops;
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto landing = mSectors[mLayers[plan.layer - 1]->getCellDefinition(plan.x, plan.y + stop).sectorIndex];
			stops.push_back({landing, int(plan.x) - int(landing->getCellX()), int(plan.y + stop) - int(landing->getCellY())});
		}
		auto unit = std::make_shared<Dumbwaiter>(plan.id, old->getIndex(), plan.layer, plan.x, plan.y,
			old->getInitialStop(), old->getTravelSeconds(), stops);
		mSectors[old->getIndex()] = unit;
		attachDumbwaiter(unit);
		for (uint32_t stop = 0; stop < 2; ++stop)
			mInteractionPoints.find(unit->getLandingButton(stop))->mPermissionRequirement = requirements[stop];
		ConstructionRecord record{ConstructionType::MoveDumbwaiter};
		record.dumbwaiterId = plan.id; record.layer = plan.layer; record.a = plan.y; record.b = plan.x;
		recordConstruction(std::move(record));
		if (mBuildFinished) finishBuild();
		return true;
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

	std::set<DumbwaiterId> World::locationEditDumbwaiters(LocationEditPlan const& plan) const
	{
		std::set<DumbwaiterId> removed;
		auto location = mSectors[plan.sectorIndex];
		for (auto const& sector : mSectors)
			if (auto unit = std::dynamic_pointer_cast<const Dumbwaiter>(sector))
				for (uint32_t stop = 0; stop < 2; ++stop)
				{
					if (unit->getStop(stop).sector != location) continue;
					auto x = unit->getCellX(), y = unit->getCellY() + stop;
					bool supported = !plan.remove && plan.cellsWide >= 2 && x >= plan.x && x - plan.x < plan.cellsWide
						&& y >= plan.y && y - plan.y < plan.levelsHigh;
					if (supported && y != plan.y)
					{
						supported = false;
						for (auto const& record : mConstructionRecords)
							if (record.type == ConstructionType::Walkway && record.a == plan.sectorIndex
								&& plan.x + record.c == x && plan.y + record.b == y) supported = true;
					}
					if (!supported) removed.insert(unit->getId());
				}
		// A resize can force both Buttons to the other side. Remove the whole
		// unit if that common border would now be a wall at the other landing.
		for (auto const& sector : mSectors)
			if (auto unit = std::dynamic_pointer_cast<const Dumbwaiter>(sector); unit && !removed.contains(unit->getId()))
			{
				bool right = false, touchesRight = false;
				for (uint32_t stop = 0; stop < 2; ++stop)
				{
					auto landing = unit->getStop(stop).sector;
					auto left = landing == location ? plan.x : landing->getCellX();
					auto width = landing == location ? plan.cellsWide : landing->getCellsWide();
					right = right || unit->getCellX() == left;
					touchesRight = touchesRight || unit->getCellX() == left + width - 1;
				}
				if (right && touchesRight) removed.insert(unit->getId());
			}
		return removed;
	}

	std::array<uint32_t, 2> World::dumbwaiterRecordLandings(ConstructionRecord const& record) const
	{
		std::array<uint32_t, 2> owners{~0u, ~0u};
		uint32_t index = 0;
		// Use authored producer footprints, not the live cells: a moved unit's
		// earlier apertures still consumed object slots at its previous landings.
		for (auto const& source : mConstructionRecords)
		{
			if (!constructionTypeCreatesSector(source.type)) continue;
			bool room = source.type == ConstructionType::Room;
			if (room || source.type == ConstructionType::Corridor || source.type == ConstructionType::Facade)
			{
				auto layer = room ? source.a : source.layer;
				auto y = room ? source.b : source.a;
				auto x = room ? source.c : source.b;
				auto width = room ? source.d : source.c;
				auto height = room ? source.e : source.d;
				if (layer == record.layer - 1 && record.b >= x && record.b - x < width)
					for (uint32_t stop = 0; stop < 2; ++stop)
						if (record.a + stop >= y && record.a + stop - y < height) owners[stop] = index;
			}
			++index;
		}
		return owners;
	}

	void World::reconcileDumbwaiterReplay(std::vector<ConstructionRecord>& records,
		std::vector<uint32_t>& originalToReplay) const
	{
		std::map<DumbwaiterId, size_t> lastMove;
		std::map<DumbwaiterId, ConstructionRecord> authored;
		std::map<DumbwaiterId, uint32_t> producers;
		uint32_t producer = 0;
		for (size_t i = 0; i < records.size(); ++i)
		{
			auto const& record = records[i];
			if (record.type == ConstructionType::Dumbwaiter)
			{
				authored.emplace(record.dumbwaiterId, record);
				producers.emplace(record.dumbwaiterId, producer);
			}
			if (record.type == ConstructionType::MoveDumbwaiter) lastMove[record.dumbwaiterId] = i;
			if (constructionTypeCreatesSector(record.type)) ++producer;
		}
		if (lastMove.empty()) return;
		// Historical placements only consumed aperture slots. Rebuild each moved
		// survivor at its final placement, after its destination Locations exist.
		// This lets an edit remove obsolete landing support without invalidating
		// a unit whose current landings remain supported.
		std::map<DumbwaiterId, std::vector<std::array<uint32_t, 2>>> historicalOwners;
		for (auto const& record : mConstructionRecords)
			if (record.type == ConstructionType::Dumbwaiter || record.type == ConstructionType::MoveDumbwaiter)
				historicalOwners[record.dumbwaiterId].push_back(dumbwaiterRecordLandings(record));
		std::map<DumbwaiterId, size_t> occurrence;
		struct Item { ConstructionRecord record; uint32_t producer{~0u}; };
		std::vector<Item> output;
		uint32_t oldProducer = 0;
		for (size_t i = 0; i < records.size(); ++i)
		{
			auto record = records[i];
			auto index = constructionTypeCreatesSector(record.type) ? oldProducer++ : ~0u;
			if ((record.type == ConstructionType::Dumbwaiter || record.type == ConstructionType::MoveDumbwaiter)
				&& lastMove.contains(record.dumbwaiterId))
			{
				auto id = record.dumbwaiterId;
				auto const& owners = historicalOwners.at(id).at(occurrence[id]++);
				if (i != lastMove.at(id))
				{
					for (auto owner : owners)
						if (owner < originalToReplay.size() && originalToReplay[owner] != ~0u)
						{
							ConstructionRecord tombstone{ConstructionType::ObjectTombstone};
							tombstone.a = originalToReplay[owner]; output.push_back({tombstone}); output.push_back({std::move(tombstone)});
						}
					continue;
				}
				auto final = authored.at(id);
				final.layer = record.layer; final.a = record.a; final.b = record.b;
				output.push_back({std::move(final), producers.at(id)});
			}
			else output.push_back({std::move(record), index});
		}
		std::vector<uint32_t> remap(producer, ~0u);
		uint32_t next = 0;
		for (auto const& item : output) if (item.producer != ~0u) remap[item.producer] = next++;
		records.clear();
		for (auto& item : output)
		{
			auto& record = item.record;
			switch (record.type)
			{
			case ConstructionType::LightSwitch: case ConstructionType::ForceBridge:
			case ConstructionType::SectorLadder: case ConstructionType::PlatformLift:
			case ConstructionType::Walkway: case ConstructionType::Marker:
			case ConstructionType::RemoveWall: case ConstructionType::RemoveMarker:
			case ConstructionType::ObjectTombstone:
				record.a = remap.at(record.a); break;
			default: break;
			}
			records.push_back(std::move(record));
		}
		for (auto& index : originalToReplay) if (index != ~0u) index = remap.at(index);
	}

	void World::removeDumbwaiterRecords(std::vector<ConstructionRecord>& records,
		std::set<DumbwaiterId> const& removed, std::vector<uint32_t>* mapping) const
	{
		std::vector<uint32_t> sectorMap(mSectors.size(), ~0u);
		uint32_t producer = 0, next = 0;
		for (auto const& record : records)
			if (constructionTypeCreatesSector(record.type))
			{
				if (record.type != ConstructionType::Dumbwaiter || !removed.contains(record.dumbwaiterId))
					sectorMap[producer] = next++;
				++producer;
			}
		std::vector<ConstructionRecord> retained;
		for (auto record : records)
		{
			if ((record.type == ConstructionType::Dumbwaiter || record.type == ConstructionType::MoveDumbwaiter)
				&& removed.contains(record.dumbwaiterId))
			{
				for (auto owner : dumbwaiterRecordLandings(record))
					if (owner < sectorMap.size() && sectorMap[owner] != ~0u)
					{
						ConstructionRecord tombstone{ConstructionType::ObjectTombstone};
						tombstone.a = sectorMap[owner]; retained.push_back(tombstone); retained.push_back(std::move(tombstone));
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
				if (record.a >= sectorMap.size() || sectorMap[record.a] == ~0u) continue;
				record.a = sectorMap[record.a]; break;
			default: break;
			}
			retained.push_back(std::move(record));
		}
		records = std::move(retained);
		if (mapping) *mapping = std::move(sectorMap);
	}

	bool World::removeDumbwaiter(DumbwaiterId id)
	{
		auto unit = lookupDumbwaiter(id);
		if (!unit) return false;
		if (!mSimulationPaused) throw WorldException(this, "Dumbwaiter deletion requires pauseSimulation()");
		auto removedIndex = unit->getIndex();
		auto records = mConstructionRecords;
		removeDumbwaiterRecords(records, {id});
		// Validate the authored result before mutation, but do not replay the
		// live World: that would reset unrelated devices and their operations.
		auto candidate = makeCandidateWorld();
		candidate->mDeserializingConstruction = true;
		for (auto const& record : records) candidate->applyConstructionRecord(record);
		candidate->finishBuild();
		beginStructuralEdit("removeDumbwaiter", true);
		detachDumbwaiter(unit);
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
		for (auto& [point, requirement] : mAuthoredControlRequirements)
		{
			(void)point;
			size_t inserted = 0;
			for (size_t index = 0; index < requirement.constructionRecord; ++index)
				if ((mConstructionRecords[index].type == ConstructionType::Dumbwaiter
					|| mConstructionRecords[index].type == ConstructionType::MoveDumbwaiter)
					&& mConstructionRecords[index].dumbwaiterId == id) ++inserted;
			requirement.constructionRecord += inserted;
		}
		mConstructionRecords = std::move(records);
		finishBuild();
		return true;
	}
}
