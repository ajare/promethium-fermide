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

		// The diagnostic opt-in is read here rather than through a shared
		// helper: core keeps no general-purpose environment API, so the one
		// platform call this timing needs stays private to it (#993).
		static bool phaseEnabled(char const* name)
		{
#if defined(_MSC_VER)
			size_t requiredSize = 0;
			return getenv_s(&requiredSize, nullptr, 0, name) == 0 && requiredSize != 0;
#else
			return std::getenv(name) != nullptr;
#endif
		}
	public:
		explicit RestorationTiming(char const* phase)
			: mPhase(phase), mEnabled(phaseEnabled("PF_RESTORATION_TIMING")),
			mStart(mEnabled ? Clock::now() : Clock::time_point{}) {}
		~RestorationTiming()
		{
			if (mEnabled)
				std::fprintf(stderr, "restoration phase=%s ms=%.3f\n", mPhase,
					std::chrono::duration<double, std::milli>(Clock::now() - mStart).count());
		}
	};
}
