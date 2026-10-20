#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace core
{
	// Count-derived time: no duration is ever accumulated. The embedding clock
	// supplies its base quantum; Station code need not depend on World.
	class StationClock
	{
	public:
		StationClock(uint32_t ratio, float baseSeconds)
			: mDuration(static_cast<double>(baseSeconds) * ratio)
		{
			if (!std::isfinite(mDuration) || mDuration <= 0
				|| mDuration > std::numeric_limits<float>::max())
				throw std::invalid_argument("Station clock requires a finite positive step duration");
		}
		double stepSeconds() const { return mDuration; }
		uint64_t steps() const { return mSteps; }
		double elapsedSeconds() const { return secondsAt(mSteps); }
		double secondsAt(uint64_t step) const { return static_cast<double>(step) * mDuration; }
		void commitStep() { ++mSteps; }
	private:
		double mDuration;
		uint64_t mSteps{0};
	};
}
