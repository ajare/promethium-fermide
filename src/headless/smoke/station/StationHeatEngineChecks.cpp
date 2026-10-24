#include "StationRig.h"
#include "Smoke.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "core/BinarySerializer.h"
#include "core/StationConnections.h"
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

// ---------------------------------------------------------------------------
// Second-law ledger (#806)
//
// The ticket's bound: heat drawn from a finite hot body of heat capacity C,
// cooling it from T_h to T_f against a sink at T_c (all temperatures
// ABSOLUTE), can have yielded at most
//
//     exergy = C * [(T_h - T_f) - T_c * ln(T_h / T_f)]
//
// of work. The engine books its work at the Carnot efficiency of the
// TICK-START hot temperature, so over one tick it delivers
//
//     w = f * (1 - T_c / T_h) * q          with q = C * (T_h - T_f)
//
// against an exergy of ex = q - C * T_c * ln(T_h / T_f). Substituting
// x = (T_h - T_f) / T_h, the per-tick difference is exactly
//
//     ex - w / f = C * T_c * [-ln(1 - x) - x]
//
// which is the left-endpoint quadrature error of the exergy integral. Summed
// over a run this is the tolerance the tick cadence earns: it is O(x) per
// tick and shrinks as the timestep shrinks. It is DERIVED from the cadence,
// not fitted to a passing result - the ledger reports the window's share of
// the work so the tightness of the bound is auditable rather than assumed,
// and a genuine second-law violation shows up as work above exergy + window.
//
// Folding the identity over the whole run gives the exact closure this suite
// asserts:  electrical + auxiliary = carnotFraction * (exergy + window).
// At carnotFraction 1.0 the engine sits exactly on the second-law ceiling;
// below it the ledger closes with the shortfall the irreversibility costs.
// ---------------------------------------------------------------------------
struct SecondLawLedger
{
	double exergyKJ{0.0};     // exergy of the heat actually extracted
	double windowKJ{0.0};     // tick-cadence quadrature error of that exergy
	double electricalKJ{0.0}; // net work delivered to the bus
	double auxiliaryKJ{0.0};  // auxiliaries the running machine burned
	double extractedKJ{0.0};  // heat drawn from the hot reservoir
	double closedFormKJ{0.0}; // the ticket's formula over the whole cooling
	double peakWindowShare{0.0}; // window / work: how tight the bound really is
	int convertingTicks{0};

	// Fold one Station step into the ledger. The temperatures are ABSOLUTE and
	// taken from the published reservoir snapshots either side of the tick;
	// the rates are the engine's own published figures for that tick. Only the
	// engine moves heat in these fixtures, so the reservoirs' temperature
	// change IS the engine's extraction and the ledger attributes cleanly.
	void fold(double capacityKJPerK, double hotBeforeK, double hotAfterK,
		double sinkBeforeK, double inputKW, double outputKW, double auxKW,
		double dtSeconds)
	{
		extractedKJ += inputKW * dtSeconds;
		electricalKJ += outputKW * dtSeconds;
		if (outputKW > 0.0) auxiliaryKJ += auxKW * dtSeconds;
		auto const dropK = hotBeforeK - hotAfterK;
		if (dropK <= 0.0 || hotBeforeK <= 0.0) return;
		require(hotAfterK > 0.0, "the hot reservoir cooled below absolute zero");
		++convertingTicks;
		exergyKJ += capacityKJPerK
			* ((hotBeforeK - hotAfterK) - sinkBeforeK * std::log(hotBeforeK / hotAfterK));
		auto const x = dropK / hotBeforeK;
		require(x > 0.0 && x < 1.0,
			"the hot reservoir's per-tick drop left the exergy integral's domain");
		windowKJ += capacityKJPerK * sinkBeforeK * (-std::log(1.0 - x) - x);
		if (electricalKJ > 0.0)
			peakWindowShare = std::max(peakWindowShare, windowKJ / electricalKJ);
	}
};

// A sweep fixture: one engine between two reservoirs at the given interface
// temperatures. The gates sit strictly INSIDE the declared gradient window
// (max-sink below min-hot, the ticket's positive-gradient-window rule), the
// rejection reservoir is sized so large it stays effectively isothermal - so
// the ticket's closed form against a fixed sink applies - and the battery
// carries the acceptance the engine needs (#805) with headroom that never
// binds, so the sweep measures conversion rather than admission.
core::StationSystemsSettings sweepFixture(double hotC, double sinkC,
	double carnotFraction, double hotCapacityKJPerK, double ratedInputKW,
	double auxKW, uint32_t stepRatio,
	std::pair<double, double> gates = {std::nan(""), std::nan("")})
{
	auto s = enginePair(hotCapacityKJPerK, 50000000.0, ratedInputKW, carnotFraction, auxKW);
	s.systemsStepWorldTicks = stepRatio;
	s.thermalReservoirs[0].startingTemperatureC = hotC;
	s.thermalReservoirs[1].startingTemperatureC = sinkC;
	// Gates default to inside the gradient, a quarter and three quarters of
	// the way up, so a sweep case stays live across its whole span. A case
	// that wants to drain all the way to the sink passes its own gates.
	if (std::isnan(gates.first))
	{
		if (hotC > sinkC)
		{
			auto const span = hotC - sinkC;
			s.heatEngines[0].maxSinkTemperatureC = sinkC + span * 0.25;
			s.heatEngines[0].minHotTemperatureC = sinkC + span * 0.75;
		}
	}
	else
	{
		s.heatEngines[0].maxSinkTemperatureC = gates.first;
		s.heatEngines[0].minHotTemperatureC = gates.second;
	}
	// The battery's charge headroom must never be the binding constraint.
	s.batteries[0].maxChargeKW = std::max(500.0, ratedInputKW * 8.0);
	return s;
}

// Run one sweep case until the engine stops converting (the gradient it came
// to spend is spent) or the step cap is reached, accumulating the ledger and
// checking the per-tick second-law properties as it goes.
struct SweepResult
{
	SecondLawLedger ledger;
	bool reachedEquilibrium{false};
	int steps{0};
};

// minSteps is the number of Station steps the case must actually convert
// for, so a deliberately thin case - one whose whole gradient collapses
// in a single tick - can still be run for its no-crossing property
// without tripping the "this fixture never engaged" guard.
SweepResult runSweepCase(core::World& world, double hotCapacityKJPerK,
	int maxSteps, int minSteps)
{
	SweepResult result;
	auto const start = world.getStationSystemsSnapshot();
	auto const hotStartK = reservoir(start, "hot").temperatureC + 273.15;
	auto const sinkStartK = reservoir(start, "reject").temperatureC + 273.15;
	for (int step = 0; step < maxSteps; ++step)
	{
		auto const before = world.getStationSystemsSnapshot();
		require(advanceStationSteps(world, 1), "sweep case step failed");
		auto const after = world.getStationSystemsSnapshot();
		require(after.healthy, "sweep case failed its audit");
		++result.steps;
		auto const orc = engine(after, "orc");
		auto const& hotBefore = reservoir(before, "hot");
		auto const& hotAfter = reservoir(after, "hot");
		auto const& sinkBefore = reservoir(before, "reject");
		// No-crossing, every tick: the engine's own extraction cannot carry
		// the hot node below the sink's tick-start temperature.
		auto const headroomKJ = hotBefore.heatCapacityKJPerK
			* std::max(0.0, hotBefore.temperatureC - sinkBefore.temperatureC);
		auto const extractionKJ = hotAfter.lastEngineExtractionKW
			* after.systemsTimestepSeconds;
		require(extractionKJ <= headroomKJ + 1e-6 * std::max(1.0, headroomKJ),
			"sweep case: engine extraction exceeded the heat held above the sink");
		// The second law at the conversion site, every tick.
		require(orc.efficiencyNow <= orc.carnotEfficiencyNow + 1e-12,
			"sweep case: actual efficiency exceeded Carnot at the interface temperatures");
		require(orc.lastElectricalOutputKW >= 0.0 && orc.lastThermalInputKW >= 0.0
			&& orc.lastRejectionKW >= 0.0,
			"sweep case: the engine booked a negative rate");
		result.ledger.fold(hotCapacityKJPerK,
			hotBefore.temperatureC + 273.15, hotAfter.temperatureC + 273.15,
			sinkBefore.temperatureC + 273.15,
			orc.lastThermalInputKW, orc.lastElectricalOutputKW, orc.lastAuxKW,
			after.systemsTimestepSeconds);
		if (orc.status != core::StationHeatEngineStatus::Active)
		{
			result.reachedEquilibrium = true;
			break;
		}
	}
	require(result.steps >= minSteps,
		"sweep case never engaged the engine enough to test what it claims");
	// The ticket's closed form over the whole cooling, against the sink the
	// run started with (the coldest it ever was, so the honest fixed-sink
	// reading of the bound).
	auto const final = world.getStationSystemsSnapshot();
	auto const hotEndK = reservoir(final, "hot").temperatureC + 273.15;
	result.ledger.closedFormKJ = hotCapacityKJPerK * ((hotStartK - hotEndK)
		- sinkStartK * std::log(hotStartK / hotEndK));
	return result;
}

// The run-to-equilibrium bound (#806): drain a small hot body against a sink
// so large it stays isothermal, at carnotFraction 1.0 - the worst case for
// the bound, because the engine is then allowed every joule of exergy the
// gradient holds. Over the whole run to equilibrium the cumulative electrical
// energy plus the auxiliaries burned never exceeds the exergy of the heat
// actually extracted, and the only slack is the tick-cadence quadrature
// window - which is small, and reported as such.
void heatEngineExergyRunToEquilibrium(smoke::Context const&)
{
	constexpr double hotCapacity = 200.0;
	constexpr double carnotFraction = 1.0;
	std::string diagnostic;
	auto settings = sweepFixture(90.0, 20.0, carnotFraction, hotCapacity, 100.0, 0.5, 6,
		{20.4, 20.5});
	core::World world("HeatEngineExergyEquilibrium", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	auto const run = runSweepCase(world, hotCapacity, 4000, 100);
	require(run.reachedEquilibrium, "the equilibrium run never drained the gradient");
	auto const& ledger = run.ledger;
	require(ledger.convertingTicks > 100,
		"the equilibrium run converted too few ticks to be a run to equilibrium");
	auto const work = ledger.electricalKJ + ledger.auxiliaryKJ;
	require(work > 1.0, "the equilibrium run delivered no work to bound");
	// The second-law bound: work never exceeds the exergy of the heat drawn,
	// with the tick-cadence quadrature window as the only allowance.
	auto const bound = ledger.exergyKJ + ledger.windowKJ;
	require(work <= bound + 1e-9 * std::max(1.0, bound),
		"cumulative electrical energy exceeded the exergy of the extracted heat");
	// The ledger closes on the identity, not approximately: at carnotFraction
	// 1.0 the engine sits exactly on the ceiling, so the closure is tight to
	// floating point and any over-extraction is caught, not absorbed.
	auto const closure = std::abs(work - carnotFraction * (ledger.exergyKJ + ledger.windowKJ));
	require(closure <= 1e-9 * std::max(1.0, work),
		"the second-law ledger did not close on electrical + auxiliary = f * (exergy + window)");
	// The ticket's closed form, evaluated over the whole cooling against the
	// run-start sink, bounds the work with the same window allowance.
	require(work <= ledger.closedFormKJ + ledger.windowKJ
		+ 1e-9 * std::max(1.0, ledger.closedFormKJ),
		"the work exceeded the ticket's closed-form exergy of the cooling");
	// Tightness evidence: the window is a fraction of a percent of the work,
	// so the bound is tight enough that a real violation - not a rounding
	// artefact - is what would trip it.
	require(ledger.peakWindowShare < 1e-2,
		"the tick-cadence window grew large enough to mask a violation");
	// And the hot side genuinely drained toward the sink: the bound was paid
	// for, not vacuously satisfied by a run that barely moved.
	auto const snapshot = world.getStationSystemsSnapshot();
	require(reservoir(snapshot, "hot").temperatureC < 25.0,
		"the equilibrium run did not drain the hot reservoir toward the sink");
}

// The tracer headline (#804): two reservoirs at unequal temperature plus one
// engine produce visible bus generation. The engine draws heat from the hot
// reservoir, delivers net kilowatts to the pool, and the published snapshot
// reports the intake, the net output, the rejection heat, the status with
// its named limiter, and actual efficiency beside Carnot efficiency.
void heatEngineLiveSettings()
{
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 0.0);
	settings.systemsStepWorldTicks = 60;
	float const baseStep = 1.0f / 60.0f;
	double const second = settings.systemsStepWorldTicks * static_cast<double>(baseStep);
	core::StationSystemsRuntime runtime(settings, baseStep);
	std::string diagnostic;
	settings.heatEngines[0].commandedInputKW = 0.0;
	require(runtime.updateSettings(settings, &diagnostic) && diagnostic.empty(),
		"live zero command refused");
	require(runtime.advanceTo(second), "live zero command step failed");
	auto const stopped = runtime.snapshot();
	require(stopped.healthy && engine(stopped, "orc").commandedInputKW == 0.0
		&& engine(stopped, "orc").lastThermalInputKW == 0.0,
		"accepted live zero command did not stop runtime conversion");

	// Resume at a changed rating, then close each gate independently. None
	// of these edits may reset the engine or its reservoir energy ledgers.
	settings.heatEngines[0].ratedThermalInputKW = 40.0;
	settings.heatEngines[0].commandedInputKW = 40.0;
	require(runtime.updateSettings(settings, &diagnostic), "live rating edit refused");
	require(runtime.advanceTo(2 * second), "live rating step failed");
	auto const active = runtime.snapshot();
	auto const running = engine(active, "orc");
	require(active.healthy && running.ratedThermalInputKW == 40.0
		&& running.commandedInputKW == 40.0
		&& std::abs(running.lastThermalInputKW - 40.0) < 1e-9,
		"live rating did not reach actual allocation");
	auto setHotGate = [&](double gate) {
		return runtime.updateNode<core::HeatEngineSettings>("Heat engine", "orc",
			[&](auto& e) { e.minHotTemperatureC = gate; }, &diagnostic);
	};
	require(setHotGate(100.0), "targeted hot gate edit refused");
	require(runtime.advanceTo(3 * second), "hot gate step failed");
	auto const gated = runtime.snapshot();
	require(gated.healthy && engine(gated, "orc").lastThermalInputKW == 0.0
		&& engine(gated, "orc").minHotTemperatureC == 100.0
		&& engine(gated, "orc").thermalInputEnergyKJ == running.thermalInputEnergyKJ
		&& reservoir(gated, "hot").temperatureC == reservoir(active, "hot").temperatureC,
		"live hot gate failed to stop conversion or preserved state changed");
	require(setHotGate(30.0), "targeted hot gate restoration refused");
	require(runtime.updateNode<core::HeatEngineSettings>("orc",
		[](auto& e) { e.maxSinkTemperatureC = 19.0; }, &diagnostic),
		"targeted sink gate edit refused");
	require(runtime.advanceTo(4 * second), "sink gate step failed");
	auto const tripped = runtime.snapshot();
	require(tripped.healthy && engine(tripped, "orc").tripped
		&& engine(tripped, "orc").lastThermalInputKW == 0.0,
		"live sink gate did not trip conversion");
	require(runtime.updateNode<core::HeatEngineSettings>("orc", [](auto& e) {
		e.maxSinkTemperatureC = 25.0;
		e.carnotFraction = 0.25;
		e.auxiliaryPowerKW = 0.5;
	}, &diagnostic), "targeted engine tuning refused");
	require(runtime.advanceTo(5 * second), "restored engine step failed");
	auto const restored = runtime.snapshot();
	auto const resumed = engine(restored, "orc");
	require(restored.healthy && resumed.lastThermalInputKW > 0.0
		&& resumed.thermalInputEnergyKJ > running.thermalInputEnergyKJ
		&& resumed.carnotFraction == 0.25 && resumed.auxiliaryPowerKW == 0.5
		&& std::abs(resumed.efficiencyNow - 0.25 * resumed.carnotEfficiencyNow) < 1e-9,
		"targeted tuning failed or accumulated state was reset");

	auto const before = runtime.effectiveSettings();
	for (int change = 0; change < 5; ++change)
	{
		auto bad = before;
		// A mixed edit must be refused before unrelated parameters land.
		bad.batteries[0].maxChargeKW = 1.0;
		if (change == 0) bad.heatEngines.clear();
		if (change == 1) { auto added = bad.heatEngines[0]; added.id = "extra"; bad.heatEngines.push_back(added); }
		if (change == 2) bad.heatEngines[0].id = "renamed";
		if (change == 3) bad.heatEngines[0].hotReservoirId = "reject";
		if (change == 4) bad.heatEngines[0].rejectionReservoirId = "hot";
		require(!runtime.updateSettings(bad, &diagnostic) && !diagnostic.empty(),
			"engine topology change accepted");
		require(runtime.effectiveSettings() == before && runtime.snapshot() == restored,
			"refused engine topology edit changed runtime state");
	}
	for (int change = 0; change < 3; ++change)
	{
		require(!runtime.updateNode<core::HeatEngineSettings>("orc", [&](auto& e) {
			e.commandedInputKW = 0.0;
			if (change == 0) e.id = "renamed";
			if (change == 1) e.hotReservoirId = "reject";
			if (change == 2) e.rejectionReservoirId = "hot";
		}, &diagnostic) && !diagnostic.empty(), "targeted engine rewiring accepted");
		require(runtime.effectiveSettings() == before && runtime.snapshot() == restored,
			"refused targeted engine edit changed runtime state");
	}
	require(runtime.updateNode<core::HeatEngineSettings>("orc",
		[](auto& e) { e.commandedInputKW = 0.0; }, &diagnostic),
		"targeted zero command refused");
	require(runtime.advanceTo(6 * second), "targeted zero step failed");
	auto const final = runtime.snapshot();
	require(final.healthy && engine(final, "orc").lastThermalInputKW == 0.0
		&& engine(final, "orc").commandedInputKW
			== runtime.effectiveSettings().heatEngines[0].commandedInputKW
		&& engine(final, "orc").thermalInputEnergyKJ == resumed.thermalInputEnergyKJ,
		"targeted zero command diverged from effective settings or reset totals");

	// Offered declaration order must not change the resolved runtime indices
	// or make a later targeted write hit another engine's settings mirror.
	auto pair = settings;
	auto other = pair.heatEngines[0];
	other.id = "other";
	other.commandedInputKW = 0.0;
	pair.heatEngines.push_back(other);
	core::StationSystemsRuntime reordered(pair, baseStep);
	std::reverse(pair.heatEngines.begin(), pair.heatEngines.end());
	pair.heatEngines[1].commandedInputKW = 10.0;
	require(reordered.updateSettings(pair, &diagnostic), "reordered engines refused");
	require(reordered.updateNode<core::HeatEngineSettings>("other",
		[](auto& e) { e.commandedInputKW = 5.0; }, &diagnostic),
		"targeted edit after engine reorder refused");
	require(reordered.advanceTo(second), "reordered engine step failed");
	auto const ordered = reordered.snapshot();
	require(ordered.healthy && engine(ordered, "orc").lastThermalInputKW == 10.0
		&& engine(ordered, "other").lastThermalInputKW == 5.0
		&& reordered.effectiveSettings().heatEngines[0].id == "orc"
		&& reordered.effectiveSettings().heatEngines[0].commandedInputKW == 10.0
		&& reordered.effectiveSettings().heatEngines[1].id == "other"
		&& reordered.effectiveSettings().heatEngines[1].commandedInputKW == 5.0,
		"engine reorder diverged from runtime indices or targeted mirror");
}

void heatEngineTracerBullet(smoke::Context const&)
{
	heatEngineLiveSettings();
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
	// #826: exercise shared-source allocation at the public runtime seam.
	// The shared floor is the warmest eligible sink; declaration order keeps
	// its existing priority, but cannot enlarge the aggregate heat budget.
	for (double secondSinkC : {20.0, 24.0})
	for (double firstCommandKW : {1000000.0, 35.0})
	for (bool reversed : {false, true})
	{
		auto shared = enginePair(1.0, 20000.0, 1000000.0, 0.5, 0.0);
		shared.systemsStepWorldTicks = 60;
		shared.thermalReservoirs[0].minTemperatureC = -200.0;
		shared.thermalReservoirs.push_back(
			engineReservoir("secondSink", "warm", 20000.0, secondSinkC));
		auto second = shared.heatEngines.front();
		second.id = "second";
		second.rejectionReservoirId = secondSinkC == 20.0 ? "reject" : "secondSink";
		shared.heatEngines.front().commandedInputKW = firstCommandKW;
		shared.heatEngines.push_back(second);
		if (reversed) std::reverse(shared.heatEngines.begin(), shared.heatEngines.end());
		std::string error;
		require(core::validateStationSystemsSettings(shared, error), error.c_str());
		auto const dt = 60.0 * static_cast<double>(core::World::getFixedTimestep());
		core::StationSystemsRuntime runtime(shared, core::World::getFixedTimestep());
		require(runtime.advanceTo(dt), "shared-source runtime step failed");
		auto const snap = runtime.snapshot();
		require(snap.healthy, "shared-source ledgers failed their unchanged audit");
		auto const& hot = reservoir(snap, "hot");
		auto const budgetKJ = 90.0 - secondSinkC;
		auto const extractedKJ = hot.lastEngineExtractionKW * dt;
		require(extractedKJ <= budgetKJ + 1e-6,
			"shared engines double-spent the source heat budget");
		require(std::abs(extractedKJ - budgetKJ) <= 1e-6
			&& std::abs(hot.temperatureC - secondSinkC) <= 1e-6,
			"shared allocation did not stop at the warmest sink");
		double intakeKJ = 0.0, outputKJ = 0.0, rejectionKJ = 0.0;
		for (auto const& e : snap.heatEngines)
		{
			intakeKJ += e.thermalInputEnergyKJ;
			outputKJ += e.electricalEnergyKJ;
			rejectionKJ += e.rejectionEnergyKJ;
		}
		require(std::abs(intakeKJ - extractedKJ) <= 1e-6
			&& std::abs(intakeKJ - outputKJ - rejectionKJ) <= 1e-6,
			"shared-source reservoir and engine ledgers disagree");
		auto const firstId = reversed ? "second" : "orc";
		auto const expectedFirstKJ = reversed ? budgetKJ
			: std::min(firstCommandKW * dt, budgetKJ);
		require(std::abs(engine(snap, firstId).thermalInputEnergyKJ
			- expectedFirstKJ) <= 1e-6, "shared allocation lost declaration priority");
	}
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
	for (int version : {108, 112})
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 111"), std::string("schemaVersion: 111").size(),
			"schemaVersion: " + std::to_string(version));
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& loaded.settings == settings, "unsupported heat engine schema was accepted");
	}
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 111"), std::string("schemaVersion: 111").size(),
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
// #829: battery acceptance is bus-side input through the actual Charge
// connection, not unconstrained storage headroom. Exercise real allocation
// through the public runtime, including lossy paths and saturation.
void heatEngineChargingPathAcceptance()
{
	for (auto const efficiency : {1.0, 0.5, 0.0})
	for (auto const bankCount : {1, 3})
	for (bool full : {false, true})
	{
		auto settings = enginePair(20000, 20000, 100, 0.5, 0.0);
		settings.systemsStepWorldTicks = 60;
		auto const bank = settings.batteries.front();
		for (int i = 1; i < bankCount; ++i)
		{
			auto extra = bank;
			extra.id = "buffer-" + std::to_string(i);
			// One nearly full bank and one full bank must not contribute
			// their nameplate charge ratings to the acceptance budget.
			extra.startingStoredKWh = extra.nameplateKWh - (i == 1 ? 1e-6 : 0.0);
			extra.chargeEfficiency = 0.8;
			settings.batteries.push_back(extra);
		}
		if (full)
			for (auto& b : settings.batteries) b.startingStoredKWh = b.nameplateKWh;
		for (auto const& link : core::station_connections::build(settings).links)
			if (link.designKey.ownerType == "Battery" && link.designKey.relation == "Charge")
			{
				auto& p = settings.connections[link.designKey];
				p.maximumThroughput = 0.01;
				p.efficiency = efficiency;
			}
		std::string diagnostic;
		require(core::validateStationSystemsSettings(settings, diagnostic), diagnostic.c_str());
		auto const quantum = core::World::getFixedTimestep();
		auto const seconds = 60.0 * static_cast<double>(quantum);
		auto const hours = seconds / 3600.0;
		core::StationSystemsRuntime runtime(settings, quantum);
		auto before = runtime.snapshot();
		auto const close = [](double a, double b) {
			require(std::abs(a - b) <= 2e-7 * std::max({1.0, std::abs(a), std::abs(b)}),
				"charging-path acceptance energy or rate mismatch");
		};
		for (int step = 1; step <= 3; ++step)
		{
			double expectedBusKW = 0.0;
			for (size_t i = 0; i < settings.batteries.size(); ++i)
			{
				auto const& b = settings.batteries[i];
				auto const wanted = std::min(b.maxChargeKW,
					std::max(0.0, b.nameplateKWh - before.power.batteries[i].storedKWh)
					/ hours / b.chargeEfficiency);
				// Zero-efficiency active requests consume input as transport
				// loss, exactly as for other physical Station connections.
				expectedBusKW += std::min(0.01, efficiency > 0 ? wanted / efficiency : wanted);
			}
			require(runtime.advanceTo(step * seconds), "charging-path step failed");
			auto const snap = runtime.snapshot();
			require(snap.healthy, "charging-path allocation failed its conservation audit");
			auto const e = engine(snap, "orc");
			close(e.lastElectricalOutputKW, expectedBusKW);
			require(e.lastLimiter == "DemandAcceptance", "charging path did not limit engine acceptance");
			double cableInputKJ = 0.0;
			for (auto const& c : snap.connections)
				if (c.key.ownerType == "Battery" && c.key.relation == "Charge")
				{
					close(c.inputTotal, c.deliveredTotal + c.lostTotal);
					close(c.lastDeliveredRate, c.lastInputRate * efficiency);
					require(c.lastInputRate <= 0.01 + 1e-12, "battery bypassed charge cable rating");
					cableInputKJ += c.inputTotal;
				}
			close(e.electricalEnergyKJ, cableInputKJ);
			for (size_t i = 0; i < settings.batteries.size(); ++i)
			{
				auto const& b = settings.batteries[i];
				close(snap.power.batteries[i].storedKWh - b.startingStoredKWh,
					snap.power.batteries[i].chargeInputKWh * b.chargeEfficiency);
			}
			close(e.thermalInputEnergyKJ, e.rejectionEnergyKJ + e.electricalEnergyKJ);
			close(reservoir(snap, "hot").temperatureC, 90.0 - e.thermalInputEnergyKJ / 20000.0);
			close(reservoir(snap, "reject").temperatureC, 20.0 + e.rejectionEnergyKJ / 20000.0);
			close(e.residualKJ, 0.0);
			before = snap;
		}
	}
}

void heatEngineDemandAcceptance(smoke::Context const&)
{
	heatEngineChargingPathAcceptance();
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
// #830: inspect every committed interval, not just a settled snapshot.
void heatEngineCurrentDemand(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 0.0);
	settings.systemsStepWorldTicks = 60;
	settings.batteries.clear();
	auto const second = 60 * static_cast<double>(core::World::getFixedTimestep());
	core::EquipmentLoadSettings load;
	load.id = "protected";
	load.thermalClass = "warm";
	load.electricalDemandKW = load.criticalFloorKW = 5.0;
	load.operatingProfile.periodSeconds = 4 * second;
	load.operatingProfile.segments = {{0.0, 1.0}, {2 * second, 0.0}};
	settings.equipmentLoads = {load};
	core::StationSystemsRuntime runtime(settings, core::World::getFixedTimestep());
	for (int step = 1; step <= 8; ++step)
	{
		require(runtime.advanceTo(step * second), "current-demand step failed");
		auto const snap = runtime.snapshot();
		require(snap.healthy, "current-demand audit failed");
		auto const expected = ((step - 1) % 4 < 2) ? 5.0 : 0.0;
		require(std::abs(loadSnap(snap, "protected").suppliedKW - expected) < 1e-9,
			"current protected demand starved on its first active interval");
		require(std::abs(engine(snap, "orc").lastElectricalOutputKW - expected) < 1e-9,
			"engine extracted heat after demand disappeared");
	}

	// Quarter-second forcing edges lie inside coarse steps; ratio 7 also
	// straddles the whole-second edges. All cadences reach the same 7 s and
	// 14 s committed endpoints. Check every interval against its analytical
	// demand/generation overlap, not merely the final steady-state output.
	for (bool competingSolar : {false, true})
	for (uint32_t ratio : {1u, 6u, 7u, 30u, 60u})
	{
		auto scenario = settings;
		scenario.systemsStepWorldTicks = ratio;
		auto const quantum = static_cast<double>(core::World::getFixedTimestep());
		auto& profile = scenario.equipmentLoads[0].operatingProfile;
		profile.segments = {{0.0, 0.0}, {15 * quantum, 1.0},
			{75 * quantum, 0.5}, {135 * quantum, 0.0}};
		if (competingSolar)
		{
			core::SolarArraySettings solar;
			solar.id = "sun";
			solar.capacityKW = 8.0;
			solar.illumination.periodSeconds = 240 * quantum;
			solar.illumination.segments = {{0.0, 1.0}, {45 * quantum, 0.25},
				{105 * quantum, 0.0}, {180 * quantum, 1.0}};
			scenario.solarArrays = {solar};
		}
		core::StationSystemsRuntime run(scenario, core::World::getFixedTimestep());
		double expectedCumulativeKJ = 0.0;
		double previousWorkKJ = 0.0;
		for (uint32_t tick = ratio; tick <= 840; tick += ratio)
		{
			double demandKJ = 0.0, workKJ = 0.0;
			for (auto t = tick - ratio; t < tick; ++t)
			{
				auto const phase = t % 240;
				auto const demand = phase < 15 ? 0.0 : phase < 75 ? 5.0 : phase < 135 ? 2.5 : 0.0;
				auto const solar = !competingSolar ? 0.0
					: phase < 45 ? 8.0 : phase < 105 ? 2.0 : phase < 180 ? 0.0 : 8.0;
				demandKJ += demand * quantum;
				workKJ += std::max(0.0, demand - solar) * quantum;
			}
			expectedCumulativeKJ += workKJ;
			if (!run.advanceTo(tick * quantum))
				throw std::runtime_error("profile-demand step failed at ratio " + std::to_string(ratio)
					+ " tick " + std::to_string(tick) + ": "
					+ (run.diagnostics().empty() ? "no diagnostic" : run.diagnostics().back().message));
			auto const snap = run.snapshot();
			auto const orc = engine(snap, "orc");
			require(snap.healthy && snap.systemsStepWorldTicks == ratio,
				"profile-demand cadence audit or clock failed");
			require(snap.power.protectedUnmetKW <= 1e-9
				&& loadSnap(snap, "protected").unmetCriticalKW <= 1e-9,
				"profile transition left protected demand unmet");
			require(std::abs(loadSnap(snap, "protected").suppliedKW
				* ratio * quantum - demandKJ) < 1e-8,
				"profile interval did not supply its integrated demand");
			require(std::abs(orc.electricalEnergyKJ - previousWorkKJ - workKJ) < 1e-8,
				"engine used stale demand or competing generation within an interval");
			require(std::abs(orc.electricalEnergyKJ - expectedCumulativeKJ) < 1e-8,
				"cadences disagree on cumulative demand-accepted work");
			if (workKJ == 0.0)
				require(orc.lastThermalInputKW == 0.0 && orc.lastRejectionKW == 0.0,
					"engine moved heat without current demand acceptance");
			require(std::abs(orc.residualKJ) <= 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
				"current-demand conversion ledger did not close");
			previousWorkKJ = orc.electricalEnergyKJ;
		}
	}
}

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
// The gradient sweep (#806): the second-law bound, the Carnot ceiling, and
// the no-crossing property are checked across interface temperature pairs
// that span near-zero, small, and large gradients rather than only at the
// reference fixture's 70 K. Near-zero gradients are where a bound that was
// fitted rather than derived would break: the work is tiny, the quadrature
// window is comparatively large, and any slip in the exergy accounting
// shows up as a large relative overshoot.
//
// The heavy variant sweeps every pair; the smoke variant takes the three
// corners so the fast lane still sees the shape of the surface.
struct SweepCase
{
	char const* name;
	double hotC;
	double sinkC;
	double carnotFraction;
	double hotCapacityKJPerK;
	double ratedInputKW;
	double auxKW;
	int maxSteps;
	int minSteps;
};

void runGradientCase(SweepCase const& c, uint32_t stepRatio)
{
	std::string diagnostic;
	auto settings = sweepFixture(c.hotC, c.sinkC, c.carnotFraction,
		c.hotCapacityKJPerK, c.ratedInputKW, c.auxKW, stepRatio);
	core::World world("HeatEngineGradientSweep", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	auto const run = runSweepCase(world, c.hotCapacityKJPerK, c.maxSteps, c.minSteps);
	auto const& ledger = run.ledger;
	require(ledger.electricalKJ > 0.0, "sweep case produced no electrical energy");
	auto const work = ledger.electricalKJ + ledger.auxiliaryKJ;
	auto const bound = ledger.exergyKJ + ledger.windowKJ;
	require(work <= bound + 1e-9 * std::max(1.0, bound),
		"sweep case: cumulative work exceeded the exergy of the extracted heat");
	require(work <= ledger.closedFormKJ + ledger.windowKJ
		+ 1e-9 * std::max(1.0, ledger.closedFormKJ),
		"sweep case: work exceeded the closed-form exergy of the cooling");
	// The window must stay a small share of the work at every gradient, or
	// the bound has stopped saying anything.
	require(ledger.peakWindowShare < 5e-2,
		"sweep case: the tick-cadence window grew large enough to mask a violation");
}

// The near-zero corner is its own case: the gradient is so thin that the
// engine converts only a few joules, and the quadrature window is a
// visible share of the work. The bound still holds, and the window's share
// is reported so the tightness at this corner is auditable.
void runNearZeroGradientCase(double spanK, uint32_t stepRatio)
{
	std::string diagnostic;
	auto settings = sweepFixture(20.0 + spanK, 20.0, 1.0, 2000.0, 50.0, 0.0, stepRatio);
	core::World world("HeatEngineNearZeroGradient", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	auto const run = runSweepCase(world, 2000.0, 400, 2);
	auto const& ledger = run.ledger;
	require(ledger.electricalKJ > 0.0,
		"the near-zero gradient case converted nothing - the fixture is vacuous");
	auto const work = ledger.electricalKJ + ledger.auxiliaryKJ;
	auto const bound = ledger.exergyKJ + ledger.windowKJ;
	require(work <= bound + 1e-9 * std::max(1.0, bound),
		"near-zero gradient: work exceeded the exergy of the extracted heat");
	// At this corner the window is a real fraction of the work, not noise.
	// It must still be bounded, and it must shrink with the cadence: the
	// ticket's tolerance is justified by the timestep, not by the result.
	require(ledger.peakWindowShare < 0.5,
		"near-zero gradient: the tick-cadence window exceeded half the work");
}

// A cap-bound case: the command sits far above what the hot body can
// surrender inside a tick, so the supply cap - C * (T_h - T_c) / dt - is
// what sets the intake. The gradient collapses in one tick, which is the
// whole point: the extraction must land exactly ON the sink and never
// through it. A cap that was loosened by any factor drives the hot node
// below the sink's tick-start temperature and trips the no-crossing check
// that the command-bound cases, where the cap never binds, leave untouched.
void runCapBoundCase(double hotCapacityKJPerK, double ratedInputKW,
	uint32_t stepRatio)
{
	std::string diagnostic;
	auto settings = sweepFixture(90.0, 20.0, 0.5, hotCapacityKJPerK,
		ratedInputKW, 0.0, stepRatio, {20.4, 20.5});
	core::World world("HeatEngineCapBound", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	// The case ends as soon as the thin body drains and the engine idles.
	// The cap is generous because how many ticks that takes is itself
	// cadence-dependent: at a fine cadence the cap is looser per tick and
	// the drain takes more of them.
	auto const run = runSweepCase(world, hotCapacityKJPerK, 400, 1);
	require(run.ledger.extractedKJ > 0.0,
		"the cap-bound case drew no heat - the fixture never engaged");
	// The drain is capped, not command-driven: the heat drawn must be far
	// less than the command could have asked for over the tick.
	require(run.ledger.extractedKJ
			< ratedInputKW * 90.0 / 100.0 * run.steps * 0.5,
		"the cap-bound case drew as if it were command-bound - the cap never bit");
	// And the cap must be the RIGHT size, not merely binding. On the first
	// converting tick the intake is the supply cap itself,
	// C * (T_h - T_c) / dt, so it is explicitly a rate PER TICK: spell
	// it against a fixed second instead of the real timestep and the
	// published intake is off by exactly that factor. This is the
	// cadence-sensitive half of the cap - the total drawn over a long
	// run hides it, because every cadence eventually drains the body.
	{
		std::string stepDiagnostic;
		auto settings = sweepFixture(90.0, 20.0, 0.5, hotCapacityKJPerK,
			ratedInputKW, 0.0, stepRatio, {20.4, 20.5});
		core::World stepWorld("HeatEngineCapBoundRate", 2, 2);
		configure(stepWorld, settings, stepDiagnostic);
		stepWorld.resumeSimulation();
		auto const before = stepWorld.getStationSystemsSnapshot();
		require(advanceStationSteps(stepWorld, 1), "cap-rate step failed");
		auto const after = stepWorld.getStationSystemsSnapshot();
		require(after.healthy, "cap-rate run failed its audit");
		auto const orc = engine(after, "orc");
		auto const expectedCapKW = hotCapacityKJPerK * std::max(0.0,
			reservoir(before, "hot").temperatureC
				- reservoir(before, "reject").temperatureC)
			/ after.systemsTimestepSeconds;
		require(orc.lastThermalInputKW > 0.0,
			"the cap-bound rate step drew nothing");
		// The command must sit above the cap for this to be a cap-bound
		// case at all; if it does not, the fixture is testing the wrong
		// constraint and should say so rather than pass vacuously.
		require(expectedCapKW < ratedInputKW,
			"the cap-bound case was not cap-bound at this cadence - the command was the tighter limit");
		require(orc.lastThermalInputKW <= expectedCapKW + 1e-6 * expectedCapKW,
			"the cap-bound engine drew above its supply cap");
		require(orc.lastThermalInputKW >= expectedCapKW - 1e-6 * expectedCapKW,
			"the supply cap was not the C * (T_h - T_c) / dt the fixture assumes");
	}
}

void heatEngineExergySweepSmoke(smoke::Context const&)
{
	// The three corners of the gradient surface, at the production cadence.
	runNearZeroGradientCase(0.05, 6);
	runGradientCase({"small", 24.0, 20.0, 1.0, 2000.0, 50.0, 0.0, 300, 2}, 6);
	runGradientCase({"large", 250.0, 20.0, 1.0, 2000.0, 100.0, 0.0, 300, 2}, 6);
	// Cap-bound: the supply cap, not the command, sets the intake.
	runCapBoundCase(50.0, 10000000.0, 6);
}

void heatEngineExergySweep(smoke::Context const&)
{
	// Near-zero gradients: thin enough that the window is a visible share
	// of the work, thick enough that the engine still converts.
	for (double span : {0.02, 0.05, 0.2, 0.5})
		runNearZeroGradientCase(span, 6);
	// Small and large gradients across the Carnot-fraction range, worst
	// case first (carnotFraction 1.0 sits on the ceiling).
	// Small and large gradients across the Carnot-fraction range, worst
	// case first (carnotFraction 1.0 sits on the ceiling). The auxiliaries
	// are zero here on purpose: a thin gradient at a low Carnot fraction
	// cannot pay a standing auxiliary draw, and that idle is correct -
	// heatEngineAuxiliaryIdle covers it. This sweep wants every case to
	// actually convert, so the bound is exercised rather than skipped.
	for (double hot : {22.0, 26.0, 40.0, 90.0, 150.0, 250.0})
	{
		for (double fraction : {1.0, 0.5, 0.1})
			runGradientCase({"sweep", hot, 20.0, fraction, 2000.0, 100.0, 0.0, 300, 2}, 6);
	}
	// Cap-bound across a range of thin-body sizes and step ratios.
	for (double capacity : {10.0, 50.0, 200.0})
	{
		for (uint32_t ratio : {1u, 6u, 60u})
			runCapBoundCase(capacity, 10000000.0, ratio);
	}
}

// Inverted gradients (#806): a hot node that is not hotter than the sink
// must produce idle, never negative output and never a pump that moves
// heat from cold to hot. The engine's own limiter is NoGradient, and the
// reservoirs must not move at all - a running pump would show up as the
// cold side warming and the warm side cooling, which is exactly what the
// second law forbids without work input, and this engine has none to give.
void heatEngineInvertedGradientIdle(smoke::Context const&)
{
	std::string diagnostic;
	// Inverted pairs: hot below the sink, and the degenerate equal case.
	for (auto pair : {std::pair<double, double>{18.0, 25.0},
		{20.0, 20.0}, {5.0, 90.0}, {19.999, 20.0}})
	{
		auto settings = enginePair(2000.0, 2000.0, 100.0, 1.0, 0.5);
		settings.thermalReservoirs[0].startingTemperatureC = pair.first;
		settings.thermalReservoirs[1].startingTemperatureC = pair.second;
		// Keep the gates legal (min-hot above max-sink) and above both
		// reservoirs, so the gradient itself - not a gate - is what the
		// case is testing.
		settings.heatEngines[0].maxSinkTemperatureC = 150.0;
		settings.heatEngines[0].minHotTemperatureC = 151.0;
		core::World world("HeatEngineInvertedGradient", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceStationSteps(world, 30), "inverted-gradient run failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "inverted-gradient run failed its audit");
		auto const orc = engine(snapshot, "orc");
		require(orc.status == core::StationHeatEngineStatus::Idle,
			"an inverted gradient did not idle the engine");
		require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
			&& orc.lastRejectionKW == 0.0,
			"an idling engine on an inverted gradient booked a non-zero rate");
		require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0,
			"an inverted gradient drove cumulative conversion");
		require(orc.lastLimiter == "NoGradient",
			"an inverted gradient was reported as something other than no gradient");
		// No pumping: neither reservoir moved. Heat did not climb.
		require(reservoir(snapshot, "hot").temperatureC == pair.first,
			"the inverted-gradient engine warmed or cooled its hot reservoir");
		require(reservoir(snapshot, "reject").temperatureC == pair.second,
			"the inverted-gradient engine pumped heat against the gradient");
	}
}

// Cadence invariance (#806, spec #762): the same simulated duration at
// different systems timestep rates produces the same conversion, within
// the tolerance the cadence itself earns. Both runs bracket the same
// continuous exergy integral; each one's error against it is its own
// quadrature window, so the two runs cannot differ by more than the sum
// of their windows. That is the whole justification for the tolerance -
// it is derived from the same identity as the bound, not tuned until the
// sweep went green - and the window shrinks as the timestep shrinks, so
// the agreement tightens with cadence exactly as it should.
// One cadence's ledger over a fixed window of world ticks.
struct CadenceRun
{
	uint32_t ratio{0};
	SecondLawLedger ledger;
	double seconds{0.0};
};

CadenceRun runCadence(double hotC, double sinkC, double carnotFraction,
	double hotCapacityKJPerK, double ratedInputKW, double auxKW,
	uint32_t ratio, uint64_t totalWorldTicks,
	std::pair<double, double> gates = {std::nan(""), std::nan("")})
{
	std::string diagnostic;
	auto settings = sweepFixture(hotC, sinkC, carnotFraction, hotCapacityKJPerK,
		ratedInputKW, auxKW, ratio, gates);
	core::World world("HeatEngineCadenceInvariance", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	CadenceRun run;
	run.ratio = ratio;
	auto const start = world.getStationSystemsSnapshot();
	auto const sinkStartK = reservoir(start, "reject").temperatureC + 273.15;
	// The step count comes from the timestep the World actually settled on,
	// and must divide the window into whole world ticks so every cadence
	// covers an exactly equal simulated duration.
	auto const stepSeconds = start.systemsTimestepSeconds;
	require(stepSeconds > 0.0, "the systems timestep came back non-positive");
	int const steps = static_cast<int>(totalWorldTicks / ratio);
	require(static_cast<uint64_t>(steps) * ratio == totalWorldTicks,
		"the cadence step count does not divide the window into whole world ticks");
	for (int step = 0; step < steps; ++step)
	{
		auto const before = world.getStationSystemsSnapshot();
		require(advanceStationSteps(world, 1), "cadence step failed");
		auto const after = world.getStationSystemsSnapshot();
		require(after.healthy, "cadence run failed its audit");
		auto const orc = engine(after, "orc");
		run.seconds += after.systemsTimestepSeconds;
		run.ledger.fold(hotCapacityKJPerK,
			reservoir(before, "hot").temperatureC + 273.15,
			reservoir(after, "hot").temperatureC + 273.15,
			sinkStartK, orc.lastThermalInputKW, orc.lastElectricalOutputKW,
			orc.lastAuxKW, after.systemsTimestepSeconds);
	}
	return run;
}

// Cadence invariance (#806, spec #762): the same simulated duration at
// different systems timestep rates produces the same conversion, within
// the tolerance the cadence itself earns. Both runs bracket the same
// continuous exergy integral; each one's error against it is its own
// quadrature window, so the two runs cannot differ by more than the sum
// of their windows. That is the whole justification for the tolerance -
// it is derived from the same identity as the bound, not tuned until the
// sweep went green - and the window shrinks as the timestep shrinks, so
// the agreement tightens with cadence exactly as it should.
//
// Two regimes are checked, because they fail differently:
//
//   * Command-bound - the intake is set by the command, not the supply
//     cap, so every cadence draws the SAME heat and only the conversion
//     integral is sampled differently. Here the extracted heat must agree
//     to floating point and the work must agree to the windows.
//   * Supply-capped - the intake is set by the heat the hot side can
//     give up within the tick, C * (T_h - T_c) / dt. That cap is
//     deliberately cadence-sensitive: a finer cadence re-reads the
//     shrinking gradient more often and can draw more over the same
//     duration. Here the heat is NOT expected to match, but the work
//     must still stay inside the summed windows, and the second-law bound
//     must hold at every cadence.
//
// A cadence-blind supply cap - one that divided by a fixed second instead
// of the real timestep - passes the command-bound regime untouched and
// breaks the supply-capped one, which is why both are needed.
void runCadenceRegime(double hotC, double sinkC, double carnotFraction,
	double hotCapacityKJPerK, double ratedInputKW, double auxKW,
	bool commandBound, std::pair<double, double> gates)
{
	// The window is stated in WORLD ticks, not seconds: every cadence
	// covers the same whole number of ticks, so the simulated durations
	// are equal by construction rather than equal to within a rounding of
	// 1/60. The fixed timestep is not exactly a sixtieth of a second, and
	// a duration spelled in seconds would leave each cadence a few
	// microseconds off the others - noise that would swamp the quadrature
	// windows being measured here.
	constexpr uint64_t totalWorldTicks = 7200;   // 120 s at a 1/60 s tick
	std::vector<CadenceRun> runs;
	for (uint32_t ratio : {1u, 6u, 30u, 60u})
		runs.push_back(runCadence(hotC, sinkC, carnotFraction, hotCapacityKJPerK,
			ratedInputKW, auxKW, ratio, totalWorldTicks, gates));
	// Every run covered the same simulated duration - the same whole
	// number of world ticks, so any residual difference is floating-point
	// accumulation over the sum, not a different window.
	auto const referenceSeconds = runs.front().seconds;
	for (auto const& run : runs)
		require(std::abs(run.seconds - referenceSeconds)
				<= 1e-9 * std::max(1.0, referenceSeconds),
			"cadence runs did not cover equal simulated durations");
	auto const& reference = runs.front();
	for (std::size_t i = 1; i < runs.size(); ++i)
	{
		auto const& run = runs[i];
		auto const spread = std::abs(run.ledger.electricalKJ - reference.ledger.electricalKJ);
		auto const allowance = run.ledger.windowKJ + reference.ledger.windowKJ;
		require(allowance > 0.0, "cadence allowance collapsed to zero");
		require(spread <= allowance + 1e-9 * std::max(1.0, allowance),
			"cadence invariance: conversion differed by more than the quadrature windows allow");
		// In the command-bound regime the drawn heat is set by the command
		// and must agree to floating point across every cadence.
		if (commandBound)
		{
			auto const heatSpread =
				std::abs(run.ledger.extractedKJ - reference.ledger.extractedKJ);
			require(heatSpread <= 1e-6 * std::max(1.0, reference.ledger.extractedKJ),
				"cadence invariance: the same command drew different heat at different rates");
		}
		// The second-law bound holds at every cadence, not just the
		// production one.
		auto const work = run.ledger.electricalKJ + run.ledger.auxiliaryKJ;
		auto const bound = run.ledger.exergyKJ + run.ledger.windowKJ;
		require(work <= bound + 1e-9 * std::max(1.0, bound),
			"cadence invariance: the work exceeded the exergy of the extracted heat");
	}
	// The window grows with the timestep: the finest cadence earns the
	// tightest bound. If this stopped being true the tolerance would be a
	// fudge rather than a quadrature statement.
	require(runs.back().ledger.windowKJ > runs.front().ledger.windowKJ,
		"cadence invariance: the quadrature window did not grow with the coarsest cadence");
	// Only the command-bound regime is expected to sample the integral
	// tightly; a cap-bound body is stiff by construction and its window
	// is a real fraction of the work.
	if (commandBound)
		require(runs.front().ledger.windowKJ < 1e-3 * reference.ledger.electricalKJ,
			"cadence invariance: the finest cadence's window is not tight relative to the work");
}

void heatEngineCadenceInvariance(smoke::Context const&)
{
	// Command-bound: huge reservoirs, a modest command. The gradient
	// barely moves over the window, so the only thing changing between
	// cadences is how finely the same integral is sampled.
	runCadenceRegime(90.0, 20.0, 0.5, 5000000.0, 200.0, 1.0,
		true, {20.4, 20.5});
	// Cap-bound over a thin hot body: the supply cap, not the command,
	// sets the intake. This regime is deliberately NOT held to the same
	// tight spread as the command-bound one - the cap is C * (T_h - T_c)
	// per tick, so a finer cadence re-reads the collapsing gradient more
	// often and legitimately draws more over the same duration. What is
	// checked here is that the spread still stays inside the summed
	// quadrature windows and the second-law bound holds at every rate.
	runCadenceRegime(90.0, 20.0, 0.5, 3.0, 100.0, 0.0,
		false, {20.4, 20.5});
}

// ---------------------------------------------------------------------------
// The heat engine as a station installation (#807)
//
// Outages (#413) and maintenance (#414) target an installation by its stable
// id alone; the family only becomes targetable by registering in
// stationInstallationIdentities. Every assertion here is made on what a
// configured World publishes across the outage window - the outage and
// maintenance snapshots, the engine's own rates and ledgers, and the two
// reservoirs' energy books - so the contract is tested as an operator sees
// it rather than as a lookup table.
// ---------------------------------------------------------------------------

core::InstallationOutageSettings makeOutage(std::string id, std::string target,
	double capacityFraction, double firstSeconds, double durationSeconds,
	double recurrenceSeconds = 0.0, int priority = 0)
{
	core::InstallationOutageSettings outage;
	outage.id = std::move(id);
	outage.installationId = std::move(target);
	outage.capacityFraction = capacityFraction;
	outage.firstOccurrenceSeconds = firstSeconds;
	outage.durationSeconds = durationSeconds;
	outage.recurrenceSeconds = recurrenceSeconds;
	outage.priority = priority;
	return outage;
}

// The #414 prerequisite set: one reusable tool and one consumed part, so
// the job really runs, really isolates and really pays for its work.
core::MaintenanceJobSettings makeMaintenance(std::string id, std::string target,
	double firstSeconds, double durationSeconds)
{
	core::MaintenanceRequirementSettings tool;
	tool.storeId = "tools"; tool.quantity = 2.0; tool.consumed = false;
	core::MaintenanceRequirementSettings part;
	part.storeId = "parts"; part.quantity = 1.0; part.consumed = true;
	core::MaintenanceJobSettings job;
	job.id = std::move(id);
	job.installationId = std::move(target);
	job.firstOccurrenceSeconds = firstSeconds;
	job.durationSeconds = durationSeconds;
	job.requirements = {tool, part};
	job.wasteStoreId = "waste";
	return job;
}

// The maintenance fixture: the standing engine pair plus the three stores a
// job needs, all in kg, so the isolation - not a missing prerequisite - is
// what moves the engine's output when the window opens.
core::StationSystemsSettings engineMaintenancePair(double firstSeconds,
	double durationSeconds)
{
	auto settings = enginePair();
	core::ResourceStoreSettings tools;
	tools.id = "tools"; tools.unit = "kg"; tools.capacity = 100.0; tools.startingQualified = 10.0;
	core::ResourceStoreSettings parts;
	parts.id = "parts"; parts.unit = "kg"; parts.capacity = 100.0; parts.startingQualified = 8.0;
	core::ResourceStoreSettings waste;
	waste.id = "waste"; waste.unit = "kg"; waste.capacity = 10.0; waste.startingQualified = 0.0;
	settings.stores = {tools, parts, waste};
	settings.maintenanceJobs = {makeMaintenance("overhaul", "orc", firstSeconds, durationSeconds)};
	return settings;
}

core::StationOutageSnapshot outageSnap(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.outages)
		if (entry.id == id) return entry;
	throw std::runtime_error("outage missing from snapshot");
}

core::StationMaintenanceSnapshot maintenanceSnap(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.maintenanceJobs)
		if (entry.id == id) return entry;
	throw std::runtime_error("maintenance job missing from snapshot");
}

double storeQuantity(core::World& world, std::string const& id)
{
	for (auto const& entry : world.getStationSystemsSnapshot().stores)
		if (entry.id == id) return entry.currentQualified;
	throw std::runtime_error("store missing from snapshot");
}

// The engine registers as an installation: an outage or a maintenance job
// may name it, the snapshots report it as a heat engine rather than an
// unknown kind, and the target's effective availability is the number the
// contract actually applied. An id shared with another installation kind is
// still refused - the enlarged family set widens the namespace, it does not
// loosen the rule that the namespace must be unambiguous.
void heatEngineInstallationRegistration(smoke::Context const&)
{
	std::string diagnostic;
	// Both schedules naming the engine are accepted, and both report the
	// engine's kind before their windows even open.
	{
		auto settings = enginePair();
		settings.outages = {makeOutage("down", "orc", 0.0, 100.0, 10.0)};
		core::World world("HeatEngineInstallRegister", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 5.0), "registration warm-up failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "registration warm-up failed its audit");
		auto const snap = outageSnap(snapshot, "down");
		require(snap.installationKind == "heat engine",
			"an outage on an engine did not report the heat engine installation kind");
		require(!snap.active && snap.installationAvailability == 1.0,
			"the engine was not at full availability ahead of its outage window");
		require(engine(snapshot, "orc").availabilityFactor == 1.0,
			"the engine snapshot did not publish full availability ahead of the outage");
	}
	{
		auto settings = engineMaintenancePair(100.0, 10.0);
		core::World world("HeatEngineInstallMaintain", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceSeconds(world, 5.0), "maintenance registration warm-up failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "maintenance registration warm-up failed its audit");
		auto const job = maintenanceSnap(snapshot, "overhaul");
		require(job.installationKind == "heat engine",
			"a maintenance job on an engine did not report the heat engine installation kind");
		require(job.phase == core::StationMaintenancePhase::Idle && !job.isolated,
			"the maintenance job was busy before its window");
	}
	// An unknown installation is still refused: registering the engine does
	// not turn the contract into a catch-all.
	{
		auto settings = enginePair();
		settings.outages = {makeOutage("ghost", "reactor", 0.0, 10.0, 5.0)};
		core::World world("HeatEngineInstallUnknown", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(!world.configureStationSystems(settings, false, &diagnostic),
			"an outage naming an unregistered installation was accepted");
	}
	// The enlarged namespace is still unambiguous: a generator sharing the
	// engine's id cannot both claim it, because the outage contract carries
	// no kind and could not say which machine it caps.
	{
		auto settings = enginePair();
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel"; diesel.unit = "kWh"; diesel.capacity = 100000.0;
		diesel.startingQualified = 100000.0;
		settings.stores = {diesel};
		core::GeneratorSettings twin;
		twin.id = "orc"; // collides with the engine's installation id
		twin.fuelStoreId = "diesel";
		twin.thermalClass = "warm"; // match the sink reservoir's service class
		twin.heatSinkId = "hot";
		twin.ratedOutputKW = 10.0;
		twin.efficiency = 0.5;
		twin.maxRampKWPerSecond = 100.0;
		twin.commandedOutputKW = 10.0;
		settings.generators = {twin};
		settings.outages = {makeOutage("down", "orc", 0.0, 10.0, 5.0)};
		core::World world("HeatEngineInstallAmbiguous", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		require(!world.configureStationSystems(settings, false, &diagnostic),
			"a heat engine and a generator sharing an installation id were accepted");
		require(diagnostic.find("heat engine") != std::string::npos,
			("the installation ambiguity refusal did not name the heat engine family: [" + diagnostic + "]").c_str());
	}
}

// A full outage takes the engine out of service: no intake, so no
// generation on the bus and no extraction or rejection booked at either
// reservoir. The control run beside it makes the difference visible where
// an operator looks - the hot reservoir stops draining.
void heatEngineOutageRemovesGeneration(smoke::Context const&)
{
	std::string diagnostic;
	// Control: the engine runs the whole time and drains the hot side.
	auto control = enginePair();
	core::World controlWorld("HeatEngineOutageControl", 2, 2);
	configure(controlWorld, control, diagnostic);
	controlWorld.resumeSimulation();
	require(advanceSeconds(controlWorld, 20.0), "outage control run failed");
	auto const controlSnap = controlWorld.getStationSystemsSnapshot();
	require(controlSnap.healthy, "outage control run failed its audit");
	auto const controlEngine = engine(controlSnap, "orc");
	require(controlEngine.status == core::StationHeatEngineStatus::Active
		&& controlEngine.lastElectricalOutputKW > 0.0,
		"the control engine was not generating before the outage");
	auto const controlHotC = reservoir(controlSnap, "hot").temperatureC;
	require(controlHotC < 90.0 - 1e-6,
		"the control engine did not drain the hot reservoir");
	// Outage from the first tick: the engine never gets to move anything.
	// The window is sampled well inside itself, and only after it has closed
	// is the restoration read.
	auto settings = enginePair();
	settings.outages = {makeOutage("down", "orc", 0.0, 0.0, 20.0)};
	core::World world("HeatEngineOutageFull", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 10.0), "full-outage run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "full-outage run failed its audit");
	auto const orc = engine(snapshot, "orc");
	require(orc.status == core::StationHeatEngineStatus::Idle
		&& orc.lastLimiter == "OutOfService",
		"an outaged engine did not idle with the OutOfService limiter");
	require(orc.availabilityFactor == 0.0,
		"an outaged engine did not report zero availability");
	require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
		&& orc.lastRejectionKW == 0.0,
		"an outaged engine still booked rates this tick");
	require(orc.thermalInputEnergyKJ == 0.0 && orc.electricalEnergyKJ == 0.0
		&& orc.rejectionEnergyKJ == 0.0,
		"an outaged engine still accumulated ledger energy");
	require(snapshot.power.heatEngineOutputKW == 0.0,
		"an outaged engine still contributed bus generation");
	// The hot reservoir's drain visibly changed: the same fixture for the
	// same elapsed time, and the outaged hot side kept every joule.
	require(reservoir(snapshot, "hot").temperatureC == 90.0,
		"the outaged run still drained the hot reservoir");
	require(reservoir(snapshot, "reject").temperatureC == 20.0,
		"the outaged run still booked rejection into the sink");
	require(reservoir(snapshot, "hot").engineExtractionEnergyKJ == 0.0
		&& reservoir(snapshot, "reject").engineRejectionEnergyKJ == 0.0,
		"the reservoir-side engine bookings were not removed by the outage");
	require(outageSnap(snapshot, "down").active
		&& outageSnap(snapshot, "down").installationAvailability == 0.0,
		"the outage snapshot did not report the engine at zero availability");
	// Restoration: the same world, the window closed, the engine returns to
	// exactly the duty the control run had at the same elapsed time.
	require(advanceSeconds(world, 15.0), "post-restoration run failed");
	auto const after = world.getStationSystemsSnapshot();
	require(after.healthy, "post-restoration run failed its audit");
	require(!outageSnap(after, "down").active
		&& outageSnap(after, "down").installationAvailability == 1.0,
		"the outage did not restore the engine's full availability");
	auto const restored = engine(after, "orc");
	require(restored.status == core::StationHeatEngineStatus::Active
		&& restored.availabilityFactor == 1.0
		&& restored.lastElectricalOutputKW > 0.0,
		"the engine did not return to service when the outage ended");
	require(restored.thermalInputEnergyKJ > 0.0 && restored.rejectionEnergyKJ > 0.0,
		"the restored engine resumed no extraction or rejection");
}

// A partial outage leaves part of the rating in service. The cap is applied
// to the deliverable intake, so generation, extraction and rejection all
// scale together by the same fraction and the conversion ledger still
// closes - the engine runs smaller, it does not run wrong.
void heatEngineOutagePartialCapacity(smoke::Context const&)
{
	std::string diagnostic;
	constexpr double fraction = 0.4;
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 1.0);
	settings.outages = {makeOutage("derate", "orc", fraction, 0.0, 30.0)};
	core::World world("HeatEngineOutagePartial", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 10.0), "partial-outage run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "partial-outage run failed its audit");
	auto const orc = engine(snapshot, "orc");
	require(orc.availabilityFactor == fraction,
		"the outage did not leave the declared fraction of the rating available");
	require(orc.status == core::StationHeatEngineStatus::Active
		&& orc.lastLimiter == "CapacityReduction",
		"a partially outaged engine did not name CapacityReduction as its binding ceiling");
	// The intake sits exactly at the remaining rating, not at the command.
	require(std::abs(orc.lastThermalInputKW - 100.0 * fraction) < 1e-9,
		"the partially outaged engine's intake was not capped at the remaining rating");
	require(orc.lastThermalInputKW < orc.commandedInputKW - 1e-9,
		"the capacity reduction did not cut below the engine's command");
	// Everything scales with the intake: the conversion ratios are the
	// running engine's own, unchanged by the outage.
	require(std::abs(orc.efficiencyNow - orc.carnotEfficiencyNow * 0.5) < 1e-12,
		"the partially outaged engine converted at anything but its declared efficiency");
	require(std::abs(orc.lastRejectionKW
		- (orc.lastThermalInputKW - orc.efficiencyNow * orc.lastThermalInputKW
			+ orc.auxiliaryPowerKW)) < 1e-9,
		"the partially outaged engine's rejection did not follow its own ledger");
	require(std::abs(orc.residualKJ) < 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
		"the partially outaged engine's energy ledger did not close");
	require(outageSnap(snapshot, "derate").installationAvailability == fraction,
		"the outage snapshot did not report the partial availability");
}

// Maintenance on an engine behaves like maintenance on any other registered
// family: the job waits, then runs and isolates the machine for its whole
// duration, then completes by paying for its work and returning the tool.
// The isolation removes the engine's recovery generation and its
// reservoir-side bookings for exactly the window, and nothing else.
void heatEngineMaintenanceIsolation(smoke::Context const&)
{
	std::string diagnostic;
	auto settings = engineMaintenancePair(20.0, 10.0);
	core::World world("HeatEngineMaintenance", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	// Ahead of the window: the engine works at full service.
	require(advanceSeconds(world, 15.0), "pre-window maintenance run failed");
	auto const before = world.getStationSystemsSnapshot();
	require(before.healthy, "pre-window maintenance run failed its audit");
	require(maintenanceSnap(before, "overhaul").phase == core::StationMaintenancePhase::Idle
		&& maintenanceSnap(before, "overhaul").occurrencesStarted == 0,
		"the maintenance job was busy before its window");
	require(engine(before, "orc").availabilityFactor == 1.0
		&& engine(before, "orc").status == core::StationHeatEngineStatus::Active,
		"the engine was not in full service before the maintenance window");
	// Inside the window: the job is Active, the engine is isolated, and the
	// recovery generation and both reservoir bookings are gone.
	require(advanceSeconds(world, 8.0), "in-window maintenance run failed");
	auto const during = world.getStationSystemsSnapshot();
	require(during.healthy, "in-window maintenance run failed its audit");
	auto const active = maintenanceSnap(during, "overhaul");
	require(active.phase == core::StationMaintenancePhase::Active && active.isolated,
		"the maintenance job did not isolate the engine while running");
	require(active.progressSeconds > 0.0 && active.remainingSeconds > 0.0,
		"the maintenance job made no progress while isolating the engine");
	auto const isolated = engine(during, "orc");
	require(isolated.availabilityFactor == 0.0
		&& isolated.status == core::StationHeatEngineStatus::Idle
		&& isolated.lastLimiter == "OutOfService",
		"an isolated engine was not held out of service with the OutOfService limiter");
	require(isolated.lastElectricalOutputKW == 0.0
		&& isolated.lastThermalInputKW == 0.0 && isolated.lastRejectionKW == 0.0,
		"an isolated engine still generated or booked heat");
	require(during.power.heatEngineOutputKW == 0.0,
		"an isolated engine still contributed bus generation");
	// The prerequisites are reserved but not yet spent: the reservation is
	// visible on the requirements while the material stays in its own store
	// until the work actually completes.
	require(active.requirements.size() == 2
		&& active.requirements[0].reserved && active.requirements[1].reserved,
		"the maintenance job did not reserve its prerequisites while running");
	require(storeQuantity(world, "tools") == 10.0 && storeQuantity(world, "parts") == 8.0,
		"reserved material left its store before the job completed");
	require(storeQuantity(world, "waste") == 0.0,
		"the maintenance job spent its waste before completing");
	// After completion: the tool is returned, the part is waste, the engine
	// is back in full service and the occurrence is recorded.
	require(advanceSeconds(world, 8.0), "post-window maintenance run failed");
	auto const after = world.getStationSystemsSnapshot();
	require(after.healthy, "post-window maintenance run failed its audit");
	auto const done = maintenanceSnap(after, "overhaul");
	require(done.phase == core::StationMaintenancePhase::Completed && !done.isolated
		&& done.occurrencesStarted == 1 && done.occurrencesCompleted == 1 && done.exhausted,
		"the maintenance job did not complete its occurrence on the engine");
	require(std::abs(storeQuantity(world, "tools") - 10.0) < 1e-9,
		"the reusable tool was not returned after the job completed");
	require(std::abs(storeQuantity(world, "parts") - 7.0) < 1e-9,
		"the consumed part was not removed from its source by completion");
	require(std::abs(storeQuantity(world, "waste") - 1.0) < 1e-9,
		"the consumed part did not land in the waste store on completion");
	require(engine(after, "orc").availabilityFactor == 1.0
		&& engine(after, "orc").status == core::StationHeatEngineStatus::Active,
		"the engine was not restored to full service after the job completed");
}

// Coherence across the outage transition: the engine's ledger and the two
// reservoirs' engine bookings agree at every sampled tick, the cumulative
// quantities never run backwards and never jump, and the outage window is
// exactly flat - no leak through a closed window and no double booking when
// the window opens or closes.
void heatEngineOutageTransitionCoherence(smoke::Context const&)
{
	std::string diagnostic;
	auto settings = enginePair();
	// One window opening at 5 s and closing at 15 s, so the sample crosses
	// the start transition, the whole closed window, and the restore.
	settings.outages = {makeOutage("down", "orc", 0.0, 5.0, 10.0)};
	core::World world("HeatEngineOutageCoherence", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	auto previous = world.getStationSystemsSnapshot();
	for (int step = 0; step < 400; ++step)
	{
		require(advanceStationSteps(world, 1), "coherence step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "coherence run failed its audit");
		auto const orc = engine(snapshot, "orc");
		auto const& hot = reservoir(snapshot, "hot");
		auto const& reject = reservoir(snapshot, "reject");
		// The engine's own ledger and the reservoir-side bookings are the
		// same money recorded at both ends of one crossing.
		require(std::abs(orc.thermalInputEnergyKJ - hot.engineExtractionEnergyKJ) < 1e-6,
				"the engine intake and the hot reservoir's extraction booking disagreed");
		require(std::abs(orc.rejectionEnergyKJ - reject.engineRejectionEnergyKJ) < 1e-6,
				"the engine rejection and the sink reservoir's booking disagreed");
		// Nothing resets or double-counts across the transition: every
		// cumulative quantity is monotone non-decreasing.
		auto const previousOrc = engine(previous, "orc");
		auto const wasActive = outageSnap(previous, "down").active;
		auto const isActive = outageSnap(snapshot, "down").active;
		require(orc.thermalInputEnergyKJ + 1e-9 >= previousOrc.thermalInputEnergyKJ
			&& orc.electricalEnergyKJ + 1e-9 >= previousOrc.electricalEnergyKJ
			&& orc.rejectionEnergyKJ + 1e-9 >= previousOrc.rejectionEnergyKJ,
			"an engine ledger ran backwards across the outage transition");
		require(hot.engineExtractionEnergyKJ + 1e-9
				>= reservoir(previous, "hot").engineExtractionEnergyKJ,
			"the hot reservoir's engine booking ran backwards");
		require(reject.engineRejectionEnergyKJ + 1e-9
				>= reservoir(previous, "reject").engineRejectionEnergyKJ,
			"the sink reservoir's engine booking ran backwards");
		// The engine's own conversion closes on itself every tick.
		require(std::abs(orc.residualKJ) < 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
			"the engine ledger failed to close across the outage transition");
		// Inside the window the ledgers are exactly flat: a closed window
		// leaks nothing. The tick that opens the window is exempt - its left
		// boundary still read full service, so the engine legitimately ran
		// up to the instant the outage took hold.
		if (isActive && wasActive)
		{
			require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
				&& orc.lastRejectionKW == 0.0,
				"an outaged engine moved energy inside its window");
			require(orc.thermalInputEnergyKJ == previousOrc.thermalInputEnergyKJ
				&& orc.electricalEnergyKJ == previousOrc.electricalEnergyKJ
				&& orc.rejectionEnergyKJ == previousOrc.rejectionEnergyKJ,
				"an engine ledger moved inside the outage window");
			require(orc.availabilityFactor == 0.0
				&& orc.status == core::StationHeatEngineStatus::Idle,
				"the engine was not held out of service inside the outage window");
		}
		else if (!isActive)
		{
			require(orc.availabilityFactor == 1.0,
				"the engine was not at full availability outside the outage window");
		}
		previous = snapshot;
	}
	// The whole run leaves the engine having generated only outside the
	// window, and the hot reservoir only cooled by exactly that much.
	auto const final = world.getStationSystemsSnapshot();
	auto const finalOrc = engine(final, "orc");
	require(finalOrc.thermalInputEnergyKJ > 0.0 && finalOrc.electricalEnergyKJ > 0.0,
		"the engine never generated outside its outage window");
	require(finalOrc.thermalInputEnergyKJ
		< 100.0 * final.systemsTimestepSeconds * 400.0,
		"the engine converted as though the outage window had never happened");
	require(reservoir(final, "hot").engineExtractionEnergyKJ
		== finalOrc.thermalInputEnergyKJ,
		"the final hot reservoir booking does not match the engine ledger");
}
// #827: conversion and cable accounts must fund the same physical transfer.
void heatEngineGenerationConnections()
{
	for (auto const efficiency : {0.5, 0.0, 1.0})
	for (auto const cap : {1000.0, 1.0})
	for (auto const aux : {0.0, 1.0})
	for (auto const acceptance : {500.0, 2.0, 0.0})
	{
		auto settings = enginePair(20000, 20000, 100, 0.5, aux);
		settings.systemsStepWorldTicks = 60;
		settings.batteries[0].maxChargeKW = acceptance;
		for (auto const& link : core::station_connections::build(settings).links)
			if (link.designKey.ownerType == "Heat engine" && link.designKey.relation == "Generation") {
				auto& p = settings.connections[link.designKey];
				p.efficiency = efficiency;
				p.maximumThroughput = cap;
			}
		std::string diagnostic;
		require(core::validateStationSystemsSettings(settings, diagnostic),
			("generation fixture refused: " + diagnostic).c_str());
		auto const quantum = core::World::getFixedTimestep();
		auto const seconds = 60.0 * static_cast<double>(quantum);
		core::StationSystemsRuntime runtime(settings, quantum);
		auto before = runtime.snapshot();
		auto const close = [](double a, double b) {
			require(std::abs(a - b) <= 2e-7 * std::max({1.0, std::abs(a), std::abs(b)}),
				"heat engine generation cable energy is unfunded or misallocated");
		};
		for (int step = 1; step <= 3; ++step)
		{
			auto const eta = 0.5 * (1.0
				- (reservoir(before, "reject").temperatureC + 273.15)
				/ (reservoir(before, "hot").temperatureC + 273.15));
			auto const expectedInput = efficiency > 0 && acceptance > 0
				? std::min({eta * 100.0 - aux, cap, acceptance / efficiency}) : 0.0;
			require(runtime.advanceTo(step * seconds), "generation connection step failed");
			auto const snap = runtime.snapshot();
			require(snap.healthy, "generation connection audit failed");
			auto const e = engine(snap, "orc");
			auto const cable = std::find_if(snap.connections.begin(), snap.connections.end(),
				[](auto const& c) { return c.key.ownerType == "Heat engine"
					&& c.key.relation == "Generation"; });
			require(cable != snap.connections.end(), "generation cable missing");
			// Reservoir extraction pays for sink rejection, bus work AND cable loss.
			close(e.thermalInputEnergyKJ,
				e.rejectionEnergyKJ + e.electricalEnergyKJ + cable->lostTotal);
			close(cable->lastInputRate, expectedInput);
			close(cable->lastDeliveredRate, expectedInput * efficiency);
			close(cable->lastLossRate, expectedInput * (1.0 - efficiency));
			close(e.residualKJ, 0.0);
			close((snap.power.batteries[0].storedKWh - 100.0) * 3600.0,
				e.electricalEnergyKJ);
			close(cable->inputTotal, cable->deliveredTotal + cable->lostTotal);
			close(cable->deliveredTotal, e.electricalEnergyKJ);
			close(e.lastElectricalOutputKW, expectedInput * efficiency);
			close(e.lastThermalInputKW, expectedInput > 0 ? (expectedInput + aux) / eta : 0);
			close(e.lastRejectionKW, e.lastThermalInputKW - expectedInput);
			close(reservoir(snap, "hot").temperatureC,
				90.0 - e.thermalInputEnergyKJ / 20000.0);
			close(reservoir(snap, "reject").temperatureC,
				20.0 + e.rejectionEnergyKJ / 20000.0);
			if (expectedInput == 0) {
				require(e.status == core::StationHeatEngineStatus::Idle, "blocked cable engine did not idle");
				for (auto const& c : snap.connections)
					require(c.inputTotal == 0 && c.lostTotal == 0,
						"idle engine left phantom connection input or loss");
			}
			before = snap;
		}
	}
}

// #828: exercise the allocator through committed World snapshots, including
// the source-side inventory, both transport ledgers and retained producer heat.
void heatEngineThermalConnections(smoke::Context const&)
{
	heatEngineGenerationConnections();
	for (auto const intakeCap : {0.01, 1000.0})
	for (auto const rejectionCap : {0.01, 1000.0})
	for (auto const intakeEfficiency : {0.0, 0.5, 1.0})
	for (auto const rejectionEfficiency : {0.0, 0.5, 1.0})
	for (auto const aux : {0.0, 0.0001, 1.0})
	{
		auto settings = enginePair(20000, 20000, 100, 0.5, aux);
		settings.systemsStepWorldTicks = 60;
		for (auto const& link : core::station_connections::build(settings).links)
		{
			if (link.designKey.ownerType != "Heat engine") continue;
			auto const& role = link.designKey.relation;
			if (role != "Thermal intake" && role != "Rejection heat") continue;
			auto& p = settings.connections[link.designKey];
			p.maximumThroughput = role == "Thermal intake" ? intakeCap : rejectionCap;
			p.efficiency = role == "Thermal intake" ? intakeEfficiency : rejectionEfficiency;
		}
		core::World world("EngineThermalConnections", 2, 2);
		std::string diagnostic;
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		auto const close = [](double a, double b) {
			require(std::abs(a - b) <= 2e-7 * std::max({1.0, std::abs(a), std::abs(b)}),
				"heat engine thermal connection ledger mismatch");
		};
		for (int step = 0; step < 3; ++step)
		{
			require(advanceStationSteps(world, 1), "thermal connection step failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.healthy, "thermal connection audit failed");
			auto const e = engine(snap, "orc");
			auto const flow = [&](char const* role) -> core::StationConnectionSnapshot const& {
				for (auto const& c : snap.connections)
					if (c.key.ownerType == "Heat engine" && c.key.relation == role) return c;
				throw std::runtime_error("missing engine thermal connection");
			};
			auto const& in = flow("Thermal intake");
			auto const& out = flow("Rejection heat");
			require(in.lastInputRate <= intakeCap + 1e-9, "engine bypassed intake throughput");
			require(out.lastInputRate <= rejectionCap + 1e-9, "engine bypassed rejection throughput");
			auto const feasible = 0.5 * e.carnotEfficiencyNow
				* std::min(100.0, intakeCap * intakeEfficiency) > aux + 1e-12;
			if (feasible || intakeEfficiency == 0)
				require(in.lastInputRate > 0, "requested engine intake has no transport telemetry");
			else close(in.lastInputRate, 0);
			auto const expectedIntake = feasible ? std::min(100.0, intakeCap * intakeEfficiency) : 0.0;
			close(e.lastThermalInputKW, expectedIntake);
			close(e.lastElectricalOutputKW, feasible
				? expectedIntake * 0.5 * e.carnotEfficiencyNow - aux : 0.0);
			close(e.lastRejectionKW, expectedIntake - e.lastElectricalOutputKW);
			if (intakeEfficiency == 0) close(in.lastInputRate, std::min(100.0, intakeCap));
			close(in.lastDeliveredRate, in.lastInputRate * intakeEfficiency);
			close(out.lastDeliveredRate, out.lastInputRate * rejectionEfficiency);
			for (auto const* c : {&in, &out}) {
				close(c->inputTotal, c->deliveredTotal + c->lostTotal);
				close(c->lastInputRate, c->lastDeliveredRate + c->lastLossRate);
			}
			close(e.lastThermalInputKW, in.lastDeliveredRate);
			close(e.thermalInputEnergyKJ, in.deliveredTotal);
			close(reservoir(snap, "hot").engineExtractionEnergyKJ, in.inputTotal);
			close(reservoir(snap, "reject").engineRejectionEnergyKJ, out.deliveredTotal);
			close(reservoir(snap, "hot").temperatureC, 90.0 - in.inputTotal / 20000.0);
			close(reservoir(snap, "reject").temperatureC, 20.0 + out.deliveredTotal / 20000.0);
			close(e.rejectionEnergyKJ, out.producedHeatKJ);
			close(out.producedHeatKJ, out.inputTotal + out.retainedHeatKJ);
			close(in.inputTotal, e.electricalEnergyKJ + out.deliveredTotal
				+ out.lostTotal + out.retainedHeatKJ + in.lostTotal);
			if (feasible) require(e.lastElectricalOutputKW > 0,
				"feasible constrained engine failed to convert heat");
		}
	}
	// A dispatched engine against a closed bus has no active intake request,
	// even if the intake connection would lose everything sent through it.
	{
		auto settings = enginePair();
		settings.batteries.clear();
		for (auto const& link : core::station_connections::build(settings).links)
			if (link.designKey.ownerType == "Heat engine" && link.designKey.relation == "Thermal intake")
				settings.connections[link.designKey].efficiency = 0;
		core::World world("EngineZeroEfficiencyNoAcceptance", 2, 2);
		std::string diagnostic;
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		require(advanceStationSteps(world, 1), "zero-acceptance lossy intake step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy && engine(snap, "orc").lastLimiter == "DemandAcceptance"
			&& reservoir(snap, "hot").engineExtractionEnergyKJ == 0,
			"zero-efficiency intake consumed heat without demand acceptance");
		for (auto const& c : snap.connections)
			require(c.inputTotal == 0, "idle closed-bus engine created transport flow");
	}
	// Independent sink heating eventually trips the whole conversion, but
	// heat already produced remains real and continues draining through its
	// constrained, lossy rejection interface even while the engine is off.
	auto settings = enginePair(20000, 1.0, 100, 0.5, 1.0);
	settings.systemsStepWorldTicks = 60;
	settings.thermalReservoirs[1].absorbedHeatKW = 1.0;
	for (auto const& link : core::station_connections::build(settings).links)
		if (link.designKey.ownerType == "Heat engine" && link.designKey.relation == "Rejection heat") {
			settings.connections[link.designKey].maximumThroughput = 0.02;
			settings.connections[link.designKey].efficiency = 0.5;
		}
	core::World world("EngineRetainedRejectionTrip", 2, 2);
	std::string diagnostic;
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "retained rejection initial step failed");
	auto before = world.getStationSystemsSnapshot();
	require(before.healthy && engine(before, "orc").lastElectricalOutputKW > 0,
		("retained rejection fixture did not run: " + engine(before, "orc").lastLimiter
			+ " sink=" + std::to_string(reservoir(before, "reject").temperatureC)
			+ " healthy=" + std::to_string(before.healthy)).c_str());
	auto after = before;
	for (int step = 0; step < 10; ++step) {
		before = after;
		require(advanceStationSteps(world, 1), "retained rejection trip step failed");
		after = world.getStationSystemsSnapshot();
		if (engine(after, "orc").status == core::StationHeatEngineStatus::Tripped) break;
	}
	auto const e = engine(after, "orc");
	require(after.healthy && e.status == core::StationHeatEngineStatus::Tripped
		&& e.lastLimiter == "SinkOver" && e.lastThermalInputKW == 0
		&& e.lastElectricalOutputKW == 0 && e.lastRejectionKW == 0,
		"constrained rejection did not preserve whole-machine sink trip");
	require(e.thermalInputEnergyKJ == engine(before, "orc").thermalInputEnergyKJ
		&& e.rejectionEnergyKJ == engine(before, "orc").rejectionEnergyKJ,
		"tripped engine kept converting heat");
	for (auto const& c : after.connections)
		if (c.key.ownerType == "Heat engine" && c.key.relation == "Rejection heat") {
			require(c.retainedHeatKJ > 0 && c.lastDeliveredRate > 0,
				"retained engine heat did not drain while tripped");
			require(std::abs(c.producedHeatKJ - c.inputTotal - c.retainedHeatKJ) < 1e-7
				&& std::abs(c.deliveredTotal - reservoir(after, "reject").engineRejectionEnergyKJ) < 1e-7,
				"retained rejection drain erased heat or missed the reservoir");
		}
}

// ---------------------------------------------------------------------------
// Whole-step mean rates across an event-split step (#832)
//
// When a forcing edge falls inside a Station step the step is subdivided and
// every sub-interval is integrated with the rates in force over it. Each
// published step-mean rate must therefore describe the WHOLE committed step:
// rate * stepSeconds has to reproduce the cumulative energy the step
// actually booked. #832: the engine's intake / output / rejection rates and
// both reservoirs' engine-exchange fields were left as the FINAL
// sub-interval's sample while the connection telemetry beside them was
// duration-weighted, so a step that began with an outage delivered 4.81895
// kJ and then published zero for every one of those rates. Endpoint-only
// quantities - status, availability, the limiter - keep their "as of the
// step boundary" meaning; these checks pin the two apart so a whole-step
// mean is never mistaken for an outage that never happened, and an
// "OutOfService" endpoint is never read as a step that produced nothing.
// ---------------------------------------------------------------------------

// A published step-mean rate must integrate to the cumulative energy the
// same committed step booked, at the conservation scale the rest of the
// suite already uses.
void expectStepMean(double meanKW, double deltaKJ, double stepSeconds,
	std::string const& what)
{
	auto const impliedKJ = meanKW * stepSeconds;
	require(std::abs(impliedKJ - deltaKJ)
			<= 1e-6 * std::max({1.0, std::abs(impliedKJ), std::abs(deltaKJ)}),
		("#832: " + what + " is not the whole-step mean (rate x step = "
			+ std::to_string(impliedKJ) + " kJ, committed " + std::to_string(deltaKJ)
			+ " kJ)").c_str());
}

core::StationConnectionSnapshot const* engineConnection(
	core::StationSystemsSnapshot const& snap, std::string const& relation)
{
	for (auto const& c : snap.connections)
		if (c.key.ownerType == "Heat engine" && c.key.relation == relation)
			return &c;
	// Connection accounts are materialised by the first committed interval,
	// so the pre-step snapshot can legitimately carry none: nothing has
	// flowed, which is the same money as no ledger at all.
	return nullptr;
}

// Every rate the step publishes for one engine crossing - the machine's own
// three, both reservoirs' engine-exchange fields, the power rollup and the
// three connection legs - must be the whole-step mean of the same energy.
void expectEngineStepMeans(core::StationSystemsSnapshot const& before,
	core::StationSystemsSnapshot const& after, double stepSeconds,
	std::string const& label)
{
	auto const orc = engine(after, "orc");
	auto const was = engine(before, "orc");
	auto const hotBefore = reservoir(before, "hot");
	auto const hotAfter = reservoir(after, "hot");
	auto const sinkBefore = reservoir(before, "reject");
	auto const sinkAfter = reservoir(after, "reject");
	auto const delta = [](double now, double then) { return now - then; };
	expectStepMean(orc.lastThermalInputKW,
		delta(orc.thermalInputEnergyKJ, was.thermalInputEnergyKJ),
		stepSeconds, label + " engine thermal intake");
	expectStepMean(orc.lastElectricalOutputKW,
		delta(orc.electricalEnergyKJ, was.electricalEnergyKJ),
		stepSeconds, label + " engine electrical output");
	expectStepMean(orc.lastRejectionKW,
		delta(orc.rejectionEnergyKJ, was.rejectionEnergyKJ),
		stepSeconds, label + " engine rejection");
	expectStepMean(hotAfter.lastEngineExtractionKW,
		delta(hotAfter.engineExtractionEnergyKJ, hotBefore.engineExtractionEnergyKJ),
		stepSeconds, label + " hot reservoir engine extraction");
	expectStepMean(sinkAfter.lastEngineRejectionKW,
		delta(sinkAfter.engineRejectionEnergyKJ, sinkBefore.engineRejectionEnergyKJ),
		stepSeconds, label + " sink reservoir engine rejection");
	expectStepMean(after.power.heatEngineOutputKW,
		delta(orc.electricalEnergyKJ, was.electricalEnergyKJ),
		stepSeconds, label + " power-rollup engine output");
	expectStepMean(after.power.heatEngineRejectionKW,
		delta(orc.rejectionEnergyKJ, was.rejectionEnergyKJ),
		stepSeconds, label + " power-rollup engine rejection");
	// The connection telemetry was already duration-weighted; the machine's
	// own rates have to agree with it rather than with the last sub-interval.
	for (auto const role : {"Thermal intake", "Rejection heat", "Generation"})
	{
		auto const now = engineConnection(after, role);
		auto const then = engineConnection(before, role);
		require(now != nullptr, (label + " connection " + role
			+ " was never published").c_str());
		auto const prior = [then](double core::StationConnectionSnapshot::* field)
		{
			return then != nullptr ? then->*field : 0.0;
		};
		expectStepMean(now->lastInputRate,
			now->inputTotal - prior(&core::StationConnectionSnapshot::inputTotal),
			stepSeconds, label + " connection " + role + " input");
		expectStepMean(now->lastDeliveredRate,
			now->deliveredTotal - prior(&core::StationConnectionSnapshot::deliveredTotal),
			stepSeconds, label + " connection " + role + " delivered");
		expectStepMean(now->lastLossRate,
			now->lostTotal - prior(&core::StationConnectionSnapshot::lostTotal),
			stepSeconds, label + " connection " + role + " loss");
	}
}

// Run one split-step scenario for exactly one Station step from t = 0 and
// check the published means against the committed energy.
core::StationSystemsSnapshot runSplitStepScenario(std::string const& label,
	core::StationSystemsSettings const& settings, double stepSeconds)
{
	core::StationSystemsRuntime runtime(settings, core::World::getFixedTimestep());
	auto const before = runtime.snapshot();
	require(before.healthy, (label + ": baseline snapshot failed its audit").c_str());
	require(runtime.advanceTo(stepSeconds),
		(label + ": step failed: "
			+ (runtime.diagnostics().empty() ? "no diagnostic"
				: runtime.diagnostics().back().message)).c_str());
	auto const after = runtime.snapshot();
	require(after.healthy, (label + ": step failed its audit").c_str());
	require(after.systemsStepWorldTicks == settings.systemsStepWorldTicks
		&& std::abs(after.systemsTimestepSeconds - stepSeconds) <= 1e-12,
		(label + ": clock is not the fixture's one-second step").c_str());
	expectEngineStepMeans(before, after, stepSeconds, label);
	return after;
}
// A fixture magnitude the split step must actually produce, compared at the
// same relative scale the step-mean check uses. The half of a one-second
// step is 30 * (1.0f / 60) seconds, which is not exactly 0.5, so the
// energies it books are not round numbers either.
void expectEnergy(double actualKJ, double expectedKJ, std::string const& what)
{
	require(std::abs(actualKJ - expectedKJ)
		<= 1e-6 * std::max(1.0, std::abs(expectedKJ)),
		("#832: " + what + " booked " + std::to_string(actualKJ)
			+ " kJ, the fixture declares " + std::to_string(expectedKJ) + " kJ").c_str());
}

// The ticket's reproduction: a zero-capacity outage opening halfway through
// a one-second step. The engine ran for the first half and stood still for
// the second, so the step means are half the running rates while the
// endpoint - correctly - reports the machine out of service.
void heatEngineSplitStepOutageStartRates(smoke::Context const&)
{
	auto const quantum = static_cast<double>(core::World::getFixedTimestep());
	auto const half = 30.0 * quantum;
	auto const full = 60.0 * quantum;
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 0.0);
	settings.systemsStepWorldTicks = 60;
	settings.outages = {makeOutage("down", "orc", 0.0, half, 10.0)};
	auto const snap = runSplitStepScenario("outage-start", settings, full);
	auto const orc = engine(snap, "orc");
	// Half a second at full command: 50 kJ drawn, 4.81895 kJ delivered.
	expectEnergy(orc.thermalInputEnergyKJ, 100.0 * half, "step intake");
	expectEnergy(orc.electricalEnergyKJ, 4.81895, "step work");
	// The published rates are the whole-step means, not the isolated tail:
	// each one times the step reproduces the energy above it.
	expectEnergy(orc.lastThermalInputKW * full, 100.0 * half, "published intake rate");
	expectEnergy(orc.lastElectricalOutputKW * full, orc.electricalEnergyKJ,
		"published output rate");
	require(orc.lastRejectionKW > 0.0,
		"the rejection rate vanished with the isolated tail of the step");
	require(reservoir(snap, "hot").lastEngineExtractionKW > 0.0
			&& reservoir(snap, "reject").lastEngineRejectionKW > 0.0,
		"the reservoirs' engine-exchange rates describe only the final sub-interval");
	require(snap.power.heatEngineOutputKW > 0.0,
		"the power rollup lost the generation the first half of the step made");
	// Endpoint semantics stay exactly as they were: the machine IS out of
	// service at the step boundary, and that is not contradicted by the
	// whole-step mean rates published beside it.
	require(orc.status == core::StationHeatEngineStatus::Idle
		&& orc.lastLimiter == "OutOfService" && orc.availabilityFactor == 0.0,
		"the outage-start endpoint status/availability changed with the rates");
	require(outageSnap(snap, "down").active
		&& outageSnap(snap, "down").installationAvailability == 0.0,
		"the outage snapshot does not agree with the engine's endpoint status");
}

// Restoration inside a step: the mirror case. The engine stood still for
// the first half and ran for the second, so the step means are again half
// the running rates while the endpoint reports full service.
void heatEngineSplitStepRestorationRates(smoke::Context const&)
{
	auto const quantum = static_cast<double>(core::World::getFixedTimestep());
	auto const half = 30.0 * quantum;
	auto const full = 60.0 * quantum;
	auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 0.0);
	settings.systemsStepWorldTicks = 60;
	settings.outages = {makeOutage("down", "orc", 0.0, 0.0, half)};
	auto const snap = runSplitStepScenario("restoration", settings, full);
	auto const orc = engine(snap, "orc");
	expectEnergy(orc.thermalInputEnergyKJ, 100.0 * half, "restored step intake");
	expectEnergy(orc.lastThermalInputKW * full, orc.thermalInputEnergyKJ,
		"restored published intake rate");
	require(std::abs(orc.lastElectricalOutputKW * full - orc.electricalEnergyKJ) < 1e-6 * 5.0,
		"the restored step's output rate does not integrate to its work");
	require(reservoir(snap, "hot").lastEngineExtractionKW > 0.0
			&& reservoir(snap, "reject").lastEngineRejectionKW > 0.0,
		"the reservoirs' engine-exchange rates skipped the restored half-step");
	require(orc.status == core::StationHeatEngineStatus::Active
		&& orc.availabilityFactor == 1.0,
		"the restoration endpoint status/availability did not return to service");
	require(!outageSnap(snap, "down").active,
		"the outage snapshot still claims its window is open after restoring");
}

// Non-outage split triggers: a load-profile edge and a maintenance
// isolation edge both subdivide the step with no outage anywhere in the
// configuration, and the engine rates must still be whole-step means. The
// profile edge brings the engine up mid-step against pure demand acceptance
// (no battery); the maintenance edge takes it back out mid-step.
void heatEngineSplitStepNonOutageTriggers(smoke::Context const&)
{
	auto const quantum = static_cast<double>(core::World::getFixedTimestep());
	auto const half = 30.0 * quantum;
	auto const full = 60.0 * quantum;
	// A demand edge mid-step, with no battery: acceptance is pure demand,
	// so the engine's whole crossing starts when the profile opens.
	{
		auto settings = enginePair(20000.0, 20000.0, 100.0, 0.5, 0.0);
		settings.systemsStepWorldTicks = 60;
		settings.batteries.clear();
		core::EquipmentLoadSettings pump;
		pump.id = "pump";
		pump.thermalClass = "warm";
		pump.electricalDemandKW = pump.criticalFloorKW = 5.0;
		pump.operatingProfile.periodSeconds = 4.0 * full;
		pump.operatingProfile.segments = {{0.0, 0.0}, {half, 1.0}};
		settings.equipmentLoads = {pump};
		auto const snap = runSplitStepScenario("profile-edge", settings, full);
		auto const orc = engine(snap, "orc");
		// Half a second against a 5 kW demand: 2.5 kJ delivered, and the
		// rate that served it averaged over the whole step is half the 5 kW
		// the engine was drawing once the profile opened - so the published
		// figure is provably the step mean and not the endpoint sample.
		expectEnergy(orc.electricalEnergyKJ, 5.0 * half, "profile-edge step work");
		expectEnergy(orc.lastElectricalOutputKW * full, orc.electricalEnergyKJ,
			"profile-edge published output rate");
		// The engine drew its intake for half the step and nothing before the
		// profile opened, so the published mean sits near 2.5 kW where the
		// endpoint sample would have read the full 5 kW it was serving.
		require(orc.lastElectricalOutputKW > 1.0 && orc.lastElectricalOutputKW < 4.0,
			"the profile-edge output rate is not distinguishable from an endpoint sample");
		require(orc.lastThermalInputKW > 0.0,
			"the profile-edge step published no intake at all");
		require(reservoir(snap, "hot").lastEngineExtractionKW > 0.0
				&& reservoir(snap, "reject").lastEngineRejectionKW > 0.0,
			"the reservoirs' engine-exchange rates missed the profile-edge step");
		require(orc.status == core::StationHeatEngineStatus::Active,
			"the engine was not in service at the profile-edge endpoint");
	}
	// A maintenance isolation edge mid-step: full service, then isolated.
	{
		auto settings = engineMaintenancePair(half, 10.0);
		settings.systemsStepWorldTicks = 60;
		auto const snap = runSplitStepScenario("maintenance-edge", settings, full);
		auto const orc = engine(snap, "orc");
		expectEnergy(orc.thermalInputEnergyKJ, 100.0 * half, "maintenance-edge intake");
		expectEnergy(orc.lastThermalInputKW * full, orc.thermalInputEnergyKJ,
			"maintenance-edge published intake rate");
		require(reservoir(snap, "hot").lastEngineExtractionKW > 0.0
				&& reservoir(snap, "reject").lastEngineRejectionKW > 0.0,
			"the reservoirs' engine-exchange rates were zeroed by the isolation tail");
		require(maintenanceSnap(snap, "overhaul").phase
			== core::StationMaintenancePhase::Active,
			"the maintenance job was not isolating the engine at the endpoint");
		require(orc.status == core::StationHeatEngineStatus::Idle
			&& orc.lastLimiter == "OutOfService" && orc.availabilityFactor == 0.0,
			"the isolation endpoint status/availability did not follow the job");
	}
}

}   // namespace

void registerStationHeatEngineChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"heatEngineThermalConnections", heatEngineThermalConnections});
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
	checks.push_back({"heatEngineCurrentDemand", heatEngineCurrentDemand});
	checks.push_back({"heatEngineExergyRunToEquilibrium", heatEngineExergyRunToEquilibrium});
	checks.push_back({"heatEngineExergySweepSmoke", heatEngineExergySweepSmoke});
	checks.push_back({"heatEngineExergySweep", heatEngineExergySweep});
	checks.push_back({"heatEngineInvertedGradientIdle", heatEngineInvertedGradientIdle});
	checks.push_back({"heatEngineCadenceInvariance", heatEngineCadenceInvariance});
	// #807: the heat engine as a registered, outageable and maintainable
	// station installation.
	checks.push_back({"heatEngineInstallationRegistration", heatEngineInstallationRegistration});
	checks.push_back({"heatEngineOutageRemovesGeneration", heatEngineOutageRemovesGeneration});
	checks.push_back({"heatEngineOutagePartialCapacity", heatEngineOutagePartialCapacity});
	checks.push_back({"heatEngineMaintenanceIsolation", heatEngineMaintenanceIsolation});
	checks.push_back({"heatEngineOutageTransitionCoherence", heatEngineOutageTransitionCoherence});
	// #832: step-mean rates across an event-split step.
	checks.push_back({"heatEngineSplitStepOutageStartRates", heatEngineSplitStepOutageStartRates});
	checks.push_back({"heatEngineSplitStepRestorationRates", heatEngineSplitStepRestorationRates});
	checks.push_back({"heatEngineSplitStepNonOutageTriggers", heatEngineSplitStepNonOutageTriggers});
}
