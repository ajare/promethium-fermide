#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/MarkerStore.h"

#include "core/Exceptions.h"
#include "core/Layer.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "core/Sector.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	MarkerStore::MarkerStore(World& world)
		: mWorld(world)
	{
	}

	// ----------------------------------------------------------------------
	// Identity and lookup
	// ----------------------------------------------------------------------

	bool MarkerStore::markerNameTaken(string const& trimmed, MarkerId except) const
	{
		// Inspect objects directly: getMarkerIds() followed by lookupMarker()
		// rescans all objects for every identity, making repeated insertion
		// cubic (including both validated restoration replays).
		for (auto const& sector : mWorld.mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (!object) continue;
				auto marker = object->getMarker();
				if (marker->getId() != except && marker->getName() == trimmed) return true;
			}
		}
		return false;
	}

	string MarkerStore::nextGeneratedMarkerName() const
	{
		for (uint64_t suffix = 1; suffix != 0; ++suffix)
		{
			auto candidate = format("Marker {}", suffix);
			if (!markerNameTaken(candidate)) return candidate;
		}
		throw WorldException(&mWorld, "No unique generated Marker name is available");
	}

	shared_ptr<Marker> MarkerStore::mutableMarker(MarkerId id) const
	{
		for (auto const& sector : mWorld.mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (!object) continue;
				auto marker = const_pointer_cast<Marker>(object->getMarker());
				if (marker->getId() == id) return marker;
			}
		}
		return nullptr;
	}

	vector<MarkerId> MarkerStore::getMarkerIds() const
	{
		vector<MarkerId> result;
		for (auto const& sector : mWorld.mSectors)
		{
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(i));
				if (object) result.push_back(object->getMarker()->getId());
			}
		}
		sort(result.begin(), result.end());
		return result;
	}

	shared_ptr<const Marker> MarkerStore::lookupMarker(MarkerId id) const
	{
		return mutableMarker(id);
	}

	// ----------------------------------------------------------------------
	// Creation
	// ----------------------------------------------------------------------

	World::CreateObjectResult MarkerStore::createMarker(uint32_t layerIndex, uint32_t x,
		uint32_t y, float xOffset, MarkerId id, string name,
		MarkerProperties properties, uint32_t* vertexIdentifier)
	{
		mWorld.invalidateSimulationSnapshot();
		string caller = format("World::createMarker({}, {}, {}, {})", layerIndex, x, y, xOffset);

		float xPos = x + xOffset;
		x = (uint32_t)xPos;

		mWorld.validateCellOccupied(caller, layerIndex, x, y);

		// Create Marker
		auto layer = mWorld.getLayer(layerIndex);
		auto const& cellDef = layer->getCellDefinition(x, y);
		auto sector = mWorld._getSector(cellDef.sectorIndex);

		return {
			sector->createMarker(sector, id, std::move(name), properties,
				x, y, xPos - x, vertexIdentifier),
			SectorObjectType::Marker,
			sector
		};
	}

	World::CreateObjectResult MarkerStore::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, uint32_t* vertexIdentifier)
	{
		mWorld.invalidateSimulationSnapshot();
		return addSectorMarker(sectorIndex, levelIndex, xOffset,
			nextGeneratedMarkerName(), vertexIdentifier);
	}

	World::CreateObjectResult MarkerStore::addSectorMarker(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, string const& name, uint32_t* vertexIdentifier)
	{
		mWorld.invalidateSimulationSnapshot();
		string diagnostic;
		if (!mWorld.canAddSectorMarker(sectorIndex, levelIndex, xOffset, &diagnostic))
			throw WorldException(&mWorld, "World::addSectorMarker - " + diagnostic);
		auto const trimmed = Marker::trimName(name);
		if (!Marker::nameIsValid(trimmed, &diagnostic))
			throw WorldException(&mWorld, "World::addSectorMarker - " + diagnostic);
		if (markerNameTaken(trimmed))
			throw WorldException(&mWorld, "World::addSectorMarker - A Marker with this name already exists");
		if (mWorld.mNextMarkerId == 0)
			throw WorldException(&mWorld, "World::addSectorMarker - Marker ID space is exhausted");
		auto const id = MarkerId{ mWorld.mNextMarkerId };
		mWorld.mNextMarkerId = mWorld.mNextMarkerId == numeric_limits<uint64_t>::max() ? 0
			: mWorld.mNextMarkerId + 1;
		return addSectorMarkerRestored(sectorIndex, levelIndex, xOffset, id, trimmed,
			0, vertexIdentifier);
	}

	World::CreateObjectResult MarkerStore::addSectorMarkerRestored(uint32_t sectorIndex,
		uint32_t levelIndex, float xOffset, MarkerId id, string name,
		MarkerProperties properties, uint32_t* vertexIdentifier, bool allowCoincident)
	{
		mWorld.invalidateSimulationSnapshot();
		string diagnostic;
		if (!id) throw WorldException(&mWorld, "Marker ID cannot be zero");
		if (properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
			throw WorldException(&mWorld, "Marker properties contain unknown bits");
		if (lookupMarker(id)) throw WorldException(&mWorld, "Marker ID is already in use");
		if (!mWorld.canAddSectorMarkerImpl(sectorIndex, levelIndex, xOffset, &diagnostic, allowCoincident))
			throw WorldException(&mWorld, "World::addSectorMarker - " + diagnostic);
		name = Marker::trimName(name);
		if (!Marker::nameIsValid(name, &diagnostic))
			throw WorldException(&mWorld, "World::addSectorMarker - " + diagnostic);
		if (markerNameTaken(name))
			throw WorldException(&mWorld, "World::addSectorMarker - A Marker with this name already exists");
		mWorld.beginStructuralEdit("addSectorMarker");

		auto sector = mWorld._getSector(sectorIndex);
		auto layerIndex = sector->getLayerIndex();
		auto layer = mWorld.getLayer(layerIndex);
		auto& cellDef = layer->getCellDefinition(sector->getCellX() + (uint32_t)xOffset,
			sector->getCellY() + levelIndex);
		auto createdMarker = createMarker(layerIndex, sector->getCellX(),
			sector->getCellY() + levelIndex, xOffset, id, name, properties,
			vertexIdentifier);
		cellDef.markers.push_back(createdMarker.index);
		World::ConstructionRecord record{ World::ConstructionType::Marker };
		record.a = sectorIndex; record.b = levelIndex; record.c = properties;
		record.x = xOffset;
		record.markerId = id; record.name = std::move(name);
		mWorld.recordConstruction(std::move(record));
		return createdMarker;
	}

	// ----------------------------------------------------------------------
	// Editing
	// ----------------------------------------------------------------------

	bool MarkerStore::canRenameMarker(MarkerId id, string const& name, string* diagnostic) const
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (!id || !lookupMarker(id)) return reject("Marker does not exist");
		auto const trimmed = Marker::trimName(name);
		string reason;
		if (!Marker::nameIsValid(trimmed, &reason)) return reject(std::move(reason));
		if (markerNameTaken(trimmed, id)) return reject("A Marker with this name already exists");
		if (diagnostic) diagnostic->clear();
		return true;
	}

	bool MarkerStore::renameMarker(MarkerId id, string const& name, string* diagnostic)
	{
		if (!canRenameMarker(id, name, diagnostic)) return false;
		auto const trimmed = Marker::trimName(name);
		auto marker = mutableMarker(id);
		if (marker->getName() == trimmed) return true;
		auto record = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[id](World::ConstructionRecord const& candidate)
			{
				return (candidate.type == World::ConstructionType::Marker && candidate.markerId == id)
					|| (candidate.type == World::ConstructionType::Furniture && std::any_of(candidate.furnitureDestinations.begin(),
						candidate.furnitureDestinations.end(), [id](auto const& p) { return p.marker == id; }));
			});
		if (record == mWorld.mConstructionRecords.end())
			throw WorldException(&mWorld, "renameMarker - Marker has no authored record");
		mWorld.invalidateSimulationSnapshot();
		marker->setName(trimmed);
		if (record->type == World::ConstructionType::Furniture)
		{
			for (auto& point : record->furnitureDestinations) if (point.marker == id) point.name = trimmed;
			for (auto& instance : mWorld.mFurniture)
				for (auto& point : instance.destinations) if (point.marker == id) point.name = trimmed;
		}
		else record->name = trimmed;
		mWorld.modify();
		return true;
	}

	bool MarkerStore::setMarkerProperties(MarkerId id, MarkerProperties properties,
		string* diagnostic)
	{
		auto reject = [diagnostic](string reason)
		{
			if (diagnostic) *diagnostic = std::move(reason);
			return false;
		};
		if (properties & ~markerPropertyBit(MarkerProperty::BlocksPathing))
			return reject("Marker properties contain unknown bits");
		auto marker = mutableMarker(id);
		if (!marker) return reject("Marker does not exist");
		if (mWorld.mBuildFinished && !mWorld.mSimulationPaused)
			return reject("Marker pathing properties can only be changed while the simulation is paused");
		if (marker->getProperties() == properties)
		{
			if (diagnostic) diagnostic->clear();
			return true;
		}
		auto record = find_if(mWorld.mConstructionRecords.begin(), mWorld.mConstructionRecords.end(),
			[id](World::ConstructionRecord const& candidate)
			{
				return (candidate.type == World::ConstructionType::Marker && candidate.markerId == id)
					|| (candidate.type == World::ConstructionType::Furniture && std::any_of(candidate.furnitureDestinations.begin(),
						candidate.furnitureDestinations.end(), [id](auto const& p) { return p.marker == id; }));
			});
		if (record == mWorld.mConstructionRecords.end())
			throw WorldException(&mWorld, "setMarkerProperties - Marker has no authored record");
		mWorld.invalidateSimulationSnapshot();
		marker->setProperties(properties);
		if (record->type == World::ConstructionType::Furniture)
		{
			for (auto& point : record->furnitureDestinations) if (point.marker == id) point.properties = properties;
			for (auto& instance : mWorld.mFurniture)
				for (auto& point : instance.destinations) if (point.marker == id) point.properties = properties;
		}
		else record->c = properties;
		mWorld.modify();
		if (diagnostic) diagnostic->clear();
		return true;
	}

	// ----------------------------------------------------------------------
	// Removal
	// ----------------------------------------------------------------------

	bool MarkerStore::canRemoveSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
		string* diagnostic) const
	{
		if (diagnostic) diagnostic->clear();
		auto reject = [diagnostic](string message)
		{
			if (diagnostic) *diagnostic = std::move(message);
			return false;
		};
		if (sectorIndex >= mWorld.mSectors.size()) return reject("The Marker Sector does not exist");
		auto const sector = mWorld.mSectors[sectorIndex];
		if (!sector || objectIndex >= sector->getNumObjects())
			return reject("The Marker object does not exist");
		auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(objectIndex));
		if (!markerObject) return reject("The selected object is not a Marker");
		auto const marker = markerObject->getMarker()->getId();
		if (mWorld.isFurnitureMarker(marker)) return reject("A Furniture-owned Marker cannot be deleted independently");
		return mWorld.markerHasNoBehaviourReferences(marker, diagnostic);
	}

	bool MarkerStore::removeSectorMarker(uint32_t sectorIndex, uint32_t objectIndex,
		string* diagnostic)
	{
		if (!canRemoveSectorMarker(sectorIndex, objectIndex, diagnostic)) return false;
		auto sector = mWorld._getSector(sectorIndex);
		auto markerObject = dynamic_pointer_cast<MarkerSectorObject>(sector->getObject(objectIndex));

		mWorld.beginStructuralEdit("removeSectorMarker");
		auto const marker = markerObject->getMarker();
		auto& cellDef = mWorld.mLayers[sector->getLayerIndex()]->getCellDefinition(
			marker->getCellX(), marker->getCellY());
		auto const found = find(cellDef.markers.begin(), cellDef.markers.end(), objectIndex);
		if (found == cellDef.markers.end())
			throw WorldException(&mWorld, "removeSectorMarker - Marker is not registered in its cell");
		cellDef.markers.erase(found);
		if (!sector->removeSectorObject(objectIndex)) return false;

		World::ConstructionRecord record{ World::ConstructionType::RemoveMarker };
		record.a = sectorIndex;
		record.b = objectIndex;
		record.markerId = marker->getId();
		mWorld.recordConstruction(std::move(record));
		return true;
	}
}
