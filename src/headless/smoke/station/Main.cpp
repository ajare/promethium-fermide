#include "Smoke.h"
#include "Documents.h"
#include "Connections.h"
#include "BatteryObservations.h"
#include "ObservationIntegration.h"
#include "StationThermalChecks.h"
#include "StationGenerationChecks.h"
#include "StationAtmosphereChecks.h"
#include "StationWaterChecks.h"
#include "StationRecoveryChecks.h"
#include "StationBiologyChecks.h"
#include "StationRig.h"
#include "StationCoreChecks.h"
#include "StationPortChecks.h"
#include "StationSeamChecks.h"
#include "StationPowerChecks.h"
#include "StationCompoundPowerChecks.h"
#include "StationBatteryChecks.h"
#include "StationLogisticsChecks.h"
void registerScheduledTransferChecks(std::vector<smoke::Check>& checks);
#include "MaterialObservations.h"
#include "Coupled.h"
#include "Reference.h"
#include "Scenarios.h"
#include "Stability.h"
#include "core/World.h"
#include "core/StationPorts.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationSystems.h"
#include "core/StationConnections.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>

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
			loop.hazardClass = "normal";
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
			thin.hazardClass = "normal";
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
			cold.hazardClass = "normal";
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
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "sink";
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
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "sink";
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
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "cold";
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
			loopA.id = "loop-a"; loopA.hazardClass = "normal"; loopA.reservoirId = "cold";
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
			loop.hazardClass = "normal";
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
			settings.hazardClass = "normal";
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

void dispatch(smoke::Context const&)
{
	// #415: the dispatcher closes the loop the operator used to close by
	// hand. In automatic mode the station commands its own generators and
	// fuel-cell backups against the real demand signal, honouring startup,
	// ramp, fuel-above-reserve and cooling limits, so an eclipse is bridged
	// by the battery and then carried by started generation while the
	// declared reserves are never spent to meet ordinary load. Manual mode
	// keeps the pre-#415 contract: the operator's setpoints are the only
	// commands and the dispatcher never moves a source.
	auto const approx = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const replaceAll = [](std::string text, std::string const& from, std::string const& to)
	{
		for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
			text.replace(pos, from.size(), to);
		return text;
	};
	auto sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	auto sinkReservoir = [](double capacityKJPerK, double startingC)
	{
		core::ThermalReservoirSettings settings;
		settings.id = "hot";
		settings.thermalClass = "generation";
		settings.heatCapacityKJPerK = capacityKJPerK;
		settings.startingTemperatureC = startingC;
		settings.minTemperatureC = -50.0;
		settings.maxTemperatureC = 200.0;
		return settings;
	};
	auto fuelStore = [](std::string id, std::string unit, double capacity,
		double starting, double reserve)
	{
		core::ResourceStoreSettings settings;
		settings.id = std::move(id);
		settings.unit = std::move(unit);
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = reserve;
		return settings;
	};
	auto generatorSettings = [](double ratedKW)
	{
		core::GeneratorSettings settings;
		settings.id = "gen";
		settings.fuelStoreId = "diesel";
		settings.thermalClass = "generation";
		settings.heatSinkId = "hot";
		settings.ratedOutputKW = ratedKW;
		settings.efficiency = 0.5;
		settings.maxRampKWPerSecond = 5.0;
		settings.commandedOutputKW = 0.0;
		settings.startupPowerKW = 1.0;
		settings.startupSeconds = 6.0;
		settings.shutdownHeatKW = 1.0;
		settings.shutdownSeconds = 10.0;
		settings.maxSinkTemperatureC = 90.0;
		return settings;
	};
	auto battery = [](double nameplateKWh, double usableKWh, double startingKWh,
		double maxDischargeKW)
	{
		core::BatterySettings settings;
		settings.id = "bank";
		settings.nameplateKWh = nameplateKWh;
		settings.usableKWh = usableKWh;
		settings.startingStoredKWh = startingKWh;
		settings.maxChargeKW = 10.0;
		settings.maxDischargeKW = maxDischargeKW;
		settings.chargeEfficiency = 1.0;
		settings.dischargeEfficiency = 1.0;
		return settings;
	};
	auto criticalLoad = [](double demandKW)
	{
		core::EquipmentLoadSettings settings;
		settings.id = "critical";
		settings.electricalDemandKW = demandKW;
		settings.operatingProfile = constantProfile(1.0);
		settings.heatOutputKW = 0.0;
		settings.thermalClass = "low";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.criticalFloorKW = demandKW;
		settings.priority = 100;
		settings.maxCurtailment = 0.0;
		return settings;
	};
	auto solarAt = [](double fraction)
	{
		core::SolarArraySettings settings;
		settings.id = "array";
		settings.capacityKW = 12.0;
		settings.illumination = constantProfile(fraction);
		return settings;
	};
	struct Rig
	{
		std::vector<core::ResourceStoreSettings> stores;
		std::vector<core::ThermalReservoirSettings> reservoirs;
		std::vector<core::GeneratorSettings> generators;
		std::vector<core::BatterySettings> batteries;
		std::vector<core::SolarArraySettings> solar;
		std::vector<core::EquipmentLoadSettings> loads;
		std::vector<core::FuelCellSettings> cells;
		core::StationDispatchMode mode{core::StationDispatchMode::Manual};
	};
	auto run = [](core::World& world, Rig rig, int ticks)
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.dispatchMode = rig.mode;
		settings.stores = std::move(rig.stores);
		settings.thermalReservoirs = std::move(rig.reservoirs);
		settings.generators = std::move(rig.generators);
		settings.batteries = std::move(rig.batteries);
		settings.solarArrays = std::move(rig.solar);
		settings.equipmentLoads = std::move(rig.loads);
		settings.fuelCells = std::move(rig.cells);
		std::string diagnostic;
		require(world.configureStationSystems(settings, false, &diagnostic),
			("automatic dispatch rig refused: " + diagnostic).c_str());
		world.resumeSimulation();
		require(world.advanceTicks(ticks), "automatic dispatch run failed");
	};
	auto fullRig = [&](core::StationDispatchMode mode, double diesel, double dieselReserve)
	{
		Rig rig;
		rig.mode = mode;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, diesel, dieselReserve),
			{"water", "kg", 1000.0, 500.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		rig.generators = {generatorSettings(10.0)};
		rig.batteries = {battery(12.0, 10.0, 12.0, 10.0)};
		rig.loads = {criticalLoad(6.0)};
		return rig;
	};
	auto makeWorld = [](char const* name)
	{
		return std::make_unique<core::World>(name, 2, 2);
	};

	// Manual mode keeps the operator in charge: with the generator set to
	// zero and the sun gone, the battery alone carries the critical load
	// and drains; the dispatcher never starts the generator. 100 s of
	// eclipse at 6 kW drains ~1/6 kWh from the bank.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Manual, 1000.0, 0.0);
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 6000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.dispatchMode == core::StationDispatchMode::Manual,
			"manual mode not reported in the snapshot");
		require(approx(power.generators[0].outputKW, 0.0),
			"manual mode let the dispatcher start the generator");
		require(power.batteries[0].storedKWh < 11.9,
			"manual eclipse did not drain the battery carrying the load");
		require(approx(power.totalUnmetKW, 0.0, 1e-9),
			"manual mode left the critical load unmet while the battery could carry it");
		require(!power.reserveExhausted,
			"manual mode raised the automatic reserve alarm");
	}

	// Automatic mode bridges the eclipse with the battery and then starts
	// the generator against the deficit; once the generator carries the
	// load the battery stops draining and holds far above the manual drain.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 6000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.dispatchMode == core::StationDispatchMode::Automatic,
			"automatic mode not reported in the snapshot");
		require(power.generators[0].dispatchCommandKW > 0.0,
			"automatic mode issued no dispatch command to the generator");
		require(power.generators[0].outputKW > 5.9,
			"automatic generator did not ramp up to carry the load");
		require(approx(power.totalUnmetKW, 0.0, 1e-9),
			"automatic dispatch left the critical load unmet once the generator was online");
		// The battery bridged startup and ramp, then held: it is well above
		// the manual-mode drain and only slightly below its starting charge.
		require(power.batteries[0].storedKWh > 11.9
			&& power.batteries[0].storedKWh < 12.0,
			"automatic battery did not bridge and then hold the load");
		require(approx(power.batteryDischargeKW, 0.0, 1e-9),
			"automatic battery still discharging once the generator carried the load");
		require(sawEvent(world, "Dispatch brought generator"),
			"automatic generator start was not recorded as a dispatch action");
		require(!power.reserveExhausted,
			"reserve alarm raised while the generator could still carry the load");
	}

	// Automatic mode with the drawable fuel exhausted: the dispatcher
	// cannot draw the protected reserve for ordinary load, so the generator
	// never runs, the battery rides the load down to its own protected
	// reserve floor, and the reserve-exhausted alarm is raised rather
	// than the protected fuel reserve being silently spent.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 5.0, 5.0);
		// A small bank power-capped to carry the load: 0.5 kWh cyclable
		// above its 2 kWh reserve floor. The load is fully curtable so it
		// keeps drawing as the bank's deliverable tapers, letting the bank
		// actually reach its protected floor rather than strand a residual.
		rig.batteries = {battery(2.5, 0.5, 2.5, 10.0)};
		rig.loads[0].maxCurtailment = 1.0;
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 24000);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.reserveExhausted,
			"running the drawable fuel dry did not raise the reserve alarm");
		require(power.totalUnmetKW > 1e-6,
			"reserve exhaustion left no unmet demand while the load still needed power");
		require(sawEvent(world, "Electrical reserve exhausted"),
			"reserve exhaustion was not recorded as an alarm");
		auto const& diesel = world.getStationSystemsSnapshot().stores[0];
		require(diesel.currentQualified + diesel.currentUnqualified >= 5.0 - 1e-9,
			"automatic dispatch spent the protected fuel reserve");
	}

	// Automatic mode with the generator out of fuel: the fuel-cell backup
	// engages so the critical load is still carried when the primary
	// source cannot run.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 0.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		rig.generators = {generatorSettings(10.0)};
		rig.batteries = {battery(12.0, 10.0, 12.0, 10.0)};
		rig.loads = {criticalLoad(4.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hazardClass = "normal";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "hot";
		cell.ratedOutputKW = 6.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 80.0;
		rig.cells = {cell};
		rig.solar = {solarAt(0.0)};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		run(world, std::move(rig), 120);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.backupEngaged,
			"fuel-cell backup did not engage with the generator out of fuel");
		require(world.getStationSystemsSnapshot().fuelCells[0].lastOutputKW > 1e-6,
			"fuel-cell backup engaged but produced no power");
		require(sawEvent(world, "Backup generation engaged"),
			"backup engagement was not recorded as an alarm");
	}

	// #578: no-battery bootstrapping. A stopped generator that needs
	// cranking no longer reserves the dispatch target it cannot serve:
	// the residual load reaches the fuel-cell backup, and the backup
	// funds the legitimate start. With no battery and no sun the old
	// controller let the generator's phantom reservation starve the
	// backup: the critical load stayed unmet forever while dispatch
	// reported zero shortfall. Here the backup carries the load and
	// the crank, the generator starts, ramps, and takes the load back.
	{
		auto bootstrapRig = [&]
		{
			Rig rig;
			rig.mode = core::StationDispatchMode::Automatic;
			rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
				fuelStore("h2", "kg", 100.0, 100.0, 0.0),
				fuelStore("o2", "kg", 800.0, 800.0, 0.0),
				{"water", "kg", 1000.0, 0.0, 0.0}};
			rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
			rig.generators = {generatorSettings(10.0)};
			rig.loads = {criticalLoad(6.0)};
			core::FuelCellSettings cell;
			cell.id = "cell";
			cell.hazardClass = "normal";
			cell.hydrogenStoreId = "h2";
			cell.oxygenStoreId = "o2";
			cell.waterStoreId = "water";
			cell.thermalClass = "generation";
			cell.heatSinkId = "hot";
			cell.ratedOutputKW = 10.0;
			cell.efficiency = 0.6;
			cell.commandedOutputKW = 0.0;
			cell.maxSinkTemperatureC = 150.0;
			rig.cells = {cell};
			return rig;
		};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		run(world, bootstrapRig(), 120);
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& power = snap.power;
			require(power.generators[0].phase == core::StationGeneratorPhase::Starting,
				"the backup-funded start never began without a battery");
			require(power.generators[0].dispatchCommandKW > 0.0,
				"the dispatcher never commanded the stopped generator to start");
			require(power.generators[0].outputKW <= 1e-12,
				"a cranking generator reported output it cannot deliver");
			require(power.generatorStartupDrawKW > 0.9 && power.generatorStartupDrawKW < 1.1,
				"the crank did not draw its startup power from the backup-funded pool");
			require(snap.fuelCells[0].lastOutputKW > 6.9
				&& snap.fuelCells[0].lastOutputKW < 7.1,
				"the backup did not carry the load plus the startup support");
			require(approx(power.dispatchCommittedKW, power.dispatchTargetKW, 1e-9),
				"the bootstrap committed did not match the funded target");
			require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
				"the backup-funded bootstrap reported a shortfall");
			require(approx(power.unmetCriticalKW, 0.0, 1e-9),
				"the critical load went unmet while the backup could carry it");
		}
		// Past startup and the ramp: the generator takes the load back
		// and the backup stands down.
		require(world.advanceTicks(600), "bootstrap run failed after startup");
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& power = snap.power;
			require(power.generators[0].phase == core::StationGeneratorPhase::Running,
				"the backup-funded generator never reached Running");
			require(power.generators[0].outputKW > 5.9,
				"the generator did not ramp up to carry the load");
			require(snap.fuelCells[0].lastOutputKW < 1e-9,
				"the backup kept burning after the generator took the load");
			require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
				"steady-state bootstrap reported a shortfall");
			require(approx(power.unmetCriticalKW, 0.0, 1e-9),
				"the load went unmet once the generator carried it");
		}
		// Determinism: the bootstrap path adds no run-to-run variance.
		{
			auto const probe = [&](Rig rig)
			{
				core::World world("Dispatch", 2, 2);
				world.pauseSimulation();
				world.finishBuild();
				run(world, std::move(rig), 500);
				auto const& snap = world.getStationSystemsSnapshot();
				auto const& power = snap.power;
				return std::array{power.generators[0].outputKW,
					snap.fuelCells[0].lastOutputKW,
					power.dispatchCommittedKW, power.dispatchShortfallKW};
			};
			require(probe(bootstrapRig()) == probe(bootstrapRig()),
				"the no-battery bootstrap was not deterministic");
		}
	}

	// #578: with no backup and nothing to crank on, the stopped
	// generator's unavailable capacity is never reported as a covered
	// target: the shortfall stays explicit beside the unmet load. The
	// old controller booked the full command as committed and reported
	// zero shortfall while the machine delivered nothing.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.batteries.clear();
		rig.solar = {solarAt(0.0)};
		run(world, std::move(rig), 120);
		auto const& power = world.getStationSystemsSnapshot().power;
		require(power.generators[0].phase == core::StationGeneratorPhase::Stopped,
			"a generator with no cranking source somehow left Stopped");
		require(approx(power.dispatchCommittedKW, 0.0, 1e-9),
			"unavailable generator capacity was booked as committed");
		require(power.dispatchShortfallKW > 1e-9,
			"the dispatch shortfall hid the capacity no source could serve");
		require(power.unmetCriticalKW > 1e-9,
			"the unmet critical load vanished from the account");
	}

	// #578: a slow ramp never books as capacity before it arrives.
	// The backup carries the difference tick by tick while the
	// generator climbs its ramp, and the committed account tracks what
	// the sources actually deliver, not what the generator will reach.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		rig.reservoirs = {sinkReservoir(100000.0, 20.0)};
		auto slow = generatorSettings(10.0);
		slow.maxRampKWPerSecond = 0.1;
		rig.generators = {slow};
		rig.loads = {criticalLoad(6.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hazardClass = "normal";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "hot";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 150.0;
		rig.cells = {cell};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		// 1560 ticks at 60 Hz is 26 s: 6 s of crank plus 20 s of
		// 0.1 kW/s ramp, so the generator sits near 2 kW of its 10 kW
		// rating while the backup carries the rest.
		run(world, std::move(rig), 1560);
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& power = snap.power;
		require(power.generators[0].phase == core::StationGeneratorPhase::Running,
			"the slow-ramp generator never reached Running");
		require(power.generators[0].outputKW > 1.5 && power.generators[0].outputKW < 2.5,
			"the slow ramp did not hold to its ramp-limited output");
		require(approx(power.generators[0].outputKW + snap.fuelCells[0].lastOutputKW, 6.0, 1e-6),
			"the backup did not carry exactly the ramp-limited difference");
		require(approx(power.dispatchCommittedKW, 6.0, 1e-6),
			"the committed account did not match what the sources deliver");
		require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
			"the slow-ramp run reported a shortfall the backup could cover");
		require(approx(power.unmetCriticalKW, 0.0, 1e-9),
			"the load went unmet while the backup covered the ramp gap");
	}

	// #578: a source trip during startup. The sink starts just below
	// the generator's maximum and the cranking heat itself pushes the
	// cooling out of qualification mid-crank: the start fails whole,
	// the generator stays stopped, and the backup keeps carrying the
	// load on its own sink with no unmet demand and no phantom
	// committed capacity.
	{
		Rig rig;
		rig.mode = core::StationDispatchMode::Automatic;
		rig.stores = {fuelStore("diesel", "kWh", 1000.0, 100.0, 0.0),
			fuelStore("h2", "kg", 100.0, 100.0, 0.0),
			fuelStore("o2", "kg", 800.0, 800.0, 0.0),
			{"water", "kg", 1000.0, 0.0, 0.0}};
		core::ThermalReservoirSettings hot;
		hot.id = "hot";
		hot.thermalClass = "generation";
		hot.heatCapacityKJPerK = 10.0;
		hot.startingTemperatureC = 89.9;
		hot.minTemperatureC = -50.0;
		hot.maxTemperatureC = 200.0;
		core::ThermalReservoirSettings fcSink;
		fcSink.id = "fcSink";
		fcSink.thermalClass = "generation";
		fcSink.heatCapacityKJPerK = 100000.0;
		fcSink.startingTemperatureC = 20.0;
		fcSink.minTemperatureC = -50.0;
		fcSink.maxTemperatureC = 200.0;
		rig.reservoirs = {hot, fcSink};
		rig.generators = {generatorSettings(10.0)};
		rig.loads = {criticalLoad(6.0)};
		core::FuelCellSettings cell;
		cell.id = "cell";
		cell.hazardClass = "normal";
		cell.hydrogenStoreId = "h2";
		cell.oxygenStoreId = "o2";
		cell.waterStoreId = "water";
		cell.thermalClass = "generation";
		cell.heatSinkId = "fcSink";
		cell.ratedOutputKW = 10.0;
		cell.efficiency = 0.6;
		cell.commandedOutputKW = 0.0;
		cell.maxSinkTemperatureC = 150.0;
		rig.cells = {cell};
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		// The 1 kW crank into a 10 kJ/K sink warms it 0.1 K/s: the
		// 0.1 K headroom is gone within a second and the start fails
		// mid-crank before the 6 s of startup energy can complete.
		run(world, std::move(rig), 2);
		bool sawMidCrankTrip = false;
		for (int tick = 0; tick < 238; ++tick)
		{
			require(world.advanceTicks(1), "trip run failed");
			auto const& p = world.getStationSystemsSnapshot().power;
			if (p.generators[0].blockReason.find("cooling lost during startup")
				!= std::string::npos)
				sawMidCrankTrip = true;
		}
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& power = snap.power;
		require(sawMidCrankTrip,
			"the mid-crank cooling trip was never reported as the block reason");
		require(power.generators[0].phase == core::StationGeneratorPhase::Stopped,
			"a generator whose cooling failed mid-crank did not fall back to Stopped");
		require(approx(power.generators[0].outputKW, 0.0, 1e-12),
			"a trip mid-crank reported generator output");
		require(snap.fuelCells[0].lastOutputKW > 5.9 && snap.fuelCells[0].lastOutputKW < 6.1,
			"the backup did not carry the load alone after the start failed");
		require(approx(power.dispatchShortfallKW, 0.0, 1e-9),
			"the failed-start run reported a shortfall the backup covers");
		require(approx(power.unmetCriticalKW, 0.0, 1e-9),
			"the load went unmet after the start failed while the backup could carry it");
	}

	// The dispatch mode persists and round-trips; a document without the
	// dispatch mode field is refused rather than silently defaulting.
	{
		core::World world("Dispatch", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
		rig.solar = {solarAt(0.5)};
		run(world, std::move(rig), 10);
		auto const text = yaml(world);
		require(text.find("dispatchMode: automatic") != std::string::npos,
			"automatic dispatch mode was not saved");
		core::SerializationWorkData data;
		auto reader = core::YamlSerializer::fromString(text);
		reader->deserialize();
		core::World loaded("Loaded", 2, 2);
		loaded.deserialize(*reader, data);
		require(loaded.getStationSystemsSnapshot().power.dispatchMode
			== core::StationDispatchMode::Automatic,
			"automatic dispatch mode did not survive the round trip");
		auto stale = text;
		require(stale.find("version: 99") != std::string::npos,
			"the fixture document carries no version line");
		require(stale.find("dispatchMode: automatic") != std::string::npos,
			"the fixture document lost the dispatch mode before the guard test");
		// A pre-91 document that still carries the dispatch mode is refused
		// rather than silently dropping the controller setting.
		stale = replaceAll(stale, "version: 99", "version: 90");
		bool refused = false;
		try
		{
			auto staleReader = core::YamlSerializer::fromString(stale);
			staleReader->deserialize();
			core::World staleWorld("Stale", 2, 2);
			staleWorld.deserialize(*staleReader, data);
		}
		catch (std::exception const&)
		{
			refused = true;
		}
		require(refused, "a pre-91 document carrying the dispatch mode was silently accepted");
	}

	// Determinism: the same rig advanced the same way produces the same
	// dispatch outputs, so the controller adds no run-to-run variance.
	{
		auto const first = [&]
		{
			core::World world("Dispatch", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
			rig.solar = {solarAt(0.0)};
			run(world, std::move(rig), 40);
			return world.getStationSystemsSnapshot().power.generators[0].outputKW;
		}();
		auto const second = [&]
		{
			core::World world("Dispatch", 2, 2);
			world.pauseSimulation();
			world.finishBuild();
			Rig rig = fullRig(core::StationDispatchMode::Automatic, 1000.0, 0.0);
			rig.solar = {solarAt(0.0)};
			run(world, std::move(rig), 40);
			return world.getStationSystemsSnapshot().power.generators[0].outputKW;
		}();
		require(approx(first, second), "automatic dispatch was not deterministic");
	}
}

// #416: protected cross-system service dependencies. Containment
// circulation, biological essential circulation and authored critical
// cooling are Critical station services: they are admitted from the
// actual generation-and-battery pool ahead of every discretionary
// external load, so shedding experiments or lighting never cuts the
// support services protected loads depend on. What the pool cannot
// deliver stays explicit unmet critical minimum with its reason;
// generator shutdown-cooling stays visible even when the machine's
// electrical output has stopped. Everything runs through the World
// tick/snapshot/event seam with the bundled Lua sandbox auditing the
// same rules inside each private instance.
void protectedServices(smoke::Context const&)
{
	auto const closeTo = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const sawEvent = [](core::World& world, std::string const& needle)
	{
		for (auto const& event : world.getStationSystemsEvents())
			if (event.message.find(needle) != std::string::npos) return true;
		return false;
	};
	auto const constantProfile = [](double level)
	{
		core::ProfileSettings profile;
		profile.constantLevel = level;
		return profile;
	};
	auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};
	auto cropComposition = []()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.0;
		composition.nutrientNitrogen = 0.05;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.03;
		composition.otherMinerals = 0.05;
		composition.energyKJPerKgDry = 1700.0;
		return composition;
	};
	auto massStore = [](std::string id, double capacity, double starting,
		std::string hazard = "normal")
	{
		core::ResourceStoreSettings settings;
		settings.id = std::move(id);
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.hazardClass = std::move(hazard);
		return settings;
	};
	auto habitat = []()
	{
		core::AtmosphereSettings settings;
		settings.id = "habitat";
		settings.hazardClass = "normal";
		settings.volumeM3 = 100.0;
		settings.startingOxygenKg = 40.0;
		settings.startingNitrogenKg = 80.0;
		settings.startingCarbonDioxideKg = 5.0;
		settings.startingWaterVapourKg = 1.0;
		settings.startingTemperatureC = 22.0;
		settings.thermalCapacityKJPerK = 20000.0;
		settings.minPressureKPa = 1.0;
		settings.maxPressureKPa = 200.0;
		return settings;
	};
	auto cropRig = [&](double lampKW, double circulationKW)
	{
		core::CropPopulationSettings settings;
		settings.id = "canopy";
		settings.atmosphereId = "habitat";
		settings.waterStoreId = "irrigation";
		settings.nitrogenStoreId = "fertN";
		settings.phosphorusStoreId = "fertP";
		settings.potassiumStoreId = "fertK";
		settings.mineralsStoreId = "fertMin";
		settings.composition = cropComposition();
		settings.growingAreaM2 = 10.0;
		settings.maxBiomassDensityKgPerM2 = 2.0;
		settings.initialBiomassKg = 1.0;
		settings.lampRatedKW = lampKW;
		settings.illuminationProfile = constantProfile(1.0);
		settings.circulationPowerKW = circulationKW;
		settings.growthYieldKgPerKWh = 2.0;
		settings.respirationPerHour = 0.5;
		settings.minGrowingTemperatureC = 10.0;
		settings.maxGrowingTemperatureC = 35.0;
		return settings;
	};
	auto cropStores = [&]()
	{
		std::vector<core::ResourceStoreSettings> stores;
		stores.push_back(massStore("irrigation", 200.0, 150.0));
		stores.push_back(massStore("fertN", 10.0, 5.0));
		stores.push_back(massStore("fertP", 10.0, 5.0));
		stores.push_back(massStore("fertK", 10.0, 5.0));
		stores.push_back(massStore("fertMin", 10.0, 5.0));
		return stores;
	};
	auto discretionaryLoad = [&](std::string id, double demandKW, double maxCurtailment)
	{
		core::EquipmentLoadSettings settings;
		settings.id = std::move(id);
		settings.electricalDemandKW = demandKW;
		settings.operatingProfile = constantProfile(1.0);
		settings.heatOutputKW = 0.0;
		settings.thermalClass = "low";
		settings.coolingDisposition = core::StationCoolingDisposition::Exported;
		settings.criticalFloorKW = 0.0;
		settings.priority = 10;
		settings.maxCurtailment = maxCurtailment;
		return settings;
	};
	auto solarArray = [&](double capacityKW)
	{
		core::SolarArraySettings settings;
		settings.id = "field";
		settings.capacityKW = capacityKW;
		settings.illumination = constantProfile(1.0);
		return settings;
	};
	auto reservoir = [](std::string id, std::string thermalClass, double startingC)
	{
		core::ThermalReservoirSettings settings;
		settings.id = std::move(id);
		settings.thermalClass = std::move(thermalClass);
		settings.heatCapacityKJPerK = 10000.0;
		settings.startingTemperatureC = startingC;
		settings.minTemperatureC = -50.0;
		settings.maxTemperatureC = 200.0;
		return settings;
	};
	auto hazardousLoop = [](std::string id, double pumpKW, double heaterKW, double startingC)
	{
		core::CoolantLoopSettings settings;
		settings.id = std::move(id);
		settings.hazardClass = "hazardous";
		settings.reservoirId = "sink";
		settings.coolantCapacityKg = 100.0;
		settings.startingCoolantKg = 100.0;
		settings.specificHeatKJPerKgK = 4.0;
		settings.startingTemperatureC = startingC;
		settings.freezeProtectionSetpointC = 5.0;
		settings.pumpPowerKW = pumpKW;
		settings.ratedFlowKgPerS = 1.0;
		settings.exchangerConductanceKWPerK = 0.0;
		settings.heaterCapacityKW = heaterKW;
		return settings;
	};
	auto configure = [](core::World& world, core::StationSystemsSettings settings, char const* what)
	{
		std::string diagnostic;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		require(world.configureStationSystems(settings, false, &diagnostic),
			(std::string(what) + " refused: " + diagnostic).c_str());
	};

	// Scenario 1: lighting curtailment with preserved circulation. The
	// sun leaves less than the canopy's lamp plus the discretionary
	// experiment wants; the essential circulation is admitted whole from
	// the actual pool ahead of the experiment, and only the discretionary
	// lamp is curtailed. Before the protection the experiment's grant
	// could eat into the circulation draw; now it never can.
	{
		core::World world("ProtCrop", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = cropStores();
		settings.atmospheres = { habitat() };
		settings.cropPopulations = { cropRig(4.0, 1.0) };
		settings.solarArrays = { solarArray(4.5) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 4.0, 0.5) };
		configure(world, settings, "lighting curtailment rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "lighting curtailment");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& crop = snap.cropPopulations[0];
		require(closeTo(crop.lastCirculationRequestedKW, 1.0),
			"the canopy circulation demand changed");
		require(closeTo(crop.lastCirculationSuppliedKW, 1.0),
			"the discretionary experiment cut into the protected biological circulation");
		require(crop.lastLightSuppliedKW + 1e-9 < crop.lastLightRequestedKW,
			"the discretionary lamp was not curtailed although the pool was short");
		require(closeTo(snap.power.loads[0].suppliedKW, 3.5),
			"the discretionary load did not take exactly the pool the protected draw left");
		require(closeTo(snap.power.protectedDemandKW, 1.0),
			"the protected demand total is wrong");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.protectedUnmetKW, 0.0),
			"the protected supply total is wrong");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised with no unmet protected demand");
	}

	// Scenario 2: hazardous shortage. The hazardous containment loop's
	// pump and freeze heater are protected by the loop's hazard class
	// alone. With a pool that covers them the containment runs whole
	// while the discretionary load is squeezed; with a pool below the
	// protected minimum the shortfall stays explicit as unmet critical
	// demand with the reason named and the event logged - never silently
	// reclassified as permissible curtailment.
	{
		core::World world("ProtHaz", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		settings.coolantLoops = { hazardousLoop("hazLoop", 1.0, 0.5, 0.0) };
		settings.solarArrays = { solarArray(3.0) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 3.0, 0.5) };
		configure(world, settings, "hazardous shortage rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "hazardous shortage");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& loop = snap.coolantLoops[0];
		require(closeTo(loop.pumpSuppliedKW, 1.0) && closeTo(loop.heaterSuppliedKW, 0.5),
			"the hazardous containment circulation was not protected ahead of the experiment");
		require(closeTo(loop.circulationFraction, 1.0),
			"the protected hazardous loop did not circulate fully");
		require(closeTo(snap.power.protectedDemandKW, 1.5)
			&& closeTo(snap.power.protectedSuppliedKW, 1.5),
			"the containment protection totals are wrong");
		require(!snap.power.protectedUnmet,
			"the containment alarm raised while the pool covered the protected draw");
	}
	{
		core::World world("ProtHazTight", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		settings.coolantLoops = { hazardousLoop("hazLoop", 1.0, 0.5, 0.0) };
		settings.solarArrays = { solarArray(1.0) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 3.0, 0.5) };
		configure(world, settings, "tight hazardous rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "tight hazardous");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& loop = snap.coolantLoops[0];
		require(closeTo(loop.pumpSuppliedKW, 1.0),
			"the containment pump lost supply even though the pool covered it first");
		require(closeTo(loop.heaterSuppliedKW, 0.0),
			"the freeze heater drew beyond the pool the protected pass left");
		require(snap.power.protectedUnmet,
			"the protected shortfall alarm did not raise under the tight pool");
		require(closeTo(snap.power.protectedUnmetKW, 0.5),
			"the unmet protected critical minimum is not the freeze-heater remainder");
		require(sawEvent(world, "Protected containment-circulation 'hazLoop' unmet"),
			"the protected containment shortfall was not announced");
		require(snap.power.protectedServices[0].reason.find("pool") != std::string::npos,
			"the unmet protected service does not name the binding pool reason");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the discretionary load took power ahead of the protected containment draw");
	}

	// Scenario 3: generator cooling shortfall and shutdown cooling.
	// A generator whose sink is already at the maximum sink temperature
	// cannot run at all - the cooling shortfall is explicit - while the
	// biological circulation stays protected through it.
	{
		core::World world("ProtGenCold", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = cropStores();
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 100.0;
		diesel.startingQualified = 50.0;
		settings.stores.push_back(diesel);
		settings.atmospheres = { habitat() };
		settings.cropPopulations = { cropRig(1.0, 1.0) };
		settings.thermalReservoirs = { reservoir("hot", "generation", 95.0) };
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.fuelStoreId = "diesel";
		generator.thermalClass = "generation";
		generator.heatSinkId = "hot";
		generator.ratedOutputKW = 5.0;
		generator.efficiency = 0.5;
		generator.maxRampKWPerSecond = 5.0;
		generator.commandedOutputKW = 5.0;
		generator.startupPowerKW = 0.0;
		generator.startupSeconds = 0.0;
		generator.shutdownHeatKW = 1.0;
		generator.shutdownSeconds = 10.0;
		generator.maxSinkTemperatureC = 90.0;
		settings.generators = { generator };
		settings.solarArrays = { solarArray(1.5) };
		configure(world, settings, "generator cooling shortfall rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "generator cooling shortfall");
		auto const& snap = world.getStationSystemsSnapshot();
		require(!snap.power.generators[0].coolingAvailable,
			"the generator reported qualified cooling into an over-hot sink");
		require(closeTo(snap.power.generators[0].outputKW, 0.0),
			"the generator produced output without qualified cooling");
		require(snap.power.generators[0].blockReason.find("cooling") != std::string::npos,
			"the generator block reason does not name the cooling shortfall");
		require(closeTo(snap.cropPopulations[0].lastCirculationSuppliedKW, 1.0),
			"the biological circulation was not protected through the generator cooling shortfall");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised while the pool covered the circulation");
	}

	// Scenario 3b: shutdown cooling stays a protected obligation after
	// the electrical output stops. The generator burns out its fuel and
	// trips to Shutdown; the decay heat keeps booking and the protected
	// service account shows the standing shutdown-cooling obligation
	// while output is zero - stopping generation never erases the heat.
	{
		core::World world("ProtGenShutdown", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		core::ResourceStoreSettings starveDiesel;
		starveDiesel.id = "diesel";
		starveDiesel.unit = "kWh";
		starveDiesel.capacity = 100.0;
		starveDiesel.startingQualified = 0.005;
		settings.stores = { starveDiesel };
		settings.thermalReservoirs = { reservoir("hot", "generation", 20.0) };
		core::GeneratorSettings generator;
		generator.id = "gen";
		generator.fuelStoreId = "diesel";
		generator.thermalClass = "generation";
		generator.heatSinkId = "hot";
		generator.ratedOutputKW = 5.0;
		generator.efficiency = 0.5;
		generator.maxRampKWPerSecond = 5.0;
		generator.commandedOutputKW = 5.0;
		generator.startupPowerKW = 0.0;
		generator.startupSeconds = 0.0;
		generator.shutdownHeatKW = 2.0;
		generator.shutdownSeconds = 60.0;
		generator.maxSinkTemperatureC = 90.0;
		settings.generators = { generator };
		settings.solarArrays = { solarArray(0.0) };
		configure(world, settings, "shutdown cooling rig");
		world.resumeSimulation();
		runOrDiagnose(world, 240, "shutdown cooling");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.power.generators[0].phase == core::StationGeneratorPhase::Shutdown,
			"the fuel-starved generator did not enter Shutdown");
		require(closeTo(snap.power.generators[0].outputKW, 0.0),
			"the generator still reports electrical output after the fuel ran out");
		require(snap.power.generators[0].coolingRequired,
			"the shutdown obligation stopped with the electrical output");
		bool sawShutdownCooling = false;
		for (auto const& service : snap.power.protectedServices)
		{
			if (service.kind != "shutdown-cooling" || service.id != "gen") continue;
			sawShutdownCooling = true;
			require(service.requestedKW > 0.0
				&& closeTo(service.unmetKW, 0.0, 1e-9),
				"the shutdown-cooling protected account is not the booked decay heat");
		}
		require(sawShutdownCooling,
			"the standing shutdown-cooling obligation is not exposed as a protected service");
	}

	// Scenario 4: microbial loss under coordinated operation. The
	// microbial digester competes for the same pool as everything else
	// and is not itself protected; under a tight pool its work is
	// curtailed while the containment circulation ahead of it stays
	// whole - the shortage lands on the discretionary process, never
	// on the protected support services.
	{
		core::World world("ProtMicrobial", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::ResourceCompositionSettings feedComposition;
		feedComposition.moisture = 0.9;
		feedComposition.nutrientNitrogen = 0.05;
		feedComposition.atmosphericNitrogen = 0.02;
		feedComposition.phosphorus = 0.02;
		feedComposition.potassium = 0.02;
		feedComposition.otherMinerals = 0.01;
		feedComposition.energyKJPerKgDry = 8000.0;
		core::ResourceStoreSettings feed;
		feed.id = "h_waste";
		feed.unit = "kg";
		feed.capacity = 100.0;
		feed.startingQualified = 50.0;
		feed.hazardClass = "hazardous";
		feed.composition = feedComposition;
		core::StationSystemsSettings settings;
		settings.stores = { feed,
			massStore("h_oxygen", 100.0, 50.0, "hazardous"),
			massStore("h_water", 100.0, 50.0, "hazardous"),
			massStore("h_media", 100.0, 50.0, "hazardous"),
			massStore("h_nutrients", 100.0, 0.0, "hazardous"),
			massStore("h_recoveredWater", 100.0, 0.0, "hazardous"),
			massStore("h_co2", 100.0, 0.0, "hazardous"),
			massStore("h_residue", 100.0, 0.0, "hazardous"),
			massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		settings.coolantLoops = { hazardousLoop("hazLoop", 1.0, 0.0, 20.0) };
		core::MicrobialProcessorSettings digester;
		digester.id = "digester";
		digester.hazardClass = "hazardous";
		digester.feedStoreId = "h_waste";
		digester.oxygenStoreId = "h_oxygen";
		digester.waterStoreId = "h_water";
		digester.consumableStoreId = "h_media";
		digester.nutrientStoreId = "h_nutrients";
		digester.recoveredWaterStoreId = "h_recoveredWater";
		digester.co2StoreId = "h_co2";
		digester.residueStoreId = "h_residue";
		digester.thermalClass = "low";
		digester.heatSinkId = "sink";
		digester.rateKgPerS = 0.001;
		digester.specificEnergyKWhPerKgFeed = 2.0;
		digester.oxygenPerKgFeedKg = 0.8;
		digester.processWaterPerKgFeedKg = 0.5;
		digester.consumablePerKgFeed = 0.01;
		digester.co2PerKgFeedKg = 0.3;
		digester.nitrogenRecovery = 0.8;
		digester.phosphorusRecovery = 0.8;
		digester.potassiumRecovery = 0.8;
		digester.mineralsRecovery = 0.7;
		digester.waterRecovery = 0.7;
		digester.reactionHeatKJPerKgFeed = 600.0;
		digester.minOperatingTemperatureC = 15.0;
		digester.maxSinkTemperatureC = 45.0;
		settings.microbialProcessors = { digester };
		settings.solarArrays = { solarArray(7.0) };
		configure(world, settings, "microbial coordination rig");
		world.resumeSimulation();
		runOrDiagnose(world, 10, "microbial coordination");
		auto const& snap = world.getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the containment circulation was not protected ahead of the digester");
		auto const& processor = snap.microbialProcessors[0];
		require(processor.lastPowerDemandKW > 1.0,
			"the digester declared no meaningful electrical demand to be curtailed");
		require(processor.lastPowerSuppliedKW + 1e-9 < processor.lastPowerDemandKW,
			"the digester was not curtailed although the pool was short after the protected draw");
		require(!snap.power.protectedUnmet,
			"the protected alarm raised while the protected draw was fully supplied");
	}

	// Scenario 5: competing protected dependencies resolve
	// deterministically by declaration order. The pool covers the first
	// hazardous loop whole and part of the second; the second loop's
	// remainder stays unmet critical demand and is announced, while the
	// first loop's event log stays clean. Two identical runs agree.
	auto competingRun = [&]() -> std::unique_ptr<core::World>
	{
		auto world = std::make_unique<core::World>("ProtCompete", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0) };
		settings.coolantLoops = { hazardousLoop("loopA", 2.0, 0.0, 20.0),
			hazardousLoop("loopB", 2.0, 0.0, 20.0) };
		settings.solarArrays = { solarArray(3.0) };
		std::string diagnostic;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		if (!world->configureStationSystems(settings, false, &diagnostic))
			throw std::runtime_error("competing protected rig refused: " + diagnostic);
		world->resumeSimulation();
		if (!world->advanceTicks(10))
			throw std::runtime_error("competing protected run failed");
		return world;
	};
	{
		auto world = competingRun();
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 2.0),
			"the first-declared protected loop did not get its full draw");
		require(closeTo(snap.coolantLoops[1].pumpSuppliedKW, 1.0),
			"the second protected loop did not get exactly the pool remainder");
		require(snap.power.protectedUnmet && closeTo(snap.power.protectedUnmetKW, 1.0),
			"the competing protected shortfall is not the second loop's unmet minimum");
		require(sawEvent(*world, "Protected containment-circulation 'loopB' unmet"),
			"the second loop's protected shortfall was not announced");
		bool firstLoopAnnounced = false;
		for (auto const& event : world->getStationSystemsEvents())
			if (event.message.find("loopA' unmet") != std::string::npos) firstLoopAnnounced = true;
		require(!firstLoopAnnounced,
			"the fully supplied first loop was announced as unmet");
		auto const firstUnmet = snap.power.protectedUnmetKW;
		auto const second = competingRun();
		require(closeTo(second->getStationSystemsSnapshot().power.protectedUnmetKW, firstUnmet),
			"competing protected allocation was not deterministic");
	}

	// Scenario 5b: critical chiller circulation dependency (#579).
	// A critical-service chiller on an ordinary, unprotected coolant loop
	// depends on that loop's pump for any cooling it can deliver, so the
	// protected pass funds the pump ahead of the chiller's own work and
	// ahead of every discretionary load, and sizes the cooling credit by
	// the circulation the pool actually powers - never by the loop's
	// fill and outage capacity alone. The loop needs no manual
	// criticalService flag for its circulation to be protected, and a
	// pump the pool cannot power leaves the cooling that could not be
	// delivered explicit as unmet critical service naming the
	// circulation as the binding dependency.
	auto normalLoop = [](std::string id, double pumpKW, double ratedFlowKgPerS)
	{
		core::CoolantLoopSettings settings;
		settings.id = std::move(id);
		settings.hazardClass = "normal";
		settings.reservoirId = "sink";
		settings.coolantCapacityKg = 1.0;
		settings.startingCoolantKg = 1.0;
		settings.specificHeatKJPerKgK = 1.0;
		settings.startingTemperatureC = 20.0;
		settings.freezeProtectionSetpointC = 5.0;
		settings.pumpPowerKW = pumpKW;
		settings.ratedFlowKgPerS = ratedFlowKgPerS;
		settings.exchangerConductanceKWPerK = 0.0;
		settings.heaterCapacityKW = 0.0;
		return settings;
	};
	auto criticalChiller = [](std::string id, std::string loopId)
	{
		core::ChillerSettings settings;
		settings.id = std::move(id);
		settings.thermalClass = "low";
		settings.coldLoopId = std::move(loopId);
		settings.condenserReservoirId = "hot";
		settings.ratedCoolingCapacityKW = 2.0;
		settings.cop = 1.0;
		settings.auxiliaryPowerKW = 0.0;
		settings.chilledSetpointC = 5.0;
		settings.maxCondensingTemperatureC = 45.0;
		settings.criticalService = true;
		return settings;
	};
	auto dependencyRun = [&](double solarKW, double ratedFlowKgPerS,
		bool fullLoopOutage = false)
	{
		auto world = std::make_unique<core::World>("ProtChiller", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0),
			reservoir("hot", "low", 20.0) };
		settings.coolantLoops = { normalLoop("loop", 1.0, ratedFlowKgPerS) };
		settings.chillers = { criticalChiller("chiller", "loop") };
		settings.solarArrays = { solarArray(solarKW) };
		settings.equipmentLoads = { discretionaryLoad("experiment", 1.0, 1.0) };
		if (fullLoopOutage)
		{
			core::InstallationOutageSettings outage;
			outage.id = "loopOut";
			outage.installationId = "loop";
			outage.capacityFraction = 0.0;
			outage.firstOccurrenceSeconds = 0.0;
			outage.durationSeconds = 10.0;
			settings.outages = { outage };
		}
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		std::string diagnostic;
		if (!world->configureStationSystems(settings, false, &diagnostic))
			throw std::runtime_error("chiller dependency rig refused: " + diagnostic);
		world->resumeSimulation();
		runOrDiagnose(*world, 1, "chiller dependency run");
		return world;
	};
	auto findProtectedService = [](core::StationSystemsSnapshot const& snap,
		std::string const& id, std::string const& kind) -> core::StationProtectedServiceSnapshot const*
	{
		for (auto const& service : snap.power.protectedServices)
			if (service.id == id && service.kind == kind) return &service;
		return nullptr;
	};
	// A tight but partially usable pool: the pump is funded whole first,
	// the chiller converts what the pool leaves into real cooling, and the
	// remainder of the protected cooling demand stays unmet with the
	// pool named as the binding constraint. The competing discretionary
	// load gets nothing ahead of the protected bundle.
	{
		auto world = dependencyRun(2.0, 1.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the unprotected loop's pump was not protected as the critical chiller's dependency");
		require(closeTo(snap.coolantLoops[0].circulationFraction, 1.0),
			"the dependency-protected loop did not circulate fully");
		require(closeTo(snap.chillers[0].powerDemandKW, 2.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 1.0),
			"the chiller did not consume exactly the pool the protected pump draw left");
		require(closeTo(snap.chillers[0].coolingKW, 1.0),
			"the chiller's delivered cooling does not match the work actually powered");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 2.0)
			&& closeTo(snap.power.protectedUnmetKW, 1.0),
			"the protected bundle totals do not account for pump plus curtailed cooling work");
		require(snap.power.protectedUnmet,
			"the protected alarm did not raise with unmet critical cooling");
		auto const* pumpService = findProtectedService(snap, "loop",
			"critical-cooling-circulation");
		require(pumpService != nullptr
			&& closeTo(pumpService->requestedKW, 1.0)
			&& closeTo(pumpService->suppliedKW, 1.0),
			"the chiller's circulation dependency is not exposed as a protected service");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 1.0)
			&& closeTo(coolingService->unmetKW, 1.0),
			"the curtailed protected cooling demand is not exposed with its unmet remainder");
		require(coolingService != nullptr
			&& coolingService->reason.find("pool") != std::string::npos,
			"the curtailed cooling does not name the binding pool");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the unmet protected cooling was not announced");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the discretionary load took power ahead of the protected chiller bundle");
	}
	// A pool that cannot even fully power the pump: the circulation
	// itself is the binding dependency, the cooling the unpowered
	// circulation cannot deliver stays explicit unmet critical service,
	// and no pool power is wasted on work the chiller cannot use.
	{
		auto world = dependencyRun(0.5, 1.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 0.5),
			"the dependency pump draw did not take the whole pool first");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"cooling work was funded beyond the circulation the pool could power");
		auto const* pumpService = findProtectedService(snap, "loop",
			"critical-cooling-circulation");
		require(pumpService != nullptr
			&& closeTo(pumpService->requestedKW, 1.0)
			&& closeTo(pumpService->suppliedKW, 0.5)
			&& closeTo(pumpService->unmetKW, 0.5),
			"the curtailed dependency pump draw is not exposed with its unmet remainder");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 0.0)
			&& closeTo(coolingService->unmetKW, 2.0),
			"the cooling blocked by unpowered circulation is not explicit unmet critical service");
		require(coolingService != nullptr
			&& coolingService->reason.find("circulation") != std::string::npos,
			"the binding circulation dependency is not named in the unmet cooling reason");
		require(sawEvent(*world, "Protected critical-cooling-circulation 'loop' unmet"),
			"the curtailed dependency pump draw was not announced");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the circulation-blocked cooling was not announced");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 0.5)
			&& closeTo(snap.power.protectedUnmetKW, 2.5),
			"the protected totals do not expose the circulation-blocked shortfall");
	}
	// A zero-rated-flow loop is a zero-circulation machine: the pump may
	// still run and dissipate as the loop pass powers it (#558), but no
	// coolant moves, so no protected power is drawn for a draw that buys
	// no circulation, and the whole cooling demand stays unmet naming the
	// missing circulation.
	{
		auto world = dependencyRun(2.0, 0.0);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].circulationFraction, 0.0),
			"a zero-rated-flow loop reported circulation it cannot move");
		require(closeTo(snap.power.loads[0].suppliedKW, 1.0),
			"the discretionary load did not take the pool the unprotectable zero-flow pump left");
		require(findProtectedService(snap, "loop", "critical-cooling-circulation") == nullptr,
			"a zero-flow loop's circulation draw was protected although it buys no circulation");
		auto const* coolingService = findProtectedService(snap, "chiller",
			"critical-cooling");
		require(coolingService != nullptr
			&& closeTo(coolingService->requestedKW, 2.0)
			&& closeTo(coolingService->suppliedKW, 0.0)
			&& closeTo(coolingService->unmetKW, 2.0),
			"zero-flow cooling was not explicit unmet critical service");
		require(coolingService != nullptr
			&& coolingService->reason.find("circulation") != std::string::npos,
			"the zero-flow unmet cooling does not name the circulation dependency");
		require(closeTo(snap.chillers[0].coolingKW, 0.0),
			"cooling was delivered on a zero-flow loop");
	}
	// A full outage of the cold loop leaves no deliverable capacity and
	// no circulation dependency to fund: the chiller draws no protected
	// power, delivers no cooling, and its own service account names the
	// missing circulation as the refusal.
	{
		auto world = dependencyRun(2.0, 1.0, true);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 0.0),
			"the outaged loop's pump drew power");
		require(findProtectedService(snap, "loop", "critical-cooling-circulation") == nullptr
			&& findProtectedService(snap, "chiller", "critical-cooling") == nullptr,
			"a fully outaged chiller bundle presented protected demand");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the outaged-loop chiller drew power or delivered cooling");
		require(snap.chillers[0].blockReason.find("circulation") != std::string::npos,
			"the outaged-loop chiller refusal does not name the missing circulation");
	}
	// Determinism: two identical runs of the tight-pool dependency rig
	// agree on every protected and delivered scalar.
	{
		auto const first = dependencyRun(2.0, 1.0);
		auto const second = dependencyRun(2.0, 1.0);
		auto const& a = first->getStationSystemsSnapshot();
		auto const& b = second->getStationSystemsSnapshot();
		require(closeTo(a.power.protectedDemandKW, b.power.protectedDemandKW)
			&& closeTo(a.power.protectedSuppliedKW, b.power.protectedSuppliedKW)
			&& closeTo(a.power.protectedUnmetKW, b.power.protectedUnmetKW)
			&& closeTo(a.coolantLoops[0].pumpSuppliedKW, b.coolantLoops[0].pumpSuppliedKW)
			&& closeTo(a.chillers[0].coolingKW, b.chillers[0].coolingKW)
			&& closeTo(a.chillers[0].powerSuppliedKW, b.chillers[0].powerSuppliedKW),
			"the protected chiller dependency allocation was not deterministic");
	}

	// Scenario 5c: protected chiller credit reconciliation (#580).
	// The protected cooling credit is sized from the loop's tick-start
	// heat, but the loop's implicit exchange with its reservoir is only
	// solved later in the tick, so the credit is a reservation, not
	// spent work: every watt the real duty cannot use must return to
	// the source it came from, and the battery ledger, the generation
	// surplus, the competing consumers and the protected requested/
	// supplied/unmet account must all agree on the work that actually
	// moved. A critical chiller on a critical loop whose cold reservoir
	// drags the extractable heat to zero (or near it) must strand
	// nothing on the bus.
	auto creditRun = [&](double solarKW, double conductanceKWPerK,
		bool withBattery, bool withCompetitor)
	{
		auto world = std::make_unique<core::World>("ChillerCredit", 2, 2);
		world->pauseSimulation();
		world->finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", -20.0),
			reservoir("hot", "low", 20.0) };
		auto loop = normalLoop("loop", 1.0, 1.0);
		loop.criticalService = true;
		loop.exchangerConductanceKWPerK = conductanceKWPerK;
		settings.coolantLoops = { loop };
		settings.chillers = { criticalChiller("chiller", "loop") };
		if (solarKW > 0.0) settings.solarArrays = { solarArray(solarKW) };
		if (withBattery)
		{
			core::BatterySettings bank;
			bank.id = "bank";
			bank.nameplateKWh = 10.0;
			bank.usableKWh = 9.0;
			bank.startingStoredKWh = 5.0;
			bank.maxChargeKW = 5.0;
			bank.maxDischargeKW = 10.0;
			bank.dischargeEfficiency = 0.9;
			settings.batteries = { bank };
		}
		if (withCompetitor)
			settings.equipmentLoads = { discretionaryLoad("experiment", 1.0, 1.0) };
		configure(*world, settings, "chiller credit rig");
		world->resumeSimulation();
		runOrDiagnose(*world, 1, "chiller credit run");
		return world;
	};
	auto const tickHours = static_cast<double>(core::World::getFixedTimestep()) / 3600.0;
	// The exchange leaves the cold loop no extractable heat: the whole
	// 2 kW work reservation has no consumer, so it returns to the bus
	// as surplus, the protected account reports only the pump as
	// supplied, and the competing discretionary load never saw those
	// watts spent.
	{
		auto world = creditRun(3.0, 1.0e6, false, true);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0),
			"the protected pump did not run in the credit reconciliation rig");
		require(closeTo(snap.chillers[0].coolingKW, 0.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the exchange-blocked chiller consumed work it could not convert");
		require(closeTo(snap.power.protectedDemandKW, 3.0)
			&& closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.protectedUnmetKW, 2.0),
			"the protected account still counts the unused reservation as supplied work");
		require(snap.power.protectedUnmet,
			"the reconciled unmet cooling did not raise the protected alarm");
		auto const* cooling = findProtectedService(snap, "chiller", "critical-cooling");
		require(cooling != nullptr
			&& closeTo(cooling->requestedKW, 2.0)
			&& closeTo(cooling->suppliedKW, 0.0)
			&& closeTo(cooling->unmetKW, 2.0)
			&& cooling->reason.find("returned to the pool") != std::string::npos,
			"the reconciled cooling service does not expose the returned credit");
		// Whole-bus energy balance: solar in = pump + chiller work +
		// surplus; the unused credit is back on the bus, not stranded.
		require(closeTo(snap.power.surplusKW, 2.0)
			&& closeTo(snap.power.availableGenerationKW,
				snap.coolantLoops[0].pumpSuppliedKW
				+ snap.chillers[0].powerSuppliedKW + snap.power.surplusKW),
			"the whole-bus energy balance does not close on the returned credit");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the competing discretionary load received power in the reconciliation rig");
		require(sawEvent(*world, "Protected critical-cooling 'chiller' unmet"),
			"the reconciled unused cooling was not announced as unmet");
	}
	// The exchange leaves a small but nonzero duty: the credit funds the
	// duty that really moves, only the remainder returns, and every
	// ledger agrees on that one number.
	{
		auto world = creditRun(3.0, 36.0, false, true);
		auto const& snap = world->getStationSystemsSnapshot();
		auto const work = snap.chillers[0].powerSuppliedKW;
		require(work > 0.5 && work < 2.0,
			"the exchange-limited duty is not a smaller-but-nonzero share of the credit");
		require(closeTo(snap.chillers[0].coolingKW, work),
			"the cooling delivered does not match the work actually powered");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0 + work)
			&& closeTo(snap.power.protectedUnmetKW, 2.0 - work),
			"the protected account does not agree with the work that actually moved");
		require(closeTo(snap.power.surplusKW, 2.0 - work)
			&& closeTo(snap.power.availableGenerationKW,
				snap.coolantLoops[0].pumpSuppliedKW + work + snap.power.surplusKW),
			"the whole-bus energy balance does not close on the partially used credit");
		require(closeTo(snap.power.loads[0].suppliedKW, 0.0),
			"the competing discretionary load took power ahead of the protected bundle");
	}
	// A battery-funded credit whose duty is zero: the refund goes back
	// into storage, not just off the discharge report - the battery paid
	// only for the pump, the work it never supplied never left storage.
	{
		auto world = creditRun(0.0, 1.0e6, true, false);
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.coolantLoops[0].pumpSuppliedKW, 1.0)
			&& closeTo(snap.chillers[0].powerSuppliedKW, 0.0),
			"the battery-funded reconciliation rig did not run as authored");
		auto const& bank = snap.power.batteries[0];
		require(closeTo(bank.lastDischargeKW, 1.0),
			"the refunded battery credit still counted as discharge this tick");
		require(closeTo(bank.storedKWh, 5.0 - 1.0 * tickHours / 0.9),
			"the refunded battery energy did not return to storage");
		require(closeTo(bank.dischargedKWh, 1.0 * tickHours / 0.9)
			&& closeTo(bank.deliveredKWh, 1.0 * tickHours),
			"the battery energy ledger does not close on the reconciled credit");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.surplusKW, 0.0),
			"the protected and surplus accounts disagree with the battery-funded reconciliation");
	}
	// A battery-funded credit with a smaller-but-nonzero duty: the
	// battery discharges exactly the pump plus the work that moved, and
	// the unused remainder of its draw is back in storage.
	{
		auto world = creditRun(0.0, 36.0, true, false);
		auto const& snap = world->getStationSystemsSnapshot();
		auto const work = snap.chillers[0].powerSuppliedKW;
		require(work > 0.5 && work < 2.0,
			"the battery-funded exchange-limited duty is not smaller but nonzero");
		auto const& bank = snap.power.batteries[0];
		require(closeTo(bank.lastDischargeKW, 1.0 + work),
			"the battery still discharged the unused part of the protected credit");
		require(closeTo(bank.storedKWh, 5.0 - (1.0 + work) * tickHours / 0.9),
			"the unused battery-funded credit did not return to storage");
		require(closeTo(snap.power.protectedSuppliedKW, 1.0 + work)
			&& closeTo(snap.power.protectedUnmetKW, 2.0 - work),
			"the protected account disagrees with the battery-funded actual work");
		require(closeTo(snap.power.surplusKW, 0.0),
			"a battery-funded reconciliation reported surplus it does not have");
	}
	// Determinism: two identical reconciliation runs agree on every
	// protected, delivered and battery scalar.
	{
		auto const first = creditRun(0.0, 36.0, true, true);
		auto const second = creditRun(0.0, 36.0, true, true);
		auto const& a = first->getStationSystemsSnapshot();
		auto const& b = second->getStationSystemsSnapshot();
		require(closeTo(a.power.protectedSuppliedKW, b.power.protectedSuppliedKW)
			&& closeTo(a.chillers[0].powerSuppliedKW, b.chillers[0].powerSuppliedKW)
			&& closeTo(a.power.batteries[0].storedKWh, b.power.batteries[0].storedKWh)
			&& closeTo(a.power.surplusKW, b.power.surplusKW),
			"the protected chiller credit reconciliation was not deterministic");
	}
	// Host/Lua seam: the bundled sandbox refunds the unused credit in
	// its own battery ledger and protected account, and both agree with
	// the host snapshot for the same tick.
	{
		SourceScope scope;
		auto const bundled = core::bundledStationSystemsSource();
		core::setStationSystemsSourceLoader([bundled] {
			return "local factory = (function()\n" + bundled + "\nend)()\n"
				"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
				"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, cropPopulations, algaeCultures, outages, dispatchMode)\n"
				"local original = model.advance\n"
				"function model.advance(self, world_tick, fixed_seconds)\n"
				"local result = original(self, world_tick, fixed_seconds)\n"
				"local chiller = self.chillers[1]\n"
				"local bank = self.batteries[1]\n"
				"local hours = fixed_seconds / 3600.0\n"
				"if world_tick == 1 then\n"
				"assert(math.abs(chiller.lastPowerSupplied) < 1e-12, 'sandbox chiller spent credit it could not convert')\n"
				"assert(math.abs(self.protectedSupplied - 1.0) < 1e-12, 'sandbox protected account not reconciled to actual work')\n"
				"assert(math.abs(self.protectedUnmet - 2.0) < 1e-12, 'sandbox protected unmet not reconciled')\n"
				"assert(math.abs(bank.last_discharge - 1.0) < 1e-12, 'sandbox battery still discharged the refunded credit')\n"
				"assert(math.abs(bank.stored - (5.0 - 1.0 * hours / 0.9)) < 1e-12, 'sandbox battery energy not returned to storage')\n"
				"else\n"
				"assert(math.abs(self.protectedSupplied - (1.0 + chiller.lastPowerSupplied)) < 1e-12, 'sandbox protected account disagrees with the actual work')\n"
				"assert(math.abs(bank.last_discharge - (1.0 + chiller.lastPowerSupplied)) < 1e-12, 'sandbox battery discharge disagrees with the actual work')\n"
				"end\n"
				"return result\n"
				"end\n"
				"return model\n"
				"end";
		});
		auto world = creditRun(0.0, 1.0e6, true, false);
		// The host ledger over the same tick agrees with what the sandbox
		// asserted for its own battery and protected accounts.
		auto const& snap = world->getStationSystemsSnapshot();
		require(closeTo(snap.power.protectedSuppliedKW, 1.0)
			&& closeTo(snap.power.batteries[0].lastDischargeKW, 1.0),
			"the host and sandbox chiller credit reconciliation disagree");
	}

	// Scenario 6: authored persistence. The protected flags round-trip
	// with the World document, and a pre-92 document carrying them is
	// refused rather than silently dropping the protection.
	{
		core::World world("ProtPersist", 2, 2);
		world.pauseSimulation();
		world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0),
			reservoir("hot", "low", 20.0) };
		auto loop = hazardousLoop("hazLoop", 1.0, 0.0, 20.0);
		loop.criticalService = true;
		settings.coolantLoops = { loop };
		core::ChillerSettings chiller;
		chiller.id = "chiller";
		chiller.thermalClass = "low";
		chiller.coldLoopId = "hazLoop";
		chiller.condenserReservoirId = "hot";
		chiller.ratedCoolingCapacityKW = 5.0;
		chiller.cop = 3.0;
		chiller.auxiliaryPowerKW = 0.2;
		chiller.chilledSetpointC = 5.0;
		chiller.maxCondensingTemperatureC = 45.0;
		chiller.criticalService = true;
		settings.chillers = { chiller };
		settings.solarArrays = { solarArray(10.0) };
		configure(world, settings, "protected persistence rig");
		auto const text = yaml(world);
		require(text.find("criticalService: true") != std::string::npos,
			"the protected service flag was not saved");
		core::SerializationWorkData data;
		auto reader = core::YamlSerializer::fromString(text);
		reader->deserialize();
		core::World loaded("ProtPersistLoaded", 2, 2);
		loaded.deserialize(*reader, data);
		require(loaded.getStationSystemsSettings() == world.getStationSystemsSettings(),
			"protected service flags did not round-trip");
		require(loaded.getStationSystemsSnapshot().worldTick == 0,
			"the loaded world restored live station state instead of the authored start");
		auto stale = text;
		require(stale.find("version: 99") != std::string::npos,
			"the fixture document carries no version line");
		stale.replace(stale.find("version: 99"), std::string("version: 99").size(),
			"version: 91");
		bool refused = false;
		try
		{
			auto staleReader = core::YamlSerializer::fromString(stale);
			staleReader->deserialize();
			core::World staleWorld("ProtPersistStale", 2, 2);
			staleWorld.deserialize(*staleReader, data);
		}
		catch (std::exception const& error)
		{
			refused = true;
			require(std::string(error.what()).find("schema 92") != std::string::npos,
				"the pre-92 protected-flag refusal does not name the required schema");
		}
		require(refused, "a pre-92 document carrying protected service flags was silently accepted");
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
	// #587: one consistent fixed-tick window policy. A never-started
	// job whose declared window has already closed is recorded missed,
	// not back-filled - the Idle path applies the same deadline the
	// Blocked path always enforced. A sub-tick window that no fixed
	// tick can open is missed the same way, while a window a tick can
	// reach mid-window still starts.
	{
		// The probe shape: a one-off with first occurrence 0.001 s
		// and duration 0.001 s. Its whole window [0.001, 0.002] has
		// closed before the first fixed tick at ~0.016667 s, with
		// every prerequisite in stock - the closed window alone is
		// what keeps it from ever running.
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
		require(snap.maintenanceJobs[0].occurrencesStarted == 0
			&& snap.maintenanceJobs[0].occurrencesCompleted == 0
			&& snap.maintenanceJobs[0].occurrencesMissed == 1
			&& snap.maintenanceJobs[0].exhausted
			&& snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Idle
			&& !snap.maintenanceJobs[0].isolated,
			"the closed one-off window was back-filled instead of recorded missed");
		require(approx(snap.stores[2].currentQualified, 8.0)
			&& approx(snap.stores[3].currentQualified, 0.0),
			"the missed job reserved or spent material it never worked with");
		require(sawEvent(world, "missed its window"),
			"the missed occurrence was not announced");
		// The missed one-off never wakes up: a minute of later ticks
		// changes nothing - no late back-fill start.
		require(world.advanceTicks(60), "expired-window run failed past the first second");
		auto const& later = world.getStationSystemsSnapshot();
		require(later.maintenanceJobs[0].occurrencesStarted == 0
			&& later.maintenanceJobs[0].occurrencesMissed == 1
			&& !later.maintenanceJobs[0].isolated
			&& approx(later.power.generators[0].availabilityFactor, 1.0),
			"a closed one-off window was replayed as startable later");
		// Determinism: a fresh World with the same settings reproduces
		// the missed counters and the maintenance event history.
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
	// #587: a recurring job whose every window is sub-tick misses each
	// window as it closes and never starts, while a job whose window a
	// tick can reach mid-window still starts inside it.
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
		// closed unstarted; the next opens at 10.001.
		require(world.advanceTicks(301), "sub-tick-window run failed into window 1");
		auto const& snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the sub-tick-window run was not healthy");
		require(snap.maintenanceJobs[0].occurrencesMissed == 1
			&& snap.maintenanceJobs[0].occurrencesStarted == 0
			&& !snap.maintenanceJobs[0].exhausted
			&& snap.maintenanceJobs[0].phase == core::StationMaintenancePhase::Idle
			&& !snap.maintenanceJobs[0].isolated,
			"a closed recurring window was back-filled instead of recorded missed");
		require(approx(snap.stores[2].currentQualified, 8.0)
			&& approx(snap.stores[3].currentQualified, 0.0),
			"the missed recurring job reserved or spent material anyway");
		// t = ~10.0167 s: the second window [10.001, 10.002] has
		// closed too - one missed count per closed window.
		require(world.advanceTicks(300), "sub-tick-window run failed into window 2");
		auto const& next = world.getStationSystemsSnapshot();
		require(next.maintenanceJobs[0].occurrencesMissed == 2
			&& next.maintenanceJobs[0].occurrencesStarted == 0,
			"the recurring closed-window catch-up did not record every missed window");
		// Determinism: the same recurring miss replays identically.
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

// #417: service recovery staging. A purely discretionary load that
// falls below its minimum operable supply is held Shed until the declared
// dwell, hysteresis, cooling and firm-source gate reopens it, so a
// capacity that flickers across the threshold cannot toggle the
// equipment on and off. A restart draws its declared startup surge on
// top of normal demand, bounded by the actual pool; the pending restart
// headroom is held out of battery charging so reserves refill only from
// surplus the recovery does not need. All scenarios drive the existing
// World tick/snapshot seam and read the recovery stage off the snapshot.
void recovery(smoke::Context const& context)
{
	auto const closeTo = [](double actual, double expected, double tolerance = 1e-6)
	{ return std::abs(actual - expected) <= tolerance; };
	auto const constantProfile = [](double level)
	{ core::ProfileSettings p; p.constantLevel = level; return p; };
	auto const stepProfile = [](double period, std::vector<std::pair<double, double>> segments)
	{
		core::ProfileSettings p;
		p.periodSeconds = period;
		for (auto const& segment : segments) p.segments.push_back({segment.first, segment.second});
		return p;
	};
	auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
	{
		if (world.advanceTicks(ticks)) return;
		auto const diagnostics = world.getStationSystemsDiagnostics();
		std::fprintf(stderr, "%s failed: %s\n", what,
			diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
		throw std::runtime_error(std::string(what) + " run failed");
	};
	auto const massStore = [](std::string id, double capacity, double starting)
	{
		core::ResourceStoreSettings s; s.id = std::move(id); s.unit = "kg";
		s.capacity = capacity; s.startingQualified = starting; return s;
	};
	auto const configure = [](core::World& world, core::StationSystemsSettings settings, char const* what)
	{
		std::string diagnostic; settings.enabled = true; settings.observationIntervalSeconds = 1.0f;
		require(world.configureStationSystems(settings, false, &diagnostic),
			(std::string(what) + " refused: " + diagnostic).c_str());
	};
	auto const recoveryLoad = [&](std::string id, double demandKW, double maxCurtailment,
		double dwell, double hysteresis, double surgeKW, double surgeSeconds, double heatKW)
	{
		core::EquipmentLoadSettings s;
		s.id = std::move(id);
		s.electricalDemandKW = demandKW;
		s.operatingProfile = constantProfile(1.0);
		s.heatOutputKW = heatKW;
		s.thermalClass = "low";
		s.coolingDisposition = core::StationCoolingDisposition::Exported;
		s.criticalFloorKW = 0.0;
		s.priority = 10;
		s.maxCurtailment = maxCurtailment;
		s.recoveryDwellSeconds = dwell;
		s.recoveryHysteresisFraction = hysteresis;
		s.restartSurgePowerKW = surgeKW;
		s.restartSurgeSeconds = surgeSeconds;
		return s;
	};
	auto const solar = [](double capacity, core::ProfileSettings illum)
	{
		core::SolarArraySettings s; s.id = "field"; s.capacityKW = capacity; s.illumination = illum; return s;
	};
	auto const reservoir = [](std::string id, std::string thermalClass, double startingC, double maxC)
	{
		core::ThermalReservoirSettings s; s.id = std::move(id); s.thermalClass = std::move(thermalClass);
		s.heatCapacityKJPerK = 10000.0; s.startingTemperatureC = startingC;
		s.minTemperatureC = -50.0; s.maxTemperatureC = maxC; return s;
	};
	auto const battery = [](std::string id, double nameplate, double usable, double starting)
	{
		core::BatterySettings s; s.id = std::move(id); s.nameplateKWh = nameplate; s.usableKWh = usable;
		s.startingStoredKWh = starting; s.maxChargeKW = 100.0; s.maxDischargeKW = 100.0; return s;
	};
	auto loadSnap = [](core::World& world, std::string const& id) -> core::StationEquipmentLoadSnapshot
	{
		for (auto const& l : world.getStationSystemsSnapshot().power.loads)
			if (l.id == id) return l;
		throw std::runtime_error("load not found: " + id);
	};

	// Scenario A: shed on shortage, hold through the dwell even after the
	// capacity returns, restart once dwell + hysteresis clear, and repeat
	// the whole cycle on the next low-sun half without drift.
	{
		core::World world("RecA", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// capacity 20 kW: level 0.2 -> 4 kW (below the 10 kW min), level 0.75 -> 15 kW (above the 12 kW restart threshold).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.2}, {50.0, 0.75}})) };
		settings.equipmentLoads = { recoveryLoad("rig", 10.0, 0.0, 5.0, 0.2, 0.0, 0.0, 0.0) };
		configure(world, settings, "recovery A rig");
		world.resumeSimulation();
		runOrDiagnose(world, 1500, "recovery A low sun"); // t = 25s
		{
			auto const& l = loadSnap(world, "rig");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
				"the load did not shed under the low-sun shortage");
			require(l.suppliedKW <= 1e-9, "a shed load drew power");
			require(l.recoveryHoldReason.find("hysteresis") != std::string::npos,
				"the shed load was not held by the hysteresis headroom");
		}
		runOrDiagnose(world, 1560, "recovery A high sun"); // t = 51s
		{
			auto const& l = loadSnap(world, "rig");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
				"the load did not restart once the dwell and hysteresis cleared");
			require(closeTo(l.suppliedKW, 10.0), "the restarted load is not fully supplied");
		}
		runOrDiagnose(world, 4440, "recovery A second low sun"); // t = 125s
		require(loadSnap(world, "rig").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the load did not shed again on the second low-sun half (repeated incident)");
		runOrDiagnose(world, 1560, "recovery A second high sun"); // t = 151s
		require(loadSnap(world, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load did not recover again on the second high-sun half (repeated incident)");
	}

	// Scenario B: a declared startup surge is drawn on top of normal
	// demand through a Restoring stage, then the load settles to Active
	// once the surge duration has elapsed.
	{
		core::World world("RecB", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.15}, {50.0, 0.75}})) };
		// demand 10, min 10, dwell 0, hysteresis 0.1 (restart needs 11), surge 5 kW for 2s.
		settings.equipmentLoads = { recoveryLoad("surger", 10.0, 0.0, 0.0, 0.1, 5.0, 2.0, 0.0) };
		configure(world, settings, "recovery B rig");
		world.resumeSimulation();
		runOrDiagnose(world, 1500, "recovery B low sun"); // t = 25s
		require(loadSnap(world, "surger").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the surging load did not shed under the low-sun shortage");
		runOrDiagnose(world, 1560, "recovery B restart"); // t = 51s, mid-Restoring
		{
			auto const& l = loadSnap(world, "surger");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Restoring,
				"the load is not in the Restoring stage mid-surge");
			require(closeTo(l.recoverySurgeKW, 5.0), "the startup surge was not drawn at the declared rate");
			require(closeTo(l.suppliedKW, 10.0), "the Restoring load's normal demand was not supplied");
			require(l.recoverySecondsRemaining > 0.0 && l.recoverySecondsRemaining <= 2.0,
				"the surge countdown is not tracking the declared surge duration");
		}
		runOrDiagnose(world, 180, "recovery B settle"); // t = 54s, surge done
		{
			auto const& l = loadSnap(world, "surger");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
				"the load did not settle to Active after the surge elapsed");
			require(closeTo(l.recoverySurgeKW, 0.0), "surge still drawn after the load went Active");
		}
	}

	// Scenario C: a heat-producing load will not restart on a thin firm-
	// generation supply - the firm-source prerequisite holds it Shed even
	// though the hysteresis headroom is met, because the restart plus its
	// surge exceeds what firm generation can carry.
	{
		core::World world("RecC", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// level 0.4 -> 8 kW (shed), level 0.6 -> 12 kW (above the 11 kW hysteresis threshold but below the 15 kW restart+surge).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.4}, {50.0, 0.6}})) };
		// heat-producing, dwell 0, hysteresis 0.1 (threshold 11), surge 5 -> restartNeed 15.
		settings.equipmentLoads = { recoveryLoad("kiln", 10.0, 0.0, 0.0, 0.1, 5.0, 1.0, 5.0) };
		configure(world, settings, "recovery C rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery C"); // t = 51s
		auto const& l = loadSnap(world, "kiln");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the heat load restarted without enough firm generation");
		require(l.recoveryHoldReason.find("firm generation") != std::string::npos,
			"the heat load was not held by the firm-source prerequisite");
	}

	// Scenario D: a heat-producing load with a heat sink at or above its
	// maximum service temperature cannot restart - the cooling
	// qualification holds it Shed even with ample firm generation.
	{
		core::World world("RecD", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.thermalReservoirs = { reservoir("hotSink", "low", 40.0, 40.0) }; // at its maximum service temp
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.15}, {50.0, 0.6}})) };
		auto kiln = recoveryLoad("kiln2", 10.0, 0.0, 0.0, 0.0, 0.0, 0.0, 5.0);
		kiln.heatSinkId = "hotSink";
		settings.equipmentLoads = { kiln };
		configure(world, settings, "recovery D rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery D"); // t = 51s
		auto const& l = loadSnap(world, "kiln2");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the heat load restarted onto a sink at or above its maximum service temperature");
		require(l.recoveryHoldReason.find("heat sink") != std::string::npos,
			"the heat load was not held by the cooling qualification");
	}

	// Scenario E: while a load is pending restart, the pending restart
	// headroom is held out of battery charging, so a depleted reserve
	// refills only from the surplus the recovery does not need and never
	// overloads the waiting restart.
	{
		core::World world("RecE", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// level 0.25 -> 5 kW (shed), level 0.6 -> 12 kW (past dwell but below the 15 kW hysteresis threshold).
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.25}, {50.0, 0.6}})) };
		settings.batteries = { battery("bank", 100.0, 80.0, 10.0) }; // depleted, wants to charge
		// demand 10, dwell 5, hysteresis 0.5 (threshold 15), no surge.
		settings.equipmentLoads = { recoveryLoad("waiter", 10.0, 0.0, 5.0, 0.5, 0.0, 0.0, 0.0) };
		configure(world, settings, "recovery E rig");
		world.resumeSimulation();
		runOrDiagnose(world, 3060, "recovery E"); // t = 51s
		auto const& snap = world.getStationSystemsSnapshot();
		require(loadSnap(world, "waiter").recoveryStage == core::StationLoadRecoveryStage::Shed,
			"the waiting load restarted although the pool was below its hysteresis threshold");
		require(snap.power.recoveryPending,
			"the recovery-pending flag did not raise while a load was Shed");
		// Surplus is 12 kW; the 10 kW pending restart headroom is held out,
		// so the battery charges at most 2 kW, not the full surplus.
		require(snap.power.batteries[0].lastChargeKW <= 2.0 + 1e-6,
			"battery charging was not capped by the pending restart headroom");
		require(snap.power.batteries[0].lastChargeKW > 0.0,
			"the depleted battery did not charge at all from the leftover surplus");
	}

	// Scenario F: the authored recovery rules survive the World document
	// round trip, and the reopened World reproduces the same recovery
	// behaviour tick-for-tick - persistence and reset stay compatible.
	{
		auto const path = context.temporaryRoot() / "recovery-roundtrip.world.yaml";
		core::World original("RecF", 2, 2);
		original.pauseSimulation(); original.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(20.0, stepProfile(100.0, {{0.0, 0.2}, {50.0, 0.75}})) };
		settings.equipmentLoads = { recoveryLoad("rig", 10.0, 0.0, 5.0, 0.2, 3.0, 1.5, 0.0) };
		configure(original, settings, "recovery F rig");
		original.resumeSimulation();
		runOrDiagnose(original, 3180, "recovery F original"); // t = 53s, surge done, recovered Active
		require(loadSnap(original, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the recovery-F original did not reach the recovered state before saving");
		original.saveTo(path.string());
		auto const reopened = core::loadWorldDocument(path);
		require(reopened != nullptr, "the recovery document failed to reopen");
		require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
			"the authored recovery rules did not survive the document round trip");
		auto const reloaded = reopened->getStationSystemsSettings().equipmentLoads.at(0);
		require(closeTo(reloaded.recoveryDwellSeconds, 5.0)
			&& closeTo(reloaded.recoveryHysteresisFraction, 0.2)
			&& closeTo(reloaded.restartSurgePowerKW, 3.0)
			&& closeTo(reloaded.restartSurgeSeconds, 1.5),
			"the recovery settings did not round-trip with their declared values");
		reopened->resumeSimulation();
		runOrDiagnose(*reopened, 3180, "recovery F reopened"); // t = 53s
		require(loadSnap(*reopened, "rig").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the reopened World did not reproduce the same recovery stage as the original");
	}

	// Scenario G (#588): a restart whose surge cannot be paid never runs
	// the equipment. A non-heat-producing 1 kW load with a required 1 kW
	// surge, zero dwell and zero hysteresis, and a battery that can
	// discharge nothing, sits behind exactly 1 kW of solar: the gate
	// releases it every tick because the pool covers its demand, but the
	// allocation pass refuses the restart because the surge is
	// unfundable. The load must never publish supplied service while it
	// is held Shed, must never consume a watt, and must never leave a
	// phantom battery discharge shortfall behind from a surge draw that
	// was never committed.
	{
		core::World world("RecG", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(1.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		settings.equipmentLoads = { recoveryLoad("starter", 1.0, 0.0, 0.0, 0.0, 1.0, 0.02, 0.0) };
		core::BatterySettings dead;
		dead.id = "dead"; dead.nameplateKWh = 1.0; dead.usableKWh = 1.0;
		dead.startingStoredKWh = 0.5; dead.maxDischargeKW = 0.0; dead.maxChargeKW = 0.0;
		settings.batteries = { dead };
		configure(world, settings, "recovery G rig");
		world.resumeSimulation();
		for (uint64_t tick = 1; tick <= 1500; ++tick) // t = 25s, past the release point
		{
			if (!world.advanceTick())
			{
				auto const diagnostics = world.getStationSystemsDiagnostics();
				throw std::runtime_error(std::string("recovery G tick failed: ")
					+ (diagnostics.empty() ? "(none)" : diagnostics.back().message));
			}
			auto const& l = loadSnap(world, "starter");
			require(!(l.suppliedKW > 1e-9
					&& l.recoveryStage == core::StationLoadRecoveryStage::Shed),
				"a failed restart published supplied service while returning to Shed");
			require(l.suppliedKW <= 1e-9,
				"an unfundable restart drew normal power it could not pair with its surge");
			require(l.recoverySurgeKW <= 1e-9,
				"an unfundable restart drew surge power");
			require(world.getStationSystemsSnapshot().power.batteries[0].lastDischargeShortfallKW
				<= 1e-9,
				"the refused surge left a battery discharge shortfall it never committed");
		}
	}

	// Scenario H (#588): a funded surge is real service. The whole
	// operable demand plus the surge is paid from generation and the
	// battery together, the supplied total counts the surge beside the
	// normal service, and the total surge energy never exceeds the
	// authored power x seconds budget - the last tick of a surge that
	// does not divide evenly by the timestep pays only what is left.
	{
		core::World world("RecH", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(3.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("pumper", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		configure(world, settings, "recovery H rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		double const authoredSurgeEnergy = 1.0 * 0.05;
		bool sawSurge = false;
		bool sawActive = false;
		for (uint64_t tick = 1; tick <= 1200; ++tick) // t = 20s
		{
			runOrDiagnose(world, 1, "recovery H");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "pumper");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the startup surge");
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
			if (l.recoverySurgeKW > 1e-9) sawSurge = true;
			if (l.recoveryStage == core::StationLoadRecoveryStage::Active) sawActive = true;
		}
		require(sawSurge, "the funded restart never drew its surge");
		require(sawActive, "the funded restart never settled to Active");
		require(surgeEnergy <= authoredSurgeEnergy + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario I (#588): supply changes between the gate's release and
	// the restart's execution. The gate releases on a full pool; the
	// sun drops before the next tick allocates, and the allocation pass
	// refuses the restart before any service commits. The failed attempt
	// restarts the declared dwell: the load cannot try again until the
	// full dwell has elapsed behind the restored supply.
	{
		core::World world("RecI", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		// Step profile: dark until t=10, full sun at t=10 (the gate
		// releases), a collapse one tick later (the restart is refused
		// before execution), and restored sun at t=17 (restart once the
		// re-armed dwell has elapsed).
		settings.solarArrays = { solar(3.0, stepProfile(200.0,
			{{0.0, 0.0}, {9.999, 0.0}, {10.0, 1.0}, {10.001, 0.05},
				{17.0, 0.05}, {17.001, 1.0}})) };
		// demand 1, min 1, dwell 5, hysteresis 0, surge 1 kW for 0.5s.
		settings.equipmentLoads = { recoveryLoad("thwarted", 1.0, 0.0, 5.0, 0.0, 1.0, 0.5, 0.0) };
		configure(world, settings, "recovery I rig");
		world.resumeSimulation();
		runOrDiagnose(world, 600, "recovery I to release"); // t = 10s, gate releases
		require(loadSnap(world, "thwarted").recoveryStage == core::StationLoadRecoveryStage::Restoring,
			"the load was not released to Restoring on the full pool");
		runOrDiagnose(world, 1, "recovery I collapse"); // t = 10.0167s, supply collapsed
		{
			auto const& l = loadSnap(world, "thwarted");
			require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
				"the restart ran although the supply collapsed before execution");
			require(l.suppliedKW <= 1e-9,
				"the collapsed restart consumed operating power");
			require(l.recoverySurgeKW <= 1e-9,
				"the collapsed restart drew surge power");
			// The failed attempt re-armed the full five-second dwell.
			require(l.recoverySecondsRemaining > 4.9,
				"the failed restart did not restart the declared dwell");
		}
		runOrDiagnose(world, 600, "recovery I restored"); // t = 20s, sun restored, dwell elapsed
		require(loadSnap(world, "thwarted").recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load did not restart once the supply and the full dwell were behind it");
	}

	// Scenario J (#588): a funded restart that cannot have its full
	// normal demand keeps the surge whole. The claim is the normal demand
	// plus the surge; the surge is a hard requirement, so when the pool
	// is tight the normal service is curtailed to its minimum operable
	// level and the surge still draws in full - the two are funded as
	// one unit and never exceed the pool.
	{
		core::World world("RecJ", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(5.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 4, maxCurtailment 0.5 (min 2), dwell 0, hysteresis 0,
		// surge 2 kW for 0.05s. The pool of 5 kW funds normal 3 + surge 2.
		settings.equipmentLoads = { recoveryLoad("drill", 4.0, 0.5, 0.0, 0.0, 2.0, 0.05, 0.0) };
		configure(world, settings, "recovery J rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawCurtailedSurge = false;
		bool sawActive = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s
		{
			runOrDiagnose(world, 1, "recovery J");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "drill");
			require(l.suppliedKW + l.recoverySurgeKW <= 5.0 + 1e-9,
				"the restart claim drew more than the pool could carry");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the startup surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				require(l.suppliedKW >= 2.0 - 1e-9,
					"the funded restart curtailed normal service below its minimum operable level");
				require(l.suppliedKW <= 3.0 + 1e-6,
					"the funded restart took normal service the surge still needed");
				sawCurtailedSurge = true;
			}
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
			if (l.recoveryStage == core::StationLoadRecoveryStage::Active) sawActive = true;
		}
		require(sawCurtailedSurge, "the restart never ran with a curtailed normal and a full surge");
		require(sawActive, "the restart never settled to Active");
		require(surgeEnergy <= 2.0 * 0.05 + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario K (#591): a sub-tick surge is prorated, not rounded up to
	// full ticks, and the battery funding it loses only the authored
	// energy. A 1 kW surge authored for 0.02 s is barely longer than one
	// fixed tick (1/60 s): the old code always bought the full 1 kW and
	// only then subtracted a full dt, drawing 0.0333 kJ against the
	// 0.02 kJ contract - a 66.7% overdraw the battery actually paid,
	// and far worse below a tick. The last tick of a surge now pays only
	// the energy left in the declared budget: the surge power is
	// prorated by window/tick, the total delivered energy never exceeds
	// power x seconds, and the battery's discharge ledger matches it
	// exactly. A 0.01 s surge - shorter than one tick - must be paid in
	// a single prorated tick, and every trace replays identically after
	// a reset.
	{
		struct SurgeRun
		{
			std::vector<double> trace;
			double energy{0.0};
			int surgeTicks{0};
		};
		auto runSurgeRig = [&](core::World& world, double surgeSeconds)
		{
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.stores = { massStore("ballast", 10.0, 1.0) };
			// 2 kW of solar covers the load's own 2 kW demand exactly once
			// the sun rises at t=5; the startup surge on top of it has to
			// come out of the battery.
			settings.solarArrays = { solar(2.0, stepProfile(200.0, {{0.0, 0.0}, {5.0, 1.0}})) };
			settings.equipmentLoads = { recoveryLoad("surger", 2.0, 0.0, 0.0, 0.0, 1.0, surgeSeconds, 0.0) };
			core::BatterySettings cell;
			cell.id = "cell"; cell.nameplateKWh = 1.0; cell.usableKWh = 1.0;
			cell.startingStoredKWh = 0.5; cell.maxDischargeKW = 1.0; cell.maxChargeKW = 0.0;
			settings.batteries = { cell };
			configure(world, settings, "recovery K rig");
			world.resumeSimulation();
		};
		auto driveSurge = [&](core::World& world, int ticks)
		{
			SurgeRun run;
			for (int tick = 1; tick <= ticks; ++tick)
			{
				runOrDiagnose(world, 1, "recovery K");
				auto const& l = loadSnap(world, "surger");
				run.trace.push_back(l.recoverySurgeKW);
				run.energy += l.recoverySurgeKW * core::World::getFixedTimestep();
				if (l.recoverySurgeKW > 1e-9) run.surgeTicks += 1;
			}
			return run;
		};
		// The battery funded the whole surge and nothing else: the energy
		// it lost from storage equals the surge energy delivered to the
		// load, so no overdraw is ever paid by the reserve.
		auto checkBattery = [&](core::World& world, double surgeEnergy, char const* what)
		{
			auto const snap = world.getStationSystemsSnapshot();
			auto const& bat = snap.power.batteries[0];
			require(closeTo(bat.dischargedKWh, surgeEnergy / 3600.0, 1e-9),
				(std::string(what) + ": the battery lost energy the surge never delivered").c_str());
			require(closeTo(bat.storedKWh, 0.5 - surgeEnergy / 3600.0, 1e-9),
				(std::string(what) + ": the battery's remaining charge does not match the surge draw").c_str());
		};
		auto checkReplay = [&](SurgeRun const& first, SurgeRun const& replay)
		{
			require(first.trace.size() == replay.trace.size(),
				"the surge replay trace length changed after reset");
			for (size_t i = 0; i < first.trace.size() && i < replay.trace.size(); ++i)
				require(closeTo(first.trace[i], replay.trace[i], 1e-12),
					"the surge proration is not deterministic across a reset/replay");
			require(closeTo(first.energy, replay.energy, 1e-12),
				"the surge energy is not deterministic across a reset/replay");
		};
		// Sub-tick surge: a 0.01 s budget is shorter than one tick and
		// is paid whole in a single prorated tick, never as a full-tick
		// purchase.
		{
			core::World world("RecK-short", 2, 2);
			runSurgeRig(world, 0.01);
			auto const first = driveSurge(world, 330); // t = 5.5s, past release + full surge
			require(first.surgeTicks == 1,
				"a sub-tick surge was not paid in a single prorated tick");
			require(closeTo(first.energy, 1.0 * 0.01, 1e-6),
				"the sub-tick surge delivered a different energy than was authored");
			for (auto const kw : first.trace)
				if (kw > 1e-9)
					require(closeTo(kw, 1.0 * 0.01 / core::World::getFixedTimestep(), 1e-6),
						"the sub-tick surge was not prorated to the remaining window");
			require(loadSnap(world, "surger").recoveryStage == core::StationLoadRecoveryStage::Active,
				"the sub-tick surge did not settle the load to Active");
			checkBattery(world, first.energy, "sub-tick surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "sub-tick surge replay");
		}
		// Ticket-shaped partial tail: a 0.02 s surge is one full tick at
		// the declared 1 kW plus one partial tick at the prorated
		// remainder - never a second full-tick purchase.
		{
			core::World world("RecK-ticket", 2, 2);
			runSurgeRig(world, 0.02);
			auto const first = driveSurge(world, 330);
			require(first.surgeTicks == 2,
				"a 0.02 s surge did not draw exactly one full tick and one partial tick");
			require(closeTo(first.energy, 1.0 * 0.02, 1e-6),
				"the partial-tick surge delivered a different energy than was authored");
			std::vector<double> drawn;
			for (auto const kw : first.trace)
				if (kw > 1e-9) drawn.push_back(kw);
			require(drawn.size() == 2
				&& closeTo(drawn[0], 1.0, 1e-6)
				&& closeTo(drawn[1], 0.2, 1e-6),
				"the partial final tick of the surge was not prorated to the remainder");
			checkBattery(world, first.energy, "ticket-shaped surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "ticket-shaped surge replay");
		}
		// Multi-tick partial tail: a 0.055 s surge is three full ticks at
		// 1 kW plus one partial tick at the prorated remainder.
		{
			core::World world("RecK-partial", 2, 2);
			runSurgeRig(world, 0.055);
			auto const first = driveSurge(world, 330);
			require(first.surgeTicks == 4,
				"a 0.055 s surge did not draw three full ticks and one partial tick");
			require(closeTo(first.energy, 1.0 * 0.055, 1e-6),
				"the partial-tick surge delivered a different energy than was authored");
			std::vector<double> drawn;
			for (auto const kw : first.trace)
				if (kw > 1e-9) drawn.push_back(kw);
			require(drawn.size() == 4
				&& closeTo(drawn[0], 1.0, 1e-6)
				&& closeTo(drawn[1], 1.0, 1e-6)
				&& closeTo(drawn[2], 1.0, 1e-6)
				&& closeTo(drawn[3], 0.3, 1e-6),
				"the partial final tick of the surge was not prorated to the remainder");
			checkBattery(world, first.energy, "multi-tick partial-tail surge");
			world.resetSimulation(); world.resumeSimulation();
			auto const replay = driveSurge(world, 330);
			checkReplay(first, replay);
			checkBattery(world, replay.energy, "multi-tick partial-tail surge replay");
		}
	}

	// Shared rig for the #589 scenarios: no solar, no battery, one
	// fuelled generator with zero startup time and power behind a cool
	// sink, and automatic dispatch. The only thing that can put power on
	// the bus is a generator the dispatcher chose to start.
	struct DispatchRig
	{
		core::StationSystemsSettings settings;
	};
	auto makeDispatchRig = [&]()
	{
		DispatchRig rig;
		auto& settings = rig.settings;
		core::ResourceStoreSettings fuel;
		fuel.id = "fuel"; fuel.unit = "kWh";
		fuel.capacity = 100.0; fuel.startingQualified = 100.0;
		settings.stores = { fuel };
		settings.dispatchMode = core::StationDispatchMode::Automatic;
		settings.thermalReservoirs = { reservoir("sink", "low", 20.0, 200.0) };
		core::GeneratorSettings gen;
		gen.id = "gen"; gen.fuelStoreId = "fuel"; gen.thermalClass = "low";
		gen.heatSinkId = "sink"; gen.ratedOutputKW = 10.0; gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 1000.0; gen.startupPowerKW = 0.0;
		gen.startupSeconds = 0.0; gen.maxSinkTemperatureC = 90.0;
		settings.generators = { gen };
		return rig;
	};
	auto storeSnap = [](core::World& world, std::string const& id) -> core::StationSystemsStoreSnapshot
	{
		for (auto const& s : world.getStationSystemsSnapshot().stores)
			if (s.id == id) return s;
		throw std::runtime_error("store not found: " + id);
	};

	// Scenario K (#589): the ticket's deadlock. With no solar and no
	// battery, a shed load drove no dispatch demand, so the dispatcher
	// never commanded the generator, the spare pool never appeared, and
	// the load stayed Shed forever with fuel still in the tank - a
	// circular wait between the restart gate and the dispatcher. Now
	// the shed load's pending restart feeds the dispatch signal, the
	// generator comes online to fund the restart, and the load returns
	// to full service.
	{
		core::World world("RecK", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		rig.settings.equipmentLoads = { recoveryLoad("rig", 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0) };
		configure(world, rig.settings, "recovery K rig");
		world.resumeSimulation();
		bool sawShed = false;
		bool sawPendingDemand = false;
		for (uint64_t tick = 1; tick <= 120; ++tick) // t = 2s
		{
			runOrDiagnose(world, 1, "recovery K");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "rig");
			if (l.recoveryStage == core::StationLoadRecoveryStage::Shed)
			{
				sawShed = true;
				if (snap.power.dispatchDemandKW > 1e-9) sawPendingDemand = true;
			}
		}
		require(sawShed, "the load never shed against the empty pool");
		require(sawPendingDemand,
			"the shed load's pending restart never reached the dispatch signal");
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& l = loadSnap(world, "rig");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
			"the load never restarted under automatic dispatch");
		require(closeTo(l.suppliedKW, 1.0), "the restarted load is not fully supplied");
		require(snap.power.generators[0].outputKW >= 1.0 - 1e-9,
			"the generator never came online for the waiting restart");
		require(storeSnap(world, "fuel").currentQualified < 100.0,
			"the generator served the load without drawing fuel");
	}

	// Scenario L (#589): the surge of a restart in flight keeps the
	// source online. A load with a declared startup surge restarted by
	// automatic dispatch must be able to pay its whole surge budget
	// without the dispatch signal dropping mid-restart and sending the
	// load back to Shed - the surge rides the signal until it is paid,
	// then the station settles back to the load's normal demand.
	{
		core::World world("RecL", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		// demand 1, min 1, dwell 0, hysteresis 0, surge 1 kW for 0.5s.
		rig.settings.equipmentLoads = { recoveryLoad("driller", 1.0, 0.0, 0.0, 0.0, 1.0, 0.5, 0.0) };
		configure(world, rig.settings, "recovery L rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawSurge = false;
		for (uint64_t tick = 1; tick <= 1200; ++tick) // t = 20s
		{
			runOrDiagnose(world, 1, "recovery L");
			auto const& l = loadSnap(world, "driller");
			require(!(l.recoveryStage == core::StationLoadRecoveryStage::Shed
					&& l.suppliedKW > 1e-9),
				"a failed restart published supplied service while returning to Shed");
			if (l.recoverySurgeKW > 1e-9) sawSurge = true;
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
		}
		require(sawSurge, "the automatic-dispatch restart never drew its surge");
		auto const& l = loadSnap(world, "driller");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Active,
			"the surge restart never settled to Active under automatic dispatch");
		require(closeTo(l.suppliedKW, 1.0), "the settled load is not fully supplied");
		require(surgeEnergy <= 1.0 * 0.5 + 1e-9,
			"the restart delivered more surge energy than was authored");
	}

	// Scenario M (#589): a shed load whose declared feed cannot sustain
	// it never drives fuel burn. Same rig as K, but the load consumes
	// from an empty store: the restart is barred by the feed itself,
	// so its need never reaches the dispatcher, the generator stays
	// stopped, and not one unit of fuel is burned for equipment that
	// cannot run.
	{
		core::World world("RecM", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto rig = makeDispatchRig();
		rig.settings.stores.push_back(massStore("brine", 10.0, 0.0));
		auto starved = recoveryLoad("starved", 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0);
		core::EquipmentMaterialExchangeSettings feed;
		feed.storeId = "brine"; feed.ratePerSecond = 0.01; feed.consumes = true;
		starved.materialExchanges = { feed };
		rig.settings.equipmentLoads = { starved };
		configure(world, rig.settings, "recovery M rig");
		world.resumeSimulation();
		runOrDiagnose(world, 120, "recovery M"); // t = 2s
		auto const& snap = world.getStationSystemsSnapshot();
		auto const& l = loadSnap(world, "starved");
		require(l.recoveryStage == core::StationLoadRecoveryStage::Shed,
			"a feed-starved load restarted despite its infeasible feed");
		require(l.suppliedKW <= 1e-9, "a shed load drew power");
		require(snap.power.dispatchDemandKW <= 1e-9,
			"a feed-starved shed load drove the dispatch signal");
		require(snap.power.generators[0].outputKW <= 1e-9,
			"the generator ran for equipment its feed cannot sustain");
		require(closeTo(storeSnap(world, "fuel").currentQualified, 100.0),
			"fuel was burned for a load that cannot run on its declared feed");
	}

	// Scenario N (#590): a battery-funded surge is booked through the
	// same station-wide aggregate accounting as every other consumer.
	// A 2 kW load behind 2 kW of sun can pay its normal service from
	// generation but nothing of its 1 kW surge: the whole surge is
	// battery delivery. The station-wide discharge must show that
	// surge exactly once, the station-wide shortfall must agree with
	// the single bank's own shortfall on every tick (the ticket's bug
	// hid a bank shortfall behind a zero aggregate), and the bank's
	// energy ledger must close over the surge with no phantom draw.
	// The rig is then reset and replayed to prove the battery surge
	// state is deterministic.
	{
		core::World world("RecN", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("surger", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings bank;
		bank.id = "bank"; bank.nameplateKWh = 1.0; bank.usableKWh = 1.0;
		bank.startingStoredKWh = 0.5; bank.maxDischargeKW = 1.0; bank.maxChargeKW = 0.0;
		settings.batteries = { bank };
		configure(world, settings, "recovery N rig");
		world.resumeSimulation();
		double surgeEnergy = 0.0;
		bool sawBatterySurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery N");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "surger");
			// One bank: the station-wide shortfall is exactly the bank's,
			// every tick, funded or not.
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall disagreed with the bank's surge shortfall");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the battery-funded surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				// Generation is fully spent on the normal service, so the
				// whole surge is battery delivery and shows in the
				// station-wide discharge exactly once.
				require(std::abs(snap.power.batteryDischargeKW - l.recoverySurgeKW) <= 1e-9,
					"the battery-funded surge was not booked into the station-wide discharge");
				require(l.suppliedKW >= 2.0 - 1e-9,
					"the battery-funded surge starved the load's normal service");
				sawBatterySurge = true;
			}
			surgeEnergy += l.recoverySurgeKW * core::World::getFixedTimestep();
		}
		require(sawBatterySurge, "the surge was never battery-funded");
		require(closeTo(surgeEnergy, 1.0 * 0.05, 1e-9),
			"the battery-funded surge did not deliver its authored energy");
		{
			auto const& snap = world.getStationSystemsSnapshot();
			auto const surgeEnergyKWh = surgeEnergy / 3600.0;
			require(closeTo(snap.power.batteries[0].deliveredKWh, surgeEnergyKWh, 1e-9),
				"the bank's delivered ledger does not match the surge energy booked");
			require(closeTo(snap.power.batteries[0].storedKWh, 0.5 - surgeEnergyKWh, 1e-9),
				"the bank's stored energy does not close over the surge with no phantom draw");
		}
		// Deterministic reset/replay (#590): the same authored rig
		// replayed from a reset reproduces the battery surge ledger
		// exactly - host and bundled model agree tick for tick.
		auto const before = world.getStationSystemsSnapshot();
		world.pauseSimulation(); world.resetSimulation(); world.resumeSimulation();
		runOrDiagnose(world, 480, "recovery N replay");
		require(world.getStationSystemsSnapshot() == before,
			"the battery-funded surge did not replay deterministically after reset");
	}

	// Scenario O (#590): a surge paid partly by generation and partly
	// by the battery. 2.5 kW of sun covers the 2 kW normal service and
	// half of the 1 kW surge; the battery delivers the other half. The
	// split battery delivery is booked into the station-wide discharge
	// and the shortfall keeps agreeing with the single bank - now at a
	// nonzero delivery with zero unserved remainder.
	{
		core::World world("RecO", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.5, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("mixer", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings bank;
		bank.id = "bank"; bank.nameplateKWh = 1.0; bank.usableKWh = 1.0;
		bank.startingStoredKWh = 0.5; bank.maxDischargeKW = 1.0; bank.maxChargeKW = 0.0;
		settings.batteries = { bank };
		configure(world, settings, "recovery O rig");
		world.resumeSimulation();
		bool sawSplitSurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery O");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "mixer");
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall disagreed with the bank's during the split surge");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the split-funded surge");
			if (l.recoverySurgeKW > 0.9)
			{
				// Full-surge ticks: generation carries 0.5 kW of the surge
				// and the battery the other half, exactly once. The last,
				// prorated tick of the surge budget carries the same split
				// against its shortened slice.
				auto const expectedBatteryShare = std::max(0.0, l.recoverySurgeKW - 0.5);
				require(std::abs(snap.power.batteryDischargeKW - expectedBatteryShare) <= 1e-6,
					"the battery's half of the split surge was not booked exactly once");
				require(std::abs(l.suppliedKW - 2.0) <= 1e-9,
					"the split surge disturbed the load's normal service");
				sawSplitSurge = true;
			}
			else if (l.recoverySurgeKW > 1e-9)
			{
				// The prorated tail fits inside generation's leftover, so
				// the battery never delivers more than its half-share.
				auto const expectedBatteryShare = std::max(0.0, l.recoverySurgeKW - 0.5);
				require(std::abs(snap.power.batteryDischargeKW - expectedBatteryShare) <= 1e-6,
					"the battery delivered beyond its share of the surge tail");
			}
		}
		require(sawSplitSurge, "the surge never ran split between generation and battery");
	}

	// Scenario P (#590): multiple banks route one surge in declaration
	// order and the station-wide totals count every watt exactly once.
	// The banks together carry only 1.5 kW - below the 2 kW minimum -
	// so the dark half sheds the load, and the restart surge of 1 kW
	// routes across them: the small bank can take only 0.4 kW and the
	// big bank carries the rest. The station-wide discharge is the sum
	// of the two banks' delivery, and the station-wide shortfall stays
	// zero: the small bank's routed-but-later-served remainder is its
	// own per-bank diagnostic and never leaks into the station
	// shortage readout as a double count.
	{
		core::World world("RecP", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(2.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 2, min 2, dwell 0, hysteresis 0, surge 1 kW for 0.05s.
		settings.equipmentLoads = { recoveryLoad("rig", 2.0, 0.0, 0.0, 0.0, 1.0, 0.05, 0.0) };
		core::BatterySettings small;
		small.id = "small"; small.nameplateKWh = 1.0; small.usableKWh = 1.0;
		small.startingStoredKWh = 0.5; small.maxDischargeKW = 0.4; small.maxChargeKW = 0.0;
		core::BatterySettings big;
		big.id = "big"; big.nameplateKWh = 1.0; big.usableKWh = 1.0;
		big.startingStoredKWh = 0.5; big.maxDischargeKW = 1.1; big.maxChargeKW = 0.0;
		settings.batteries = { small, big };
		configure(world, settings, "recovery P rig");
		world.resumeSimulation();
		bool sawMultiBankSurge = false;
		for (uint64_t tick = 1; tick <= 480; ++tick) // t = 8s, past the restart
		{
			runOrDiagnose(world, 1, "recovery P");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "rig");
			require(snap.power.totalSuppliedKW + 1e-9
				>= l.suppliedKW + l.recoverySurgeKW,
				"the supplied total did not count the multi-bank surge");
			if (l.recoverySurgeKW > 1e-9)
			{
				// The whole surge is battery delivery and the station-wide
				// discharge is the two banks' delivery counted once.
				require(std::abs(snap.power.batteryDischargeKW - l.recoverySurgeKW) <= 1e-9,
					"the multi-bank surge was not booked into the station-wide discharge exactly once");
				require(std::abs(snap.power.batteryDischargeKW
						- (snap.power.batteries[0].lastDischargeKW
							+ snap.power.batteries[1].lastDischargeKW)) <= 1e-9,
					"the station-wide discharge is not the sum of the banks' delivery");
				// Declaration-order routing: the small bank takes the surge
				// first, up to its power limit.
				auto const expectedSmall = std::min(l.recoverySurgeKW, 0.4);
				require(std::abs(snap.power.batteries[0].lastDischargeKW - expectedSmall) <= 1e-9,
					"the surge did not route through the banks in declaration order");
				// Nothing went unserved, so the station-wide shortfall is
				// zero - it never accumulates the small bank's routed
				// remainder that the big bank went on to deliver.
				require(snap.power.batteryShortfallKW <= 1e-9,
					"a served routed remainder double-counted into the station battery shortfall");
				sawMultiBankSurge = true;
			}
		}
		require(sawMultiBankSurge, "the surge never routed across multiple banks");
	}

	// Scenario Q (#590): the ticket's probe through the public seam. A
	// 1 kW load with a 1 kW surge behind exactly 1 kW of sun and a
	// battery that can discharge nothing: the restart is unfundable
	// every tick. Nothing is routed to the battery and nothing is
	// refused, so the station-wide shortfall and the bank's own
	// shortfall agree at zero - the aggregate can never hide a
	// routed-but-unserved surge from the station readout, and the
	// refused restart never publishes service.
	{
		core::World world("RecQ", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		core::StationSystemsSettings settings;
		settings.stores = { massStore("ballast", 10.0, 1.0) };
		settings.solarArrays = { solar(1.0, stepProfile(100.0, {{0.0, 0.0}, {5.0, 1.0}})) };
		// demand 1, min 1, dwell 0, hysteresis 0, surge 1 kW for 0.02s.
		settings.equipmentLoads = { recoveryLoad("starter", 1.0, 0.0, 0.0, 0.0, 1.0, 0.02, 0.0) };
		core::BatterySettings dead;
		dead.id = "dead"; dead.nameplateKWh = 1.0; dead.usableKWh = 1.0;
		dead.startingStoredKWh = 0.5; dead.maxDischargeKW = 0.0; dead.maxChargeKW = 0.0;
		settings.batteries = { dead };
		configure(world, settings, "recovery Q rig");
		world.resumeSimulation();
		for (uint64_t tick = 1; tick <= 600; ++tick) // t = 10s of refused restarts
		{
			runOrDiagnose(world, 1, "recovery Q");
			auto const& snap = world.getStationSystemsSnapshot();
			auto const& l = loadSnap(world, "starter");
			require(std::abs(snap.power.batteryShortfallKW
					- snap.power.batteries[0].lastDischargeShortfallKW) <= 1e-9,
				"the station-wide battery shortfall hid the bank's shortfall on the failed surge");
			require(l.suppliedKW <= 1e-9, "the unfundable restart published normal service");
			require(l.recoverySurgeKW <= 1e-9, "the unfundable restart drew surge power");
			require(snap.power.batteryDischargeKW <= 1e-9,
				"the failed surge discharged the battery it could not pay with");
		}
		require(loadSnap(world, "starter").recoveryStage != core::StationLoadRecoveryStage::Active,
			"an unfundable surge restart settled to Active without ever paying its surge");
	}
}

	// #418: bounded trends and incident history. Trend series are sampled
	// from the committed ledger at the existing observation cadence - no
	// second clock, no per-tick history - with a fixed per-series sample
	// budget (pairwise-merge downsampling on overflow) and an authored
	// retention cap in simulation seconds. Events carry a stable
	// presentation category so later features' incident types render
	// through the same contract. Reading trends never mutates state.
	void trends(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const constantProfile = [](double level)
		{ core::ProfileSettings p; p.constantLevel = level; return p; };
		auto const replaceAll = [](std::string text, std::string const& from, std::string const& to)
		{
			for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
				text.replace(pos, from.size(), to);
			return text;
		};
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findSeries = [](std::vector<core::StationTrendSeries> const& trends,
			std::string const& subsystem, std::string const& id) -> core::StationTrendSeries const*
		{
			for (auto const& series : trends)
				if (series.subsystem == subsystem && series.id == id) return &series;
			return nullptr;
		};
		// Steady-state rig: a 10 kW sun, a 6 kW constant load fully
		// supplied, and a 1 kg/s transfer with room at both ends. Every
		// sampled value is a known constant, so merge or retention
		// arithmetic errors show up as value drift, not just count drift.
		auto const rigSettings = [&]()
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			core::ResourceStoreSettings source;
			source.id = "source"; source.unit = "kg";
			source.capacity = 10000.0; source.startingQualified = 8000.0;
			core::ResourceStoreSettings sink;
			sink.id = "sink"; sink.unit = "kg";
			sink.capacity = 10000.0; sink.startingQualified = 0.0;
			settings.stores = { source, sink };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "source"; pump.destinationId = "sink";
			pump.unit = "kg"; pump.ratePerSecond = 1.0; pump.priority = 5;
			settings.transfers = { pump };
			core::SolarArraySettings sun;
			sun.id = "sun"; sun.capacityKW = 10.0; sun.illumination = constantProfile(1.0);
			settings.solarArrays = { sun };
			core::EquipmentLoadSettings load;
			load.id = "shop"; load.electricalDemandKW = 6.0;
			load.operatingProfile = constantProfile(1.0);
			load.heatOutputKW = 0.0; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			load.criticalFloorKW = 0.0; load.priority = 10; load.maxCurtailment = 1.0;
			settings.equipmentLoads = { load };
			return settings;
		};
		std::string diagnostic;
		// Extreme limits and invalid spans exercise the public shared recorder.
		{
			core::StationRecordingSlot a, b;
			a.metric.id = "a"; b.metric.id = "b";
			core::StationRecordingLimits limits; limits.samples = 4;
			core::StationHistory history({a, b}, 1, 100, limits);
			std::vector<core::StationMetricSample> values(2);
			for (auto& v : values) { v.actual = 0; v.validity = core::StationMetricValidity::Valid; }
			for (int t = 1; t <= 8; ++t)
			{
				values[1].validity = t == 2 ? core::StationMetricValidity::Unavailable : core::StationMetricValidity::Valid;
				history.observe(t, values);
			}
			auto const recorded = history.metrics();
			require(history.status().coarseningLevel == 1 && history.status().truncated,
				"shared history did not coarsen deterministically");
			require(recorded[0].samples.size() == recorded[1].samples.size()
				&& recorded[0].samples[0].actual == 0 && !recorded[1].samples[0].actual
				&& recorded[1].samples[0].coverage == 0.5,
				"coarsening confused a known zero with an invalid interval");
			for (size_t i = 0; i < recorded[0].samples.size(); ++i)
				require(recorded[0].samples[i].elapsedSeconds == recorded[1].samples[i].elapsedSeconds,
					"shared history lost timeline alignment");
			limits.memoryBytes = 1;
			core::StationHistory limited({a, b}, 1, 100, limits);
			require(limited.status().limited && limited.metrics().empty(), "memory limit silently omitted a metric");
			limits = {}; limits.metrics = 1;
			require(core::StationHistory({a, b}, 1, 100, limits).status().limited, "cardinality limit ignored");
			limits = {}; limits.metadataBytes = 1;
			require(core::StationHistory({a, b}, 1, 100, limits).status().limited, "metadata limit ignored");
			limits = {}; limits.samples = 4;
			core::StationHistory retained({a, b}, 1, 2, limits);
			for (int t = 1; t <= 20; ++t) retained.observe(t, values);
			require(retained.status().retainedUntilSeconds - retained.status().retainedFromSeconds <= 2
				&& retained.status().recordingBytes <= limits.memoryBytes, "retention or recording bound exceeded");
		}
		// #697: public revision/query seam is read-only and independent of
		// population size. Counters measure work, not machine-dependent timing.
		{
			core::World world("Cached applied run", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			for (size_t i = 1; i < core::MaxStationEquipmentLoads; ++i)
			{
				auto load = settings.equipmentLoads.front(); load.id = "other-" + std::to_string(i);
				settings.equipmentLoads.push_back(load);
			}
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.markSaved();
			auto revision = world.getStationSystemsObservationRevision();
			auto const* catalogue = &world.getStationSystemsObservationCatalogue();
			world.resumeSimulation(); runOrDiagnose(world, 60, "cache run"); world.pauseSimulation();
			auto const recordedRevision = world.getStationSystemsObservationRevision();
			require(recordedRevision.run == revision.run && recordedRevision.history > revision.history,
				"observation did not advance only the history revision");
			auto const beforeQueryDocument = yaml(world);
			auto const beforeQuerySnapshot = world.getStationSystemsSnapshot();
			auto const expected = world.queryStationSystemsChart("power");
			auto const work = world.getStationSystemsQueryWork();
			auto const* buffer = world.queryStationSystemsChart("power").series[0].values.data();
			for (int i = 0; i < 100; ++i)
			{
				require(&world.getStationSystemsObservationCatalogue() == catalogue
					&& world.queryStationSystemsChart("power").series[0].values.data() == buffer,
					"unchanged query replaced catalogue or prepared buffer");
			}
			require(work.queries == 1 && work.metricsExported == 2
				&& world.getStationSystemsQueryWork().queries == work.queries
				&& world.getStationSystemsQueryWork().metricsExported == work.metricsExported
				&& world.getStationSystemsObservationRevision() == recordedRevision && !world.isModified()
				&& yaml(world) == beforeQueryDocument && world.getStationSystemsSnapshot() == beforeQuerySnapshot,
				"cached query rebuilt unrelated metrics or mutated the paused document");
			auto changed = settings; changed.equipmentLoads.clear();
			require(!world.configureStationSystems(changed, false)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"unconfirmed application replaced the run");
			auto invalid = changed; invalid.observationIntervalSeconds = -1;
			require(!world.configureStationSystems(invalid, true)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"invalid application replaced the run");
			require(world.configureStationSystems(settings, false)
				&& world.getStationSystemsObservationRevision() == recordedRevision,
				"unchanged application reset history");
			(void)world.queryStationSystemsChart("unavailable");
			auto const reselected = world.queryStationSystemsChart("power");
			require(reselected.series[0].values == expected.series[0].values
				&& reselected.series[0].times == expected.series[0].times
				&& world.getSimulationTick() == 60 && world.getStationSystemsObservationRevision() == recordedRevision,
				"property reselection did not reuse retained history without replay");
			world.resetSimulation();
			require(world.getStationSystemsObservationRevision().run != revision.run
				&& world.queryStationSystemsChart("power").series[0].values.empty(),
				"Reset reused an old-run query");
			world.resumeSimulation(); runOrDiagnose(world, 60, "cached replay"); world.pauseSimulation();
			auto const replay = world.queryStationSystemsChart("power");
			require(replay.series[0].values == expected.series[0].values
				&& replay.series[1].values == expected.series[1].values
				&& replay.series[0].times == expected.series[0].times, "cached replay differs");
			revision = world.getStationSystemsObservationRevision();
			require(world.configureStationSystems(changed, true)
				&& world.getStationSystemsObservationRevision().run != revision.run
				&& world.queryStationSystemsChart("power").series[0].values.empty(),
				"successful application retained old query history");
			// Replace the sole cache entry repeatedly, including an unknown
			// property; this cannot accumulate a per-selection history map.
			auto const* cacheEntry = &world.queryStationSystemsChart("power");
			for (int i = 0; i < 50; ++i)
			{
				auto const& missing = world.queryStationSystemsChart("unavailable-" + std::to_string(i));
				require(missing.series.empty() && &missing == cacheEntry, "unknown property fabricated data or grew the cache");
				auto const& power = world.queryStationSystemsChart("power");
				require(power.series.size() == 2 && &power == cacheEntry, "Power selection did not reuse the bounded cache entry");
			}
			require(world.getStationSystemsQueryWork().metricsExported == 102,
				"selection built unrelated metrics");
		}
		// Scenario A: sampling cadence. With a 60 s observation interval
		// and a 60 s run, each series holds exactly one sample at the
		// observation boundary - never one per tick - carrying units,
		// simulation timestamps, requested versus actual and identity.
		{
			core::World world("TrendCadence", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 60.0f;
			settings.trendRetentionSeconds = 3600.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 3600, "trend cadence"); // t = 60 s: one boundary
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4,
				"expected exactly the power, power-generation, equipment-load and transfer trend series");
			for (auto const& series : trends)
			{
				require(series.samples.size() == 1,
					"trend series sampled more than once within one observation window");
				require(series.samples[0].worldTick == 3600,
					"trend sample is not at the observation-boundary tick");
				require(series.downsampleLevel == 0 && !series.truncated,
					"a fresh series reports downsampling or truncation");
				require(approx(series.sampleIntervalSeconds, 60.0, 1e-4),
					"series stride does not match the authored observation cadence");
			}
			auto const* power = findSeries(trends, "power", "");
			require(power != nullptr && power->unit == "kW",
				"station-wide power trend series missing or mislabelled");
			require(approx(power->samples[0].requested, 6.0)
				&& approx(power->samples[0].actual, 6.0),
				"the power trend did not record requested versus actual supply");
			auto const* generation = findSeries(trends, "power-generation", "");
			require(generation && generation->unit == "kW"
				&& approx(generation->samples[0].actual, 10.0)
				&& approx(generation->samples[0].requested, 10.0),
				"generation history must retain 10 kW generation, not 6 kW supplied service");
			auto const metrics = world.getStationSystemsMetricHistory();
			auto const chart = core::queryStationChart(core::stationCharts().front(), metrics);
			require(chart.series.size() == 2 && !chart.incomplete
				&& chart.series[0].times.size() == 1 && approx(chart.series[0].times[0], 60.0, 1e-5)
				&& chart.series[0].values.size() == 1 && approx(chart.series[0].values[0], 6.0)
				&& chart.series[1].values.size() == 1 && approx(chart.series[1].values[0], 10.0),
				"declarative Power query lost surplus generation, demand or simulation time");
			require(metrics.size() == 4 + 2 * settings.stores.size() + core::stationExchangeObservations(settings).size()
				&& metrics[0].descriptor.requestedSemantics
				&& !metrics[1].descriptor.requestedSemantics && !metrics[1].samples[0].requested,
				"catalogue manufactured requested generation semantics");
			auto const* transfer = findSeries(trends, "transfer", "pump");
			require(transfer != nullptr && transfer->unit == "kg",
				"transfer trend identity or unit missing");
			// The snapshot's transfer requested/completed are per-tick
			// amounts (rate x fixed step); the trend mirrors that exactly.
			auto const perTick = 1.0 * static_cast<double>(core::World::getFixedTimestep());
			require(approx(transfer->samples[0].requested, perTick)
				&& approx(transfer->samples[0].actual, perTick),
				"the transfer trend did not record requested versus completed flow");
			auto const* load = findSeries(trends, "equipment-load", "shop");
			require(load != nullptr && load->unit == "kW" && load->id == "shop",
				"equipment load trend identity or unit missing");
		}
		// Scenario B: a long run under the fixed sample budget. 1300
		// observations per series must coarsen (pairwise merge, stride
		// x2 per level, cadence-aware decimation) instead of growing
		// storage, keep the newest retained sample within one stride of
		// the run end, keep gaps at the retained stride, and - because
		// the rig is steady - keep every merged value exactly constant.
		{
			core::World world("TrendLongRun", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 86400.0f; // retention never binds here
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 78000, "trend long run"); // t = 1300 s, 1300 raw samples
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "long run lost a trend series");
			for (auto const& series : trends)
			{
				require(series.samples.size() <= 256,
					"trend series exceeded the 256-sample storage bound");
				require(series.downsampleLevel >= 1,
					"a 1300-sample run did not downsample under the storage bound");
				require(series.truncated,
					"a downsampled series does not report truncation");
				require(approx(series.sampleIntervalSeconds,
					std::pow(2.0, static_cast<double>(series.downsampleLevel))),
					"effective stride is not the base cadence times 2^level");
				require(78000 - series.samples.back().worldTick < static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5),
					"the series lost the newest sample: the newest retained sample is not within one stride of the run end");
				for (size_t index = 1; index < series.samples.size(); ++index)
				{
					auto const gap = series.samples[index].worldTick - series.samples[index - 1].worldTick;
					require(gap >= 60 && gap <= static_cast<uint64_t>(
						60.0 * series.sampleIntervalSeconds + 0.5),
						"downsampled series has a gap outside the retained stride");
				}
				for (auto const& sample : series.samples)
					require(approx(sample.requested, sample.actual),
						"a fully supplied series merged into unequal requested/actual values");
				}
			auto const* power = findSeries(trends, "power", "");
			require(power != nullptr && approx(power->samples.back().requested, 6.0)
				&& approx(power->samples.back().actual, 6.0),
				"the downsampled power trend drifted from the steady 6 kW");
		}
		// Scenario C: the authored retention caps the retained window in
		// simulation seconds. A 60 s retention over a 200 s run keeps at
		// most the last minute of samples and marks the series truncated.
		{
			core::World world("TrendRetention", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 60.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 12000, "trend retention"); // t = 200 s
			auto const trends = world.getStationSystemsTrends();
			for (auto const& series : trends)
			{
				require(series.downsampleLevel == 0,
					"the retention window forced a downsample it did not need");
				require(series.truncated,
					"retention dropping old samples did not mark the series truncated");
				require(series.samples.back().elapsedSeconds
					- series.samples.front().elapsedSeconds <= 60.0 + 1e-9,
					"the retained window exceeds the authored retention");
				require(series.samples.size() <= 62 && series.samples.size() >= 2,
					"retention kept a sample count outside the window it declares");
			}
		}
		// Scenario D: observation never mutates. Two identical rigs run
		// the same ticks; one has its trends, snapshot and event history
		// read repeatedly during the run, the other is never read until
		// the end. Both must finish identical - visibility/plotting and
		// UI update frequency cannot change simulated results.
		{
			core::World quiet("TrendQuiet", 2, 2);
			quiet.pauseSimulation(); quiet.finishBuild();
			require(quiet.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			core::World noisy("TrendNoisy", 2, 2);
			noisy.pauseSimulation(); noisy.finishBuild();
			require(noisy.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			quiet.resumeSimulation(); noisy.resumeSimulation();
			for (uint64_t leg = 0; leg < 8; ++leg)
			{
				runOrDiagnose(quiet, 50, "trend quiet twin");
				runOrDiagnose(noisy, 50, "trend noisy twin");
				(void)noisy.getStationSystemsTrends();
				(void)noisy.getStationSystemsSnapshot();
				(void)noisy.getStationSystemsEvents();
			}
			require(quiet.getStationSystemsSnapshot() == noisy.getStationSystemsSnapshot(),
				"reading trends/snapshots changed the simulated result");
			require(quiet.getStationSystemsTrends() == noisy.getStationSystemsTrends(),
				"reading trends changed the trend history");
			require(!quiet.getStationSystemsTrends().empty(),
				"the twin run produced no trends at all");
		}
		// Scenario E: Reset reconstructs. After a reset the trend history
		// is gone - nothing leaks across the boundary - and a fresh run
		// rebuilds the series from tick zero with no truncation.
		{
			core::World world("TrendReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(world.configureStationSystems(rigSettings(), false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 6000, "trend reset pre-run"); // t = 100 s
			require(!world.getStationSystemsTrends().empty(),
				"the pre-reset run produced no trends");
			world.resetSimulation();
			require(world.getStationSystemsTrends().empty(),
				"reset left trend history behind");
			require(world.getStationSystemsSnapshot().worldTick == 0,
				"reset did not return the World to tick zero");
			runOrDiagnose(world, 600, "trend post-reset run"); // t = 10 s
			auto const trends = world.getStationSystemsTrends();
			for (auto const& series : trends)
			{
				require(series.samples.size() == 10,
					"the post-reset series did not restart its sample window");
				require(series.downsampleLevel == 0 && !series.truncated,
					"the post-reset series carried downsampling or truncation across the reset");
				require(series.samples.front().worldTick == 60,
					"the post-reset series did not start from tick zero");
			}
		}
		// Scenario F: the authored retention persists with the World and
		// pre-95 documents carrying it are refused rather than silently
		// dropped; a pre-95 document without it loads the default.
		{
			auto const path = context.temporaryRoot() / "trends-roundtrip.world.yaml";
			core::World original("TrendSave", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			auto settings = rigSettings();
			settings.trendRetentionSeconds = 7200.0f;
			require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			original.resumeSimulation();
			runOrDiagnose(original, 300, "trend save run");
			original.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the trend document failed to reopen");
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"the trend retention did not survive the document round trip");
			require(reopened->getStationSystemsSettings().trendRetentionSeconds == 7200.0f,
				"the reopened retention is not the authored 7200 s");
			auto const text = yaml(original);
			require(text.find("trendRetentionSeconds: 7200") != std::string::npos,
				"the saved document does not carry the authored retention");
			// A pre-95 document carrying the field is refused, not silently
			// dropped, and the refusal names the schema it needs.
			auto stale = replaceAll(text, "version: 99", "version: 94");
			bool refused = false;
			std::string refusal;
			try
			{
				auto staleReader = core::YamlSerializer::fromString(stale);
				staleReader->deserialize();
				core::SerializationWorkData data;
				core::World staleWorld("Stale", 2, 2);
				staleWorld.deserialize(*staleReader, data);
			}
			catch (std::exception const& error) { refused = true; refusal = error.what(); }
			require(refused && refusal.find("trend retention requires World schema 95")
				!= std::string::npos,
				("a pre-95 document carrying the trend retention was not refused by name: " + refusal).c_str());
			// The same pre-95 document without the field loads with the
			// default retention - compatible with documents that predate
			// the feature.
			stale = replaceAll(stale, "trendRetentionSeconds: 7200", "");
			bool loaded = false;
			try
			{
				auto legacyReader = core::YamlSerializer::fromString(stale);
				legacyReader->deserialize();
				core::SerializationWorkData data;
				core::World legacyWorld("LegacyTrends", 2, 2);
				legacyWorld.deserialize(*legacyReader, data);
				loaded = legacyWorld.getStationSystemsSettings().trendRetentionSeconds == 3600.0f;
			}
			catch (std::exception const& error)
			{
				std::fprintf(stderr, "legacy trend document failed: %s\n", error.what());
			}
			require(loaded, "a pre-95 document without the retention field did not load the default");
		}
		// Scenario G: the stable presentation contract. Init, observation
		// and eclipse-transition events each carry their declared category,
		// so incidents render by subsystem without message parsing.
		{
			core::World world("TrendEvents", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			core::ProfileSettings flicker;
			flicker.periodSeconds = 20.0;
			flicker.segments = {{0.0, 1.0}, {10.0, 0.0}};
			settings.solarArrays.front().illumination = flicker;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1500, "trend event categories"); // t = 25 s: eclipse legs
			bool sawStation = false, sawObservation = false, sawSolar = false;
			for (auto const& event : world.getStationSystemsEvents())
			{
				if (event.message.rfind("Station systems initialized", 0) == 0)
				{
					require(event.category == "station",
						"the initialization event does not carry the station category");
					sawStation = true;
				}
				if (event.message.find("completed the World tick") != std::string::npos)
				{
					require(event.category == "observation",
						"the observation event does not carry the observation category");
					sawObservation = true;
				}
				if (event.message.rfind("Solar array", 0) == 0)
				{
					require(event.category == "solar",
						"a solar transition event does not carry the solar category");
					sawSolar = true;
				}
				if (event.message.rfind("Transfer '", 0) == 0)
					require(event.category == "transfer",
						"a transfer event does not carry the transfer category");
			}
			require(sawStation && sawObservation && sawSolar,
				"the bounded history lost one of the categorized event producers");
		}
		// Scenario H: the #603 regression. The ticket reproduced at a
		// station with no physical installations: 4500 s at the ordinary
		// 1 s observation cadence left the station-wide power series
		// declaring a ~545-year stride while new samples still arrived
		// every second, and the level-34 presentation shift was
		// undefined behavior. With cadence-aware decimation the series
		// is uniformly spaced at its declared stride all the way
		// through: every retained sample stands for exactly the declared
		// number of seconds, the level stays small because coarsening
		// itself slows the recording cadence, and the audit passes.
		{
			core::World world("TrendStrideTruth", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 86400.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 270000, "trend stride truth run"); // t = 4500 s
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "the 4500 s run lost a trend series");
			auto const recorded = world.getStationSystemsMetricHistory();
			auto const recording = world.getStationSystemsRecordingStatus();
			require(!recording.limited && recording.recordingBytes <= 32 * 1024 * 1024
				&& recording.coarseningLevel > 0, "World did not expose its bounded shared recorder");
			for (auto const& metric : recorded)
			{
				require(metric.samples.size() == recorded.front().samples.size(), "World metric timelines differ in length");
				for (size_t i = 0; i < metric.samples.size(); ++i)
					require(metric.samples[i].elapsedSeconds == recorded.front().samples[i].elapsedSeconds
						&& metric.samples[i].coverage == 1, "World metric timelines or coverage lost alignment");
			}
			for (auto const& series : trends)
			{
				require(series.samples.size() <= 256,
					"the coarsened series exceeded the sample bound");
				require(series.downsampleLevel >= 1,
					"a 4500-sample run did not coarsen at all");
				require(series.downsampleLevel <= 6,
					"cadence-aware decimation coarsened far past what the run length needs");
				require(approx(series.sampleIntervalSeconds,
					std::pow(2.0, static_cast<double>(series.downsampleLevel))),
					"the declared stride is not the base cadence times 2^level");
				auto const strideTicks = static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5);
				for (size_t index = 1; index < series.samples.size(); ++index)
					require(series.samples[index].worldTick - series.samples[index - 1].worldTick
						== strideTicks,
					"samples are not uniformly spaced at the declared stride");
				require(270000 - series.samples.back().worldTick < strideTicks,
					"the newest retained sample is more than one stride behind the run end");
				require(series.truncated, "a coarsened series does not report truncation");
			}
			auto const audit = core::auditStationStability(world.getStationSystemsSnapshot(),
				world.getStationSystemsEvents(), trends, world.getStationSystemsDiagnostics());
			require(audit.clean, "the coarsened trend history failed the stability audit");
			// The audit must refuse the old failure mode: a series whose
			// samples sit closer together than its declared stride
			// reports a fictitious stride and is a bound violation, and
			// so is a level past the published cap.
			core::StationTrendSeries lying;
			lying.subsystem = "power";
			lying.unit = "kW";
			lying.sampleIntervalSeconds = 32.0;
			lying.downsampleLevel = 5;
			lying.samples.push_back({60, 1.0, 1.0, 1.0});
			lying.samples.push_back({120, 2.0, 1.0, 1.0});
			auto const strideAudit = core::auditStationStability(world.getStationSystemsSnapshot(),
				{}, {lying}, {});
			require(!strideAudit.clean,
				"a series with a fictitious stride audited clean");
			require(std::any_of(strideAudit.boundViolations.begin(), strideAudit.boundViolations.end(),
				[](std::string const& text) { return text.find("below its declared stride") != std::string::npos; }),
				"the fictitious-stride violation was not reported by name");
			lying.downsampleLevel = core::StationTrendDownsampleLevelCap + 1;
			auto const capAudit = core::auditStationStability(world.getStationSystemsSnapshot(),
				{}, {lying}, {});
			require(!capAudit.clean,
				"a downsample level past the cap audited clean");
			require(std::any_of(capAudit.boundViolations.begin(), capAudit.boundViolations.end(),
				[](std::string const& text) { return text.find("exceeds the cap") != std::string::npos; }),
				"the past-cap level violation was not reported by name");
		}
		// Scenario I: retention expiry on a coarsened series. A 1000 s
		// retention over a 5000 s run binds after the second
		// coarsening: the window stays capped at the authored seconds,
		// the stride stays uniform inside it, and the series never
		// coarsens past the point where retention holds the count
		// below the storage bound.
		{
			core::World world("TrendRetentionCoarsened", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 1000.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 300000, "retention-bound coarsened run"); // t = 5000 s
			auto const trends = world.getStationSystemsTrends();
			require(trends.size() == 4, "the retention-bound run lost a trend series");
			for (auto const& series : trends)
			{
				require(series.downsampleLevel >= 1,
					"a 5000-sample run did not coarsen before retention bound it");
				require(series.downsampleLevel <= 2,
					"retention failed to hold the count below the storage bound, so coarsening ran on");
				require(series.samples.size() <= 256,
					"the retention-bound series exceeded the sample bound");
				auto const window = series.samples.back().elapsedSeconds
					- series.samples.front().elapsedSeconds;
				require(window <= 1000.0 + 1e-9,
					"the retained window exceeds the authored retention");
				require(window >= 900.0,
					"the retention window did not actually fill the authored seconds");
				auto const strideTicks = static_cast<uint64_t>(
					60.0 * series.sampleIntervalSeconds + 0.5);
				for (size_t index = 1; index < series.samples.size(); ++index)
					require(series.samples[index].worldTick - series.samples[index - 1].worldTick
						== strideTicks,
					"the retention-bound series lost uniform spacing");
				require(series.truncated, "a retention-trimmed series does not report truncation");
			}
		}
		// Scenario J: reset and replay. After a reset the coarsened
		// history is gone, and re-running the same ticks reproduces the
		// exact same series - the coarsening is a deterministic
		// function of the tick grid, not of accumulated drift.
		{
			core::World world("TrendReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto settings = rigSettings();
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 86400.0f;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 33000, "replay pre-reset run"); // t = 550 s: coarsened once
			auto const firstRun = world.getStationSystemsTrends();
			require(firstRun.size() == 4, "the pre-reset replay run produced no trends");
			require(firstRun.front().downsampleLevel >= 1,
				"the pre-reset run did not coarsen, so the replay proves nothing");
			world.resetSimulation();
			require(world.getStationSystemsTrends().empty(),
				"reset left coarsened trend history behind");
			runOrDiagnose(world, 33000, "replay post-reset run");
			require(world.getStationSystemsTrends() == firstRun,
				"the replay after reset did not reproduce the trend history exactly");
		}
		// Scenario K: the primitive exercised far past anything the
		// World-driven bounds can reach - 52 successive coarsenings
		// with nonconstant input, through the level cap and into the
		// sliding-window regime. A shadow model of the documented merge
		// semantics tracks every accepted value; the series must match
		// it exactly below the cap, stay uniformly spaced at the
		// declared stride throughout, and keep every published number
		// finite past the cap.
		{
			std::vector<core::StationTrendSeries> series;
			double const base = 1.0;
			double const retention = 1e18; // never binds: pure coarsening
			uint64_t tick = 0;
			double value = 0.0;
			uint32_t level = 0;
			int overflows = 0;
			std::vector<double> expected;
			while (overflows < 52)
			{
				// Mirror the primitive's own order: coarsen first when
				// the buffer is full, then record at the new stride.
				if (expected.size() >= 256)
				{
					std::vector<double> merged;
					for (size_t index = 0; index + 1 < expected.size(); index += 2)
						merged.push_back((expected[index] + expected[index + 1]) / 2.0);
					if (expected.size() % 2 != 0) merged.push_back(expected.back());
					expected = std::move(merged);
					++level;
					++overflows;
				}
				auto const effective = std::min(level, core::StationTrendDownsampleLevelCap);
				auto const strideTicks = static_cast<uint64_t>(60) << effective;
				tick += strideTicks;
				value += 1.0;
				core::appendBoundedTrendSample(series, "power", "", "kW", base, retention,
					tick, static_cast<double>(tick) / 60.0, value, value);
				expected.push_back(value);
				auto const& s = series.front();
				require(s.downsampleLevel == effective,
					"the primitive level diverged from the documented coarsening");
				require(s.samples.size() <= 256,
					"the primitive exceeded the sample bound");
				require(approx(s.sampleIntervalSeconds,
					std::ldexp(base, static_cast<int>(effective))),
					"the primitive stride is not base x 2^level");
				require(std::isfinite(s.sampleIntervalSeconds),
					"the stride went non-finite during repeated coarsening");
				for (size_t index = 1; index < s.samples.size(); ++index)
					require(s.samples[index].worldTick - s.samples[index - 1].worldTick
						== strideTicks,
						"the primitive lost uniform spacing across a coarsening");
				if (level <= core::StationTrendDownsampleLevelCap)
				{
					require(s.samples.size() == expected.size(),
						"the primitive and the documented merge semantics kept different counts");
					for (size_t index = 0; index < expected.size(); ++index)
						require(approx(s.samples[index].requested, expected[index])
							&& approx(s.samples[index].actual, expected[index]),
							"a merged value disagrees with the documented pairwise mean");
				}
			}
			auto const& s = series.front();
			require(s.downsampleLevel == core::StationTrendDownsampleLevelCap,
				"the level ran past the cap instead of sliding the window");
			require(s.samples.size() <= 256 && approx(s.samples.back().requested, value),
				"the sliding window lost the newest sample or grew past the bound");
			require(s.truncated, "the capped sliding window does not report truncation");
			for (auto const& sample : s.samples)
				require(std::isfinite(sample.requested) && std::isfinite(sample.actual)
					&& std::isfinite(sample.elapsedSeconds),
					"a sample went non-finite past the level cap");
		}
	}

	// #419: per-resource endurance explanation. For every Resource store
	// and battery the snapshot distinguishes the physical total, the
	// usable/qualified inventory and the protected reserve, reports the
	// net rate observed over the declared observation interval, and turns
	// that into a basis (unknown / depleted / no-net-change / growing /
	// depleting) with a stability claim and bounded estimates - never an
	// unjustified infinite countdown. The estimate is the explicit static
	// current-rate assumption: the observed net rate continues, no future
	// profile or consumption change is forecast. Next-delivery context is
	// shown per store without claiming a survival countdown.
	void endurance(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-9)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const constantProfile = [](double level)
		{ core::ProfileSettings p; p.constantLevel = level; return p; };
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findEndurance = [](std::vector<core::StationEnduranceSnapshot> const& entries,
			std::string const& kind, std::string const& id) -> core::StationEnduranceSnapshot const*
		{
			for (auto const& entry : entries)
				if (entry.kind == kind && entry.id == id) return &entry;
			return nullptr;
		};
		std::string diagnostic;
		// Scenario A: steady depletion with a next delivery in context.
		// A 2 kg/s transfer drains a store whose protected reserve is
		// withheld; the estimate is drawable / observed rate, the first
		// window carries no stability claim, the second confirms stability,
		// and the soonest authored delivery is reported as context.
		{
			core::World world("EnduranceDepletion", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 600.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings resupply;
			resupply.id = "tanker"; resupply.storeId = "water"; resupply.unit = "kg";
			resupply.quantity = 100.0; resupply.firstOccurrenceSeconds = 300.0;
			resupply.recurrenceSeconds = 100.0;
			settings.deliveries = { resupply };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			// Before the first window completes nothing is known: no rate,
			// no estimate - and never an infinite countdown.
			runOrDiagnose(world, 300, "endurance pre-window"); // t = 5 s
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry != nullptr, "no endurance entry for the depleted store");
				require(entry->unit == "kg", "endurance entry lost the store's declared unit");
				require(entry->basis == core::StationEnduranceBasis::Unknown,
					"endurance claimed a basis before any observation window completed");
				require(entry->stability == core::StationRateStability::Unknown,
					"endurance claimed stability with no observation");
				require(approx(entry->estimateSeconds, 0.0),
					"an unobserved store reported an endurance estimate");
				require(approx(entry->protectedReserve, 100.0)
					&& approx(entry->drawableUsable, 490.0, 1e-4),
					"the reserve/drawable split is not the qualified amount above the protected reserve");
				require(entry->hasNextDelivery && approx(entry->nextDeliverySeconds, 300.0)
					&& approx(entry->nextDeliveryQuantity, 100.0)
					&& entry->nextDeliveryArrivesQualified,
					"the next-delivery context is missing or wrong before the first window");
			}
			runOrDiagnose(world, 300, "endurance first window"); // t = 10 s: first boundary
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry != nullptr, "endurance entry vanished after the first window");
				require(entry->basis == core::StationEnduranceBasis::Depleting,
					"a steady drain did not read as depleting");
				require(entry->stability == core::StationRateStability::Unknown,
					"a single observation window claimed stability");
				require(approx(entry->observedNetRatePerSecond, -2.0, 1e-5),
					"the observed net rate does not match the 2 kg/s drain");
				require(approx(entry->observationWindowSeconds, 10.0, 1e-6)
					&& approx(entry->observedUntilSeconds, 10.0, 1e-6),
					"the observation window is not the declared observation interval");
				// drawable 480 at t=10, rate 2 kg/s -> 240 s static estimate.
				require(approx(entry->drawableUsable, 480.0, 1e-4),
					"drawable inventory drifted from the committed ledger");
				require(approx(entry->estimateSeconds, 240.0, 1e-4),
					"the static current-rate estimate is not drawable / observed rate");
				require(std::isfinite(entry->estimateSeconds),
					"the endurance estimate is not finite");
				require(approx(entry->boundMinSeconds, entry->estimateSeconds, 1e-9)
					&& approx(entry->boundMaxSeconds, 0.0),
					"a single-window drain must claim its estimate as the lower bound with no upper bound");
			}
			runOrDiagnose(world, 600, "endurance second window"); // t = 20 s
			{
				auto const snap = world.getStationSystemsSnapshot();
				auto const* entry = findEndurance(snap.endurance, "store", "water");
				require(entry->stability == core::StationRateStability::Stable,
					"two identical windows did not establish a stable rate");
				require(approx(entry->drawableUsable, 460.0, 1e-4),
					"drawable inventory drifted on the second window");
				require(approx(entry->estimateSeconds, 230.0, 1e-4),
					"the stable estimate is not drawable / observed rate on the second window");
				require(approx(entry->boundMaxSeconds, entry->estimateSeconds, 1e-6),
					"a stable rate must bound above at the same value it bounds below");
				require(approx(entry->physicalTotal, 560.0, 1e-4)
					&& approx(entry->usableQualified, 560.0, 1e-4),
					"physical total / usable qualified do not match the committed store state");
				require(entry->hasNextDelivery && approx(entry->nextDeliverySeconds, 300.0),
					"the next-delivery context moved off the authored schedule");
				auto const* tank = findEndurance(snap.endurance, "store", "tank");
				require(tank != nullptr && tank->basis == core::StationEnduranceBasis::Growing
					&& tank->observedNetRatePerSecond > 0.0,
					"the receiving store did not read as growing");
				require(approx(tank->estimateSeconds, 0.0),
					"a growing store reported a depletion countdown");
			}
		}
		// Scenario B: zero demand. A store with no flows at all reads as
		// no-net-change after its windows complete - explicitly not a
		// countdown, never an infinite endurance.
		{
			core::World world("EnduranceZeroDemand", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings idle;
			idle.id = "idle"; idle.unit = "L";
			idle.capacity = 500.0; idle.startingQualified = 250.0;
			settings.stores = { idle };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 900, "endurance zero demand"); // t = 15 s, three windows
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "idle");
			require(entry != nullptr, "no endurance entry for the idle store");
			require(entry->basis == core::StationEnduranceBasis::NoNetChange,
				"a store with no flows did not read as no-net-change");
			require(entry->stability == core::StationRateStability::Stable,
				"a zero rate across windows is not stable");
			require(approx(entry->observedNetRatePerSecond, 0.0),
				"the observed net rate of an untouched store is not zero");
			require(approx(entry->estimateSeconds, 0.0)
				&& approx(entry->boundMinSeconds, 0.0)
				&& approx(entry->boundMaxSeconds, 0.0),
				"no-net-change must not publish any countdown");
			require(!entry->hasNextDelivery,
				"a store with no authored deliveries reported next-delivery context");
		}
		// Scenario C: a sudden rate change. An equipment load whose
		// operating profile steps 4x at t=20 makes the drain rate triple
		// across the t=30 boundary: the rate is unstable, so the estimate
		// is a bound built from the fastest and slowest observed drains,
		// not a single precise countdown.
		{
			core::World world("EnduranceRateChange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings feed;
			feed.id = "feed"; feed.unit = "kg";
			feed.capacity = 10000.0; feed.startingQualified = 5000.0;
			settings.stores = { feed };
			core::SolarArraySettings sun;
			sun.id = "sun"; sun.capacityKW = 10.0; sun.illumination = constantProfile(1.0);
			settings.solarArrays = { sun };
			core::EquipmentLoadSettings pump;
			pump.id = "pump"; pump.electricalDemandKW = 1.0;
			core::ProfileSettings step;
			step.periodSeconds = 100000.0;
			step.segments = {{0.0, 0.25}, {25.0, 1.0}};
			pump.operatingProfile = step;
			pump.heatOutputKW = 0.0; pump.thermalClass = "low";
			pump.coolingDisposition = core::StationCoolingDisposition::Exported;
			pump.criticalFloorKW = 0.0; pump.priority = 10; pump.maxCurtailment = 1.0;
			core::EquipmentMaterialExchangeSettings draw;
			draw.storeId = "feed"; draw.ratePerSecond = 4.0; draw.consumes = true;
			pump.materialExchanges = { draw };
			settings.equipmentLoads = { pump };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1800, "endurance rate change pre-step"); // t = 30 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "feed");
			require(entry != nullptr, "no endurance entry for the equipment-fed store");
			require(entry->basis == core::StationEnduranceBasis::Depleting,
				"the post-step drain did not read as depleting");
			require(entry->stability == core::StationRateStability::Unstable,
				"a 4x profile step across the window boundary did not mark the rate unstable");
			// The window t=20..30 ran half its ticks at 0.25 x 4 = 1 kg/s
			// and half at the full 4 kg/s: the observed rate is the
			// average, about -2.5 kg/s (the boundary tick lands on the
			// later segment).
			require(approx(entry->observedNetRatePerSecond, -2.505, 1e-2),
				"the post-step window rate is not the profile-averaged draw");
			// The previous window ran at 1 kg/s. The bound is built from
			// both drains: fastest gives the conservative lower bound,
			// slowest the upper bound.
			auto const drawable = entry->drawableUsable;
			require(approx(entry->boundMinSeconds * std::abs(entry->observedNetRatePerSecond),
				drawable, 1e-6),
				"the unstable lower bound is not drawable over the fastest observed drain");
			require(approx(entry->boundMaxSeconds, drawable / 1.0, 1.0),
				"the unstable upper bound is not the slowest observed drain (1 kg/s)");
			require(approx(entry->estimateSeconds, entry->boundMinSeconds, 1e-9),
				"the unstable estimate is not the conservative lower bound");
			require(entry->boundMinSeconds < entry->boundMaxSeconds,
				"an unstable rate collapsed to a single unjustified point estimate");
		}
		// Scenario D: unavailable inventory. Material below the protected
		// reserve and material held unqualified are both not drawable: the
		// store reads depleted with a zero estimate, while the physical
		// total still shows what the station owns.
		{
			core::World world("EnduranceUnavailable", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings brine;
			brine.id = "brine"; brine.unit = "kg";
			brine.capacity = 100.0; brine.startingQualified = 50.0;
			brine.protectedReserve = 50.0; // everything qualified is withheld
			core::ResourceStoreSettings held;
			held.id = "held"; held.unit = "kg";
			held.capacity = 100.0; held.startingQualified = 0.0;
			held.startingUnqualified = 80.0; // owned but not usable
			settings.stores = { brine, held };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "endurance unavailable inventory"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* brineEntry = findEndurance(snap.endurance, "store", "brine");
			require(brineEntry != nullptr
				&& brineEntry->basis == core::StationEnduranceBasis::Depleted
				&& approx(brineEntry->drawableUsable, 0.0)
				&& approx(brineEntry->estimateSeconds, 0.0),
				"inventory fully inside the protected reserve did not read depleted");
			auto const* heldEntry = findEndurance(snap.endurance, "store", "held");
			require(heldEntry != nullptr
				&& heldEntry->basis == core::StationEnduranceBasis::Depleted
				&& approx(heldEntry->physicalTotal, 80.0, 1e-6)
				&& approx(heldEntry->usableQualified, 0.0)
				&& approx(heldEntry->drawableUsable, 0.0),
				"unqualified holdings were counted as usable endurance");
		}
		// Scenario E: battery energy/power distinctions. Endurance is
	// energy over the observed energy rate: the reserve floor is
	// excluded from the drawable energy, the discharge power rating is
	// reported as a constraint on how fast the endurance can be drawn
	// and is never itself the endurance rate.
		{
			core::World world("EnduranceBattery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 10.0;
			bank.maxChargeKW = 5.0; bank.maxDischargeKW = 3.6;
			settings.batteries = { bank };
			core::SolarArraySettings night;
			night.id = "night"; night.capacityKW = 10.0;
			night.illumination = constantProfile(0.0);
			settings.solarArrays = { night };
			core::EquipmentLoadSettings load;
			load.id = "life"; load.electricalDemandKW = 1.8;
			load.operatingProfile = constantProfile(1.0);
			load.heatOutputKW = 0.0; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			load.criticalFloorKW = 0.0; load.priority = 10; load.maxCurtailment = 1.0;
			settings.equipmentLoads = { load };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "endurance battery window"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "battery", "bank");
			require(entry != nullptr, "no endurance entry for the battery");
			require(entry->unit == "kWh", "battery endurance must be reported in energy units");
			require(approx(entry->physicalTotal, 10.0) && approx(entry->usableQualified, 8.0)
				&& approx(entry->protectedReserve, 2.0),
				"the battery total/usable/reserve split is not nameplate/cyclable/nameplate-minus-usable");
			require(approx(entry->maxDrawPowerKW, 3.6),
				"the discharge power constraint is missing from the battery endurance entry");
			require(entry->basis == core::StationEnduranceBasis::Depleting,
				"a battery covering a deficit did not read as depleting");
			// 1.8 kW = 1.8 kJ/s = 0.0005 kWh/s of stored energy.
			require(approx(entry->observedNetRatePerSecond, -0.0005, 1e-7),
				"the observed battery rate is not the stored-energy rate of the deficit");
			auto const drawable = entry->drawableUsable;
			require(approx(drawable, 7.995, 1e-5),
				"drawable battery energy is not stored above the reserve floor");
			require(approx(entry->estimateSeconds, drawable / 0.0005, 1e-3),
				"the battery endurance is not drawable energy over the observed energy rate");
			// The energy/power distinction: dividing energy by the power
			// rating (kWh / kW) is hours, not the seconds-scale endurance
			// the observed rate produces; the estimate must never equal it.
			require(std::abs(entry->estimateSeconds - drawable / 3.6) > 1.0,
				"the battery endurance was computed from the power rating, not the observed energy rate");
			require(entry->estimateSeconds < drawable / 0.0005 + 1.0
				&& std::isfinite(entry->estimateSeconds),
				"the battery endurance estimate is not a finite seconds-scale number");
		}
		// Scenario F: Reset clears the observation history. After a reset
		// every entry is back to Unknown (or Depleted where that is knowable
		// from the authored state alone) - no rate or estimate leaks across
		// the boundary - and a fresh run re-observes from the authored start.
		{
			core::World world("EnduranceReset", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings store;
			store.id = "air"; store.unit = "kg";
			store.capacity = 1000.0; store.startingQualified = 400.0;
			store.protectedReserve = 50.0;
			settings.stores = { store };
			core::ResourceTransferSettings bleed;
			bleed.id = "bleed"; bleed.sourceId = "air";
			bleed.destinationId = "air"; bleed.unit = "kg";
			bleed.ratePerSecond = 1.0; bleed.priority = 1;
			// A self-transfer is refused by validation; use a second store.
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1000.0; ballast.startingQualified = 0.0;
			settings.stores = { store, ballast };
			bleed.destinationId = "ballast";
			settings.transfers = { bleed };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 900, "endurance reset pre-run"); // t = 15 s, three windows
			auto const before = world.getStationSystemsSnapshot();
			auto const* beforeEntry = findEndurance(before.endurance, "store", "air");
			require(beforeEntry != nullptr
				&& beforeEntry->basis == core::StationEnduranceBasis::Depleting
				&& beforeEntry->stability == core::StationRateStability::Stable,
				"the pre-reset run did not establish a stable depletion");
			world.resetSimulation();
			auto const after = world.getStationSystemsSnapshot();
			auto const* afterEntry = findEndurance(after.endurance, "store", "air");
			require(afterEntry != nullptr, "reset lost the endurance entry");
			require(afterEntry->basis == core::StationEnduranceBasis::Unknown,
				"reset leaked an endurance basis across the boundary");
			require(approx(afterEntry->observedNetRatePerSecond, 0.0)
				&& approx(afterEntry->estimateSeconds, 0.0)
				&& afterEntry->observationWindowSeconds == 0.0,
				"reset leaked observed rate or estimate state");
			runOrDiagnose(world, 300, "endurance post-reset window"); // t = 5 s
			auto const replay = world.getStationSystemsSnapshot();
			auto const* replayEntry = findEndurance(replay.endurance, "store", "air");
			require(replayEntry != nullptr
				&& replayEntry->basis == core::StationEnduranceBasis::Depleting
				&& approx(replayEntry->observedNetRatePerSecond, -1.0, 1e-5)
				&& approx(replayEntry->drawableUsable, 345.0, 1e-4),
				"the post-reset run did not re-observe from the authored start");
		}
		// Scenario G: observation determinism. Two identical rigs run the
		// same ticks; one has its snapshot read repeatedly during the run.
		// Endurance is tick-driven, so UI visibility and read frequency
		// cannot change the reported estimates.
		{
			core::World quiet("EnduranceQuiet", 2, 2);
			quiet.pauseSimulation(); quiet.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 4.0f;
			core::ResourceStoreSettings a;
			a.id = "a"; a.unit = "kg"; a.capacity = 500.0; a.startingQualified = 300.0;
			core::ResourceStoreSettings b;
			b.id = "b"; b.unit = "kg"; b.capacity = 500.0; b.startingQualified = 100.0;
			settings.stores = { a, b };
			core::ResourceTransferSettings t;
			t.id = "t"; t.sourceId = "a"; t.destinationId = "b";
			t.unit = "kg"; t.ratePerSecond = 0.5; t.priority = 1;
			settings.transfers = { t };
			require(quiet.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			core::World noisy("EnduranceNoisy", 2, 2);
			noisy.pauseSimulation(); noisy.finishBuild();
			require(noisy.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			quiet.resumeSimulation(); noisy.resumeSimulation();
			for (uint64_t leg = 0; leg < 10; ++leg)
			{
				runOrDiagnose(quiet, 90, "endurance quiet twin");
				runOrDiagnose(noisy, 90, "endurance noisy twin");
				(void)noisy.getStationSystemsSnapshot();
				(void)noisy.getStationSystemsEvents();
			}
			require(quiet.getStationSystemsSnapshot() == noisy.getStationSystemsSnapshot(),
				"reading snapshots changed the endurance result");
			require(!quiet.getStationSystemsSnapshot().endurance.empty(),
				"the twin run produced no endurance entries at all");
		}
		// Scenario H: persistence compatibility. Endurance is derived
		// runtime state: it never travels in the document. A reopened
		// World carries the authored settings but no observed rates or
		// estimates - it must re-observe from its own ticks.
		{
			core::World original("EnduranceSave", 2, 2);
			original.pauseSimulation(); original.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 5.0f;
			core::ResourceStoreSettings store;
			store.id = "air"; store.unit = "kg";
			store.capacity = 1000.0; store.startingQualified = 400.0;
			store.protectedReserve = 50.0;
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1000.0; ballast.startingQualified = 0.0;
			settings.stores = { store, ballast };
			core::ResourceTransferSettings bleed;
			bleed.id = "bleed"; bleed.sourceId = "air"; bleed.destinationId = "ballast";
			bleed.unit = "kg"; bleed.ratePerSecond = 1.0; bleed.priority = 1;
			settings.transfers = { bleed };
			require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			original.resumeSimulation();
			runOrDiagnose(original, 600, "endurance save run"); // t = 10 s, two windows
			auto const path = context.temporaryRoot() / "endurance-roundtrip.world.yaml";
			original.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the endurance document failed to reopen");
			require(reopened->getStationSystemsSettings() == original.getStationSystemsSettings(),
				"the station settings did not survive the endurance round trip");
			auto const snap = reopened->getStationSystemsSnapshot();
			auto const* entry = findEndurance(snap.endurance, "store", "air");
			require(entry != nullptr
				&& entry->basis == core::StationEnduranceBasis::Unknown
				&& approx(entry->observedNetRatePerSecond, 0.0)
				&& approx(entry->estimateSeconds, 0.0),
				"observed endurance travelled in the document instead of being re-derived");
		}
	}

	// #420: the bounded scenario forecast. The static estimate is
	// extended from the observed current rate to the configured
	// demand/production profiles and the authored future delivery
	// schedule: projected inventory at resupply, shortfall onset with
	// the limiting service, capacity-capped accepted delivery
	// quantities, and scenario/uncertainty labels that never present
	// biology as guaranteed recovery. The forecast mutates no live
	// inventory, advances no World clock, consumes no deterministic
	// stream, and is bounded by the declared horizon and segment cap.
	void forecast(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findForecast = [](std::vector<core::StationForecastEntry> const& entries,
			std::string const& id) -> core::StationForecastEntry const*
		{
			for (auto const& entry : entries)
				if (entry.storeId == id) return &entry;
			return nullptr;
		};
		auto const findStore = [](std::vector<core::StationSystemsStoreSnapshot> const& stores,
			std::string const& id) -> core::StationSystemsStoreSnapshot const*
		{
			for (auto const& store : stores)
				if (store.id == id) return &store;
			return nullptr;
		};
		auto const chainStores = [](std::string const& id, double capacity,
			double starting) -> core::ResourceStoreSettings
		{
			core::ResourceStoreSettings settings;
			settings.id = id; settings.unit = "kg";
			settings.capacity = capacity; settings.startingQualified = starting;
			return settings;
		};
		auto const chainTransfer = [](std::string const& id, std::string const& src,
			std::string const& dst, double rate, int priority)
			-> core::ResourceTransferSettings
		{
			core::ResourceTransferSettings settings;
			settings.id = id; settings.sourceId = src; settings.destinationId = dst;
			settings.unit = "kg"; settings.ratePerSecond = rate;
			settings.priority = priority;
			return settings;
		};
		std::string diagnostic;
		// Scenario A: a known analytic profile with a timely one-off
		// resupply. A 2 kg/s drain on 180 drawable at t=10 is rescued at
		// t=50 by a 400 kg arrival (projected 100 drawable remaining at
		// the arrival), then runs out at t=300 with the transfer named as
		// the limiting service and the uncovered drain reported.
		{
			core::World world("ForecastTimely", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 100.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast timely resupply"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available,
				"the scenario forecast is not available for an enabled Station");
			require(approx(snap.forecast.asOfSeconds, 10.0),
				"the forecast is not as of the snapshot's simulation time");
			require(approx(snap.forecast.horizonSeconds, core::StationForecastHorizonSeconds),
				"the forecast horizon is not the declared bound");
			require(!snap.forecast.truncated, "a sparse schedule truncated the forecast");
			require(snap.forecast.scenarioLabel.find("declared-profile scenario") == 0,
				"the forecast carries no scenario label");
			require(snap.forecast.uncertaintyLabel.find("provisional biology") == 0,
				"the forecast carries no uncertainty label");
			auto const* entry = findForecast(snap.forecast.stores, "water");
			require(entry != nullptr, "no forecast entry for the drained store");
			require(entry->unit == "kg", "the forecast entry lost the store's declared unit");
			require(approx(entry->drawableAtStart, 180.0, 1e-4)
				&& approx(entry->physicalAtStart, 280.0, 1e-4),
				"the forecast start state does not match the committed inventory");
			require(approx(entry->netRateAtStart, -2.0),
				"the projected net rate does not match the authored drain");
			require(entry->basis == core::StationForecastBasis::Shortfall
				&& entry->hasShortfall,
				"a store that runs out inside the horizon did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 300.0, 1e-5),
				"the projected depletion onset is not drawable-after-arrival / rate");
			require(entry->limitingServiceId == "pump"
				&& entry->limitingServiceKind == "transfer",
				"the limiting service is not the binding transfer");
			require(approx(entry->projectedDrawableAtHorizon, 0.0),
				"a depleted store did not project zero drawable at the horizon");
			require(entry->arrivals.size() == 1, "the authored arrival is missing");
			if (entry->arrivals.size() == 1)
			{
				auto const& arrival = entry->arrivals[0];
				require(arrival.deliveryId == "tanker" && arrival.storeId == "water",
					"the projected arrival lost its identity");
				require(approx(arrival.arrivalSeconds, 50.0),
					"the projected arrival is not on the authored schedule");
				require(approx(arrival.projectedDrawableBefore, 100.0, 1e-5),
					"the projected inventory at resupply is wrong");
				require(approx(arrival.acceptedQuantity, 400.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"a delivery that fits was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"a timely qualified arrival did not read as a rescue");
			}
			require(approx(entry->unmetOutflowTotal,
				2.0 * (10.0 + core::StationForecastHorizonSeconds - 300.0), 1e-3),
				"the uncovered drain after depletion is not reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the projected exchanges do not conserve for the drained store");
			auto const* tankEntry = findForecast(snap.forecast.stores, "tank");
			require(tankEntry != nullptr
				&& tankEntry->basis == core::StationForecastBasis::Sustained
				&& approx(tankEntry->netRateAtStart, 2.0)
				&& approx(tankEntry->projectedDrawableAtHorizon, 700.0, 1e-3),
				"the receiving store did not project as sustained with the delivered total");
			// The forecast mutated nothing: the live store snapshot is
			// unchanged by projection, and a second projection from the
			// same state is identical.
			auto const before = snap.stores;
			auto const again = world.getStationSystemsSnapshot();
			require(again.stores == before,
				"computing the forecast mutated the live store inventory");
			require(again.forecast == snap.forecast,
				"the forecast is not a deterministic function of the committed state");
			require(world.getSimulationTick() == snap.worldTick,
				"computing the forecast advanced the World");
			require(world.getStationSystemsDiagnostics().empty(),
				"the forecast produced a Station diagnostic");
			// Determinism across two identical worlds: the same scenario
			// projects identically, so no hidden stream was consumed.
			core::World twin("ForecastTimelyTwin", 2, 2);
			twin.pauseSimulation(); twin.finishBuild();
			require(twin.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			twin.resumeSimulation();
			runOrDiagnose(twin, 600, "forecast twin run");
			require(twin.getStationSystemsSnapshot().forecast == snap.forecast,
				"two identical scenario runs projected differently");
			// Persistence: the forecast is derived, never stored. A reset
			// re-derives it from the initial state, and a saved and
			// reopened World - which starts from that same initial state -
			// re-derives the identical projection.
			world.pauseSimulation();
			world.resetSimulation();
			auto const reset = world.getStationSystemsSnapshot().forecast;
			require(reset.asOfSeconds == 0.0
				&& approx(reset.stores.size(), 2.0)
				&& approx(findForecast(reset.stores, "water")->drawableAtStart, 200.0, 1e-4)
				&& approx(findForecast(reset.stores, "water")->shortfallOnsetSeconds, 300.0, 1e-5),
				"reset did not re-derive the forecast from the initial state");
			auto const path = context.temporaryRoot() / "forecast-roundtrip.world.yaml";
			world.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the forecast document failed to reopen");
			require(reopened->getStationSystemsSettings() == settings,
				"the station settings did not survive the forecast round trip");
			require(reopened->getStationSystemsSnapshot().forecast == reset,
				"the reopened forecast is not re-derived identically from the initial state");
			require(reopened->getStationSystemsSnapshot().forecast.available,
				"the reopened forecast is not available");
		}
		// Scenario B: delayed resupply. The arrival lands after the
		// projected depletion: the shortfall onset stands, the arrival is
		// not a rescue, and the drain blocked between depletion and
		// arrival is reported as uncovered demand.
		{
			core::World world("ForecastDelayed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 150.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast delayed resupply"); // t = 10 s
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->hasShortfall,
				"a delayed resupply did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 100.0, 1e-5),
				"the depletion onset ignored the delayed arrival");
			require(entry->arrivals.size() == 1 && !entry->arrivals[0].rescuesStore,
				"an arrival after depletion read as a rescue");
			require(approx(entry->arrivals[0].projectedDrawableBefore, 0.0),
				"the projected inventory at a late arrival is not zero");
			require(approx(entry->arrivals[0].acceptedQuantity, 400.0),
				"a late arrival that fits was not accepted");
			// Blocked 100..150 plus blocked again after the second drain
			// ends at 350: the uncovered drain is the whole blocked span.
			require(approx(entry->unmetOutflowTotal,
				2.0 * (50.0 + (10.0 + core::StationForecastHorizonSeconds - 350.0)), 1e-3),
				"the blocked drain around the delayed arrival is not fully reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the delayed-resupply projection does not conserve");
		}
		// Scenario C: accepted delivery quantity is capped by the store's
		// projected capacity; what cannot fit is refused at the boundary
		// and reported, never overflowed.
		{
			core::World world("ForecastCapacity", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 400.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 900.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->arrivals.size() == 1,
				"the capacity-capped arrival is missing");
			require(approx(entry->arrivals[0].acceptedQuantity, 200.0),
				"the accepted quantity is not the projected headroom");
			require(approx(entry->arrivals[0].rejectedQuantity, 700.0),
				"the refused quantity is not reported");
			require(approx(entry->shortfallOnsetSeconds, 200.0, 1e-5),
				"the depletion after the capped arrival is wrong");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the capped-arrival projection does not conserve");
		}
		// Scenario D: a configured crew with a stepped activity profile.
		// The food draw follows the profile, the food store runs out
		// inside the horizon with the crew named as the limiting service,
		// and - matching the live whole-metabolism rule - the urine and
		// organic-waste returns stop together with the draw, so the waste
		// stores freeze at the value the last fed moment produced.
		{
			core::World world("ForecastCrew", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 100.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {50.0, 1.0}};
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 101.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 100000.0, 1.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			// The projection is read at t=0 from the initial state.
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr, "no forecast entry for the crew food store");
			require(approx(food->netRateAtStart, -0.05),
				"the initial food draw is not crew x rate x half-activity");
			require(food->basis == core::StationForecastBasis::Shortfall,
				"the stepping crew draw did not forecast a food shortfall");
			// 13 full periods burn 97.5 kg, the next half-activity leg
			// burns 2.5, and the full-activity leg burns the last 1.0 kg
			// 10 s in: onset 1360 s.
			require(approx(food->shortfallOnsetSeconds, 1360.0, 1e-4),
				"the profiled food depletion onset is wrong");
			require(food->limitingServiceId == "crew"
				&& food->limitingServiceKind == "crew",
				"the limiting service is not the crew");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& urine->basis == core::StationForecastBasis::Sustained
				&& approx(urine->projectedDrawableAtHorizon, 152.5, 1e-3),
				"the urine return did not freeze at the last fed moment");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(waste != nullptr
				&& approx(waste->projectedDrawableAtHorizon, 21.2, 1e-3),
				"the organic-waste return did not freeze at the last fed moment");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr && water->basis == core::StationForecastBasis::Sustained
				&& approx(water->projectedDrawableAtHorizon, 798.0, 1e-3),
				"the crew water draw over the profiled window is wrong");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew-profile projection does not conserve");
			// The live run stays healthy: the forecast is pure observability
			// and the Lua audit is undisturbed.
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast crew live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the crew forecast scenario made the Station unhealthy");
			require(world.getStationSystemsDiagnostics().empty(),
				"the crew forecast scenario produced a diagnostic");
		}
		// Scenario E: uncertainty labels. A store that a crop harvest and
		// a water treatment could replenish is flagged: the possible
		// recovery is visible but is NOT counted as guaranteed, so the
		// projection of the destination stores stays exactly what the
		// declared non-biology flows say.
		{
			core::World world("ForecastUncertainty", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings cropComposition;
			cropComposition.moisture = 0.0;
			cropComposition.nutrientNitrogen = 0.05;
			cropComposition.atmosphericNitrogen = 0.0;
			cropComposition.phosphorus = 0.02;
			cropComposition.potassium = 0.03;
			cropComposition.otherMinerals = 0.05;
			cropComposition.energyKJPerKgDry = 1700.0;
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 40.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingCarbonDioxideKg = 5.0; habitat.startingWaterVapourKg = 1.0;
			habitat.startingTemperatureC = 22.0;
			habitat.thermalCapacityKJPerK = 20000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CropPopulationSettings canopy;
			canopy.id = "canopy"; canopy.atmosphereId = "habitat";
			canopy.waterStoreId = "irrigation";
			canopy.nitrogenStoreId = "fertN"; canopy.phosphorusStoreId = "fertP";
			canopy.potassiumStoreId = "fertK"; canopy.mineralsStoreId = "fertMin";
			canopy.composition = cropComposition;
			canopy.growingAreaM2 = 10.0; canopy.maxBiomassDensityKgPerM2 = 2.0;
			canopy.initialBiomassKg = 1.0;
			canopy.lampRatedKW = 3.6; canopy.illuminationProfile.constantLevel = 1.0;
			canopy.circulationPowerKW = 0.1;
			canopy.growthYieldKgPerKWh = 2.0; canopy.respirationPerHour = 0.5;
			canopy.transpirationKgPerKgGrowth = 5.0;
			canopy.minGrowingTemperatureC = 10.0; canopy.maxGrowingTemperatureC = 35.0;
			canopy.foodStoreId = "food"; canopy.wasteStoreId = "residue";
			canopy.edibleFraction = 0.5; canopy.harvestTriggerKg = 5.0;
			canopy.harvestFraction = 1.0;
			core::WaterTreatmentSettings treatment;
			treatment.id = "polish"; treatment.hazardClass = "normal";
			treatment.sourceStoreId = "raw"; treatment.treatedStoreId = "treated";
			treatment.concentrateStoreId = "concentrate";
			treatment.consumableStoreId = "consumable";
			treatment.throughputKgPerS = 0.005; treatment.recoveryRatio = 0.8;
			treatment.specificEnergyKWhPerKg = 0.5; treatment.consumablePerKg = 0.01;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				plain("irrigation", 200.0, 150.0), plain("fertN", 10.0, 5.0),
				plain("fertP", 10.0, 5.0), plain("fertK", 10.0, 5.0),
				plain("fertMin", 10.0, 5.0),
				plain("food", 1000.0, 100.0), plain("residue", 1000.0, 1.0),
				plain("raw", 1000.0, 100.0), plain("treated", 1000.0, 1.0),
				plain("concentrate", 1000.0, 0.0), plain("consumable", 100.0, 10.0) };
			settings.atmospheres = { habitat };
			settings.cropPopulations = { canopy };
			settings.waterTreatments = { treatment };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->biologyRecoveryPossible,
				"a crop-destined store is not flagged for uncertain biology recovery");
			require(food->basis == core::StationForecastBasis::Sustained
				&& approx(food->netRateAtStart, 0.0)
				&& food->arrivals.empty(),
				"the uncertain harvest was counted into the guaranteed projection");
			auto const* residue = findForecast(snap.forecast.stores, "residue");
			require(residue != nullptr && residue->biologyRecoveryPossible,
				"the crop waste destination is not flagged for uncertain biology");
			auto const* treated = findForecast(snap.forecast.stores, "treated");
			require(treated != nullptr && treated->processProductionPossible,
				"a treatment-destined store is not flagged for unprojected process production");
			require(!treated->biologyRecoveryPossible
				&& treated->basis == core::StationForecastBasis::Sustained,
				"the treatment destination carries the wrong uncertainty mix");
			// #594: the consuming side of the same scenario - the crop's
			// uptake stores and the treatment's source and consumable are
			// drained by pathways the forecast does not project, and must
			// carry the matching consumption flags.
			for (auto const* id : {"irrigation", "fertN", "fertP", "fertK", "fertMin"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->biologyConsumptionPossible
					&& !entry->processConsumptionPossible,
					"a crop uptake store is not flagged for unprojected biology consumption");
			}
			for (auto const* id : {"raw", "consumable"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a treatment feed store is not flagged for unprojected process consumption");
			}
			require(food != nullptr && !food->biologyConsumptionPossible
				&& !food->processConsumptionPossible,
				"the crop food destination carries a consumption flag it does not earn");
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast uncertainty live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the uncertainty forecast scenario made the Station unhealthy");
		}
		// Scenario F: the computation is bounded. A one-second
		// recurrence across the whole horizon cannot make the forecast
		// run forever: collection stops at the declared segment cap and
		// the truncation is reported. #604: a truncated store with no
		// shortfall inside its complete range reports Incomplete, never
		// Sustained, and the snapshot exposes the complete-through bound
		// so the unavailable range beyond it is explicit.
		{
			core::World world("ForecastBounded", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1.0e12; ballast.startingQualified = 1.0;
			settings.stores = { ballast };
			core::ResourceDeliverySettings drip;
			drip.id = "drip"; drip.storeId = "ballast"; drip.unit = "kg";
			drip.quantity = 1.0; drip.firstOccurrenceSeconds = 1.0;
			drip.recurrenceSeconds = 1.0;
			settings.deliveries = { drip };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.truncated,
				"a schedule denser than the segment cap did not truncate the forecast");
			require(snap.forecast.segmentsComputed <= core::StationForecastMaxSegments,
				"the forecast computed more segments than the declared cap");
			require(snap.forecast.completeThroughSeconds > snap.forecast.asOfSeconds
				&& snap.forecast.completeThroughSeconds
					< snap.forecast.asOfSeconds + core::StationForecastHorizonSeconds,
				"a truncated forecast does not report a partial complete-through bound");
			auto const* entry = findForecast(snap.forecast.stores, "ballast");
			require(entry != nullptr
				&& entry->basis == core::StationForecastBasis::Incomplete,
				"a truncated store with no shortfall inside its complete range "
				"did not report Incomplete instead of an unjustified Sustained");
			require(!entry->arrivals.empty(),
				"the truncated projection dropped every already-collected arrival");
			for (auto const& storeEntry : snap.forecast.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= snap.forecast.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
		}
		// Scenario #604-A: the ticket reproduction. A dense unrelated
		// drip (0.1 s recurrence into "noisy") is declared before a
		// one-off 10 kg rescue of the "feed" store, which starts with
		// 2 drawable and drains 1 kg/s to the sink. Without the rescue
		// feed runs out at t=2; with the rescue landing at t=1 it runs
		// out at t=12. The drip still overruns the event budget - the
		// forecast truncates - but the chronological frontier merge
		// keeps the rescue inside the complete-through range, so the
		// depletion onset reads 12 in both declaration orders and
		// matches the sparse control that has no drip at all.
		{
			core::World dense("ForecastDenseRescue", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("noisy", 1.0e9, 1.0),
				chainStores("feed", 100.0, 2.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::ResourceDeliverySettings noise;
			noise.id = "noise"; noise.storeId = "noisy"; noise.unit = "kg";
			noise.quantity = 1.0; noise.firstOccurrenceSeconds = 0.1;
			noise.recurrenceSeconds = 0.1;
			core::ResourceDeliverySettings rescue;
			rescue.id = "rescue"; rescue.storeId = "feed"; rescue.unit = "kg";
			rescue.quantity = 10.0; rescue.firstOccurrenceSeconds = 1.0;
			settings.deliveries = { noise, rescue };
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense drip schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& feed->basis == core::StationForecastBasis::Shortfall,
				"the rescued feed store did not forecast a shortfall");
			require(approx(feed->shortfallOnsetSeconds, 12.0, 1e-3),
				("the dense unrelated schedule dropped the early rescue: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 12").c_str());
			require(feed->arrivals.size() == 1
				&& feed->arrivals[0].deliveryId == "rescue"
				&& approx(feed->arrivals[0].arrivalSeconds, 1.0)
				&& approx(feed->arrivals[0].acceptedQuantity, 10.0)
				&& feed->arrivals[0].rescuesStore,
				"the rescue arrival is missing or misapplied behind the dense schedule");
			require(denseSnap.completeThroughSeconds > 12.0
				&& denseSnap.completeThroughSeconds
					< denseSnap.asOfSeconds + core::StationForecastHorizonSeconds,
				"the truncated forecast does not cover the rescue but stop short of the horizon");
			for (auto const& storeEntry : denseSnap.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= denseSnap.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
			// Declaration permutation: the rescue declared first must
			// project identically - the frontier merge never lets the
			// drip's budget position depend on declaration order.
			settings.deliveries = { rescue, noise };
			core::World permuted("ForecastDenseRescuePermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			require(permutedSnap.truncated, "the permuted forecast stopped truncating");
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the declaration order changed the projected depletion onset");
			// Sparse control: the same rig without the dense drip must
			// project the same depletion onset over the full horizon.
			settings.deliveries = { rescue };
			core::World sparse("ForecastDenseRescueControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			require(!sparseSnap.truncated,
				"the sparse control forecast truncated without a dense schedule");
			require(approx(sparseSnap.completeThroughSeconds,
				sparseSnap.asOfSeconds + sparseSnap.horizonSeconds),
				"a complete forecast does not report complete-through at the horizon");
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-schedule projection disagrees with the sparse control");
		}
		// Scenario #604-B: a dense ejection schedule declared first
		// must not drop a later one-off ejection. The feed store starts
		// with 10 drawable and drains 1 kg/s; a one-off 3 kg ejection
		// at t=1 leaves 6, so depletion lands at t=7. A dense unrelated
		// bilge ejection every 0.1 s overruns the event budget; the
		// merged frontier must still include the one-off whatever the
		// declaration order, matching the sparse control.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("bilge", 1.0e9, 1.0e8),
				chainStores("feed", 100.0, 10.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::WasteEjectionSettings bilgeEject;
			bilgeEject.id = "bilgeEject"; bilgeEject.storeId = "bilge";
			bilgeEject.unit = "kg"; bilgeEject.quantity = 1.0;
			bilgeEject.firstOccurrenceSeconds = 0.1; bilgeEject.recurrenceSeconds = 0.1;
			core::WasteEjectionSettings feedEject;
			feedEject.id = "feedEject"; feedEject.storeId = "feed";
			feedEject.unit = "kg"; feedEject.quantity = 3.0;
			feedEject.firstOccurrenceSeconds = 1.0;
			settings.wasteEjections = { bilgeEject, feedEject };
			core::World dense("ForecastDenseEject", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense ejection schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 7.0, 1e-3),
				("the dense ejection schedule dropped the one-off ejection: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 7").c_str());
			settings.wasteEjections = { feedEject, bilgeEject };
			core::World permuted("ForecastDenseEjectPermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedSnap.truncated && permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the ejection declaration order changed the onset");
			settings.wasteEjections = { feedEject };
			core::World sparse("ForecastDenseEjectControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-ejection projection disagrees with the sparse control");
		}
		// Scenario #604-C: a dense demand profile must not drop another
		// profile's breakpoints. A crew with a maximally dense activity
		// profile (8 segments per second) is declared before an
		// equipment load whose operating profile (period 100 s) steps on
		// at t=50.05 and drains a feed store holding 62 drawable at
		// 1 kg/s. The crew never touches the feed store; only the load's
		// step at 50.05 matters, and the crew's 0.125 s breakpoints
		// cannot stand in for it. The load draws 49.95 kg during
		// [50.05, 100), idles through [100, 150.05) and takes the last
		// 12.05 kg from 150.05, depleting the store at t=162.1. The
		// crew alone overruns the breakpoint budget so the forecast
		// truncates, but the merged frontier must still have collected
		// the load's 50.05 step, putting depletion at t=162.1 exactly
		// as in the sparse control with no crew. The old
		// declaration-order collection dropped every load breakpoint
		// once the crew filled the budget, starting the drain no earlier
		// than the next 0.125 s crew breakpoint and reporting the
		// depletion late.
		{
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto compound = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "denseCrew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 1.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {0.125, 0.6},
				{0.25, 0.7}, {0.375, 0.8}, {0.5, 0.9}, {0.625, 1.0},
				{0.75, 0.9}, {0.875, 0.8}};
			crew.foodStoreId = "rations"; crew.waterStoreId = "potable";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::EquipmentLoadSettings drill;
			drill.id = "drill"; drill.electricalDemandKW = 0.0;
			drill.operatingProfile.periodSeconds = 100.0;
			drill.operatingProfile.segments = {{0.0, 0.0}, {50.05, 1.0}};
			drill.heatOutputKW = 0.0; drill.thermalClass = "low";
			drill.coolingDisposition = core::StationCoolingDisposition::Exported;
			drill.criticalFloorKW = 0.0; drill.priority = 0; drill.maxCurtailment = 1.0;
			core::EquipmentMaterialExchangeSettings draw;
			draw.storeId = "feed"; draw.ratePerSecond = 1.0; draw.consumes = true;
			drill.materialExchanges = { draw };
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("rations", 100000.0, 100000.0, foodComposition),
				compound("potable", 100000.0, 100000.0, waterComposition),
				compound("urine", 100000.0, 1.0, urineComposition),
				compound("organicWaste", 100000.0, 1.0, wasteComposition),
				chainStores("feed", 1000.0, 62.0) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.equipmentLoads = { drill };
			core::World dense("ForecastDenseProfile", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense crew profile did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 162.1, 1e-3),
				("the dense crew profile dropped the load's step breakpoint: onset is "
					+ std::to_string(feed->shortfallOnsetSeconds)
					+ ", expected 162.1").c_str());
			settings.crews.clear();
			settings.atmospheres.clear();
			core::World sparse("ForecastDenseProfileControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-6),
				"the dense-profile projection disagrees with the sparse control");
		}
		// Scenario G: a store that starts depleted reads as a shortfall
		// from the forecast instant, never as sustained supply.
		{
			core::World world("ForecastDepletedStart", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings dry;
			dry.id = "dry"; dry.unit = "kg";
			dry.capacity = 100.0; dry.startingQualified = 0.0;
			core::ResourceStoreSettings held;
			held.id = "held"; held.unit = "kg";
			held.capacity = 100.0; held.startingQualified = 50.0;
			held.protectedReserve = 50.0;
			settings.stores = { dry, held };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* dryEntry = findForecast(snap.forecast.stores, "dry");
			auto const* heldEntry = findForecast(snap.forecast.stores, "held");
			require(dryEntry != nullptr && dryEntry->hasShortfall
				&& approx(dryEntry->shortfallOnsetSeconds, snap.forecast.asOfSeconds),
				"a store with nothing drawable did not forecast an immediate shortfall");
			require(heldEntry != nullptr && heldEntry->hasShortfall
				&& approx(heldEntry->drawableAtStart, 0.0),
				"a store fully inside its protected reserve did not forecast a shortfall");
		}
		// Scenario H: destination backpressure. A transfer into a store
		// that fills inside the horizon stops at the fill-up: the source
		// keeps the material it never got to send, and neither store is
		// a shortage - the capacity limit is the account.
		{
			core::World world("ForecastBackpressure", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings src;
			src.id = "src"; src.unit = "kg";
			src.capacity = 1000.0; src.startingQualified = 200.0;
			core::ResourceStoreSettings dst;
			dst.id = "dst"; dst.unit = "kg";
			dst.capacity = 100.0; dst.startingQualified = 90.0;
			settings.stores = { src, dst };
			core::ResourceTransferSettings feed;
			feed.id = "feed"; feed.sourceId = "src"; feed.destinationId = "dst";
			feed.unit = "kg"; feed.ratePerSecond = 1.0; feed.priority = 1;
			settings.transfers = { feed };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* srcEntry = findForecast(snap.forecast.stores, "src");
			auto const* dstEntry = findForecast(snap.forecast.stores, "dst");
			require(srcEntry != nullptr && srcEntry->basis == core::StationForecastBasis::Sustained
				&& approx(srcEntry->projectedDrawableAtHorizon, 190.0, 1e-3),
				"the source did not keep the material the full destination back-pressured");
			require(approx(srcEntry->unmetOutflowTotal, 0.0),
				"destination backpressure was misreported as a source shortage");
			require(dstEntry != nullptr && dstEntry->basis == core::StationForecastBasis::Sustained
				&& approx(dstEntry->projectedDrawableAtHorizon, 100.0, 1e-3),
				"the backpressured destination did not project exactly full");
			require(std::abs(srcEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6
				&& std::abs(dstEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the backpressure projection does not conserve");
		}
		// Scenario I: a full waste destination back-pressures the whole
		// crew metabolism. Once the urine store fills the crew draws no
		// food or water and returns nothing - the live whole-metabolism
		// rule (#401) - and the held-back food draw is reported as
		// uncovered demand.
		{
			core::World world("ForecastWasteFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 100.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 3.0, 0.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			// Urine fills 3 kg at 0.15 kg/s after 20 s; the crew then
			// stops whole: food stays at 100 - 0.1 x 20 = 98 and the
			// held-back draw is uncovered demand.
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->basis == core::StationForecastBasis::Sustained
				&& approx(food->projectedDrawableAtHorizon, 98.0, 1e-2),
				"the full waste destination did not stop the crew food draw");
			require(approx(food->unmetOutflowTotal,
				0.1 * (core::StationForecastHorizonSeconds - 20.0), 1e-1),
				"the held-back crew draw is not reported as uncovered demand");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& approx(urine->projectedDrawableAtHorizon, 3.0, 1e-2),
				"the urine store did not project exactly to its capacity");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr
				&& approx(water->projectedDrawableAtHorizon, 996.0, 1e-2),
				"the crew water draw did not stop with the metabolism");
		}
		// Scenario J: a transfer chain through a dry intermediate store
		// (#593). A -> B -> C at equal 1 kg/s rates with B starting
		// empty must flow straight through. Gating each transfer on the
		// inventory at the segment start made the solve alternate fill
		// and drain half-duty phases: it truncated the projection at the
		// segment cap, delivered only half the through-flow to C, and
		// invented an unmet transfer demand at B that the live chain
		// never has. The forecast must instead carry the full horizon of
		// through-flow, report no shortage and no uncovered outflow at
		// the dry intermediate, and agree with the live trajectory.
		{
			core::World world("ForecastChainDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available
				&& !snap.forecast.truncated,
				"a sparse constant-rate transfer chain truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a steady through-flow chain needed more than a couple of segments");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(a != nullptr && b != nullptr && c != nullptr,
				"a transfer-chain store is missing from the forecast");
			require(a->netRateAtStart == -1.0
				&& !a->hasShortfall
				&& approx(a->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the chain source did not project a full-horizon drain");
			require(!b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0),
				"the dry intermediate invented an unmet transfer demand");
			require(c->netRateAtStart == 1.0
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the chain end did not receive the full horizon of through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the dry-intermediate chain projection does not conserve");
			// Agreement with the live trajectory: 1200 fixed ticks (20 s)
			// of continuous through-flow, then the forecast from that
			// state starts exactly where the live stores are and continues
			// the same line.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain dry live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "A")->currentQualified, 99980.0, 1e-3)
				&& approx(findStore(live.stores, "B")->currentQualified, 0.0, 1e-3)
				&& approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live dry-intermediate chain did not flow continuously");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the through-flow chain truncated after the live run");
			require(approx(findForecast(f2.stores, "A")->drawableAtStart, 99980.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->drawableAtStart, 20.0, 1e-3),
				"the re-forecast start state does not match the live stores");
			require(approx(findForecast(f2.stores, "A")->projectedDrawableAtHorizon,
				13580.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->projectedDrawableAtHorizon,
					86420.0, 1e-3),
				"the re-forecast did not continue the live through-flow trajectory");
			require(!findForecast(f2.stores, "B")->hasShortfall
				&& approx(findForecast(f2.stores, "B")->unmetOutflowTotal, 0.0),
				"the dry intermediate still invents unmet demand after the live run");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run dry-intermediate chain projection does not conserve");
		}
		// Scenario K: the same chain with the intermediate store starting
		// full. The incoming leg must not stop while the outgoing leg
		// drains it: the through-flow is symmetric with the dry case and
		// C again receives the full horizon.
		{
			core::World world("ForecastChainFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 10.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate through-flow chain truncated the forecast");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(b != nullptr && !b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under symmetric through-flow");
			require(c != nullptr
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the full-intermediate chain end did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate chain projection does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain full live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate chain did not deliver the through-flow");
			require(approx(findForecast(live.forecast.stores, "C")->projectedDrawableAtHorizon,
				findStore(live.stores, "C")->currentQualified + 86400.0, 1e-3),
				"the full-intermediate re-forecast did not continue the live trajectory");
			require(!findForecast(live.forecast.stores, "B")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "B")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario L: projected inventory at a later delivery on a
		// through-flow chain. A drop lands on C at t=40; the forecast
		// taken after the 20 s live run must see C at 20 plus 20 s of
		// continued through-flow (40 kg drawable) at the arrival, accept
		// the whole drop, and read it as a rescue.
		{
			core::World world("ForecastChainDelivery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			core::ResourceDeliverySettings drop;
			drop.id = "drop"; drop.storeId = "C"; drop.unit = "kg";
			drop.quantity = 100.0; drop.firstOccurrenceSeconds = 40.0;
			settings.deliveries = { drop };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain delivery live run"); // t = 20 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(c != nullptr && c->arrivals.size() == 1,
				"the later delivery is missing from the through-flow chain forecast");
			if (c != nullptr && c->arrivals.size() == 1)
			{
				auto const& arrival = c->arrivals[0];
				require(arrival.deliveryId == "drop"
					&& approx(arrival.arrivalSeconds, 40.0),
					"the later delivery lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 40.0, 1e-3),
					"the projected inventory at the later delivery ignored the through-flow");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0)
					&& arrival.rescuesStore,
					"the later delivery was not accepted as a rescue");
			}
			require(approx(c->projectedDrawableAtHorizon, 86520.0, 1e-3),
				"the through-flow chain horizon total did not include the accepted delivery");
			require(std::abs(c->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the through-flow delivery projection does not conserve");
		}
		// Scenario M: generator fuel is projected demand, never
		// sustained inventory (#594). The ticket's minimal generator: a
		// manually commanded 1 kW machine at efficiency 1 burns a
		// 0.001 kWh store dry in 3.6 s. The forecast must report the
		// shortfall with the generator as the limiting service and the
		// post-depletion draw as uncovered demand, and the live run must
		// agree: the fuel really is gone inside the window the forecast
		// named.
		{
			core::World world("ForecastGeneratorFuel", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr,
				"no forecast entry for the generator fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, -1.0 / 3600.0, 1e-12),
					"the commanded generator fuel draw is not in the projected net rate");
				require(fuelEntry->basis == core::StationForecastBasis::Shortfall
					&& fuelEntry->hasShortfall,
					"a finite fuel store under a commanded generator read as sustained");
				require(approx(fuelEntry->shortfallOnsetSeconds, 3.6, 1e-6),
					"the projected fuel depletion onset is wrong");
				require(fuelEntry->limitingServiceId == "gen"
					&& fuelEntry->limitingServiceKind == "generator",
					"the limiting service is not the generator");
				require(approx(fuelEntry->unmetOutflowTotal,
					(core::StationForecastHorizonSeconds - 3.6) / 3600.0, 1e-6),
					"the fuel demand after depletion is not reported as uncovered");
				require(!fuelEntry->processConsumptionPossible
					&& !fuelEntry->biologyConsumptionPossible,
					"a projected manual generator draw must not also read as unprojected consumption");
				require(std::abs(fuelEntry->conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the generator-fuel projection does not conserve");
			}
			// The live run agrees: the whole 0.001 kWh inventory is gone
			// well inside the window the forecast called short, and the
			// re-forecast from the empty store reads an immediate
			// shortfall rather than sustained supply.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "generator fuel live run"); // t = 20 s
			auto const live = world.getStationSystemsSnapshot();
			auto const* liveFuel = findStore(live.stores, "fuel");
			require(liveFuel != nullptr && liveFuel->currentQualified <= 1e-12,
				"the live run did not consume the fuel the forecast projected away");
			auto const* reEntry = findForecast(live.forecast.stores, "fuel");
			require(reEntry != nullptr && reEntry->hasShortfall
				&& approx(reEntry->drawableAtStart, 0.0)
				&& approx(reEntry->shortfallOnsetSeconds, live.forecast.asOfSeconds),
				"the re-forecast of the empty fuel store is not an immediate shortfall");
			require(live.healthy && world.getStationSystemsDiagnostics().empty(),
				"the generator-fuel forecast scenario produced a Station diagnostic");
		}
		// Scenario N: under Automatic dispatch the generator's draw
		// follows the unmodelled net load, so it is not projected - but
		// the fuel store must carry the unprojected-consumption flag so
		// its Sustained basis is never read as a promise (#594).
		{
			core::World world("ForecastGeneratorAuto", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.dispatchMode = core::StationDispatchMode::Automatic;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr, "no forecast entry for the fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, 0.0),
					"an Automatic-dispatch generator must not project a fixed fuel draw");
				require(fuelEntry->processConsumptionPossible,
					"the fuel store under Automatic dispatch is not flagged for unprojected consumption");
				require(!fuelEntry->biologyConsumptionPossible,
					"the generator fuel store carries a biology consumption flag");
			}
			require(snap.forecast.uncertaintyLabel.find("processConsumptionPossible")
				!= std::string::npos,
				"the uncertainty label does not explain the unprojected consuming pathways");
		}
		// Scenario O: every deliberately-excluded consuming pathway
		// flags the stores it drains (#594): electrolysis feed water,
		// fuel-cell reactants and consumed maintenance spares carry
		// processConsumptionPossible; pure product destinations and an
		// uncommanded manual generator's fuel store carry neither flag.
		{
			core::World world("ForecastConsumptionFlags", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings idleGen;
			idleGen.id = "idleGen"; idleGen.fuelStoreId = "diesel";
			idleGen.thermalClass = "generation"; idleGen.heatSinkId = "genSink";
			idleGen.ratedOutputKW = 1.0; idleGen.commandedOutputKW = 0.0;
			idleGen.efficiency = 0.35; idleGen.maxRampKWPerSecond = 60.0;
			core::ThermalReservoirSettings genSink;
			genSink.id = "genSink"; genSink.thermalClass = "generation";
			genSink.heatCapacityKJPerK = 10000.0; genSink.startingTemperatureC = 20.0;
			genSink.minTemperatureC = 0.0; genSink.maxTemperatureC = 100.0;
			core::ElectrolyzerSettings electrolyzer;
			electrolyzer.id = "elektro"; electrolyzer.hazardClass = "normal";
			electrolyzer.waterStoreId = "feedWater";
			electrolyzer.oxygenStoreId = "o2"; electrolyzer.hydrogenStoreId = "h2";
			electrolyzer.thermalClass = "moderate"; electrolyzer.heatSinkId = "sink";
			electrolyzer.rateKgPerS = 0.001;
			core::FuelCellSettings fuelCell;
			fuelCell.id = "cell"; fuelCell.hazardClass = "normal";
			fuelCell.hydrogenStoreId = "cellH2"; fuelCell.oxygenStoreId = "cellO2";
			fuelCell.waterStoreId = "cellWater";
			fuelCell.thermalClass = "moderate"; fuelCell.heatSinkId = "sink";
			fuelCell.ratedOutputKW = 1.0; fuelCell.commandedOutputKW = 0.0;
			core::MaintenanceJobSettings job;
			job.id = "service"; job.installationId = "idleGen";
			job.firstOccurrenceSeconds = 0.0; job.durationSeconds = 10.0;
			core::MaintenanceRequirementSettings spare;
			spare.storeId = "spares"; spare.quantity = 1.0; spare.consumed = true;
			job.requirements = { spare };
			job.wasteStoreId = "usedParts";
			core::ResourceStoreSettings diesel;
			diesel.id = "diesel"; diesel.unit = "kWh";
			diesel.capacity = 100.0; diesel.startingQualified = 50.0;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				diesel,
				plain("feedWater", 100.0, 50.0),
				plain("o2", 100.0, 1.0), plain("h2", 100.0, 1.0),
				plain("cellH2", 100.0, 10.0), plain("cellO2", 100.0, 10.0),
				plain("cellWater", 100.0, 0.0),
				plain("spares", 10.0, 5.0), plain("usedParts", 10.0, 0.0) };
			settings.thermalReservoirs = { sink, genSink };
			settings.generators = { idleGen };
			settings.electrolyzers = { electrolyzer };
			settings.fuelCells = { fuelCell };
			settings.maintenanceJobs = { job };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const requireProcess = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible,
					"a store feeding an unprojected consuming pathway is not flagged");
				require(!entry->biologyConsumptionPossible,
					"a process-feed store carries a biology consumption flag"); };
			requireProcess("feedWater");
			requireProcess("cellH2");
			requireProcess("cellO2");
			requireProcess("spares");
			// Product destinations and the uncommanded manual generator's
			// fuel store carry no omitted drain.
			auto const requireClean = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && !entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a store with no omitted consuming pathway carries a consumption flag"); };
			requireClean("diesel");
			requireClean("o2");
			requireClean("h2");
			requireClean("cellWater");
			requireClean("usedParts");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "consumption-flags live run");
			require(world.getStationSystemsSnapshot().healthy
				&& world.getStationSystemsDiagnostics().empty(),
				"the consumption-flag scenario produced a Station diagnostic");
		}
		// Scenario N: exactly-timely resupply (#595). The store runs dry
		// at the very instant the delivery lands; because arrivals run
		// ahead of the tick's drains, that delivery prevents the t=10
		// shortage and the first projected shortfall is the later one.
		// The arrival must classify as a rescue even though the drawable
		// immediately before it is zero - the old drawable-positive rule
		// called it late while the same forecast put the onset at t=110.
		{
			core::World world("ForecastExactResupply", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings exact;
			exact.id = "exact"; exact.storeId = "A"; exact.unit = "kg";
			exact.quantity = 100.0; exact.firstOccurrenceSeconds = 10.0;
			settings.deliveries = { exact };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && snap.forecast.asOfSeconds == 0.0,
				"the exactly-timely forecast was not read from the paused t=0 state");
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall
				&& a->basis == core::StationForecastBasis::Shortfall,
				"the exactly-timely resupply store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 110.0, 1e-5),
				"the exactly-timely delivery did not shift the shortfall onset past itself");
			require(a->arrivals.size() == 1, "the exactly-timely delivery is missing");
			if (a != nullptr && a->arrivals.size() == 1)
			{
				auto const& arrival = a->arrivals[0];
				require(arrival.deliveryId == "exact"
					&& approx(arrival.arrivalSeconds, 10.0),
					"the exactly-timely arrival lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 0.0, 1e-9),
					"the exactly-timely arrival did not see a zero drawable");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"the exactly-timely delivery was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"an exactly-timely zero-drawable arrival was not a rescue");
			}
			require(approx(a->unmetOutflowTotal,
				core::StationForecastHorizonSeconds - 110.0, 1e-2),
				"the drain after the exactly-timely rescue is not reported as uncovered");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the exactly-timely resupply projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 110.0, 1e-3),
				"the receiving store did not collect the rescued through-flow total");
		}
		// Scenario O: genuinely late and multiple deliveries after a
		// previous depletion (#595). The store runs dry at t=10; a 100 kg
		// delivery lands at t=20 and a 50 kg delivery at t=60. Neither is
		// a rescue: the t=20 arrival lands after the shortfall with a
		// zero drawable, and the t=60 arrival - despite a positive 60 kg
		// drawable restored by the earlier delivery - still lands after
		// the shortage that already occurred at t=10.
		{
			core::World world("ForecastLateDeliveries", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings late;
			late.id = "late"; late.storeId = "A"; late.unit = "kg";
			late.quantity = 100.0; late.firstOccurrenceSeconds = 20.0;
			core::ResourceDeliverySettings later;
			later.id = "later"; later.storeId = "A"; later.unit = "kg";
			later.quantity = 50.0; later.firstOccurrenceSeconds = 60.0;
			settings.deliveries = { late, later };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall,
				"the late-delivery store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 10.0, 1e-5),
				"the shortfall onset moved even though every arrival is later");
			require(a->arrivals.size() == 2,
				"the late deliveries are not both in the forecast");
			if (a != nullptr && a->arrivals.size() == 2)
			{
				auto const& first = a->arrivals[0];
				require(first.deliveryId == "late"
					&& approx(first.arrivalSeconds, 20.0)
					&& approx(first.projectedDrawableBefore, 0.0, 1e-9)
					&& approx(first.acceptedQuantity, 100.0)
					&& !first.rescuesStore,
					"the genuinely late t=20 arrival did not read as after-the-shortfall");
				auto const& second = a->arrivals[1];
				require(second.deliveryId == "later"
					&& approx(second.arrivalSeconds, 60.0)
					&& approx(second.projectedDrawableBefore, 60.0, 1e-3)
					&& approx(second.acceptedQuantity, 50.0)
					&& !second.rescuesStore,
					"a delivery after a previous depletion read as a rescue on its positive drawable");
			}
			require(approx(a->unmetOutflowTotal,
				10.0 + (core::StationForecastHorizonSeconds - 170.0), 1e-2),
				"the blocked drain around the late deliveries is not fully reported");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the late-delivery projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 160.0, 1e-3),
				"the receiving store did not collect the late-delivery through-flow total");
		}
		// Scenario P: equipment-to-transfer through-flow at a dry
		// intermediate store (#605), the ticket fixture. A producer
		// delivers 1 kg/s into a dry 1 kg buffer and a transfer drains
		// the buffer at 1 kg/s. The live station flows straight through;
		// the forecast gated the transfer on the inventory at the
		// segment start and the equipment on the destination at the
		// segment start, so the solve alternated fill/drain half-duty
		// phases: it burned the whole segment cap, invented a buffer
		// shortage and delivered only a fraction of the through-flow.
		// The solve now settles every pathway together: no shortage, no
		// truncation, a segment count bounded well below the cap and
		// independent of the horizon, full-horizon throughput at the
		// sink, closed conservation, and agreement with the live
		// trajectory.
		{
			core::World world("ForecastEquipmentThroughDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("buffer", 1.0, 0.0),
				chainStores("sink", 100000.0, 0.0) };
			core::EquipmentLoadSettings producer;
			producer.id = "producer";
			producer.electricalDemandKW = 1.0;
			producer.thermalClass = "warm";
			producer.materialExchanges = {{"buffer", 1.0, false}};
			settings.equipmentLoads = {producer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			settings.transfers = {
				chainTransfer("drain", "buffer", "sink", 1.0, 0) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && !snap.forecast.truncated,
				"the equipment-to-transfer through-flow truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a constant equipment through-flow needed more than a couple of segments");
			auto const* buffer = findForecast(snap.forecast.stores, "buffer");
			auto const* sink = findForecast(snap.forecast.stores, "sink");
			require(buffer != nullptr && sink != nullptr,
				"an equipment-through-flow store is missing from the forecast");
			require(buffer != nullptr && !buffer->hasShortfall
				&& approx(buffer->netRateAtStart, 0.0)
				&& approx(buffer->unmetOutflowTotal, 0.0),
				"the equipment-fed dry intermediate invented a shortage");
			require(sink != nullptr
				&& approx(sink->netRateAtStart, 1.0)
				&& approx(sink->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the sink did not receive the full horizon of equipment through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the equipment-through-flow projection does not conserve");
			// Agreement with the live trajectory: the producer adds
			// 1/60 kg per tick and the transfer moves it out the same
			// tick, leaving the buffer empty.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "equipment through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "buffer")->currentQualified, 0.0, 1e-9)
				&& approx(findStore(live.stores, "sink")->currentQualified, 20.0, 1e-3),
				"the live equipment-to-transfer chain did not flow straight through");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the equipment through-flow truncated after the live run");
			require(!findForecast(f2.stores, "buffer")->hasShortfall
				&& approx(findForecast(f2.stores, "buffer")->unmetOutflowTotal, 0.0),
				"the equipment-fed intermediate still invents demand after the live run");
			require(approx(findForecast(f2.stores, "sink")->projectedDrawableAtHorizon,
				86420.0, 1e-3),
				"the re-forecast did not continue the equipment through-flow trajectory");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run equipment-through-flow projection does not conserve");
		}
		// Scenario Q: the dual boundary - a full intermediate store
		// supplied by a transfer and drained by equipment consumption
		// (#605). The incoming leg must not stop while the equipment
		// drains: the through-flow is symmetric with the dry case and
		// the intermediate holds steady at capacity for the horizon.
		{
			core::World world("ForecastEquipmentThroughFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("source", 1.0e6, 100000.0),
				chainStores("mid", 10.0, 10.0),
				chainStores("product", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("fill", "source", "mid", 1.0, 1) };
			core::EquipmentLoadSettings drainer;
			drainer.id = "drainer";
			drainer.electricalDemandKW = 1.0;
			drainer.thermalClass = "warm";
			drainer.materialExchanges = {
				{"mid", 1.0, true}, {"product", 1.0, false} };
			settings.equipmentLoads = {drainer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate equipment through-flow chain truncated the forecast");
			auto const* mid = findForecast(snap.forecast.stores, "mid");
			auto const* source = findForecast(snap.forecast.stores, "source");
			auto const* product = findForecast(snap.forecast.stores, "product");
			require(mid != nullptr && !mid->hasShortfall
				&& approx(mid->netRateAtStart, 0.0)
				&& approx(mid->unmetOutflowTotal, 0.0)
				&& approx(mid->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under equipment through-flow");
			require(source != nullptr
				&& approx(source->netRateAtStart, -1.0)
				&& approx(source->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the transfer source did not project the full-horizon through-flow drain");
			require(product != nullptr
				&& approx(product->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the equipment product store did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate equipment through-flow does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "full-intermediate equipment live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "mid")->currentQualified, 10.0, 1e-3)
				&& approx(findStore(live.stores, "product")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate equipment chain did not hold the store steady");
			require(!findForecast(live.forecast.stores, "mid")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "mid")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario R: shared-feed competition at a dry store (#605).
		// One producer feeds 1 kg/s into a dry store and two equipment
		// loads each want 1 kg/s from it. Only one load can run: the
		// other stops whole and its demand stays uncovered - the
		// forecast must never promise the same kilogram twice.
		{
			core::World world("ForecastSharedFeed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("feed", 1.0, 0.0),
				chainStores("out1", 1.0e6, 0.0),
				chainStores("out2", 1.0e6, 0.0) };
			core::EquipmentLoadSettings supplier;
			supplier.id = "supplier";
			supplier.electricalDemandKW = 1.0;
			supplier.thermalClass = "warm";
			supplier.materialExchanges = {{"feed", 1.0, false}};
			core::EquipmentLoadSettings consumer1;
			consumer1.id = "consumer1";
			consumer1.electricalDemandKW = 1.0;
			consumer1.thermalClass = "warm";
			consumer1.materialExchanges = {
				{"feed", 1.0, true}, {"out1", 1.0, false} };
			core::EquipmentLoadSettings consumer2;
			consumer2.id = "consumer2";
			consumer2.electricalDemandKW = 1.0;
			consumer2.thermalClass = "warm";
			consumer2.materialExchanges = {
				{"feed", 1.0, true}, {"out2", 1.0, false} };
			settings.equipmentLoads = {supplier, consumer1, consumer2};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the shared-feed competition truncated the forecast");
			auto const* feed = findForecast(snap.forecast.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 0.0),
				"the over-subscribed dry feed did not read as short from the start");
			require(approx(feed->unmetOutflowTotal,
				core::StationForecastHorizonSeconds, 1e-2),
				"the losing consumer's demand is not fully reported as uncovered");
			// Exactly one consumer runs: one output store collects the
			// full horizon of through-flow and the other stays empty.
			auto const* out1 = findForecast(snap.forecast.stores, "out1");
			auto const* out2 = findForecast(snap.forecast.stores, "out2");
			require(out1 != nullptr && out2 != nullptr, "a consumer output is missing");
			if (out1 != nullptr && out2 != nullptr)
			{
				auto const one = out1->projectedDrawableAtHorizon;
				auto const other = out2->projectedDrawableAtHorizon;
				require((approx(one, 86400.0, 1e-3) && approx(other, 0.0, 1e-9))
					|| (approx(other, 86400.0, 1e-3) && approx(one, 0.0, 1e-9)),
					"the shared feed did not run exactly one consumer at full rate");
			}
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the shared-feed competition does not conserve");
		}
		// Scenario S: crew metabolism through-flow at dry food and
		// water stores (#605). The crew's food and water stores start
		// empty but transfers cover the draws concurrently, so the
		// metabolism runs at full rate for the whole horizon: the
		// waste streams accumulate at the full crew rate and the dry
		// food stores are a covered pass-through, not a shortage.
		{
			core::World world("ForecastCrewThroughFlow", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto const compound = [](std::string const& id, double capacity,
				double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("bulkFood", 1.0e6, 100000.0, foodComposition),
				compound("bulkWater", 1.0e6, 100000.0, waterComposition),
				compound("food", 1.0, 0.0, foodComposition),
				compound("water", 1.0, 0.0, waterComposition),
				compound("urine", 1.0e6, 0.0, urineComposition),
				compound("organicWaste", 1.0e6, 0.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.transfers = {
				chainTransfer("foodFill", "bulkFood", "food", 0.1, 1),
				chainTransfer("waterFill", "bulkWater", "water", 0.2, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the crew through-flow forecast truncated or churned segments");
			auto const* food = findForecast(snap.forecast.stores, "food");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(food != nullptr && !food->hasShortfall
				&& approx(food->netRateAtStart, 0.0)
				&& approx(food->unmetOutflowTotal, 0.0),
				"the covered dry food store invented a crew food shortage");
			require(water != nullptr && !water->hasShortfall
				&& approx(water->netRateAtStart, 0.0)
				&& approx(water->unmetOutflowTotal, 0.0),
				"the covered dry water store invented a crew water shortage");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(urine != nullptr
				&& approx(urine->netRateAtStart, 0.15)
				&& approx(urine->projectedDrawableAtHorizon, 12960.0, 1e-2),
				"the covered crew did not project full-rate urine");
			require(waste != nullptr
				&& approx(waste->netRateAtStart, 0.02)
				&& approx(waste->projectedDrawableAtHorizon, 1728.0, 1e-2),
				"the covered crew did not project full-rate organic waste");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew through-flow projection does not conserve");
			// The live station agrees: the crew eats what the transfers
			// deliver each tick and the waste streams accumulate at the
			// full crew rate (minus the first tick the empty food store
			// costs the metabolism).
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "crew through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "food")->currentQualified, 0.1 / 60.0, 1e-9)
				&& approx(findStore(live.stores, "urine")->currentQualified,
					0.15 * (1199.0 / 60.0), 1e-2),
				"the live crew did not run on the delivered through-flow");
			require(!findForecast(live.forecast.stores, "food")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "food")->unmetOutflowTotal, 0.0),
				"the covered crew invents unmet demand after the live run");
		}
		// Scenario T: a closed equipment cycle between two dry stores
		// cannot bootstrap itself (#605). Load X consumes A and
		// produces B; load Y consumes B and produces A; both stores
		// start empty with no external supply. Neither load is
		// supported: both stop whole, their feed demand stays
		// uncovered, and no phantom material appears anywhere.
		{
			core::World world("ForecastCycleDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 10.0, 0.0),
				chainStores("B", 10.0, 0.0) };
			core::EquipmentLoadSettings x;
			x.id = "x";
			x.thermalClass = "warm";
			x.materialExchanges = {{"A", 1.0, true}, {"B", 1.0, false}};
			core::EquipmentLoadSettings y;
			y.id = "y";
			y.thermalClass = "warm";
			y.materialExchanges = {{"B", 1.0, true}, {"A", 1.0, false}};
			settings.equipmentLoads = {x, y};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the unsupported equipment cycle churned the segment budget");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(a != nullptr && a->hasShortfall
				&& approx(a->shortfallOnsetSeconds, 0.0)
				&& approx(a->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(a->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store A instead of reporting its demand uncovered");
			require(b != nullptr && b->hasShortfall
				&& approx(b->shortfallOnsetSeconds, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(b->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store B instead of reporting its demand uncovered");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the unsupported equipment cycle does not conserve");
		}
	}

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks{{"outages", outages}, {"maintenance", maintenance}, {"failures", failures}, {"dispatch", dispatch}, {"protectedServices", protectedServices}, {"recovery", recovery}, {"trends", trends}, {"endurance", endurance}, {"forecast", forecast}};
	registerStationRecoveryChecks(checks);
	registerStationBiologyChecks(checks);
	registerStationThermalChecks(checks);
	registerStationGenerationChecks(checks);
	registerStationAtmosphereChecks(checks);
	registerStationWaterChecks(checks);
	registerStationCompoundPowerChecks(checks);
	registerStationBatteryChecks(checks);
	registerStationLogisticsChecks(checks);
	registerStationCoreChecks(checks);
	registerStationPortChecks(checks);
	registerStationSeamChecks(checks);
	registerStationPowerChecks(checks);
	registerStationDocumentChecks(checks);
	registerScheduledTransferChecks(checks);
	registerStationConnectionChecks(checks);
	registerBatteryObservationChecks(checks);
	registerObservationIntegrationChecks(checks);
	registerMaterialObservationChecks(checks);
	registerCoupledStationChecks(checks);
	registerReferenceStationChecks(checks);
	registerScenarioChecks(checks);
	registerStabilityChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
