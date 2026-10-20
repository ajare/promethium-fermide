#include "StationCoreChecks.h"
#include "core/StationSystemsRuntime.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	void require(bool ok, char const* message) { if (!ok) throw std::runtime_error(message); }
	// Deliberately no World, Agents, or tick pipeline in this scenario.
	void clock(smoke::Context const&)
	{
		constexpr float quantum = 1.0f / 60.0f;
		for (uint32_t ratio : {1u, 6u, 30u, 600u})
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.systemsStepWorldTicks = ratio;
			settings.stores = {{"source", "kg", 100, 50, 0}, {"sink", "kg", 100, 0, 0}};
			core::ResourceTransferSettings transfer;
			transfer.id = "pump"; transfer.sourceId = "source";
			transfer.destinationId = "sink"; transfer.unit = "kg";
			transfer.ratePerSecond = 0.01;
			settings.transfers = {transfer};
			core::StationSystemsRuntime direct(settings, quantum), partitioned(settings, quantum);
			auto const duration = 120.0 * static_cast<double>(quantum) * 60;
			require(direct.advanceTo(duration), "Standalone Station drive failed");
			for (double target : {duration / 4, duration / 2, duration})
				require(partitioned.advanceTo(target), "Partitioned Station drive failed");
			auto const result = direct.snapshot();
			require(result == partitioned.snapshot(), "Call partition changed committed Station results");
			require(result.elapsedSeconds == duration, "Count-derived Station clock drifted");
			require(std::abs(result.stores[1].currentQualified - 0.01 * duration) < 1e-10,
				"Standalone transfer did not integrate simulated seconds");
			for (double target : {duration, 0.0, duration / 2})
			{
				require(direct.advanceTo(target), "Reached target was refused");
				require(direct.snapshot() == result, "Reached target mutated Station state");
			}
			for (double target : {-1.0, std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max()})
			{
				require(!direct.advanceTo(target), "Invalid target accepted");
				require(direct.snapshot() == result, "Invalid target mutated Station state");
			}
			// A non-boundary target runs a whole step, never a fractional one.
			auto const stepSeconds = static_cast<double>(quantum) * ratio;
			require(direct.advanceTo(duration + stepSeconds / 2), "Whole-step catch-up failed");
			require(direct.snapshot().elapsedSeconds == duration + stepSeconds,
				"Catch-up performed a partial step");
			auto const overshot = direct.snapshot();
			require(direct.advanceTo(duration + stepSeconds * 0.75) && direct.snapshot() == overshot,
				"Drive stepped again while already ahead of target");
		}
		for (uint32_t ratio : {0u, 601u, std::numeric_limits<uint32_t>::max()})
		{
			core::StationSystemsSettings settings;
			settings.systemsStepWorldTicks = ratio;
			std::string diagnostic;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find(std::to_string(ratio)) != std::string::npos
				&& diagnostic.find("[1, 600]") != std::string::npos, "Step ratio diagnostic missing range/value");
		}
	}
}

void registerStationClockChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"clock", clock});
}
