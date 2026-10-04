#pragma once

#include "core/Transit.h"
#include "core/Window.h"
#include <array>

namespace core
{
	// An authored object-service device, deliberately not a passenger Transport.
	class Dumbwaiter final : public Transit
	{
		friend class World;
		DumbwaiterId mId;
		uint32_t mInitialStop;
		float mTravelSeconds;
		std::array<std::shared_ptr<const BoothWindow>, 2> mApertures;
	public:
		Dumbwaiter(DumbwaiterId id, uint32_t index, uint32_t layer, uint32_t x, uint32_t y,
			uint32_t initialStop, float seconds, std::vector<TransitStop> const& stops)
			: Transit(SectorType::Dumbwaiter, "Dumbwaiter", layer, index, x, y, 0, 0,
				1, 2, 1, 2, 1.0f, 0, stops),
			  mId(id), mInitialStop(initialStop), mTravelSeconds(seconds) {}
		std::string getDescription() const override { return "Dumbwaiter"; }
		bool sectorSupportsObjectType(SectorObjectType) const override { return false; }
		DumbwaiterId getId() const { return mId; }
		uint32_t getInitialStop() const { return mInitialStop; }
		float getTravelSeconds() const { return mTravelSeconds; }
		Vector2 getCarPosition() const { return {float(getCellX()), float(getCellY() + mInitialStop)}; }
		std::shared_ptr<const BoothWindow> getAperture(uint32_t stop) const { return mApertures.at(stop); }
	};
}
