// #424: long-duration stability and repeatability of the Station systems.
//
// These checks drive the packaged long-duration stability workload -
// the reconciled Meridian register run for three simulated hours with
// every scheduled mechanism recurring many times over - through the
// public World tick/snapshot/event/diagnostic seam and the public
// stability audit. They verify: finite values and inventory bounds at
// every checkpoint, conservation ledgers closed inside the declared
// residual tolerance, exact scheduled occurrence counts across dozens
// of recurring firings, bounded event/trend history, and equivalent
// outcomes across every supported World update batching/time-scale
// path (headless advanceTicks, update() at 1x, 4x and 0.25x), across
// a mid-run reset and replay, and across multiple Worlds interleaved
// tick-for-tick. Nothing here clamps or relaxes a tolerance: a drift is
// a failing regression, reported with its magnitude.
#include "Stability.h"

#include "core/StationReference.h"
#include "core/StationSystems.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }

	auto const scenarioTicks = [](double seconds)
	{
		return static_cast<uint64_t>(std::llround(seconds / core::World::getFixedTimestep()));
	};

	auto const configureOrDiagnose = [](core::World& world,
		core::StationSystemsSettings const& settings, char const* what)
	{
		std::string diagnostic;
		if (!world.configureStationSystems(settings, false, &diagnostic))
		{
			std::fprintf(stderr, "%s configure failed: %s\n", what, diagnostic.c_str());
			throw std::runtime_error(std::string(what) + " configuration failed");
		}
	};

	auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s run failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};

	// Drive the World through update() at a chosen time scale so every
	// call executes exactly 60 fixed ticks: one simulated second of
	// World time per call at 1x, and the scale-adjusted wall input for
	// the others. The tick delta is asserted per call so a batching
	// defect names itself at the call that broke it rather than showing
	// up as a drifted final state.
	auto const runUpdatePath = [](core::World& world, double timeScale,
		uint64_t totalTicks, float elapsedPerCall, char const* what)
	{
		world.setSimulationTimeScale(timeScale);
		uint64_t calls = 0;
		while (world.getSimulationTick() < totalTicks)
		{
			auto const before = world.getSimulationTick();
			world.update(elapsedPerCall);
			auto const after = world.getSimulationTick();
			++calls;
			if (after - before != 60)
			{
				std::fprintf(stderr,
					"%s: update call %llu at scale %g advanced %llu ticks, expected exactly 60\n",
					what, static_cast<unsigned long long>(calls), timeScale,
					static_cast<unsigned long long>(after - before));
				throw std::runtime_error(std::string(what) + " update batching is not 60 ticks/call");
			}
		}
		if (world.getSimulationTick() != totalTicks)
			throw std::runtime_error(std::string(what) + " overshot the target tick count");
	};

	auto const auditOf = [](core::World& world)
	{
		return core::auditStationStability(world.getStationSystemsSnapshot(),
			world.getStationSystemsEvents(), world.getStationSystemsTrends(),
			world.getStationSystemsDiagnostics());
	};

	auto const reportAudit = [](core::StationStabilityAudit const& audit, char const* what)
	{
		auto const report = [&](std::vector<std::string> const& findings, char const* kind)
		{
			for (size_t index = 0; index < std::min<size_t>(findings.size(), 8); ++index)
				std::fprintf(stderr, "%s: %s\n", what, findings[index].c_str());
		};
		report(audit.nonFiniteFindings, "non-finite");
		report(audit.boundViolations, "bound violation");
		report(audit.conservationViolations, "conservation violation");
	};

	// Expected completed occurrences of a recurring schedule at time t:
	// one-off fires once at first; recurring fires at first + k*recurrence.
	auto const expectedOccurrences = [](double t, double first, double recurrence)
	{
		if (t < first) return 0ULL;
		if (recurrence <= 0.0) return 1ULL;
		return static_cast<uint64_t>(std::floor((t - first) / recurrence + 1e-9)) + 1ULL;
	};

	auto findDelivery = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.deliveries)
			if (entry.id == id) return entry;
		return core::StationDeliverySnapshot{};
	};
	auto findEjection = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.wasteEjections)
			if (entry.id == id) return entry;
		return core::StationWasteEjectionSnapshot{};
	};
	auto findOutage = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.outages)
			if (entry.id == id) return entry;
		return core::StationOutageSnapshot{};
	};
	auto findCycle = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.airlockCycles)
			if (entry.id == id) return entry;
		return core::StationAirlockCycleSnapshot{};
	};
	auto findMaintenance = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.maintenanceJobs)
			if (entry.id == id) return entry;
		return core::StationMaintenanceSnapshot{};
	};

	// Verify every recurring schedule in the workload has completed
	// exactly the occurrence count its authored schedule implies at the
	// given simulated time. Schedule phase error - a missed or doubled
	// firing, a drifting due-time - shows up here, not as a clamped rate.
	auto const requireScheduleCounts = [](core::World& world, double t,
		core::StationSystemsSettings const& rig, char const* what)
	{
		auto const snap = world.getStationSystemsSnapshot();
		for (auto const& delivery : rig.deliveries)
		{
			auto const expected = expectedOccurrences(t, delivery.firstOccurrenceSeconds,
				delivery.recurrenceSeconds);
			auto const actual = findDelivery(snap, delivery.id).occurrencesCompleted;
			if (actual != expected)
			{
				std::fprintf(stderr, "%s: delivery '%s' completed %llu occurrences, schedule says %llu by t=%.1f s\n",
					what, delivery.id.c_str(), static_cast<unsigned long long>(actual),
					static_cast<unsigned long long>(expected), t);
				throw std::runtime_error(std::string(what) + " delivery schedule count mismatch");
			}
		}
		for (auto const& ejection : rig.wasteEjections)
		{
			auto const expected = expectedOccurrences(t, ejection.firstOccurrenceSeconds,
				ejection.recurrenceSeconds);
			auto const actual = findEjection(snap, ejection.id).occurrencesCompleted;
			if (actual != expected)
			{
				std::fprintf(stderr, "%s: waste ejection '%s' completed %llu occurrences, schedule says %llu by t=%.1f s\n",
					what, ejection.id.c_str(), static_cast<unsigned long long>(actual),
					static_cast<unsigned long long>(expected), t);
				throw std::runtime_error(std::string(what) + " ejection schedule count mismatch");
			}
		}
		for (auto const& cycle : rig.airlockCycles)
		{
			auto const expected = expectedOccurrences(t, cycle.firstOccurrenceSeconds,
				cycle.recurrenceSeconds);
			auto const actual = findCycle(snap, cycle.id).occurrencesCompleted;
			if (actual != expected)
			{
				std::fprintf(stderr, "%s: airlock cycle '%s' completed %llu occurrences, schedule says %llu by t=%.1f s\n",
					what, cycle.id.c_str(), static_cast<unsigned long long>(actual),
					static_cast<unsigned long long>(expected), t);
				throw std::runtime_error(std::string(what) + " airlock schedule count mismatch");
			}
		}
		// Outage and maintenance windows complete only after their full
		// duration has passed; count windows whose end has arrived.
		for (auto const& outage : rig.outages)
		{
			auto const expected = expectedOccurrences(t - outage.durationSeconds,
				outage.firstOccurrenceSeconds, outage.recurrenceSeconds);
			auto const actual = findOutage(snap, outage.id).occurrencesCompleted;
			if (actual != expected)
			{
				std::fprintf(stderr, "%s: outage '%s' completed %llu windows, schedule says %llu closed by t=%.1f s\n",
					what, outage.id.c_str(), static_cast<unsigned long long>(actual),
					static_cast<unsigned long long>(expected), t);
				throw std::runtime_error(std::string(what) + " outage schedule count mismatch");
			}
		}
		for (auto const& job : rig.maintenanceJobs)
		{
			auto const expected = expectedOccurrences(t - job.durationSeconds,
				job.firstOccurrenceSeconds, job.recurrenceSeconds);
			auto const actual = findMaintenance(snap, job.id).occurrencesCompleted;
			if (actual != expected)
			{
				std::fprintf(stderr, "%s: maintenance job '%s' completed %llu occurrences, schedule says %llu closed by t=%.1f s\n",
					what, job.id.c_str(), static_cast<unsigned long long>(actual),
					static_cast<unsigned long long>(expected), t);
				throw std::runtime_error(std::string(what) + " maintenance schedule count mismatch");
			}
		}
	};

	// The workload's declared residual tolerance is the published
	// Station balance tolerance; the audit reports the worst ledger as
	// a multiple of it. Keep the worst offender visible in failure text.
	void stabilityWorkloadBundled(smoke::Context const&)
	{
		auto const& workload = core::bundledStationLongRunWorkload();
		require(workload.scenarioKind == "stability",
			"the bundled long-run workload is not authored as a stability scenario");
		require(workload.scenarioSeconds == 7200.0,
			"the bundled long-run workload is not a two-simulated-hour scenario");
		require(workload.settings.enabled, "the bundled long-run workload is not authored enabled");
		std::string diagnostic;
		require(core::validateStationSystemsSettings(workload.settings, diagnostic),
			("the bundled long-run workload was refused: " + diagnostic).c_str());
		// Every schedule family is present and recurring, so the long run
		// exercises repeated occurrences rather than single firings.
		require(!workload.settings.deliveries.empty(), "the workload has no deliveries");
		require(std::any_of(workload.settings.deliveries.begin(), workload.settings.deliveries.end(),
			[](core::ResourceDeliverySettings const& d) { return d.recurrenceSeconds > 0.0; }),
			"no delivery in the workload recurs");
		require(workload.settings.wasteEjections.size() >= 3
			&& std::all_of(workload.settings.wasteEjections.begin(),
				workload.settings.wasteEjections.end(),
				[](core::WasteEjectionSettings const& e) { return e.recurrenceSeconds > 0.0; }),
			"the workload's waste ejections do not all recur");
		require(workload.settings.outages.size() == 1
			&& workload.settings.outages[0].recurrenceSeconds > 0.0,
			"the workload's installation outage does not recur");
		require(workload.settings.airlockCycles.size() == 1
			&& workload.settings.airlockCycles[0].recurrenceSeconds > 0.0,
			"the workload's airlock cycle does not recur");
		require(workload.settings.maintenanceJobs.size() == 1
			&& workload.settings.maintenanceJobs[0].recurrenceSeconds > 0.0,
			"the workload's maintenance job does not recur");
		// The workload declares its residual tolerance: the published
		// balance tolerance is the contract the audit enforces.
		require(core::StationBalanceTolerance > 0.0
			&& core::StationBalanceTolerance <= 1e-9,
			"the declared residual tolerance is not the published 1e-9 contract");
		// The resource text round-trips through the authoritative reader.
		core::StationReferenceDocument reparsed;
		require(core::parseStationReference(core::bundledStationLongRunWorkloadText(),
			reparsed, &diagnostic),
			("re-parsing the packaged workload text failed: " + diagnostic).c_str());
		require(reparsed.settings == workload.settings,
			"re-parsing the packaged workload text produced different settings");
		require(reparsed.scenarioKind == workload.scenarioKind
			&& reparsed.scenarioSeconds == workload.scenarioSeconds,
			"re-parsing the packaged workload text lost the scenario declaration");
	}

	// The flagship long run: two simulated hours on the headless
	// deterministic seam, audited every half simulated hour.
	void stabilityLongRun(smoke::Context const&)
	{
		auto const& workload = core::bundledStationLongRunWorkload();
		auto const& rig = workload.settings;
		core::World world("StabilityLongRun", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		configureOrDiagnose(world, rig, "stability long run");
		world.resumeSimulation();
		auto const total = scenarioTicks(workload.scenarioSeconds);
		auto const checkpoint = scenarioTicks(1800.0);
		size_t checkpointIndex = 0;
		std::vector<size_t> storeSizes;
		for (uint64_t done = 0; done < total; done += checkpoint)
		{
			auto const leg = std::min(checkpoint, total - done);
			runOrDiagnose(world, leg, "stability long run leg");
			++checkpointIndex;
			auto const t = static_cast<double>(done + leg) * core::World::getFixedTimestep();
			auto const audit = auditOf(world);
			if (!audit.clean)
			{
				reportAudit(audit, "stability audit dirty at checkpoint");
				throw std::runtime_error("the long-run stability audit reported violations");
			}
			require(audit.enabled && audit.finiteChecks > 1000,
				"the audit did not cover the full published state at the checkpoint");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the long run was not healthy at a checkpoint");
			for (auto const ticks : snap.completedTicks)
				require(ticks == done + leg, "a station system fell behind in the long run");
			// Bounded retained history at every checkpoint: the event
			// window and every trend series stay inside their published
			// bounds no matter how long the run goes on.
			require(audit.eventsRetained <= audit.eventHistoryLimit,
				"the event history grew past its bound mid-run");
			require(audit.trendSamplesRetained
				<= audit.trendSeries * audit.trendSampleLimit,
				"a trend series grew past its sample bound mid-run");
			// The snapshot's vector sizes are a function of the authored
			// register, never of elapsed time: no per-tick accumulation
			// in the published state.
			std::vector<size_t> sizes{snap.stores.size(), snap.transfers.size(),
				snap.balances.size(), snap.constituentBalances.size(),
				snap.deliveries.size(), snap.wasteEjections.size(),
				snap.outages.size(), snap.maintenanceJobs.size(),
				snap.crews.size(), snap.atmospheres.size(),
				snap.endurance.size()};
			if (storeSizes.empty()) storeSizes = sizes;
			else require(sizes == storeSizes,
				"the published snapshot grew with elapsed time");
		}
		require(checkpointIndex == 4, "the long run did not complete its four checkpoints");
		// Scheduling correctness over the whole horizon: every recurring
		// schedule fired exactly its authored occurrence count.
		requireScheduleCounts(world, workload.scenarioSeconds, rig, "stability long run");
		// The final audit is clean and the run produced no software
		// failure diagnostics: no Lua memory or instruction budget was
		// exhausted across the whole two-hour run.
		auto const audit = auditOf(world);
		require(audit.softwareFailures == 0,
			"the long run produced software failure diagnostics");
		require(audit.maxResidualRatio <= 1.0,
			"a conservation ledger left the declared tolerance in the long run");
		require(world.getStationSystemsDiagnostics().empty(),
			"the long run left diagnostics behind");
	}

	// Equivalent outcomes across the supported World update batching /
	// time-scale paths: the same workload driven headless tick-by-tick,
	// through update() at 1x, at 4x accelerated and at 0.25x slowed
	// wall time must land on the identical station state and event
	// history. The time scale changes how fast ticks arrive, never what
	// a tick computes - no separate station clock.
	void stabilityPathEquivalence(smoke::Context const&)
	{
		auto const& workload = core::bundledStationLongRunWorkload();
		auto const& rig = workload.settings;
		// Half an simulated hour per path keeps the check inside its
		// budget; the equivalence property is about the paths, not the
		// duration.
		auto const total = scenarioTicks(1800.0);
		core::World reference("StabilityPathRef", 2, 2);
		reference.pauseSimulation(); reference.finishBuild();
		configureOrDiagnose(reference, rig, "stability path reference");
		reference.resumeSimulation();
		runOrDiagnose(reference, total, "stability path reference run");
		auto const expected = reference.getStationSystemsSnapshot();
		auto const expectedEvents = reference.getStationSystemsEvents();
		require(expected.healthy, "the reference path was not healthy");

		struct Path { char const* name; double scale; float elapsed; };
		for (auto const& path : {
			Path{"update 1x", 1.0, 1.0f},
			Path{"update 4x", 4.0, 0.25f},
			Path{"update 0.25x", 0.25, 4.0f} })
		{
			core::World world(path.name, 2, 2);
			world.pauseSimulation(); world.finishBuild();
			configureOrDiagnose(world, rig, path.name);
			world.resumeSimulation();
			runUpdatePath(world, path.scale, total, path.elapsed, path.name);
			require(world.getStationSystemsSnapshot() == expected,
				("the " + std::string(path.name)
					+ " path produced a different station snapshot").c_str());
			require(world.getStationSystemsEvents() == expectedEvents,
				("the " + std::string(path.name)
					+ " path produced a different event history").c_str());
			auto const audit = auditOf(world);
			require(audit.clean, ("the " + std::string(path.name)
				+ " path failed the stability audit").c_str());
		}
	}

	// Reset and replay: a run interrupted by a World reset and replayed
	// from the authored start reproduces the uninterrupted outcome
	// exactly - the audit state, the ledgers and the event history all
	// restart clean and re-derive identically.
	void stabilityResetIsolation(smoke::Context const&)
	{
		auto const& workload = core::bundledStationLongRunWorkload();
		auto const& rig = workload.settings;
		auto const total = scenarioTicks(1800.0);
		core::World uninterrupted("StabilityResetRef", 2, 2);
		uninterrupted.pauseSimulation(); uninterrupted.finishBuild();
		configureOrDiagnose(uninterrupted, rig, "stability reset reference");
		uninterrupted.resumeSimulation();
		runOrDiagnose(uninterrupted, total, "stability reset reference run");
		auto const expected = uninterrupted.getStationSystemsSnapshot();
		auto const expectedEvents = uninterrupted.getStationSystemsEvents();

		core::World world("StabilityResetReplay", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		configureOrDiagnose(world, rig, "stability reset replay");
		world.resumeSimulation();
		runOrDiagnose(world, total / 2, "stability replay first half");
		world.pauseSimulation();
		world.resetSimulation();
		// After reset the audit sees the authored start: the pre-reset
		// history is gone - at most the fresh runtime's own tick-0
		// initialization announcement remains - and no ledger, observed
		// rate or trend sample leaked across the boundary.
		auto const afterReset = auditOf(world);
		require(afterReset.enabled, "reset disabled the station");
		auto const& resetEvents = world.getStationSystemsEvents();
		require(resetEvents.size() <= 1 &&
			(resetEvents.empty() || resetEvents.front().worldTick == 0),
			"reset leaked pre-reset event history");
		require(afterReset.worldTick == 0 && afterReset.elapsedSeconds == 0.0,
			"reset leaked the station clock");
		require(afterReset.trendSamplesRetained == 0,
			"reset leaked trend history");
		require(afterReset.ledgerChecks > 0 && afterReset.maxResidualRatio <= 1.0,
			"the reset audit did not re-check the ledgers from the authored start");
		world.resumeSimulation();
		runOrDiagnose(world, total, "stability replay full run");
		require(world.getStationSystemsSnapshot() == expected,
			"the reset replay produced a different station snapshot");
		require(world.getStationSystemsEvents() == expectedEvents,
			"the reset replay produced a different event history");
		require(auditOf(world).clean, "the reset replay failed the stability audit");
	}

	// Multi-World isolation: two Worlds running the same workload
	// interleaved tick-for-tick with different chunk sizes, alongside a
	// third World running a different station configuration, must all
	// stay independent - the twins land on identical state and the
	// odd-config World never perturbs them through any shared runtime.
	void stabilityMultiWorldIsolation(smoke::Context const&)
	{
		auto const& workload = core::bundledStationLongRunWorkload();
		auto const& rig = workload.settings;
		auto const total = scenarioTicks(900.0);
		core::World a("StabilityTwinA", 2, 2);
		a.pauseSimulation(); a.finishBuild();
		configureOrDiagnose(a, rig, "stability twin A");
		core::World b("StabilityTwinB", 2, 2);
		b.pauseSimulation(); b.finishBuild();
		configureOrDiagnose(b, rig, "stability twin B");
		// A third World with a deliberately different configuration: a
		// different register and observation cadence, interleaved with
		// the twins so any shared mutable runtime state would show up
		// as twin divergence.
		auto other = rig;
		other.observationIntervalSeconds = 2.0f;
		for (auto& store : other.stores)
			if (store.id == "drinking") store.startingQualified = 150.0;
		core::World c("StabilityOther", 2, 2);
		c.pauseSimulation(); c.finishBuild();
		configureOrDiagnose(c, other, "stability other-config world");
		a.resumeSimulation(); b.resumeSimulation(); c.resumeSimulation();
		// Interleave: A in single ticks, B in 7-tick chunks, C in
		// 13-tick chunks, all the way to the shared target.
		uint64_t doneA = 0, doneB = 0, doneC = 0;
		while (doneA < total || doneB < total || doneC < total)
		{
			if (doneA < total) { auto const n = std::min<uint64_t>(1, total - doneA); runOrDiagnose(a, n, "twin A"); doneA += n; }
			if (doneB < total) { auto const n = std::min<uint64_t>(7, total - doneB); runOrDiagnose(b, n, "twin B"); doneB += n; }
			if (doneC < total) { auto const n = std::min<uint64_t>(13, total - doneC); runOrDiagnose(c, n, "other world"); doneC += n; }
		}
		require(a.getStationSystemsSnapshot() == b.getStationSystemsSnapshot(),
			"interleaved twin Worlds diverged - station runtime state is not World-isolated");
		require(a.getStationSystemsEvents() == b.getStationSystemsEvents(),
			"interleaved twin Worlds produced different event histories");
		require(a.getStationSystemsSnapshot() != c.getStationSystemsSnapshot(),
			"the different-config World reads identical to the twins - the configs did not reach the runtime");
		require(auditOf(a).clean && auditOf(b).clean,
			"an interleaved twin failed the stability audit");
		// The twins still match the single-world result computed alone:
		// interleaving changed nothing observable.
		core::World solo("StabilitySolo", 2, 2);
		solo.pauseSimulation(); solo.finishBuild();
		configureOrDiagnose(solo, rig, "stability solo");
		solo.resumeSimulation();
		runOrDiagnose(solo, total, "stability solo run");
		require(solo.getStationSystemsSnapshot() == a.getStationSystemsSnapshot(),
			"the interleaved twins do not match the solo run");
	}
}

void registerStabilityChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"stabilityWorkloadBundled", stabilityWorkloadBundled});
	checks.push_back({"stabilityLongRun", stabilityLongRun});
	checks.push_back({"stabilityPathEquivalence", stabilityPathEquivalence});
	checks.push_back({"stabilityResetIsolation", stabilityResetIsolation});
	checks.push_back({"stabilityMultiWorldIsolation", stabilityMultiWorldIsolation});
}
