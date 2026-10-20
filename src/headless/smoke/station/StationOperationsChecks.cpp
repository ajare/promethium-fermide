#include "StationOperationsChecks.h"
#include "StationRig.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/StationConnections.h"
#include <filesystem>
#include <fstream>

namespace
{
	using namespace smoke::station;

	void failures(smoke::Context const&)
	{
		SourceScope scope;
		core::World unaffected("Unaffected", 2, 2); enable(unaffected);
		using Failure = core::ScriptExecutionFailure;
		struct Fixture { char const* body; Failure failure; char const* message; };
		for (auto const& fixture : {
			Fixture{"error('failed')", Failure::LuaError, "failed"},
			Fixture{"error({})", Failure::LuaError, "Station step failed"},
			Fixture{"return 0/0", Failure::ConversionError, "non-finite"},
			Fixture{"return math.huge", Failure::ConversionError, "non-finite"},
			Fixture{"return '1'", Failure::ConversionError, "invalid completed ticks"},
			Fixture{"return 999", Failure::ConversionError, "accounting mismatch"},
			Fixture{"while true do end", Failure::InstructionBudgetExceeded, ""},
			Fixture{"pcall(function() while true do end end); return tick", Failure::InstructionBudgetExceeded, ""},
			Fixture{"local t = {}; for i=1,1000000 do t[i]=i end; return tick", Failure::MemoryBudgetExceeded, ""},
			Fixture{"pcall(function() local t = {}; for i=1,1000000 do t[i]=i end end); return tick", Failure::MemoryBudgetExceeded, ""},
			Fixture{"error(string.rep('x', 10000))", Failure::LuaError, "xxx"}})
		{
			// Commit a successful prefix, then mutate every object before failing
			// in the last system. Only public committed results may survive.
			std::string source = "return function(name) assert(os == nil and io == nil and debug == nil and package == nil and world == nil); return {advance=function(self,tick,seconds) self.attempted = tick; if tick <= 60 or name ~= 'Monitoring/supervision' then return tick end; " + std::string(fixture.body) + " end} end";
			core::setStationSystemsSourceLoader([source] { return source; });
			core::World failed("Failure", 2, 2); enable(failed);
			failed.resumeSimulation(); require(failed.advanceTicks(60), "Successful prefix failed");
			auto before = failed.getStationSystemsSnapshot();
			auto events = failed.getStationSystemsEvents();
			auto settings = failed.getStationSystemsSettings();
			require(!failed.advanceTick(), "Lua failure accepted");
			auto result = failed.getStationSystemsSnapshot();
			require(failed.isSimulationPaused() && failed.getSimulationTick() == 60 && !result.healthy,
				"Station failure not fail-visible and synchronized");
			before.healthy = false;
			require(result == before && failed.getStationSystemsEvents() == events, "Partial Lua step or event published");
			require(failed.getStationSystemsSettings() == settings, "Failure changed authored settings");
			auto diagnostics = failed.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].system == "Monitoring/supervision" && !diagnostics[0].message.empty(), "Missing structured diagnostic");
			require(diagnostics[0].worldTick == 61 && diagnostics[0].failure == fixture.failure, "Failure misclassified or misdated");
			require(diagnostics[0].message.size() <= 2048 && diagnostics[0].message.find(fixture.message) != std::string::npos,
				"Diagnostic missing context or unbounded");
			failed.resumeSimulation(); require(!failed.advanceTicks(10), "Failed Lua objects retried");
			require(failed.getStationSystemsDiagnostics().size() == 1 && failed.getStationSystemsSnapshot() == result,
				"Retry changed failed state or duplicated diagnostic");
			unaffected.resumeSimulation(); require(unaffected.advanceTick(), "Failure leaked to another World");
			failed.resetSimulation();
			require(failed.getSimulationTick() == 0 && failed.getStationSystemsSnapshot().healthy
				&& failed.getStationSystemsDiagnostics().empty(), "Reset did not clear failure");
			failed.resumeSimulation(); require(failed.advanceTicks(60), "Reset prefix failed");
			before.healthy = true;
			require(failed.getStationSystemsSnapshot() == before && failed.getStationSystemsEvents() == events,
				"Successful replay after failure was not deterministic");
			require(!failed.advanceTick() && failed.getStationSystemsSnapshot() == result, "Failed replay changed outcome");
		}
		for (auto const* source : {"error('constructor failed')", "while true do end",
			"pcall(function() while true do end end); return function() return {} end",
			"local t = {}; for i=1,1000000 do t[i]=i end"})
		{
			core::setStationSystemsSourceLoader([source] { return std::string(source); });
			core::World refused("Constructor", 2, 2); refused.pauseSimulation(); refused.finishBuild();
			auto before = yaml(refused);
			std::string diagnostic;
			require(!refused.configureStationSystems({true, 1}, false, &diagnostic)
				&& !diagnostic.empty() && yaml(refused) == before && !refused.getStationSystemsSnapshot().enabled,
				"Constructor failure mutated authored settings or omitted diagnostic");
		}
		// Caught application errors are not resource exhaustion. A locally
		// handled condition must be able to produce a valid successful step.
		core::setStationSystemsSourceLoader([] {
			return "return function(name) return {advance=function(self,tick) local ok = pcall(function() error('handled condition') end); assert(not ok); return tick end} end";
		});
		core::World handled("Handled", 2, 2); enable(handled); handled.resumeSimulation();
		require(handled.advanceTicks(120) && handled.getStationSystemsDiagnostics().empty()
			&& handled.getStationSystemsSnapshot().healthy, "Caught application error triggered software pause");
		// A real host-only fault: each authored power demand is finite,
		// but their sum overflows in the final host audit, after inventory,
		// battery, connection and incident changes have been staged. A
		// transfer between near-equal compositions is no longer a fault:
		// actual-constituent transport now conserves that material correctly.
		auto addHostOverflow = [](core::StationSystemsSettings& settings,
			std::string const& thermalClass, double atSeconds = 0.0)
		{
			for (unsigned index = 0; index < 2; ++index)
			{
				core::EquipmentLoadSettings load;
				load.id = "overflow-" + std::to_string(index);
				load.electricalDemandKW = 1e308;
				load.thermalClass = thermalClass;
				load.maxCurtailment = 1.0;
				if (atSeconds > 0.0)
				{
					load.operatingProfile.periodSeconds = atSeconds * 2;
					load.operatingProfile.segments = {{0.0, 0.0}, {atSeconds, 1.0}};
				}
				settings.equipmentLoads.push_back(load);
			}
		};
		// Host-ledger failure containment (#555): every sandbox step
		// succeeds with valid completed counts, but the host's final
		// finite-result audit fails. No partial mutation may become visible: the entire
		// public snapshot, the event log and the World clock must be
		// exactly what they were before the failed tick, apart from the
		// failure status and its diagnostic.
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostFailure("HostLedger", 2, 2);
		hostFailure.pauseSimulation(); hostFailure.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings source;
			source.id = "a"; source.unit = "kg"; source.capacity = 100.0;
			source.startingQualified = 100.0;
			source.composition = core::ResourceCompositionSettings{};
			core::ResourceStoreSettings destination;
			destination.id = "b"; destination.unit = "kg"; destination.capacity = 100.0;
			destination.composition = core::ResourceCompositionSettings{};
			settings.stores = {source, destination};
			settings.transfers = {{"a-to-b", "a", "b", "kg", 3000.0, 0}};
			core::EquipmentLoadSettings lamp;
			lamp.id = "lamp"; lamp.electricalDemandKW = 1.0;
			lamp.thermalClass = "low";
			settings.equipmentLoads = {lamp};
			addHostOverflow(settings, "low");
			settings.batteries = {{"battery", 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0}};
			std::string diagnostic;
			// Positive control: the old composition trap is now legitimate
			// transport. Only adding the overflowing demands must cause failure.
			auto control = settings; control.equipmentLoads = {lamp};
			control.stores[1].composition->moisture = 5e-10;
			core::World conserved("ConservedTransfer", 2, 2);
			conserved.pauseSimulation(); conserved.finishBuild();
			auto const controlConfigured = conserved.configureStationSystems(control, false, &diagnostic);
			require(controlConfigured, diagnostic.c_str()); conserved.resumeSimulation();
			require(conserved.advanceTick() && conserved.getStationSystemsSnapshot().healthy
				&& conserved.getStationSystemsSnapshot().transfers[0].lastCompleted > 0.0,
				"Near-equal-composition transport was mistaken for a host-ledger fault");
			auto const configured = hostFailure.configureStationSystems(settings, false, &diagnostic);
			require(configured, diagnostic.c_str());
		}
		hostFailure.resumeSimulation();
		auto const before = hostFailure.getStationSystemsSnapshot();
		auto const beforeEvents = hostFailure.getStationSystemsEvents();
		require(before.healthy && before.worldTick == 0, "Host-ledger fixture did not start healthy");
		require(!hostFailure.advanceTick(), "Host ledger violation accepted");
		auto const after = hostFailure.getStationSystemsSnapshot();
		require(!after.healthy && hostFailure.getSimulationTick() == 0
			&& hostFailure.isSimulationPaused(),
			"Host ledger failure not fail-visible and synchronized");
		auto expected = before;
		expected.healthy = false;
		require(after == expected, "Failed host tick published partially mutated ledger state");
		// Explicit spot checks beyond whole-snapshot equality: the failed
		// allocation, flow results and energy draw must not be visible.
		require(after.stores[0].currentQualified == 100.0
			&& after.stores[1].currentQualified == 0.0
			&& after.stores[0].lastTickOut == 0.0 && after.stores[1].lastTickIn == 0.0,
			"Failed transfer left committed inventory mutated");
		require(after.transfers[0].lastRequested == 0.0 && after.transfers[0].lastCompleted == 0.0
			&& after.transfers[0].lastUnmet == 0.0
			&& after.transfers[0].status == core::StationTransferStatus::Idle,
			"Failed transfer results escaped into the snapshot");
		require(after.power.batteries[0].storedKWh == 1.0
			&& after.power.batteries[0].lastDischargeKW == 0.0
			&& after.power.batteries[0].dischargedKWh == 0.0
			&& after.power.loads[0].suppliedKW == 0.0
			&& after.power.totalSuppliedKW == 0.0 && after.power.surplusKW == 0.0,
			"Failed energy draw escaped into the snapshot");
		require(hostFailure.getStationSystemsEvents() == beforeEvents,
			"Failed host tick published events");
		auto const hostDiagnostics = hostFailure.getStationSystemsDiagnostics();
		require(hostDiagnostics.size() == 1 && hostDiagnostics[0].system == "Power allocation"
			&& hostDiagnostics[0].failure == Failure::ConversionError
			&& hostDiagnostics[0].worldTick == 1
			&& hostDiagnostics[0].message == "non-finite total requested power is not a publishable physical result",
			("Host ledger failure diagnostic missing or wrong: " + (hostDiagnostics.empty()
				? std::string("none") : hostDiagnostics[0].system + ": " + hostDiagnostics[0].message)).c_str());
		// Terminal retry refusal: the failed runtime never runs again and
		// never mutates further.
		require(!hostFailure.advanceTicks(10), "Failed host ledger retried after failure");
		require(hostFailure.getStationSystemsSnapshot() == after
			&& hostFailure.getStationSystemsEvents() == beforeEvents
			&& hostFailure.getStationSystemsDiagnostics().size() == 1,
			"Retry changed failed host-ledger state or duplicated the diagnostic");
		// Reset recovery: the failure clears, and the same fixture fails
		// again identically without leaking partial state.
		hostFailure.resetSimulation();
		require(hostFailure.getSimulationTick() == 0
			&& hostFailure.getStationSystemsSnapshot().healthy
			&& hostFailure.getStationSystemsDiagnostics().empty(),
			"Reset did not clear the host ledger failure");
		hostFailure.resumeSimulation();
		require(!hostFailure.advanceTick(), "Reset host ledger failure not reproduced");
		require(hostFailure.getStationSystemsSnapshot() == after
			&& hostFailure.getStationSystemsEvents() == beforeEvents,
			"Failed host-ledger replay was not identical");
		// Resetting the source loader afterwards cannot undo leaked host
		// state because none was left behind.
		core::setStationSystemsSourceLoader({});
		require(hostFailure.getStationSystemsSnapshot() == after,
			"Resetting the source loader changed the failed World's snapshot");
		// A failed host tick must not leak outage state or incident history
		// past the rollback (#602). The outage pass mutates outage state
		// and appends window start/restoration events, and maintenance
		// appends start/completion/interruption events, all before the
		// audits that can still fail the tick - and an append into a full
		// bounded history evicts previously committed events. This fixture
		// schedules every one of those transitions onto the tick whose
		// finite-result audit fails: an outage restoration and a fresh
		// outage start, a maintenance completion, and a maintenance
		// interruption, with the event history already pinned at its
		// 256-entry bound. The complete public snapshot and the complete
		// event history must equal the preceding successful boundary
		// apart from the failure status and its diagnostic, and retry
		// refusal and reset replay must stay intact.
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostIncidents("HostIncidents", 2, 2);
		hostIncidents.pauseSimulation(); hostIncidents.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			// The fastest legal observation cadence fills the bounded event
			// history to its cap long before the failing tick, so the
			// failed tick's appends would evict committed events if the
			// rollback did not restore the whole history.
			settings.observationIntervalSeconds = 0.1f;
			auto plainStore = [](std::string id, std::string unit,
				double capacity, double qualified)
			{
				core::ResourceStoreSettings store;
				store.id = std::move(id);
				store.unit = std::move(unit);
				store.capacity = capacity;
				store.startingQualified = qualified;
				return store;
			};
			// The source starts empty so the delivery tick stages inventory
			// and transfer changes alongside the scheduled overflow fault.
			core::ResourceStoreSettings source = plainStore("a", "kg", 100.0, 0.0);
			source.composition = core::ResourceCompositionSettings{};
			core::ResourceStoreSettings destination = plainStore("b", "kg", 100.0, 0.0);
			destination.composition = core::ResourceCompositionSettings{};
			settings.stores = {source, destination,
				plainStore("tools", "kg", 100.0, 10.0),
				plainStore("parts", "kg", 100.0, 10.0),
				plainStore("waste", "kg", 10.0, 0.0),
				plainStore("diesel", "kWh", 100000.0, 100000.0)};
			settings.transfers = {{"a-to-b", "a", "b", "kg", 3000.0, 0}};
			core::ThermalReservoirSettings sink;
			sink.id = "sink";
			sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 100000.0;
			sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0;
			sink.maxTemperatureC = 60.0;
			settings.thermalReservoirs = {sink};
			core::CoolantLoopSettings loop;
			loop.id = "loop";
			loop.reservoirId = "sink";
			loop.coolantCapacityKg = 1.0;
			loop.startingCoolantKg = 1.0;
			loop.specificHeatKJPerKgK = 1.0;
			settings.coolantLoops = {loop};
			core::GeneratorSettings generator;
			generator.id = "gen";
			generator.fuelStoreId = "diesel";
			generator.thermalClass = "generation";
			generator.heatSinkId = "sink";
			generator.ratedOutputKW = 10.0;
			generator.efficiency = 0.5;
			generator.maxRampKWPerSecond = 100.0;
			generator.commandedOutputKW = 10.0;
			generator.startupPowerKW = 1.0;
			generator.startupSeconds = 1.0;
			generator.maxSinkTemperatureC = 90.0;
			settings.generators = {generator};
			addHostOverflow(settings, "generation", 120.0);
			// One outage window closes on the failing tick (its
			// restoration would be announced there) and another opens on
			// it (its start would be announced there).
			core::InstallationOutageSettings restoring;
			restoring.id = "outage-restore";
			restoring.installationId = "loop";
			restoring.capacityFraction = 0.0;
			restoring.firstOccurrenceSeconds = 110.0;
			restoring.durationSeconds = 10.0;
			core::InstallationOutageSettings starting;
			starting.id = "outage-start";
			starting.installationId = "loop";
			starting.capacityFraction = 0.0;
			starting.firstOccurrenceSeconds = 120.0;
			starting.durationSeconds = 10.0;
			settings.outages = {restoring, starting};
			// The completing job wins the waste-headroom contest while its
			// window runs; the lower-precedence job's reservation stands on
			// the last of that headroom, so the waste delivery on the
			// failing tick interrupts it exactly as the audit fails.
			core::MaintenanceJobSettings completer;
			completer.id = "maint-complete";
			completer.installationId = "gen";
			completer.firstOccurrenceSeconds = 110.0;
			completer.durationSeconds = 10.0;
			completer.priority = 10;
			completer.requirements = {{"tools", 5.0, false}, {"parts", 2.0, true}};
			completer.wasteStoreId = "waste";
			core::MaintenanceJobSettings interrupted;
			interrupted.id = "maint-interrupt";
			interrupted.installationId = "loop";
			interrupted.firstOccurrenceSeconds = 115.0;
			interrupted.durationSeconds = 20.0;
			interrupted.priority = 5;
			interrupted.requirements = {{"parts", 8.0, true}};
			interrupted.wasteStoreId = "waste";
			// A reusable-only job whose window opens exactly on the failing
			// tick: its start attempt and fresh reservation happen on the
			// tick the audit fails, so the start publication must roll back
			// too.
			core::MaintenanceJobSettings starter;
			starter.id = "maint-start";
			starter.installationId = "loop";
			starter.firstOccurrenceSeconds = 120.0;
			starter.durationSeconds = 10.0;
			starter.priority = 1;
			starter.requirements = {{"tools", 2.0, false}};
			settings.maintenanceJobs = {completer, interrupted, starter};
			// The failing tick: the first delivery feeds a real transfer;
			// the second eats the lower job's reserved waste headroom.
			// Both must roll back when the scheduled aggregate overflows.
			settings.deliveries = {
				{"resupply-a", "a", "kg", 50.0, 120.0, 0.0, true, 0},
				{"resupply-waste", "waste", "kg", 1.0, 120.0, 0.0, true, 0}};
			std::string diagnostic;
			require(hostIncidents.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostIncidents.resumeSimulation();
		require(hostIncidents.advanceTicks(7199),
			"the incident fixture failed before its failing tick");
		auto const incidentsBefore = hostIncidents.getStationSystemsSnapshot();
		auto const incidentsBeforeEvents = hostIncidents.getStationSystemsEvents();
		require(incidentsBefore.healthy && incidentsBefore.worldTick == 7199,
			"incident fixture not healthy at the boundary tick");
		require(incidentsBeforeEvents.size() == core::StationEventHistoryLimit,
			"incident fixture history not pinned at its bound before the failing tick");
		// The committed state the failing tick must not disturb: the
		// restoring outage is mid-window with nothing restored yet, the
		// starting outage has not begun, and both maintenance jobs are
		// Active with their reservations held.
		require(incidentsBefore.outages.size() == 2
			&& incidentsBefore.outages[0].id == "outage-restore"
			&& incidentsBefore.outages[0].active
			&& incidentsBefore.outages[0].occurrencesStarted == 1
			&& incidentsBefore.outages[0].occurrencesCompleted == 0
			&& incidentsBefore.outages[1].id == "outage-start"
			&& !incidentsBefore.outages[1].active
			&& incidentsBefore.outages[1].occurrencesStarted == 0,
			"outage state not as authored at the boundary tick");
		require(incidentsBefore.maintenanceJobs.size() == 3
			&& incidentsBefore.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& incidentsBefore.maintenanceJobs[0].occurrencesCompleted == 0
			&& incidentsBefore.maintenanceJobs[1].phase == core::StationMaintenancePhase::Active
			&& incidentsBefore.maintenanceJobs[1].occurrencesStarted == 1
			&& incidentsBefore.maintenanceJobs[1].requirements[0].reserved
			&& incidentsBefore.maintenanceJobs[2].id == "maint-start"
			&& incidentsBefore.maintenanceJobs[2].phase == core::StationMaintenancePhase::Idle
			&& incidentsBefore.maintenanceJobs[2].occurrencesStarted == 0,
			"maintenance state not as authored at the boundary tick");
		require(!hostIncidents.advanceTick(),
			"incident-bearing host ledger violation accepted");
		require(hostIncidents.isSimulationPaused() && hostIncidents.getSimulationTick() == 7199,
			"incident host failure not fail-visible and synchronized");
		auto const incidentsAfter = hostIncidents.getStationSystemsSnapshot();
		auto incidentsExpected = incidentsBefore;
		incidentsExpected.healthy = false;
		require(incidentsAfter == incidentsExpected,
			"failed incident tick published partially mutated snapshot state");
		require(hostIncidents.getStationSystemsEvents() == incidentsBeforeEvents,
			"failed incident tick published events or evicted committed history");
		// Explicit spot checks beyond whole-snapshot equality: the failed
		// tick's outage start and restoration, the maintenance completion
		// and interruption, and the material they would have spent are all
		// invisible through the public snapshot.
		require(incidentsAfter.outages[0].active
			&& incidentsAfter.outages[0].occurrencesCompleted == 0
			&& incidentsAfter.outages[0].lastRestoreSeconds == 0.0
			&& !incidentsAfter.outages[1].active
			&& incidentsAfter.outages[1].occurrencesStarted == 0
			&& incidentsAfter.outages[1].lastStartSeconds == 0.0,
			"failed tick leaked outage start or restoration state");
		require(incidentsAfter.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& incidentsAfter.maintenanceJobs[0].occurrencesCompleted == 0
			&& incidentsAfter.maintenanceJobs[1].phase == core::StationMaintenancePhase::Active
			&& incidentsAfter.maintenanceJobs[1].occurrencesMissed == 0
			&& incidentsAfter.maintenanceJobs[1].blockReason.empty()
			&& incidentsAfter.maintenanceJobs[2].phase == core::StationMaintenancePhase::Idle
			&& incidentsAfter.maintenanceJobs[2].occurrencesStarted == 0
			&& !incidentsAfter.maintenanceJobs[2].requirements[0].reserved,
			"failed tick leaked maintenance start, completion or interruption state");
		for (auto const& store : incidentsAfter.stores)
		{
			auto const expected = store.id == "tools" ? 10.0
				: store.id == "parts" ? 10.0
				: store.id == "diesel" ? incidentsBefore.stores[5].currentQualified : 0.0;
			require(store.currentQualified == expected && store.currentUnqualified == 0.0,
				"failed incident tick left committed inventory mutated");
		}
		auto const incidentDiagnostics = hostIncidents.getStationSystemsDiagnostics();
		require(incidentDiagnostics.size() == 1
			&& incidentDiagnostics[0].system == "Power allocation"
			&& incidentDiagnostics[0].failure == Failure::ConversionError
			&& incidentDiagnostics[0].worldTick == 7200
			&& incidentDiagnostics[0].message == "non-finite total requested power is not a publishable physical result",
			"incident host failure diagnostic missing or wrong");
		// Terminal retry refusal across the incident state.
		require(!hostIncidents.advanceTicks(10),
			"failed incident host ledger retried after failure");
		require(hostIncidents.getStationSystemsSnapshot() == incidentsAfter
			&& hostIncidents.getStationSystemsEvents() == incidentsBeforeEvents
			&& hostIncidents.getStationSystemsDiagnostics().size() == 1,
			"retry changed failed incident state or duplicated the diagnostic");
		// Reset reproducibility: the same fixture replays the identical
		// successful boundary and fails identically on the incident tick.
		hostIncidents.resetSimulation();
		require(hostIncidents.getSimulationTick() == 0
			&& hostIncidents.getStationSystemsSnapshot().healthy
			&& hostIncidents.getStationSystemsDiagnostics().empty(),
			"reset did not clear the incident host ledger failure");
		hostIncidents.resumeSimulation();
		require(hostIncidents.advanceTicks(7199),
			"incident replay diverged before the failing tick");
		require(hostIncidents.getStationSystemsSnapshot() == incidentsBefore
			&& hostIncidents.getStationSystemsEvents() == incidentsBeforeEvents,
			"incident replay of the successful boundary was not identical");
		require(!hostIncidents.advanceTick(),
			"incident host failure not reproduced after reset");
		require(hostIncidents.getStationSystemsSnapshot() == incidentsAfter
			&& hostIncidents.getStationSystemsEvents() == incidentsBeforeEvents,
			"failed incident replay was not identical");
		core::setStationSystemsSourceLoader({});
		// Non-finite derived magnitudes are never published as healthy
		// results (#553): two finite 1e308 kW arrays sum to infinite
		// generation. With the bundled mirror the sandbox rejects the
		// overflowed pool itself; with a non-mirroring sandbox the host
		// ledger audit fails the tick whole. Either way the tick fails
		// atomically: the World pauses, the public snapshot stays exactly
		// at the last committed tick apart from the failure status, no
		// event is published, and a machine-visible non-finite
		// diagnostic is recorded.
		auto allowExtremeSolar = [](core::StationSystemsSettings& settings)
		{
			// The default 1e12 cable cap would safely mask these intentionally
			// extreme generation magnitudes before they reach the pool audit.
			for (auto const& link : core::station_connections::build(settings).links)
				if (link.designKey.ownerType == "Solar") settings.connections[link.designKey].maximumThroughput = 1e308;
		};
		core::setStationSystemsSourceLoader({});
		core::World overflow("Overflow", 2, 2);
		overflow.pauseSimulation(); overflow.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}, {"array-b", 1e308, {}}};
			std::string diagnostic;
			allowExtremeSolar(settings);
			require(overflow.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		overflow.resumeSimulation();
		auto const overflowBefore = overflow.getStationSystemsSnapshot();
		auto const overflowEvents = overflow.getStationSystemsEvents();
		require(overflowBefore.healthy && overflowBefore.worldTick == 0,
			"Overflow fixture did not start healthy");
		require(!overflow.advanceTick(), "Infinite generation sum published as healthy");
		require(overflow.isSimulationPaused() && !overflow.getStationSystemsSnapshot().healthy
			&& overflow.getSimulationTick() == 0,
			"Non-finite output not fail-visible and synchronized");
		auto expectedOverflow = overflowBefore;
		expectedOverflow.healthy = false;
		require(overflow.getStationSystemsSnapshot() == expectedOverflow,
			"Failed non-finite tick published mutated snapshot state");
		require(overflow.getStationSystemsEvents() == overflowEvents,
			"Failed non-finite tick published events");
		auto const overflowDiagnostics = overflow.getStationSystemsDiagnostics();
		require(overflowDiagnostics.size() == 1 && overflowDiagnostics[0].worldTick == 1
			&& overflowDiagnostics[0].message.find("finite") != std::string::npos,
			"Non-finite output diagnostic missing or wrong");
		// The host ledger's own finiteness audit: with a sandbox that
		// mirrors nothing, the host must still refuse to commit an
		// infinite generation sum and name the failed subsystem.
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostOverflow("HostOverflow", 2, 2);
		hostOverflow.pauseSimulation(); hostOverflow.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}, {"array-b", 1e308, {}}};
			std::string diagnostic;
			allowExtremeSolar(settings);
			require(hostOverflow.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		hostOverflow.resumeSimulation();
		require(!hostOverflow.advanceTick(), "Host committed an infinite generation sum");
		require(hostOverflow.isSimulationPaused() && !hostOverflow.getStationSystemsSnapshot().healthy
			&& hostOverflow.getSimulationTick() == 0,
			"Host non-finite failure not fail-visible and synchronized");
		require(hostOverflow.getStationSystemsSnapshot().power.availableGenerationKW == 0.0
			&& hostOverflow.getStationSystemsSnapshot().power.surplusKW == 0.0,
			"Infinite generation escaped into the rolled-back snapshot");
		auto const hostOverflowDiagnostics = hostOverflow.getStationSystemsDiagnostics();
		require(hostOverflowDiagnostics.size() == 1
			&& hostOverflowDiagnostics[0].system == "Power allocation"
			&& hostOverflowDiagnostics[0].failure == Failure::ConversionError
			&& hostOverflowDiagnostics[0].worldTick == 1
			&& hostOverflowDiagnostics[0].message.find("non-finite") != std::string::npos,
			"Host non-finite audit diagnostic missing or wrong");
		// Finite extremes remain valid: a single 1e308 kW array is a
		// finite result, its load is served fully and the surplus stays
		// finite — the finiteness audit refuses only non-finite results
		// and never a large-but-finite magnitude.
		core::setStationSystemsSourceLoader({});
		core::World huge("HugeFinite", 2, 2);
		huge.pauseSimulation(); huge.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array-a", 1e308, {}}};
			core::EquipmentLoadSettings lamp;
			lamp.id = "lamp"; lamp.electricalDemandKW = 1.0; lamp.thermalClass = "low";
			settings.equipmentLoads = {lamp};
			std::string diagnostic;
			allowExtremeSolar(settings);
			require(huge.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		}
		huge.resumeSimulation();
		require(huge.advanceTicks(10) && huge.getStationSystemsSnapshot().healthy
			&& huge.getStationSystemsSnapshot().power.availableGenerationKW == 1e308
			&& huge.getStationSystemsSnapshot().power.surplusKW == 1e308
			&& huge.getStationSystemsSnapshot().power.totalSuppliedKW == 1.0,
			"Finite extreme generation refused as if non-finite");
		// #565: derived atmosphere pressures, the saturation model and
		// the thermal/class rollups cannot escape the non-finite-result
		// containment. Reproduction A: a 1e-310 m3 compartment with a
		// kilogram of gas is individually-valid authored input whose
		// starting ideal-gas pressure already overflows to infinity.
		// The authoring audit refuses it transactionally: no runtime,
		// no snapshot, no mutation of the World.
		core::World thinAir("ThinAir", 2, 2);
		thinAir.pauseSimulation(); thinAir.finishBuild();
		auto const thinAirBefore = yaml(thinAir);
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::AtmosphereSettings thin;
			thin.id = "air";
			thin.volumeM3 = 1e-310;
			thin.startingOxygenKg = 0.2;
			thin.startingNitrogenKg = 0.8;
			thin.startingTemperatureC = 20.0;
			thin.thermalCapacityKJPerK = 1.0;
			thin.minPressureKPa = 1.0;
			thin.maxPressureKPa = 200.0;
			settings.atmospheres = {thin};
			std::string diagnostic;
			require(!thinAir.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("non-finite pressure") != std::string::npos,
				"Overflowing starting atmosphere pressure not refused at authoring");
			require(yaml(thinAir) == thinAirBefore
				&& !thinAir.getStationSystemsSnapshot().enabled,
				"Refused atmosphere mutated the World or published a snapshot");
		}
		// Reproduction B: the Magnus saturation expression is singular
		// at -243.04 C and overflowed to infinity below it. The model
		// now carries a documented finite continuation - zero vapour
		// capacity at or below the singular temperature - so the
		// initial snapshot and every tick publish a finite saturation
		// of 0 instead of an overflow dressed up as a shortage.
		core::setStationSystemsSourceLoader({});
		core::World coldSat("ColdSaturation", 2, 2);
		coldSat.pauseSimulation(); coldSat.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::AtmosphereSettings cold;
			cold.id = "air";
			cold.volumeM3 = 1.0;
			cold.startingOxygenKg = 0.2;
			cold.startingNitrogenKg = 0.8;
			cold.startingTemperatureC = -243.05;
			cold.thermalCapacityKJPerK = 1.0;
			cold.minPressureKPa = 1.0;
			cold.maxPressureKPa = 200.0;
			settings.atmospheres = {cold};
			std::string diagnostic;
			require(coldSat.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		{
			auto const initial = coldSat.getStationSystemsSnapshot();
			require(initial.healthy && initial.atmospheres.size() == 1
				&& initial.atmospheres[0].saturationVapourPressureKPa == 0.0
				&& std::isfinite(initial.atmospheres[0].pressureKPa),
				"Initial snapshot published a non-finite saturation or pressure");
		}
		coldSat.resumeSimulation();
		require(coldSat.advanceTicks(10) && coldSat.getStationSystemsSnapshot().healthy
			&& coldSat.getStationSystemsDiagnostics().empty()
			&& coldSat.getStationSystemsSnapshot().atmospheres[0].saturationVapourPressureKPa == 0.0
			&& std::isfinite(coldSat.getStationSystemsSnapshot().atmospheres[0].relativeHumidity),
			"Saturation continuation did not keep the tick finite and healthy");
		// Reproduction C: two loops each circulate a finite 1e308 kg/s;
		// the station-wide circulation rollup the snapshot publishes
		// overflows to infinity even though every per-loop result is
		// finite. With the bundled mirror the sandbox rejects the
		// overflowed aggregate itself; with a non-mirroring sandbox the
		// host ledger refuses the commit naming the coolant-circulation
		// rollup. Either way the tick fails whole and nothing publishes.
		core::setStationSystemsSourceLoader({});
		core::World circulation("CirculationOverflow", 2, 2);
		circulation.pauseSimulation(); circulation.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 1000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = -100.0; sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = {sink};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.reservoirId = "sink";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 20.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1e308;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			std::string diagnostic;
			require(circulation.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		circulation.resumeSimulation();
		auto const circulationBefore = circulation.getStationSystemsSnapshot();
		auto const circulationEvents = circulation.getStationSystemsEvents();
		require(!circulation.advanceTick(), "Infinite circulation rollup published as healthy");
		require(circulation.isSimulationPaused()
			&& !circulation.getStationSystemsSnapshot().healthy
			&& circulation.getSimulationTick() == 0,
			"Circulation rollup failure not fail-visible and synchronized");
		auto expectedCirculation = circulationBefore;
		expectedCirculation.healthy = false;
		require(circulation.getStationSystemsSnapshot() == expectedCirculation
			&& circulation.getStationSystemsEvents() == circulationEvents,
			"Failed circulation rollup tick published partial state");
		{
			auto const diagnostics = circulation.getStationSystemsDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].worldTick == 1
				&& diagnostics[0].message.find("finite") != std::string::npos,
				"Circulation rollup diagnostic missing or wrong");
		}
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostCirculation("HostCirculation", 2, 2);
		hostCirculation.pauseSimulation(); hostCirculation.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 1000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = -100.0; sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = {sink};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.reservoirId = "sink";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 20.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1e308;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			std::string diagnostic;
			require(hostCirculation.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostCirculation.resumeSimulation();
		require(!hostCirculation.advanceTick(), "Host committed an infinite circulation rollup");
		auto const hostCirculationDiagnostics = hostCirculation.getStationSystemsDiagnostics();
		require(hostCirculationDiagnostics.size() == 1
			&& hostCirculationDiagnostics[0].system == "Coolant circulation"
			&& hostCirculationDiagnostics[0].failure == Failure::ConversionError
			&& hostCirculationDiagnostics[0].message
				.find("non-finite station total coolant circulation flow") != std::string::npos,
			"Host circulation rollup audit diagnostic missing or wrong");
		// Reproduction D: two 1e308 kW chillers in one service class
		// overflow the published cold-class capacity even though both
		// machines are gated off at their chilled setpoint and every
		// per-chiller result is finite. The capacity is authored-derived,
		// so the bundled sandbox's initial audit refuses the whole
		// configuration transactionally before any state exists, and the
		// host ledger's own class-rollup audit refuses the tick when a
		// sandbox mirrors nothing.
		core::setStationSystemsSourceLoader({});
		core::World coldClass("ColdClassOverflow", 2, 2);
		coldClass.pauseSimulation(); coldClass.finishBuild();
		auto const coldClassBefore = yaml(coldClass);
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings cold;
			cold.id = "cold"; cold.thermalClass = "moderate";
			cold.heatCapacityKJPerK = 1000.0; cold.startingTemperatureC = 20.0;
			cold.minTemperatureC = -100.0; cold.maxTemperatureC = 200.0;
			core::ThermalReservoirSettings hot = cold;
			hot.id = "hot";
			settings.thermalReservoirs = {cold, hot};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.reservoirId = "cold";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 5.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1.0;
			loopA.exchangerConductanceKWPerK = 1.0;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			core::ChillerSettings chillerA;
			chillerA.id = "chiller-a"; chillerA.thermalClass = "moderate";
			chillerA.coldLoopId = "loop-a"; chillerA.condenserReservoirId = "hot";
			chillerA.ratedCoolingCapacityKW = 1e308; chillerA.cop = 2.0;
			chillerA.auxiliaryPowerKW = 0.0; chillerA.chilledSetpointC = 5.0;
			chillerA.maxCondensingTemperatureC = 45.0;
			core::ChillerSettings chillerB = chillerA;
			chillerB.id = "chiller-b"; chillerB.coldLoopId = "loop-b";
			settings.chillers = {chillerA, chillerB};
			std::string diagnostic;
			require(!coldClass.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("cold-service class 'moderate' rollup is not finite")
					!= std::string::npos,
				"Overflowing cold-class capacity not refused at configuration");
			require(yaml(coldClass) == coldClassBefore
				&& !coldClass.getStationSystemsSnapshot().enabled,
				"Refused cold-class capacity mutated the World or published a snapshot");
		}
		core::setStationSystemsSourceLoader([] {
			return "return function() local n = 0 "
				"return {advance=function() n = n + 1; return n end} end";
		});
		core::World hostColdClass("HostColdClass", 2, 2);
		hostColdClass.pauseSimulation(); hostColdClass.finishBuild();
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.solarArrays = {{"array", 100.0, {}}};
			core::ThermalReservoirSettings cold;
			cold.id = "cold"; cold.thermalClass = "moderate";
			cold.heatCapacityKJPerK = 1000.0; cold.startingTemperatureC = 20.0;
			cold.minTemperatureC = -100.0; cold.maxTemperatureC = 200.0;
			core::ThermalReservoirSettings hot = cold;
			hot.id = "hot";
			settings.thermalReservoirs = {cold, hot};
			core::CoolantLoopSettings loopA;
			loopA.id = "loop-a"; loopA.reservoirId = "cold";
			loopA.coolantCapacityKg = 1.0; loopA.startingCoolantKg = 1.0;
			loopA.specificHeatKJPerKgK = 4.0; loopA.startingTemperatureC = 5.0;
			loopA.pumpPowerKW = 1.0; loopA.ratedFlowKgPerS = 1.0;
			loopA.exchangerConductanceKWPerK = 1.0;
			core::CoolantLoopSettings loopB = loopA;
			loopB.id = "loop-b";
			settings.coolantLoops = {loopA, loopB};
			core::ChillerSettings chillerA;
			chillerA.id = "chiller-a"; chillerA.thermalClass = "moderate";
			chillerA.coldLoopId = "loop-a"; chillerA.condenserReservoirId = "hot";
			chillerA.ratedCoolingCapacityKW = 1e308; chillerA.cop = 2.0;
			chillerA.auxiliaryPowerKW = 0.0; chillerA.chilledSetpointC = 5.0;
			chillerA.maxCondensingTemperatureC = 45.0;
			core::ChillerSettings chillerB = chillerA;
			chillerB.id = "chiller-b"; chillerB.coldLoopId = "loop-b";
			settings.chillers = {chillerA, chillerB};
			std::string diagnostic;
			require(hostColdClass.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
		}
		hostColdClass.resumeSimulation();
		require(!hostColdClass.advanceTick(), "Host committed an infinite cold-class capacity");
		auto const hostColdClassDiagnostics = hostColdClass.getStationSystemsDiagnostics();
		require(hostColdClassDiagnostics.size() == 1
			&& hostColdClassDiagnostics[0].system == "Chilled thermal service"
			&& hostColdClassDiagnostics[0].failure == Failure::ConversionError
			&& hostColdClassDiagnostics[0].message
				.find("cold-service class 'moderate' total cooling capacity") != std::string::npos,
			"Host cold-class rollup audit diagnostic missing or wrong");
		unaffected.resumeSimulation();
		require(unaffected.advanceTick() && unaffected.getStationSystemsSnapshot().healthy,
			"Non-finite containment leaked between Worlds");
	}
	// #413: scheduled outages and capacity reductions. An outage is a
	// hard ceiling on one registered installation's deliverable rated
	// capacity, opened and closed on the shared World time base. The
	// ceiling bites the moment a window opens, overlapping windows take
	// the deepest reduction, and a restoration only lifts the ceiling -
	// it never replenishes any inventory. Everything is exercised through
	// the World tick / snapshot / event seam, never the Lua internals.
	void outages(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto outageMessages = [](core::World& world)
		{
			std::vector<std::string> messages;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.rfind("Outage '", 0) == 0)
					messages.push_back(event.message);
			return messages;
		};
		// A generator that reaches full rated output well before any
		// outage window opens, so the ceiling - not startup or the ramp -
		// is what moves the output when the window opens.
		auto fuelStore = []
		{
			core::ResourceStoreSettings settings;
			settings.id = "diesel";
			settings.unit = "kWh";
			settings.capacity = 100000.0;
			settings.startingQualified = 100000.0;
			settings.protectedReserve = 0.0;
			return settings;
		};
		auto sinkReservoir = []
		{
			core::ThermalReservoirSettings settings;
			settings.id = "hot";
			settings.thermalClass = "generation";
			settings.heatCapacityKJPerK = 100000.0;
			settings.startingTemperatureC = 20.0;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto generatorSettings = []
		{
			core::GeneratorSettings settings;
			settings.id = "gen";
			settings.fuelStoreId = "diesel";
			settings.thermalClass = "generation";
			settings.heatSinkId = "hot";
			settings.ratedOutputKW = 10.0;
			settings.efficiency = 0.5;
			settings.maxRampKWPerSecond = 100.0;
			settings.commandedOutputKW = 10.0;
			settings.startupPowerKW = 1.0;
			settings.startupSeconds = 1.0;
			settings.shutdownHeatKW = 0.0;
			settings.shutdownSeconds = 0.0;
			settings.maxSinkTemperatureC = 90.0;
			return settings;
		};
		auto outage = [](std::string id, std::string target, double fraction,
			double first, double duration, double recurrence = 0.0, int priority = 0)
		{
			core::InstallationOutageSettings settings;
			settings.id = std::move(id);
			settings.installationId = std::move(target);
			settings.capacityFraction = fraction;
			settings.firstOccurrenceSeconds = first;
			settings.durationSeconds = duration;
			settings.recurrenceSeconds = recurrence;
			settings.priority = priority;
			return settings;
		};
		auto configure = [&](core::World& world,
			std::vector<core::InstallationOutageSettings> outageSettings,
			std::vector<core::CoolantLoopSettings> loops = {},
			std::vector<core::ChillerSettings> chillers = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {fuelStore()};
			settings.thermalReservoirs = {sinkReservoir()};
			settings.generators = {generatorSettings()};
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			field.illumination = sun;
			settings.solarArrays = {field};
			settings.coolantLoops = std::move(loops);
			settings.chillers = std::move(chillers);
			settings.outages = std::move(outageSettings);
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		// Refused authored settings: the contract is validated at the
		// crossing, never discovered mid-tick.
		for (auto mutation : {"fraction-negative", "fraction-one", "fraction-above-one",
			"unknown-installation", "zero-duration", "negative-first",
			"recurrence-below-duration", "duplicate-id", "empty-id"})
		{
			std::vector<core::InstallationOutageSettings> outageSettings = {
				outage("out1", "gen", 0.5, 10.0, 5.0)};
			if (std::string_view(mutation) == "fraction-negative")
				outageSettings[0].capacityFraction = -0.1;
			else if (std::string_view(mutation) == "fraction-one")
				outageSettings[0].capacityFraction = 1.0;
			else if (std::string_view(mutation) == "fraction-above-one")
				outageSettings[0].capacityFraction = 1.5;
			else if (std::string_view(mutation) == "unknown-installation")
				outageSettings[0].installationId = "ghost";
			else if (std::string_view(mutation) == "zero-duration")
				outageSettings[0].durationSeconds = 0.0;
			else if (std::string_view(mutation) == "negative-first")
				outageSettings[0].firstOccurrenceSeconds = -1.0;
			else if (std::string_view(mutation) == "recurrence-below-duration")
				outageSettings[0].recurrenceSeconds = 2.0;
			else if (std::string_view(mutation) == "duplicate-id")
				outageSettings.push_back(outage("out1", "gen", 0.25, 20.0, 5.0));
			else if (std::string_view(mutation) == "empty-id")
				outageSettings[0].id = "";
			core::World world("OutageRefusal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {fuelStore()};
			settings.thermalReservoirs = {sinkReservoir()};
			settings.generators = {generatorSettings()};
			settings.outages = std::move(outageSettings);
			require(!world.configureStationSystems(settings, false, &diagnostic),
				std::string("an invalid outage (").append(mutation).append(") was accepted").c_str());
		}
		// One-off outage: full output, capped while the window is open,
		// restored after, with the start and restore announced once.
		{
			core::World world("OutageOneOff", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// first at 20s, duration 10s -> window [20, 30).
			require(configure(world, {outage("out1", "gen", 0.5, 20.0, 10.0)}),
				"one-off outage scenario was rejected");
			world.resumeSimulation();
			// t = 12s: generator at full rated output, no outage yet.
			require(world.advanceTicks(12 * 60), "one-off run failed before the outage");
			auto const& before = world.getStationSystemsSnapshot();
			require(approx(before.power.generators[0].outputKW, 10.0, 1e-3)
				&& approx(before.power.generators[0].availabilityFactor, 1.0),
				"generator was not at full rated output before the outage");
			require(before.outages.size() == 1 && !before.outages[0].active
				&& before.outages[0].occurrencesStarted == 0,
				"the one-off outage was already active before its window");
			// t = 25s: window open, output capped to half the rating.
			require(world.advanceTicks(13 * 60), "one-off run failed into the outage");
			auto const& during = world.getStationSystemsSnapshot();
			require(during.outages[0].active
				&& during.outages[0].occurrencesStarted == 1
				&& during.outages[0].occurrencesCompleted == 0,
				"the one-off outage window did not open as authored");
			require(approx(during.power.generators[0].availabilityFactor, 0.5)
				&& approx(during.power.generators[0].outputKW, 5.0, 1e-2),
				"the outage did not cap the generator to its declared fraction");
			require(sawEvent(world, "began at"),
				"the outage start was not announced");
			// t = 35s: window closed, ceiling lifted, output restored.
			require(world.advanceTicks(10 * 60), "one-off run failed past the outage");
			auto const& after = world.getStationSystemsSnapshot();
			require(!after.outages[0].active
				&& after.outages[0].occurrencesCompleted == 1
				&& after.outages[0].exhausted,
				"the one-off outage did not restore and exhaust as authored");
			require(approx(after.power.generators[0].availabilityFactor, 1.0)
				&& approx(after.power.generators[0].outputKW, 10.0, 1e-2),
				"the generator output was not restored when the outage lifted");
			require(sawEvent(world, "restored full declared availability"),
				"the outage restoration was not announced");
			require(sawEvent(world, "no inventory replenished"),
				"the restoration event did not state that no inventory was replenished");
		}
		// Overlapping outages take the deepest reduction regardless of
		// declaration order.
		{
			core::World world("OutageOverlap", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// Both windows cover t = 25s: 0.5 and 0.25 -> effective 0.25.
			require(configure(world, {
				outage("outA", "gen", 0.5, 20.0, 10.0),
				outage("outB", "gen", 0.25, 22.0, 8.0)}),
				"overlapping outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(25 * 60), "overlap run failed");
			auto const& snap = world.getStationSystemsSnapshot();
			require(approx(snap.power.generators[0].availabilityFactor, 0.25)
				&& approx(snap.power.generators[0].outputKW, 2.5, 1e-2),
				"overlapping outages did not take the deepest reduction");
		}
		// #577: newly due outage starts and restorations are announced in
		// the declared scenario event order - occurrence simulation time
		// ascending, then service priority descending, then declaration
		// order - the same rule that governs deliveries and ejections
		// (#392). The deepest-wins availability, exactly-once occurrence
		// counts and no-replenishment semantics are unchanged.
		// Coincident starts: the higher-priority schedule is announced
		// first regardless of declaration order.
		{
			core::World world("OutagePriorityOrder", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {
				outage("low", "gen", 0.5, 0.0, 10.0, 0.0, 0),
				outage("high", "gen", 0.5, 0.0, 10.0, 0.0, 100)}),
				"the coincident-priority outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(1), "coincident-priority run failed");
			auto const messages = outageMessages(world);
			require(messages.size() == 2
				&& messages[0].find("'high'") != std::string::npos
				&& messages[0].find("began") != std::string::npos
				&& messages[1].find("'low'") != std::string::npos
				&& messages[1].find("began") != std::string::npos,
				"coincident outage starts were not announced higher-priority-first");
		}
		// Coincident starts with equal priority: the stable declaration
		// tie-break keeps the first-declared schedule first.
		{
			core::World world("OutageEqualPriorityOrder", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {
				outage("declFirst", "gen", 0.5, 0.0, 10.0, 0.0, 5),
				outage("declSecond", "gen", 0.5, 0.0, 10.0, 0.0, 5)}),
				"the equal-priority outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(1), "equal-priority run failed");
			auto const messages = outageMessages(world);
			require(messages.size() == 2
				&& messages[0].find("'declFirst'") != std::string::npos
				&& messages[1].find("'declSecond'") != std::string::npos,
				"equal-priority coincident starts did not keep declaration order");
		}
		// Unequal sub-tick event times that come due inside the same fixed
		// tick: the earlier occurrence is announced first even when the
		// later occurrence has the higher priority.
		{
			core::World world("OutageSubTickOrder", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {
				outage("early_low", "gen", 0.5, 10.008, 5.0, 0.0, 0),
				outage("late_high", "gen", 0.5, 10.012, 5.0, 0.0, 100)}),
				"the sub-tick ordering outage scenario was rejected");
			world.resumeSimulation();
			// t = 10.0s: neither sub-tick occurrence is due yet.
			require(world.advanceTicks(600), "sub-tick run failed before the events");
			require(outageMessages(world).empty(),
				"a sub-tick outage event fired before its occurrence time");
			// One more tick carries both 10.008s and 10.012s into the same
			// sampled tick; the earlier time wins over the higher priority.
			require(world.advanceTicks(1), "sub-tick run failed into the events");
			auto const messages = outageMessages(world);
			require(messages.size() == 2
				&& messages[0].find("'early_low'") != std::string::npos
				&& messages[1].find("'late_high'") != std::string::npos,
				"same-tick outage events were not ordered by occurrence time over priority");
		}
		// Same-boundary start/restore inside one schedule (recurrence ==
		// duration): the closing window's restoration is announced before
		// the opening window's start, so the log reads in causal order.
		{
			core::World world("OutageSameBoundaryOrder", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {
				outage("cont", "gen", 0.5, 10.0, 10.0, 10.0, 0)}),
				"the contiguous-window outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(1200), "contiguous-window run failed");
			auto const messages = outageMessages(world);
			require(messages.size() == 3
				&& messages[0].find("began at 10.000000") != std::string::npos
				&& messages[1].find("restored") != std::string::npos
				&& messages[1].find("at 20.000000") != std::string::npos
				&& messages[2].find("began at 20.000000") != std::string::npos,
				"a same-boundary restore was not announced before the next window's start");
			auto const& snap = world.getStationSystemsSnapshot().outages;
			require(snap.size() == 1 && snap[0].occurrencesStarted == 2
				&& snap[0].occurrencesCompleted == 1 && snap[0].active,
				"the contiguous-window occurrence counts were not exactly-once");
		}
		// Cross-schedule same-boundary start/restore: priority orders the
		// announcements across event kinds - the higher-priority start is
		// announced before the lower-priority restoration.
		{
			core::World world("OutageCrossBoundaryPriority", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {
				outage("restorer", "gen", 0.5, 0.0, 10.0, 0.0, 0),
				outage("starter", "gen", 0.5, 10.0, 10.0, 0.0, 100)}),
				"the cross-boundary priority outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(600), "cross-boundary priority run failed");
			auto const messages = outageMessages(world);
			require(messages.size() == 3
				&& messages[0].find("'restorer'") != std::string::npos
				&& messages[0].find("began") != std::string::npos
				&& messages[1].find("'starter'") != std::string::npos
				&& messages[1].find("began") != std::string::npos
				&& messages[2].find("'restorer'") != std::string::npos
				&& messages[2].find("restored") != std::string::npos,
				"coincident start/restore from different schedules ignored priority");
		}
		// Batched versus stepped replay: the same scenario announces the
		// same ordered event log and the same exactly-once occurrence
		// counts whether the ticks arrive in one accelerated update or
		// one at a time.
		{
			auto scenario = [&]
			{
				// a: windows [5,10), [15,20), [25,30), [35,40) prio 1;
				// b: windows [10,15), [20,25), [30,35) prio 2. Every
				// boundary at 10..35s is a coincident start/restore pair
				// ordered by priority.
				return std::vector<core::InstallationOutageSettings>{
					outage("a", "gen", 0.5, 5.0, 5.0, 10.0, 1),
					outage("b", "gen", 0.25, 10.0, 5.0, 10.0, 2)};
			};
			core::World batched("OutageBatchedOrder", 2, 2);
			batched.pauseSimulation(); batched.finishBuild();
			require(configure(batched, scenario()),
				"the batched-order outage scenario was rejected");
			batched.resumeSimulation();
			require(batched.advanceTicks(35 * 60), "batched-order run failed");
			core::World stepped("OutageSteppedOrder", 2, 2);
			stepped.pauseSimulation(); stepped.finishBuild();
			require(configure(stepped, scenario()),
				"the stepped-order outage scenario was rejected");
			stepped.resumeSimulation();
			for (int tick = 0; tick < 35 * 60; ++tick)
				require(stepped.advanceTicks(1), "stepped-order run failed");
			auto const batchedMessages = outageMessages(batched);
			auto const steppedMessages = outageMessages(stepped);
			require(batchedMessages == steppedMessages,
				"batched and stepped runs announced outage events in different orders");
			// 13 occurrences: a starts at 5/15/25/35 and restores at
			// 10/20/30; b starts at 10/20/30 and restores at 15/25/35.
			require(batchedMessages.size() == 13,
				"the batched run did not announce every outage occurrence exactly once");
			require(batchedMessages.back().find("'a'") != std::string::npos
				&& batchedMessages.back().find("began") != std::string::npos
				&& batchedMessages[batchedMessages.size() - 2].find("'b'") != std::string::npos
				&& batchedMessages[batchedMessages.size() - 2].find("restored") != std::string::npos,
				"the coincident t=35s restore/start pair was not priority-ordered");
			auto const& batchedSnap = batched.getStationSystemsSnapshot().outages;
			auto const& steppedSnap = stepped.getStationSystemsSnapshot().outages;
			require(batchedSnap == steppedSnap
				&& batchedSnap[0].occurrencesStarted == 4
				&& batchedSnap[0].occurrencesCompleted == 3
				&& batchedSnap[1].occurrencesStarted == 3
				&& batchedSnap[1].occurrencesCompleted == 3,
				"occurrence counts differed between the batched and stepped runs");
			require(batchedSnap[0].active && !batchedSnap[1].active
				&& batchedSnap[1].exhausted == false
				&& batchedSnap[0].exhausted == false,
				"the batched run's final window state differs from the authored schedule");
		}
		// Recurring outage: the same window repeats every recurrenceSeconds.
		{
			core::World world("OutageRecurring", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// first 10s, duration 5s, recurrence 20s -> windows [10,15), [30,35).
			require(configure(world, {outage("outR", "gen", 0.4, 10.0, 5.0, 20.0)}),
				"recurring outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(12 * 60), "recurring run failed in window 1");
			auto const& w1 = world.getStationSystemsSnapshot();
			require(w1.outages[0].active && w1.outages[0].occurrencesStarted == 1
				&& approx(w1.power.generators[0].outputKW, 4.0, 1e-2),
				"the first recurring window did not bite as authored");
			require(world.advanceTicks(13 * 60), "recurring run failed between windows");
			auto const& gap = world.getStationSystemsSnapshot();
			require(!gap.outages[0].active && !gap.outages[0].exhausted
				&& approx(gap.power.generators[0].outputKW, 10.0, 1e-2),
				"the recurring outage did not release between windows");
			require(world.advanceTicks(7 * 60), "recurring run failed in window 2");
			auto const& w2 = world.getStationSystemsSnapshot();
			require(w2.outages[0].active && w2.outages[0].occurrencesStarted == 2
				&& approx(w2.power.generators[0].outputKW, 4.0, 1e-2),
				"the second recurring window did not bite as authored");
		}
		// A full outage (fraction 0) stops the machine outright and a
		// restoration never invents fuel: the drained store stays drained.
		{
			core::World world("OutageFullNoReplenish", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			core::ResourceStoreSettings fuel;
			fuel.id = "diesel"; fuel.unit = "kWh";
			fuel.capacity = 50.0; fuel.startingQualified = 50.0; fuel.protectedReserve = 0.0;
			settings.stores = {fuel};
			settings.thermalReservoirs = {sinkReservoir()};
			settings.generators = {generatorSettings()};
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			field.illumination = sun;
			settings.solarArrays = {field};
			settings.outages = {outage("outFull", "gen", 0.0, 5.0, 5.0)};
			require(world.configureStationSystems(settings, false, nullptr),
				"full-outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(7 * 60), "full-outage run failed");
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.outages[0].active
				&& approx(snap.power.generators[0].availabilityFactor, 0.0)
				&& snap.power.generators[0].outputKW == 0.0,
				"a full outage left deliverable capacity behind");
			// The store drained while the generator ran before the outage;
			// the restoration lifts the ceiling but cannot refill it.
			require(snap.stores[0].currentQualified < 50.0,
				"the generator never drew fuel before the outage");
			auto const drained = snap.stores[0].currentQualified;
			require(world.advanceTicks(8 * 60), "full-outage restore run failed");
			auto const& restored = world.getStationSystemsSnapshot();
			require(!restored.outages[0].active
				&& restored.stores[0].currentQualified <= drained,
				"the restoration replenished the drained fuel store");
		}
		// Coolant-loop and chiller installations carry the same ceiling on
		// their circulation and cooling capacity.
		{
			core::CoolantLoopSettings loop;
			loop.id = "loop";
			loop.reservoirId = "hot";
			loop.coolantCapacityKg = 10.0;
			loop.startingCoolantKg = 10.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.ratedFlowKgPerS = 2.0;
			loop.pumpPowerKW = 1.0;
			loop.exchangerConductanceKWPerK = 5.0;
			loop.freezeProtectionSetpointC = -100.0;
			core::World world("OutageLoopChiller", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configure(world, {outage("outLoop", "loop", 0.5, 5.0, 100.0)},
				{loop}),
				"loop outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(10 * 60), "loop outage run failed");
			auto const& snap = world.getStationSystemsSnapshot();
			require(snap.coolantLoops[0].availabilityFactor == 0.5
				&& approx(snap.coolantLoops[0].flowKgPerS, 1.0, 1e-2),
				"the loop outage did not halve circulation capacity");
		}
		// #575: an outage names its chiller by stable installation id.
		// With two chillers on distinct coolant loops, outaging the
		// second-named chiller must take the second out of service and
		// leave the first untouched - never default to whichever chiller
		// happens to sit at index 0.
		auto coldReservoir = [](std::string id)
		{
			core::ThermalReservoirSettings settings;
			settings.id = std::move(id);
			settings.thermalClass = "low";
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = 20.0;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto chillerLoop = [](std::string id, std::string reservoirId)
		{
			core::CoolantLoopSettings settings;
			settings.id = std::move(id);
			settings.reservoirId = std::move(reservoirId);
			settings.coolantCapacityKg = 10.0;
			settings.startingCoolantKg = 10.0;
			settings.specificHeatKJPerKgK = 4.0;
			settings.startingTemperatureC = 20.0;
			settings.freezeProtectionSetpointC = -100.0;
			settings.pumpPowerKW = 0.5;
			settings.ratedFlowKgPerS = 2.0;
			settings.exchangerConductanceKWPerK = 5.0;
			return settings;
		};
		auto chiller = [](std::string id, std::string coldLoopId)
		{
			core::ChillerSettings settings;
			settings.id = std::move(id);
			settings.thermalClass = "low";
			settings.coldLoopId = std::move(coldLoopId);
			settings.condenserReservoirId = "hot";
			settings.ratedCoolingCapacityKW = 2.0;
			settings.cop = 1.0;
			settings.chilledSetpointC = 5.0;
			settings.maxCondensingTemperatureC = 45.0;
			return settings;
		};
		auto configureChillers = [&](core::World& world,
			std::vector<core::InstallationOutageSettings> outageSettings)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = {coldReservoir("cold1"),
				coldReservoir("cold2"), coldReservoir("hot")};
			settings.coolantLoops = {chillerLoop("loop1", "cold1"),
				chillerLoop("loop2", "cold2")};
			settings.chillers = {chiller("first", "loop1"), chiller("second", "loop2")};
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			field.illumination = sun;
			settings.solarArrays = {field};
			settings.outages = std::move(outageSettings);
			return world.configureStationSystems(settings, false, &diagnostic);
		};
		// Full outage of the second-named chiller: the named machine
		// leaves service while the other keeps cooling at its full rating.
		{
			core::World world("OutageNamedChiller", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configureChillers(world, {outage("second_off", "second", 0.0, 5.0, 10.0)}),
				"the named-chiller full-outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(4 * 60), "named-chiller run failed before the outage");
			auto const before = world.getStationSystemsSnapshot();
			require(before.chillers[0].availabilityFactor == 1.0
				&& before.chillers[1].availabilityFactor == 1.0
				&& approx(before.chillers[0].coolingKW, 2.0, 1e-2)
				&& approx(before.chillers[1].coolingKW, 2.0, 1e-2),
				"both chillers were not at full service before the outage window");
			require(world.advanceTicks(2 * 60), "named-chiller run failed into the outage");
			auto const during = world.getStationSystemsSnapshot();
			require(during.outages[0].active && during.outages[0].installationKind == "chiller"
				&& during.outages[0].installationId == "second",
				"the outage did not report the chiller it names as its target");
			require(during.chillers[1].availabilityFactor == 0.0
				&& during.chillers[1].coolingKW == 0.0 && !during.chillers[1].admitted,
				"the full outage did not take the named chiller out of service");
			require(during.chillers[0].availabilityFactor == 1.0
				&& approx(during.chillers[0].coolingKW, 2.0, 1e-2),
				"the outage touched the non-target chiller instead of the named one");
			require(sawEvent(world, "chiller 'second'"),
				"the outage event did not name the targeted chiller");
			require(world.advanceTicks(9 * 60), "named-chiller run failed past the outage");
			auto const restored = world.getStationSystemsSnapshot();
			require(!restored.outages[0].active && restored.outages[0].exhausted
				&& restored.chillers[1].availabilityFactor == 1.0
				&& approx(restored.chillers[1].coolingKW, 2.0, 1e-2),
				"the named chiller did not return to service when the outage restored");
			// Reset restarts the authored schedule from its beginning; the
			// replay outages the same named chiller in the same window.
			world.pauseSimulation(); world.resetSimulation();
			auto const fresh = world.getStationSystemsSnapshot();
			require(fresh.outages[0].occurrencesStarted == 0
				&& fresh.chillers[0].availabilityFactor == 1.0
				&& fresh.chillers[1].availabilityFactor == 1.0,
				"reset did not restore full declared availability on both chillers");
			world.resumeSimulation();
			require(world.advanceTicks(6 * 60), "named-chiller reset replay failed");
			require(world.getStationSystemsSnapshot().chillers == during.chillers
				&& world.getStationSystemsSnapshot().outages == during.outages,
				"the reset replay did not outage the same named chiller");
		}
		// Partial outage: the named chiller keeps half its rated duty and
		// the other chiller is untouched.
		{
			core::World world("OutagePartialChiller", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configureChillers(world, {outage("second_half", "second", 0.5, 5.0, 10.0)}),
				"the named-chiller partial-outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(6 * 60), "partial named-chiller run failed into the outage");
			auto const during = world.getStationSystemsSnapshot();
			require(during.outages[0].active
				&& during.outages[0].installationKind == "chiller"
				&& during.outages[0].installationId == "second",
				"the partial outage did not report the chiller it names as its target");
			require(during.chillers[1].availabilityFactor == 0.5
				&& approx(during.chillers[1].coolingKW, 1.0, 1e-2),
				"the partial outage did not cap the named chiller to half its rated duty");
			require(during.chillers[0].availabilityFactor == 1.0
				&& approx(during.chillers[0].coolingKW, 2.0, 1e-2),
				"the partial outage disturbed the non-target chiller");
			require(world.advanceTicks(9 * 60), "partial named-chiller run failed past the outage");
			auto const restored = world.getStationSystemsSnapshot();
			require(!restored.outages[0].active
				&& restored.chillers[1].availabilityFactor == 1.0
				&& approx(restored.chillers[1].coolingKW, 2.0, 1e-2),
				"the named chiller did not regain its full rated duty after restoration");
		}
		// Load: the document round trip reconstructs the same authored
		// scenario, and the reopened World outages the same named chiller
		// tick-for-tick.
		{
			namespace filesystem = std::filesystem;
			auto const path = context.temporaryRoot() / "named-chiller.world.yaml";
			core::World original("OutageNamedChillerLoad", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			require(configureChillers(original, {outage("second_off", "second", 0.0, 5.0, 10.0)}),
				"the load-round-trip named-chiller scenario was rejected");
			original.resumeSimulation();
			require(original.advanceTicks(6 * 60), "load-round-trip original run failed");
			auto const expected = original.getStationSystemsSnapshot();
			require(expected.chillers[1].availabilityFactor == 0.0
				&& expected.chillers[0].availabilityFactor == 1.0,
				"the original run did not outage the named chiller before saving");
			original.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"the outage settings did not survive the document round trip");
			reopened->resumeSimulation();
			require(reopened->advanceTicks(6 * 60), "reopened named-chiller run failed");
			require(reopened->getStationSystemsSnapshot().chillers == expected.chillers
				&& reopened->getStationSystemsSnapshot().outages == expected.outages,
				"the reopened World did not outage the same named chiller as the original");
		}
		// Host/Lua observable agreement: the sandbox resolves the outage
		// target by stable installation id, and its per-chiller view -
		// which object is capped and which keeps cooling - matches the
		// host snapshot for the same ticks.
		{
			SourceScope scope;
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
					"assert(#model.outages == 1, 'sandbox did not receive the outage schedule')\n"
					"assert(model.outages[1].installation == model.chillers[2], 'sandbox resolved the outage to the wrong chiller object')\n"
					"assert(model.outages[1].installationId == 'second', 'sandbox lost the named installation id')\n"
					"local original = model.advance\n"
					"function model.advance(self, world_tick, fixed_seconds)\n"
					"local result = original(self, world_tick, fixed_seconds)\n"
					"local first, second = self.chillers[1], self.chillers[2]\n"
					"if world_tick >= 300 and world_tick < 900 then\n"
					"assert(math.abs((second.availability or 1.0) - 0.0) < 1e-12, 'sandbox did not cap the named chiller')\n"
					"assert(math.abs((first.availability or 1.0) - 1.0) < 1e-12, 'sandbox capped the wrong chiller')\n"
					"assert(second.lastCooling == 0, 'sandbox named chiller still cooling during the outage')\n"
					"assert(first.lastCooling > 0, 'sandbox non-target chiller lost service during the outage')\n"
					"else\n"
					"assert(math.abs((first.availability or 1.0) - 1.0) < 1e-12\n"
					"and math.abs((second.availability or 1.0) - 1.0) < 1e-12, 'sandbox availability not restored outside the window')\n"
					"end\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World world("OutageNamedChillerSandbox", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(configureChillers(world, {outage("second_off", "second", 0.0, 5.0, 10.0)}),
				"the sandbox named-chiller outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(20 * 60),
				"bundled Lua named-chiller outage mirror failed its in-sandbox assertions");
			// The host ledger over the same ticks agrees with what the
			// sandbox asserted for the same named chiller.
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.chillers[0].availabilityFactor == 1.0
				&& snap.chillers[0].coolingKW > 0.0
				&& snap.chillers[1].availabilityFactor == 1.0
				&& snap.chillers[1].coolingKW > 0.0,
				"host and sandbox disagree on the restored named chiller after the window");
		}
		// Determinism: two identically-authored worlds with outages agree.
		{
			core::World first("OutageDeterminism", 2, 2);
			first.pauseSimulation(); first.finishBuild();
			configure(first, {outage("out1", "gen", 0.5, 8.0, 6.0, 12.0)});
			first.resumeSimulation();
			first.advanceTicks(40 * 60);
			core::World second("OutageDeterminism", 2, 2);
			second.pauseSimulation(); second.finishBuild();
			configure(second, {outage("out1", "gen", 0.5, 8.0, 6.0, 12.0)});
			second.resumeSimulation();
			second.advanceTicks(40 * 60);
			require(first.getStationSystemsSnapshot().power.generators
				== second.getStationSystemsSnapshot().power.generators,
				"identically-authored outage worlds diverged in generator state");
			require(first.getStationSystemsSnapshot().outages
				== second.getStationSystemsSnapshot().outages,
				"identically-authored outage worlds diverged in outage state");
		}
		// #576: installation ids are unique across the whole shared
		// registered namespace. The outage contract carries only an
		// installationId, never a kind, so a generator, coolant loop and
		// chiller sharing an id could not say which machine the outage
		// caps - resolution would silently pick whichever kind it
		// consults first. Every cross-kind pairing is refused at the
		// crossing, with or without an outage naming the ambiguous id,
		// and the refusal is transactional: a rejected draft leaves the
		// previously authored scenario untouched.
		auto chillerOnto = [](std::string id, std::string coldLoopId,
			std::string condenserId)
		{
			core::ChillerSettings settings;
			settings.id = std::move(id);
			settings.thermalClass = "low";
			settings.coldLoopId = std::move(coldLoopId);
			settings.condenserReservoirId = std::move(condenserId);
			settings.ratedCoolingCapacityKW = 2.0;
			settings.cop = 1.0;
			settings.chilledSetpointC = 5.0;
			settings.maxCondensingTemperatureC = 45.0;
			return settings;
		};
		// One generator, one coolant loop and one chiller; the coldLoopRef
		// is the loop id the chiller references so a renamed loop keeps
		// its internal references consistent and only the namespace
		// collision remains as the reason a variant is refused.
		auto kindsSettings = [&](std::string generatorId, std::string loopId,
			std::string chillerId, std::string coldLoopRef,
			std::vector<core::InstallationOutageSettings> outageSettings)
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {fuelStore()};
			settings.thermalReservoirs = {sinkReservoir(), coldReservoir("cold"),
				coldReservoir("sinklow")};
			core::GeneratorSettings generator = generatorSettings();
			generator.id = std::move(generatorId);
			settings.generators = {std::move(generator)};
			settings.coolantLoops = {chillerLoop(std::move(loopId), "cold")};
			settings.chillers = {chillerOnto(std::move(chillerId),
				std::move(coldLoopRef), "sinklow")};
			core::ProfileSettings sun; sun.constantLevel = 1.0;
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			field.illumination = sun;
			settings.solarArrays = {field};
			settings.outages = std::move(outageSettings);
			return settings;
		};
		// Each cross-kind pairing is refused as ambiguous, whether or not
		// an outage names the colliding id: the namespace itself is the
		// contract, not just the references into it.
		for (auto pairing : {"generator+loop", "generator+chiller", "loop+chiller"})
		{
			std::string diagnostic;
			core::World world("OutageKindCollision", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const settings = std::string(pairing) == "generator+loop"
				? kindsSettings("dup", "dup", "chill", "dup", {})
				: std::string(pairing) == "generator+chiller"
					? kindsSettings("dup", "loop", "dup", "loop", {})
					: kindsSettings("gen", "dup", "dup", "dup", {});
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("ambiguous") != std::string::npos,
				("the " + std::string(pairing)
					+ " installation id collision was not refused as ambiguous").c_str());
			require(!world.getStationSystemsSettings().enabled,
				("the " + std::string(pairing)
					+ " collision draft was partially applied").c_str());
			// The same collision named by an outage is refused the same way:
			// the ambiguity never reaches a silent first-kind resolution.
			auto named = settings;
			named.outages = {outage("off", "dup", 0.0, 5.0, 10.0)};
			require(!world.configureStationSystems(named, false, &diagnostic)
				&& diagnostic.find("ambiguous") != std::string::npos,
				("an outage naming the " + std::string(pairing)
					+ " collision was not refused as ambiguous").c_str());
		}
		// Transactional refusal: a colliding draft over a running
		// scenario leaves the authored settings and the live World
		// exactly as they were.
		{
			std::string diagnostic;
			core::World world("OutageNamespaceTransactional", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(world.configureStationSystems(
				kindsSettings("gen", "loop", "chill", "loop", {}), false, &diagnostic),
				diagnostic.c_str());
			auto const authored = world.getStationSystemsSettings();
			require(!world.configureStationSystems(
				kindsSettings("loop", "loop", "chill", "loop", {}), false, &diagnostic),
				"the colliding draft over an authored scenario was accepted");
			require(world.getStationSystemsSettings() == authored,
				"the refused collision draft mutated the authored Station settings");
			world.resumeSimulation();
			require(world.advanceTicks(60),
				"the World did not keep running the authored scenario after the refused draft");
		}
		// Distinct ids across the three kinds register successfully and
		// each outage caps its own kind and only its own kind; reset and
		// replay reproduce the same per-kind targets tick for tick.
		{
			std::string diagnostic;
			core::World world("OutageDistinctKinds", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(world.configureStationSystems(kindsSettings("gen", "loop",
				"chill", "loop", {
					outage("gen_off", "gen", 0.0, 5.0, 10.0),
					outage("loop_half", "loop", 0.5, 5.0, 10.0),
					outage("chill_off", "chill", 0.0, 5.0, 10.0)}), false, &diagnostic),
				diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(4 * 60), "distinct-kinds run failed before the window");
			auto const before = world.getStationSystemsSnapshot();
			require(approx(before.power.generators[0].availabilityFactor, 1.0)
				&& approx(before.coolantLoops[0].availabilityFactor, 1.0)
				&& approx(before.chillers[0].availabilityFactor, 1.0)
				&& approx(before.power.generators[0].outputKW, 10.0, 1e-2),
				"the distinct-id kinds were not all at full service before the window");
			require(world.advanceTicks(2 * 60), "distinct-kinds run failed into the window");
			auto const during = world.getStationSystemsSnapshot();
			require(during.outages.size() == 3, "the three outages were not all tracked");
			require(during.outages[0].active
				&& during.outages[0].installationKind == "generator"
				&& during.outages[0].installationId == "gen",
				"the generator outage did not report the generator it names");
			require(during.outages[1].active
				&& during.outages[1].installationKind == "coolant loop"
				&& during.outages[1].installationId == "loop",
				"the loop outage did not report the coolant loop it names");
			require(during.outages[2].active
				&& during.outages[2].installationKind == "chiller"
				&& during.outages[2].installationId == "chill",
				"the chiller outage did not report the chiller it names");
			require(approx(during.power.generators[0].availabilityFactor, 0.0)
				&& during.power.generators[0].outputKW == 0.0,
				"the generator outage did not take the generator out of service");
			require(during.coolantLoops[0].availabilityFactor == 0.5,
				"the loop outage did not cap the loop to half availability");
			require(approx(during.chillers[0].availabilityFactor, 0.0)
				&& during.chillers[0].coolingKW == 0.0,
				"the chiller outage did not take the chiller out of service");
			require(world.advanceTicks(14 * 60), "distinct-kinds run failed past the window");
			auto const restored = world.getStationSystemsSnapshot();
			require(!restored.outages[0].active && !restored.outages[1].active
				&& !restored.outages[2].active
				&& approx(restored.power.generators[0].availabilityFactor, 1.0)
				&& approx(restored.coolantLoops[0].availabilityFactor, 1.0)
				&& approx(restored.chillers[0].availabilityFactor, 1.0)
				&& approx(restored.power.generators[0].outputKW, 10.0, 1e-2),
				"the distinct-id kinds did not all return to full service after restoration");
			world.pauseSimulation(); world.resetSimulation();
			auto const fresh = world.getStationSystemsSnapshot();
			require(fresh.outages[0].occurrencesStarted == 0
				&& fresh.outages[1].occurrencesStarted == 0
				&& fresh.outages[2].occurrencesStarted == 0
				&& approx(fresh.power.generators[0].availabilityFactor, 1.0)
				&& approx(fresh.coolantLoops[0].availabilityFactor, 1.0)
				&& approx(fresh.chillers[0].availabilityFactor, 1.0),
				"reset did not restore full declared availability on all three kinds");
			world.resumeSimulation();
			require(world.advanceTicks(6 * 60), "distinct-kinds reset replay failed");
			auto const replay = world.getStationSystemsSnapshot();
			require(replay.power.generators == during.power.generators
				&& replay.coolantLoops == during.coolantLoops
				&& replay.chillers == during.chillers
				&& replay.outages == during.outages,
				"the reset replay did not hit the same distinct-id kinds");
		}
		// File seam: a saved document whose loop has been renamed onto the
		// generator's id is refused at load, not silently resolved.
		{
			namespace filesystem = std::filesystem;
			auto const replaceAll = [](std::string text, std::string const& from,
				std::string const& to)
			{
				for (size_t pos = text.find(from); pos != std::string::npos;
					pos = text.find(from, pos + to.size()))
					text.replace(pos, from.size(), to);
				return text;
			};
			auto const directory = context.temporaryRoot() / "station-outage-namespace";
			filesystem::create_directories(directory);
			std::string diagnostic;
			core::World original("OutageCollisionFile", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			require(original.configureStationSystems(
				kindsSettings("gen", "coolloop", "chill", "coolloop", {}), false, &diagnostic),
				diagnostic.c_str());
			original.resumeSimulation();
			require(original.advanceTicks(60), "collision-file original run failed");
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData data;
			original.serialize(*writer, data);
			writer->serialize();
			auto const tampered = replaceAll(writer->getSerializedString(), "coolloop", "gen");
			auto const tamperedPath = directory / "ambiguous-installation.world.yaml";
			std::ofstream out(tamperedPath, std::ios::binary);
			out << tampered;
			out.close();
			bool refused = false;
			std::string loadError;
			try { (void)core::loadWorldDocument(tamperedPath); }
			catch (std::exception const& error) { refused = true; loadError = error.what(); }
			require(refused && loadError.find("ambiguous") != std::string::npos,
				"a document carrying a cross-kind installation id collision loaded silently");
		}
		// Host/Lua seam: the sandbox mirror refuses an installation id
		// registered by two kinds instead of silently keeping whichever
		// it registered last, and with distinct ids it resolves each
		// outage to the object of the kind it names, capping the same
		// availability the host ledger shows.
		{
			SourceScope scope;
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
					"local tamperedGenerator = {}\n"
					"for k, v in pairs(generators[1]) do tamperedGenerator[k] = v end\n"
					"tamperedGenerator.id = coolantLoops[1].id\n"
					"local collided = { tamperedGenerator }\n"
					"local ok, err = pcall(factory, name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, collided, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
					"assert(not ok, 'sandbox accepted a generator sharing the coolant loop id')\n"
					"assert(type(err) == 'string' and string.find(err, 'ambiguous', 1, true) ~= nil, 'sandbox rejected the collision for the wrong reason: ' .. tostring(err))\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
					"assert(#model.outages == 3, 'sandbox did not receive the three outage schedules')\n"
					"assert(model.outages[1].installation == model.generators[1], 'sandbox resolved the generator outage to the wrong object')\n"
					"assert(model.outages[2].installation == model.coolant_loops[1], 'sandbox resolved the loop outage to the wrong object')\n"
					"assert(model.outages[3].installation == model.chillers[1], 'sandbox resolved the chiller outage to the wrong object')\n"
					"local original = model.advance\n"
					"function model.advance(self, world_tick, fixed_seconds)\n"
					"local result = original(self, world_tick, fixed_seconds)\n"
					"local gen, loop, chiller = self.generators[1], self.coolant_loops[1], self.chillers[1]\n"
					"if world_tick >= 300 and world_tick < 900 then\n"
					"assert(math.abs((gen.availability or 1.0) - 0.0) < 1e-12, 'sandbox did not cap the generator')\n"
					"assert(math.abs((loop.availability or 1.0) - 0.5) < 1e-12, 'sandbox did not cap the loop to half')\n"
					"assert(math.abs((chiller.availability or 1.0) - 0.0) < 1e-12, 'sandbox did not cap the chiller')\n"
					"else\n"
					"assert(math.abs((gen.availability or 1.0) - 1.0) < 1e-12\n"
					"and math.abs((loop.availability or 1.0) - 1.0) < 1e-12\n"
					"and math.abs((chiller.availability or 1.0) - 1.0) < 1e-12, 'sandbox availability not restored outside the window')\n"
					"end\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			std::string diagnostic;
			core::World world("OutageDistinctKindsSandbox", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(world.configureStationSystems(kindsSettings("gen", "loop",
				"chill", "loop", {
					outage("gen_off", "gen", 0.0, 5.0, 10.0),
					outage("loop_half", "loop", 0.5, 5.0, 10.0),
					outage("chill_off", "chill", 0.0, 5.0, 10.0)}), false, &diagnostic),
				"the sandbox distinct-kinds outage scenario was rejected");
			world.resumeSimulation();
			require(world.advanceTicks(20 * 60),
				"bundled Lua distinct-kinds outage mirror failed its in-sandbox assertions");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.power.generators[0].availabilityFactor, 1.0)
				&& approx(snap.coolantLoops[0].availabilityFactor, 1.0)
				&& approx(snap.chillers[0].availabilityFactor, 1.0),
				"the host ledger disagrees with what the sandbox asserted after restoration");
		}
	}

// #414: resource-constrained maintenance. A scheduled timed job on a
// registered installation completes only by paying with declared tools,
// spare parts and consumables: reusable tools are reserved and returned,
// consumed parts leave their source and land in the declared waste
// store, the installation is isolated while the work runs, and absent
// prerequisites never let the job complete for free. Reservation is
// exclusive in the declared deterministic order, so overlapping work
// cannot double-spend a unit of material; a reservation that cannot be
// held interrupts the job with its progress discarded. Everything is
// exercised through the World tick / snapshot / event seam with no
// Agent assignments.
void maintenance(smoke::Context const&)
{
	auto const approx = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	auto store = [](std::string id, std::string unit, double capacity, double qualified)
	{
		core::ResourceStoreSettings settings;
		settings.id = std::move(id);
		settings.unit = std::move(unit);
		settings.capacity = capacity;
		settings.startingQualified = qualified;
		return settings;
	};
	auto sinkReservoir = []
	{
		core::ThermalReservoirSettings settings;
		settings.id = "hot";
		settings.thermalClass = "generation";
		settings.heatCapacityKJPerK = 100000.0;
		settings.startingTemperatureC = 20.0;
		settings.minTemperatureC = -50.0;
		settings.maxTemperatureC = 200.0;
		return settings;
	};
	auto generator = [](std::string id)
	{
		core::GeneratorSettings settings;
		settings.id = std::move(id);
		settings.fuelStoreId = "diesel";
		settings.thermalClass = "generation";
		settings.heatSinkId = "hot";
		settings.ratedOutputKW = 10.0;
		settings.efficiency = 0.5;
		settings.maxRampKWPerSecond = 100.0;
		settings.commandedOutputKW = 10.0;
		settings.startupPowerKW = 1.0;
		settings.startupSeconds = 1.0;
		settings.shutdownHeatKW = 0.0;
		settings.shutdownSeconds = 0.0;
		settings.maxSinkTemperatureC = 90.0;
		return settings;
	};
	auto requirement = [](std::string storeId, double quantity, bool consumed)
	{
		core::MaintenanceRequirementSettings settings;
		settings.storeId = std::move(storeId);
		settings.quantity = quantity;
		settings.consumed = consumed;
		return settings;
	};
	auto job = [](std::string id, std::string installation, double first,
		double duration, std::vector<core::MaintenanceRequirementSettings> requirements,
		std::string wasteStoreId = {}, double recurrence = 0.0, int priority = 0)
	{
		core::MaintenanceJobSettings settings;
		settings.id = std::move(id);
		settings.installationId = std::move(installation);
		settings.firstOccurrenceSeconds = first;
		settings.durationSeconds = duration;
		settings.recurrenceSeconds = recurrence;
		settings.priority = priority;
		settings.requirements = std::move(requirements);
		settings.wasteStoreId = std::move(wasteStoreId);
		return settings;
	};
	// The shared scenario: a fuelled generator, a reusable tool store,
	// a spare-parts store and a waste store, all in kg. The generator
	// reaches full rated output long before any maintenance window, so
	// the isolation - not startup or the ramp - is what moves its
	// output when a job runs.
	auto baseSettings = [&]
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.stores = {
			store("diesel", "kWh", 100000.0, 100000.0),
			store("tools", "kg", 100.0, 10.0),
			store("parts", "kg", 100.0, 8.0),
			store("waste", "kg", 10.0, 0.0)};
		settings.thermalReservoirs = {sinkReservoir()};
		settings.generators = {generator("gen")};
		core::ProfileSettings sun; sun.constantLevel = 1.0;
		core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
		field.illumination = sun;
		settings.solarArrays = {field};
		return settings;
	};
	auto configure = [&](core::World& world, core::StationSystemsSettings settings)
	{
		std::string diagnostic;
		auto const ok = world.configureStationSystems(settings, false, &diagnostic);
		if (!ok) return false;
		world.resumeSimulation();
		return true;
	};
	// Refused authored settings: the prerequisite contract is validated
	// at the crossing, never discovered mid-tick.
	for (auto mutation : {"unknown-installation", "unknown-requirement-store",
		"empty-requirements", "consume-without-waste", "waste-without-consume",
		"waste-unit-mismatch", "waste-own-source", "unknown-waste-store",
		"duplicate-requirement-store", "zero-duration", "zero-quantity",
		"recurrence-below-duration", "duplicate-id", "empty-id"})
	{
		auto settings = baseSettings();
		settings.maintenanceJobs = {job("maint", "gen", 10.0, 5.0,
			{requirement("tools", 2.0, false), requirement("parts", 1.0, true)},
			"waste")};
		if (std::string_view(mutation) == "unknown-installation")
			settings.maintenanceJobs[0].installationId = "ghost";
		else if (std::string_view(mutation) == "unknown-requirement-store")
			settings.maintenanceJobs[0].requirements[0].storeId = "nope";
		else if (std::string_view(mutation) == "empty-requirements")
			settings.maintenanceJobs[0].requirements.clear();
		else if (std::string_view(mutation) == "consume-without-waste")
			settings.maintenanceJobs[0].wasteStoreId.clear();
		else if (std::string_view(mutation) == "waste-without-consume")
			settings.maintenanceJobs[0].requirements = {requirement("tools", 2.0, false)};
		else if (std::string_view(mutation) == "waste-unit-mismatch")
			settings.stores[3] = store("waste", "L", 10.0, 0.0);
		else if (std::string_view(mutation) == "waste-own-source")
			settings.maintenanceJobs[0].requirements[1].storeId = "waste";
		else if (std::string_view(mutation) == "unknown-waste-store")
			settings.maintenanceJobs[0].wasteStoreId = "nope";
		else if (std::string_view(mutation) == "duplicate-requirement-store")
			settings.maintenanceJobs[0].requirements.push_back(requirement("tools", 1.0, false));
		else if (std::string_view(mutation) == "zero-duration")
			settings.maintenanceJobs[0].durationSeconds = 0.0;
		else if (std::string_view(mutation) == "zero-quantity")
			settings.maintenanceJobs[0].requirements[0].quantity = 0.0;
		else if (std::string_view(mutation) == "recurrence-below-duration")
			settings.maintenanceJobs[0].recurrenceSeconds = 2.0;
		else if (std::string_view(mutation) == "duplicate-id")
			settings.maintenanceJobs.push_back(job("maint", "gen", 20.0, 5.0,
				{requirement("parts", 1.0, true)}, "waste"));
		else if (std::string_view(mutation) == "empty-id")
			settings.maintenanceJobs[0].id = "";
		core::World world("MaintenanceRefusal", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(!world.configureStationSystems(settings, false, &diagnostic),
			std::string("an invalid maintenance job (").append(mutation).append(") was accepted").c_str());
	}
	// Nominal one-off maintenance: the job reserves its tool and part at
	// the window, isolates the generator for its duration, then spends
	// the part into the waste store, returns the tool and restores the
	// machine - without replenishing anything.
	{
		auto settings = baseSettings();
		settings.maintenanceJobs = {job("maint", "gen", 20.0, 10.0,
			{requirement("tools", 5.0, false), requirement("parts", 2.0, true)},
			"waste")};
		core::World world("MaintenanceNominal", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, settings), "nominal maintenance scenario was rejected");
		// t = 12s: idle, generator at full rated output.
		require(world.advanceTicks(12 * 60), "nominal run failed before the job");
		auto const& before = world.getStationSystemsSnapshot();
		require(before.maintenanceJobs.size() == 1
			&& before.maintenanceJobs[0].phase == core::StationMaintenancePhase::Idle
			&& before.maintenanceJobs[0].occurrencesStarted == 0,
			"the maintenance job was busy before its window");
		require(approx(before.power.generators[0].availabilityFactor, 1.0)
			&& approx(before.power.generators[0].outputKW, 10.0, 1e-2),
			"the generator was not at full output before maintenance");
		// t = 25s: work in progress - the generator is isolated, the
		// tool and part are reserved (still in their stores, but held).
		if (!world.advanceTicks(13 * 60))
		{
			auto const& diags = world.getStationSystemsDiagnostics();
			std::string msg = diags.empty() ? std::string("no diagnostic") : diags.back().message;
			require(false, ("nominal run failed into the job: " + msg).c_str());
		}
		auto const& during = world.getStationSystemsSnapshot();
		require(during.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& during.maintenanceJobs[0].isolated
			&& during.maintenanceJobs[0].occurrencesStarted == 1,
			("the maintenance job did not start and isolate as authored: phase="
				+ std::to_string((int)during.maintenanceJobs[0].phase)
				+ " isolated=" + std::to_string((int)during.maintenanceJobs[0].isolated)
				+ " started=" + std::to_string(during.maintenanceJobs[0].occurrencesStarted)
				+ " reason=" + during.maintenanceJobs[0].blockReason).c_str());
		require(approx(during.maintenanceJobs[0].progressSeconds, 5.0, 0.05)
			&& approx(during.maintenanceJobs[0].remainingSeconds, 5.0, 0.05),
			"maintenance progress is not tracking the fixed tick");
		require(approx(during.power.generators[0].availabilityFactor, 0.0)
			&& approx(during.power.generators[0].outputKW, 0.0, 1e-3),
			"the maintained generator was not isolated");
		require(during.maintenanceJobs[0].requirements[0].reserved
			&& during.maintenanceJobs[0].requirements[1].reserved,
			"the live reservation is not visible on the requirements");
		require(approx(during.stores[1].currentQualified, 10.0)
			&& approx(during.stores[2].currentQualified, 8.0),
			"reserved material left its store before completion");
		require(sawEvent(world, "started at") && sawEvent(world, "installation isolated"),
			"the maintenance start was not announced");
		// t = 31s: completed - the part is spent into the waste store,
		// the tool is returned, the generator runs again, and nothing
		// was replenished.
		require(world.advanceTicks(11 * 60), "nominal run failed past completion");
		auto const& after = world.getStationSystemsSnapshot();
		require(after.maintenanceJobs[0].phase == core::StationMaintenancePhase::Completed
			&& !after.maintenanceJobs[0].isolated
			&& after.maintenanceJobs[0].occurrencesCompleted == 1
			&& after.maintenanceJobs[0].exhausted,
			"the one-off maintenance did not complete and exhaust as authored");
		require(approx(after.power.generators[0].availabilityFactor, 1.0)
			&& approx(after.power.generators[0].outputKW, 10.0, 1e-2),
			"the generator was not restored after maintenance");
		require(approx(after.stores[1].currentQualified, 10.0),
			"the reusable tool store changed - tools must be returned, not consumed");
		require(approx(after.stores[2].currentQualified, 6.0),
			"the consumed part was not deducted from its store");
		require(approx(after.stores[3].currentQualified, 2.0),
			"the used material did not land in the waste store");
		require(after.maintenanceJobs[0].requirements[0].lastConsumed == 0.0
			&& approx(after.maintenanceJobs[0].requirements[1].lastConsumed, 2.0),
			"the per-requirement spend is wrong (tool must show zero consumed)");
		require(sawEvent(world, "completed at")
			&& sawEvent(world, "reusable tools returned")
			&& sawEvent(world, "no supplies replenished"),
			"the maintenance completion was not announced with its accounting");
		// Conservation: the kg boundary closes - the part moved from
		// parts to waste, nothing was created or deleted.
		for (auto const& balance : after.balances)
			if (balance.unit == "kg")
				require(approx(balance.residual, 0.0, 1e-6),
					"the maintenance kg boundary did not close");
	}
	// Missing prerequisites prevent free completion: with no parts in
	// stock the job stays Blocked naming the binding store, the
	// generator keeps running (no work, no isolation), and the window
	// closes as a missed occurrence.
	{
		auto settings = baseSettings();
		settings.stores[2] = store("parts", "kg", 100.0, 0.0);
		settings.maintenanceJobs = {job("maint", "gen", 10.0, 5.0,
			{requirement("tools", 2.0, false), requirement("parts", 1.0, true)},
			"waste")};
		core::World world("MaintenanceMissingParts", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, settings), "missing-parts scenario was rejected");
		require(world.advanceTicks(12 * 60), "missing-parts run failed");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Blocked
			&& snap.maintenanceJobs[0].blockReason.find("parts") != std::string::npos
			&& snap.maintenanceJobs[0].occurrencesStarted == 0,
			"the job did not block on the missing part store");
		require(!snap.maintenanceJobs[0].isolated
			&& approx(snap.power.generators[0].outputKW, 10.0, 1e-2),
			"a blocked job isolated the installation without working");
		require(sawEvent(world, "blocked at")
			&& sawEvent(world, "part/consumable store 'parts' cannot cover"),
			"the blocking reason was not announced");
		// t = 16s: the window closed with the work never started.
		require(world.advanceTicks(4 * 60), "missing-parts run failed past the window");
		auto const& missed = world.getStationSystemsSnapshot();
		require(missed.maintenanceJobs[0].occurrencesMissed == 1
			&& missed.maintenanceJobs[0].occurrencesCompleted == 0
			&& missed.maintenanceJobs[0].exhausted,
			"the unstarted window was not reported as missed and exhausted");
		require(approx(missed.stores[3].currentQualified, 0.0),
			"a missed job booked waste it never spent");
		require(sawEvent(world, "missed its window"),
			"the missed occurrence was not announced");
	}
	// Interruption: a transfer drains the reserved part store below the
	// reservation mid-job. The job is interrupted with its progress
	// discarded and nothing spent; a delivery refills the store and the
	// job re-reserves and runs the full duration from the restart.
	{
		auto settings = baseSettings();
		settings.stores[2] = store("parts", "kg", 100.0, 10.0);
		settings.stores.push_back(store("bilge", "kg", 1000.0, 0.0));
		core::ResourceTransferSettings drain;
		drain.id = "drain";
		drain.sourceId = "parts";
		drain.destinationId = "bilge";
		drain.unit = "kg";
		drain.ratePerSecond = 0.5;
		drain.priority = 0;
		settings.transfers = {drain};
		settings.maintenanceJobs = {job("maint", "gen", 10.0, 10.0,
			{requirement("tools", 2.0, false), requirement("parts", 2.0, true)},
			"waste")};
		core::ResourceDeliverySettings refill;
		refill.id = "refill";
		refill.storeId = "parts";
		refill.unit = "kg";
		refill.quantity = 20.0;
		refill.firstOccurrenceSeconds = 19.0;
		refill.recurrenceSeconds = 0.0;
		settings.deliveries = {refill};
		core::World world("MaintenanceInterrupt", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, settings), "interruption scenario was rejected");
		// t = 11s: the job started at 10s with 5 kg drawable; the
		// drain has been pulling 0.5 kg/s since the run began.
		require(world.advanceTicks(11 * 60), "interruption run failed into the job");
		require(world.getStationSystemsSnapshot().maintenanceJobs[0].phase
			== core::StationMaintenancePhase::Active,
			"the job did not start before the drain reached its reservation");
		// t = 17s: parts = 10 - 8.5 = 1.5 < 2 reserved -> interrupted.
		require(world.advanceTicks(6 * 60), "interruption run failed into the interrupt");
		auto const& cut = world.getStationSystemsSnapshot();
		require(cut.maintenanceJobs[0].phase == core::StationMaintenancePhase::Blocked,
			"the lost reservation did not block the job");
		require(approx(cut.maintenanceJobs[0].progressSeconds, 0.0),
			"the interrupted job kept progress it did not finish");
		require(approx(cut.stores[3].currentQualified, 0.0),
			"the interrupted job spent material anyway");
		require(sawEvent(world, "interrupted at")
			&& sawEvent(world, "progress discarded"),
			"the interruption was not announced with its discarded progress");
		// t = 20s: the refill landed at 19s; the job re-reserves and
		// is running again inside its window.
		require(world.advanceTicks(3 * 60), "interruption run failed into the restart");
		auto const& restarted = world.getStationSystemsSnapshot();
		require(restarted.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& restarted.maintenanceJobs[0].occurrencesStarted == 2,
			"the job did not restart after the refill");
		require(approx(restarted.maintenanceJobs[0].progressSeconds, 1.0, 0.05),
			"the restarted job did not begin the full duration afresh");
		// t = 29s: the full duration ran from the restart; the spend
		// lands once, into waste, and the tool comes back.
		require(world.advanceTicks(10 * 60), "interruption run failed past completion");
		auto const& done = world.getStationSystemsSnapshot();
		require(done.maintenanceJobs[0].phase == core::StationMaintenancePhase::Completed
			&& done.maintenanceJobs[0].occurrencesCompleted == 1
			&& done.maintenanceJobs[0].occurrencesMissed == 0,
			"the restarted job did not complete exactly once");
		require(approx(done.stores[3].currentQualified, 2.0),
			"the restarted job's spend did not land in the waste store");
		require(approx(done.stores[1].currentQualified, 10.0),
			"the reusable tool was not returned after the restart");
	}
	// Simultaneous jobs cannot double-spend a shared tool: the
	// higher-priority job holds the only copy, the lower one blocks
	// naming the tool store, and starts the moment the tool is returned.
	{
		auto settings = baseSettings();
		settings.generators.push_back(generator("gen2"));
		settings.stores[1] = store("tools", "kg", 100.0, 4.0);
		settings.stores.push_back(store("partsB", "kg", 100.0, 5.0));
		settings.maintenanceJobs = {
			job("high", "gen", 10.0, 10.0,
				{requirement("tools", 4.0, false), requirement("parts", 1.0, true)},
				"waste", 0.0, 10),
			job("low", "gen2", 10.0, 15.0,
				{requirement("tools", 4.0, false), requirement("partsB", 1.0, true)},
				"waste", 0.0, 1)};
		core::World world("MaintenanceContention", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, settings), "contention scenario was rejected");
		// t = 12s: the higher-priority job holds the tools; the lower
		// one is blocked naming the tool store, and gen2 keeps running.
		require(world.advanceTicks(12 * 60), "contention run failed into contention");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& snap.maintenanceJobs[1].phase == core::StationMaintenancePhase::Blocked,
			"the priority order did not decide who holds the shared tool");
		require(snap.maintenanceJobs[1].blockReason.find("tools") != std::string::npos,
			"the blocked job did not name the contended tool store");
		require(!snap.maintenanceJobs[1].isolated
			&& approx(snap.power.generators[1].outputKW, 10.0, 1e-2),
			"a blocked job isolated the second installation");
		// t = 24s: the high job completed at 20s and returned the
		// tool; the low job holds it now and gen is back on line
		// (given its restart crank and ramp room).
		require(world.advanceTicks(12 * 60), "contention run failed past the handoff");
		auto const& handed = world.getStationSystemsSnapshot();
		require(handed.maintenanceJobs[0].occurrencesCompleted == 1
			&& handed.maintenanceJobs[1].phase == core::StationMaintenancePhase::Active
			&& handed.maintenanceJobs[1].isolated,
			"the returned tool did not pass to the waiting job");
		require(approx(handed.power.generators[0].outputKW, 10.0, 1e-2)
			&& approx(handed.power.generators[1].outputKW, 0.0, 1e-3),
			"the isolation did not move with the work");
		// t = 36s: the low job completes too; total spend is exactly
		// the two authored kilograms, both in the waste store.
		require(world.advanceTicks(12 * 60), "contention run failed past the second completion");
		auto const& done = world.getStationSystemsSnapshot();
		require(done.maintenanceJobs[1].occurrencesCompleted == 1
			&& done.maintenanceJobs[1].phase == core::StationMaintenancePhase::Completed,
			"the second job did not complete");
		require(approx(done.stores[3].currentQualified, 2.0),
			"the two jobs together spent something other than their authored 2 kg");
		require(approx(done.stores[1].currentQualified, 4.0),
			"the shared tool was consumed instead of returned");
	}
	// #586: two jobs with independent 1 kg part stores both reserve
	// into the same empty 1 kg waste store. Only the higher-precedence
	// job that fits the remaining waste headroom starts: the lower one
	// is blocked at the start check naming the waste store, keeps its
	// installation online, and never burns progress or off-service
	// cycles against headroom it can never commit into. When the waste
	// store frees up while its window is still open, the job commits.
	{
		auto wasteHeadroomSettings = [&]
		{
			auto settings = baseSettings();
			settings.generators.push_back(generator("gen2"));
			settings.stores[2] = store("parts", "kg", 1.0, 1.0);
			settings.stores[3] = store("waste", "kg", 1.0, 0.0);
			settings.stores.push_back(store("partsB", "kg", 1.0, 1.0));
			settings.maintenanceJobs = {
				job("high", "gen", 10.0, 10.0,
					{requirement("parts", 1.0, true)}, "waste", 0.0, 10),
				job("low", "gen2", 10.0, 20.0,
					{requirement("partsB", 1.0, true)}, "waste", 0.0, 1)};
			core::WasteEjectionSettings clear;
			clear.id = "clear";
			clear.storeId = "waste";
			clear.unit = "kg";
			clear.quantity = 1.0;
			clear.firstOccurrenceSeconds = 25.0;
			settings.wasteEjections = {clear};
			return settings;
		};
		core::World world("MaintenanceWasteHeadroom", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, wasteHeadroomSettings()),
			"waste headroom contention scenario was rejected");
		// t = 12s: 'high' holds the only 1 kg of waste headroom.
		// 'low' is blocked at the start check naming the waste store,
		// never isolated, and gen2 stays at full output.
		require(world.advanceTicks(12 * 60), "waste headroom run failed into contention");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& snap.maintenanceJobs[0].isolated,
			"the higher-precedence job did not start into the shared waste headroom");
		require(snap.maintenanceJobs[1].phase == core::StationMaintenancePhase::Blocked
			&& !snap.maintenanceJobs[1].isolated
			&& snap.maintenanceJobs[1].occurrencesStarted == 0,
			"the lower-precedence job started against waste headroom it could not commit");
		require(snap.maintenanceJobs[1].blockReason.find("waste") != std::string::npos,
			"the blocked job did not name the contended waste store");
		require(approx(snap.power.generators[1].outputKW, 10.0, 1e-2),
			"the blocked job took the second installation off service");
		// t = 24s: 'high' completed at 20s and filled the waste
		// store; 'low' never started, was never interrupted, and gen2
		// never left service.
		require(world.advanceTicks(12 * 60),
			"waste headroom run failed past the first completion");
		auto const& full = world.getStationSystemsSnapshot();
		require(full.maintenanceJobs[0].occurrencesCompleted == 1
			&& approx(full.stores[3].currentQualified, 1.0),
			"the higher-precedence spend did not fill the shared waste store");
		require(full.maintenanceJobs[1].occurrencesStarted == 0
			&& full.maintenanceJobs[1].phase == core::StationMaintenancePhase::Blocked
			&& !full.maintenanceJobs[1].isolated,
			"the lower-precedence job cycled into service against a full waste store");
		for (auto const& event : world.getStationSystemsEvents())
			require(event.message.find("Maintenance 'low'") == std::string::npos
				|| event.message.find("interrupted") == std::string::npos,
				"the lower-precedence job was interrupted instead of never started");
		// t = 27s: the ejection at 25s cleared the waste store;
		// 'low' is still inside its [10, 30] window, so it commits
		// now and gen2 goes off service for the real work.
		require(world.advanceTicks(3 * 60), "waste headroom run failed into the late start");
		auto const& late = world.getStationSystemsSnapshot();
		require(late.maintenanceJobs[1].phase == core::StationMaintenancePhase::Active
			&& late.maintenanceJobs[1].isolated
			&& late.maintenanceJobs[1].occurrencesStarted == 1,
			"the waiting job did not commit once the waste headroom freed up");
		// t = 48s: 'low' completes its full 20 s duration and books
		// its kilogram back into the cleared waste store.
		require(world.advanceTicks(21 * 60),
			"waste headroom run failed past the late completion");
		auto const& done = world.getStationSystemsSnapshot();
		require(done.maintenanceJobs[1].phase == core::StationMaintenancePhase::Completed
			&& done.maintenanceJobs[1].occurrencesCompleted == 1
			&& done.maintenanceJobs[1].occurrencesMissed == 0
			&& approx(done.stores[3].currentQualified, 1.0)
			&& approx(done.stores[4].currentQualified, 0.0),
			"the late-started job did not complete its full duration as authored");
		// Determinism (#586): the same waste-headroom contention
		// replayed in a fresh World reproduces the maintenance
		// snapshots and the event history exactly.
		core::World replay("MaintenanceWasteHeadroomReplay", 2, 2);
		replay.pauseSimulation(); replay.finishBuild();
		require(configure(replay, wasteHeadroomSettings()),
			"waste headroom replay scenario was rejected");
		require(replay.advanceTicks(48 * 60), "waste headroom replay run failed");
		require(world.getStationSystemsSnapshot().maintenanceJobs
			== replay.getStationSystemsSnapshot().maintenanceJobs,
			"the waste headroom contention replay diverged");
		auto maintenanceEvents = [](core::World& w)
		{
			std::vector<std::string> messages;
			for (auto const& event : w.getStationSystemsEvents())
				if (event.message.rfind("Maintenance '", 0) == 0)
					messages.push_back(event.message);
			return messages;
		};
		require(maintenanceEvents(world) == maintenanceEvents(replay),
			"the waste headroom event history is not reproducible");
		require(!maintenanceEvents(world).empty(),
			"the waste headroom scenario produced no maintenance events at all");
	}
	// Determinism: the same authored scenario replayed from Reset
	// reproduces the snapshot and the maintenance event history exactly.
	{
		auto settings = baseSettings();
		settings.stores[2] = store("parts", "kg", 100.0, 10.0);
		settings.stores.push_back(store("bilge", "kg", 1000.0, 0.0));
		core::ResourceTransferSettings drain;
		drain.id = "drain";
		drain.sourceId = "parts";
		drain.destinationId = "bilge";
		drain.unit = "kg";
		drain.ratePerSecond = 0.5;
		settings.transfers = {drain};
		settings.maintenanceJobs = {job("maint", "gen", 10.0, 10.0,
			{requirement("tools", 2.0, false), requirement("parts", 2.0, true)},
			"waste", 20.0, 3)};
		core::World first("MaintenanceReplayA", 2, 2);
		first.pauseSimulation(); first.finishBuild();
		require(configure(first, settings), "replay scenario A was rejected");
		require(first.advanceTicks(40 * 60), "replay run A failed");
		core::World second("MaintenanceReplayB", 2, 2);
		second.pauseSimulation(); second.finishBuild();
		require(configure(second, settings), "replay scenario B was rejected");
		require(second.advanceTicks(40 * 60), "replay run B failed");
		require(first.getStationSystemsSnapshot().maintenanceJobs
			== second.getStationSystemsSnapshot().maintenanceJobs,
			"the recurring maintenance replay diverged");
		auto maintenanceEvents = [](core::World& world)
		{
			std::vector<std::string> messages;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.rfind("Maintenance '", 0) == 0)
					messages.push_back(event.message);
			return messages;
		};
		require(maintenanceEvents(first) == maintenanceEvents(second),
			"the maintenance event history is not reproducible");
		require(!maintenanceEvents(first).empty(),
			"the recurring scenario produced no maintenance events at all");
	}
	// Host/Lua seam: the sandbox mirror receives the maintenance
	// descriptors, resolves the target installation to the generator
	// object it names, and its own reservation ledger spends exactly
	// what the host ledger spends - the sandbox asserts its store
	// movement in place, so a corrupt crossing or model drift fails
	// contained like any other script failure.
	{
		SourceScope scope;
		auto const bundled = core::bundledStationSystemsSource();
		core::setStationSystemsSourceLoader([bundled] {
			return std::string("local factory = (function()\n") + bundled + "\nend)()\n"
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode, maintenance)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures, outages, dispatchMode, maintenance)\n"
				"assert(#model.maintenance == 1, 'sandbox did not receive the maintenance schedule')\n"
				"local job = model.maintenance[1]\n"
				"assert(job.installation == model.generators[1], 'sandbox resolved the maintenance target to the wrong object')\n"
				"assert(#job.requirements == 2, 'sandbox lost a maintenance requirement')\n"
				"assert(job.requirements[1].consumed == false, 'sandbox flipped the reusable tool to consumed')\n"
				"assert(job.requirements[2].consumed == true, 'sandbox flipped the consumed part to reusable')\n"
				"local original = model.advance\n"
				"function model.advance(self, world_tick, fixed_seconds)\n"
				"local result = original(self, world_tick, fixed_seconds)\n"
				"local elapsed = world_tick * fixed_seconds\n"
				"if elapsed >= 30.0 then\n"
				"assert(math.abs(self.stores[3].qualified - 6.0) < 1e-9, 'sandbox parts spend disagrees with the authored 2 kg')\n"
				"assert(math.abs(self.stores[4].qualified - 2.0) < 1e-9, 'sandbox waste booking disagrees with the spent material')\n"
				"assert(math.abs(self.stores[2].qualified - 10.0) < 1e-9, 'sandbox consumed the reusable tool store')\n"
				"assert(self.maintenance[1].completed == 1, 'sandbox maintenance did not complete once')\n"
				"end\n"
				"return result\n"
				"end\n"
				"return model\n"
				"end";
		});
		auto settings = baseSettings();
		settings.maintenanceJobs = {job("maint", "gen", 20.0, 10.0,
			{requirement("tools", 5.0, false), requirement("parts", 2.0, true)},
			"waste")};
		core::World world("MaintenanceSandboxCrossing", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, settings), "sandbox crossing scenario was rejected");
		require(world.advanceTicks(35 * 60),
			"bundled Lua maintenance mirror failed its in-sandbox assertions");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy
			&& approx(snap.stores[2].currentQualified, 6.0)
			&& approx(snap.stores[3].currentQualified, 2.0),
			"the host ledger disagrees with what the sandbox asserted");
	}
	// #767: event splitting reaches a sub-tick window at its exact start
	// and completion. It is no longer missed merely because no full
	// Station step ends inside it.
	{
		// The probe shape: a one-off with first occurrence 0.001 s
		// and duration 0.001 s. Its whole window [0.001, 0.002] has
		// contained in the first fixed tick, with every prerequisite
		// in stock. The split must service it exactly once.
		auto expiredSettings = [&]
		{
			auto settings = baseSettings();
			settings.maintenanceJobs = {job("expired", "gen", 0.001, 0.001,
				{requirement("tools", 2.0, false), requirement("parts", 1.0, true)},
				"waste")};
			return settings;
		};
		core::World world("MaintenanceExpiredWindow", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, expiredSettings()),
			"expired-window scenario was rejected");
		require(world.advanceTicks(1), "expired-window first tick failed");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the expired-window run was not healthy");
		require(snap.maintenanceJobs[0].occurrencesStarted == 1
			&& snap.maintenanceJobs[0].occurrencesCompleted == 1
			&& snap.maintenanceJobs[0].occurrencesMissed == 0
			&& snap.maintenanceJobs[0].exhausted
			&& snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Completed
			&& !snap.maintenanceJobs[0].isolated
			&& approx(snap.maintenanceJobs[0].lastStartSeconds, 0.001)
			&& approx(snap.maintenanceJobs[0].lastCompleteSeconds, 0.002),
			"the sub-tick one-off did not run at its exact declared times");
		require(approx(snap.stores[2].currentQualified, 7.0)
			&& approx(snap.stores[3].currentQualified, 1.0),
			"the sub-tick job did not conserve consumed material");
		require(sawEvent(world, "completed at 0.002000"),
			"the sub-tick completion was not announced");
		// The completed one-off never wakes up again.
		require(world.advanceTicks(60), "expired-window run failed past the first second");
		auto const& later = world.getStationSystemsSnapshot();
		require(later.maintenanceJobs[0].occurrencesStarted == 1
			&& later.maintenanceJobs[0].occurrencesCompleted == 1
			&& later.maintenanceJobs[0].occurrencesMissed == 0
			&& !later.maintenanceJobs[0].isolated
			&& approx(later.power.generators[0].availabilityFactor, 1.0),
			"a closed one-off window was replayed as startable later");
		// Determinism: a fresh World with the same settings reproduces
		// the completed counters and the maintenance event history.
		core::World replay("MaintenanceExpiredWindowReplay", 2, 2);
		replay.pauseSimulation(); replay.finishBuild();
		require(configure(replay, expiredSettings()),
			"expired-window replay scenario was rejected");
		require(replay.advanceTicks(61), "expired-window replay run failed");
		require(replay.getStationSystemsSnapshot().maintenanceJobs
			== world.getStationSystemsSnapshot().maintenanceJobs,
			"the expired-window replay diverged from the original run");
		auto maintenanceEvents = [](core::World& w)
		{
			std::vector<std::string> messages;
			for (auto const& event : w.getStationSystemsEvents())
				if (event.message.rfind("Maintenance '", 0) == 0)
					messages.push_back(event.message);
			return messages;
		};
		require(maintenanceEvents(world) == maintenanceEvents(replay),
			"the expired-window event history is not reproducible");
	}
	// #767: recurring sub-tick windows also run exactly once; a longer
	// window remains active across a Station step boundary.
	{
		auto tightSettings = [&]
		{
			auto settings = baseSettings();
			settings.maintenanceJobs = {job("tight", "gen", 0.001, 0.001,
				{requirement("parts", 1.0, true)}, "waste", 10.0)};
			return settings;
		};
		core::World world("MaintenanceSubTickWindows", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(configure(world, tightSettings()),
			"sub-tick-window scenario was rejected");
		// t = ~5.0167 s: only the first window [0.001, 0.002] has
		// completed; the next opens at 10.001.
		require(world.advanceTicks(301), "sub-tick-window run failed into window 1");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the sub-tick-window run was not healthy");
		require(snap.maintenanceJobs[0].occurrencesMissed == 0
			&& snap.maintenanceJobs[0].occurrencesStarted == 1
			&& snap.maintenanceJobs[0].occurrencesCompleted == 1
			&& !snap.maintenanceJobs[0].exhausted
			&& snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Completed
			&& !snap.maintenanceJobs[0].isolated,
			"the recurring sub-tick window was not serviced at its exact instant");
		require(approx(snap.stores[2].currentQualified, 7.0)
			&& approx(snap.stores[3].currentQualified, 1.0),
			"the recurring job did not conserve consumed material");
		// t = ~10.0167 s: the second window [10.001, 10.002] has
		// completed too - one completion per declared window.
		require(world.advanceTicks(300), "sub-tick-window run failed into window 2");
		auto const& next = world.getStationSystemsSnapshot();
		require(next.maintenanceJobs[0].occurrencesMissed == 0
			&& next.maintenanceJobs[0].occurrencesStarted == 2
			&& next.maintenanceJobs[0].occurrencesCompleted == 2,
			"the recurring split-window catch-up did not complete every occurrence");
		// Determinism: the same recurring work replays identically.
		core::World replay("MaintenanceSubTickWindowsReplay", 2, 2);
		replay.pauseSimulation(); replay.finishBuild();
		require(configure(replay, tightSettings()),
			"sub-tick-window replay scenario was rejected");
		require(replay.advanceTicks(601), "sub-tick-window replay run failed");
		require(replay.getStationSystemsSnapshot().maintenanceJobs
			== world.getStationSystemsSnapshot().maintenanceJobs,
			"the sub-tick-window replay diverged from the original run");
		// The policy is not over-restrictive: a window wide enough for
		// a tick to land inside it still starts mid-window. Same first
		// occurrence, a duration that keeps the window open past the
		// first tick.
		auto wideSettings = tightSettings();
		wideSettings.maintenanceJobs[0].id = "wide";
		wideSettings.maintenanceJobs[0].durationSeconds = 5.0;
		wideSettings.maintenanceJobs[0].recurrenceSeconds = 0.0;
		core::World midWindow("MaintenanceMidWindowStart", 2, 2);
		midWindow.pauseSimulation(); midWindow.finishBuild();
		require(configure(midWindow, wideSettings),
			"mid-window-start scenario was rejected");
		require(midWindow.advanceTicks(1), "mid-window-start first tick failed");
		auto const& started = midWindow.getStationSystemsSnapshot();
		require(started.maintenanceJobs[0].phase == core::StationMaintenancePhase::Active
			&& started.maintenanceJobs[0].occurrencesStarted == 1
			&& started.maintenanceJobs[0].occurrencesMissed == 0
			&& started.maintenanceJobs[0].isolated,
			"an open window was not startable mid-window - the deadline policy over-restricts");
	}
}
}

void registerStationOperationsChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"outages", outages});
	checks.push_back({"maintenance", maintenance});
	checks.push_back({"failures", failures});
}
