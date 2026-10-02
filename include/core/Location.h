#pragma once

#include <bitset>
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

	public:

		Location(std::string const& name, SectorType type, uint32_t layerIndex, uint32_t index, uint32_t cellX, uint32_t cellY,  uint32_t cellsWide, uint32_t levelsHigh, float topLevelHeight, uint32_t capacity, bool isCorridor = false);

		[[nodiscard]] bool isCorridor() const { return mIsCorridor; }
		[[nodiscard]] std::bitset<256> const& getPermissionRequirement() const { return mPermissionRequirement; }

		~Location() = default;

		// Overridden from Sector
		[[nodiscard]] std::string getDescription() const override;
	
		// Overridden from Sector
		[[nodiscard]] bool sectorSupportsObjectType(SectorObjectType type) const override;
	};

} // core
