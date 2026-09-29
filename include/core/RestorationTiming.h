#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace core
{
	// Opt-in, inclusive wall-clock phase timings. Nested phases must not be
	// summed. No retained samples or World/resource references.
	class RestorationTiming
	{
		using Clock = std::chrono::steady_clock;
		char const* mPhase;
		bool mEnabled;
		Clock::time_point mStart;
	public:
		explicit RestorationTiming(char const* phase)
			: mPhase(phase), mEnabled(std::getenv("PF_RESTORATION_TIMING") != nullptr),
			mStart(mEnabled ? Clock::now() : Clock::time_point{}) {}
		~RestorationTiming()
		{
			if (mEnabled)
				std::fprintf(stderr, "restoration phase=%s ms=%.3f\n", mPhase,
					std::chrono::duration<double, std::milli>(Clock::now() - mStart).count());
		}
	};
}
