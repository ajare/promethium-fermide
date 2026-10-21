#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace core
{
	enum class StationObservableInvarianceClass { Exact, Tolerance, Bound, StepDependent, Unverified };

	struct StationObservableInvariance
	{
		std::string_view name;
		StationObservableInvarianceClass classification;
		double absoluteTolerance{0.0};
		double relativeTolerance{0.0};
		unsigned convergenceOrder{0};
		double bound{0.0};
		// Global guarantee only; zero means none, not ratio 1.
		uint32_t certifiedRatio{0};
		// Largest sampled ratio, limited to the fixture/scope stated in basis.
		// This does not assert testing of every intermediate ratio.
		uint32_t verifiedThroughRatio{0};
		std::string_view basis;
		std::string_view verifiedBy;
	};

	// Coverage inventory, not a blanket timestep guarantee. Top-level mixed
	// aggregates remain Unverified until each physical observable has evidence.
	// Verified leaf entries state their fixture scope and actual comparator;
	// per-step doses/counters are not physical invariants. Presentation must
	// not interpret Unverified entries as measured tolerances.
	std::span<StationObservableInvariance const> stationObservableInvariances();
}
