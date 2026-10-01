#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>

namespace routing_support
{
	// Shared deterministic workload/assertions for smoke and the explicit World
	// export tool. Smoke never exports files, reports timings, or samples memory.
	uint64_t populationRoutingRun(std::filesystem::path const& output = {}, bool verifyReset = true,
		std::ostream* report = nullptr, size_t (*workingSetBytes)() = nullptr);
}
