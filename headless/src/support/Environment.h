#pragma once

#include <cstdlib>

namespace headless_support
{
	// Verification-only platform helper: checks gate optional trace output on an
	// environment variable. It lives in the headless tier because the World
	// domain aggregate keeps no general-purpose environment API (#993).
	inline bool hasEnvironmentVariable(char const* name)
	{
#if defined(_MSC_VER)
		size_t requiredSize = 0;
		return getenv_s(&requiredSize, nullptr, 0, name) == 0 && requiredSize != 0;
#else
		return std::getenv(name) != nullptr;
#endif
	}
}
