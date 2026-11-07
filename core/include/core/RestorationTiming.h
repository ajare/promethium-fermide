#pragma once

#include <chrono>
#include <cstdio>
#include "Environment.h"

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
			: mPhase(phase), mEnabled(hasEnvironmentVariable("PF_RESTORATION_TIMING")),
			mStart(mEnabled ? Clock::now() : Clock::time_point{}) {}
		~RestorationTiming()
		{
			if (mEnabled)
				std::fprintf(stderr, "restoration phase=%s ms=%.3f\n", mPhase,
					std::chrono::duration<double, std::milli>(Clock::now() - mStart).count());
		}
	};
}
