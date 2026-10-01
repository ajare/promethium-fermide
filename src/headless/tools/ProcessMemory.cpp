#include "ToolSupport.h"
#if defined(_WIN32)
#include <Psapi.h>
#else
#include <unistd.h>
#include <fstream>
#endif

namespace
{
	size_t currentWorkingSetBytes()
	{
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		return GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))
			? counters.WorkingSetSize : 0;
#elif defined(__linux__)
		long totalPages = 0;
		long residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (!(statm >> totalPages >> residentPages)) return 0;
		auto const pageSize = sysconf(_SC_PAGESIZE);
		return pageSize > 0 ? static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize) : 0;
#else
#error "Unsupported platform"
#endif
	}
}

size_t tool::workingSetBytes()
{
	return currentWorkingSetBytes();
}

size_t tool::peakWorkingSetBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
		? counters.PeakWorkingSetSize : 0;
#else
	return 0; // Unavailable; not a zero-memory claim.
#endif
}
