#include "Smoke.h"
#include "MicrobialObservationFixture.h"
#include "core/StationSystemsRuntime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace
{
	constexpr float quantum = 1.0f / 60.0f;

	struct SourceScope
	{
		~SourceScope() { core::setStationSystemsSourceLoader({}); }
	};

	void microbialSupplyRoundoff(smoke::Context const&)
	{
		using smoke::require;
		// The CTest registrations drive this same public-runtime check at
		// ratios 1 and 6; no hand-written World tick loop or ratio override.
		auto settings = smoke::station::microbialObservationFixture();
		auto& processor = settings.microbialProcessors[0];
		processor.initialCondition = 1;
		processor.recoveryPerHour = 0;
		processor.starvationDegradePerHour = 360;
		processor.consumablePerKgFeed = 0.01;
		settings.stores[0].startingQualified = 0.003;
		{
			SourceScope restore;
			core::setStationSystemsSourceLoader([] {
				auto source = core::bundledStationSystemsSource();
				auto const at = source.find("        self.ticks = self.ticks + 1");
				require(at != std::string::npos, "Lua regression assertion point missing");
				source.insert(at, R"lua(
        if station_step * fixed_seconds >= 4 then
            local p = self.microbial_processors[1]
            assert(p.lastStressors == 'starvation', 'roundoff feed prevented Lua starvation')
            assert(p.condition < 0.95, 'Lua starvation did not degrade condition')
        end
)lua");
				return source;
			});
			core::StationSystemsRuntime runtime(settings, quantum);
			require(runtime.advanceTo(5), "Microbial exhaustion scenario failed");
			auto const snapshot = runtime.snapshot();
			auto const& live = snapshot.microbialProcessors[0];
			require(live.lastStressors == "starvation" && !live.recovering,
				"Microscopic feed residue prevented starvation");
			require(std::abs(live.condition - 0.8) < 1e-6,
				"Starvation did not degrade condition after feed depletion");
			require(std::abs(live.feedConsumedKg - 0.003) < 1e-12,
				"Exhaustion changed the amount of processed feed");
			for (auto const& balance : snapshot.constituentBalances)
				require(std::abs(balance.residual) <= snapshot.balanceTolerance
					* std::max(1.0, std::abs(balance.starting)),
					"Exhaustion broke constituent conservation");
		}

		// A stress decision must not delete the residual or spend replacement
		// stock to heal. Zero electrical supply isolates it from processing.
		// Exercise feed and medium, native kg/g units, protected reserves, and
		// positive controls well above roundoff but far below a full dose.
		for (bool medium : {false, true})
		for (bool grams : {false, true})
		for (bool reserve : {false, true})
		for (bool meaningful : {false, true})
		{
			auto s = smoke::station::microbialObservationFixture();
			s.solarArrays[0].capacityKW = 0;
			auto& p = s.microbialProcessors[0];
			p.initialCondition = 0.5;
			p.recoveryPerHour = 360;
			p.starvationDegradePerHour = 360;
			p.consumableExhaustionDegradePerHour = 360;
			p.consumablePerKgFeed = 0.01;
			auto& supply = s.stores[medium ? 3 : 0];
			supply.unit = grams ? "g" : "kg";
			auto const unitScale = grams ? 1000.0 : 1.0;
			// Fixed-per-feed consumables are authored in store units.
			if (medium) p.consumablePerKgFeed *= unitScale;
			supply.protectedReserve = reserve ? unitScale : 0;
			supply.startingQualified = meaningful
				? supply.protectedReserve + 1e-10 * unitScale
				: reserve ? std::nextafter(supply.protectedReserve,
					std::numeric_limits<double>::infinity()) : 1e-25 * unitScale;
			if (reserve) supply.capacity = 100 * unitScale;
			core::StationSystemsRuntime runtime(s, quantum);
			require(runtime.advanceTo(0.1), "Supply-roundoff scenario failed");
			auto const snapshot = runtime.snapshot();
			auto const& live = snapshot.microbialProcessors[0];
			auto const stressor = medium ? "consumable exhaustion" : "starvation";
			require((live.lastStressors.find(stressor) != std::string::npos) == !meaningful,
				"Supply exhaustion confused roundoff with meaningful stock");
			require(snapshot.stores[medium ? 3 : 0].currentQualified == supply.startingQualified,
				"Stress detection clamped or consumed residual stock");
			require(meaningful ? live.condition > 0.5 : live.condition < 0.5,
				"Supply exhaustion did not control condition/recovery");
			require(meaningful ? live.stockConsumedUnits > 0 : live.stockConsumedUnits == 0,
				"Exhausted supply allowed replacement-stock-funded healing");
		}
	}

	void historyCadencePrecision(smoke::Context const&)
	{
		using smoke::require;
		for (float cadence : {0.1f, 0.35f, 1.0f, 1.001f})
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = cadence;
			settings.trendRetentionSeconds = 10000;
			core::StationSystemsRuntime runtime(settings, quantum);
			require(runtime.advanceTo(2100), "History cadence regression failed to advance");
			auto const trends = runtime.trends();
			require(!trends.empty(), "History regression retained no trends");
			for (auto const& series : trends)
			{
				require(series.downsampleLevel > 0 && series.samples.size() > 1,
					"History regression did not exercise coarsening");
				for (size_t i = 1; i < series.samples.size(); ++i)
				{
					auto const gap = series.samples[i].elapsedSeconds - series.samples[i - 1].elapsedSeconds;
					require(std::abs(gap - series.sampleIntervalSeconds) <= 1e-12
						* std::max(1.0, series.samples[i].elapsedSeconds),
						"Published history stride differs from retained timestamps");
				}
			}
			auto const audit = core::auditStationStability(runtime.snapshot(), runtime.events(),
				trends, runtime.diagnostics());
			require(audit.clean, "Count-derived history failed the unchanged stability audit");
		}
	}
}

void registerStationRuntimeRegressionChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"microbialSupplyRoundoff", microbialSupplyRoundoff});
	checks.push_back({"historyCadencePrecision", historyCadencePrecision});
}
