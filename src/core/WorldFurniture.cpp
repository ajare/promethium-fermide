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
			[id](auto const& instance) { return instance.marker == id; });
	}

	bool World::canPlaceFurniture(uint32_t sectorIndex, std::string const& key,
		float x, float y, std::string const& name, std::string* diagnostic) const
	{
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!mFurnitureCatalogue) return reject("No Furniture catalogue is loaded");
		auto definition = mFurnitureCatalogue->definition(key);
		if (!definition) return reject("Missing Furniture definition: " + key);
		if (sectorIndex >= mSectors.size() || !mSectors[sectorIndex]
			|| !isLocationLike(mSectors[sectorIndex]->getType()))
			return reject("Furniture requires a Room, Corridor or Facade");
		auto sector = mSectors[sectorIndex];
		if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0
			|| std::floor(y) != y || y >= sector->getLevelsHigh()
			|| x + 1 > sector->getCellsWide())
			return reject("Furniture requires floor-aligned y and its complete width inside one Location");
		for (auto cellX = static_cast<uint32_t>(std::floor(x)); cellX < static_cast<uint32_t>(std::ceil(x + 1)); ++cellX)
		{
			auto const& cell = mLayers[sector->getLayerIndex()]->getCellDefinition(
				sector->getCellX() + cellX, sector->getCellY() + static_cast<uint32_t>(y));
			if (cell.sectorIndex != sectorIndex || (cell.floorType != CellFloorType::Ground
				&& cell.floorType != CellFloorType::Walkway))
				return reject("Furniture requires continuous Floor or Walkway support across its complete width");
		}
		for (auto const& instance : mFurniture)
			if (instance.sector == sectorIndex && x < instance.x + 1 && x + 1 > instance.x
				&& y < instance.y + 1 && y + 1 > instance.y)
				return reject("Furniture footprint overlaps at depth 0: " + instance.name);
		std::string reason;
		auto trimmed = Marker::trimName(name);
		if (!Marker::nameIsValid(trimmed, &reason)) return reject("Invalid Furniture name: " + reason);
		if (!mDeserializingConstruction)
		{
			auto markerName = trimmed + " " + definition->usableLabel;
			if (!Marker::nameIsValid(markerName, &reason)) return reject("Invalid usable Marker name: " + reason);
			if (markerNameTaken(markerName)) return reject("A Marker with this name already exists");
		}
		if (!canAddSectorMarker(sectorIndex, static_cast<uint32_t>(y), x + definition->usableX, &reason))
			return reject(reason);
		if (!mDeserializingConstruction && (!mNextFurnitureId || !mNextMarkerId)) return reject("Furniture or Marker identity space is exhausted");
		if (mBuildFinished && !mSimulationPaused) return reject("Pause the simulation before placing Furniture");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	uint64_t World::placeFurniture(uint32_t sector, std::string const& key,
		float x, float y, std::string const& name)
	{
		std::string diagnostic;
		if (!canPlaceFurniture(sector, key, x, y, name, &diagnostic))
			throw WorldException(this, diagnostic);
		auto const& definition = *mFurnitureCatalogue->definition(key);
		ConstructionRecord record{ ConstructionType::Furniture };
		record.a = sector; record.x = x; record.y = y;
		record.name = Marker::trimName(name); record.definitionKey = key;
		record.usableKey = definition.usableKey;
		record.markerName = record.name + " " + definition.usableLabel;
		record.markerId = MarkerId{ mNextMarkerId };
		record.furnitureId = mNextFurnitureId;
		record.c = markerPropertyBit(MarkerProperty::BlocksPathing);
		restoreFurniture(record);
		++mNextFurnitureId; ++mNextMarkerId;
		return record.furnitureId;
	}

	bool World::canEditFurniture(uint64_t id, float x, float y,
		std::string const& name, std::string* diagnostic) const
	{
		auto reject = [&](std::string reason) { if (diagnostic) *diagnostic = std::move(reason); return false; };
		if (!mSimulationPaused) return reject("Pause the simulation before editing Furniture");
		auto records = mConstructionRecords;
		auto found = std::find_if(records.begin(), records.end(), [&](auto const& record) {
			return record.type == ConstructionType::Furniture && record.furnitureId == id;
		});
		if (found == records.end()) return reject("The Furniture instance no longer exists");
		found->x = x; found->y = y; found->name = Marker::trimName(name);
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
		std::string const& name, std::string* diagnostic)
	{
		if (!canEditFurniture(id, x, y, name, diagnostic)) return false;
		auto records = mConstructionRecords;
		for (auto& record : records)
			if (record.type == ConstructionType::Furniture && record.furnitureId == id)
			{
				auto trimmed = Marker::trimName(name);
				if (record.x == x && record.y == y && record.name == trimmed) return false;
				record.x = x; record.y = y; record.name = std::move(trimmed);
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
		return markerHasNoBehaviourReferences(found->marker, diagnostic);
	}

	bool World::removeFurniture(uint64_t id, std::string* diagnostic)
	{
		if (!canRemoveFurniture(id, diagnostic)) return false;
		auto records = mConstructionRecords;
		for (auto& record : records)
			if (record.type == ConstructionType::Furniture && record.furnitureId == id)
			{
				// Preserve other objects' authored slots, but never the destination.
				ConstructionRecord tombstone{ ConstructionType::ObjectTombstone };
				tombstone.a = record.a;
				record = std::move(tombstone);
				break;
			}
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}

	std::string World::furnitureSupportDiagnostic(uint32_t sector, uint32_t x, uint32_t y) const
	{
		std::string result;
		for (auto const& instance : mFurniture)
			if (instance.sector == sector && instance.y == y && instance.x < x + 1.f && instance.x + 1.f > x)
				result += "\n- " + instance.name + " (" + std::to_string(instance.id) + ")";
		return result.empty() ? result : "Floor removal would leave Furniture unsupported:" + result;
	}

	void World::restoreFurniture(ConstructionRecord const& record)
	{
		if (!mFurnitureCatalogue) throw WorldException(this, "Missing Furniture catalogue dependency");
		auto definition = mFurnitureCatalogue->definition(record.definitionKey);
		if (!definition) throw WorldException(this, "Missing Furniture definition: " + record.definitionKey);
		if (definition->usableKey != record.usableKey)
			throw WorldException(this, "Missing Furniture usable-point key: " + record.usableKey);
		// Geometry and overlap use exactly the authoring contract. The independent
		// saved Marker name may differ from its initial generated name.
		std::string diagnostic;
		if (!canPlaceFurniture(record.a, record.definitionKey, record.x, record.y, record.name, &diagnostic))
			throw WorldException(this, "Furniture '" + record.name + "' (" + std::to_string(record.furnitureId) + "): " + diagnostic);
		if (!record.furnitureId || std::any_of(mFurniture.begin(), mFurniture.end(),
			[&](auto const& i) { return i.id == record.furnitureId; }))
			throw WorldException(this, "Furniture instance identity is zero or duplicated");
		// Nested Marker creation must not produce an independent authored layout.
		auto old = mDeserializingConstruction;
		mDeserializingConstruction = true;
		try
		{
			addSectorMarkerRestored(record.a, static_cast<uint32_t>(record.y), record.x + definition->usableX,
				record.markerId, record.markerName, record.c);
		}
		catch (...) { mDeserializingConstruction = old; throw; }
		mDeserializingConstruction = old;
		mFurniture.push_back({ record.furnitureId, record.a, record.x, record.y,
			record.definitionKey, record.name, record.usableKey, record.markerId });
		recordConstruction(record);
	}
}
