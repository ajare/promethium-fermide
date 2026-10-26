#include "StationRig.h"
#include "Smoke.h"
#include "core/StationPowerBuses.h"

namespace
{
	using namespace smoke::station;
	using core::StationBusPowerSnapshot;
	StationBusPowerSnapshot const& bus(core::StationSystemsSnapshot const& s, std::string_view id)
	{
		auto found = std::find_if(s.power.buses.begin(), s.power.buses.end(), [&](auto const& b) { return b.busId == id; });
		require(found != s.power.buses.end(), "missing bus snapshot");
		return *found;
	}
	void near(double actual, double expected, char const* message)
	{
		require(std::abs(actual - expected) < 1e-8,
			(std::string(message) + ": actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected)).c_str());
	}
	core::StationSystemsSettings twoBuses()
	{
		core::StationSystemsSettings s;
		s.enabled = true; s.systemsStepWorldTicks = 1;
		s.powerBuses = {{"main", "Science", 100, 0}, {"backup", "Emergency", 0, 0}};
		for (auto const& id : {"main", "backup"})
		{
			core::SolarArraySettings a;
			a.id = id; a.capacityKW = id == std::string("main") ? 20 : 3;
			a.illumination.constantLevel = 1; a.busIds = {id}; s.solarArrays.push_back(a);
			core::EquipmentLoadSettings l;
			l.id = id; l.electricalDemandKW = 8; l.operatingProfile.constantLevel = 1; l.thermalClass = "moderate";
			l.maxCurtailment = 1; l.supplyBusIds = {id}; s.equipmentLoads.push_back(l);
		}
		return s;
	}
	void balance(smoke::Context const&)
	{
		auto s = twoBuses();
		core::StationSystemsRuntime runtime(s, 1.0f), replay(s, 1.0f);
		for (double time : {1.0, 5.0, 20.0})
		{
			require(runtime.advanceTo(time) && replay.advanceTo(time), "two-bus drive failed");
			auto const snap = runtime.snapshot();
			require(snap.healthy && snap.power == replay.snapshot().power, "bus replay diverged");
			near(bus(snap, "main").suppliedKW, 8, "main supply changed");
			near(bus(snap, "main").surplusKW, 12, "main surplus changed");
			near(bus(snap, "backup").suppliedKW, 3, "surplus rescued another bus");
			near(bus(snap, "backup").unmetKW, 5, "bus unmet hidden");
			near(bus(snap, "main").residualKW, 0, "main audit drift");
			near(bus(snap, "backup").residualKW, 0, "backup audit drift");
			near(snap.power.totalSuppliedKW, 11, "station supply not a rollup");
		}
		for (auto* run : {&runtime, &replay})
			require(run->updateNode<core::SolarArraySettings>("backup", [](auto& source) { source.capacityKW = 0; }), "source perturbation refused");
		require(runtime.advanceTo(21) && replay.advanceTo(21), "perturbed replay drive failed");
		require(runtime.snapshot().power == replay.snapshot().power, "identical perturbations did not replay");
		near(bus(runtime.snapshot(), "backup").suppliedKW, 0, "source loss rescued across buses");
		// One source with two landings still has just one feeder rating.
		s.solarArrays.resize(1); s.solarArrays[0].capacityKW = 12;
		s.solarArrays[0].busIds = {"main", "backup"};
		for (auto& load : s.equipmentLoads) load.criticalFloorKW = 2;
		core::StationSystemsRuntime shared(s, 1.0f);
		require(shared.advanceTo(5), "shared-source drive failed");
		auto const sharedSnap = shared.snapshot();
		near(sharedSnap.power.totalSuppliedKW, 12, "shared source duplicated or stranded capacity");
		near(bus(sharedSnap, "main").unmetCriticalKW + bus(sharedSnap, "backup").unmetCriticalKW, 0, "shared source failed protected-first");
		near(bus(sharedSnap, "main").generationKW + bus(sharedSnap, "backup").generationKW, 12, "shared feeder counted twice");
		s = twoBuses();
		// A pulse inside one Station interval must be averaged per circuit,
		// not reported as just the last interval's allocation.
		core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
		s.systemsStepWorldTicks = 6;
		s.solarArrays[1].illumination.periodSeconds = 6;
		s.solarArrays[1].illumination.segments = {{0, 1}, {3, 0}};
		core::StationSystemsRuntime split(s, 1.0f);
		require(split.advanceTo(6), "split bus drive failed");
		auto const snap = split.snapshot();
		near(bus(snap, "backup").generationKW, 1.5, "split generation not averaged");
		near(bus(snap, "backup").suppliedKW, 1.5, "split supply not averaged");
		near(bus(snap, "backup").residualKW, 0, "split audit drift");
		// A later sub-interval fails AFTER an earlier one has committed
		// allocation scratch. Rollback must restore the previous bus account
		// and dispatch feedback, not publish the successful partial interval.
		s = twoBuses(); s.systemsStepWorldTicks = 6;
		s.stores = {{"tank", "kg", 1e308, 0, 0}};
		s.deliveries = {{"huge", "tank", "kg", 1e308, 7, 2}};
		s.wasteEjections = {{"drain", "tank", "kg", 1e308, 8}};
		core::StationSystemsRuntime rollback(s, 1.0f);
		require(rollback.advanceTo(6), "rollback baseline failed");
		auto const before = rollback.snapshot().power;
		require(!rollback.advanceTo(12), "overflowing split committed");
		auto const after = rollback.snapshot().power;
		require(before.buses == after.buses && before.dispatchDemandKW == after.dispatchDemandKW,
			"failed split leaked bus allocation or dispatch feedback");
	}
	void protectedFirst(smoke::Context const&)
	{
		for (double supply : {0.0, 3.0})
		{
			auto s = twoBuses(); s.solarArrays[1].capacityKW = supply;
			core::ThermalReservoirSettings reservoir;
			reservoir.id = "sink"; reservoir.thermalClass = "moderate";
			reservoir.heatCapacityKJPerK = 100000; reservoir.startingTemperatureC = 20;
			s.thermalReservoirs = {reservoir};
			core::CoolantLoopSettings loop;
			loop.id = "protected"; loop.reservoirId = "sink";
			loop.coolantCapacityKg = 10; loop.startingCoolantKg = 10; loop.specificHeatKJPerKgK = 4;
			loop.startingTemperatureC = 20; loop.ratedFlowKgPerS = 1;
			loop.pumpPowerKW = 2; loop.criticalService = true;
			loop.supplyBusIds = {"backup"}; s.coolantLoops = {loop};
			core::StationSystemsRuntime runtime(s, 1.0f);
			require(runtime.advanceTo(5), "protected bus drive failed");
			auto const snap = runtime.snapshot(); auto const& b = bus(snap, "backup");
			near(b.protectedDemandKW, 2, "protected demand on wrong bus");
			near(b.protectedSuppliedKW, std::min(2.0, supply), "protected service did not draw first");
			near(b.protectedUnmetKW, std::max(0.0, 2 - supply), "protected shortfall hidden");
			near(b.suppliedKW, supply, "backup took main surplus");
			near(b.throughPowerKW, supply, "internal service omitted from through-power");
			near(b.demandKW, 10, "internal service omitted from demand");
			require(b.protectedServices.size() == 1, "protected service attribution lost");
			if (supply == 0)
				require(b.unmetCriticalKW == 2 && hasAlert(snap, "protectedUnmetKW"), "cross-bus surplus cleared critical alarm");
			near(bus(snap, "main").protectedDemandKW, 0, "protected demand leaked to main");
		}
	}
	void batteries(smoke::Context const&)
	{
		auto s = twoBuses(); s.solarArrays[1].capacityKW = 0;
		for (auto const& id : {"main", "backup"})
		{
			core::BatterySettings b;
			b.id = id; b.nameplateKWh = 10; b.usableKWh = 10;
			b.startingStoredKWh = id == std::string("main") ? 0 : 5;
			b.maxChargeKW = 5; b.maxDischargeKW = 10;
			b.chargeEfficiency = 1; b.dischargeEfficiency = 1;
			b.busIds = {id}; s.batteries.push_back(b);
		}
		core::StationSystemsRuntime runtime(s, 1.0f);
		require(runtime.advanceTo(5), "battery bus drive failed");
		auto const snap = runtime.snapshot();
		near(bus(snap, "main").batteryChargeKW, 5, "main battery did not charge from local surplus");
		near(bus(snap, "main").batteryDischargeKW, 0, "main battery rescued backup");
		near(bus(snap, "backup").batteryDischargeKW, 8, "backup battery did not fund local load");
		near(bus(snap, "backup").batteryChargeKW, 0, "backup battery charged from main surplus");
		near(bus(snap, "main").residualKW, 0, "charging audit drift");
		near(bus(snap, "backup").residualKW, 0, "discharge audit drift");
		// An empty backup battery cannot be filled from another circuit.
		s.batteries[1].startingStoredKWh = 0;
		core::StationSystemsRuntime empty(s, 1.0f);
		require(empty.advanceTo(5), "empty battery bus drive failed");
		near(bus(empty.snapshot(), "backup").suppliedKW, 0, "empty battery bridged unrelated buses");
	}
	void sourceDispatch(smoke::Context const&)
	{
		auto s = twoBuses(); s.solarArrays[1].capacityKW = 0;
		s.dispatchMode = core::StationDispatchMode::Automatic;
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "generation";
		sink.heatCapacityKJPerK = 1000000; sink.startingTemperatureC = 20;
		s.thermalReservoirs = {sink};
		core::ResourceStoreSettings fuel;
		fuel.id = "fuel"; fuel.unit = "kWh"; fuel.capacity = 100; fuel.startingQualified = 100;
		s.stores = {fuel};
		core::GeneratorSettings gen;
		gen.id = "gen"; gen.fuelStoreId = "fuel"; gen.heatSinkId = "sink";
		gen.thermalClass = "generation"; gen.ratedOutputKW = 20; gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 1000; gen.maxSinkTemperatureC = 90;
		gen.busIds = {"backup"}; gen.supplyBusIds = {"backup"};
		s.generators = {gen};
		core::StationSystemsRuntime runtime(s, 1.0f);
		require(runtime.advanceTo(5), "automatic bus drive failed");
		auto const snap = runtime.snapshot();
		near(bus(snap, "backup").suppliedKW, 8, "main surplus suppressed backup generation");
		near(snap.power.dispatchTargetKW, 8, "dispatch target pooled unrelated solar");
		near(snap.power.dispatchShortfallKW, 0, "local generator did not close target");
		// A source connected only to the wrong bus must not be commanded
		// to cover this deficit, even when its fuel is available.
		s.generators[0].busIds = {"main"};
		core::StationSystemsRuntime wrong(s, 1.0f);
		require(wrong.advanceTo(5), "wrong-bus generator drive failed");
		near(wrong.snapshot().power.generators[0].dispatchCommandKW, 0, "wrong-bus generator was commanded");
		near(bus(wrong.snapshot(), "backup").suppliedKW, 0, "generator rescued unrelated bus");

		// Engine acceptance must likewise see only its own circuit's
		// current demand, not global spare or a different bus's demand.
		s.generators.clear(); s.stores.clear(); s.dispatchMode = core::StationDispatchMode::Manual;
		s.thermalReservoirs[0].thermalClass = "warm";
		auto hot = s.thermalReservoirs[0]; hot.id = "hot"; hot.startingTemperatureC = 90;
		hot.maxTemperatureC = 200; s.thermalReservoirs.push_back(hot);
		core::HeatEngineSettings engine;
		engine.id = "engine"; engine.hotReservoirId = "hot"; engine.rejectionReservoirId = "sink";
		engine.thermalClass = "warm"; engine.ratedThermalInputKW = 100; engine.commandedInputKW = 100;
		engine.carnotFraction = 0.5; engine.auxiliaryPowerKW = 1;
		engine.minHotTemperatureC = 30; engine.maxSinkTemperatureC = 25; engine.busIds = {"backup"};
		s.heatEngines = {engine};
		core::StationSystemsRuntime engineRun(s, 1.0f);
		require(engineRun.advanceTo(1), "engine bus drive failed");
		near(bus(engineRun.snapshot(), "backup").suppliedKW, 8, "unrelated solar suppressed engine acceptance");
		s.equipmentLoads[1].electricalDemandKW = 0; s.solarArrays[0].capacityKW = 0;
		core::StationSystemsRuntime idle(s, 1.0f);
		require(idle.advanceTo(1), "idle engine bus drive failed");
		near(idle.snapshot().heatEngines[0].lastElectricalOutputKW, 0, "engine converted heat for unrelated demand");
	}
	void recoveryReserve(smoke::Context const&)
	{
		auto s = twoBuses();
		s.solarArrays[0].capacityKW = 12;
		s.solarArrays[0].illumination.periodSeconds = 10;
		s.solarArrays[0].illumination.segments = {{0, 0}, {2, 1}};
		s.solarArrays[1].capacityKW = 20;
		auto& load = s.equipmentLoads[0];
		load.maxCurtailment = 0; load.recoveryDwellSeconds = 0;
		load.recoveryHysteresisFraction = 0; load.restartSurgePowerKW = 4; load.restartSurgeSeconds = 2;
		for (auto const& id : {"main", "backup"})
		{
			core::BatterySettings b; b.id = id; b.busIds = {id};
			b.nameplateKWh = 10; b.usableKWh = 10; b.startingStoredKWh = 0;
			b.maxChargeKW = 5; b.maxDischargeKW = 0; b.chargeEfficiency = 1; b.dischargeEfficiency = 1;
			s.batteries.push_back(b);
		}
		core::StationSystemsRuntime runtime(s, 1.0f);
		require(runtime.advanceTo(3), "recovery bus drive failed");
		auto const snap = runtime.snapshot();
		require(snap.power.recoveryPending, "test lost pending restart");
		near(bus(snap, "main").batteryChargeKW, 0, "battery stole local restart headroom");
		near(bus(snap, "backup").batteryChargeKW, 5, "restart reserve suppressed unrelated charging");
		near(bus(snap, "main").residualKW, 0, "recovery reserve ledger drift");
	}
	void internalConsumers(smoke::Context const&)
	{
		auto baseline = core::bundledStationReference().settings;
		auto isolated = baseline;
		isolated.powerBuses.push_back({"unused", "Unrelated surplus", 0, 0});
		core::SolarArraySettings extra; extra.id = "unrelated"; extra.capacityKW = 100000;
		extra.illumination.constantLevel = 1; extra.busIds = {"unused"};
		isolated.solarArrays.push_back(extra);
		core::StationSystemsRuntime original(baseline, 1.0f), split(isolated, 1.0f);
		require(original.advanceTo(10) && split.advanceTo(10), "internal bus drive failed");
		auto const a = original.snapshot(), b = split.snapshot();
		near(bus(a, "main").demandKW, bus(b, "main").demandKW, "unrelated surplus changed internal demand");
		near(bus(a, "main").suppliedKW, bus(b, "main").suppliedKW, "internal consumers used unrelated surplus");
		near(bus(b, "unused").suppliedKW, 0, "internal draw landed on unrelated bus");
		// Strand every internal consumer while leaving all producers and
		// storage on main. None may spend the healthy main supply.
		core::forEachStationPowerMember(isolated, [&](auto& entry) {
			if constexpr (requires { entry.supplyBusIds; }) entry.supplyBusIds = {"unused"};
		});
		isolated.solarArrays.pop_back();
		core::StationSystemsRuntime stranded(isolated, 1.0f);
		require(stranded.advanceTo(5), "stranded internal drive failed");
		auto const snap = stranded.snapshot();
		near(bus(snap, "unused").suppliedKW, 0, "internal consumer rescued across buses");
		require(bus(snap, "unused").demandKW > 0 && bus(snap, "unused").protectedUnmetKW > 0,
			"stranded internal demand disappeared");
		near(bus(snap, "main").residualKW, 0, "internal main audit drift");
		near(bus(snap, "unused").residualKW, 0, "internal stranded audit drift");
	}
}
namespace
{
	void breakers(smoke::Context const&)
	{
		auto s = twoBuses();
		s.powerBuses[0].ratedCapacityKW = 5;
		s.powerBuses[0].tripPersistenceSeconds = 3;
		s.equipmentLoads[0].criticalFloorKW = 4;
		core::StationSystemsRuntime run(s, 1.0f);
		std::string diagnostic;
		require(!run.breakerReset("main", &diagnostic) && !diagnostic.empty()
			&& !run.breakerReset("missing"), "invalid BreakerReset accepted");
		require(run.advanceTo(2) && !bus(run.snapshot(), "main").tripped, "premature trip");
		require(run.advanceTo(3) && bus(run.snapshot(), "main").tripped, "persistence did not trip");
		auto snap = run.snapshot();
		require(snap.activeAlerts.size() == 1 && snap.activeAlerts[0].metric == "busBreakerTrip"
			&& snap.activeAlerts[0].severity == core::AlertSeverity::Critical
			&& snap.activeAlerts[0].threshold == 5 && snap.activeAlerts[0].currentValue == 8
			&& snap.activeAlerts[0].firedAtSeconds == 3, "breaker Alert payload missing without Monitor");
		require(run.advanceTo(4), "dead bus failed audit");
		snap = run.snapshot();
		near(bus(snap, "main").generationKW, 0, "tripped solar connected");
		near(bus(snap, "main").suppliedKW, 0, "tripped load supplied");
		near(bus(snap, "main").unmetKW, 8, "dead demand vanished");
		near(bus(snap, "main").unmetCriticalKW, 4, "dead critical floor vanished");
		require(!bus(snap, "backup").tripped, "unrated bus tripped");
		auto disabledMonitors = run.effectiveSettings();
		require(core::World::setStationMonitorsEnabled(disabledMonitors, false, diagnostic)
			&& run.updateSettings(disabledMonitors) && run.snapshot().activeAlerts.size() == 1,
			"bulk Monitor disable cleared mandatory breaker Alert");
		require(run.updateNode<core::EquipmentLoadSettings>("main", [](auto& load) { load.electricalDemandKW = 4; }), "load recovery refused");
		require(run.advanceTo(8) && bus(run.snapshot(), "main").tripped
			&& run.snapshot().activeAlerts.size() == 1, "latch auto-cleared on recovery");
		require(run.breakerReset("main") && !bus(run.snapshot(), "main").tripped, "reset failed");
		snap = run.snapshot();
		require(snap.activeAlerts.empty() && snap.closedAlerts.size() == 1
			&& snap.closedAlerts[0].clearReason == core::AlertClearReason::BreakerReset, "reset Alert lifecycle failed");
		require(run.advanceTo(9), "reset supply drive failed");
		near(bus(run.snapshot(), "main").suppliedKW, 4, "reset did not restore");
		require(run.updateNode<core::EquipmentLoadSettings>("main", [](auto& load) { load.electricalDemandKW = 8; }), "overload restore refused");
		require(run.advanceTo(11) && !bus(run.snapshot(), "main").tripped, "re-trip reused old persistence");
		require(run.advanceTo(12) && bus(run.snapshot(), "main").tripped
			&& run.snapshot().activeAlerts.size() == 1, "persistent overload did not re-trip");
		// Authored Monitors cannot impersonate or disable this producer.
		auto collision = s;
		core::StationMonitorSettings monitor;
		monitor.id = "breaker:main";
		collision.monitors.push_back(monitor);
		require(!core::validateStationMonitors(collision, diagnostic) && !diagnostic.empty(), "Monitor stole breaker Alert identity");
		// A coarse Station interval splits exactly at a fractional persistence
		// boundary. Its power accounts describe the whole elapsed interval.
		{
			core::ScopedStationStepRatioForTesting explicitClock(std::nullopt);
			auto coarseSettings = s;
			coarseSettings.systemsStepWorldTicks = 6;
			coarseSettings.powerBuses[0].tripPersistenceSeconds = 2.5;
			core::StationSystemsRuntime coarse(coarseSettings, 1.0f);
			require(coarse.advanceTo(6), "coarse breaker split failed");
			auto coarseSnap = coarse.snapshot();
			near(bus(coarseSnap, "main").trippedAtSimSeconds, 2.5, "coarse trip missed persistence boundary");
			near(bus(coarseSnap, "main").suppliedKW, 8 * 2.5 / 6, "coarse trip leaked supply after boundary");
		}
		// Below-rating time resets the timer; equality is not overload.
		auto pulseSettings = s;
		pulseSettings.powerBuses[0].ratedCapacityKW = 8;
		core::StationSystemsRuntime equal(pulseSettings, 1.0f);
		require(equal.advanceTo(10) && !bus(equal.snapshot(), "main").tripped, "equality tripped");
		pulseSettings.powerBuses[0].ratedCapacityKW = 5;
		pulseSettings.equipmentLoads[0].operatingProfile.periodSeconds = 4;
		pulseSettings.equipmentLoads[0].operatingProfile.segments = {{0, 1}, {2, 0}};
		core::StationSystemsRuntime pulses(pulseSettings, 1.0f);
		require(pulses.advanceTo(12) && !bus(pulses.snapshot(), "main").tripped, "separate overload pulses accumulated");
		// Instant protection must roll back the trial draw, including batteries.
		s.powerBuses[0].tripPersistenceSeconds = 0;
		core::BatterySettings battery;
		battery.id = "bank"; battery.busIds = {"main"}; battery.nameplateKWh = 10; battery.usableKWh = 10;
		battery.startingStoredKWh = 10; battery.maxDischargeKW = 20; battery.maxChargeKW = 20;
		s.batteries = {battery};
		s.solarArrays[0].capacityKW = 0; // Only the battery can cause this overload.
		core::StationSystemsRuntime instant(s, 1.0f);
		require(instant.advanceTo(1), "instant trip audit failed");
		snap = instant.snapshot();
		require(bus(snap, "main").tripped, "zero persistence failed");
		near(bus(snap, "main").throughPowerKW, 0, "instant trial draw leaked");
		near(bus(snap, "main").batteryDischargeKW, 0, "tripped battery discharged");
		near(bus(snap, "main").batteryChargeKW, 0, "tripped battery charged");
		require(instant.breakerReset("main") && instant.advanceTo(2)
			&& bus(instant.snapshot(), "main").tripped, "instant persistent re-trip failed");
		// Charging alone is also through-power; its trial energy must not
		// survive an instantaneous trip, even with no equipment demand.
		auto chargingSettings = s;
		chargingSettings.solarArrays[0].capacityKW = 20;
		chargingSettings.batteries[0].startingStoredKWh = 0;
		chargingSettings.equipmentLoads[0].electricalDemandKW = 0;
		chargingSettings.equipmentLoads[0].criticalFloorKW = 0;
		core::StationSystemsRuntime charging(chargingSettings, 1.0f);
		require(charging.advanceTo(1) && bus(charging.snapshot(), "main").tripped, "charging overload did not trip");
		near(charging.snapshot().power.batteries[0].storedKWh, 0, "instant charge probe leaked stored energy");
		// Opening a first landing must not strand a multi-landing source or
		// battery, and their remaining flow must never book on the opened wire.
		auto landed = twoBuses();
		landed.powerBuses[0].ratedCapacityKW = 5;
		landed.solarArrays.resize(1); landed.solarArrays[0].busIds = {"main", "backup"};
		core::StationSystemsRuntime sharedSource(landed, 1.0f);
		require(sharedSource.advanceTo(2), "shared source isolation failed");
		auto ls = sharedSource.snapshot();
		near(bus(ls, "backup").suppliedKW, 8, "tripped first landing stranded live solar landing");
		for (auto const& flow : ls.connections)
			if (flow.key.peerType == "Power bus" && flow.key.peerId == "main")
				near(flow.lastInputRate, 0, "opened source cable still carried power");
		landed.solarArrays.clear(); landed.batteries = {battery};
		landed.batteries[0].busIds = {"main", "backup"};
		core::StationSystemsRuntime sharedBattery(landed, 1.0f);
		require(sharedBattery.advanceTo(2), "shared battery isolation failed");
		ls = sharedBattery.snapshot();
		near(bus(ls, "backup").suppliedKW, 8, "tripped first landing stranded live battery landing");
		for (auto const& flow : ls.connections)
			if (flow.key.peerType == "Power bus" && flow.key.peerId == "main")
				near(flow.lastInputRate, 0, "opened battery cable still carried power");
		// A running generator isolated by a trip follows its ordinary shutdown
		// and decay heat path, even if it also has an untripped landing.
		s = twoBuses(); s.solarArrays.clear();
		s.powerBuses[0].ratedCapacityKW = 5; s.powerBuses[0].tripPersistenceSeconds = 2;
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "generation"; sink.heatCapacityKJPerK = 1000000; sink.startingTemperatureC = 20;
		s.thermalReservoirs = {sink};
		core::ResourceStoreSettings fuel;
		fuel.id = "fuel"; fuel.unit = "kWh"; fuel.capacity = 100; fuel.startingQualified = 100; s.stores = {fuel};
		core::GeneratorSettings gen;
		gen.id = "gen"; gen.fuelStoreId = "fuel"; gen.heatSinkId = "sink"; gen.thermalClass = "generation";
		gen.ratedOutputKW = 20; gen.commandedOutputKW = 20; gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 1000; gen.maxSinkTemperatureC = 90;
		gen.shutdownSeconds = 10; gen.shutdownHeatKW = 2;
		gen.busIds = {"main", "backup"}; s.generators = {gen};
		core::StationSystemsRuntime generated(s, 1.0f);
		require(generated.advanceTo(4), "isolated generator drive failed");
		auto gs = generated.snapshot();
		require(bus(gs, "main").tripped && gs.power.generators[0].phase == core::StationGeneratorPhase::Shutdown
			&& gs.power.generators[0].decayHeatKW > 0, "isolated generator skipped shutdown/decay heat");
		near(gs.power.generators[0].averageOutputKW, 0, "isolated generator kept producing");
		near(bus(gs, "backup").generationKW, 0, "isolated generator energized another landing");
	}
}
void registerStationBusDispatchChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"busBreakers", breakers});
	checks.push_back({"busDispatchBalance", balance});
	checks.push_back({"busDispatchProtected", protectedFirst});
	checks.push_back({"busDispatchBatteries", batteries});
	checks.push_back({"busDispatchInternal", internalConsumers});
	checks.push_back({"busDispatchSources", sourceDispatch});
	checks.push_back({"busDispatchRecovery", recoveryReserve});
}
