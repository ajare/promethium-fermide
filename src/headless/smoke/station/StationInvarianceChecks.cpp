#include "StationCoreChecks.h"
#include "core/StationObservableInvariance.h"
#include "core/StationSystemsRuntime.h"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	constexpr float quantum = 1.0f / 60.0f;
	void require(bool value, char const* message) { if (!value) throw std::runtime_error(message); }

	core::StationSystemsSnapshot run(uint32_t ratio)
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.systemsStepWorldTicks = ratio;
		settings.stores = {{"source", "kg", 10, 2, 0}, {"sink", "kg", 1, 0, 0}};
		core::ResourceTransferSettings transfer;
		transfer.id = "transfer"; transfer.sourceId = "source"; transfer.destinationId = "sink";
		transfer.unit = "kg"; transfer.ratePerSecond = 1;
		settings.transfers = {transfer};
		settings.deliveries = {{"delivery", "source", "kg", 1, 0.025}};
		core::StationSystemsRuntime runtime(settings, quantum);
		require(runtime.advanceTo(1.0), "Cross-step scenario did not advance");
		return runtime.snapshot();
	}

	void invariance(smoke::Context const&)
	{
		auto const entries = core::stationObservableInvariances();
		require(!entries.empty(), "Station observable invariance registry is empty");
		std::set<std::string_view> names;
		for (auto const& entry : entries)
		{
			require(!entry.name.empty() && names.insert(entry.name).second,
				"Station observable is unclassified or duplicated");
			require(!entry.basis.empty() && !entry.verifiedBy.empty(),
				"Station observable classification lacks a written basis or verifier");
			if (entry.classification == core::StationObservableInvarianceClass::Tolerance)
				require(entry.convergenceOrder >= 1, "Tolerance observable lacks convergence order");
			if (entry.classification == core::StationObservableInvarianceClass::Bound)
				require(entry.certifiedRatio >= 1, "Bound observable lacks certified ratio");
		}

		auto const reference = run(1);
		for (uint32_t ratio : {6u, 30u})
		{
			auto const candidate = run(ratio);
			require(std::abs(candidate.elapsedSeconds - reference.elapsedSeconds) < 1e-12,
				"Station elapsed time changed with step ratio");
			require(std::abs(candidate.stores[0].currentQualified - reference.stores[0].currentQualified) < 1e-10
				&& std::abs(candidate.stores[1].currentQualified - reference.stores[1].currentQualified) < 1e-10,
				"Store observable exceeded its exact cross-step classification");
			require(std::abs(candidate.transfers[0].cumulativeMoved - reference.transfers[0].cumulativeMoved) < 1e-10
				&& std::abs(candidate.deliveries[0].acceptedTotal - reference.deliveries[0].acceptedTotal) < 1e-10,
				"Scheduled or transfer observable exceeded its exact cross-step classification");
		}
	}
}

void registerStationInvarianceChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"invariance", invariance});
}
