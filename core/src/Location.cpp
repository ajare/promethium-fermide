#include <format>
#include <cassert>
#include <cmath>

#include "core/Defines.h"
#include "core/Location.h"
#include "core/DoorSectorObject.h"
#include "core/WindowSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/ButtonSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"


namespace core
{

	using namespace std;

	Location::Location(string const& name, SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY, uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity, bool isCorridor)
		: Sector(type, layerIndex, index, cellX, cellY, 0.0f, 0.0f, (float)cellsWide, (float)((levelsHigh - 1) + topLevelHeight), name, cellsWide, levelsHigh, topLevelHeight, capacity)
		, mIsCorridor(isCorridor)
	{
	}

	string Location::getDescription() const
	{
		return format("{} at {},{} on Layer {}", getName(), getCellX(), getCellY(), getLayerIndex());
	}

	bool Location::sectorSupportsObjectType(SectorObjectType type) const
	{
		return type == SectorObjectType::BulkheadDoor ||
			type == SectorObjectType::Door ||
			type == SectorObjectType::ForceBridge ||
			type == SectorObjectType::InteractionPoint ||
			type == SectorObjectType::Ladder ||
			type == SectorObjectType::Lift ||
			type == SectorObjectType::Marker ||
			type == SectorObjectType::AccessPanel ||
			type == SectorObjectType::Walkway ||
			isWindowAperture(type);
	}

	float Location::getEffectiveTopLevelHeight() const
	{
		return mHeightScale ? CORE_ROOM_STANDARD_HEIGHT * *mHeightScale : Sector::getEffectiveTopLevelHeight();
	}

	bool Location::roomHeightScaleIsValid(std::optional<float> scale)
	{
		return !scale || (std::isfinite(*scale)
			&& *scale >= CORE_ROOM_HEIGHT_SCALE_MIN && *scale <= CORE_ROOM_HEIGHT_SCALE_MAX);
	}

	bool Location::setHeightScale(std::optional<float> scale)
	{
		if (!isRoom() || getLevelsHigh() != 1 || !roomHeightScaleIsValid(scale)) return false;
		mHeightScale = scale;
		setSize({ getSize().x, (float)(getLevelsHigh() - 1) + getEffectiveTopLevelHeight() });
		return true;
	}

} // core