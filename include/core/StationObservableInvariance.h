#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace core
{
	enum class StationObservableInvarianceClass { Exact, Tolerance, Bound };

	struct StationObservableInvariance
	{
		std::string_view name;
		StationObservableInvarianceClass classification;
		double absoluteTolerance{0.0};
		double relativeTolerance{0.0};
		unsigned convergenceOrder{0};
		double bound{0.0};
		uint32_t certifiedRatio{1};
		std::string_view basis;
		std::string_view verifiedBy;
	};

	// Production-visible declaration of the timestep guarantee made for every
	// top-level field published by StationSystemsSnapshot. The test-side
	// comparator consumes this table; presentation uses certifiedRatio.
	std::span<StationObservableInvariance const> stationObservableInvariances();
}
