#include "StationRig.h"
#include "Smoke.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/BinarySerializer.h"
#include "core/StationConfigurationDocument.h"
#include "core/YamlSerializer.h"

// Heat engine tracer bullet checks (#804): an authored, Carnot-bounded
// heat-to-power crossing between two Thermal reservoirs. Every assertion is
// made on what a configured World publishes after being advanced - never on
// integrator internals - so replacing the conversion with any other scheme
// producing the same published behaviour still passes.
namespace
{
using namespace smoke::station;

// A reservoir pair with distinct non-round starting temperatures: the audit's
// thermal closure bound scales with the starting energy, so a zero-magnitude
// datum would leave the accumulated rounding no room at all. Neither
// reservoir carries a radiator or absorbed heat: the engine is the only
// thing moving heat between them, so every joule in these runs is the
// engine's own and the closure is exact.
core::ThermalReservoirSettings engineReservoir(std::string const& id,
	std::string const& thermalClass, double capacityKJPerK, double startingC)
{
	core::ThermalReservoirSettings r;
	r.id = id;
	r.thermalClass = thermalClass;
	r.heatCapacityKJPerK = capacityKJPerK;
	r.startingTemperatureC = startingC;
	r.minTemperatureC = -50.0;
	r.maxTemperatureC = 300.0;
	return r;
}

// The tracer fixture: a hot reservoir at 90 C, a rejection reservoir at
// 20 C, and one engine between them. At carnotFraction 0.5 the reference
// numbers from the spec apply: eta_C = 1 - 293.15/363.15 ~ 0.1926,
// eta ~ 0.0963. Gates are set so the fixture gradient is live: min-hot
// 30 C above max-sink 25 C. The battery is the acceptance the engine
// needs (#805): with no load and no charge headroom the acceptance-coupled
// throttle holds the engine at zero, so the standing fixture carries a
// bank with room to charge - the engine's power has somewhere to go.
// The command defaults to full rated: the fixture engines are unthrottled
// unless a test says otherwise.
core::StationSystemsSettings enginePair(double hotCapacity = 20000.0,
	double sinkCapacity = 20000.0, double ratedInputKW = 100.0,
	double carnotFraction = 0.5, double auxKW = 1.0)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.thermalReservoirs = {engineReservoir("hot", "warm", hotCapacity, 90.0),
		engineReservoir("reject", "warm", sinkCapacity, 20.0)};
	core::HeatEngineSettings engine;
	engine.id = "orc";
	engine.hotReservoirId = "hot";
	engine.rejectionReservoirId = "reject";
	engine.thermalClass = "warm";
	engine.ratedThermalInputKW = ratedInputKW;
	engine.commandedInputKW = ratedInputKW;
	engine.carnotFraction = carnotFraction;
	engine.auxiliaryPowerKW = auxKW;
	engine.minHotTemperatureC = 30.0;
	engine.maxSinkTemperatureC = 25.0;
	s.heatEngines = {engine};
	core::BatterySettings bank;
	bank.id = "buffer";
	bank.nameplateKWh = 1000.0;
	bank.usableKWh = 900.0;
	bank.startingStoredKWh = 100.0;
	bank.maxChargeKW = 500.0;
	bank.maxDischargeKW = 500.0;
	s.batteries = {bank};
	return s;
}

void configure(core::World& world, core::StationSystemsSettings const& settings,
	std::string& diagnostic)
{
	world.pauseSimulation(); world.finishBuild();
	if (!world.configureStationSystems(settings, false, &diagnostic))
		throw std::runtime_error("configure refused: [" + diagnostic + "]");
}

core::StationHeatEngineSnapshot engine(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.heatEngines)
		if (entry.id == id) return entry;
	throw std::runtime_error("heat engine missing from snapshot");
}

core::StationThermalReservoirSnapshot const& reservoir(
	core::StationSystemsSnapshot const& snap, std::string const& id)
{
	for (auto const& entry : snap.thermal.reservoirs)
		if (entry.id == id) return entry;
	throw std::runtime_error("reservoir missing from snapshot");
}

core::StationEquipmentLoadSnapshot loadSnap(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.power.loads)
		if (entry.id == id) return entry;
	throw std::runtime_error("load missing from snapshot");
}

// The tracer headline (#804): two reservoirs at unequal temperature plus one
// engine produce visible bus generation. The engine draws heat from the hot
// reservoir, delivers net kilowatts to the pool, and the published snapshot
// reports the intake, the net output, the rejection heat, the status with
// its named limiter, and actual efficiency beside Carnot efficiency.
void heatEngineTracerBullet(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("HeatEngineTracer", 2, 2);
	configure(world, enginePair(), diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	require(initial.heatEngines.size() == 1, "configured engine missing before the run");
	require(engine(initial, "orc").status == core::StationHeatEngineStatus::Idle
		&& engine(initial, "orc").thermalInputEnergyKJ == 0.0,
		"an engine reports activity before the World has run");
	world.resumeSimulation();
	require(advanceSeconds(world, 60.0), "tracer run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "tracer run failed its audit");
	auto const orc = engine(snapshot, "orc");
	// Visible bus generation: the engine's net output is in the pool total
	// and in its own published output, and the hot side genuinely cooled.
	require(orc.status == core::StationHeatEngineStatus::Active,
		"a live-gradient engine did not report Active");
	require(orc.lastElectricalOutputKW > 1.0,
		"the engine delivered no visible bus generation");
	require(snapshot.power.heatEngineOutputKW >= orc.lastElectricalOutputKW - 1e-9
		&& snapshot.power.poolGenerationKW >= orc.lastElectricalOutputKW - 1e-9,
		"the engine output is not visible in the pool generation total");
	require(orc.lastThermalInputKW > orc.lastElectricalOutputKW,
		"the thermal intake does not exceed the electrical output it was converted from");
	require(orc.lastRejectionKW > 0.0,
		"a converting engine booked no rejection heat");
	// The hot reservoir lost heat and the rejection reservoir gained it.
	require(reservoir(snapshot, "hot").temperatureC < 90.0 - 1e-6,
		"the hot reservoir did not cool under engine extraction");
	require(reservoir(snapshot, "reject").temperatureC > 20.0 + 1e-6,
		"the rejection reservoir did not warm under engine rejection");
	// Efficiency beside Carnot: actual is the declared fraction of Carnot,
	// and never exceeds it.
	require(orc.carnotEfficiencyNow > 0.15 && orc.carnotEfficiencyNow < 0.25,
		"the published Carnot efficiency is not in the expected range for 90/20 C");
	require(orc.efficiencyNow > 0.0 && orc.efficiencyNow <= orc.carnotEfficiencyNow + 1e-12,
		"the actual efficiency exceeded the Carnot efficiency at the interface temperatures");
	require(std::abs(orc.efficiencyNow - 0.5 * orc.carnotEfficiencyNow) < 1e-9,
		"the actual efficiency is not the declared carnotFraction of Carnot");
}

// Per-tick closure, exactly: the hot reservoir's loss equals the bus net
// gain plus the rejection reservoir's gain, every tick, and the engine's own
// ledger closes over input = electrical + rejection. No existing residual
// assertion is weakened: the reservoir residuals keep closing with the new
// terms in the formula.
void heatEngineClosure(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("HeatEngineClosure", 2, 2);
	configure(world, enginePair(20000.0, 30000.0, 80.0, 0.4, 2.0), diagnostic);
	world.resumeSimulation();
	double previousHotEnergy = 0.0, previousSinkEnergy = 0.0;
	bool first = true;
	for (int step = 0; step < 40; ++step)
	{
		require(advanceStationSteps(world, 1), "closure step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "closure run failed its audit");
		auto const& hot = reservoir(snapshot, "hot");
		auto const& sink = reservoir(snapshot, "reject");
		auto const orc = engine(snapshot, "orc");
		if (!first)
		{
			auto const hotLossKJ = previousHotEnergy - hot.currentEnergyKJ;
			auto const sinkGainKJ = sink.currentEnergyKJ - previousSinkEnergy;
			auto const busGainKJ = orc.lastElectricalOutputKW
				* snapshot.systemsTimestepSeconds;
			// Hot loss = bus net gain + rejection gain, exactly within the
			// representation tolerance of the energies involved.
			auto const bound = 1e-9 * std::max({1.0, std::abs(hotLossKJ),
				std::abs(sinkGainKJ), std::abs(busGainKJ)});
			require(std::abs(hotLossKJ - (busGainKJ + sinkGainKJ)) <= bound,
				"per-tick closure failed: hot loss != bus net gain + rejection gain");
			// The booked reservoir terms match the engine's own published
			// rates, so the two ledgers close on one number.
			require(std::abs(hot.lastEngineExtractionKW - orc.lastThermalInputKW) <= 1e-9,
				"the reservoir extraction term disagrees with the engine intake");
			require(std::abs(sink.lastEngineRejectionKW - orc.lastRejectionKW) <= 1e-9,
				"the reservoir rejection term disagrees with the engine rejection");
		}
		previousHotEnergy = hot.currentEnergyKJ;
		previousSinkEnergy = sink.currentEnergyKJ;
		first = false;
		// The engine's own ledger closes every tick.
		require(std::abs(orc.residualKJ) <= 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
			"the engine conversion ledger did not close over input = electrical + rejection");
		if (orc.status != core::StationHeatEngineStatus::Active) break;
	}
	// The reservoir residuals still close with the engine terms in the
	// formula - the pre-existing audit was not weakened to accommodate.
	auto const final = world.getStationSystemsSnapshot();
	require(std::abs(reservoir(final, "hot").residualKJ)
		<= 1e-6 * std::max(1.0, std::abs(reservoir(final, "hot").startingEnergyKJ)),
		"the hot reservoir residual did not close with engine extraction present");
	require(std::abs(reservoir(final, "reject").residualKJ)
		<= 1e-6 * std::max(1.0, std::abs(reservoir(final, "reject").startingEnergyKJ)),
		"the rejection reservoir residual did not close with engine rejection present");
}

// The second law at the conversion site: at any tick, the actual efficiency
// never exceeds the Carnot efficiency at the tick-start interface
// temperatures, while the gradient shrinks over the run.
void heatEngineCarnotBound(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("HeatEngineCarnot", 2, 2);
	// A small sink capacity so the rejection warms the sink fast and the
	// gradient visibly decays: the bound must hold on every tick of the decay.
	configure(world, enginePair(20000.0, 4000.0, 120.0, 1.0, 0.0), diagnostic);
	world.resumeSimulation();
	for (int step = 0; step < 120; ++step)
	{
		require(advanceStationSteps(world, 1), "carnot-bound step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "carnot-bound run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.efficiencyNow <= orc.carnotEfficiencyNow + 1e-12,
			"the actual efficiency exceeded Carnot at the interface temperatures");
		if (orc.status != core::StationHeatEngineStatus::Active) break;
	}
	// carnotFraction 1.0 means the engine sits exactly on the Carnot bound;
	// the run must still never exceed it, and the gradient genuinely decayed.
	auto const final = world.getStationSystemsSnapshot();
	auto const orc = engine(final, "orc");
	require(orc.carnotEfficiencyNow < 0.1926 + 1e-9,
		"the Carnot efficiency did not decay as the sink warmed");
	require(orc.efficiencyNow <= orc.carnotEfficiencyNow + 1e-12,
		"the efficiency bound failed at the end of the decaying run");
}

// The engine idles - no extraction, no output, nothing booked - when the
// gradient is dead (hot at or below sink) or the source is below the
// min-hot gate. A cooled station never shows phantom generation.
void heatEngineIdle(smoke::Context const&)
{
	std::string diagnostic;
	// Dead gradient: both reservoirs start at the same temperature.
	{
		auto settings = enginePair();
		settings.thermalReservoirs[0].startingTemperatureC = 20.0;
		core::World world("HeatEngineIdleFlat", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "flat-gradient run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "flat-gradient run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Idle
			&& orc.lastLimiter == "NoGradient",
			"an engine with no gradient did not idle with the NoGradient limiter");
		require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
			&& orc.lastRejectionKW == 0.0 && orc.thermalInputEnergyKJ == 0.0,
			"a dead-gradient engine booked activity anyway");
		require(snapshot.power.heatEngineOutputKW == 0.0,
			"a dead-gradient engine produced phantom bus generation");
	}
	// Below the min-hot gate: the gradient is live but the source is too cold.
	{
		auto settings = enginePair();
		settings.thermalReservoirs[0].startingTemperatureC = 25.0; // below minHot 30
		core::World world("HeatEngineIdleCold", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "too-cold run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "too-cold run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Idle
			&& orc.lastLimiter == "TooCold",
			"an engine below its min-hot gate did not idle with the TooCold limiter");
		require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0,
			"a too-cold engine drew heat or produced power");
	}
	// Zero rated input disables the engine entirely.
	{
		auto settings = enginePair(20000.0, 20000.0, 0.0, 0.5, 1.0);
		core::World world("HeatEngineDisabled", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "disabled run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "disabled run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Disabled
			&& orc.disabled && orc.lastLimiter == "Disabled",
			"a zero-rated engine did not report Disabled");
		require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0
			&& orc.rejectionEnergyKJ == 0.0,
			"a disabled engine moved energy");
	}
}

// No crossing at any rated input: the engine cannot end a tick with its hot
// reservoir below the rejection reservoir's tick-start temperature, because
// its extraction is capped by the heat the hot side holds above the sink.
// A rated input a hundred times the available gradient changes nothing but
// the limiter.
void heatEngineNoCrossing(smoke::Context const&)
{
	std::string diagnostic;
	// A deliberately absurd rating against a thin gradient: the hot side may
	// never be driven below the sink's tick-start temperature by the engine.
	auto settings = enginePair(2000.0, 40000.0, 5000.0, 0.5, 0.0);
	settings.thermalReservoirs[0].startingTemperatureC = 21.0;
	settings.thermalReservoirs[1].startingTemperatureC = 20.0;
	core::World world("HeatEngineNoCrossing", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	for (int step = 0; step < 60; ++step)
	{
		auto const before = world.getStationSystemsSnapshot();
		auto const sinkStartC = reservoir(before, "reject").temperatureC;
		require(advanceStationSteps(world, 1), "no-crossing step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "no-crossing run failed its audit");
		// The engine's own extraction cannot have carried the hot node below
		// the sink's tick-start temperature: the extraction is bounded by
		// C_hot x (T_h - T_c), so T_hot_end >= T_c_start from the engine's
		// contribution alone.
		auto const& hot = reservoir(snapshot, "hot");
		auto const extractionKJ = hot.lastEngineExtractionKW * snapshot.systemsTimestepSeconds;
		auto const headroomKJ = hot.heatCapacityKJPerK
			* (reservoir(before, "hot").temperatureC - sinkStartC);
		require(extractionKJ <= headroomKJ + 1e-6 * std::max(1.0, headroomKJ),
			"engine extraction exceeded the heat the hot reservoir held above the sink");
		if (hot.temperatureC <= sinkStartC + 1e-9)
		{
			// If the hot side has reached the sink's level, the engine must
			// be throttled to nothing: no extraction across the boundary.
			require(hot.lastEngineExtractionKW <= 1e-9,
				"the engine kept extracting at or past the no-crossing boundary");
		}
	}
}

// The sink gate trips: rejection into a reservoir at or above the maximum
// sink temperature trips the engine whole - no intake, no output - and the
// trip clears on its own when the sink falls back below the gate.
void heatEngineSinkTrip(smoke::Context const&)
{
	std::string diagnostic;
	// Start the rejection reservoir above the max-sink gate: the engine is
	// tripped from the first tick despite a live gradient.
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 1.0);
	settings.thermalReservoirs[1].startingTemperatureC = 26.0; // >= maxSink 25
	core::World world("HeatEngineTrip", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 10.0), "trip run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "trip run failed its audit");
	auto const orc = engine(snapshot, "orc");
	require(orc.status == core::StationHeatEngineStatus::Tripped && orc.tripped
		&& orc.lastLimiter == "SinkOver",
		"rejection into an over-gate sink did not trip the engine with SinkOver");
	require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0,
		"a tripped engine drew heat or produced power");
	// Clearing: a fresh World with the gate raised above the sink (and still
	// below min-hot, so the gates stay a legal window) - the trip is a
	// reported state, not a latch, and the engine runs again.
	auto cleared = settings;
	cleared.heatEngines[0].maxSinkTemperatureC = 28.0;
	core::World clearedWorld("HeatEngineTripClear", 2, 2);
	configure(clearedWorld, cleared, diagnostic);
	clearedWorld.resumeSimulation();
	require(advanceSeconds(clearedWorld, 10.0), "trip-clear run failed");
	auto const after = clearedWorld.getStationSystemsSnapshot();
	require(after.healthy, "trip-clear run failed its audit");
	require(engine(after, "orc").status == core::StationHeatEngineStatus::Active,
		"the engine did not resume once the sink fell below the gate");
}

// Every validation refusal names the offending entry, and no refusal
// mutates the World: the configuration stands exactly as it was.
void heatEngineValidation(smoke::Context const&)
{
	std::string diagnostic;
	core::StationSystemsSettings good;
	good.enabled = true;
	good.thermalReservoirs = {engineReservoir("hot", "warm", 20000.0, 90.0),
		engineReservoir("reject", "warm", 20000.0, 20.0),
		engineReservoir("other", "low", 20000.0, 20.0)};
	core::HeatEngineSettings base;
	base.id = "orc";
	base.hotReservoirId = "hot";
	base.rejectionReservoirId = "reject";
	base.thermalClass = "warm";
	base.ratedThermalInputKW = 100.0;
	base.commandedInputKW = 100.0;
	base.carnotFraction = 0.5;
	base.auxiliaryPowerKW = 1.0;
	base.minHotTemperatureC = 30.0;
	base.maxSinkTemperatureC = 25.0;
	good.heatEngines = {base};
	require(core::validateStationSystemsSettings(good, diagnostic), diagnostic.c_str());

	auto const refuseNamed = [&](core::HeatEngineSettings const& bad,
		std::vector<std::string> const& needles, char const* what)
	{
		auto probe = good;
		probe.heatEngines = {bad};
		require(!core::validateStationSystemsSettings(probe, diagnostic)
			&& std::all_of(needles.begin(), needles.end(), [&](std::string const& needle)
				{ return diagnostic.find(needle) != std::string::npos; }),
			what);
	};
	// Endpoints that do not resolve to Thermal reservoirs.
	auto bad = base; bad.hotReservoirId = "nowhere";
	refuseNamed(bad, {"orc", "nowhere"}, "an unresolved hot endpoint was not refused by name");
	bad = base; bad.rejectionReservoirId = "ghost";
	refuseNamed(bad, {"orc", "ghost"}, "an unresolved rejection endpoint was not refused by name");
	// Hot and rejection resolving to the same reservoir: a single-temperature
	// engine is a perpetual-motion machine.
	bad = base; bad.rejectionReservoirId = "hot";
	refuseNamed(bad, {"orc", "hot", "same reservoir"},
		"a same-reservoir engine was not refused by name");
	// thermalClass mismatch against the rejection reservoir.
	bad = base; bad.thermalClass = "low";
	refuseNamed(bad, {"orc", "low", "reject"},
		"a thermal class mismatch against the rejection reservoir was not refused by name");
	// carnotFraction outside (0, 1]: the second law is an authoring error.
	bad = base; bad.carnotFraction = 0.0;
	refuseNamed(bad, {"orc", "carnotFraction"},
		"a zero carnotFraction was not refused by name");
	bad = base; bad.carnotFraction = 1.0 + 1e-9;
	refuseNamed(bad, {"orc", "carnotFraction"},
		"an over-unity carnotFraction was not refused by name");
	bad = base; bad.carnotFraction = std::numeric_limits<double>::quiet_NaN();
	refuseNamed(bad, {"orc", "carnotFraction"},
		"a non-finite carnotFraction was not refused by name");
	// Negative or non-finite values.
	bad = base; bad.ratedThermalInputKW = -1.0;
	refuseNamed(bad, {"orc", "rated thermal input"},
		"a negative rated thermal input was not refused by name");
	bad = base; bad.auxiliaryPowerKW = -std::numeric_limits<double>::infinity();
	refuseNamed(bad, {"orc", "auxiliary power"},
		"a non-finite auxiliary power was not refused by name");
	// A dead gradient window: min-hot at or below max-sink.
	bad = base; bad.minHotTemperatureC = 25.0; // == maxSink
	refuseNamed(bad, {"orc", "positive-gradient window"},
		"a dead gradient window (min-hot == max-sink) was not refused by name");
	bad = base; bad.minHotTemperatureC = 10.0; // below maxSink
	refuseNamed(bad, {"orc", "positive-gradient window"},
		"a dead gradient window (min-hot below max-sink) was not refused by name");
	// The commanded intake dispatch must sit in [0, rated] (#805): a
	// negative command is nonsense and a command above rated asks for
	// intake the machine was never built to draw. Both are refused by name.
	bad = base; bad.commandedInputKW = -1.0;
	refuseNamed(bad, {"orc", "commanded input"},
		"a negative commanded input was not refused by name");
	bad = base; bad.commandedInputKW = 100.0 + 1e-9; // above rated
	refuseNamed(bad, {"orc", "commanded input"},
		"a commanded input above rated was not refused by name");
	bad = base; bad.commandedInputKW = std::numeric_limits<double>::quiet_NaN();
	refuseNamed(bad, {"orc", "commanded input"},
		"a non-finite commanded input was not refused by name");
	// The declared ceiling, in the spirit of the existing component ceilings.
	auto many = good;
	many.heatEngines.clear();
	for (size_t i = 0; i < core::MaxStationHeatEngines; ++i)
	{
		auto entry = base;
		entry.id = "orc" + std::to_string(i);
		many.heatEngines.push_back(entry);
	}
	require(core::validateStationSystemsSettings(many, diagnostic), diagnostic.c_str());
	many.heatEngines.push_back([&]{ auto e = base; e.id = "over-limit"; return e; }());
	require(!core::validateStationSystemsSettings(many, diagnostic)
		&& diagnostic.find("limited") != std::string::npos,
		"the heat engine count ceiling was not enforced");
	// No refusal mutates the World.
	core::World world("HeatEngineValidationWorld", 2, 2);
	configure(world, good, diagnostic);
	auto const baseline = world.getStationSystemsSnapshot();
	std::vector<core::HeatEngineSettings> refusals;
	{
		auto e = base; e.hotReservoirId = "nowhere"; refusals.push_back(e);
		e = base; e.rejectionReservoirId = "hot"; refusals.push_back(e);
		e = base; e.thermalClass = "low"; refusals.push_back(e);
		e = base; e.carnotFraction = 2.0; refusals.push_back(e);
		e = base; e.minHotTemperatureC = 20.0; refusals.push_back(e);
		e = base; e.commandedInputKW = 150.0; refusals.push_back(e);
		e = base; e.commandedInputKW = -0.5; refusals.push_back(e);
	}
	for (auto const& refusal : refusals)
	{
		auto mutated = good;
		mutated.heatEngines = {refusal};
		require(!world.configureStationSystems(mutated, false, &diagnostic)
			&& world.getStationSystemsSnapshot() == baseline
			&& world.getStationSystemsSettings() == good,
			"a refused heat engine configuration mutated the World");
	}
}

// Settings round-trip through documents and the cumulative counters clear
// on reset: the engine's ledger is runtime state reconstructed from the
// authored values, exactly like every other machine.
void heatEnginePersistence(smoke::Context const&)
{
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.thermalReservoirs = {engineReservoir("hot", "warm", 18000.0, 95.5),
		engineReservoir("reject", "warm", 26000.0, 17.25)};
	core::HeatEngineSettings engineSettings;
	engineSettings.id = "orc-a";
	engineSettings.hotReservoirId = "hot";
	engineSettings.rejectionReservoirId = "reject";
	engineSettings.thermalClass = "warm";
	engineSettings.ratedThermalInputKW = 137.5;
	// A throttled dispatch (#805): the command below rated must survive
	// the round trip, and the wire carries it precisely because it is not
	// the default.
	engineSettings.commandedInputKW = 100.0;
	engineSettings.carnotFraction = 0.375;
	engineSettings.auxiliaryPowerKW = 2.25;
	engineSettings.minHotTemperatureC = 41.0;
	engineSettings.maxSinkTemperatureC = 33.5;
	settings.heatEngines = {engineSettings};
	// The run needs acceptance: a charge-headroom battery keeps the
	// acceptance-coupled throttle open so the persistence run actually
	// converts (#805).
	core::BatterySettings bank;
	bank.id = "buffer";
	bank.nameplateKWh = 500.0;
	bank.usableKWh = 450.0;
	bank.startingStoredKWh = 50.0;
	bank.maxChargeKW = 300.0;
	bank.maxDischargeKW = 300.0;
	settings.batteries = {bank};
	// The Station configuration document round-trips the exact settings.
	core::StationConfigurationDocument document;
	document.settings = settings;
	auto const yaml = core::serializeStationConfiguration(document);
	core::StationConfigurationDocument loaded;
	std::string diagnostic;
	require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
	require(loaded.settings == settings,
		"heat engine Station configuration changed on round trip");
	// Unknown-field discipline: a field the loader does not know is refused
	// naming the entry and the field, and the loaded settings are untouched.
	for (auto const* field : {"carnotFraction", "hotReservoirId", "maxSinkTemperatureC",
		"commandedInputKW"})
	{
		auto bad = yaml;
		auto const at = bad.find(std::string(field) + ":");
		require(at != std::string::npos, "missing heat engine wire field");
		bad.insert(at, "unknown_");
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& diagnostic.find("orc-a") != std::string::npos
			&& diagnostic.find(std::string("unknown_") + field) != std::string::npos
			&& diagnostic.find("conversion required") != std::string::npos
			&& loaded.settings == settings,
			"unknown heat engine field was ignored or mutated the document");
	}
	// Version discipline: a pre-109 document carrying engines is refused as
	// conversion-required rather than silently dropping them, a version
	// beyond the accepted range is refused outright, and a pre-110 document
	// carrying the commanded-input field is refused rather than silently
	// dropping the throttle the author commanded (#805).
	for (int version : {108, 111})
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 110"), std::string("schemaVersion: 110").size(),
			"schemaVersion: " + std::to_string(version));
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& loaded.settings == settings, "unsupported heat engine schema was accepted");
	}
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 110"), std::string("schemaVersion: 110").size(),
			"schemaVersion: 109");
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& diagnostic.find("commanded input") != std::string::npos
			&& diagnostic.find("conversion required") != std::string::npos
			&& loaded.settings == settings,
			"a pre-110 document carrying the commanded-input field was accepted");
	}
	// The default: a document without the commanded-input field loads the
	// engine at full rated - the throttle is what travels, not the default.
	{
		auto stripped = yaml;
		auto const at = stripped.find("commandedInputKW:");
		require(at != std::string::npos, "throttled command missing from the wire");
		// Erase the whole line, indentation included, so the next field
		// keeps its own column rather than inheriting the deleted one.
		auto const lineStart = stripped.rfind('\n', at);
		auto const eraseFrom = lineStart == std::string::npos ? 0 : lineStart + 1;
		auto const end = stripped.find('\n', at);
		stripped.erase(eraseFrom, end == std::string::npos
			? std::string::npos : end + 1 - eraseFrom);
		core::StationConfigurationDocument defaulted;
		require(core::parseStationConfiguration(stripped, defaulted, &diagnostic),
			diagnostic.c_str());
		require(defaulted.settings.heatEngines.size() == 1
			&& defaulted.settings.heatEngines[0].commandedInputKW
				== defaulted.settings.heatEngines[0].ratedThermalInputKW,
			"an absent commanded-input field did not default to full rated");
	}
	// Both World persistence paths: the reloaded World carries the exact
	// authored engines and reproduces the original run exactly.
	for (bool binary : {false, true})
	{
		core::World original("HeatEnginePersistence", 2, 2);
		std::string note;
		configure(original, settings, note);
		auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work;
		original.serialize(*writer, work);
		writer->serialize();
		auto const text = binary
			? static_cast<core::BinarySerializer&>(*writer).getSerializedString()
			: static_cast<core::YamlSerializer&>(*writer).getSerializedString();
		core::World restored("HeatEnginePersistenceRestored", 2, 2);
		restored.pauseSimulation(); restored.finishBuild();
		auto reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(text))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(text));
		reader->deserialize();
		restored.deserialize(*reader, work);
		require(restored.getStationSystemsSettings() == settings,
			"the restored World lost or changed the authored heat engines");
		// Run both to the same elapsed time and compare the engine picture.
		original.resumeSimulation(); restored.resumeSimulation();
		require(advanceSeconds(original, 30.0) && advanceSeconds(restored, 30.0),
			"persistence run failed");
		require(engine(restored.getStationSystemsSnapshot(), "orc-a").thermalInputEnergyKJ
				== engine(original.getStationSystemsSnapshot(), "orc-a").thermalInputEnergyKJ
			&& engine(restored.getStationSystemsSnapshot(), "orc-a").electricalEnergyKJ
				== engine(original.getStationSystemsSnapshot(), "orc-a").electricalEnergyKJ
			&& engine(restored.getStationSystemsSnapshot(), "orc-a").rejectionEnergyKJ
				== engine(original.getStationSystemsSnapshot(), "orc-a").rejectionEnergyKJ,
			"the round-tripped engine did not reproduce the original run");
	}
}

// Cumulative input/output/rejection energy clears on reset, and the rerun
// reproduces the first run exactly.
void heatEngineResetIsolation(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("HeatEngineReset", 2, 2);
	configure(world, enginePair(), diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 60.0), "reset-isolation run failed");
	auto const run = world.getStationSystemsSnapshot();
	require(engine(run, "orc").thermalInputEnergyKJ > 1.0
		&& engine(run, "orc").electricalEnergyKJ > 1.0
		&& engine(run, "orc").rejectionEnergyKJ > 1.0,
		"reset fixture converted nothing to clear");
	world.pauseSimulation();
	world.resetSimulation();
	auto const reset = world.getStationSystemsSnapshot();
	require(reset.thermal.reservoirs[0].temperatureC == 90.0
		&& reset.thermal.reservoirs[1].temperatureC == 20.0,
		"reset did not restore the authored starting temperatures");
	require(engine(reset, "orc").thermalInputEnergyKJ == 0.0
		&& engine(reset, "orc").electricalEnergyKJ == 0.0
		&& engine(reset, "orc").rejectionEnergyKJ == 0.0
		&& engine(reset, "orc").lastThermalInputKW == 0.0
		&& engine(reset, "orc").lastElectricalOutputKW == 0.0,
		"reset did not clear the engine's cumulative counters");
	// And the rerun reproduces the original run exactly.
	world.resumeSimulation();
	require(advanceSeconds(world, 60.0), "reset-isolation rerun failed");
	require(world.getStationSystemsSnapshot() == run,
		"the rerun after reset is not identical to the first run");
}

// The engine coexists with the rest of the thermal plant: a generator
// dumping waste heat into the same hot reservoir feeds the engine, and both
// ledgers close. The engine's extraction is booked alongside the
// generator's rejection into the shared reservoir without either side
// double-counting.
void heatEnginePlantCoexistence(smoke::Context const&)
{
	std::string diagnostic;
	auto settings = enginePair(40000.0, 40000.0, 150.0, 0.5, 1.0);
	// A generator sharing the hot reservoir as its waste sink, and a
	// radiator on the rejection reservoir so the plant reaches a live
	// steady state rather than running to the gates.
	core::ResourceStoreSettings fuel;
	fuel.id = "diesel"; fuel.unit = "kWh"; fuel.capacity = 100000.0;
	fuel.startingQualified = 80000.0;
	settings.stores = {fuel};
	core::GeneratorSettings gen;
	gen.id = "genset"; gen.fuelStoreId = "diesel"; gen.thermalClass = "warm";
	gen.heatSinkId = "hot"; gen.ratedOutputKW = 50.0; gen.efficiency = 0.4;
	gen.maxRampKWPerSecond = 100.0; gen.commandedOutputKW = 50.0;
	gen.maxSinkTemperatureC = 200.0;
	settings.generators = {gen};
	core::RadiatorSettings rad;
	rad.id = "sink-rad"; rad.reservoirId = "reject"; rad.areaM2 = 30.0;
	rad.emissivity = 0.9; rad.viewFactor = 1.0; rad.sinkTemperatureC = -20.0;
	settings.radiators = {rad};
	core::World world("HeatEngineCoexistence", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 60.0), "coexistence run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "coexistence run failed its audit");
	auto const orc = engine(snapshot, "orc");
	require(orc.status == core::StationHeatEngineStatus::Active,
		"the engine did not run alongside the generator feeding its hot side");
	// Both the generator heat and the engine extraction are booked on the
	// hot reservoir, and the reservoir residual closes over both.
	auto const& hot = reservoir(snapshot, "hot");
	require(hot.lastGeneratorHeatKW > 1.0 && hot.lastEngineExtractionKW > 1.0,
		"the shared hot reservoir did not see both the generator heat and the engine extraction");
	require(std::abs(hot.residualKJ) <= 1e-6 * std::max(1.0, std::abs(hot.startingEnergyKJ)),
		"the shared hot reservoir residual did not close with generator and engine both booked");
	// The engine's net output and the generator's output both ride the pool.
	require(snapshot.power.heatEngineOutputKW > 0.0
		&& snapshot.power.generatorOutputKW > 1.0,
		"both sources did not show up as pool generation");
}

// The acceptance-coupled throttle (#805): the engine converts only the
// heat whose power the station actually accepts. With zero demand and zero
// charge headroom the extraction is zero and the limiter reads
// DemandAcceptance; as acceptance rises the intake rises with it, up to
// the dispatch ceiling.
void heatEngineDemandAcceptance(smoke::Context const&)
{
	std::string diagnostic;
	// Zero acceptance: no loads, and the one battery is full, so there
	// is neither demand nor charge headroom for the engine's power.
	{
		auto settings = enginePair();
		auto& bank = settings.batteries[0];
		bank.startingStoredKWh = bank.nameplateKWh; // no charge headroom
		core::World world("HeatEngineNoAcceptance", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "zero-acceptance run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "zero-acceptance run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Idle
			&& orc.lastLimiter == "DemandAcceptance",
			"an engine with no demand and no charge headroom did not idle with the DemandAcceptance limiter");
		require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
			&& orc.lastRejectionKW == 0.0 && orc.thermalInputEnergyKJ == 0.0
			&& orc.electricalEnergyKJ == 0.0 && orc.rejectionEnergyKJ == 0.0,
			"an unaccepted engine booked extraction, output or rejection anyway");
		require(reservoir(snapshot, "hot").temperatureC == 90.0,
			"the hot reservoir drained for power nobody accepted");
		require(snapshot.power.heatEngineOutputKW == 0.0,
			"an unaccepted engine produced bus generation");
	}
	// Contrast: the same station with charge headroom admits the engine's
	// power and the hot reservoir drains.
	{
		core::World world("HeatEngineChargeHeadroom", 2, 2);
		configure(world, enginePair(), diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "charge-headroom run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "charge-headroom run failed its audit");
		require(engine(snapshot, "orc").status == core::StationHeatEngineStatus::Active,
			"charge headroom did not admit the engine's power");
		require(reservoir(snapshot, "hot").temperatureC < 90.0 - 1e-6,
			"the hot reservoir did not drain once acceptance existed");
	}
	// No battery and no load at all: the same zero acceptance, the same
	// idle - the engine never over-extracts against an empty station.
	{
		auto settings = enginePair();
		settings.batteries.clear();
		core::World world("HeatEngineNoPool", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "no-pool run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "no-pool run failed its audit");
		require(engine(snapshot, "orc").lastLimiter == "DemandAcceptance"
			&& engine(snapshot, "orc").thermalInputEnergyKJ == 0.0,
			"an engine with no pool at all over-extracted against zero acceptance");
	}
}

// A commanded input below rated caps the intake accordingly: the same
// station throttled to a quarter of rated draws a quarter of the heat and
// leaves measurably more of it in the hot reservoir over the run, with
// the throttle reason named in the snapshot.
void heatEngineThrottleCommandedInput(smoke::Context const&)
{
	std::string diagnostic;
	auto full = enginePair();
	auto throttled = enginePair();
	throttled.heatEngines[0].commandedInputKW = 25.0; // rated 100
	core::World fullWorld("HeatEngineFullCommand", 2, 2);
	core::World throttledWorld("HeatEngineThrottled", 2, 2);
	configure(fullWorld, full, diagnostic);
	configure(throttledWorld, throttled, diagnostic);
	fullWorld.resumeSimulation();
	throttledWorld.resumeSimulation();
	require(advanceSeconds(fullWorld, 60.0) && advanceSeconds(throttledWorld, 60.0),
		"throttle comparison run failed");
	auto const fullSnap = fullWorld.getStationSystemsSnapshot();
	auto const throttledSnap = throttledWorld.getStationSystemsSnapshot();
	require(fullSnap.healthy && throttledSnap.healthy, "throttle run failed its audit");
	auto const fullEngine = engine(fullSnap, "orc");
	auto const throttledEngine = engine(throttledSnap, "orc");
	require(fullEngine.status == core::StationHeatEngineStatus::Active
		&& throttledEngine.status == core::StationHeatEngineStatus::Active,
		"neither the full- nor the throttled-command engine ran");
	require(std::abs(throttledEngine.lastThermalInputKW - 25.0) < 1e-9,
		"the throttled engine did not cap its intake at the commanded input");
	require(throttledEngine.lastLimiter == "CommandedInput",
		"the throttled engine did not name the commanded-input throttle in the snapshot");
	require(fullEngine.lastThermalInputKW > 90.0
		&& fullEngine.lastLimiter != "CommandedInput",
		"the full-command engine did not draw near its rated intake");
	// Measurably more heat left in the throttled run's hot reservoir:
	// the quarter-rated command drew a quarter of the intake, so three
	// quarters of the drain the full command made stays behind.
	auto const fullDrainK = 90.0 - reservoir(fullSnap, "hot").temperatureC;
	auto const throttledDrainK = 90.0 - reservoir(throttledSnap, "hot").temperatureC;
	require(throttledDrainK > 0.0 && fullDrainK > 3.0 * throttledDrainK,
		"throttling down did not leave measurably more heat in the hot reservoir");
	require(throttledEngine.thermalInputEnergyKJ * 3.0
		< fullEngine.thermalInputEnergyKJ + 1e-9,
		"the throttled engine drew the same cumulative heat as the full-command engine");
	require(throttledEngine.electricalEnergyKJ
		< fullEngine.electricalEnergyKJ - 1.0,
		"the throttled engine delivered the same cumulative power as the full-command engine");
}

// Aux-gated running under the throttle: an engine whose gross output
// cannot cover its own auxiliaries at the available gradient idles - it
// never draws net power from the bus to feed a machine that cannot pay
// for itself.
void heatEngineAuxiliaryIdle(smoke::Context const&)
{
	std::string diagnostic;
	// A thin gradient: at 24 C over 20 C with carnotFraction 0.5 the
	// gross output is well under a tenth of the auxiliaries, so the
	// engine must not run at all.
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 5.0);
	settings.thermalReservoirs[0].startingTemperatureC = 24.0;
	settings.thermalReservoirs[1].startingTemperatureC = 20.0;
	settings.heatEngines[0].minHotTemperatureC = 24.0;
	settings.heatEngines[0].maxSinkTemperatureC = 22.0;
	core::World world("HeatEngineAuxIdle", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 30.0), "aux-idle run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "aux-idle run failed its audit");
	auto const orc = engine(snapshot, "orc");
	require(orc.status == core::StationHeatEngineStatus::Idle
		&& orc.lastLimiter == "AuxiliaryPower",
		"an engine whose gross cannot cover its auxiliaries did not idle with AuxiliaryPower");
	require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0
		&& orc.rejectionEnergyKJ == 0.0,
		"an aux-idle engine moved energy anyway");
	require(snapshot.power.heatEngineOutputKW == 0.0,
		"an aux-idle engine showed bus output");
	// The bus and the bank are untouched: the engine never becomes a load.
	require(snapshot.power.batteries[0].storedKWh == 100.0,
		"the battery moved energy for an engine that cannot pay for itself");
	require(reservoir(snapshot, "hot").temperatureC == 24.0,
		"the hot reservoir changed for an aux-idle engine");
}

// Ledger closure stays exact under throttling: hot loss = bus net gain +
// rejection gain, every tick, with the command holding the intake at a
// fraction of rated.
void heatEngineClosureUnderThrottle(smoke::Context const&)
{
	std::string diagnostic;
	auto settings = enginePair(20000.0, 30000.0, 80.0, 0.4, 2.0);
	settings.heatEngines[0].commandedInputKW = 40.0; // half of rated
	core::World world("HeatEngineThrottleClosure", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	double previousHotEnergy = 0.0, previousSinkEnergy = 0.0;
	bool first = true, sawActive = false;
	for (int step = 0; step < 40; ++step)
	{
		require(advanceStationSteps(world, 1), "throttle-closure step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "throttle-closure run failed its audit");
		auto const& hot = reservoir(snapshot, "hot");
		auto const& sink = reservoir(snapshot, "reject");
		auto const orc = engine(snapshot, "orc");
		require(orc.lastThermalInputKW <= 40.0 + 1e-9,
			"the throttled engine drew more than its commanded input");
		if (!first)
		{
			auto const hotLossKJ = previousHotEnergy - hot.currentEnergyKJ;
			auto const sinkGainKJ = sink.currentEnergyKJ - previousSinkEnergy;
			auto const busGainKJ = orc.lastElectricalOutputKW
				* snapshot.systemsTimestepSeconds;
			auto const bound = 1e-9 * std::max({1.0, std::abs(hotLossKJ),
				std::abs(sinkGainKJ), std::abs(busGainKJ)});
			require(std::abs(hotLossKJ - (busGainKJ + sinkGainKJ)) <= bound,
				"per-tick closure failed under throttling: hot loss != bus net gain + rejection gain");
			require(std::abs(hot.lastEngineExtractionKW - orc.lastThermalInputKW) <= 1e-9,
				"the reservoir extraction term disagrees with the throttled engine intake");
			require(std::abs(sink.lastEngineRejectionKW - orc.lastRejectionKW) <= 1e-9,
				"the reservoir rejection term disagrees with the throttled engine rejection");
		}
		previousHotEnergy = hot.currentEnergyKJ;
		previousSinkEnergy = sink.currentEnergyKJ;
		first = false;
		require(std::abs(orc.residualKJ) <= 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
			"the throttled engine conversion ledger did not close over input = electrical + rejection");
		if (orc.status == core::StationHeatEngineStatus::Active) sawActive = true;
	}
	require(sawActive, "the throttled closure fixture never ran the engine");
}

// Throttle interactions with protected-service admission (#805): the
// protected floor is part of the acceptance signal, so a protected load
// never starves because the engine's throttle exists - and the engine
// never over-extracts beyond the acceptance it was given.
void heatEngineProtectedService(smoke::Context const&)
{
	std::string diagnostic;
	core::EquipmentLoadSettings lifeSupport;
	lifeSupport.id = "life-support";
	lifeSupport.electricalDemandKW = 20.0;
	lifeSupport.criticalFloorKW = 5.0;
	lifeSupport.thermalClass = "warm";
	// The engine is the station's only source and there is no charge
	// headroom: acceptance is pure demand. If the acceptance signal
	// missed the protected floor, the engine would idle and the floor
	// would starve; instead the engine runs for it.
	{
		auto settings = enginePair(40000.0, 40000.0, 100.0, 0.5, 1.0);
		settings.batteries.clear();
		settings.equipmentLoads = {lifeSupport};
		core::World world("HeatEngineProtectedOnly", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "protected-only run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "protected-only run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Active,
			"the engine idled while a protected floor needed its power");
		// The protected floor is served from the engine's admitted power:
		// protected admission draws from the pool ahead of every
		// discretionary watt, and the acceptance signal carried the
		// floor's demand so the engine ran for it.
		auto const service = loadSnap(snapshot, "life-support");
		require(service.unmetCriticalKW <= 1e-9 && service.admitted
			&& service.suppliedKW >= 5.0 - 1e-9,
			"a protected floor went unmet with the engine running for it");
		require(snapshot.power.protectedUnmetKW <= 1e-9 && !snapshot.power.protectedUnmet,
			"the station raised a protected-unmet alarm with the floor served");
		// Never over-extract against unmet acceptance: the engine's net
		// output cannot exceed the demand signal it was admitted against.
		require(orc.lastElectricalOutputKW <= snapshot.power.dispatchDemandKW + 1e-9,
			"the engine over-extracted beyond the acceptance it was given");
	}
	// Solar alone covers the protected floor and the engine is throttled
	// fully closed: the protected service is admitted from the other
	// source unchanged - the throttle cuts only what would have gone
	// elsewhere, never the protected admission.
	{
		auto covered = enginePair(40000.0, 40000.0, 100.0, 0.5, 1.0);
		covered.batteries.clear();
		core::SolarArraySettings sun;
		sun.id = "sun";
		sun.capacityKW = 8.0; // constant full illumination by default
		covered.solarArrays = {sun};
		covered.equipmentLoads = {lifeSupport};
		covered.heatEngines[0].commandedInputKW = 0.0; // throttle closed
		core::World world("HeatEngineProtectedSolar", 2, 2);
		configure(world, covered, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 30.0), "protected-solar run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "protected-solar run failed its audit");
		require(snapshot.power.protectedUnmetKW <= 1e-9 && !snapshot.power.protectedUnmet
			&& loadSnap(snapshot, "life-support").unmetCriticalKW <= 1e-9,
			"the engine throttle starved a protected floor the other source could carry");
		require(engine(snapshot, "orc").lastThermalInputKW == 0.0
			&& engine(snapshot, "orc").thermalInputEnergyKJ == 0.0,
			"a fully throttled engine still drew heat");
		// The hot reservoir is untouched: every joule the closed engine
		// could not be asked to convert stays exactly where it was.
		require(reservoir(snapshot, "hot").temperatureC == 90.0,
			"the closed engine still touched the hot reservoir");
	}
}
}   // namespace

void registerStationHeatEngineChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"heatEngineTracerBullet", heatEngineTracerBullet});
	checks.push_back({"heatEngineClosure", heatEngineClosure});
	checks.push_back({"heatEngineCarnotBound", heatEngineCarnotBound});
	checks.push_back({"heatEngineIdle", heatEngineIdle});
	checks.push_back({"heatEngineNoCrossing", heatEngineNoCrossing});
	checks.push_back({"heatEngineSinkTrip", heatEngineSinkTrip});
	checks.push_back({"heatEngineValidation", heatEngineValidation});
	checks.push_back({"heatEnginePersistence", heatEnginePersistence});
	checks.push_back({"heatEngineResetIsolation", heatEngineResetIsolation});
	checks.push_back({"heatEnginePlantCoexistence", heatEnginePlantCoexistence});
	checks.push_back({"heatEngineDemandAcceptance", heatEngineDemandAcceptance});
	checks.push_back({"heatEngineThrottleCommandedInput", heatEngineThrottleCommandedInput});
	checks.push_back({"heatEngineAuxiliaryIdle", heatEngineAuxiliaryIdle});
	checks.push_back({"heatEngineClosureUnderThrottle", heatEngineClosureUnderThrottle});
	checks.push_back({"heatEngineProtectedService", heatEngineProtectedService});
}
