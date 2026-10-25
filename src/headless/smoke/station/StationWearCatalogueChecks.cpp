// #848: the permanent Wear catalogue. Every station machine type that was
// left out of the #843 pilot opts into the same layer here, under its own
// named mode, and each mode must fade the capability that machine actually
// declares. The checks are made on what a driven Station publishes - never
// on integrator internals - so a mode that claims a rating it does not
// enforce fails here rather than in a player's save.
#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
using namespace smoke::station;

// The model every catalogue case runs on: one hour of real work costs a
// tenth of the declared rating, one hour of calendar costs a hundredth.
// The operating term dominates a working machine, and the calendar term
// keeps a machine that never works ageing anyway - which is what makes an
// opted-in machine impossible to keep forever.
core::InstallationWearSettings catalogueModel(double operating = 0.1, double calendar = 0.01,
	double throughput = 0.0, double ceiling = 1.0)
{
	return {0.0, operating, throughput, calendar, ceiling};
}

core::ResourceStoreSettings stock(std::string const& id, std::string const& unit, double amount)
{
	core::ResourceStoreSettings s;
	s.id = id; s.unit = unit; s.capacity = 100000.0; s.startingQualified = amount;
	return s;
}

core::ThermalReservoirSettings reservoir(std::string const& id, std::string const& thermalClass,
	double capacityKJPerK, double startingC)
{
	core::ThermalReservoirSettings r;
	r.id = id; r.thermalClass = thermalClass; r.heatCapacityKJPerK = capacityKJPerK;
	r.startingTemperatureC = startingC; r.minTemperatureC = -50.0; r.maxTemperatureC = 400.0;
	return r;
}

// The conversion plant: every machine that consumes power and mass runs off
// a sun that never runs short, and every input store is pre-filled so no
// machine waits on an upstream one to get its first kilogram.
core::StationSystemsSettings processStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	s.stores = {
		stock("rawWater", "kg", 5000.0), stock("treatedWater", "kg", 5000.0),
		stock("concentrate", "kg", 0.0), stock("kit", "kg", 5000.0),
		stock("h2", "kg", 5000.0), stock("o2", "kg", 5000.0),
		stock("co2", "kg", 5000.0), stock("methane", "kg", 5000.0),
		stock("carbon", "kg", 0.0), stock("waterProduct", "kg", 0.0),
		stock("captured", "kg", 0.0), stock("scrubberMedia", "kg", 5000.0),
	};
	core::AtmosphereSettings habitat;
	habitat.id = "habitat"; habitat.volumeM3 = 100.0;
	habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
	habitat.startingCarbonDioxideKg = 50.0; habitat.startingTemperatureC = 25.0;
	habitat.thermalCapacityKJPerK = 10000.0;
	habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
	s.atmospheres = {habitat};
	s.thermalReservoirs = {reservoir("sink", "low", 1.0e6, 10.0)};

	core::SolarArraySettings sun;
	sun.id = "sun"; sun.capacityKW = 500.0; sun.illumination = constantProfile(1.0);
	sun.cellDegradation = catalogueModel();
	s.solarArrays = {sun};

	core::WaterTreatmentSettings filter;
	filter.id = "filter"; filter.sourceStoreId = "rawWater";
	filter.treatedStoreId = "treatedWater"; filter.concentrateStoreId = "concentrate";
	filter.consumableStoreId = "kit";
	filter.throughputKgPerS = 0.002; filter.specificEnergyKWhPerKg = 0.5;
	filter.consumablePerKg = 0.01;
	filter.membraneFouling = catalogueModel();
	s.waterTreatments = {filter};

	core::ElectrolyzerSettings electro;
	electro.id = "electro"; electro.waterStoreId = "treatedWater";
	electro.oxygenStoreId = "o2"; electro.hydrogenStoreId = "h2";
	electro.thermalClass = "low"; electro.heatSinkId = "sink";
	electro.rateKgPerS = 0.001; electro.specificEnergyKWhPerKgWater = 4.5;
	electro.membraneDegradation = catalogueModel();
	s.electrolyzers = {electro};

	core::Co2CaptureSettings capture;
	capture.id = "capture"; capture.atmosphereId = "habitat";
	capture.captureStoreId = "captured"; capture.consumableStoreId = "scrubberMedia";
	capture.maxRateKgPerS = 0.001; capture.specificEnergyKWhPerKg = 0.5;
	capture.consumablePerKg = 0.01;
	capture.absorbentDegradation = catalogueModel();
	s.co2Captures = {capture};

	core::SabatierReactorSettings sabatier;
	sabatier.id = "sabatier"; sabatier.co2StoreId = "co2";
	sabatier.hydrogenStoreId = "h2"; sabatier.methaneStoreId = "methane";
	sabatier.waterStoreId = "waterProduct";
	sabatier.thermalClass = "low"; sabatier.heatSinkId = "sink";
	sabatier.rateKgPerS = 0.001; sabatier.specificEnergyKWhPerKgCO2 = 1.0;
	sabatier.catalystDeactivation = catalogueModel();
	s.sabatierReactors = {sabatier};

	core::MethaneProcessorSettings methane;
	methane.id = "methane"; methane.methaneStoreId = "methane";
	methane.hydrogenStoreId = "h2"; methane.carbonStoreId = "carbon";
	methane.thermalClass = "low"; methane.heatSinkId = "sink";
	methane.rateKgPerS = 0.001; methane.specificEnergyKWhPerKgCH4 = 2.0;
	methane.catalystDeactivation = catalogueModel();
	s.methaneProcessors = {methane};
	return s;
}

// The dispatch plant: the sun cannot reach the load, so every machine that
// only runs when it is called actually is called. The heat engine carries
// its own live gradient; the generator and fuel cell carry their own
// stores.
core::StationSystemsSettings dispatchStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	s.stores = {
		stock("diesel", "kWh", 100000.0), stock("h2", "kg", 5000.0),
		stock("o2", "kg", 5000.0), stock("waterProduct", "kg", 0.0),
	};
	s.thermalReservoirs = {
		reservoir("hot", "warm", 20000.0, 90.0),
		reservoir("reject", "warm", 20000.0, 20.0),
	};

	core::SolarArraySettings sun;
	sun.id = "sun"; sun.capacityKW = 1.0; sun.illumination = constantProfile(1.0);
	sun.cellDegradation = catalogueModel();
	s.solarArrays = {sun};

	core::EquipmentLoadSettings load;
	load.id = "load"; load.electricalDemandKW = 40.0; load.thermalClass = "moderate";
	load.coolingDisposition = core::StationCoolingDisposition::Exported;
	s.equipmentLoads = {load};

	core::GeneratorSettings gen;
	gen.id = "gen"; gen.fuelStoreId = "diesel";
	gen.thermalClass = "warm"; gen.heatSinkId = "reject";
	gen.ratedOutputKW = 20.0; gen.efficiency = 0.5;
	gen.maxRampKWPerSecond = 50.0; gen.commandedOutputKW = 20.0;
	gen.maxSinkTemperatureC = 200.0;
	gen.fouledInjector = catalogueModel();
	s.generators = {gen};

	core::HeatEngineSettings orc;
	orc.id = "orc"; orc.hotReservoirId = "hot"; orc.rejectionReservoirId = "reject";
	orc.thermalClass = "warm";
	orc.ratedThermalInputKW = 100.0; orc.commandedInputKW = 100.0;
	orc.carnotFraction = 0.5; orc.auxiliaryPowerKW = 0.5;
	orc.minHotTemperatureC = 30.0; orc.maxSinkTemperatureC = 25.0;
	orc.efficiencyDrift = catalogueModel();
	s.heatEngines = {orc};

	core::FuelCellSettings cell;
	cell.id = "cell"; cell.hydrogenStoreId = "h2"; cell.oxygenStoreId = "o2";
	cell.waterStoreId = "waterProduct";
	cell.thermalClass = "warm"; cell.heatSinkId = "reject";
	cell.ratedOutputKW = 20.0; cell.efficiency = 0.6; cell.commandedOutputKW = 20.0;
	cell.maxSinkTemperatureC = 200.0;
	cell.stackDegradation = catalogueModel();
	s.fuelCells = {cell};
	return s;
}

// One catalogue machine as the check sees it: the mode it opted into, the
// rating that mode fades, and the shape the fade takes on that rating.
// `Shrink` is the ordinary case - the machine can do less. `Rise` is the
// process dose: a degraded machine needs more energy per kilogram, so the
// declared kWh/kg goes up. `Divide` is a conversion efficiency that has
// to stretch to cover what the fouling costs it.
enum class Shape { Shrink, Rise, Divide };

struct Case
{
	std::string id, mode;
	double authored{0.0};
	Shape shape{Shape::Shrink};
};

double expectedCapability(Case const& c, double fraction)
{
	switch (c.shape)
	{
	case Shape::Rise: return c.authored * (1.0 + fraction);
	case Shape::Divide: return c.authored / (1.0 + fraction);
	case Shape::Shrink: break;
	}
	return c.authored * (1.0 - fraction);
}

std::map<std::string, core::StationMachineWearSnapshot> allWear(core::StationSystemsSnapshot const& s)
{
	std::map<std::string, core::StationMachineWearSnapshot> out;
	for (auto const& x : s.power.sources) out[x.id] = x.wear;
	for (auto const& x : s.power.generators) out[x.id] = x.wear;
	for (auto const& x : s.heatEngines) out[x.id] = x.wear;
	for (auto const& x : s.waterTreatments) out[x.id] = x.wear;
	for (auto const& x : s.electrolyzers) out[x.id] = x.wear;
	for (auto const& x : s.co2Captures) out[x.id] = x.wear;
	for (auto const& x : s.fuelCells) out[x.id] = x.wear;
	for (auto const& x : s.sabatierReactors) out[x.id] = x.wear;
	for (auto const& x : s.methaneProcessors) out[x.id] = x.wear;
	return out;
}

std::vector<Case> catalogueCases()
{
	return {
		{"sun", "cell-degradation", 500.0, Shape::Shrink},
		{"filter", "membrane-fouling", 0.002, Shape::Shrink},
		{"electro", "membrane-degradation", 4.5, Shape::Rise},
		{"capture", "absorbent-degradation", 0.001, Shape::Shrink},
		{"sabatier", "catalyst-deactivation", 0.001, Shape::Shrink},
		{"methane", "catalyst-deactivation", 0.001, Shape::Shrink},
		{"gen", "fouled-injector", 0.5, Shape::Divide},
		{"orc", "efficiency-drift", 0.5, Shape::Shrink},
		{"cell", "stack-degradation", 20.0, Shape::Shrink},
	};
}

bool sawMessage(core::StationSystemsRuntime const& run, std::string const& needle)
{
	auto const events = run.events();
	return std::any_of(events.begin(), events.end(), [&](auto const& e)
	{
		return e.message.find(needle) != std::string::npos;
	});
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
		"catalogue wear replay did not reproduce exact snapshots and events");
	require(replayBytes<Serializer>(*restored) == bytes,
		"catalogue wear replay log did not round-trip byte-exactly");
	require(restored->advanceTo(run.snapshot().worldTick * 0.25 + 60),
		"restored catalogue wear failed subsequent audited drive");
}

void wearCatalogueFade(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	core::StationSystemsRuntime process(processStation(), 0.25f);
	core::StationSystemsRuntime dispatch(dispatchStation(), 0.25f);
	require(process.advanceTo(3600), "catalogue process drive failed");
	require(dispatch.advanceTo(3600), "catalogue dispatch drive failed");
	auto const pSnap = process.snapshot();
	auto const dSnap = dispatch.snapshot();
	auto const wear = allWear(pSnap);
	auto const dispatchWear = allWear(dSnap);
	// The onset announcement is checked on a short horizon: the event ring
	// is bounded, and a full hour of step observations evicts the tick
	// the fade actually began on.
	// Pulls the (mode, machine) pairs the run actually announced. Keyed by
	// the pair, not the mode: two machines share `catalyst-deactivation`.
	auto const onset = [](core::StationSystemsRuntime& run)
	{
		std::set<std::pair<std::string, std::string>> announced;
		for (auto const& e : run.events())
		{
			auto const head = e.message.find("Wear '");
			if (head == std::string::npos) continue;
			auto const modeEnd = e.message.find("'", head + 6);
			auto const idHead = e.message.find("' on '", head);
			if (modeEnd == std::string::npos || idHead == std::string::npos) continue;
			auto const idEnd = e.message.find("'", idHead + 6);
			announced.insert({e.message.substr(head + 6, modeEnd - head - 6),
				e.message.substr(idHead + 6, idEnd - idHead - 6)});
		}
		return announced;
	};
	core::StationSystemsRuntime brief(processStation(), 0.25f);
	require(brief.advanceTo(30), "catalogue onset horizon failed");
	core::StationSystemsRuntime briefDispatch(dispatchStation(), 0.25f);
	require(briefDispatch.advanceTo(30), "catalogue dispatch onset horizon failed");
	auto shortWear = onset(brief);
	for (auto const& pair : onset(briefDispatch)) shortWear.insert(pair);
	for (auto const& c : catalogueCases())
	{
		auto const& source = c.id == "gen" || c.id == "orc" || c.id == "cell" ? dispatchWear : wear;
		auto const found = source.find(c.id);
		require(found != source.end(), "catalogue machine missing from the wear snapshot");
		auto const& w = found->second;
		// The mode names itself, and the accumulators moved.
		require(w.modeId == c.mode, "catalogue machine reported the wrong wear mode");
		require(w.calendarHours > 0.9 && w.calendarHours < 1.1,
			"catalogue wear calendar hours did not track the run");
		require(w.wearFraction > 0.0 && w.wearFraction <= 1.0,
			"catalogue wear fraction never accumulated");
		// The declared capability faded by exactly the fade the model
		// priced - and the snapshot says so in the rating's own units.
		require(std::abs(w.effectiveCapability - expectedCapability(c, w.wearFraction)) < 1e-9,
			("effective capability off for " + c.id + ": got " + std::to_string(w.effectiveCapability)
				+ " want " + std::to_string(expectedCapability(c, w.wearFraction))
				+ " f=" + std::to_string(w.wearFraction)).c_str());
		require(std::abs(w.derivedImpact - w.wearFraction) < 1e-12,
			"derived impact did not report the fraction of rating lost");
		// A machine that actually worked paid the operating term, which is
		// an order of magnitude above the calendar term, so the fade is
		// clearly duty-driven and not just ageing.
		require(w.wearFraction > 0.02,
			"catalogue wear was calendar-only: the machine never banked its operating term");
		require(w.operatingHours > 0.0, "no operating hours banked by a working machine");
		require(shortWear.count({c.mode, c.id}) == 1,
			("the onset of catalogue wear mode " + c.mode + " on '" + c.id
				+ "' was never announced").c_str());
	}
	// The throughput term is wired where the mode meters mass and absent
	// where it does not.
	require(allWear(process.snapshot())["filter"].throughputKg > 0.0
		&& allWear(process.snapshot())["electro"].throughputKg > 0.0
		&& allWear(process.snapshot())["capture"].throughputKg > 0.0
		&& allWear(process.snapshot())["sabatier"].throughputKg > 0.0
		&& allWear(process.snapshot())["methane"].throughputKg > 0.0,
		"a mass-metered catalogue mode banked no throughput");
	require(allWear(dispatch.snapshot())["sun"].throughputKg == 0.0
		&& allWear(dispatch.snapshot())["gen"].throughputKg == 0.0
		&& allWear(dispatch.snapshot())["orc"].throughputKg == 0.0,
		"a mode with no mass metered throughput anyway");
	// A throughput rate actually fades what it meters: double the
	// throughput rate and the fade at the same mass is larger.
	auto heavy = processStation();
	heavy.waterTreatments[0].membraneFouling = catalogueModel(0.0, 0.0, 0.01);
	core::StationSystemsRuntime heavyRun(heavy, 0.25f);
	require(heavyRun.advanceTo(3600), "throughput-driven wear drive failed");
	auto const& heavyWear = allWear(heavyRun.snapshot())["filter"];
	require(heavyWear.throughputKg > 1.0 && heavyWear.wearFraction > 0.0
		&& std::abs(heavyWear.wearFraction - heavyWear.throughputKg * 0.01) < 1e-9,
		"the throughput term did not price the fade from the mass actually processed");
}

void wearCataloguePermanence(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	core::StationSystemsRuntime run(processStation(), 0.25f);
	require(run.advanceTo(600), "catalogue permanence setup failed");
	// Monotone: across the whole run no machine's fade ever recedes.
	std::map<std::string, double> previous;
	for (auto const& [id, w] : allWear(run.snapshot())) previous[id] = w.wearFraction;
	for (double at : {1200., 1800., 2400., 3600.})
	{
		require(run.advanceTo(at), "catalogue permanence drive failed");
		for (auto const& [id, w] : allWear(run.snapshot()))
			require(w.wearFraction >= previous[id] - 1e-12,
				"a catalogue wear fraction receded mid-run");
		for (auto const& [id, w] : allWear(run.snapshot())) previous[id] = w.wearFraction;
	}
	// Wear is not a Fault: no command surface can inject it, repair it or
	// reset it. A machine cannot be made new again by fixing it.
	std::string diagnostic;
	auto const before = run.snapshot();
	auto const events = run.events();
	for (auto const& c : catalogueCases())
	{
		require(!run.injectFault(c.id, c.mode, {}, &diagnostic) && !diagnostic.empty(),
			"a catalogue wear fade was treated as an injectable Fault");
		require(!run.repairFault(c.id, c.mode, nullptr, &diagnostic) && !diagnostic.empty(),
			"a catalogue wear fade was treated as a repairable Fault");
		require(!run.resetTrip(c.id, &diagnostic) && !diagnostic.empty(),
			"a catalogue wear fade was treated as a resettable trip");
	}
	require(run.snapshot() == before && run.events() == events && !run.hasFaultCommands(),
		"a refused catalogue wear command mutated state, events or the command journal");
	// Lowering the ceiling mid-run stops future fade but never heals the
	// fade already banked.
	auto const worn = allWear(run.snapshot())["filter"].wearFraction;
	require(worn > 0.0, "no fade banked before the ceiling was lowered");
	require(run.updateNode<core::WaterTreatmentSettings>("filter",
		[&](auto& t) { t.membraneFouling->maxWearFraction = worn * 0.5; }, &diagnostic),
		"lowering the wear ceiling live was refused");
	require(run.advanceTo(4200), "post-ceiling drive failed");
	require(std::abs(allWear(run.snapshot())["filter"].wearFraction - worn) < 1e-12,
		"lowering the wear ceiling healed fade that had already accumulated");
}

template<class Serializer>
void retunedReplay(core::StationSystemsRuntime& run)
{
	auto const bytes = replayBytes<Serializer>(run);
	auto in = Serializer::fromString(bytes); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"retuned catalogue Wear-only replay lost historical rates");
	require(replayBytes<Serializer>(*restored) == bytes, "retuned catalogue journal changed on reload");
	auto const next = run.snapshot().worldTick * 0.25 + 60;
	require(run.advanceTo(next) && restored->advanceTo(next)
		&& restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"retuned catalogue Wear-only continuation diverged");
}

void wearCatalogueReplay(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// #857: exercise each opt-in in isolation, so another machine's Wear
	// cannot accidentally pin the initial settings on its behalf.
	auto checkRetune = [](auto settings, auto selectModel)
	{
		for (auto& a : settings.solarArrays) a.cellDegradation.reset();
		for (auto& g : settings.generators) g.fouledInjector.reset();
		for (auto& e : settings.heatEngines) e.efficiencyDrift.reset();
		for (auto& t : settings.waterTreatments) t.membraneFouling.reset();
		for (auto& e : settings.electrolyzers) e.membraneDegradation.reset();
		for (auto& c : settings.co2Captures) c.absorbentDegradation.reset();
		for (auto& f : settings.fuelCells) f.stackDegradation.reset();
		for (auto& s : settings.sabatierReactors) s.catalystDeactivation.reset();
		for (auto& m : settings.methaneProcessors) m.catalystDeactivation.reset();
		selectModel(settings) = catalogueModel(0.0, 0.2);
		core::StationSystemsRuntime run(settings, 0.25f);
		require(run.advanceTo(30), "isolated catalogue Wear setup failed");
		selectModel(settings)->fadePerCalendarHour = 0.4;
		require(run.updateSettings(settings) && run.advanceTo(60), "catalogue Wear retune failed");
		retunedReplay<core::YamlSerializer>(run);
		retunedReplay<core::BinarySerializer>(run);
	};
	checkRetune(processStation(), [](auto& s) -> auto& { return s.solarArrays[0].cellDegradation; });
	checkRetune(processStation(), [](auto& s) -> auto& { return s.waterTreatments[0].membraneFouling; });
	checkRetune(processStation(), [](auto& s) -> auto& { return s.electrolyzers[0].membraneDegradation; });
	checkRetune(processStation(), [](auto& s) -> auto& { return s.co2Captures[0].absorbentDegradation; });
	checkRetune(processStation(), [](auto& s) -> auto& { return s.sabatierReactors[0].catalystDeactivation; });
	checkRetune(processStation(), [](auto& s) -> auto& { return s.methaneProcessors[0].catalystDeactivation; });
	checkRetune(dispatchStation(), [](auto& s) -> auto& { return s.generators[0].fouledInjector; });
	checkRetune(dispatchStation(), [](auto& s) -> auto& { return s.heatEngines[0].efficiencyDrift; });
	checkRetune(dispatchStation(), [](auto& s) -> auto& { return s.fuelCells[0].stackDegradation; });
	// Catalogue wear integrates over the run with no client command at
	// all, so a save of a worn station needs the replay block and must
	// come back with the fade exactly where it was left.
	core::StationSystemsRuntime process(processStation(), 0.25f);
	require(process.advanceTo(900), "catalogue replay setup failed");
	require(process.hasFaultReplayState(),
		"a catalogue-wear-only run does not need the replay block");
	require(allWear(process.snapshot())["filter"].wearFraction > 0.0,
		"no catalogue fade accumulated before the replay round trip");
	roundTrip<core::BinarySerializer>(process);
	roundTrip<core::YamlSerializer>(process);
	core::StationSystemsRuntime dispatch(dispatchStation(), 0.25f);
	require(dispatch.advanceTo(900), "catalogue dispatch replay setup failed");
	require(dispatch.hasFaultReplayState(),
		"a dispatch catalogue-wear run does not need the replay block");
	roundTrip<core::BinarySerializer>(dispatch);
	roundTrip<core::YamlSerializer>(dispatch);
	// A World save/load carries every catalogue fade through both encoders
	// and the reloaded station keeps fading identically.
	auto const s = processStation();
	core::World world("CatalogueWear", 2, 2); world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), "World catalogue wear setup failed");
	world.resumeSimulation();
	require(advanceSeconds(world, 600), "World catalogue wear drive failed");
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
		require(loaded.deserialize(*in, work), "World catalogue wear reload failed");
		require(loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot()
			&& loaded.getStationSystemsEvents() == world.getStationSystemsEvents(),
			"World save/load lost a catalogue wear state");
		require(advanceSeconds(loaded, 60) && advanceSeconds(world, 60)
			&& loaded.getStationSystemsSnapshot() == world.getStationSystemsSnapshot(),
			"the reloaded catalogue wear state diverged on subsequent drive");
	}
}

void wearCatalogueTransactions(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	std::string diagnostic;
	// Refused models, checked on every catalogue machine through the one
	// settings surface that carries them.
	for (auto const& bad : {
		core::InstallationWearSettings{0.1, 0.0, 0.0, 0.0, 1.0},
		core::InstallationWearSettings{0.0, -0.1, 0.0, 0.0, 1.0},
		core::InstallationWearSettings{0.0, 0.0, 0.0, 0.0, 0.0},
		core::InstallationWearSettings{0.0, 0.0, 0.0, 0.0, 1.5},
		core::InstallationWearSettings{0.0, std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 1.0}})
	{
		auto s = processStation();
		s.solarArrays[0].cellDegradation = bad;
		require(!core::validateStationSystemsSettings(s, diagnostic),
			"an invalid catalogue wear model was accepted");
	}
	{
		auto noMass = processStation();
		noMass.solarArrays[0].cellDegradation->fadePerThroughputKg = 0.1;
		require(!core::validateStationSystemsSettings(noMass, diagnostic)
			&& diagnostic.find("fadePerThroughputKg") != std::string::npos,
			"a solar array accepted a throughput rate it cannot meter");
		auto looped = processStation();
		looped.waterTreatments[0].membraneFouling->fadePerThroughputKg = 0.1;
		require(core::validateStationSystemsSettings(looped, diagnostic),
			"a water treatment was refused a throughput rate it does meter");
	}
	// Live topology: the opt-in presence is frozen, the rates are live.
	core::StationSystemsRuntime run(processStation(), 0.25f);
	require(run.advanceTo(600), "catalogue transaction setup failed");
	require(!run.updateNode<core::WaterTreatmentSettings>("filter",
		[](auto& t) { t.membraneFouling.reset(); }, &diagnostic),
		"a catalogue wear opt-in was removed live");
	// Adding an opt-in a machine never authored is the same topology
	// crossing, in the other direction, and is refused the same way.
	require(!run.updateNode<core::SolarArraySettings>("sun",
		[](auto& a) { a.cellDegradation.reset(); }, &diagnostic),
		"a catalogue wear opt-in was removed live");
	auto unwornSun = processStation();
	unwornSun.solarArrays[0].cellDegradation.reset();
	core::StationSystemsRuntime bare(unwornSun, 0.25f);
	require(bare.advanceTo(600), "bare-sun setup failed");
	require(!bare.updateNode<core::SolarArraySettings>("sun",
		[](auto& a) { a.cellDegradation = catalogueModel(); }, &diagnostic),
		"a catalogue wear opt-in was added live");
	require(bare.snapshot().power.sources[0].wear.wearFraction == 0.0,
		"a refused opt-in still left the machine worn");
	// A retune never reprices the fade already accumulated.
	require(run.advanceTo(3600), "catalogue first hour failed");
	auto const firstHour = allWear(run.snapshot())["filter"].wearFraction;
	require(firstHour > 0.0, "no fade banked in the first catalogue hour");
	require(run.updateNode<core::WaterTreatmentSettings>("filter",
		[](auto& t) { t.membraneFouling->fadePerOperatingHour = 0.2; }, &diagnostic),
		"catalogue wear rate retune refused");
	require(run.advanceTo(7200), "catalogue second hour failed");
	auto const retuned = allWear(run.snapshot())["filter"].wearFraction;
	require(retuned > firstHour && retuned < firstHour * 3.5,
		"a live catalogue retune repriced the fade that had already accumulated");
	// A poisoned rate fails the tick whole and rolls the fade back.
	core::StationSystemsRuntime tainted(processStation(), 0.25f);
	require(tainted.advanceTo(600), "catalogue poison setup failed");
	auto const clean = allWear(tainted.snapshot())["filter"].wearFraction;
	require(tainted.updateNode<core::WaterTreatmentSettings>("filter",
		[](auto& t) { t.membraneFouling->fadePerCalendarHour =
			std::numeric_limits<double>::infinity(); }, &diagnostic),
		"poisoned catalogue rate refused before the audit could catch it");
	require(!tainted.advanceTo(610) && !tainted.snapshot().healthy
		&& allWear(tainted.snapshot())["filter"].wearFraction == clean,
		"a poisoned catalogue rate published a partial fade instead of rolling back");
	// Schema: a catalogue block needs 121; a pre-121 document carrying
	// one is refused, and 121 round-trips through both encoders.
	auto const s = processStation();
	for (uint32_t version : {120u, 121u})
	{
		auto out = core::YamlSerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto in = core::YamlSerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored;
		bool refused = false;
		try { core::readStationSystemsSettingsMap(*in, version, restored); }
		catch (core::SerializationException const&) { refused = true; }
		require(refused == (version < 121), "catalogue wear settings schema guard failed");
		if (version >= 121) require(restored == s, "catalogue wear settings did not round-trip");
	}
	{
		auto out = core::BinarySerializer::toString();
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
		auto in = core::BinarySerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings restored;
		core::readStationSystemsSettingsMap(*in, 121, restored);
		require(restored == s, "catalogue wear settings did not round-trip through binary");
	}
	core::StationConfigurationDocument document; document.settings = s;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 121") != std::string::npos,
		"a catalogue wear opt-in did not bump the wire schema");
	yaml.replace(yaml.find("schemaVersion: 121"), std::string("schemaVersion: 121").size(),
		"schemaVersion: 120");
	core::StationConfigurationDocument restoredDoc;
	require(!core::parseStationConfiguration(yaml, restoredDoc, &diagnostic),
		"a pre-121 document silently accepted catalogue wear models");
}

void wearCatalogueOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// Feature absent: no fade anywhere, every effective rating equals the
	// authored rating, and the document keeps the pre-feature wire shape.
	auto absent = processStation();
	absent.solarArrays[0].cellDegradation.reset();
	absent.waterTreatments[0].membraneFouling.reset();
	absent.electrolyzers[0].membraneDegradation.reset();
	absent.co2Captures[0].absorbentDegradation.reset();
	absent.sabatierReactors[0].catalystDeactivation.reset();
	absent.methaneProcessors[0].catalystDeactivation.reset();
	core::StationSystemsRuntime unworn(absent, 0.25f);
	require(unworn.advanceTo(3600), "catalogue absent-opt-in drive failed");
	for (auto const& [id, w] : allWear(unworn.snapshot()))
	{
		require(w.modeId.empty() && w.wearFraction == 0.0 && w.operatingHours == 0.0
			&& w.throughputKg == 0.0 && w.calendarHours == 0.0 && w.derivedImpact == 0.0,
			"a catalogue machine without the opt-in accumulated wear");
	}
	// The unworn machines still do their jobs at their authored ratings.
	auto const& snap = unworn.snapshot();
	require(snap.waterTreatments[0].lastProcessedKg > 0.0
		&& snap.electrolyzers[0].lastSplitKg > 0.0
		&& snap.co2Captures[0].lastCapturedKg > 0.0
		&& snap.sabatierReactors[0].lastCo2ConsumedKg > 0.0
		&& snap.methaneProcessors[0].lastMethaneConsumedKg > 0.0,
		"the unworn catalogue machines stopped converting");
	// A feature-absent document stamps the pre-catalogue schema and
	// carries none of the nine field names, so existing fixtures stay
	// byte-identical.
	core::StationConfigurationDocument document; document.settings = absent;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 121") == std::string::npos,
		"a catalogue-wear-absent document bumped the schema");
	for (auto const* field : {"cellDegradation", "fouledInjector", "efficiencyDrift",
		"membraneDegradation", "membraneFouling", "absorbentDegradation", "stackDegradation",
		"catalystDeactivation"})
		require(yaml.find(field) == std::string::npos,
			"a catalogue machine carried its wear field on the wire without opting in");
	// An absent machine cannot be worn by any command surface.
	std::string diagnostic;
	require(!unworn.injectFault("filter", "membrane-fouling", {}, &diagnostic) && !diagnostic.empty(),
		"a catalogue wear fade was injected into a machine without the opt-in");
	// The dispatch plant, absent: the generator, engine and fuel cell keep
	// their authored ratings and never fade.
	auto plain = dispatchStation();
	plain.solarArrays[0].cellDegradation.reset();
	plain.generators[0].fouledInjector.reset();
	plain.heatEngines[0].efficiencyDrift.reset();
	plain.fuelCells[0].stackDegradation.reset();
	core::StationSystemsRuntime idle(plain, 0.25f);
	require(idle.advanceTo(3600), "catalogue dispatch absent-opt-in drive failed");
	for (auto const& [id, w] : allWear(idle.snapshot()))
		require(w.modeId.empty() && w.wearFraction == 0.0,
			"a dispatch machine without the opt-in accumulated wear");
}
}

void registerStationWearCatalogueChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"wearCatalogueFade", wearCatalogueFade});
	checks.push_back({"wearCataloguePermanence", wearCataloguePermanence});
	checks.push_back({"wearCatalogueReplay", wearCatalogueReplay});
	checks.push_back({"wearCatalogueTransactions", wearCatalogueTransactions});
	checks.push_back({"wearCatalogueOptIn", wearCatalogueOptIn});
}
