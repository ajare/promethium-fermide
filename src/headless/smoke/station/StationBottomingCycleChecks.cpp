#include "StationRig.h"
#include "Smoke.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

// #808: the reference station's bottoming cycle. The generator's own
// waste-heat reservoir is the heat engine's hot source and a dedicated
// rejection reservoir with a radiator of its own drains the remainder to
// space, so the ORC is two components sharing plumbing rather than one
// fused compound machine. Everything here is read back through the public
// World snapshot seam - status, limiter, published kilowatts and the
// reservoir ledgers - so a different conversion scheme that published the
// same behaviour would pass, and one that published a different number
// would not.
namespace
{
using namespace smoke::station;

// The demo run's operating state.
//
// The reference's nominal scenario keeps the generator commanded off,
// which leaves genSink at 20 C and the cycle idle: there is no waste heat
// to recover. The demo therefore puts the station into the state the
// cycle exists for - the generator carrying an array that has fallen into
// partial shadow - and starts genSink hot, at the temperature hours of
// generator running would have left it. That starting temperature is
// declared rather than earned: the generator's 20000 kJ/K sink is far too
// large for 15 kW of waste heat to warm on any demo-length timescale
// (about 700 simulated seconds per kelvin), so spending ten simulated
// hours to arrive at 90 C would buy no additional evidence. What the demo
// demonstrates is what the cycle does with the heat once it is there,
// which is the question the ticket actually asks.
//
// The array is shadowed rather than left in full sun for a reason too: the
// acceptance-coupled throttle (#805) admits the engine's power only
// against demand the station presented plus charge headroom the batteries
// can still take. With the array at full output the station has no room
// for the recovered kilowatts and the engine would idle on
// DemandAcceptance, which is a story about the bus, not about heat.
core::StationSystemsSettings demoSettings()
{
	auto settings = core::bundledStationReference().settings;
	settings.solarArrays[0].illumination.constantLevel = 0.25;
	settings.generators[0].commandedOutputKW = settings.generators[0].ratedOutputKW;
	for (auto& reservoir : settings.thermalReservoirs)
	{
		if (reservoir.id == "genSink") reservoir.startingTemperatureC = 89.5;
		if (reservoir.id == "engineReject") reservoir.startingTemperatureC = 5.0;
	}
	// The scheduled outage and maintenance windows are cleared. The demo
	// is about the machines' own choreography, and a calendar window that
	// pulled the generator out of service mid-run would show an operator
	// removal where the ticket asks to see a trip.
	settings.outages.clear();
	settings.maintenanceJobs.clear();
	return settings;
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

core::StationThermalReservoirSnapshot reservoir(
	core::StationSystemsSnapshot const& snap, std::string const& id)
{
	for (auto const& entry : snap.thermal.reservoirs)
		if (entry.id == id) return entry;
	throw std::runtime_error("thermal reservoir missing from snapshot");
}

core::StationGeneratorSnapshot generator(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.power.generators)
		if (entry.id == id) return entry;
	throw std::runtime_error("generator missing from snapshot");
}

// One systems step of the demo, sampled whole: the engine's own report,
// the generator's cooling state, and both reservoirs' temperatures. The
// choreography is a statement about the order these arrive in, so the
// whole trace is kept rather than a final snapshot.
struct Step
{
	double elapsedSeconds{};
	double stepSeconds{};
	core::StationHeatEngineSnapshot engine;
	core::StationGeneratorSnapshot generator;
	core::StationThermalReservoirSnapshot hot;
	core::StationThermalReservoirSnapshot reject;
};

// The sink gates in this model are pre-commit qualifications (#771): a
// machine is refused before its reservoir actually touches the limit, by
// the gross heating the protection prediction still carries for the
// interval. So "this reservoir reached its gate" has to allow exactly
// that one-step lead - and nothing more, which is what makes the
// assertion still mean something.
double gateLeadC(double grossHeatingKW, double capacityKJPerK, double stepSeconds)
{
	return std::max(0.0, grossHeatingKW) * stepSeconds / capacityKJPerK + 1e-6;
}

// ---------------------------------------------------------------------------
// The cycle waits for waste heat (#808, AC6).
//
// As authored and run normally, the generator never starts, so the
// bottoming cycle must be visibly inert rather than subtly active: it
// draws nothing from a cold genSink, books no rejection, and contributes
// nothing to the power balance. An engine that quietly produced from a
// 20 C sink would be the worst possible failure of this configuration,
// because it would read as free power.
// ---------------------------------------------------------------------------
void referenceBottomingCycleIdle(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ReferenceBottomingIdle", 2, 2);
	configure(world, core::bundledStationReference().settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 120.0), "the idle reference run failed");
	auto const snap = world.getStationSystemsSnapshot();
	require(snap.healthy, "the idle reference run failed its audit");
	auto const orc = engine(snap, "orcBottoming");
	require(orc.status == core::StationHeatEngineStatus::Idle
		&& orc.lastLimiter == "TooCold",
		"the bottoming cycle did not idle TooCold against the cold generator sink");
	require(orc.lastThermalInputKW == 0.0 && orc.lastElectricalOutputKW == 0.0
		&& orc.lastRejectionKW == 0.0 && orc.thermalInputEnergyKJ == 0.0
		&& orc.electricalEnergyKJ == 0.0 && orc.rejectionEnergyKJ == 0.0,
		"an idle bottoming cycle drew heat, booked rejection or generated power anyway");
	require(snap.power.heatEngineOutputKW == 0.0
		&& snap.power.heatEngineRejectionKW == 0.0,
		"an idle bottoming cycle still showed up in the station power totals");
	// The generator sink and the rejection buffer are untouched by the
	// engine: the only thing that moved the buffer is its own radiator.
	require(reservoir(snap, "genSink").engineExtractionEnergyKJ == 0.0,
		"the generator sink lost heat to an engine that was not running");
	require(reservoir(snap, "engineReject").engineRejectionEnergyKJ == 0.0,
		"the rejection reservoir took rejection heat from an engine that was not running");
}

// ---------------------------------------------------------------------------
// The demo run (#808, AC2 and AC3).
//
// Recovery kilowatts arrive on the bus and are attributable to the
// engine; the engine throttles as the gradient shrinks; the engine's
// sink trip hands the heat back to the generator's own cooling gate,
// which then closes; and the energy across the joined set - hot
// reservoir, rejection reservoir, bus - closes over the whole run.
// ---------------------------------------------------------------------------
void referenceBottomingCycle(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ReferenceBottomingCycle", 2, 2);
	configure(world, demoSettings(), diagnostic);
	world.resumeSimulation();

	std::vector<Step> trace;
	constexpr int steps = 12'500;
	for (int step = 0; step < steps; ++step)
	{
		require(advanceStationSteps(world, 1), "the bottoming demo run failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "the bottoming demo run failed its audit");
		trace.push_back({snap.elapsedSeconds, snap.systemsTimestepSeconds,
			engine(snap, "orcBottoming"),
			generator(snap, "dieselGen"), reservoir(snap, "genSink"),
			reservoir(snap, "engineReject")});
		// Stop sampling once both gates have closed and the generator has
		// finished its decay-heat window; nothing new is learned after.
		auto const& orc = trace.back().engine;
		auto const& gen = trace.back().generator;
		if (orc.status == core::StationHeatEngineStatus::Tripped
			&& !gen.coolingAvailable && gen.decayHeatKW == 0.0
			&& trace.size() > 1'100) break;
	}

	// --- the engine ran, and the bus got its power -------------------
	auto const firstActive = std::find_if(trace.begin(), trace.end(),
		[](Step const& s) { return s.engine.status == core::StationHeatEngineStatus::Active; });
	require(firstActive != trace.end(),
		"the bottoming cycle never ran on the hot generator sink");
	auto const trip = std::find_if(trace.begin(), trace.end(),
		[](Step const& s) { return s.engine.status == core::StationHeatEngineStatus::Tripped; });
	require(trip != trace.end(),
		"the rejection buffer never saturated and the engine never tripped");
	require(trip > firstActive,
		"the engine tripped before it ever converted");
	auto const tripOrc = trip->engine;
	require(tripOrc.lastLimiter == "SinkOver",
		"the engine did not trip on its saturated rejection path");
	auto const tripLead = trip - 1;
	require(trip->reject.temperatureC
		+ gateLeadC(tripLead->reject.lastEngineRejectionKW,
			tripLead->reject.heatCapacityKJPerK, trip->stepSeconds)
		>= tripOrc.maxSinkTemperatureC - 1e-9,
		"the engine tripped as SinkOver while its rejection reservoir was short of the max-sink gate");

	// Recovery is real and attributable: the station-level engine output
	// is the engine's own published output, and it sits inside the pool
	// total the station is running on.
	auto const peak = std::max_element(firstActive, trip,
		[](Step const& a, Step const& b)
		{ return a.engine.lastElectricalOutputKW < b.engine.lastElectricalOutputKW; });
	require(peak->engine.lastElectricalOutputKW >= 3.0,
		"the bottoming cycle delivered no recovery kilowatts to the bus");
	require(peak->engine.status == core::StationHeatEngineStatus::Active
		&& std::abs(peak->engine.lastAuxKW - 0.4) < 1e-12,
		"the peak recovery did not come from a running engine carrying its auxiliaries");
	require(peak->engine.lastThermalInputKW > peak->engine.lastElectricalOutputKW,
		"the recovery exceeded the heat it was converted from");
	// The engine's efficiency is the authored fraction of Carnot at the
	// interface temperatures and never above it: the recovered power is
	// bounded at the conversion site, not merely reported.
	require(peak->engine.efficiencyNow > 0.0
		&& peak->engine.efficiencyNow <= peak->engine.carnotEfficiencyNow + 1e-12,
		"the bottoming cycle beat Carnot at its own interface temperatures");
	require(std::abs(peak->engine.efficiencyNow
		- 0.5 * peak->engine.carnotEfficiencyNow) < 1e-9,
		"the bottoming cycle's efficiency is not the authored carnotFraction of Carnot");

	// --- the engine throttles as the gradient shrinks ----------------
	// The rejection buffer is a saturating buffer, so the cold side climbs
	// while the hot side barely moves: the Carnot window closes from
	// below and the same intake yields less and less power. Compare the
	// first third of the converting run against the last third; the
	// envelope must fall by a clear margin, not by rounding.
	auto const span = static_cast<size_t>(std::distance(firstActive, trip));
	require(span > 600, "the converting run is too short to show a throttle");
	auto const firstThirdEnd = firstActive + span / 3;
	auto const lastThirdBegin = trip - span / 3;
	auto const earlyMin = std::min_element(firstActive, firstThirdEnd,
		[](Step const& a, Step const& b)
		{ return a.engine.lastElectricalOutputKW < b.engine.lastElectricalOutputKW; });
	auto const lateMax = std::max_element(lastThirdBegin, trip,
		[](Step const& a, Step const& b)
		{ return a.engine.lastElectricalOutputKW < b.engine.lastElectricalOutputKW; });
	require(lateMax->engine.lastElectricalOutputKW
		<= 0.9 * earlyMin->engine.lastElectricalOutputKW,
		"the engine output did not throttle down as the gradient shrank");
	require(lateMax->engine.carnotEfficiencyNow < earlyMin->engine.carnotEfficiencyNow - 1e-6,
		"the Carnot window did not close over the converting run");

	// --- the sink trip hands the heat back to the generator's gate ---
	// While the engine ran it was draining the generator's sink, so the
	// generator's own cooling stayed qualified even at 89 C. The engine's
	// trip removes that drain, and the generator's waste heat - with only
	// its 0.5 m2 panel against it - then closes the generator's own gate.
	// The order is the whole point: the engine goes down first, and the
	// generator's gate is still open when it does.
	require(trip->generator.coolingAvailable,
		"the generator's cooling gate had already closed when the engine tripped");
	require(trip->hot.temperatureC < trip->generator.maxSinkTemperatureC - 1e-9,
		"the generator sink was already at its gate at the engine's trip");
	auto const gate = std::find_if(trip, trace.end(),
		[](Step const& s) { return !s.generator.coolingAvailable; });
	require(gate != trace.end(),
		"the generator's cooling gate never closed once the engine stopped draining its sink");
	auto const gateLead = gate - 1;
	require(gate->hot.temperatureC
		+ gateLeadC(gateLead->hot.lastGeneratorHeatKW,
			gateLead->hot.heatCapacityKJPerK, gate->stepSeconds)
		>= gate->generator.maxSinkTemperatureC - 1e-9,
		"the generator's cooling gate closed without its sink reaching the maximum");
	require(gate->generator.phase == core::StationGeneratorPhase::Shutdown
		|| gate->generator.phase == core::StationGeneratorPhase::Stopped,
		"the generator kept running after its cooling gate closed");
	// Every step of the converting phase held the generator's cooling
	// alive: the engine was the sink's relief, right up to its own trip.
	require(std::all_of(firstActive, trip,
		[](Step const& s) { return s.generator.coolingAvailable; }),
		"the generator lost cooling while the bottoming cycle was still converting");
	// And the generator's output is gone at the end of the run: the
	// waste-heat source itself is what the closed gate took away.
	require(trace.back().generator.averageOutputKW == 0.0
		&& trace.back().generator.outputKW == 0.0,
		"the generator was still producing after its cooling gate closed");

	// --- the joined set closes --------------------------------------
	// Heat enters the joined set only as waste heat booked into the hot
	// reservoir. It leaves as four things: heat still standing in the hot
	// reservoir, heat still standing in the rejection reservoir, heat
	// radiated from each of the two panels, and electrical energy on the
	// bus. Nothing else touches either reservoir, so the identity is
	// exact rather than approximate - and it closes over the whole run,
	// past the trip and through the short-cycling chatter, not just to
	// the last converting step. The chatter is where it must close: the
	// engine restarts each time the panel pulls the rejection reservoir
	// under the max-sink gate and trips again a step later, so the
	// engine's books, the reservoirs' books and the bus must stay one
	// story through every restart. They do - because the ledger treats
	// the heat engine transactionally like every other machine: a
	// discarded prediction pass restores the engine's cumulative
	// counters along with the reservoirs, so no uncommitted conversion
	// survives in any account.
	auto const& closing = trace.back();
	auto const& hot = closing.hot;
	auto const& reject = closing.reject;
	auto const& orc = closing.engine;
	auto const heatInKJ = hot.generatorEnergyKJ + hot.electrolysisEnergyKJ + hot.fuelCellEnergyKJ;
	auto const hotDeltaKJ = hot.currentEnergyKJ - hot.startingEnergyKJ;
	auto const rejectDeltaKJ = reject.currentEnergyKJ - reject.startingEnergyKJ;
	auto const joinedOutKJ = hotDeltaKJ + rejectDeltaKJ
		+ hot.rejectedEnergyKJ + reject.rejectedEnergyKJ + orc.electricalEnergyKJ;
	require(std::abs(heatInKJ - joinedOutKJ)
		<= 1e-6 * std::max({1.0, std::abs(heatInKJ), std::abs(joinedOutKJ)}),
		"energy across the hot reservoir, rejection reservoir and bus did not close");
	// The engine's own ledger closes over the same numbers, and the
	// reservoirs booked exactly what it says it drew and rejected, so the
	// three accounts agree on one quantity rather than each balancing
	// itself against its own books.
	require(std::abs(orc.thermalInputEnergyKJ
		- (orc.electricalEnergyKJ + orc.rejectionEnergyKJ))
		<= 1e-6 * std::max(1.0, orc.thermalInputEnergyKJ),
		"the engine ledger did not close over intake = electrical + rejection");
	require(hot.engineExtractionEnergyKJ == orc.thermalInputEnergyKJ
		&& reject.engineRejectionEnergyKJ == orc.rejectionEnergyKJ,
		"the reservoir engine bookings disagree with the engine's own ledger");
	// The closure holds tick by tick across the whole run, not just in
	// the cumulative total: every step - through the converting phase,
	// the trip, and the short-cycling tail - balances on the same
	// identity, so no single step is hiding an error that a later one
	// cancels.
	for (auto it = trace.begin() + 1; it != trace.end(); ++it)
	{
		auto const stepHeatIn = (it->hot.generatorEnergyKJ - (it - 1)->hot.generatorEnergyKJ)
			+ (it->hot.electrolysisEnergyKJ - (it - 1)->hot.electrolysisEnergyKJ)
			+ (it->hot.fuelCellEnergyKJ - (it - 1)->hot.fuelCellEnergyKJ);
		auto const stepOut = (it->hot.currentEnergyKJ - (it - 1)->hot.currentEnergyKJ)
			+ (it->reject.currentEnergyKJ - (it - 1)->reject.currentEnergyKJ)
			+ (it->hot.rejectedEnergyKJ - (it - 1)->hot.rejectedEnergyKJ)
			+ (it->reject.rejectedEnergyKJ - (it - 1)->reject.rejectedEnergyKJ)
			+ (it->engine.electricalEnergyKJ - (it - 1)->engine.electricalEnergyKJ);
		require(std::abs(stepHeatIn - stepOut)
			<= 1e-6 * std::max(1.0, std::abs(stepHeatIn)),
			"the joined set did not close on a step inside the converting run");
	}
	// The pre-existing per-reservoir residuals still close with the
	// bottoming terms in the formula - nothing was weakened to fit.
	require(std::abs(hot.residualKJ) <= 1e-6 * std::max(1.0, std::abs(hot.startingEnergyKJ))
		&& std::abs(reject.residualKJ) <= 1e-6 * std::max(1.0, std::abs(reject.startingEnergyKJ)),
		"a reservoir residual failed to close with the bottoming cycle present");
	// The station kept its service through the whole choreography: the
	// battery bridged the generator's removal, nothing went unmet.
	auto const& finalSnap = world.getStationSystemsSnapshot();
	require(finalSnap.power.unmetCriticalKW <= 1e-9,
		"the bottoming demo left unmet critical demand behind it");
}
} // namespace

void registerStationBottomingCycleChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"referenceBottomingCycleIdle", referenceBottomingCycleIdle});
	checks.push_back({"referenceBottomingCycle", referenceBottomingCycle});
}
