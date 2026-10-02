#pragma once

#include <array>
#include "core/Transit.h"

namespace core
{
	// Stationary same-Layer Transit. Device state lives here; admission and
	// occupants belong to its World-owned Traversal resource.
	class AirlockTransit : public Transit
	{
		friend class World;
		friend class SimulationCoordinator;
		TraversalResourceId mTraversalResource;
		uint64_t mCycleRemainingTicks{ 0 };
		int mActiveSide{ -1 };
		bool mClosing{ false };
		bool mExitRequested{ false };
		std::array<bool, 2> mOutsideRequests{};
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
		float getRemainingCycleSeconds() const;
		bool isCycleComplete() const { return mCycleRemainingTicks == 0; }
		bool isTraversalAvailable() const { return true; }
		TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		SectorEndType getPreviousEnd(int side) const { return mPreviousEnds.at(side); }
		std::shared_ptr<const BulkheadDoor> getDoor(int side) const { return mDoors.at(side); }
		InteractionPointId getControl(uint32_t index) const { return mControls.at(index); }
		std::string getDescription() const override { return "Airlock"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
	};
}
