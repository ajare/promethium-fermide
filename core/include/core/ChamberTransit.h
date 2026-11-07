#pragma once

#include <array>
#include <cmath>
#include "core/Transit.h"
#include "core/ChamberSubtype.h"
#include <stdexcept>

namespace core
{
	enum class SecurityScannerPhase
	{
		Idle, EntryOpening, Boarding, Positioning, EntryClosing,
		PreDelay, Scanning, PostPause, ExitOpening, Exiting, ExitClosing, OccupancyViolation
	};

	class ChamberTransit : public Transit
	{
		friend class World;
		friend class SimulationCoordinator;
		TraversalResourceId mTraversalResource;
		SecurityScannerPhase mPhase{ SecurityScannerPhase::Idle };
		uint64_t mRemainingTicks{ 0 };
		uint64_t mBoardingWindowRemainingTicks{ 0 };
		AgentId mOccupant;
		float mScanProgress{ 0 };
		float mSensorDistance{ 0.5f }, mPreDelaySeconds{ 1 }, mScanSeconds{ 2 }, mPostPauseSeconds{ 1 };
		uint64_t mActivePreTicks{ 0 }, mActiveScanTicks{ 0 }, mActivePostTicks{ 0 };
		bool mLeftToRight;
		std::array<SectorEndType, 2> mPreviousEnds;
		std::array<std::shared_ptr<BulkheadDoor>, 2> mDoors;

		ChamberSubtype const mSubtype;

	public:
		ChamberTransit(uint32_t index, uint32_t layer, uint32_t x, uint32_t y,
			uint32_t width, bool leftToRight, std::vector<TransitStop> const& stops,
			std::array<SectorEndType, 2> previousEnds,
			ChamberSubtype subtype = ChamberSubtype::SecurityScanner)
			: Transit(SectorType::Chamber, "Chamber", layer, index, x, y, 0, 0,
				(float)width, CORE_CORRIDOR_HEIGHT, width, 1, CORE_CORRIDOR_HEIGHT, subtype == ChamberSubtype::Decontamination ? width : 1, stops)
			, mLeftToRight(leftToRight), mPreviousEnds(previousEnds), mSubtype(subtype)
		{
			if (!isSupportedChamberSubtype(subtype)) throw std::invalid_argument("Unsupported Chamber subtype");
		}

		ChamberSubtype getSubtype() const { return mSubtype; }
		uint32_t getJourneyCapacity() const { return getCapacity(); }
		float getDecontaminationOpacity() const
		{
			if (mSubtype != ChamberSubtype::Decontamination || mPhase != SecurityScannerPhase::Scanning) return 0;
			return mScanProgress <= 0.25f ? mScanProgress * 4.0f : (1.0f - mScanProgress) / 0.75f;
		}
		bool isLeftToRight() const { return mLeftToRight; }
		int getEntrySide() const { return mLeftToRight ? CORE_SIDE_LEFT : CORE_SIDE_RIGHT; }
		int getExitSide() const { return 1 - getEntrySide(); }
		static bool validConfiguration(float sensor, float pre, float scan, float post)
		{
			return std::isfinite(sensor) && sensor >= 0 && std::isfinite(pre) && pre >= 0 && pre <= 10
				&& std::isfinite(scan) && scan >= 0.1f && scan <= 10
				&& std::isfinite(post) && post >= 0 && post <= 10;
		}
		float getPreDelaySeconds() const { return mPreDelaySeconds; }
		float getScanSeconds() const { return mScanSeconds; }
		float getPostPauseSeconds() const { return mPostPauseSeconds; }
		float getSensorDistance() const { return mSensorDistance; }
		bool isTraversalAvailable() const { return mPhase != SecurityScannerPhase::OccupancyViolation; }
		TraversalResourceId getTraversalResourceId() const { return mTraversalResource; }
		SecurityScannerPhase getPhase() const { return mPhase; }
		std::string getPhaseName() const;
		float getRemainingSeconds() const;
		float getScanProgress() const { return mScanProgress; }
		AgentId getOccupant() const { return mOccupant; }
		SectorEndType getPreviousEnd(int side) const { return mPreviousEnds.at(side); }
		std::shared_ptr<const BulkheadDoor> getDoor(int side) const { return mDoors.at(side); }
		std::string getDescription() const override { return mSubtype == ChamberSubtype::Decontamination ? "Chamber (Decontamination)" : "Chamber (Security scanner)"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
	};
}
