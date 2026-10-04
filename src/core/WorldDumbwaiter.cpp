#include "core/World.h"
#include "core/Exceptions.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace core
{
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
				record.c = options.initialStop; record.x = options.travelSeconds;
				rebuildFromConstructionRecords(std::move(records));
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
		rebuildFromConstructionRecords(std::move(records));
		return true;
	}
}
