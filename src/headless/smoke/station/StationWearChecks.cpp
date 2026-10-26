#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <limits>

namespace
{
using namespace smoke::station;

// #843: the capacity-fade pilot bank. A 10 kWh battery with the wear
// model wired on cycles and calendar time, full sun for charging, and a
// switchable load for driving discharge.
core::StationSystemsSettings fadeBatteryStation(
	double fadePerCycle, double fadePerCalendarHour, double maxWear = 1.0)
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::BatterySettings bank;
	bank.id = "bank"; bank.nameplateKWh = 10; bank.usableKWh = 10;
	bank.startingStoredKWh = 0; bank.maxChargeKW = 10; bank.maxDischargeKW = 10;
	bank.capacityFade = core::InstallationWearSettings{fadePerCycle, 0.0, 0.0, fadePerCalendarHour, maxWear};
	s.batteries = {bank};
	core::SolarArraySettings sun;
	sun.id = "sun"; sun.capacityKW = 10; sun.illumination = constantProfile(1.0);
	s.solarArrays = {sun};
	core::EquipmentLoadSettings load;
	load.id = "load"; load.electricalDemandKW = 0; load.thermalClass = "moderate";
	load.coolingDisposition = core::StationCoolingDisposition::Exported;
	s.equipmentLoads = {load};
	return s;
}

// A bank that only ever discharges: night, a standing load, and a
// starting charge to spend.
core::StationSystemsSettings drainBatteryStation(double fadePerCycle, double startingStored)
{
	auto s = fadeBatteryStation(fadePerCycle, 0.0);
	s.solarArrays[0].illumination = constantProfile(0.0);
	s.equipmentLoads[0].electricalDemandKW = 10;
	s.batteries[0].startingStoredKWh = startingStored;
	return s;
}

// The wear loop: a circulating coolant loop opted into Wear on operating
// hours (no calendar fade, so the fade only moves while the pump really
// works), with a leak and repair-parts wiring so the repair path can be
// shown to leave the fade exactly where it found it.
core::StationSystemsSettings wearLoopStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::ThermalReservoirSettings reservoir;
	reservoir.id = "sink"; reservoir.thermalClass = "moderate";
	reservoir.heatCapacityKJPerK = 10000; reservoir.startingTemperatureC = 20;
	s.thermalReservoirs = {reservoir};
	core::CoolantLoopSettings loop;
	loop.id = "loop"; loop.reservoirId = "sink";
	loop.coolantCapacityKg = loop.startingCoolantKg = 100;
	loop.specificHeatKJPerKgK = 4; loop.startingTemperatureC = 20;
	loop.ratedFlowKgPerS = 10; loop.pumpPowerKW = 1;
	loop.wear = core::InstallationWearSettings{0.0, 0.1, 0.0, 0.0, 0.5};
	loop.coolantLeak = core::CoolantLeakSettings{"room", 40};
	core::RepairPartsSettings wiring;
	wiring.partsStoreId = "parts";
	wiring.repairs = {core::FaultRepairSettings{"coolant-leak", 10.0, 2.0, "waste"}};
	loop.repairParts = wiring;
	s.coolantLoops = {loop};
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	core::ResourceStoreSettings parts;
	parts.id = "parts"; parts.unit = "kg"; parts.capacity = 100; parts.startingQualified = 10;
	core::ResourceStoreSettings waste;
	waste.id = "waste"; waste.unit = "kg"; waste.capacity = 100; waste.startingQualified = 0;
	s.stores = {parts, waste};
	core::SolarArraySettings sun;
	sun.id = "sun"; sun.capacityKW = 10; sun.illumination = constantProfile(1.0);
	s.solarArrays = {sun};
	return s;
}

int countCategory(core::StationSystemsRuntime const& run, std::string const& category)
{
	auto const events = run.events();
	return static_cast<int>(std::count_if(events.begin(), events.end(),
		[&](auto const& e) { return e.category == category; }));
}
bool sawMessage(core::StationSystemsRuntime const& run, std::string const& needle)
{
	auto const events = run.events();
	return std::any_of(events.begin(), events.end(), [&](auto const& e)
	{
		return e.message.find(needle) != std::string::npos;
	});
}
const core::StationMaintenanceSnapshot* findJob(core::StationSystemsSnapshot const& snap, std::string const& id)
{
	auto const it = std::find_if(snap.maintenanceJobs.begin(), snap.maintenanceJobs.end(),
		[&](auto const& job) { return job.id == id; });
	return it == snap.maintenanceJobs.end() ? nullptr : &*it;
}
template<class Serializer>
std::string replayBytes(core::StationSystemsRuntime const& run)
{
	auto out = Serializer::toString();
	out->beginMap("root"); run.writeFaultReplay(*out); out->endMap(); out->serialize();
	return out->getSerializedString();
}
template<class Serializer>
void roundTrip(core::StationSystemsRuntime const& run)
{
	auto const bytes = replayBytes<Serializer>(run);
	auto in = Serializer::fromString(bytes); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"wear replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes, "wear command log did not round-trip byte-exactly");
	require(restored->advanceTo(run.snapshot().worldTick * 0.25 + 60),
		"restored wear state failed subsequent audited drive");
}

void wearLifecycle(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// Charge: the surplus sun fills the bank, and every stored kWh banks
	// half of an equivalent cycle - a full charge plus a full discharge
	// of the authored cyclable energy is one cycle.
	auto s = fadeBatteryStation(0.2, 0.0);
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(900), "charge drive failed");
	auto snap = run.snapshot();
	auto const& bank = snap.power.batteries[0];
	require(bank.storedKWh > 1.0 && bank.wearCycles > 0.0,
		"charging banked no cycles");
	require(std::abs(bank.wearCycles - bank.chargedKWh / 20.0) < 1e-9,
		"cycles did not track real charge at half a round trip per side");
	require(std::abs(bank.wearFraction - bank.wearCycles * 0.2) < 1e-9,
		"wearFraction did not follow the authored fade per cycle");
	require(std::abs(bank.effectiveNameplateKWh - 10.0 * (1.0 - bank.wearFraction)) < 1e-9,
		"effective nameplate did not scale with the fade");
	require(std::abs(bank.effectiveUsableKWh - 10.0 * (1.0 - bank.wearFraction)) < 1e-9
		&& bank.effectiveReserveKWh < 1e-12,
		"effective usable/reserve did not scale with the fade");
	require(bank.wearOperatingHours > 0.0 && bank.wearCalendarHours > 0.0
		&& bank.wearOperatingHours <= bank.wearCalendarHours + 1e-12,
		"operating hours exceeded calendar hours while charging");
	// Keep charging: the faded ceiling is permanent - the bank saturates
	// below its authored nameplate and can never be refilled past it.
	require(run.advanceTo(3600), "long charge drive failed");
	snap = run.snapshot();
	auto const& faded = snap.power.batteries[0];
	require(faded.saturated && faded.storedKWh < 10.0 - 1e-3,
		"a faded bank still filled to the authored nameplate");
	require(faded.storedKWh <= faded.effectiveNameplateKWh + 1e-2,
		"stored energy passed the faded ceiling beyond one interval's fade step");
	require(faded.wearFraction > 0.0 && faded.effectiveNameplateKWh < 10.0,
		"capacity fade did not reduce the rated capacity");
	// Discharge: real discharge banks the other half of each cycle.
	auto drained = drainBatteryStation(0.2, 5.0);
	core::StationSystemsRuntime down(drained, 0.25f);
	require(down.advanceTo(1800), "drain drive failed");
	auto const& spent = down.snapshot().power.batteries[0];
	require(std::abs(spent.wearCycles - 0.25) < 1e-6 && std::abs(spent.wearFraction - 0.05) < 1e-6,
		"a full drain of 5 kWh did not bank a quarter cycle at the authored rate");
	require(spent.storedKWh < 1e-9 && spent.atReserve,
		"the drain did not stop at the reserve floor");
	// Idle: calendar ageing acts with zero cycles and zero operating hours.
	auto idle = fadeBatteryStation(0.0, 0.001);
	idle.batteries[0].startingStoredKWh = 5;
	idle.solarArrays[0].illumination = constantProfile(0.0);
	core::StationSystemsRuntime resting(idle, 0.25f);
	require(resting.advanceTo(7200), "idle drive failed");
	auto const& aged = resting.snapshot().power.batteries[0];
	require(aged.wearCycles == 0.0 && aged.wearOperatingHours == 0.0,
		"an idle battery banked operation it never performed");
	require(std::abs(aged.wearCalendarHours - 2.0) < 1e-6
		&& std::abs(aged.wearFraction - 0.002) < 1e-9,
		"calendar ageing did not advance on an idle bank");
	// The ceiling clamps the fade; the rating never goes negative. A
	// cycle-coupled fade self-limits as the shrinking ceiling throttles
	// the charging that feeds it, so the clamp is shown on calendar fade,
	// which nothing throttles.
	auto clamped = fadeBatteryStation(0.0, 0.4, 0.6);
	clamped.batteries[0].startingStoredKWh = 5;
	clamped.solarArrays[0].illumination = constantProfile(0.0);
	core::StationSystemsRuntime hard(clamped, 0.25f);
	require(hard.advanceTo(10800), "clamped drive failed");
	auto const& capped = hard.snapshot().power.batteries[0];
	require(capped.wearFraction == 0.6
		&& std::abs(capped.effectiveNameplateKWh - 4.0) < 1e-9,
		"the fade ceiling did not clamp at the authored maxWearFraction");
	// Monotonic: the fade never recedes across the run.
	auto watch = fadeBatteryStation(0.2, 0.0005);
	core::StationSystemsRuntime rising(watch, 0.25f);
	double previous = 0.0;
	for (double seconds : {300., 900., 1800., 3600., 5400.})
	{
		require(rising.advanceTo(seconds), "monotonic drive failed");
		auto const fraction = rising.snapshot().power.batteries[0].wearFraction;
		require(fraction >= previous - 1e-12, "wearFraction decreased mid-run");
		previous = fraction;
	}
	// Wear is not a Fault: it cannot be injected, repaired or reset.
	std::string diagnostic;
	auto const before = rising.snapshot();
	auto const events = rising.events();
	require(!rising.injectFault("bank", "capacity-fade", {}, &diagnostic) && !diagnostic.empty()
		&& !rising.repairFault("bank", "capacity-fade", nullptr, &diagnostic) && !diagnostic.empty()
		&& !rising.resetTrip("bank", &diagnostic) && !diagnostic.empty(),
		"a wear fade was treated as an injectable or repairable Fault");
	require(rising.snapshot() == before && rising.events() == events && !rising.hasFaultCommands(),
		"a refused wear command mutated state, events or the command journal");
}

void wearDecayRepair(smoke::Context const&)
{
	auto s = wearLoopStation(); s.systemsStepWorldTicks = 60;
	s.coolantLoops[0].wear = core::InstallationWearSettings{};
	s.coolantLoops[0].wear->fadeHalfLifeHours = 0.01;
	core::World world("decay repair", 2, 2); world.pauseSimulation(); world.finishBuild();
	std::string why; auto configured = world.configureStationSystems(s, false, &why); require(configured, why.c_str());
	world.resumeSimulation(); require(advanceSeconds(world, 10), "repair decay initial drive failed");
	require(world.injectStationFault("loop", "coolant-leak", {core::FaultCause::Kind::AgentAction, "agent:1"}), "decay leak refused");
	std::string job;
	require(world.repairStationFault("loop", "coolant-leak", &job), "decay repair refused");
	require(advanceSeconds(world, 20), "decay repair drive failed");
	auto snap = world.getStationSystemsSnapshot();
	require(snap.coolantLoops[0].activeFaults.empty(), "decay repair did not clear fault");
	require(std::abs(snap.coolantLoops[0].wearFraction - (1 - std::exp2(-snap.elapsedSeconds / 36))) < 1e-12,
		"repair healed decay or stopped its clock during isolation");
}

void wearRepair(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// The loop wears while it actually circulates. With no calendar fade,
	// the fade only moves while the pump really works - so across the
	// repair, where the Installation is isolated at zero flow, the
	// wearFraction must not change by even one float step.
	auto s = wearLoopStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(3600), "wearing drive failed");
	auto worn = run.snapshot().coolantLoops[0];
	require(std::abs(worn.wearOperatingHours - 1.0) < 1e-6
		&& std::abs(worn.wearThroughputKg - 34200.0) < 50.0,
		"operating hours and throughput did not track real circulation");
	require(std::abs(worn.wearFraction - 0.1) < 1e-6
		&& std::abs(worn.effectiveRatedFlowKgPerS - 9.0) < 1e-3
		&& std::abs(worn.flowKgPerS - 9.0) < 1e-3,
		"the worn pump did not flow at its faded rating");
	// #859: even a ceiling lowered below accrued Wear cannot turn repair into replacement.
	require(run.updateNode<core::CoolantLoopSettings>("loop",
		[](auto& l) { l.wear->maxWearFraction = 0.01; }), "lower Wear ceiling refused");
	require(run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::AgentAction, "agent:1"})
		&& run.advanceTo(3610), "leak setup failed");
	auto const leaked = run.snapshot().coolantLoops[0];
	require(leaked.wearFraction >= worn.wearFraction,
		"lowering the loop Wear ceiling before repair healed past Wear");
	require(leaked.availabilityFactor == 0.6
		&& leaked.flowKgPerS < 6.0 && leaked.flowKgPerS > 5.0,
		"the leak cap did not compose against the faded rating");
	std::string jobId;
	require(run.repairFault("loop", "coolant-leak", &jobId), "repair of the worn loop refused");
	auto const commanded = run.snapshot().coolantLoops[0].wearFraction;
	require(run.advanceTo(3615), "repair start failed");
	require(findJob(run.snapshot(), jobId) && findJob(run.snapshot(), jobId)->phase
		== core::StationMaintenancePhase::Active, "repair did not start");
	// Isolated for the full duration: no flow, no operating hours, no
	// throughput, and - with no calendar fade - no fade movement at all.
	auto const mid = run.snapshot().coolantLoops[0];
	require(mid.flowKgPerS == 0.0 && mid.availabilityFactor == 0.0,
		"the repair did not isolate the worn loop at zero flow");
	require(mid.wearFraction == commanded,
		"isolation moved the wearFraction of a no-calendar-fade model");
	require(run.advanceTo(3621), "repair completion failed");
	auto const fixed = run.snapshot().coolantLoops[0];
	require(fixed.activeFaults.empty() && fixed.availabilityFactor == 1.0,
		"the repair did not clear the Fault and lift its cap");
	// The repair itself added no fade: everything above the commanded
	// value is at most the sub-second of post-clear operation between
	// the completion boundary and this snapshot.
	require(fixed.wearFraction >= commanded
		&& fixed.wearFraction <= commanded + 0.1 / 3600.0 + 1e-9,
		"repair reversed or altered the wearFraction");
	require(fixed.coolantKg == 60.0, "the repair replenished coolant");
	require(sawMessage(run, "no Wear reversed"),
		"the repair completion did not announce that Wear stands");
	// And the fade keeps accruing after the repair: fixed is not new, and
	// the loop flows at its faded rating, never the authored one.
	require(run.updateNode<core::CoolantLoopSettings>("loop",
		[](auto& l) { l.wear->maxWearFraction = 0.5; }), "higher Wear ceiling refused");
	require(run.advanceTo(7200), "post-repair drive failed");
	auto const after = run.snapshot().coolantLoops[0];
	require(after.wearFraction > commanded,
		"the repaired loop stopped wearing while it operated");
	require(std::abs(after.effectiveRatedFlowKgPerS - 10.0 * (1.0 - after.wearFraction)) < 1e-9
		&& std::abs(after.flowKgPerS - after.effectiveRatedFlowKgPerS * 0.6) < 0.05
		&& after.flowKgPerS < 10.0 - 0.5,
		"the repaired loop flowed at the authored rating instead of the faded one");
	require(after.activeFaults.empty(), "the cleared fault came back without a fresh injection");
}

template<class Serializer>
void retunedReplay(core::StationSystemsRuntime& run)
{
	auto const bytes = replayBytes<Serializer>(run);
	auto in = Serializer::fromString(bytes); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"retuned Wear-only replay lost historical rates");
	require(replayBytes<Serializer>(*restored) == bytes, "retuned Wear-only journal changed on reload");
	auto const next = run.snapshot().worldTick * 0.25 + 60;
	require(run.advanceTo(next) && restored->advanceTo(next)
		&& restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"retuned Wear-only continuation diverged");
}

void wearReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// #857: no injectable Fault, Condition or command may mask the missing
	// initial-settings pin. Historical rates must survive a live retune.
	auto calendar = fadeBatteryStation(0.0, 0.2);
	core::StationSystemsRuntime retunedBank(calendar, 0.25f);
	require(retunedBank.advanceTo(900)
		&& retunedBank.updateNode<core::BatterySettings>("bank",
			[](auto& b) { b.capacityFade->fadePerCalendarHour = 0.4; })
		&& retunedBank.advanceTo(1800), "Wear-only battery retune failed");
	require(std::abs(retunedBank.snapshot().power.batteries[0].wearFraction - 0.15) < 1e-9,
		"battery retune repriced historical Wear");
	// #859: lowering a live ceiling may arrest fade, never heal its history.
	auto const bankWear = retunedBank.snapshot().power.batteries[0].wearFraction;
	require(retunedBank.updateNode<core::BatterySettings>("bank", [](auto& b)
		{ b.capacityFade->maxWearFraction = 0.01; b.capacityFade->fadePerCalendarHour = 0.001; })
		&& retunedBank.advanceTo(1801), "battery ceiling reduction failed");
	require(retunedBank.snapshot().power.batteries[0].wearFraction == bankWear
		&& std::abs(retunedBank.snapshot().power.batteries[0].effectiveNameplateKWh
			- 10.0 * (1.0 - bankWear)) < 1e-9,
		"lowering the battery Wear ceiling healed accumulated fade");
	retunedReplay<core::YamlSerializer>(retunedBank);
	retunedReplay<core::BinarySerializer>(retunedBank);
	require(retunedBank.updateNode<core::BatterySettings>("bank", [](auto& b)
		{ b.capacityFade->maxWearFraction = 0.8; }) && retunedBank.advanceTo(1922)
		&& retunedBank.snapshot().power.batteries[0].wearFraction >= bankWear,
		"later battery ceiling/rate edits healed Wear");
	auto loopOnly = wearLoopStation();
	loopOnly.coolantLoops[0].coolantLeak.reset();
	loopOnly.coolantLoops[0].repairParts.reset();
	core::StationSystemsRuntime retunedLoop(loopOnly, 0.25f);
	require(retunedLoop.advanceTo(30)
		&& retunedLoop.updateNode<core::CoolantLoopSettings>("loop",
			[](auto& l) { l.wear->fadePerOperatingHour = 0.2; })
		&& retunedLoop.advanceTo(60), "Wear-only loop retune failed");
	auto const loopWear = retunedLoop.snapshot().coolantLoops[0].wearFraction;
	require(retunedLoop.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.wear->maxWearFraction = 0.0001; l.wear->fadePerOperatingHour = 0.001; })
		&& retunedLoop.advanceTo(61), "loop ceiling reduction failed");
	require(retunedLoop.snapshot().coolantLoops[0].wearFraction == loopWear
		&& std::abs(retunedLoop.snapshot().coolantLoops[0].effectiveRatedFlowKgPerS
			- 10.0 * (1.0 - loopWear)) < 1e-9,
		"lowering the coolant-loop Wear ceiling healed accumulated fade");
	retunedReplay<core::YamlSerializer>(retunedLoop);
	retunedReplay<core::BinarySerializer>(retunedLoop);
	require(retunedLoop.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.wear->maxWearFraction = 0.5; }) && retunedLoop.advanceTo(182)
		&& retunedLoop.snapshot().coolantLoops[0].wearFraction >= loopWear,
		"later coolant-loop ceiling/rate edits healed Wear");
	// Funded Condition restoration is not replacement either.
	auto restorable = wearLoopStation();
	auto& condition = restorable.coolantLoops[0].condition.emplace();
	condition.initialCondition = 0.8; condition.restoreStoreId = "parts";
	condition.restorePerHour = 360; condition.stockUnitsPerConditionPoint = 1;
	core::StationSystemsRuntime restoredCondition(restorable, 0.25f);
	require(restoredCondition.advanceTo(60), "Condition/Wear setup failed");
	auto const permanent = restoredCondition.snapshot().coolantLoops[0].wearFraction;
	require(restoredCondition.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.wear->maxWearFraction = 0.0001; })
		&& restoredCondition.restoreCondition("loop", 1.0)
		&& restoredCondition.advanceTo(65), "funded restoration after ceiling change failed");
	require(restoredCondition.snapshot().coolantLoops[0].condition == 1.0
		&& restoredCondition.snapshot().coolantLoops[0].wearFraction == permanent,
		"restoreCondition healed permanent Wear after a ceiling change");
	roundTrip<core::YamlSerializer>(restoredCondition);
	roundTrip<core::BinarySerializer>(restoredCondition);
	// A wear-only run needs the replay block with no client command at
	// all: the fade integrated over the run must survive the save.
	auto batteryOnly = fadeBatteryStation(0.2, 0.0005);
	core::StationSystemsRuntime fading(batteryOnly, 0.25f);
	require(fading.advanceTo(900), "wear-only drive failed");
	require(fading.hasFaultReplayState(),
		"a capacity-fade run without commands does not need the replay block");
	require(fading.snapshot().power.batteries[0].wearFraction > 0.0,
		"no fade accumulated before the replay round trip");
	roundTrip<core::BinarySerializer>(fading);
	roundTrip<core::YamlSerializer>(fading);
	// A scripted wear-and-repair sequence crosses both encodings exactly.
	auto s = wearLoopStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(600) && run.injectFault("loop", "coolant-leak",
		{core::FaultCause::Kind::ScenarioEvent, "strike"}), "wear+repair setup failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	std::string jobId;
	require(run.repairFault("loop", "coolant-leak", &jobId) && run.advanceTo(615),
		"repair drive failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(625) && run.snapshot().coolantLoops[0].activeFaults.empty(),
		"the replayed run lost the repair completion");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	// World save/load carries the wear state through both serializers.
	core::World world("Wear", 2, 2); world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), "World wear setup failed");
	world.resumeSimulation();
	require(advanceSeconds(world, 600) && world.injectStationFault("loop", "coolant-leak", {})
		&& advanceSeconds(world, 10), "World wear drive failed");
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work; world.serialize(*out, work); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); core::World loaded("Loaded", 2, 2);
		require(loaded.deserialize(*in, work), "World wear reload failed");
		require(loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot()
			&& loaded.getStationSystemsEvents() == world.getStationSystemsEvents(),
			"World save/load lost the wear state");
		require(advanceSeconds(loaded, 60) && advanceSeconds(world, 60)
			&& loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot(),
			"the reloaded wear state diverged on subsequent drive");
	}
}

void wearTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	std::string diagnostic;
	// Settings validation of the wear model itself.
	for (auto const& bad : {
		core::InstallationWearSettings{std::numeric_limits<double>::quiet_NaN(), 0, 0, 0, 1},
		core::InstallationWearSettings{0.1, -0.1, 0, 0, 1},
		core::InstallationWearSettings{0.1, 0, 0, 0, 0},
		core::InstallationWearSettings{0.1, 0, 0, 0, 1.5},
		core::InstallationWearSettings{0.0, 0.0, 0.0, 0.0, 1.0}})
	{
		auto settings = fadeBatteryStation(0.1, 0.0);
		settings.batteries[0].capacityFade = bad;
		require(!core::validateStationSystemsSettings(settings, diagnostic),
			"an invalid battery wear model was accepted");
	}
	{
		auto unwired = fadeBatteryStation(0.1, 0.0);
		unwired.batteries[0].capacityFade->fadePerThroughputKg = 0.1;
		require(!core::validateStationSystemsSettings(unwired, diagnostic)
			&& diagnostic.find("fadePerThroughputKg") != std::string::npos,
			"a battery accepted a throughput rate it cannot accumulate");
		auto looped = wearLoopStation();
		looped.coolantLoops[0].wear->fadePerCycle = 0.1;
		require(!core::validateStationSystemsSettings(looped, diagnostic)
			&& diagnostic.find("fadePerCycle") != std::string::npos,
			"a coolant loop accepted a cycle rate it cannot count");
	}
	// Live topology: the opt-in presence is frozen, the rates are live.
	auto s = wearLoopStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(600), "setup drive failed");
	require(!run.updateNode<core::CoolantLoopSettings>("loop",
		[](auto& l) { l.wear.reset(); }, &diagnostic), "wear opt-in removed live");
	require(!run.updateSettings(fadeBatteryStation(0.1, 0.0), &diagnostic),
		"a whole-settings swap crossed unrelated topology");
	// A retune never reprices the fade already accumulated: the first
	// hour ran at 0.1/h and lands at 0.1; one more hour at 0.2/h lands
	// at 0.3, not 0.4.
	require(run.advanceTo(3600), "first wearing hour failed");
	auto const firstHour = run.snapshot().coolantLoops[0];
	require(std::abs(firstHour.wearFraction - 0.1) < 1e-3,
		"the first hour did not fade at its authored rate");
	require(run.updateNode<core::CoolantLoopSettings>("loop",
		[](auto& l) { l.wear->fadePerOperatingHour = 0.2; }), "wear rate retune refused");
	require(run.advanceTo(7200), "second wearing hour failed");
	auto const retuned = run.snapshot().coolantLoops[0];
	require(std::abs(retuned.wearFraction - 0.3) < 1e-3,
		"a live rate retune repriced the fade that had already accumulated");
	// A poisoned rate fails the tick whole and rolls the fade back.
	auto poison = wearLoopStation();
	core::StationSystemsRuntime tainted(poison, 0.25f);
	require(tainted.advanceTo(600), "poison setup failed");
	auto const clean = tainted.snapshot().coolantLoops[0].wearFraction;
	require(tainted.updateNode<core::CoolantLoopSettings>("loop",
		[](auto& l) { l.wear->fadePerCalendarHour = std::numeric_limits<double>::infinity(); }),
		"poison retune refused before the audit could catch it");
	require(!tainted.advanceTo(610) && !tainted.snapshot().healthy
		&& tainted.snapshot().coolantLoops[0].wearFraction == clean,
		"a poisoned wear rate published a partial fade instead of rolling back");
	// Step-ratio independence of the accumulated fade.
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = fadeBatteryStation(0.2, 0.0005);
		offered.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.advanceTo(1800), "wear failed under a supported Station ratio");
		auto const& faded = stepped.snapshot().power.batteries[0];
		// Half an hour at 10 kW into the 10 kWh bank: a quarter equivalent
		// cycle at 0.2 per cycle plus half an hour of calendar at 0.0005.
		require(std::abs(faded.wearFraction - 0.05025) < 1e-4
			&& std::abs(faded.wearCalendarHours - 0.5) < 1e-6,
			"the accumulated fade depended on the Station step ratio");
	}
	// Schema: wear blocks need 116; a pre-116 document carrying them is
	// refused, and 116 round-trips both settings through both encoders.
	for (uint32_t version : {115u, 116u})
	{
		auto out = core::YamlSerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto in = core::YamlSerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored;
		bool refused = false;
		try { core::readStationSystemsSettingsMap(*in, version, restored); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused == (version < 116), "wear settings schema guard failed");
		if (version >= 116) require(restored == s, "wear settings did not round-trip");
	}
	{
		auto out = core::BinarySerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto in = core::BinarySerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored;
		core::readStationSystemsSettingsMap(*in, 116, restored);
		require(restored == s, "wear settings did not round-trip through binary");
	}
	core::StationConfigurationDocument document; document.settings = s;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 116") != std::string::npos, "wear opt-in did not bump the wire schema");
	yaml.replace(yaml.find("schemaVersion: 116"), std::string("schemaVersion: 116").size(), "schemaVersion: 115");
	core::StationConfigurationDocument restoredDoc;
	require(!core::parseStationConfiguration(yaml, restoredDoc, &diagnostic),
		"pre-116 document silently accepted wear models");
}

void wearOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// Feature absent: no fade, effective ratings equal the authored ones,
	// and the document keeps the pre-feature wire shape.
	auto absent = fadeBatteryStation(0.2, 0.0);
	absent.batteries[0].capacityFade.reset();
	core::StationSystemsRuntime unworn(absent, 0.25f);
	require(unworn.advanceTo(3600), "absent-opt-in drive failed");
	auto const& bank = unworn.snapshot().power.batteries[0];
	require(bank.wearFraction == 0.0 && bank.wearCycles == 0.0
		&& bank.wearOperatingHours == 0.0 && bank.wearCalendarHours == 0.0,
		"a battery without the fade model accumulated wear");
	require(bank.effectiveNameplateKWh == bank.nameplateKWh
		&& bank.effectiveUsableKWh == bank.usableKWh
		&& bank.effectiveReserveKWh == bank.reserveKWh,
		"effective ratings diverged from the authored ratings without the opt-in");
	require(bank.saturated && std::abs(bank.storedKWh - 10.0) < 1e-9,
		"the unworn bank did not fill to its full authored nameplate");
	auto absentLoop = wearLoopStation();
	absentLoop.coolantLoops[0].wear.reset();
	core::StationSystemsRuntime plain(absentLoop, 0.25f);
	require(plain.advanceTo(3600), "absent loop drive failed");
	auto const& loop = plain.snapshot().coolantLoops[0];
	require(loop.wearFraction == 0.0 && loop.wearOperatingHours == 0.0
		&& loop.wearThroughputKg == 0.0 && loop.wearCalendarHours == 0.0
		&& loop.effectiveRatedFlowKgPerS == loop.ratedFlowKgPerS,
		"a loop without the Wear model accumulated wear");
	// A feature-absent document stamps the old schema and carries none of
	// the new fields, so existing fixtures stay byte-identical.
	core::StationConfigurationDocument document; document.settings = absent;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 111") != std::string::npos,
		"a wear-absent document did not keep the pre-feature schema");
	require(yaml.find("fadePerCycle") == std::string::npos
		&& yaml.find("capacityFade") == std::string::npos,
		"a wear-absent battery carried wear fields on the wire");
	require(!unworn.hasFaultReplayState(), "feature-absent run acquired replay state");
	require(unworn.updateNode<core::BatterySettings>("bank",
		[](auto& b) { b.maxChargeKW = 9; }) && !unworn.hasFaultCommands()
		&& !unworn.hasFaultReplayState(), "feature-absent retune acquired a replay journal");
	// Repair-only documents keep stamping 115 exactly as before #843.
	auto repairOnly = wearLoopStation();
	repairOnly.coolantLoops[0].wear.reset();
	core::StationConfigurationDocument repairDoc; repairDoc.settings = repairOnly;
	auto repairYaml = core::serializeStationConfiguration(repairDoc);
	require(repairYaml.find("schemaVersion: 115") != std::string::npos,
		"a repair-only document stopped stamping its pre-#843 schema");
	// An absent battery cannot be worn by any command surface.
	std::string diagnostic;
	require(!unworn.injectFault("bank", "capacity-fade", {}, &diagnostic) && !diagnostic.empty(),
		"a wear fade was injected into a battery without the opt-in");
}
}
void registerStationWearChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"wearLifecycle", wearLifecycle});
	checks.push_back({"wearDecayRepair", wearDecayRepair});
	checks.push_back({"wearRepair", wearRepair});
	checks.push_back({"wearReplay", wearReplay});
	checks.push_back({"wearTransactions", wearTransactions});
	checks.push_back({"wearOptIn", wearOptIn});
}
