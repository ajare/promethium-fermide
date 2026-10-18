#include "core/FurnitureStore.h"

#include "core/Agent.h"
#include "core/Exceptions.h"
#include "core/Marker.h"
#include "core/SectorType.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace core
{
	FurnitureStore::FurnitureStore(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Catalogue reference
	// ----------------------------------------------------------------------

	void FurnitureStore::attachFurnitureCatalogue(std::string resourceName,
		std::shared_ptr<const FurnitureCatalogue> catalogue)
	{
		std::filesystem::path path(resourceName);
		if (!catalogue || resourceName.empty() || path.is_absolute()
			|| path.has_parent_path() || path.filename().string() != resourceName)
			throw WorldException(&mWorld, "Furniture catalogue reference must be a Resource name");
		if (!mWorld.mFurniture.empty() && (resourceName != mWorld.mFurnitureCatalogueResourceName || catalogue != mWorld.mFurnitureCatalogue))
			throw WorldException(&mWorld, "Cannot replace a Furniture catalogue while instances use it");
		mWorld.mFurnitureCatalogue = std::move(catalogue);
		mWorld.mFurnitureCatalogueResourceName = std::move(resourceName);
		mWorld.modify();
	}

	bool FurnitureStore::reloadFurnitureCatalogue(std::filesystem::path const& path,
		std::string* diagnostic)
	{
		auto& world = mWorld;
		auto reject = [&](std::string message) { if (diagnostic) *diagnostic = "Furniture reload: " + message; return false; };
		if (!FurnitureCatalogue::filenameIsValid(path.filename().string()))
			return reject("Furniture catalogue must end with .furniture.lua; YAML Furniture requires conversion to Lua");
		if (!world.isSimulationPaused()) return reject("Pause the simulation before reloading");
		if (!world.mFurnitureCatalogue) return reject("Select this catalogue before reloading it");
		try
		{
			auto replacement = FurnitureCatalogue::load(path);
			if (replacement->uuid() != world.mFurnitureCatalogue->uuid()) return reject("Catalogue UUID does not match the selected package");
			auto candidate = world.makeCandidateWorld();
			candidate->mFurnitureCatalogue = replacement;
			candidate->mDeserializingConstruction = true;
			for (auto const& record : world.mConstructionRecords) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
			for (auto const& [marker, actions] : world.mMarkerActions)
				for (auto const& action : actions)
					if (!candidate->actionAvailable(marker, action))
						return reject("Marker " + std::to_string(marker.value) + " references unavailable Action " + action);
			// Every lifecycle in this catalogue is affected, even if geometry is unchanged.
			// Finish all users in stable Agent order, retaining the old package on error.
			for (auto const& [id, agent] : world.mAgents.entries())
				if (agent->mFurnitureUse && agent->mFurnitureUse->catalogue == world.mFurnitureCatalogue)
				{
					std::string reason;
					if (!world.furniturePoseFits(id, agent->mFurnitureUse->marker,
						world.mFurnitureCatalogue->definition(agent->mFurnitureUse->definition)->finishUsePose, false, &reason))
						return reject(reason);
				}
			bool finishingFailed = false;
			for (auto const& [id, agent] : world.mAgents.entries())
				if (agent->mFurnitureUse && agent->mFurnitureUse->catalogue == world.mFurnitureCatalogue)
				{
					auto outcomes = world.mPendingMovementOutcomes.size();
					world.finishFurnitureUse(id);
					finishingFailed |= world.mPendingMovementOutcomes.size() != outcomes;
				}
			if (finishingFailed) return reject("Old finish_use failed; users were safely cleaned up and the old catalogue remains installed (see Action diagnostics)");
			bool layoutChanged = false;
			for (auto const& instance : world.mFurniture)
				layoutChanged |= *world.mFurnitureCatalogue->definition(instance.definitionKey) != *replacement->definition(instance.definitionKey);
			auto old = world.mFurnitureCatalogue;
			auto goals = world.mMovementGoals;
			auto outcomes = world.mPendingMovementOutcomes;
			world.mFurnitureCatalogue = std::move(replacement);
			if (layoutChanged)
			{
				try { world.rebuildFromConstructionRecords(world.mConstructionRecords); }
				catch (...) { world.mFurnitureCatalogue = std::move(old); throw; }
				// Structural replay carries Agents, not Paths. Retain request identities
				// and replan against the new graph, never replacing their Action with Idle.
				world.mMovementGoals = std::move(goals);
				world.mPendingMovementOutcomes = std::move(outcomes);
				for (auto& [id, goal] : world.mMovementGoals)
				{
					goal.retainedPath.reset();
					if (auto agent = world.mAgents.find(id)) world.mSimulationCoordinator.beginRoutePlanning(*agent);
				}
			}
			world.invalidateSimulationSnapshot();
			for (auto& [id, goal] : world.mMovementGoals)
				if (goal.marker && !world.actionAvailable(goal.marker, goal.selectedAction))
				{ goal.actionInvalidated = true; goal.cancelling = true; }
			world.clearUnavailableResetRequests();
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (std::exception const& error) { return reject(error.what()); }
	}

	// ----------------------------------------------------------------------
	// Instance management
	// ----------------------------------------------------------------------

	bool FurnitureStore::isFurnitureMarker(MarkerId id) const
	{
		return std::any_of(mWorld.mFurniture.begin(), mWorld.mFurniture.end(),
			[id](auto const& instance) { return std::any_of(instance.destinations.begin(), instance.destinations.end(),
				[id](auto const& point) { return point.marker == id; }); });
	}

	bool FurnitureStore::canPlaceFurniture(uint32_t sectorIndex, std::string const& key,
		float x, float y, std::string const& name, std::string* diagnostic, int localDepth) const
	{
		auto& world = mWorld;
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!world.mFurnitureCatalogue) return reject("No Furniture catalogue is loaded");
		auto definition = world.mFurnitureCatalogue->definition(key);
		if (!definition) return reject("Missing Furniture definition: " + key);
		if (localDepth < 0) return reject("Furniture Local depth must be non-negative");
		for (auto const& edge : definition->edges)
			if (edge.depthOffset && (int64_t{ localDepth } + *edge.depthOffset < 0
				|| int64_t{ localDepth } + *edge.depthOffset > std::numeric_limits<int>::max()))
				return reject("Furniture resolved edge depth must be non-negative and representable");
		if (sectorIndex >= world.mSectors.size() || !world.mSectors[sectorIndex]
			|| !isLocationLike(world.mSectors[sectorIndex]->getType()))
			return reject("Furniture requires a Room, Corridor or Facade");
		auto sector = world.mSectors[sectorIndex];
		if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0
			|| std::floor(y) != y || y >= sector->getLevelsHigh()
			|| x + definition->minX < 0 || x + definition->maxX > sector->getCellsWide()
			|| y + definition->maxY > sector->getLevelsHigh())
			return reject("Furniture requires floor-aligned y and its complete width inside one Location");
		for (auto cellX = static_cast<uint32_t>(std::floor(x + definition->minX)); cellX < static_cast<uint32_t>(std::ceil(x + definition->maxX)); ++cellX)
		{
			auto const& cell = world.mLayers[sector->getLayerIndex()]->getCellDefinition(
				sector->getCellX() + cellX, sector->getCellY() + static_cast<uint32_t>(y));
			if (cell.sectorIndex != sectorIndex || (cell.floorType != CellFloorType::Ground
				&& cell.floorType != CellFloorType::Walkway))
				return reject("Furniture requires continuous Floor or Walkway support across its complete width");
		}
		for (auto const& instance : world.mFurniture)
		{
			auto const& other = *world.mFurnitureCatalogue->definition(instance.definitionKey);
			if (instance.sector == sectorIndex && instance.localDepth == localDepth && x + definition->minX < instance.x + other.maxX
				&& x + definition->maxX > instance.x + other.minX
				&& y + definition->minY < instance.y + other.maxY && y + definition->maxY > instance.y + other.minY)
				return reject("Furniture footprint overlaps at Local depth " + std::to_string(localDepth) + ": " + instance.name);
		}
		std::string reason;
		auto trimmed = Marker::trimName(name);
		if (!Marker::nameIsValid(trimmed, &reason)) return reject("Invalid Furniture name: " + reason);
		for (auto const& point : definition->usablePoints)
		{
			if (!world.mDeserializingConstruction)
			{
				auto markerName = trimmed + " " + point.label;
				if (!Marker::nameIsValid(markerName, &reason)) return reject("Invalid usable Marker name: " + reason);
				if (world.markerNameTaken(markerName)) return reject("A Marker with this name already exists");
			}
			if (!world.canAddSectorMarkerImpl(sectorIndex, static_cast<uint32_t>(y), x + point.x, &reason, true)) return reject(reason);
		}
		if (!world.mDeserializingConstruction && (!world.mNextFurnitureId || !world.mNextMarkerId
			|| definition->usablePoints.size() > std::numeric_limits<uint64_t>::max() - world.mNextMarkerId + 1))
			return reject("Furniture or Marker identity space is exhausted");
		if (world.mBuildFinished && !world.mSimulationPaused) return reject("Pause the simulation before placing Furniture");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint64_t FurnitureStore::placeFurniture(uint32_t sector, std::string const& key,
		float x, float y, std::string const& name, int localDepth)
	{
		std::string diagnostic;
		if (!canPlaceFurniture(sector, key, x, y, name, &diagnostic, localDepth))
			throw WorldException(&mWorld, diagnostic);
		auto const& definition = *mWorld.mFurnitureCatalogue->definition(key);
		World::ConstructionRecord record{ World::ConstructionType::Furniture };
		record.a = sector; record.x = x; record.y = y; record.furnitureDepth = localDepth;
		record.name = Marker::trimName(name); record.definitionKey = key;
		auto next = mWorld.mNextMarkerId;
		for (auto const& point : definition.usablePoints)
			record.furnitureDestinations.push_back({ point.key, MarkerId{ next++ }, record.name + " " + point.label,
				point.blocksPathing ? markerPropertyBit(MarkerProperty::BlocksPathing) : 0 });
		record.furnitureId = mWorld.mNextFurnitureId;
		restoreFurniture(record);
		++mWorld.mNextFurnitureId; mWorld.mNextMarkerId = next;
		return record.furnitureId;
	}

	bool FurnitureStore::canEditFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth) const
	{
		auto& world = mWorld;
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!world.mSimulationPaused) return reject("Pause the simulation before editing Furniture");
		auto records = world.mConstructionRecords;
		auto found = std::find_if(records.begin(), records.end(), [&](auto const& record) {
			return record.type == World::ConstructionType::Furniture && record.furnitureId == id;
		});
		if (found == records.end()) return reject("The Furniture instance no longer exists");
		// Moving a used instance finishes at the Agent's current usable point,
		// not at the candidate destination. Preflight must never run callbacks.
		for (auto const& [agentId, agent] : world.mAgents.entries())
			if (agent->mFurnitureUse && agent->mFurnitureUse->instance == id)
				if (!world.furniturePoseFits(agentId, agent->mFurnitureUse->marker,
					agent->mFurnitureUse->catalogue->definition(agent->mFurnitureUse->definition)->finishUsePose,
					false, diagnostic)) return false;
		found->x = x; found->y = y; found->name = Marker::trimName(name); found->furnitureDepth = localDepth.value_or(found->furnitureDepth);
		try
		{
			auto candidate = world.makeCandidateWorld();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
			world.validateReplayPoseState(*candidate, std::numeric_limits<uint32_t>::max(), 0, 0);
		}
		catch (std::exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool FurnitureStore::editFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth)
	{
		if (!canEditFurniture(id, x, y, name, diagnostic, localDepth)) return false;
		auto records = mWorld.mConstructionRecords;
		for (auto& record : records)
			if (record.type == World::ConstructionType::Furniture && record.furnitureId == id)
			{
				auto trimmed = Marker::trimName(name);
				auto depth = localDepth.value_or(record.furnitureDepth);
				if (record.x == x && record.y == y && record.name == trimmed && record.furnitureDepth == depth) return false;
				record.x = x; record.y = y; record.name = std::move(trimmed); record.furnitureDepth = depth;
				break;
			}
		// No sector translation: carried Agents stay at their physical positions.
		mWorld.rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	bool FurnitureStore::canRemoveFurniture(uint64_t id, std::string* diagnostic) const
	{
		auto& world = mWorld;
		if (diagnostic) diagnostic->clear();
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!world.mSimulationPaused) return reject("Pause the simulation before deleting Furniture");
		auto found = std::find_if(world.mFurniture.begin(), world.mFurniture.end(), [id](auto const& instance) { return instance.id == id; });
		if (found == world.mFurniture.end()) return reject("The Furniture instance no longer exists");
		for (auto const& [agentId, agent] : world.mAgents.entries())
			if (agent->mFurnitureUse && agent->mFurnitureUse->instance == id)
				if (!world.furniturePoseFits(agentId, agent->mFurnitureUse->marker,
					agent->mFurnitureUse->catalogue->definition(agent->mFurnitureUse->definition)->finishUsePose,
					false, diagnostic)) return false;
		std::string references;
		for (auto const& point : found->destinations)
		{
			std::string reason;
			if (!world.markerHasNoBehaviourReferences(point.marker, &reason)) references += reason + "\n";
		}
		if (!references.empty()) return reject(references);
		return true;
	}

	bool FurnitureStore::removeFurniture(uint64_t id, std::string* diagnostic)
	{
		if (!canRemoveFurniture(id, diagnostic)) return false;
		std::vector<World::ConstructionRecord> records;
		for (auto const& record : mWorld.mConstructionRecords)
		{
			if (record.type != World::ConstructionType::Furniture || record.furnitureId != id) records.push_back(record);
			else for (size_t i = 0; i < record.furnitureDestinations.size(); ++i)
			{
				// Preserve every owned object's slot for subsequent authored removals.
				World::ConstructionRecord tombstone{ World::ConstructionType::ObjectTombstone };
				tombstone.a = record.a;
				records.push_back(std::move(tombstone));
			}
		}
		mWorld.rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	std::string FurnitureStore::furnitureSupportDiagnostic(uint32_t sector, uint32_t x, uint32_t y) const
	{
		std::string result;
		for (auto const& instance : mWorld.mFurniture)
		{
			auto const& definition = *mWorld.mFurnitureCatalogue->definition(instance.definitionKey);
			if (instance.sector == sector && instance.y == y
				&& instance.x + definition.minX < x + 1.f && instance.x + definition.maxX > x)
				result += "\n- " + instance.name + " (" + std::to_string(instance.id) + ")";
		}
		return result.empty() ? result : "Floor removal would leave Furniture unsupported:" + result;
	}

	void FurnitureStore::restoreFurniture(World::ConstructionRecord const& record)
	{
		auto& world = mWorld;
		if (!world.mFurnitureCatalogue) throw WorldException(&world, "Missing Furniture catalogue dependency");
		auto definition = world.mFurnitureCatalogue->definition(record.definitionKey);
		if (!definition) throw WorldException(&world, "Missing Furniture definition: " + record.definitionKey);
		if (record.furnitureDestinations.size() != definition->usablePoints.size())
			throw WorldException(&world, "Furniture usable-point layout does not match saved destinations");
		std::vector<std::pair<FurnitureDestination const*, FurnitureUsablePoint const*>> points;
		for (auto const& saved : record.furnitureDestinations)
		{
			auto point = std::find_if(definition->usablePoints.begin(), definition->usablePoints.end(),
				[&](auto const& p) { return p.key == saved.key; });
			if (point == definition->usablePoints.end())
				throw WorldException(&world, "Missing Furniture usable-point key: " + saved.key);
			if (std::any_of(points.begin(), points.end(), [&](auto const& p) { return p.first->key == saved.key; }))
				throw WorldException(&world, "Duplicate Furniture usable-point key: " + saved.key);
			if (!saved.marker || world.lookupMarker(saved.marker) || !Marker::nameIsValid(saved.name, nullptr)
				|| Marker::trimName(saved.name) != saved.name || world.markerNameTaken(saved.name)
				|| (saved.properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
				|| std::any_of(points.begin(), points.end(), [&](auto const& p) {
					return p.first->marker == saved.marker || p.first->name == saved.name; }))
				throw WorldException(&world, "Invalid or duplicate Furniture Marker identity, name or properties");
			points.emplace_back(&saved, &*point);
		}
		// Geometry and overlap use exactly the authoring contract. The independent
		// saved Marker name may differ from its initial generated name.
		std::string diagnostic;
		if (!canPlaceFurniture(record.a, record.definitionKey, record.x, record.y, record.name, &diagnostic, record.furnitureDepth))
			throw WorldException(&world, "Furniture '" + record.name + "' (" + std::to_string(record.furnitureId) + "): " + diagnostic);
		if (!record.furnitureId || std::any_of(world.mFurniture.begin(), world.mFurniture.end(),
			[&](auto const& i) { return i.id == record.furnitureId; }))
			throw WorldException(&world, "Furniture instance identity is zero or duplicated");
		// Nested Marker creation must not produce an independent authored layout.
		auto old = world.mDeserializingConstruction;
		world.mDeserializingConstruction = true;
		try
		{
			for (auto const& [saved, point] : points)
				world.addSectorMarkerRestored(record.a, static_cast<uint32_t>(record.y), record.x + point->x,
					saved->marker, saved->name, saved->properties, nullptr, true);
		}
		catch (...) { world.mDeserializingConstruction = old; throw; }
		world.mDeserializingConstruction = old;
		world.mFurniture.push_back({ record.furnitureId, record.a, record.x, record.y,
			record.definitionKey, record.name, record.furnitureDestinations,
			record.furnitureDestinations.empty() ? MarkerId{} : record.furnitureDestinations.front().marker, record.furnitureDepth });
		world.recordConstruction(record);
	}

	// ----------------------------------------------------------------------
	// Usable-point resolution
	// ----------------------------------------------------------------------

	World::ResolvedUsablePoint FurnitureStore::resolveUsablePoint(MarkerId marker,
		std::shared_ptr<const FurnitureCatalogue> const& catalogue) const
	{
		// One pass over the instances resolves the whole Marker -> instance,
		// definition, destination, usable-point support and Sector chain.
		// The result borrows from the current World and catalogue and is
		// meant to live only inside the calling evaluation (#627).
		World::ResolvedUsablePoint resolved;
		auto const& activeCatalogue = catalogue ? catalogue : mWorld.mFurnitureCatalogue;
		if (!marker || !activeCatalogue) return resolved;
		for (auto const& instance : mWorld.mFurniture)
		{
			++mWorld.mUsablePointResolveVisits;
			auto const destination = std::find_if(instance.destinations.begin(), instance.destinations.end(),
				[&](auto const& point) { return point.marker == marker; });
			if (destination == instance.destinations.end()) continue;
			auto const definition = activeCatalogue->definition(instance.definitionKey);
			if (!definition) return resolved;
			auto const point = std::find_if(definition->usablePoints.begin(), definition->usablePoints.end(),
				[&](auto const& candidate) { return candidate.key == destination->key; });
			resolved.marker = marker;
			resolved.instance = &instance;
			resolved.definition = definition;
			resolved.destination = &*destination;
			resolved.point = point != definition->usablePoints.end() ? &*point : nullptr;
			resolved.sector = mWorld.mSectors.at(instance.sector).get();
			return resolved;
		}
		return resolved;
	}

	FurnitureInstance const* FurnitureStore::furnitureForMarker(MarkerId marker) const
	{
		for (auto const& instance : mWorld.mFurniture)
			for (auto const& point : instance.destinations)
				if (point.marker == marker) return &instance;
		return nullptr;
	}
}
