#include "core/World.h"
#include "core/Exceptions.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>

namespace core
{
	void World::attachFurnitureCatalogue(std::string filename,
		std::shared_ptr<const FurnitureCatalogue> catalogue)
	{
		std::filesystem::path path(filename);
		if (!catalogue || filename.empty() || path.has_parent_path()
			|| !filename.ends_with(".furniture.yaml"))
			throw WorldException(this, "Furniture catalogue reference must be a .furniture.yaml basename");
		if (!mFurniture.empty() && (filename != mFurnitureCatalogueFilename || catalogue != mFurnitureCatalogue))
			throw WorldException(this, "Cannot replace a Furniture catalogue while instances use it");
		mFurnitureCatalogue = std::move(catalogue);
		mFurnitureCatalogueFilename = std::move(filename);
		modify();
	}

	bool World::isFurnitureMarker(MarkerId id) const
	{
		return std::any_of(mFurniture.begin(), mFurniture.end(),
			[id](auto const& instance) { return std::any_of(instance.destinations.begin(), instance.destinations.end(),
				[id](auto const& point) { return point.marker == id; }); });
	}

	bool World::canPlaceFurniture(uint32_t sectorIndex, std::string const& key,
		float x, float y, std::string const& name, std::string* diagnostic, int localDepth) const
	{
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!mFurnitureCatalogue) return reject("No Furniture catalogue is loaded");
		auto definition = mFurnitureCatalogue->definition(key);
		if (!definition) return reject("Missing Furniture definition: " + key);
		if (localDepth < 0) return reject("Furniture Local depth must be non-negative");
		for (auto const& edge : definition->edges)
			if (edge.depthOffset && (int64_t{ localDepth } + *edge.depthOffset < 0
				|| int64_t{ localDepth } + *edge.depthOffset > std::numeric_limits<int>::max()))
				return reject("Furniture resolved edge depth must be non-negative and representable");
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| !isLocationLike(mSectors[sectorIndex]->getType()))
			return reject("Furniture requires a Room, Corridor or Facade");
		auto sector = mSectors[sectorIndex];
		if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0
			|| std::floor(y) != y || y >= sector->getLevelsHigh()
			|| x + definition->minX < 0 || x + definition->maxX > sector->getCellsWide()
			|| y + definition->maxY > sector->getLevelsHigh())
			return reject("Furniture requires floor-aligned y and its complete width inside one Location");
		for (auto cellX = static_cast<uint32_t>(std::floor(x + definition->minX)); cellX < static_cast<uint32_t>(std::ceil(x + definition->maxX)); ++cellX)
		{
			auto const& cell = mLayers[sector->getLayerIndex()]->getCellDefinition(
				sector->getCellX() + cellX, sector->getCellY() + static_cast<uint32_t>(y));
			if (cell.sectorIndex != sectorIndex || (cell.floorType != CellFloorType::Ground
				&& cell.floorType != CellFloorType::Walkway))
				return reject("Furniture requires continuous Floor or Walkway support across its complete width");
		}
		for (auto const& instance : mFurniture)
		{
			auto const& other = *mFurnitureCatalogue->definition(instance.definitionKey);
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
			if (!mDeserializingConstruction)
			{
				auto markerName = trimmed + " " + point.label;
				if (!Marker::nameIsValid(markerName, &reason)) return reject("Invalid usable Marker name: " + reason);
				if (markerNameTaken(markerName)) return reject("A Marker with this name already exists");
			}
			if (!canAddSectorMarkerImpl(sectorIndex, static_cast<uint32_t>(y), x + point.x, &reason, true)) return reject(reason);
		}
		if (!mDeserializingConstruction && (!mNextFurnitureId || !mNextMarkerId
			|| definition->usablePoints.size() > std::numeric_limits<uint64_t>::max() - mNextMarkerId + 1))
			return reject("Furniture or Marker identity space is exhausted");
		if (mBuildFinished && !mSimulationPaused) return reject("Pause the simulation before placing Furniture");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint64_t World::placeFurniture(uint32_t sector, std::string const& key,
		float x, float y, std::string const& name, int localDepth)
	{
		std::string diagnostic;
		if (!canPlaceFurniture(sector, key, x, y, name, &diagnostic, localDepth))
			throw WorldException(this, diagnostic);
		auto const& definition = *mFurnitureCatalogue->definition(key);
		ConstructionRecord record{ ConstructionType::Furniture };
		record.a = sector; record.x = x; record.y = y; record.furnitureDepth = localDepth;
		record.name = Marker::trimName(name); record.definitionKey = key;
		auto next = mNextMarkerId;
		for (auto const& point : definition.usablePoints)
			record.furnitureDestinations.push_back({ point.key, MarkerId{ next++ }, record.name + " " + point.label,
				markerPropertyBit(MarkerProperty::BlocksPathing) });
		record.furnitureId = mNextFurnitureId;
		restoreFurniture(record);
		++mNextFurnitureId; mNextMarkerId = next;
		return record.furnitureId;
	}

	bool World::canEditFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth) const
	{
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!mSimulationPaused) return reject("Pause the simulation before editing Furniture");
		auto records = mConstructionRecords;
		auto found = std::find_if(records.begin(), records.end(), [&](auto const& record) {
			return record.type == ConstructionType::Furniture && record.furnitureId == id;
		});
		if (found == records.end()) return reject("The Furniture instance no longer exists");
		found->x = x; found->y = y; found->name = Marker::trimName(name); found->furnitureDepth = localDepth.value_or(found->furnitureDepth);
		try
		{
			auto candidate = makeCandidateWorld();
			candidate->mDeserializingConstruction = true;
			for (auto const& record : records) candidate->applyConstructionRecord(record);
			candidate->finishBuild();
		}
		catch (std::exception const& error) { return reject(error.what()); }
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool World::editFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic, std::optional<int> localDepth)
	{
		if (!canEditFurniture(id, x, y, name, diagnostic, localDepth)) return false;
		auto records = mConstructionRecords;
		for (auto& record : records)
			if (record.type == ConstructionType::Furniture && record.furnitureId == id)
			{
				auto trimmed = Marker::trimName(name);
				auto depth = localDepth.value_or(record.furnitureDepth);
				if (record.x == x && record.y == y && record.name == trimmed && record.furnitureDepth == depth) return false;
				record.x = x; record.y = y; record.name = std::move(trimmed); record.furnitureDepth = depth;
				break;
			}
		// No sector translation: carried Agents stay at their physical positions.
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	bool World::canRemoveFurniture(uint64_t id, std::string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!mSimulationPaused) return reject("Pause the simulation before deleting Furniture");
		auto found = std::find_if(mFurniture.begin(), mFurniture.end(), [id](auto const& instance) { return instance.id == id; });
		if (found == mFurniture.end()) return reject("The Furniture instance no longer exists");
		std::string references;
		for (auto const& point : found->destinations)
		{
			std::string reason;
			if (!markerHasNoBehaviourReferences(point.marker, &reason)) references += reason + "\n";
		}
		if (!references.empty()) return reject(references);
		return true;
	}

	bool World::removeFurniture(uint64_t id, std::string* diagnostic)
	{
		if (!canRemoveFurniture(id, diagnostic)) return false;
		std::vector<ConstructionRecord> records;
		for (auto const& record : mConstructionRecords)
		{
			if (record.type != ConstructionType::Furniture || record.furnitureId != id) records.push_back(record);
			else for (size_t i = 0; i < record.furnitureDestinations.size(); ++i)
			{
				// Preserve every owned object's slot for subsequent authored removals.
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = record.a;
				records.push_back(std::move(tombstone));
			}
		}
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	std::string World::furnitureSupportDiagnostic(uint32_t sector, uint32_t x, uint32_t y) const
	{
		std::string result;
		for (auto const& instance : mFurniture)
		{
			auto const& definition = *mFurnitureCatalogue->definition(instance.definitionKey);
			if (instance.sector == sector && instance.y == y
				&& instance.x + definition.minX < x + 1.f && instance.x + definition.maxX > x)
				result += "\n- " + instance.name + " (" + std::to_string(instance.id) + ")";
		}
		return result.empty() ? result : "Floor removal would leave Furniture unsupported:" + result;
	}

	void World::restoreFurniture(ConstructionRecord const& record)
	{
		if (!mFurnitureCatalogue) throw WorldException(this, "Missing Furniture catalogue dependency");
		auto definition = mFurnitureCatalogue->definition(record.definitionKey);
		if (!definition) throw WorldException(this, "Missing Furniture definition: " + record.definitionKey);
		if (record.furnitureDestinations.size() != definition->usablePoints.size())
			throw WorldException(this, "Furniture usable-point layout does not match saved destinations");
		std::vector<std::pair<FurnitureDestination const*, FurnitureUsablePoint const*>> points;
		for (auto const& saved : record.furnitureDestinations)
		{
			auto point = std::find_if(definition->usablePoints.begin(), definition->usablePoints.end(),
				[&](auto const& p) { return p.key == saved.key; });
			if (point == definition->usablePoints.end())
				throw WorldException(this, "Missing Furniture usable-point key: " + saved.key);
			if (std::any_of(points.begin(), points.end(), [&](auto const& p) { return p.first->key == saved.key; }))
				throw WorldException(this, "Duplicate Furniture usable-point key: " + saved.key);
			if (!saved.marker || lookupMarker(saved.marker) || !Marker::nameIsValid(saved.name, nullptr)
				|| Marker::trimName(saved.name) != saved.name || markerNameTaken(saved.name)
				|| (saved.properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
				|| std::any_of(points.begin(), points.end(), [&](auto const& p) {
					return p.first->marker == saved.marker || p.first->name == saved.name; }))
				throw WorldException(this, "Invalid or duplicate Furniture Marker identity, name or properties");
			points.emplace_back(&saved, &*point);
		}
		// Geometry and overlap use exactly the authoring contract. The independent
		// saved Marker name may differ from its initial generated name.
		std::string diagnostic;
		if (!canPlaceFurniture(record.a, record.definitionKey, record.x, record.y, record.name, &diagnostic, record.furnitureDepth))
			throw WorldException(this, "Furniture '" + record.name + "' (" + std::to_string(record.furnitureId) + "): " + diagnostic);
		if (!record.furnitureId || std::any_of(mFurniture.begin(), mFurniture.end(),
			[&](auto const& i) { return i.id == record.furnitureId; }))
			throw WorldException(this, "Furniture instance identity is zero or duplicated");
		// Nested Marker creation must not produce an independent authored layout.
		auto old = mDeserializingConstruction;
		mDeserializingConstruction = true;
		try
		{
			for (auto const& [saved, point] : points)
				addSectorMarkerRestored(record.a, static_cast<uint32_t>(record.y), record.x + point->x,
					saved->marker, saved->name, saved->properties, nullptr, true);
		}
		catch (...) { mDeserializingConstruction = old; throw; }
		mDeserializingConstruction = old;
		mFurniture.push_back({ record.furnitureId, record.a, record.x, record.y,
			record.definitionKey, record.name, record.furnitureDestinations, record.furnitureDestinations.front().marker, record.furnitureDepth });
		recordConstruction(record);
	}
}
