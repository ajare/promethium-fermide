#pragma once

#include <array>
#include "core/Transit.h"

namespace core
{
	// Stationary same-Layer Transit. Device operation and traversal admission are
	// deliberately unavailable in the authored-chamber slice (#322).
	class AirlockTransit : public Transit
	{
		friend class World;
		float mCycleSeconds;
		std::array<SectorEndType, 2> mPreviousEnds;
		std::array<std::shared_ptr<BulkheadDoor>, 2> mDoors;
		std::array<InteractionPointId, 3> mControls;

	public:
		AirlockTransit(uint32_t index, uint32_t layer, uint32_t x, uint32_t y,
			uint32_t width, float cycleSeconds, std::vector<TransitStop> const& stops,
			std::array<SectorEndType, 2> previousEnds)
			: Transit(SectorType::Airlock, "Airlock", layer, index, x, y, 0, 0,
				(float)width, CORE_CORRIDOR_HEIGHT, width, 1, CORE_CORRIDOR_HEIGHT, width, stops)
			, mCycleSeconds(cycleSeconds), mPreviousEnds(previousEnds) {}

		float getCycleSeconds() const { return mCycleSeconds; }
		float getRemainingCycleSeconds() const { return 0.0f; }
		bool isCycleComplete() const { return true; }
		bool isTraversalAvailable() const { return false; }
		SectorEndType getPreviousEnd(int side) const { return mPreviousEnds.at(side); }
		std::shared_ptr<const BulkheadDoor> getDoor(int side) const { return mDoors.at(side); }
		InteractionPointId getControl(uint32_t index) const { return mControls.at(index); }
		std::string getDescription() const override { return "Airlock (traversal unavailable)"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
	};
}
