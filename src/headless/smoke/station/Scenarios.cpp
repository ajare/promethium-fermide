// #423: the packaged emergency and recovery scenario variants.
//
// These checks drive each authored scenario through the public World
// tick/snapshot/event/diagnostic seam and verify the documented
// expectations: the limiting services really limit, the key events fire
// in the documented order, the outcomes land inside the documented
// bounded ranges, and the run preserves lifecycle semantics across
// pause, reset and a document round trip. Traces of the event timeline
// are printed at each checkpoint so a failure shows why the outcome
// occurred, not just that it did.
#include "Scenarios.h"

#include "core/AgentTagRegistryDocument.h"
#include "core/StationReference.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace
{
	void require(bool condition, char const* message) { if (!condition) throw std::runtime_error(message); }

	auto scenarioTicks = [](double seconds)
	{
		return static_cast<uint64_t>(std::llround(seconds / core::World::getFixedTimestep()));
	};

	auto configureOrDiagnose = [](core::World& world,
		core::StationSystemsSettings const& settings, char const* what)
	{
		std::string diagnostic;
		if (!world.configureStationSystems(settings, false, &diagnostic))
		{
			std::fprintf(stderr, "%s configure failed: %s\n", what, diagnostic.c_str());
			throw std::runtime_error(std::string(what) + " configuration failed");
		}
	};

	auto runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s run failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};

	auto storeSnap = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.stores)
			if (entry.id == id) return entry;
		return core::StationSystemsStoreSnapshot{};
	};
	auto findCrew = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.crews)
			if (entry.id == id) return entry;
		return core::StationCrewSnapshot{};
	};
	auto findDelivery = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.deliveries)
			if (entry.id == id) return entry;
		return core::StationDeliverySnapshot{};
	};
	auto findMaintenance = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.maintenanceJobs)
			if (entry.id == id) return entry;
		return core::StationMaintenanceSnapshot{};
	};
	auto findChiller = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.chillers)
			if (entry.id == id) return entry;
		return core::StationChillerSnapshot{};
	};
	auto findTreatment = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.waterTreatments)
			if (entry.id == id) return entry;
		return core::StationWaterTreatmentSnapshot{};
	};
	auto findMicrobial = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.microbialProcessors)
			if (entry.id == id) return entry;
		return core::StationMicrobialProcessorSnapshot{};
	};
	auto findSolar = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.power.sources)
			if (entry.id == id) return entry;
		return core::StationSolarSourceSnapshot{};
	};
	auto findGenerator = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.power.generators)
			if (entry.id == id) return entry;
		return core::StationGeneratorSnapshot{};
	};
	auto findLoad = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.power.loads)
			if (entry.id == id) return entry;
		return core::StationEquipmentLoadSnapshot{};
	};
	auto findReservoir = [](core::StationSystemsSnapshot const& snap, std::string const& id)
	{
		for (auto const& entry : snap.thermal.reservoirs)
			if (entry.id == id) return entry;
		return core::StationThermalReservoirSnapshot{};
	};

	// Print the scenario's event timeline so the trace shows why the
	// outcomes occurred. Categories are the stable #418 vocabulary.
	void printTimeline(core::World const& world, std::string const& label,
		std::initializer_list<char const*> categories)
	{
		std::printf("[%s] event timeline:\n", label.c_str());
		for (auto const& event : world.getStationSystemsEvents())
		{
			auto const seconds = event.worldTick * core::World::getFixedTimestep();
			for (auto const* category : categories)
			{
				if (event.category == category)
				{
					std::printf("  t=%.1fs [%s] %s\n", seconds, event.category.c_str(),
						event.message.c_str());
					break;
				}
			}
		}
		std::fflush(stdout);
	}

	auto const& scenarioById(char const* id)
	{
		static auto const& scenarios = core::bundledStationScenarios();
		auto const found = std::find_if(scenarios.begin(), scenarios.end(),
			[&](core::BundledStationScenario const& scenario)
			{ return scenario.id == id; });
		if (found == scenarios.end())
			throw std::runtime_error(std::string("no bundled scenario with id '") + id + "'");
		return *found;
	}

	// Bring up a World on one bundled scenario, paused then running.
	void bringUpScenario(core::World& world, char const* id)
	{
		auto const& scenario = scenarioById(id);
		world.pauseSimulation();
		world.finishBuild();
		configureOrDiagnose(world, scenario.document.settings, id);
		world.resumeSimulation();
	}

	// Every per-unit and per-constituent ledger must close within the
	// published tolerance - the conservation contract holds through
	// every emergency, not just in nominal operation.
	void requireLedgersClose(core::StationSystemsSnapshot const& snap, char const* what)
	{
		for (auto const& balance : snap.balances)
			require(std::abs(balance.residual) <= snap.balanceTolerance
				* std::max(1.0, balance.totalStarting),
				(std::string(what) + ": the " + balance.unit
					+ " boundary balance did not close").c_str());
		for (auto const& balance : snap.constituentBalances)
			require(std::abs(balance.residual) <= snap.balanceTolerance
				* std::max(1.0, balance.starting),
				(std::string(what) + ": the " + balance.constituent
					+ " constituent balance did not close").c_str());
	}

	// ------------------------------------------------------------------
	// The bundled scenario set: every variant parses through the same
	// authoritative reader as the base reference, validates, carries its
	// documented expectations, and round-trips through its own text.
	// ------------------------------------------------------------------
	void scenariosBundled(smoke::Context const&)
	{
		auto const& scenarios = core::bundledStationScenarios();
		require(scenarios.size() == 4, "expected four packaged emergency scenarios");
		std::vector<std::string> ids;
		for (auto const& scenario : scenarios)
		{
			require(!scenario.id.empty(), "a bundled scenario has no id");
			ids.push_back(scenario.id);
			auto const& doc = scenario.document;
			require(doc.scenarioKind == "emergency",
				("bundled scenario '" + scenario.id + "' is not authored as an emergency drill").c_str());
			require(doc.schemaVersion == 96,
				("bundled scenario '" + scenario.id + "' is not on the current station schema").c_str());
			require(!doc.summary.empty(), ("bundled scenario '" + scenario.id + "' has no summary").c_str());
			require(doc.scenarioSeconds > 0.0,
				("bundled scenario '" + scenario.id + "' declares no scenario duration").c_str());
			require(!doc.qualification.empty(),
				("bundled scenario '" + scenario.id + "' declares no qualification limits").c_str());
			// The three documented expectation categories are all present
			// and every entry carries both the item and its bound/rationale.
			require(!doc.limitingServices.empty(),
				("bundled scenario '" + scenario.id + "' documents no limiting services").c_str());
			require(!doc.eventOrdering.empty(),
				("bundled scenario '" + scenario.id + "' documents no event ordering").c_str());
			require(!doc.outcomeRanges.empty(),
				("bundled scenario '" + scenario.id + "' documents no outcome ranges").c_str());
			for (auto const* expectations : { &doc.limitingServices, &doc.eventOrdering, &doc.outcomeRanges })
				for (auto const& expectation : *expectations)
					require(!expectation.item.empty() && !expectation.detail.empty(),
						("bundled scenario '" + scenario.id
							+ "' has an expectation without an item or documented bound").c_str());
			// The settings are ordinary authored configuration and pass
			// the same validation any saved World faces.
			require(doc.settings.enabled,
				("bundled scenario '" + scenario.id + "' is not authored enabled").c_str());
			std::string diagnostic;
			require(core::validateStationSystemsSettings(doc.settings, diagnostic),
				("bundled scenario '" + scenario.id + "' was refused: " + diagnostic).c_str());
			// The resource text round-trips to identical settings.
			core::StationReferenceDocument reparsed;
			require(core::parseStationReference(scenario.text, reparsed, &diagnostic),
				("re-parsing bundled scenario '" + scenario.id + "' failed: " + diagnostic).c_str());
			require(reparsed.settings == doc.settings,
				("re-parsing bundled scenario '" + scenario.id + "' produced different settings").c_str());
			require(reparsed.limitingServices == doc.limitingServices
				&& reparsed.eventOrdering == doc.eventOrdering
				&& reparsed.outcomeRanges == doc.outcomeRanges
				&& reparsed.scenarioKind == doc.scenarioKind,
				("re-parsing bundled scenario '" + scenario.id
					+ "' lost its documented expectations").c_str());
		}
		auto const expected = { "resupply-delay", "power-cooling-outage",
			"waste-constrained", "maintenance-recovery" };
		for (auto const& want : expected)
			require(std::find(ids.begin(), ids.end(), std::string(want)) != ids.end(),
				("bundled scenario set is missing '" + std::string(want) + "'").c_str());
		// The base reference is not an emergency drill and carries no
		// expectations: the variants extend the format, they do not
		// redefine the baseline.
		auto const& reference = core::bundledStationReference();
		require(reference.scenarioKind == "reference",
			"the base reference is not kinded as the normal-operation baseline");
		require(reference.limitingServices.empty() && reference.eventOrdering.empty()
			&& reference.outcomeRanges.empty(),
			"the base reference unexpectedly carries emergency expectations");
	}

	// ------------------------------------------------------------------
	// Resupply delay: the crew lives through two shortfall-and-rescue
	// cycles driven only by the authored thin stores and the delayed
	// scheduled deliveries.
	// ------------------------------------------------------------------
	void scenarioResupplyDelay(smoke::Context const&)
	{
		core::World world("ScenarioResupplyDelay", 2, 2);
		bringUpScenario(world, "resupply-delay");
		// Checkpoints walk the documented story in time order:
		// [end-second, expected crew scale, expected limiter fragment].
		struct Checkpoint { double seconds; double scale; std::string limiterFragment; };
		Checkpoint const ordered[] = {
			{ 599.0, 1.0, "" },           // still fed to the last full ration
			{ 601.0, 0.0, "food" },      // rationed on the empty food store
			{ 890.0, 0.0, "food" },      // still rationed just before the flight
			{ 910.0, 1.0, "" },           // rescued by the 900 s food flight
			{ 1799.0, 1.0, "" },
			{ 1801.0, 0.0, "drinking" },  // rationed at the protected reserve
			{ 2090.0, 0.0, "drinking" },
			{ 2110.0, 1.0, "" },          // rescued by the 2100 s water flight
			{ 2400.0, 1.0, "" },
		};
		double urineAtRationStart = 0.0;
		for (size_t index = 0; index < std::size(ordered); ++index)
		{
			auto const& checkpoint = ordered[index];
			auto const from = index == 0 ? 0.0 : ordered[index - 1].seconds;
			runOrDiagnose(world, scenarioTicks(checkpoint.seconds) - scenarioTicks(from),
				("resupply-delay leg to " + std::to_string(checkpoint.seconds)).c_str());
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "the resupply-delay run was not healthy");
			auto const& crew = findCrew(snap, "crew");
			require(std::abs(crew.metabolicScale - checkpoint.scale) < 1e-9,
				("resupply-delay: crew scale at " + std::to_string(checkpoint.seconds)
					+ " s is " + std::to_string(crew.metabolicScale) + ", expected "
					+ std::to_string(checkpoint.scale) + " (limiter: " + crew.limiter + ")").c_str());
			if (!checkpoint.limiterFragment.empty())
				require(crew.limiter.find(checkpoint.limiterFragment) != std::string::npos,
					("resupply-delay: crew limiter at " + std::to_string(checkpoint.seconds)
						+ " s is '" + crew.limiter + "', expected to name '"
						+ checkpoint.limiterFragment + "'").c_str());
			if (checkpoint.seconds == 601.0)
			{
				// Unmet demand is recorded at the full nominal rate while
				// the crew is rationed - the shortfall is visible, never
				// silently dropped.
				require(crew.lastFoodUnmet > 0.0,
					"the rationed crew recorded no unmet food demand");
				urineAtRationStart = crew.urineProduced;
			}
			if (checkpoint.seconds == 890.0)
			{
				// Waste honesty across the whole rationing window: with zero
				// scale the crew returns nothing - no phantom urine from
				// food that was never eaten.
				require(std::abs(crew.urineProduced - urineAtRationStart) < 1e-12,
					"the zero-scale crew produced waste from absent food");
				require(crew.lastFoodActual < 1e-12,
					"the rationed crew drew food it should not have had");
			}
		}
		auto const& snap = world.getStationSystemsSnapshot();
		// Both delayed flights landed through the ordinary delivery seam.
		require(findDelivery(snap, "delayedFoodFlight").occurrencesCompleted == 1,
			"the delayed food flight never arrived");
		require(findDelivery(snap, "delayedWaterFlight").occurrencesCompleted == 1,
			"the delayed water flight never arrived");
		// The food store ends with the delivered mass minus the resumed
		// draw: the rescue is real inventory, not a display flag.
		require(storeSnap(snap, "food").currentQualified > 15.0,
			"the delivered food did not land as usable inventory");
		require(storeSnap(snap, "drinking").currentQualified > 49.0,
			"the delivered water did not land above the protected reserve");
		requireLedgersClose(snap, "resupply-delay");
		printTimeline(world, "resupply-delay", { "delivery", "crew" });
	}

	// ------------------------------------------------------------------
	// Power and cooling outage: eclipse plus chiller derate, carried by
	// dispatch, battery and curtailment, recovered when the windows close.
	// ------------------------------------------------------------------
	void scenarioPowerCoolingOutage(smoke::Context const&)
	{
		core::World world("ScenarioPowerCooling", 2, 2);
		bringUpScenario(world, "power-cooling-outage");
		// Before the eclipse: solar carries the station.
		runOrDiagnose(world, scenarioTicks(119.0), "outage leg to 119 s");
		auto const& before = world.getStationSystemsSnapshot();
		require(findSolar(before, "solarMain").availableKW > 15.0,
			"the solar array was not carrying the station before the eclipse");
		require(before.power.unmetCriticalKW <= 1e-9,
			"unmet critical demand before the eclipse");
		// Eclipse onset: solar collapses to zero.
		runOrDiagnose(world, scenarioTicks(121.0) - scenarioTicks(119.0), "outage leg to 121 s");
		auto const& onset = world.getStationSystemsSnapshot();
		require(findSolar(onset, "solarMain").availableKW == 0.0,
			"the eclipse did not collapse solar availability");
		require(onset.power.batteryDischargeKW > 0.0,
			"the battery did not start covering the eclipse shortfall");
		require(onset.power.unmetCriticalKW <= 1e-9,
			"a protected floor went unserved at eclipse onset");
		// Mid-eclipse: the generator is dispatched on and ramping or at
		// rated; the chiller is capped at its derated ceiling; the
		// critical floors still hold.
		runOrDiagnose(world, scenarioTicks(200.0) - scenarioTicks(121.0), "outage leg to 200 s");
		auto const& mid = world.getStationSystemsSnapshot();
		auto const& gen = findGenerator(mid, "dieselGen");
		require(gen.phase != core::StationGeneratorPhase::Stopped,
			"automatic dispatch never started the generator during the eclipse");
		require(mid.power.unmetCriticalKW <= 1e-9,
			"a protected floor went unserved mid-eclipse");
		auto const& chiller = findChiller(mid, "chillerMain");
		// The outage caps the chiller at 25 percent of its 3 kW rating.
		// In this model the cold-side demand presented follows the capped
		// deliverable, so the shortfall shows up as the chilled loop not
		// being held at its setpoint - the service is capped, never
		// silently met from elsewhere.
		require(chiller.deliverableKW <= 0.25 * chiller.ratedCoolingCapacityKW + 1e-9,
			"the derated chiller presented more than its capped deliverable");
		require(chiller.coolingKW <= 0.25 * chiller.ratedCoolingCapacityKW + 1e-9,
			"the chiller delivered past its derated ceiling during the outage");
		require(!chiller.atChilledSetpoint,
			"the chilled loop was held at setpoint despite the cooling derate");
		auto const& chilled = findReservoir(mid, "chilledWater");
		require(chilled.temperatureC > chiller.chilledSetpointC,
			"the chilled loop is not riding above its setpoint under the capped service");
		// The loop's declared capacity rides the 300 s derate: it stays
		// within 1 K of its 8 C start (provisional thermal assumption;
		// declared limits are reports, never clamps).
		require(chilled.temperatureC < 9.0 && chilled.temperatureC > 7.0,
			"the chilled-water loop left its documented 1 K riding bound");
		// Sun return: solar comes back, dispatch releases the generator,
		// the shutdown-heat tail is real, and the battery recharges.
		runOrDiagnose(world, scenarioTicks(430.0) - scenarioTicks(200.0), "outage leg to 430 s");
		auto const& returned = world.getStationSystemsSnapshot();
		require(findSolar(returned, "solarMain").availableKW > 15.0,
			"the sun did not return");
		require(returned.power.batteryChargeKW > 0.0,
			"the battery never recharged on the solar surplus");
		// The chiller cap lifted with the window: full deliverable is
		// back above the derated ceiling.
		require(findChiller(returned, "chillerMain").deliverableKW
			> 0.25 * findChiller(returned, "chillerMain").ratedCoolingCapacityKW + 1e-9,
			"the chiller stayed capped after its outage window closed");
		// Recovery: by 720 s the generator is fully off service, every
		// external load is back at full delivery, and the station is
		// fully supplied.
		runOrDiagnose(world, scenarioTicks(720.0) - scenarioTicks(430.0), "outage leg to 720 s");
		auto const& recovered = world.getStationSystemsSnapshot();
		require(findGenerator(recovered, "dieselGen").phase == core::StationGeneratorPhase::Stopped,
			"the generator never returned to stopped after the eclipse");
		require(recovered.power.totalUnmetKW <= 1e-9,
			"the station did not fully recover its electrical supply after the eclipse");
		for (auto const& load : recovered.power.loads)
			require(load.deliveredFraction >= 1.0 - 1e-9,
				("load '" + load.id + "' never returned to full delivery after the eclipse").c_str());
		require(findChiller(recovered, "chillerMain").coolingUnmetKW <= 1e-9,
			"the chilled service never recovered after the derate window");
		require(recovered.power.unmetCriticalKW <= 1e-9,
			"unmet critical demand after recovery");
		requireLedgersClose(recovered, "power-cooling-outage");
		printTimeline(world, "power-cooling-outage",
			{ "outage", "generator", "battery", "dispatch", "load", "chiller" });
	}

	// ------------------------------------------------------------------
	// Constrained waste storage and stalled ejection: backpressure
	// stalls the services, ejection restarts recover them.
	// ------------------------------------------------------------------
	void scenarioWasteConstrained(smoke::Context const&)
	{
		core::World world("ScenarioWasteConstrained", 2, 2);
		bringUpScenario(world, "waste-constrained");
		// Act one: the residue store backs up from routed harvest waste
		// and digester residue and stalls the microbial stage between
		// 550 s and 750 s. The culture degrades honestly and stays
		// bounded while stalled.
		runOrDiagnose(world, scenarioTicks(750.0), "waste leg to 750 s");
		auto const& stalled = world.getStationSystemsSnapshot();
		auto const& dig = findMicrobial(stalled, "dig");
		require(dig.scale < 1e-9,
			"the digester was not stalled by its full residue store at 750 s");
		require(dig.limiter.find("residue") != std::string::npos,
			("digester limiter at 750 s is '" + dig.limiter
				+ "', expected to name the residue store").c_str());
		require(dig.condition > 0.95 && dig.condition <= 1.0,
			"the digester culture degraded faster than its documented bound while stalled");
		auto const feedAtStall = dig.feedConsumedKg;
		// Act two: the normal treatment concentrate store fills. The
		// tiny 0.005 kg buffer is destination-blocked by 900 s, fresh
		// water production stops, and the untreated feed accumulates
		// visibly upstream in the raw buffer.
		runOrDiagnose(world, scenarioTicks(900.0) - scenarioTicks(750.0), "waste leg to 900 s");
		auto const& mid = world.getStationSystemsSnapshot();
		auto const& wtr = findTreatment(mid, "wtr");
		require(wtr.scale < 1e-9,
			"the normal water treatment was not stalled by its full concentrate store at 900 s");
		require(wtr.limiter.find("concentrate") != std::string::npos,
			("normal treatment limiter at 900 s is '" + wtr.limiter
				+ "', expected to name the concentrate store").c_str());
		require(storeSnap(mid, "rawWater").currentQualified > 10.0,
			"the backed-up raw water feed is not accumulating visibly");
		// The digester is still stalled just before the ejection restart.
		runOrDiagnose(world, scenarioTicks(1490.0) - scenarioTicks(900.0), "waste leg to 1490 s");
		require(findMicrobial(world.getStationSystemsSnapshot(), "dig").scale < 1e-9,
			"the digester was not still stalled just before the residue ejection restart");
		// Recovery: the residue line restarts at 1500 s. Residue is
		// disposal-ready qualified terminal waste, so the ejection
		// removes real material, headroom opens, and the digester
		// resumes feeding within 30 s of the opening.
		runOrDiagnose(world, scenarioTicks(1530.0) - scenarioTicks(1490.0), "waste leg to 1530 s");
		auto const& resumed = world.getStationSystemsSnapshot();
		require(findMicrobial(resumed, "dig").feedConsumedKg > feedAtStall,
			"the digester did not resume after the residue ejection restart opened headroom");
		require(storeSnap(resumed, "residue").currentQualified < 0.6 - 1e-6,
			"the residue ejection removed nothing from the store");
		// The retained-stream contract: the concentrate block persists
		// to the end of the run. Concentrate lands unqualified and
		// ejection never touches held material, so the treatment stays
		// visibly stalled while the disposal-ready stream has recovered.
		runOrDiagnose(world, scenarioTicks(1800.0) - scenarioTicks(1530.0), "waste leg to 1800 s");
		auto const& end = world.getStationSystemsSnapshot();
		require(end.healthy, "the waste-constrained run was not healthy");
		require(findTreatment(end, "wtr").scale < 1e-9
			&& findTreatment(end, "wtr").limiter.find("concentrate") != std::string::npos,
			"the normal retained-stream block cleared when it should persist");
		require(storeSnap(end, "concentrate").currentUnqualified > 0.0,
			"the retained concentrate is not accounted as held material");
		requireLedgersClose(end, "waste-constrained");
		printTimeline(world, "waste-constrained", { "waste-ejection", "microbial" });
	}

	void scenarioMaintenanceRecovery(smoke::Context const&)
	{
		core::World world("ScenarioMaintenance", 2, 2);
		bringUpScenario(world, "maintenance-recovery");
		// Window one opens at 300 s with the kit short: the job blocks
		// naming the tool store.
		runOrDiagnose(world, scenarioTicks(301.0), "maintenance leg to 301 s");
		auto blocked = findMaintenance(world.getStationSystemsSnapshot(), "maintChiller");
		require(blocked.phase == core::StationMaintenancePhase::Blocked,
			"the maintenance job did not block at its first window with the kit short");
		require(blocked.blockReason.find("toolKit") != std::string::npos,
			("the maintenance block reason does not name the tool kit: '"
				+ blocked.blockReason + "'").c_str());
		// The window closes at 600 s: the occurrence is missed, never
		// back-filled.
		runOrDiagnose(world, scenarioTicks(601.0) - scenarioTicks(301.0), "maintenance leg to 601 s");
		auto missed = findMaintenance(world.getStationSystemsSnapshot(), "maintChiller");
		require(missed.occurrencesMissed == 1,
			"the closed first window was not recorded as exactly one missed occurrence");
		require(missed.occurrencesStarted == 0,
			"the job started despite the missing kit");
		// The kit drop lands at 900 s.
		runOrDiagnose(world, scenarioTicks(901.0) - scenarioTicks(601.0), "maintenance leg to 901 s");
		require(std::abs(storeSnap(world.getStationSystemsSnapshot(), "toolKit").currentQualified
			- 2.2) < 1e-9, "the tool-kit drop did not land as 2.2 kg");
		// Window two opens at 1200 s: the job starts and isolates the
		// chiller - a real cooling gap.
		runOrDiagnose(world, scenarioTicks(1201.0) - scenarioTicks(901.0), "maintenance leg to 1201 s");
		auto started = findMaintenance(world.getStationSystemsSnapshot(), "maintChiller");
		require(started.phase == core::StationMaintenancePhase::Active,
			"the maintenance job did not start at its second window with the kit present");
		require(started.isolated, "the job did not isolate its target installation");
		// During the isolation the chiller is out of service: the chilled
		// service is unmet and the loop warms within its documented
		// bound.
		runOrDiagnose(world, scenarioTicks(1400.0) - scenarioTicks(1201.0), "maintenance leg to 1400 s");
		auto const& during = world.getStationSystemsSnapshot();
		require(findChiller(during, "chillerMain").coolingKW <= 1e-9,
			"the isolated chiller kept cooling during the maintenance window");
		auto const& chilled = findReservoir(during, "chilledWater");
		require(chilled.temperatureC > 8.0 && chilled.temperatureC < 9.0,
			"the chilled-water loop left its documented warming bound during isolation");
		// Completion at 1500 s: the media is consumed into the waste
		// ledger, the kit returns, and the chiller is back in service.
		runOrDiagnose(world, scenarioTicks(1501.0) - scenarioTicks(1400.0), "maintenance leg to 1501 s");
		auto const& done = world.getStationSystemsSnapshot();
		auto const completed = findMaintenance(done, "maintChiller");
		require(completed.occurrencesCompleted == 1,
			"the second maintenance window did not complete");
		require(completed.occurrencesMissed == 1 && completed.occurrencesStarted == 1,
			"the miss/start counts drifted from the documented one-miss-one-start story");
		require(std::abs(storeSnap(done, "maintenanceWaste").currentQualified - 0.5) < 1e-9,
			"the consumed maintenance media did not land in the waste store");
		require(std::abs(storeSnap(done, "toolKit").currentQualified - 2.2) < 1e-9,
			"the reusable tool kit did not return to inventory");
		require(std::abs(storeSnap(done, "treatmentMedia").currentQualified - 19.5) < 0.01,
			"the treatment media consumption did not match the job requirement");
		require(findChiller(done, "chillerMain").coolingKW > 0.0,
			"the chiller never returned to service after the maintenance completed");
		requireLedgersClose(done, "maintenance-recovery");
		printTimeline(world, "maintenance-recovery", { "maintenance", "delivery" });
	}

	// ------------------------------------------------------------------
	// Lifecycle: pause, reset and a document round trip preserve the
	// scenario semantics - the same authored scenario replays
	// identically, and a paused scenario is stable under inspection.
	// ------------------------------------------------------------------
	void scenarioLifecycle(smoke::Context const& context)
	{
		auto const& scenario = scenarioById("power-cooling-outage");
		auto const leg = scenarioTicks(300.0);
		core::World world("ScenarioLifecycle", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		configureOrDiagnose(world, scenario.document.settings, "scenario lifecycle");
		world.resumeSimulation();
		runOrDiagnose(world, leg, "lifecycle leg 1");
		// Pause preserves the state in memory: repeated reads are stable
		// and no time advances.
		world.pauseSimulation();
		auto const paused = world.getStationSystemsSnapshot();
		auto const pausedEvents = world.getStationSystemsEvents();
		require(world.getStationSystemsSnapshot() == paused,
			"reading a paused scenario changed its state");
		require(world.isSimulationPaused(),
			"the scenario did not pause");
		// Resume continues from the preserved state, not a restart.
		world.resumeSimulation();
		runOrDiagnose(world, leg, "lifecycle leg 2");
		auto const continued = world.getStationSystemsSnapshot();
		require(continued.worldTick == paused.worldTick * 2,
			"resume did not continue from the paused tick");
		// Reset returns to the authored start: identical replay.
		world.pauseSimulation();
		world.resetSimulation();
		world.resumeSimulation();
		runOrDiagnose(world, leg, "lifecycle replay");
		require(world.getStationSystemsSnapshot() == paused,
			"the reset replay produced a different scenario state");
		require(world.getStationSystemsEvents() == pausedEvents,
			"the reset replay produced a different scenario event history");
		// Document round trip: the scenario is authored settings only;
		// the reopened World reproduces the same run from the same ticks.
		auto const path = context.temporaryRoot() / "scenario-lifecycle.world.yaml";
		world.pauseSimulation();
		world.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "the scenario document failed to reopen");
		require(reopened->getStationSystemsSettings() == scenario.document.settings,
			"the scenario settings did not survive the document round trip");
		reopened->resumeSimulation();
		runOrDiagnose(*reopened, leg, "lifecycle reopened run");
		require(reopened->getStationSystemsSnapshot() == paused,
			"the reopened scenario World did not reproduce the run");
		require(reopened->getStationSystemsEvents() == pausedEvents,
			"the reopened scenario World produced a different event history");
	}
}

void registerScenarioChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"scenariosBundled", scenariosBundled});
	checks.push_back({"scenarioResupplyDelay", scenarioResupplyDelay});
	checks.push_back({"scenarioPowerCoolingOutage", scenarioPowerCoolingOutage});
	checks.push_back({"scenarioWasteConstrained", scenarioWasteConstrained});
	checks.push_back({"scenarioMaintenanceRecovery", scenarioMaintenanceRecovery});
	checks.push_back({"scenarioLifecycle", scenarioLifecycle});
}
