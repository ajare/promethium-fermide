#include "core/Defines.h"
#include "core/PlatformLift.h"

namespace core
{
	PlatformLift::PlatformLift(uint32_t cellX, uint32_t cellY, uint32_t cellsWide,
		std::vector<uint32_t> const& stopOffsets)
		: Lift(cellX, cellY, 0.0f, 0.0f, (float)cellsWide, -0.05f,
			CORE_PLATFORM_LIFT_SPEED, stopOffsets)
	{
	}

	std::string PlatformLift::getDescription() const
	{
		return isBroken() ? "Platform Lift (Broken)" : "Platform Lift";
	}
	void PlatformLift::getCurrentShape(Vector2& minExtent, Vector2& maxExtent) const
	{
		getFullShape(minExtent, maxExtent);
	}
}
