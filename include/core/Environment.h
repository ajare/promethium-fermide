#pragma once

#include <cstdlib>

namespace core
{
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
