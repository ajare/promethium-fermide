#pragma once

#include "core/Transit.h"
#include "core/Window.h"
#include <array>

namespace core
{
	enum class DumbwaiterPhase { Idle, Closing, Travelling, Opening };
	enum class DumbwaiterButtonState { Here, Elsewhere, Busy };

	// An authored object-service device, deliberately not a passenger Transport.
	class Dumbwaiter final : public Transit
	{
		friend class World;
		friend class SimulationCoordinator;
		DumbwaiterId mId;
		DumbwaiterPhase mPhase{DumbwaiterPhase::Idle};
		uint32_t mCurrentStop, mDestination{0};
		uint64_t mTravelTicks{0};
		float mCarOffset;
		DeviceOperationId mOperation;
		uint32_t mInitialStop;
		float mTravelSeconds;
		std::array<std::shared_ptr<const BoothWindow>, 2> mApertures;
		std::array<InteractionPointId, 2> mLandingButtons;
	public:
		Dumbwaiter(DumbwaiterId id, uint32_t index, uint32_t layer, uint32_t x, uint32_t y,
			uint32_t initialStop, float seconds, std::vector<TransitStop> const& stops)
			: Transit(SectorType::Dumbwaiter, "Dumbwaiter", layer, index, x, y, 0, 0,
				1, 2, 1, 2, 1.0f, 0, stops),
			  mId(id), mCurrentStop(initialStop), mCarOffset(float(initialStop)),
			  mInitialStop(initialStop), mTravelSeconds(seconds) {}
		std::string getDescription() const override { return "Dumbwaiter"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
		DumbwaiterId getId() const { return mId; }
		uint32_t getInitialStop() const { return mInitialStop; }
		float getTravelSeconds() const { return mTravelSeconds; }
		Vector2 getCarPosition() const { return {float(getCellX()), float(getCellY()) + mCarOffset}; }
		DumbwaiterPhase getPhase() const { return mPhase; }
		char const* getPhaseName() const
		{
			switch (mPhase) {
			case DumbwaiterPhase::Closing: return "Departure closing";
			case DumbwaiterPhase::Travelling: return "Travelling";
			case DumbwaiterPhase::Opening: return "Arrival opening";
			default: return "Idle";
			}
		}
		bool isBusy() const { return mPhase != DumbwaiterPhase::Idle; }
		DumbwaiterButtonState getButtonState(uint32_t stop) const
		{
			return isBusy() ? DumbwaiterButtonState::Busy
				: stop == mCurrentStop ? DumbwaiterButtonState::Here : DumbwaiterButtonState::Elsewhere;
		}
		DeviceOperationId getOperation() const { return mOperation; }
		InteractionPointId getLandingButton(uint32_t stop) const { return mLandingButtons.at(stop); }
		std::shared_ptr<const BoothWindow> getAperture(uint32_t stop) const { return mApertures.at(stop); }
	};
}
