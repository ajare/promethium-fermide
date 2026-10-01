#pragma once

#include <filesystem>

namespace restoration_support
{
	// Optional benchmark reporting; smoke execution emits only harness records.
	using Report = void (*)(unsigned cycle, double reloadMs, double resetMs);
	void verify(std::filesystem::path const& input, unsigned cycles, Report report = nullptr);
}
