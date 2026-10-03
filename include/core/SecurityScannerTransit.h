#pragma once

#include <array>
#include "core/Transit.h"

namespace core
{
	enum class SecurityScannerPhase
	{
		Idle, EntryOpening, Boarding, Positioning, EntryClosing,
		PreDelay, Scanning, PostPause, ExitOpening, Exiting, ExitClosing, OccupancyViolation
	};

	class SecurityScannerTransit : public Transit
	{
		friend class World;
		friend class SimulationCoordinator;
		TraversalResourceId mTraversalResource;
		SecurityScannerPhase mPhase{ SecurityScannerPhase::Idle };
		uint64_t mRemainingTicks{ 0 };
		AgentId mOccupant;
		float mScanProgress{ 0 };
		bool mLeftToRight;
		std::array<SectorEndType, 2> mPreviousEnds;
		std::array<std::shared_ptr<BulkheadDoor>, 2> mDoors;

	public:
		SecurityScannerTransit(uint32_t index, uint32_t layer, uint32_t x, uint32_t y,
			uint32_t width, bool leftToRight, std::vector<TransitStop> const& stops,
			std::array<SectorEndType, 2> previousEnds)
			: Transit(SectorType::SecurityScanner, "Security scanner", layer, index, x, y, 0, 0,
				(float)width, CORE_CORRIDOR_HEIGHT, width, 1, CORE_CORRIDOR_HEIGHT, 1, stops)
			, mLeftToRight(leftToRight), mPreviousEnds(previousEnds) {}

		bool isLeftToRight() const { return mLeftToRight; }
		int getEntrySide() const { return mLeftToRight ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT; }
		int getExitSide() const { return 1 - getEntrySide(); }
		float getPreDelaySeconds() const { return 1.0f; }
		float getScanSeconds() const { return 2.0f; }
		float getPostPauseSeconds() const { return 1.0f; }
		float getSensorDistance() const { return 0.5f; }
		bool isTraversalAvailable() const { return mPhase != SecurityScannerPhase::OccupancyViolation; }
		TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		SecurityScannerPhase getPhase() const { return mPhase; }
		std::string getPhaseName() const;
		float getRemainingSeconds() const;
		float getScanProgress() const { return mScanProgress; }
		AgentId getOccupant() const { return mOccupant; }
		SectorEndType getPreviousEnd(int side) const { return mPreviousEnds.at(side); }
		std::shared_ptr<const BulkheadDoor> getDoor(int side) const { return mDoors.at(side); }
		std::string getDescription() const override { return "Security scanner"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
	};
}
