#include "StationRig.h"
#include "CarbonObservationFixture.h"
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
core::StationSystemsSettings punctureStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::ThermalReservoirSettings reservoir;
	reservoir.id = "sink"; reservoir.thermalClass = "moderate";
	reservoir.heatCapacityKJPerK = 10000;
	reservoir.startingTemperatureC = 20;
	s.thermalReservoirs = {reservoir};
	core::CoolantLoopSettings loop;
	loop.id = "loop"; loop.reservoirId = "sink";
	loop.coolantCapacityKg = loop.startingCoolantKg = 100;
	loop.specificHeatKJPerKgK = 4; loop.startingTemperatureC = 20;
	loop.ratedFlowKgPerS = 10; loop.pumpPowerKW = 1;
	loop.coolantLeak = core::CoolantLeakSettings{"room", 40};
	s.coolantLoops = {loop};
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingTemperatureC = 20; room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	core::SolarArraySettings solar;
	solar.id = "solar"; solar.capacityKW = 10;
	s.solarArrays = {solar};
	return s;
}
void faultInjection(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (bool critical : {false, true})
	{
		auto settings = punctureStation();
		settings.coolantLoops[0].criticalService = critical;
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.advanceTo(1), "initial drive failed");
		auto const before = run.snapshot();
		auto const events = run.events();
		std::string diagnostic;
		for (auto const& request : std::vector<std::pair<std::string, std::string>>{
			{"missing", "coolant-leak"}, {"loop", "missing"}, {"solar", "coolant-leak"}})
		{
			require(!run.injectFault(request.first, request.second, {}, &diagnostic) && !diagnostic.empty(),
				"invalid fault injection accepted or unexplained");
			require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
				"refused command mutated state");
		}
		require(run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::AgentAction, "agent:7"}),
			"valid injection refused");
		require(run.snapshot() == before && run.events() == events, "command applied before tick boundary");
		require(!run.injectFault("loop", "coolant-leak", {}, &diagnostic), "pending duplicate accepted");
		require(run.advanceTo(1.25), "fault booking failed audit");
		auto const snap = run.snapshot();
		auto const& loop = snap.coolantLoops[0];
		require(snap.healthy && loop.coolantKg == 60 && snap.atmospheres[0].spilledCoolantKg == 40,
			"coolant mass was not conservatively transferred");
		require(loop.leakedCoolantEnergyKJ == snap.atmospheres[0].spilledCoolantEnergyKJ
			&& std::abs(loop.residualKJ) < 1e-7, "carried coolant heat did not close");
		require(std::abs(loop.flowKgPerS - 6) < 1e-10 && loop.availabilityFactor == 0.6,
			"remaining charge was not the circulation cap");
		require(loop.activeFaults.size() == 1 && loop.activeFaults[0].modeId == "coolant-leak"
			&& loop.activeFaults[0].onsetTick == before.worldTick + 1
			&& loop.activeFaults[0].causeKind == core::FaultCause::Kind::AgentAction
			&& loop.activeFaults[0].causeDetail == "agent:7"
			&& loop.activeFaults[0].derivedImpact == 0.4, "fault snapshot lost onset or impact");
		require(std::count_if(run.events().begin(), run.events().end(), [](auto const& e) { return e.category == "fault"; }) == 1,
			"onset missing from bounded fault event log");
		auto const faultEvents = run.events();
		require(!run.injectFault("loop", "coolant-leak", {}, &diagnostic)
			&& run.snapshot() == snap && run.events() == faultEvents, "active duplicate mutated state");
		require(run.advanceTo(10) && run.snapshot().coolantLoops[0].coolantKg == 60,
			"one-shot leak booked more than once");
		auto const histories = run.metrics();
		auto const spill = std::find_if(histories.begin(), histories.end(), [](auto const& history)
			{ return history.descriptor.id == "coolant-spill/room/coolant/inventory"; });
		require(spill != histories.end() && !spill->samples.empty() && spill->samples.back().actual == 40,
			"retained spill disappeared from accounted material inventory history");
	}
}
void faultOutages(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (double cap : {0.0, 0.3, 0.8})
	{
		auto settings = punctureStation();
		core::InstallationOutageSettings outage;
		outage.id = "outage"; outage.installationId = "loop"; outage.capacityFraction = cap;
		outage.firstOccurrenceSeconds = 0; outage.durationSeconds = 2;
		settings.outages = {outage};
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(1), "outaged leak failed");
		auto const loop = run.snapshot().coolantLoops[0];
		require(loop.availabilityFactor == std::min(cap, 0.6)
			&& std::abs(loop.flowKgPerS - 10 * std::min(cap, 0.6)) < 1e-10,
			"fault and Outage multiplied instead of deepest-wins");
		require(run.advanceTo(3) && run.snapshot().coolantLoops[0].availabilityFactor == 0.6,
			"Outage restoration cleared the fault cap");
	}
	for (double mass : {100.0, 200.0})
	{
		auto settings = punctureStation(); settings.coolantLoops[0].coolantLeak->releasedKg = mass;
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(3), "dry leak failed");
		require(run.snapshot().coolantLoops[0].coolantKg == 0
			&& run.snapshot().coolantLoops[0].flowKgPerS == 0
			&& run.snapshot().atmospheres[0].spilledCoolantKg == 100, "dry loop invented coolant or flow");
	}
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
		"fault replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes, "fault command log did not round-trip byte-exactly");
	require(restored->advanceTo(12), "restored fault failed subsequent audited drive");
}
void faultReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = punctureStation();
	auto second = settings.coolantLoops[0]; second.id = "second";
	settings.coolantLoops.push_back(second);
	core::StationSystemsRuntime run(settings, 0.25f);
	require(run.advanceTo(1) && run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::ScenarioEvent, "strike"}), "inject failed");
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.coolantLeak->releasedKg = 5; }),
		"live booking-size edit failed");
	require(run.updateNode<core::SolarArraySettings>("solar", [](auto& a) { a.capacityKW = 7; }),
		"live supply edit failed");
	// Pending commands survive saves without prematurely applying the booking.
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(2) && run.injectFault("second", "coolant-leak", {core::FaultCause::Kind::Editor, "debug"})
		&& run.advanceTo(10), "scripted injection sequence failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.snapshot().coolantLoops[0].leakedCoolantKg == 40, "retuning changed an accepted fault's effective parameters");
	// Settings opt-in also crosses both persistence encodings, with a schema guard.
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, settings); out->endMap(); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored; core::readStationSystemsSettingsMap(*in, 112, restored);
		require(restored == settings, "fault settings did not survive serialization");
	}
	core::StationConfigurationDocument document; document.settings = settings;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 112") != std::string::npos, "fault opt-in did not bump the wire schema");
	yaml.replace(yaml.find("schemaVersion: 112"), std::string("schemaVersion: 112").size(), "schemaVersion: 111");
	std::string diagnostic;
	require(!core::parseStationConfiguration(yaml, document, &diagnostic)
		&& diagnostic.find("schema 112") != std::string::npos, "pre-112 document silently accepted faults");
	{
		auto bad = replayBytes<core::YamlSerializer>(run);
		auto const at = bad.find("causeKind: 1");
		require(at != std::string::npos, "missing persisted cause");
		bad.replace(at, std::string("causeKind: 1").size(), "causeKind: 99");
		auto in = core::YamlSerializer::fromString(bad); in->deserialize(); in->beginMap("root");
		bool refused = false;
		try { core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "invalid persisted fault cause accepted");
	}
}
void tripLifecycle(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = carbonObservationFixture(); settings.systemsStepWorldTicks = 4;
	settings.fuelCells.clear();
	settings.thermalReservoirs[0].heatCapacityKJPerK = 100;
	settings.thermalReservoirs[0].startingTemperatureC = 30;
	settings.sabatierReactors[0].overtempRunaway = {25, 100};
	settings.methaneProcessors[0].overtempRunaway = {25, 200};
	core::RadiatorSettings radiator; radiator.id = "radiator"; radiator.reservoirId = "sink"; radiator.areaM2 = 100;
	settings.radiators = {radiator};
	// Legacy gates share the very same sink: both must clear without reset,
	// while the two new Trips remain isolated after the sink cools.
	auto hot = settings.thermalReservoirs[0]; hot.id = "hot"; hot.startingTemperatureC = 90;
	hot.heatCapacityKJPerK = 100000; hot.maxTemperatureC = 100; settings.thermalReservoirs.push_back(hot);
	core::HeatEngineSettings engine; engine.id = "engine"; engine.hotReservoirId = "hot";
	engine.rejectionReservoirId = "sink"; engine.thermalClass = "moderate"; engine.ratedThermalInputKW = 1;
	engine.carnotFraction = 0.5; engine.minHotTemperatureC = 50; engine.maxSinkTemperatureC = 25;
	settings.heatEngines = {engine};
	core::BatterySettings battery; battery.id = "battery"; battery.nameplateKWh = 100;
	battery.usableKWh = 100; battery.maxChargeKW = 100; settings.batteries = {battery};
	core::ResourceStoreSettings fuel; fuel.id = "fuel"; fuel.unit = "kWh"; fuel.capacity = 100; fuel.startingQualified = 100;
	settings.stores.push_back(fuel);
	core::GeneratorSettings generator; generator.id = "generator"; generator.fuelStoreId = "fuel";
	generator.heatSinkId = "sink"; generator.thermalClass = "moderate"; generator.ratedOutputKW = 1;
	generator.commandedOutputKW = 1; generator.maxRampKWPerSecond = 10; generator.maxSinkTemperatureC = 25;
	settings.generators = {generator};
	core::StationSystemsRuntime run(settings, 0.25f);
	std::string diagnostic;
	require(!run.resetTrip("reactor", &diagnostic) && !diagnostic.empty(), "unlatched reset accepted");
	require(run.advanceTo(1), "automatic trip failed its audit");
	auto s = run.snapshot();
	require(s.sabatierReactors[0].tripped && s.methaneProcessors[0].tripped
		&& s.sabatierReactors[0].energySuppliedKJ == 0 && s.methaneProcessors[0].energySuppliedKJ == 0,
		"overtemp-runaway did not isolate both installations");
	require(s.thermal.reservoirs[0].hazardEnergyKJ == 300
		&& std::abs(s.thermal.reservoirs[0].residualKJ) < 1e-7, "trip heat was not booked conservatively");
	require(s.heatEngines[0].tripped && s.power.generators[0].outputKW == 0, "legacy gates did not refuse hot sink");
	auto tripEventCount = [&] {
		auto const events = run.events();
		return std::count_if(events.begin(), events.end(), [](auto const& e) { return e.category == "trip"; });
	};
	require(tripEventCount() == 2, "Trip latch events missing or duplicated");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(60), "sink recovery drive failed"); s = run.snapshot();
	require(s.thermal.reservoirs[0].temperatureC < 25 && s.sabatierReactors[0].tripped
		&& s.methaneProcessors[0].tripped && s.thermal.reservoirs[0].hazardEnergyKJ == 300,
		"Trip auto-cleared or repeated its heat spike");
	require(!s.heatEngines[0].tripped && s.power.generators[0].outputKW > 0,
		"new latches changed legacy gate auto-clear behavior");
	auto const inventory = s.stores;
	require(run.resetTrip("reactor") && run.resetTrip("processor") && !run.snapshot().sabatierReactors[0].tripped
		&& !run.snapshot().methaneProcessors[0].tripped && run.snapshot().stores == inventory,
		"reset was not free and instant");
	require(!run.resetTrip("generator", &diagnostic) && !diagnostic.empty()
		&& !run.resetTrip("engine", &diagnostic) && !run.resetTrip("missing", &diagnostic)
		&& !run.resetTrip("reactor", &diagnostic), "non-Trip reset accepted");
	require(tripEventCount() == 4, "Trip reset events missing or refusal emitted an event");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(61) && run.snapshot().sabatierReactors[0].lastCo2ConsumedKg > 0
		&& run.snapshot().methaneProcessors[0].lastMethaneConsumedKg > 0, "reset did not restore service");
	require(run.injectFault("reactor", "overtemp-runaway", {})
		&& !run.injectFault("reactor", "overtemp-runaway", {}) && !run.resetTrip("reactor", &diagnostic),
		"pending trip legality failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	require(run.advanceTo(62) && run.snapshot().thermal.reservoirs[0].hazardEnergyKJ == 400, "injected Trip booking failed");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	auto puncture = punctureStation(); core::StationSystemsRuntime other(puncture, 0.25f);
	require(other.injectFault("loop", "coolant-leak", {}) && other.advanceTo(1)
		&& !other.resetTrip("loop", &diagnostic) && !diagnostic.empty(), "reset cleared a non-Trip fault");
	core::StationConfigurationDocument document; document.settings = settings;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 114") != std::string::npos, "Trip schema not bumped");
	core::StationConfigurationDocument restored;
	require(core::parseStationConfiguration(yaml, restored, &diagnostic) && restored.settings == settings, "Trip settings round trip failed");
	yaml.replace(yaml.find("schemaVersion: 114"), 18, "schemaVersion: 113");
	require(!core::parseStationConfiguration(yaml, restored, &diagnostic), "pre-114 Trip accepted");
}

void tripTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = carbonObservationFixture(); settings.systemsStepWorldTicks = 4;
	settings.sabatierReactors[0].overtempRunaway = {80, 100};
	settings.methaneProcessors[0].overtempRunaway = {80, 200};
	std::string diagnostic;
	for (double invalid : {0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = settings; bad.sabatierReactors[0].overtempRunaway->heatSpikeKJ = invalid;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid Trip heat accepted");
		bad = settings; bad.methaneProcessors[0].overtempRunaway->triggerTemperatureC = -273.15;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "nonphysical Trip threshold accepted");
	}
	auto bad = settings; bad.methaneProcessors[0].id = "reactor";
	require(!core::validateStationSystemsSettings(bad, diagnostic), "ambiguous Trip identity accepted");
	core::StationSystemsRuntime run(settings, 0.25f);
	auto const before = run.snapshot(); auto const events = run.events();
	for (auto const& request : {std::pair{"missing", "overtemp-runaway"}, {"sun", "overtemp-runaway"}, {"reactor", "coolant-leak"}})
		require(!run.injectFault(request.first, request.second, {}, &diagnostic) && !diagnostic.empty(), "invalid Trip injection accepted");
	require(!run.injectFault("reactor", "overtemp-runaway", {core::FaultCause::Kind::ThermalGate, "forged"}, &diagnostic)
		&& run.snapshot() == before && run.events() == events && !run.hasFaultCommands(), "refusal mutated Trip state");
	require(!run.updateNode<core::SabatierReactorSettings>("reactor", [](auto& r) { r.overtempRunaway.reset(); }, &diagnostic),
		"Trip opt-in changed live");
	require(run.injectFault("processor", "overtemp-runaway", {})
		&& run.updateNode<core::MethaneProcessorSettings>("processor", [](auto& p) { p.overtempRunaway->heatSpikeKJ = 500; })
		&& run.advanceTo(1) && run.snapshot().thermal.reservoirs[0].hazardEnergyKJ == 200,
		"retuning changed an accepted booking");
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	// Invalid numeric results must roll back the latch, booking, and event
	// together, not publish a Trip whose heat never entered the ledger.
	auto poison = settings; poison.sabatierReactors[0].overtempRunaway->heatSpikeKJ = std::numeric_limits<double>::max();
	poison.methaneProcessors[0].overtempRunaway->heatSpikeKJ = std::numeric_limits<double>::max();
	core::StationSystemsRuntime failed(poison, 0.25f);
	auto const originalEvents = failed.events();
	require(failed.injectFault("reactor", "overtemp-runaway", {}) && failed.injectFault("processor", "overtemp-runaway", {})
		&& !failed.advanceTo(1) && !failed.snapshot().healthy
		&& failed.snapshot().thermal.reservoirs[0].hazardEnergyKJ == 0
		&& !failed.snapshot().sabatierReactors[0].tripped && failed.events() == originalEvents,
		"failed heat audit published a partial Trip");
	// Both enabled-but-unused models must be observational no-ops.
	auto absent = settings; absent.sabatierReactors[0].overtempRunaway.reset(); absent.methaneProcessors[0].overtempRunaway.reset();
	core::StationSystemsRuntime opted(settings, 0.25f), legacy(absent, 0.25f);
	require(opted.advanceTo(5) && legacy.advanceTo(5) && opted.snapshot() == legacy.snapshot() && opted.events() == legacy.events(),
		"unused Trip opt-in changed legacy operation");
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = settings; offered.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.injectFault("processor", "overtemp-runaway", {}) && stepped.advanceTo(5)
			&& stepped.snapshot().methaneProcessors[0].tripped && stepped.snapshot().thermal.reservoirs[0].hazardEnergyKJ == 200,
			"Trip depended on Station step ratio");
	}
	auto hot = settings; hot.sabatierReactors[0].overtempRunaway->triggerTemperatureC = 20;
	core::StationSystemsRuntime retrip(hot, 0.25f);
	require(retrip.advanceTo(1) && retrip.resetTrip("reactor") && !retrip.snapshot().sabatierReactors[0].tripped
		&& retrip.advanceTo(2) && retrip.snapshot().sabatierReactors[0].tripped
		&& retrip.snapshot().thermal.reservoirs[0].hazardEnergyKJ == 200, "unresolved trigger did not relatch after reset");
	core::World world("Trips", 2, 2); world.pauseSimulation(); world.finishBuild();
	require(!world.resetStationTrip("reactor", &diagnostic) && world.configureStationSystems(settings, false, &diagnostic), "World Trip setup failed");
	world.resumeSimulation(); require(advanceSeconds(world, 1), "World advance failed");
	require(world.injectStationFault("reactor", "overtemp-runaway", {}) && advanceSeconds(world, 1), "World Trip failed");
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work; world.serialize(*out, work); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString() : static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); core::World loaded("Loaded", 2, 2);
		require(loaded.deserialize(*in, work), "World Trip reload failed");
		require(loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot() && loaded.resetStationTrip("reactor")
			&& !loaded.getStationSystemsSnapshot().sabatierReactors[0].tripped, "World Trip persistence/reset failed");
	}
}

void faultTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = punctureStation();
	core::StationSystemsRuntime run(settings, 0.25f);
	auto const before = run.snapshot(); auto const events = run.events();
	std::string diagnostic;
	require(!run.injectFault("loop", "coolant-leak", {static_cast<core::FaultCause::Kind>(99), ""}, &diagnostic)
		&& !run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::Editor, std::string(1025, 'x')}, &diagnostic)
		&& run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
		"invalid causes were not rejected whole");
	require(!run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l) { l.coolantLeak.reset(); }, &diagnostic),
		"fault opt-in topology changed live");
	auto rewired = settings; rewired.coolantLoops[0].coolantLeak->compartmentId = "missing";
	require(!run.updateSettings(rewired, &diagnostic) && run.effectiveSettings() == settings,
		"fault endpoint was rewired live");
	require(run.injectFault("loop", "coolant-leak", {}), "rollback injection failed");
	// The ordinary audit must roll back the hazard booking and its event,
	// along with all other ledger mutations, if a later subsystem fails.
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.specificHeatKJPerKgK = std::numeric_limits<double>::infinity(); }), "audit poison refused");
	require(!run.advanceTo(1) && !run.snapshot().healthy, "broken hazard booking did not fail the audit");
	require(run.snapshot().coolantLoops[0].coolantKg == 100
		&& run.snapshot().coolantLoops[0].activeFaults.empty()
		&& run.snapshot().atmospheres[0].spilledCoolantKg == 0 && run.events() == events,
		"failed audit published a partial fault, spill or event");
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = settings; offered.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.advanceTo(1) && stepped.injectFault("loop", "coolant-leak", {})
			&& stepped.advanceTo(5), "fault failed under a supported Station ratio");
		auto const snap = stepped.snapshot();
		require(snap.coolantLoops[0].coolantKg == 60 && snap.atmospheres[0].spilledCoolantKg == 40
			&& snap.coolantLoops[0].availabilityFactor == 0.6, "fault booking depended on step ratio");
	}
}
// #842: a repair station is the puncture station plus the repair-parts
// wiring: a per-Installation parts store and per-mode repair plans.
core::StationSystemsSettings repairStation()
{
	auto s = punctureStation();
	core::ResourceStoreSettings parts;
	parts.id = "parts"; parts.unit = "kg"; parts.capacity = 100; parts.startingQualified = 10;
	core::ResourceStoreSettings waste;
	waste.id = "waste"; waste.unit = "kg"; waste.capacity = 100; waste.startingQualified = 0;
	s.stores = {parts, waste};
	core::RepairPartsSettings wiring;
	wiring.partsStoreId = "parts";
	wiring.repairs = {
		core::FaultRepairSettings{"coolant-leak", 10.0, 2.0, "waste"},
		core::FaultRepairSettings{"pump-seizure", 20.0, 4.0, "waste"}};
	s.coolantLoops[0].repairParts = wiring;
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
const core::StationSystemsStoreSnapshot* findStoreSnap(core::StationSystemsSnapshot const& snap, std::string const& id)
{
	auto const it = std::find_if(snap.stores.begin(), snap.stores.end(),
		[&](auto const& store) { return store.id == id; });
	return it == snap.stores.end() ? nullptr : &*it;
}
void repairLifecycle(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = repairStation();
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::AgentAction, "agent:1"})
		&& run.advanceTo(1), "leak setup failed");
	require(run.snapshot().coolantLoops[0].availabilityFactor == 0.6, "leak cap missing before repair");
	std::string jobId;
	require(run.repairFault("loop", "coolant-leak", &jobId), "valid repair refused");
	require(jobId == "repair-1", "repair job id not as expected");
	auto const pending = run.snapshot();
	auto const* job = findJob(pending, "repair-1");
	require(job && job->kind == core::StationMaintenanceKind::Repair
		&& job->faultModeId == "coolant-leak" && job->installationId == "loop"
		&& job->phase == core::StationMaintenancePhase::Blocked
		&& job->requirements.size() == 1 && job->requirements[0].storeId == "parts"
		&& job->requirements[0].quantity == 2 && job->requirements[0].consumed
		&& !job->requirements[0].reserved,
		"pending repair missing from the maintenance picture with its requirement detail");
	require(run.advanceTo(3), "repair start failed audit");
	auto const mid = run.snapshot();
	job = findJob(mid, "repair-1");
	require(job && job->phase == core::StationMaintenancePhase::Active && job->isolated
		&& job->requirements[0].reserved && job->progressSeconds > 1.5 && job->remainingSeconds > 6.5,
		"repair did not start, reserve and progress");
	require(mid.coolantLoops[0].availabilityFactor == 0 && mid.coolantLoops[0].flowKgPerS == 0,
		"repair did not isolate the installation at zero availability");
	require(findStoreSnap(mid, "parts")->currentQualified == 10,
		"reserved parts left the store before completion");
	// The repair started at the command boundary (t=1.0) and owes its
	// full 10 s: still isolated just before the completion instant.
	require(run.advanceTo(10.5), "mid-repair drive failed");
	require(run.snapshot().coolantLoops[0].availabilityFactor == 0
		&& findJob(run.snapshot(), "repair-1")->phase == core::StationMaintenancePhase::Active,
		"repair stopped isolating before its full duration");
	require(run.advanceTo(11.5), "repair completion failed audit");
	auto const done = run.snapshot();
	job = findJob(done, "repair-1");
	require(job && job->phase == core::StationMaintenancePhase::Completed && job->exhausted
		&& job->occurrencesCompleted == 1 && !job->isolated
		&& job->requirements[0].lastConsumed == 2 && job->requirements[0].consumedTotal == 2,
		"repair did not complete as a full one-off");
	require(done.coolantLoops[0].activeFaults.empty()
		&& done.coolantLoops[0].availabilityFactor == 1
		&& done.coolantLoops[0].coolantKg == 60,
		"completion did not clear the fault cap, or it replenished coolant");
	require(findStoreSnap(done, "parts")->currentQualified == 8
		&& findStoreSnap(done, "waste")->currentQualified == 2
		&& findStoreSnap(done, "waste")->currentUnqualified == 0,
		"consumed parts did not land in waste as qualified material");
	require(countCategory(run, "repair") == 2 && sawMessage(run, "'repair-1'")
		&& sawMessage(run, "installation isolated") && sawMessage(run, "booked into waste store 'waste'"),
		"repair start/complete events missing");
	require(countCategory(run, "fault") == 2 && sawMessage(run, "cleared by repair 'repair-1'"),
		"fault clear event missing");
	std::string diagnostic;
	require(!run.repairFault("loop", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"repair of a cleared fault accepted");
	// Re-damage after repair is a fresh instance with a fresh repair.
	require(run.injectFault("loop", "coolant-leak", {core::FaultCause::Kind::ScenarioEvent, "strike"})
		&& run.advanceTo(12), "re-injection failed");
	require(run.repairFault("loop", "coolant-leak", &jobId, &diagnostic) && jobId == "repair-2",
		"second repair after re-damage refused or misnumbered");
	require(run.advanceTo(22.5), "second repair failed its drive");
	auto const second = run.snapshot();
	require(second.coolantLoops[0].activeFaults.empty()
		&& findJob(second, "repair-2")->phase == core::StationMaintenancePhase::Completed
		&& findStoreSnap(second, "waste")->currentQualified == 4,
		"second repair did not clear and spend again");
}
void repairInterruption(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// A continuous drain from the parts store breaks the repair's
	// reservation mid-job: progress is discarded, nothing is spent, and
	// after a refill the job reruns the full duration from the restart -
	// the #414 interruption contract on the repair starter.
	auto s = repairStation();
	core::ResourceStoreSettings bilge;
	bilge.id = "bilge"; bilge.unit = "kg"; bilge.capacity = 1000; bilge.startingQualified = 0;
	s.stores.push_back(bilge);
	core::ResourceTransferSettings drain;
	drain.id = "drain"; drain.sourceId = "parts"; drain.destinationId = "bilge";
	drain.unit = "kg"; drain.ratePerSecond = 1; drain.priority = 0;
	s.transfers = {drain};
	core::ResourceDeliverySettings refill;
	refill.id = "refill"; refill.storeId = "parts"; refill.unit = "kg";
	refill.quantity = 20; refill.firstOccurrenceSeconds = 9; refill.recurrenceSeconds = 0;
	s.deliveries = {refill};
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(1), "setup failed");
	require(run.repairFault("loop", "coolant-leak"), "repair with drawable parts refused");
	require(run.advanceTo(3) && findJob(run.snapshot(), "repair-1")->phase
		== core::StationMaintenancePhase::Active, "repair did not start");
	require(run.advanceTo(8.5), "drain drive failed");
	auto const cut = run.snapshot();
	auto const* job = findJob(cut, "repair-1");
	require(job && job->phase == core::StationMaintenancePhase::Blocked && job->progressSeconds == 0,
		"the lost reservation did not interrupt and discard progress");
	require(findStoreSnap(cut, "waste")->currentQualified == 0
		&& !cut.coolantLoops[0].activeFaults.empty(),
		"the interrupted repair spent material or cleared the fault anyway");
	require(sawMessage(run, "interrupted at") && sawMessage(run, "progress discarded"),
		"the interruption was not announced with its discarded progress");
	require(run.advanceTo(10), "refill drive failed");
	job = findJob(run.snapshot(), "repair-1");
	require(job && job->phase == core::StationMaintenancePhase::Active
		&& job->occurrencesStarted == 2 && job->progressSeconds < 2,
		"the repair did not rerun afresh after the refill");
	require(run.advanceTo(19.5), "rerun completion failed");
	auto const done = run.snapshot();
	job = findJob(done, "repair-1");
	require(job && job->phase == core::StationMaintenancePhase::Completed
		&& job->occurrencesCompleted == 1 && job->occurrencesMissed == 0,
		"the rerun did not complete exactly once");
	require(done.coolantLoops[0].activeFaults.empty() && findStoreSnap(done, "waste")->currentQualified == 2,
		"the rerun's spend or fault clear failed");
}
void repairTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	std::string diagnostic;
	// Refusals whole: unknown installation, unknown mode, Trip mode, and
	// no fault to repair - each with no mutation, no event, no command.
	auto s = repairStation();
	core::StationSystemsRuntime run(s, 0.25f);
	auto const before = run.snapshot(); auto const events = run.events();
	require(!run.repairFault("ghost", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"unknown installation accepted");
	require(!run.repairFault("loop", "missing-mode", nullptr, &diagnostic) && !diagnostic.empty(),
		"unknown fault mode accepted");
	require(!run.repairFault("loop", "overtemp-runaway", nullptr, &diagnostic) && !diagnostic.empty(),
		"Trip mode accepted for repair");
	require(!run.repairFault("loop", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"repair with no fault present accepted");
	require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
		"a refused repair mutated state, events or the command journal");
	require(run.injectFault("loop", "coolant-leak", {}) && run.advanceTo(1), "fault setup failed");
	// Installation without wiring, and wiring without a plan for the mode.
	auto noWiring = punctureStation();
	core::StationSystemsRuntime plain(noWiring, 0.25f);
	require(plain.injectFault("loop", "coolant-leak", {}) && plain.advanceTo(1)
		&& !plain.repairFault("loop", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"repair without repair-parts wiring accepted");
	auto otherPlan = repairStation();
	otherPlan.coolantLoops[0].repairParts->repairs = {core::FaultRepairSettings{"pump-seizure", 20, 4, "waste"}};
	core::StationSystemsRuntime unplanned(otherPlan, 0.25f);
	require(unplanned.injectFault("loop", "coolant-leak", {}) && unplanned.advanceTo(1)
		&& !unplanned.repairFault("loop", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"repair without a plan for the mode accepted");
	// Unreservable parts: too little stock, and no waste headroom.
	auto shortParts = repairStation();
	shortParts.stores[0].startingQualified = 1;
	core::StationSystemsRuntime starved(shortParts, 0.25f);
	require(starved.injectFault("loop", "coolant-leak", {}) && starved.advanceTo(1)
		&& !starved.repairFault("loop", "coolant-leak", nullptr, &diagnostic)
		&& diagnostic.find("cannot cover") != std::string::npos,
		"repair with unreservable parts accepted");
	auto tightWaste = repairStation();
	tightWaste.stores[1].capacity = 1;
	core::StationSystemsRuntime clogged(tightWaste, 0.25f);
	require(clogged.injectFault("loop", "coolant-leak", {}) && clogged.advanceTo(1)
		&& !clogged.repairFault("loop", "coolant-leak", nullptr, &diagnostic)
		&& diagnostic.find("headroom") != std::string::npos,
		"repair with no waste headroom accepted");
	// Duplicate: one repair per fault instance.
	require(run.repairFault("loop", "coolant-leak"), "first repair refused");
	auto const started = run.snapshot(); auto const startedEvents = run.events();
	require(!run.repairFault("loop", "coolant-leak", nullptr, &diagnostic) && !diagnostic.empty(),
		"duplicate repair accepted");
	require(run.snapshot() == started && run.events() == startedEvents,
		"the refused duplicate mutated state or events");
	// A pending fault may be repaired immediately after injection.
	auto pendingRun = repairStation();
	core::StationSystemsRuntime pending(pendingRun, 0.25f);
	require(pending.injectFault("loop", "coolant-leak", {}) && pending.repairFault("loop", "coolant-leak", nullptr, &diagnostic),
		"repair of a pending fault refused");
	require(pending.advanceTo(11.5) && pending.snapshot().coolantLoops[0].activeFaults.empty(),
		"pending-fault repair did not clear at completion");
	// Settings validation of the wiring itself.
	for (auto mutation : {"unknown-parts-store", "unknown-waste-store", "zero-duration",
		"zero-parts", "bad-mode", "duplicate-mode", "waste-is-parts", "unit-mismatch"})
	{
		auto bad = repairStation();
		if (std::string_view(mutation) == "unknown-parts-store")
			bad.coolantLoops[0].repairParts->partsStoreId = "nope";
		else if (std::string_view(mutation) == "unknown-waste-store")
			bad.coolantLoops[0].repairParts->repairs[0].wasteStoreId = "nope";
		else if (std::string_view(mutation) == "zero-duration")
			bad.coolantLoops[0].repairParts->repairs[0].durationSeconds = 0;
		else if (std::string_view(mutation) == "zero-parts")
			bad.coolantLoops[0].repairParts->repairs[0].partsQuantity = 0;
		else if (std::string_view(mutation) == "bad-mode")
			bad.coolantLoops[0].repairParts->repairs[0].modeId = "fire";
		else if (std::string_view(mutation) == "duplicate-mode")
			bad.coolantLoops[0].repairParts->repairs.push_back(
				core::FaultRepairSettings{"coolant-leak", 5, 1, "waste"});
		else if (std::string_view(mutation) == "waste-is-parts")
			bad.coolantLoops[0].repairParts->repairs[0].wasteStoreId = "parts";
		else if (std::string_view(mutation) == "unit-mismatch")
			bad.stores[1].unit = "L";
		require(!core::validateStationSystemsSettings(bad, diagnostic),
			std::string("an invalid repair wiring (").append(mutation).append(") was accepted").c_str());
	}
	// Live topology: store identities and the plan set are frozen; the
	// duration and parts quantity of a plan are live-tunable but never
	// reach a repair that already started.
	require(!run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.repairParts->partsStoreId = "other"; }, &diagnostic), "repair parts store rewired live");
	require(!run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.repairParts->repairs[0].modeId = "pump-seizure"; }, &diagnostic), "repair plan mode changed live");
	require(run.updateNode<core::CoolantLoopSettings>("loop", [](auto& l)
		{ l.repairParts->repairs[0].durationSeconds = 30; }), "repair duration retune refused");
	require(run.advanceTo(11.5) && findJob(run.snapshot(), "repair-1")->phase
		== core::StationMaintenancePhase::Completed,
		"the live duration retune reached the already-started repair");
	// Replay: the repair command and its job state cross both serializers.
	roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	// Schema: repair wiring needs 115; a pre-115 document carrying it is refused.
	for (uint32_t version : {114u, 115u})
	{
		auto out = core::YamlSerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto in = core::YamlSerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored;
		bool refused = false;
		try { core::readStationSystemsSettingsMap(*in, version, restored); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused == (version < 115), "repair wiring schema guard failed");
		if (version >= 115) require(restored == s, "repair wiring did not round-trip");
	}
	core::StationConfigurationDocument document; document.settings = s;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 115") != std::string::npos, "repair wiring did not bump the wire schema");
	yaml.replace(yaml.find("schemaVersion: 115"), std::string("schemaVersion: 115").size(), "schemaVersion: 114");
	core::StationConfigurationDocument restoredDoc;
	require(!core::parseStationConfiguration(yaml, restoredDoc, &diagnostic),
		"pre-115 document silently accepted repair wiring");
	// Opt-in no-op: wiring alone changes nothing until a repair is commanded.
	auto absent = repairStation();
	absent.coolantLoops[0].repairParts.reset();
	core::StationSystemsRuntime wired(repairStation(), 0.25f), unwired(absent, 0.25f);
	for (double seconds : {1., 2., 5., 10.})
	{
		require(wired.advanceTo(seconds) && unwired.advanceTo(seconds), "opt-in baseline drive failed");
		require(wired.snapshot() == unwired.snapshot() && wired.events() == unwired.events(),
			"unused repair wiring changed legacy behavior");
	}
	// Step-ratio independence.
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto offered = repairStation(); offered.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime stepped(offered, 1.f / 60.f);
		require(stepped.injectFault("loop", "coolant-leak", {}) && stepped.advanceTo(1)
			&& stepped.repairFault("loop", "coolant-leak") && stepped.advanceTo(12),
			"repair failed under a supported Station ratio");
		auto const snap = stepped.snapshot();
		require(snap.coolantLoops[0].activeFaults.empty()
			&& findStoreSnap(snap, "waste")->currentQualified == 2
			&& findJob(snap, "repair-1")->phase == core::StationMaintenancePhase::Completed,
			"repair depended on the Station step ratio");
	}
	// World surface: command forwarding, save/load with a repair in flight.
	core::World world("Repairs", 2, 2); world.pauseSimulation(); world.finishBuild();
	require(!world.repairStationFault("loop", "coolant-leak", nullptr, &diagnostic)
		&& world.configureStationSystems(repairStation(), false, &diagnostic), "World repair setup failed");
	world.resumeSimulation();
	require(advanceSeconds(world, 1) && world.injectStationFault("loop", "coolant-leak", {})
		&& advanceSeconds(world, 1), "World fault injection failed");
	std::string worldJob;
	require(world.repairStationFault("loop", "coolant-leak", &worldJob, &diagnostic) && worldJob == "repair-1",
		"World repair command failed");
	require(advanceSeconds(world, 5), "World mid-repair drive failed");
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
		require(loaded.deserialize(*in, work), "World repair reload failed");
		require(loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot()
			&& loaded.getStationSystemsEvents() == world.getStationSystemsEvents(),
			"World save/load lost the in-flight repair");
	}
	require(advanceSeconds(world, 6), "World repair completion drive failed");
	require(world.getStationSystemsSnapshot().coolantLoops[0].activeFaults.empty()
		&& findJob(world.getStationSystemsSnapshot(), "repair-1")->phase
			== core::StationMaintenancePhase::Completed,
		"World repair did not complete");
}
void repairSeizureClearing(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// A degradation-spawned pump-seizure is repairable like any other
	// Fault: the crossing raises it, the repair clears it, and the edge-
	// triggered crossing does not instantly re-raise while Condition
	// stays below the threshold.
	auto s = repairStation();
	s.coolantLoops[0].coolantLeak.reset();
	s.coolantLoops[0].condition = core::InstallationConditionSettings{};
	s.coolantLoops[0].condition->initialCondition = 0.2;
	s.coolantLoops[0].condition->seizureThreshold = 0.15;
	s.coolantLoops[0].condition->overheatStressC = 10;
	s.coolantLoops[0].condition->overheatDegradePerHour = 360;
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(1) && run.snapshot().coolantLoops[0].availabilityFactor == 0,
		"seizure crossing did not stop the loop");
	require(run.repairFault("loop", "pump-seizure"), "seizure repair refused");
	require(run.advanceTo(5) && findJob(run.snapshot(), "repair-1")->phase
		== core::StationMaintenancePhase::Active,
		"seizure repair did not start");
	require(run.advanceTo(21.5), "seizure repair completion failed");
	auto const done = run.snapshot();
	require(done.coolantLoops[0].activeFaults.empty()
		&& done.coolantLoops[0].availabilityFactor == 1,
		"the repair did not clear the seizure and lift the zero cap");
	require(sawMessage(run, "Fault 'pump-seizure' on coolant loop 'loop' cleared by repair 'repair-1'"),
		"the seizure clear was not announced");
	require(run.advanceTo(23) && run.snapshot().coolantLoops[0].activeFaults.empty(),
		"the cleared seizure re-raised without a fresh crossing");
}
core::StationSystemsSettings atmosphereFaultStation()
{
	auto s = punctureStation();
	s.atmospheres[0].startingOxygenKg = 10;
	s.atmospheres[0].startingNitrogenKg = 30;
	s.atmospheres[0].fire = core::AtmosphereFireSettings{2, 0.1, 1, 20, 2};
	core::AtmospherePathwaySettings path;
	path.id = "breach"; path.aAtmosphereId = "room";
	path.bAtmosphereId = core::StationExteriorAtmosphereId;
	path.areaM2 = 1; path.openFraction = 0;
	s.atmospherePathways = {path};
	core::AirlockCycleSettings cycle;
	cycle.id = "lock"; cycle.atmosphereId = "room";
	cycle.gasMassKg = 4; cycle.firstOccurrenceSeconds = 1;
	cycle.recurrenceSeconds = 1; cycle.recoveredFraction = 0.75;
	cycle.interruptedCycle = true;
	cycle.interlockFailure = core::AirlockInterlockFailureSettings{"breach", 100};
	s.airlockCycles = {cycle};
	return s;
}
void atmosphereFire(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (uint32_t ratio : {1u, 6u, 60u})
	{
		auto s = atmosphereFaultStation(); s.airlockCycles.clear();
		s.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime run(s, 1.f / 60.f);
		require(run.injectFault("room", "fire", {}) && run.advanceTo(1), "fire failed its audit");
		auto const burning = run.snapshot().atmospheres[0];
		require(burning.activeFaults.size() == 1 && burning.activeFaults[0].derivedImpact == 1
			&& approx(burning.fireOxygenConsumedKg, 2) && approx(burning.fireCarbonDioxideKg, 1)
			&& approx(burning.carbonMonoxideKg, 0.1) && approx(burning.fireHeatKJ, 20),
			"fire rates or derived impact were not booked together");
		require(run.advanceTo(10), "fire extinction failed its audit");
		auto const done = run.snapshot().atmospheres[0];
		require(done.activeFaults.empty() && approx(done.oxygenKg, 2)
			&& approx(done.fireOxygenConsumedKg, 8) && approx(done.fireCarbonDioxideKg, 4)
			&& approx(done.carbonMonoxideKg, 0.4) && approx(done.fireHeatKJ, 80)
			&& approx(done.hazardHeatKJ, 80) && approx(done.temperatureC, 20.008),
			"fire did not self-extinguish conservatively at its oxygen boundary");
		require(run.advanceTo(12) && run.snapshot().atmospheres[0] == done,
			"extinguished fire continued emitting");
		require(countCategory(run, "fault") == 1 && countCategory(run, "fault-extinguished") == 1,
			"fire onset or extinction event missing or repeated");
	}
	// Extinction retires an instance, not its cumulative material/heat ledger.
	{
		auto s = atmosphereFaultStation(); s.airlockCycles.clear();
		s.atmospheres[0].startingOxygenKg = 1;
		s.atmospheres[0].fire = core::AtmosphereFireSettings{1, 0.1, 1, 20, 0.9};
		auto donor = s.atmospheres[0]; donor.id = "donor"; donor.fire.reset();
		donor.startingOxygenKg = 10;
		s.atmospheres.push_back(donor);
		s.atmospherePathways = {{"refill", "donor", "room", 10000, 0}};
		core::StationSystemsRuntime run(s, 0.25f);
		require(run.injectFault("room", "fire", {core::FaultCause::Kind::Editor, "first"})
			&& run.advanceTo(0.25) && run.snapshot().healthy
			&& run.snapshot().atmospheres[0].activeFaults.empty(), "first fire did not extinguish");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.updateNode<core::AtmospherePathwaySettings>("refill", [](auto& p) { p.openFraction = 1; })
			&& run.advanceTo(0.5)
			&& run.updateNode<core::AtmospherePathwaySettings>("refill", [](auto& p) { p.openFraction = 0; }),
			"oxygen replenishment failed");
		auto const refilled = run.snapshot().atmospheres[0];
		require(refilled.oxygenKg > 1 && refilled.activeFaults.empty()
			&& approx(refilled.fireOxygenConsumedKg, 0.1) && approx(refilled.fireHeatKJ, 2)
			&& run.advanceTo(0.75) && run.snapshot().atmospheres[0].activeFaults.empty()
			&& run.snapshot().atmospheres[0].oxygenKg == refilled.oxygenKg
			&& run.snapshot().atmospheres[0].fireHeatKJ == refilled.fireHeatKJ,
			"oxygen replenishment automatically reignited fire");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.injectFault("room", "fire", {core::FaultCause::Kind::AgentAction, "second"}),
			"extinguished fire blocked fresh explicit injection");
		require(!run.injectFault("room", "fire", {}), "pending duplicate fire accepted");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.advanceTo(1), "reinjected fire failed audit");
		auto const burning = run.snapshot().atmospheres[0];
		require(burning.activeFaults.size() == 1 && burning.activeFaults[0].onsetTick == 4
			&& burning.activeFaults[0].causeKind == core::FaultCause::Kind::AgentAction
			&& burning.activeFaults[0].causeDetail == "second"
			&& approx(burning.fireOxygenConsumedKg, 0.35), "reinjection lost fresh onset/cause or prior ledger");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.advanceTo(12), "repeated fire failed cumulative audit");
		auto const done = run.snapshot().atmospheres[0];
		auto const consumed = 0.1 + refilled.oxygenKg - 0.9;
		require(done.activeFaults.empty() && approx(done.oxygenKg, 0.9)
			&& approx(done.fireOxygenConsumedKg, consumed)
			&& approx(done.carbonMonoxideKg, consumed * 0.1)
			&& approx(done.fireCarbonDioxideKg, consumed)
			&& approx(done.fireHeatKJ, consumed * 20) && approx(done.hazardHeatKJ, consumed * 20)
			&& countCategory(run, "fault") == 2 && countCategory(run, "fault-extinguished") == 2,
			"repeated fires reset or double-counted cumulative bookings or transitions");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
	}
	// Already below the threshold: emit no part of the compound dose.
	auto s = atmosphereFaultStation(); s.airlockCycles.clear(); s.atmospheres[0].startingOxygenKg = 1;
	core::StationSystemsRuntime cold(s, 0.25f);
	require(cold.injectFault("room", "fire", {}) && cold.advanceTo(1)
		&& cold.snapshot().atmospheres[0].fireHeatKJ == 0
		&& cold.snapshot().atmospheres[0].activeFaults.empty(), "oxygen-starved fire emitted material or heat");
}
void airlockFaultBookings(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = atmosphereFaultStation();
	core::StationSystemsRuntime interrupted(s, 0.25f), normal(s, 0.25f);
	require(interrupted.injectFault("lock", "interrupted-cycle", {}) && interrupted.advanceTo(2)
		&& normal.advanceTo(2), "interrupted cycle failed audit");
	auto const broken = interrupted.snapshot(); auto const nominal = normal.snapshot();
	require(broken.airlockCycles[0].occurrencesCompleted == 2
		&& broken.airlockCycles[0].removedTotal == 8 && broken.airlockCycles[0].lostTotal == 8
		&& broken.airlockCycles[0].faultLostKg == 8 && broken.airlockCycles[0].recoveredTotal == 0
		&& nominal.airlockCycles[0].removedTotal == 8 && nominal.airlockCycles[0].lostTotal == 2
		&& broken.atmospheres[0].oxygenKg == nominal.atmospheres[0].oxygenKg
		&& broken.airlockCycles[0].activeFaults[0].derivedImpact == 1,
		"interrupted cycle double-counted a scheduled removal or recovered vented gas");
	// A breach uses exactly the same exterior solve as an authored open pathway.
	s.airlockCycles[0].firstOccurrenceSeconds = 100;
	auto opened = s; opened.atmospherePathways[0].areaM2 = 100;
	opened.atmospherePathways[0].openFraction = 1;
	core::StationSystemsRuntime breach(s, 0.25f), equivalent(opened, 0.25f);
	require(breach.injectFault("lock", "interlock-failure", {}) && breach.advanceTo(2)
		&& equivalent.advanceTo(2), "breach failed audit");
	auto const snap = breach.snapshot();
	require(snap.atmospherePathways[0].netTransferredKg == equivalent.snapshot().atmospherePathways[0].netTransferredKg
		&& snap.atmospheres == equivalent.snapshot().atmospheres
		&& snap.atmospheres[0].exteriorLostKg[0] > 0
		&& snap.airlockCycles[0].lostTotal == 0 && snap.airlockCycles[0].faultLostKg == 0
		&& snap.airlockCycles[0].activeFaults[0].derivedImpact == 1,
		"breach bypassed or duplicated the exterior pathway ledger");
}
void atmosphereFaultReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (auto const& command : std::vector<std::pair<std::string, std::string>>{
		{"room", "fire"}, {"lock", "interrupted-cycle"}, {"lock", "interlock-failure"}})
	{
		auto s = atmosphereFaultStation(); core::StationSystemsRuntime run(s, 0.25f);
		auto const before = run.snapshot(); auto const events = run.events();
		require(run.injectFault(command.first, command.second, {})
			&& !run.injectFault(command.first, command.second, {})
			&& run.snapshot() == before && run.events() == events, "injection boundary or duplicate refusal failed");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		// Accepted parameters must not change under later live retuning.
		require(run.updateNode<core::AtmosphereSettings>("room", [](auto& a) { a.fire->heatKW = 50; })
			&& run.updateNode<core::AirlockCycleSettings>("lock", [](auto& c) { c.interlockFailure->breachAreaM2 = 5; }),
			"fault rate retune refused");
		require(run.advanceTo(2), "atmosphere drive failed");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		require(run.advanceTo(10), "atmosphere continuation failed");
		roundTrip<core::BinarySerializer>(run); roundTrip<core::YamlSerializer>(run);
		if (command.second == "fire") require(approx(run.snapshot().atmospheres[0].fireHeatKJ,
			run.snapshot().atmospheres[0].fireOxygenConsumedKg * 10), "live edit rewrote accepted fire parameters");
	}
	core::StationConfigurationDocument document; document.settings = atmosphereFaultStation();
	auto const yaml = core::serializeStationConfiguration(document);
	core::StationConfigurationDocument parsed; std::string diagnostic;
	require(yaml.find("schemaVersion: 120") != std::string::npos
		&& core::parseStationConfiguration(yaml, parsed, &diagnostic)
		&& parsed.settings == document.settings, "atmosphere configuration schema or round trip failed");
	for (bool binary : {false, true})
	{
		auto s = atmosphereFaultStation();
		std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		for (uint32_t version : {119u, 120u})
		{
			std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
			in->deserialize(); in->beginMap("root"); core::StationSystemsSettings loaded; bool threw = false;
			try { core::readStationSystemsSettingsMap(*in, version, loaded); }
			catch (core::SerializationException const&) { threw = true; }
			require(version == 119 ? threw : !threw && loaded == s, "atmosphere schema boundary or round trip failed");
		}
	}
}
void atmosphereFaultWorld(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	core::World world("Atmosphere faults", 2, 2); world.pauseSimulation(); world.finishBuild();
	require(world.configureStationSystems(atmosphereFaultStation(), false), "World atmosphere setup failed");
	world.resumeSimulation();
	require(advanceSeconds(world, 1) && world.injectStationFault("room", "fire", {})
		&& world.injectStationFault("lock", "interrupted-cycle", {})
		&& world.injectStationFault("lock", "interlock-failure", {}), "World fault forwarding failed");
	for (bool pending : {true, false})
	{
		if (!pending) require(advanceSeconds(world, 2), "World fault drive failed");
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
			require(loaded.deserialize(*in, work), "World atmosphere reload failed");
			require(loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot()
				&& loaded.getStationSystemsEvents() == world.getStationSystemsEvents(),
				"World save/load lost pending or applied atmosphere faults");
		}
	}
}
void atmosphereFaultTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = atmosphereFaultStation(); core::StationSystemsRuntime run(s, 0.25f);
	auto const before = run.snapshot(); auto const events = run.events();
	std::string diagnostic;
	for (auto const& command : std::vector<std::pair<std::string, std::string>>{
		{"room", "interrupted-cycle"}, {"lock", "fire"}, {"room", "interlock-failure"}})
		require(!run.injectFault(command.first, command.second, {}, &diagnostic) && !diagnostic.empty()
			&& run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
			"invalid atmosphere fault mutated runtime");
	require(!run.updateNode<core::AtmosphereSettings>("room", [](auto& a) { a.fire.reset(); })
		&& !run.updateNode<core::AirlockCycleSettings>("lock", [](auto& c) { c.interruptedCycle = false; })
		&& run.snapshot() == before, "fault topology changed during a run");
	for (double bad : {-1., 0., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto invalid = s; invalid.atmospheres[0].fire->oxygenKgPerS = bad;
		require(!core::validateStationSystemsSettings(invalid, diagnostic), "invalid fire rate accepted");
		invalid = s; invalid.airlockCycles[0].interlockFailure->breachAreaM2 = bad;
		require(!core::validateStationSystemsSettings(invalid, diagnostic), "invalid breach area accepted");
	}
	core::StationSystemsRuntime retuned(s, 0.25f);
	require(retuned.updateNode<core::AtmosphereSettings>("room", [](auto& a) { a.fire->oxygenKgPerS = -1; })
		&& retuned.updateNode<core::AirlockCycleSettings>("lock", [](auto& c) { c.interlockFailure->breachAreaM2 = 0; })
		&& !retuned.injectFault("room", "fire", {}, &diagnostic)
		&& !retuned.injectFault("lock", "interlock-failure", {}, &diagnostic)
		&& !retuned.hasFaultCommands(), "invalid live parameters entered the fault ledger");
	auto removed = s; removed.airlockCycles[0].interlockFailure.reset();
	require(!run.updateSettings(removed, &diagnostic), "bulk update changed fault topology");
	auto collision = s; collision.atmospheres[0].id = "solar";
	require(!core::validateStationSystemsSettings(collision, diagnostic), "ambiguous fire identity accepted");
	collision = s; collision.airlockCycles[0].id = "room";
	require(!core::validateStationSystemsSettings(collision, diagnostic), "ambiguous airlock identity accepted");
	auto invalid = s; invalid.airlockCycles[0].interlockFailure->pathwayId = "missing";
	require(!core::validateStationSystemsSettings(invalid, diagnostic), "missing breach pathway accepted");
	auto absent = s; absent.atmospheres[0].fire.reset(); absent.airlockCycles[0].interruptedCycle = false;
	absent.airlockCycles[0].interlockFailure.reset();
	// A failed audit must roll back all four fire bookings and its onset event.
	auto overflow = s; overflow.airlockCycles.clear();
	overflow.atmospheres[0].fire->heatKW = 1e308;
	overflow.atmospheres[0].thermalCapacityKJPerK = 1e-300;
	core::StationSystemsRuntime failing(overflow, 0.25f);
	auto const unchanged = failing.snapshot(); auto const oldEvents = failing.events();
	require(failing.injectFault("room", "fire", {}) && !failing.advanceTo(0.25)
		&& !failing.snapshot().healthy && failing.snapshot().atmospheres == unchanged.atmospheres
		&& failing.snapshot().worldTick == unchanged.worldTick && failing.events() == oldEvents,
		"failed fire audit published a partial compound booking");
	core::StationSystemsRuntime baseline(absent, 0.25f);
	require(!baseline.injectFault("room", "fire", {}) && !baseline.injectFault("lock", "interrupted-cycle", {})
		&& !baseline.injectFault("lock", "interlock-failure", {}), "absent opt-in accepted a fault");
	require(run.advanceTo(10) && baseline.advanceTo(10) && run.snapshot() == baseline.snapshot()
		&& run.events() == baseline.events(), "unused atmosphere opt-ins changed baseline behavior");
}
void faultOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto enabled = punctureStation(), absent = enabled;
	absent.coolantLoops[0].coolantLeak.reset();
	core::StationSystemsRuntime a(enabled, 0.25f), b(absent, 0.25f);
	std::string diagnostic;
	require(!b.injectFault("loop", "coolant-leak", {}, &diagnostic) && !diagnostic.empty(), "absent opt-in accepted fault");
	for (double seconds : {1., 2., 5., 10.})
	{
		require(a.advanceTo(seconds) && b.advanceTo(seconds), "opt-in baseline drive failed");
		require(a.snapshot() == b.snapshot() && a.events() == b.events(), "unused opt-in changed legacy behavior");
	}
	for (double mass : {-1., 0., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto invalid = enabled; invalid.coolantLoops[0].coolantLeak->releasedKg = mass;
		require(!core::validateStationSystemsSettings(invalid, diagnostic), "invalid booking size accepted");
	}
	auto invalid = enabled; invalid.coolantLoops[0].coolantLeak->compartmentId = "missing";
	require(!core::validateStationSystemsSettings(invalid, diagnostic), "unknown hazard destination accepted");
}
}
void registerStationFaultChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"tripLifecycle", tripLifecycle});
	checks.push_back({"tripTransactions", tripTransactions});
	checks.push_back({"atmosphereFaultWorld", atmosphereFaultWorld});
	checks.push_back({"atmosphereFire", atmosphereFire});
	checks.push_back({"airlockFaultBookings", airlockFaultBookings});
	checks.push_back({"atmosphereFaultReplay", atmosphereFaultReplay});
	checks.push_back({"atmosphereFaultTransactions", atmosphereFaultTransactions});
	checks.push_back({"faultInjection", faultInjection});
	checks.push_back({"faultOutages", faultOutages});
	checks.push_back({"faultReplay", faultReplay});
	checks.push_back({"faultOptIn", faultOptIn});
	checks.push_back({"faultTransactions", faultTransactions});
	checks.push_back({"repairLifecycle", repairLifecycle});
	checks.push_back({"repairInterruption", repairInterruption});
	checks.push_back({"repairTransactions", repairTransactions});
	checks.push_back({"repairSeizureClearing", repairSeizureClearing});
}
