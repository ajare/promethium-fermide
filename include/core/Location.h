#pragma once

#include <bitset>
#include <optional>
#include <string>
#include <vector>

#include "core/Sector.h"


namespace core
{

	class Location : public Sector
	{
		friend class World;
		bool mIsCorridor;
		std::bitset<256> mPermissionRequirement;
		// A one-cell-high Room's optional height override: a scale in
		// [CORE_ROOM_HEIGHT_SCALE_MIN, CORE_ROOM_HEIGHT_SCALE_MAX] applied to the
		// standard Room height. Only ever non-null for a Room (Location that is
		// neither a Corridor nor a Facade) with exactly one level high.
		std::optional<float> mHeightScale;

	public:

		Location(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY,  uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity, bool isCorridor = false);

		[[nodiscard]] bool isCorridor() const { return mIsCorridor; }
		[[nodiscard]] std::bitset<256> const& getPermissionRequirement() const { return mPermissionRequirement; }

		~Location() = default;

		// Overridden from Sector
		[[nodiscard]] std::string getDescription() const override;
	
		// Overridden from Sector
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;

		// Overridden from Sector. A Room's effective top height reflects its
		// height override when one is present.
		[[nodiscard]] float getEffectiveTopLevelHeight() const override;

		// Overridden from Sector. A Room is a Location that is neither a Corridor
		// nor a Facade.
		[[nodiscard]] bool isRoom() const override { return getType() == SectorType::Location && !isCorridor(); }

		[[nodiscard]] std::optional<float> getHeightScale() const { return mHeightScale; }

		// Whether a proposed override is valid: absent, or a finite value in the
		// authored [0.2, 1.0] range.
		static bool roomHeightScaleIsValid(std::optional<float> scale);

		// Applies or clears the override. Refuses non-Rooms and multi-level
		// Locations, and keeps the Sector's bounding height in step with the
		// effective top-level height. Returns false when refused.
		bool setHeightScale(std::optional<float> scale);
	};

} // core
