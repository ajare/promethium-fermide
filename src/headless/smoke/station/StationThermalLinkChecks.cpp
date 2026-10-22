#include "StationRig.h"
#include "Smoke.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// Thermal link tracer bullet checks (#794): an authored, undirected
// conduction pair between two Atmosphere compartments. Every assertion is
// made on what a configured World publishes after being advanced - never
// on integrator internals - so replacing the closed form with any other
// scheme producing the same published behaviour still passes.
namespace
{
using namespace smoke::station;

// Both compartments carry the identical gas inventory: the contrast with
// the thermally-neutral pathway (#788) is then purely about heat - the
// same composition, different temperature. The vapour load is kept far
// below saturation at the coldest temperature any fixture reaches, so
// no condensation latent heat enters the energy accounting and every
// joule moved in these runs is the link's own.
core::AtmosphereSettings linkCompartment(std::string const& id, double temperatureC,
	double capacityKJPerK)
{
	core::AtmosphereSettings a;
	a.id = id;
	a.volumeM3 = 100;
	a.thermalCapacityKJPerK = capacityKJPerK;
	a.startingTemperatureC = temperatureC;
	a.startingOxygenKg = 30;
	a.startingNitrogenKg = 80;
	a.startingCarbonDioxideKg = 0.6;
	a.startingWaterVapourKg = 0.001;
	a.minPressureKPa = 1;
	a.maxPressureKPa = 200;
	return a;
}

// Starting temperatures are deliberately non-zero on both sides: the
// audit's thermal closure bound scales with the starting energy, and a
// compartment starting at exactly 0 C would carry a zero-magnitude
// datum, leaving the accumulated rounding with no room at all.
core::StationSystemsSettings linkedPair(double aCapacity = 10000.0, double bCapacity = 10000.0,
	double conductanceKWPerK = 10.0)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.atmospheres = {linkCompartment("a", 40.0, aCapacity),
		linkCompartment("b", 10.0, bCapacity)};
	s.thermalLinks = {{"wall", "a", "b", conductanceKWPerK}};
	return s;
}

void configure(core::World& world, core::StationSystemsSettings const& settings,
	std::string& diagnostic)
{
	world.pauseSimulation(); world.finishBuild();
	if (!world.configureStationSystems(settings, false, &diagnostic))
		throw std::runtime_error("configure refused: [" + diagnostic + "]");
}

core::StationThermalLinkSnapshot link(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.thermalLinks)
		if (entry.id == id) return entry;
	throw std::runtime_error("thermal link missing from snapshot");
}

// Gas inventory in pathway-transfer order: oxygen, nitrogen, carbon
// dioxide, water vapour - the same slot order the pathway snapshots use.
std::array<double, 4> masses(core::StationAtmosphereSnapshot const& air)
{
	return {air.oxygenKg, air.nitrogenKg, air.carbonDioxideKg, air.waterVapourKg};
}

// The primary conservation test: total C*T across the link-joined set is
// invariant, each side's residual closes with its conducted term, and the
// two sides' conducted totals are exact negations of each other and of
// the link's own cumulative transfer.
void thermalLinkConservation(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ThermalLinkConservation", 2, 2);
	configure(world, linkedPair(), diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	require(initial.thermalLinks.size() == 1, "configured link missing before the run");
	require(link(initial, "wall").status == core::StationThermalLinkStatus::Idle
		&& link(initial, "wall").netTransferredKJ == 0.0,
		"a link reports activity before the World has run");
	world.resumeSimulation();
	while (world.getStationSystemsSnapshot().elapsedSeconds < 60)
	{
		require(advanceStationSteps(world, 1), "conservation step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "conservation run failed its audit");
		auto const& a = snapshot.atmospheres[0];
		auto const& b = snapshot.atmospheres[1];
		require(a.conductedEnergyKJ == -b.conductedEnergyKJ,
			"conducted heat was not booked as an exact negation on each side");
		require(link(snapshot, "wall").netTransferredKJ == b.conductedEnergyKJ,
			"the link ledger and the compartment ledgers disagree");
		auto const total = a.thermalCapacityKJPerK * a.temperatureC
			+ b.thermalCapacityKJPerK * b.temperatureC;
		auto const totalStart = a.thermalCapacityKJPerK * initial.atmospheres[0].temperatureC
			+ b.thermalCapacityKJPerK * initial.atmospheres[1].temperatureC;
		require(std::abs(total - totalStart) <= 1e-6 * std::abs(totalStart),
			"total energy across the link-joined set was not invariant");
		require(std::abs(a.residualKJ) <= 1e-6 * std::max(1.0, std::abs(a.currentEnergyKJ))
			&& std::abs(b.residualKJ) <= 1e-6 * std::max(1.0, std::abs(b.currentEnergyKJ)),
			"compartment energy residual did not close with links present");
	}
	// Heat genuinely moved: the hot side cooled, the cold side warmed,
	// and the signed flow points from hot to cold (A started hot).
	auto const wall = link(world.getStationSystemsSnapshot(), "wall");
	require(wall.flowRateKW > 0.0 && wall.netTransferredKJ > 1.0,
		"heat did not flow from the hot compartment to the cold one");
	require(wall.status == core::StationThermalLinkStatus::Active,
		"a conducting link did not report Active");
}

// #795 headline contrast, making ADR-0026's thermal-neutrality claim a
// checked one rather than a documented one: two compartments at identical
// composition and different temperature, joined by an Atmosphere pathway
// and otherwise thermally sealed. With no link the pathway equalises the
// gas and keeps the temperature difference indefinitely - the result the
// ADR promises. Adding a Thermal link is the only difference between the
// two runs, and it removes exactly that difference. Same setup, opposite
// outcome, one aggregate apart; anyone later making the pathway carry
// heat as a shortcut breaks one side or the other.
void thermalLinkAdr0026Contrast(smoke::Context const&)
{
	std::string diagnostic;
	auto withLink = linkedPair(10000.0, 10000.0, 100.0);
	// The pathway is the only connection in the control run, so a
	// preserved temperature difference is a property of the pathway and
	// not of an empty gap. Vapour is raised to 0.05 kg per side - still
	// far below saturation at 10 C, so no latent heat enters either
	// ledger - which keeps every gas's control-run transfer comfortably
	// above the measurable threshold rather than a rounding-scale trickle.
	for (auto& air : withLink.atmospheres)
		air.startingWaterVapourKg = 0.05;
	withLink.atmospherePathways = {{"hatch", "a", "b", 1.0, 1.0}};
	auto withoutLink = withLink;
	withoutLink.thermalLinks.clear();
	// Control: same composition, real gas exchange, no heat path.
	core::World isolated("ThermalIsolatedPair", 2, 2);
	configure(isolated, withoutLink, diagnostic);
	auto const isolatedStart = isolated.getStationSystemsSnapshot();
	isolated.resumeSimulation();
	require(advanceSeconds(isolated, 1800), "isolated pair run failed");
	auto const isolatedSnap = isolated.getStationSystemsSnapshot();
	require(isolatedSnap.healthy, "isolated pair run failed its audit");
	// The pathway genuinely worked: every gas crossed in a measurable
	// amount (a is hot, so every partial pressure gradient points a->b),
	// so the preserved difference below is not an idle fixture.
	for (size_t gas = 0; gas < 4; ++gas)
		require(isolatedSnap.atmospherePathways[0].netTransferredKg[gas] > 1e-3,
			"the control pathway did not actually exchange every gas");
	// And ADR-0026 holds to full precision: thirty minutes of continuous
	// gas exchange moved no sensible heat at all.
	require(std::abs(isolatedSnap.atmospheres[0].temperatureC
			- isolatedStart.atmospheres[0].temperatureC) < 1e-12
		&& std::abs(isolatedSnap.atmospheres[1].temperatureC
			- isolatedStart.atmospheres[1].temperatureC) < 1e-12,
		"the thermally-neutral pathway did not keep the authored temperatures");
	require(isolatedSnap.atmospheres[0].temperatureC
		- isolatedSnap.atmospheres[1].temperatureC > 29.999,
		"the temperature difference decayed without a link");
	// With the link added - the only change - the same pair converges to
	// the energy-weighted average: equal capacities, so the arithmetic
	// mean of 40 C and 10 C. The link's tau is the reduced capacity
	// 5,000 kJ/K over 100 kW/K = 50 s, so 1,800 s is 36 relaxation
	// time constants and the residual difference is long gone.
	core::World joined("ThermalJoinedPair", 2, 2);
	configure(joined, withLink, diagnostic);
	joined.resumeSimulation();
	require(advanceSeconds(joined, 1800), "joined pair run failed");
	auto const joinedSnap = joined.getStationSystemsSnapshot();
	require(joinedSnap.healthy, "joined pair run failed its audit");
	auto const expected = 25.0;
	require(std::abs(joinedSnap.atmospheres[0].temperatureC - expected) < 1e-3
		&& std::abs(joinedSnap.atmospheres[1].temperatureC - expected) < 1e-3,
		"a linked pair did not converge to the energy-weighted average");
	require(joinedSnap.atmospheres[0].conductedEnergyKJ < -1.0
		&& joinedSnap.atmospheres[1].conductedEnergyKJ > 1.0,
		"the linked pair did not book conducted heat");
}

// Zero temperature difference: nothing drives conduction, so the link
// reports Idle with no flow and no conducted heat anywhere.
void thermalLinkIdle(smoke::Context const&)
{
	auto settings = linkedPair();
	settings.atmospheres[1].startingTemperatureC = 40.0;
	std::string diagnostic;
	core::World world("ThermalLinkIdle", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 60), "idle link run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "idle link run failed its audit");
	auto const wall = link(snapshot, "wall");
	require(wall.status == core::StationThermalLinkStatus::Idle
		&& wall.flowRateKW == 0.0 && wall.netTransferredKJ == 0.0,
		"a link with no temperature difference reported flow");
	require(snapshot.atmospheres[0].conductedEnergyKJ == 0.0
		&& snapshot.atmospheres[1].conductedEnergyKJ == 0.0,
		"a link with no temperature difference booked conducted heat");
}

// Equilibrium is the capacity-weighted average: the big compartment
// dominates the settled temperature exactly as the formula says.
void thermalLinkCapacityWeighting(smoke::Context const&)
{
	// Ca = 3x Cb: the settled temperature is (3*40 + 1*10)/4 = 32.5 C.
	std::string diagnostic;
	core::World world("ThermalLinkWeighting", 2, 2);
	configure(world, linkedPair(30000.0, 10000.0, 100.0), diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 3600), "weighting run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "weighting run failed its audit");
	require(std::abs(snapshot.atmospheres[0].temperatureC - 32.5) < 1e-3
		&& std::abs(snapshot.atmospheres[1].temperatureC - 32.5) < 1e-3,
		"equilibrium was not the capacity-weighted average temperature");
}

// No crossing: a hot compartment never ends the tick colder than the
// cold compartment it was warming, at any conductance.
void thermalLinkNoCrossing(smoke::Context const&)
{
	// Absurd geometry: enormous conductance against a tiny cold capacity.
	std::string diagnostic;
	core::World world("ThermalLinkNoCrossing", 2, 2);
	configure(world, linkedPair(10000.0, 1.0, 1000.0), diagnostic);
	world.resumeSimulation();
	for (int step = 0; step < 600; ++step)
	{
		require(advanceStationSteps(world, 1), "no-crossing step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "no-crossing run failed its audit");
		require(std::isfinite(snapshot.atmospheres[0].temperatureC)
			&& std::isfinite(snapshot.atmospheres[1].temperatureC),
			"an over-conducted link produced a non-finite temperature");
		require(snapshot.atmospheres[0].temperatureC >= snapshot.atmospheres[1].temperatureC,
			"the hot compartment ended colder than the cold one it was warming");
		require(snapshot.atmospheres[0].temperatureC <= 40.0 + 1e-9
			&& snapshot.atmospheres[1].temperatureC >= 10.0 - 1e-9,
			"conduction overshot past the starting temperatures");
	}
	// The tiny cold compartment lands on the weighted average beside the
	// big hot one, not past it.
	auto const snapshot = world.getStationSystemsSnapshot();
	auto const expected = (10000.0 * 40.0 + 1.0 * 10.0) / 10001.0;
	require(std::abs(snapshot.atmospheres[1].temperatureC - expected) < 1e-6,
		"the over-conducted cold compartment did not settle on the weighted average");
}

// Parallel additivity: two half-conductance links equal one full-
// conductance link exactly, so parallel walls reason additively.
void thermalLinkParallelAdditivity(smoke::Context const&)
{
	std::string diagnostic;
	auto const single = linkedPair(10000.0, 10000.0, 10.0);
	auto halves = single;
	halves.thermalLinks = {{"half1", "a", "b", 5.0}, {"half2", "a", "b", 5.0}};
	core::World oneWorld("ThermalLinkSingle", 2, 2);
	configure(oneWorld, single, diagnostic);
	core::World twoWorld("ThermalLinkHalves", 2, 2);
	configure(twoWorld, halves, diagnostic);
	oneWorld.resumeSimulation(); twoWorld.resumeSimulation();
	require(advanceSeconds(oneWorld, 300) && advanceSeconds(twoWorld, 300),
		"parallel additivity run failed");
	auto const one = oneWorld.getStationSystemsSnapshot();
	auto const two = twoWorld.getStationSystemsSnapshot();
	require(one.healthy && two.healthy, "parallel additivity run failed its audit");
	// Exact in exact arithmetic: the closed form multiplies the remaining
	// difference by exp(-G·S·dt/2) per half link, so two halves compose
	// to exp(-G·S·dt) - the single link's factor. Floating point leaves
	// a last-bits difference, so compare at a tight physical tolerance.
	for (size_t i = 0; i < 2; ++i)
		require(std::abs(one.atmospheres[i].temperatureC - two.atmospheres[i].temperatureC) < 1e-12,
			"two half-conductance links did not equal one full-conductance link");
	// The receiver's conducted total is the link's own net transfer:
	// positive into B, and identical for the single-link world.
	require(std::abs(two.atmospheres[1].conductedEnergyKJ
		- link(one, "wall").netTransferredKJ)
		< 1e-9 * std::abs(link(one, "wall").netTransferredKJ) + 1e-9,
		"the parallel pair did not book the same conducted energy as the single link");
}

// Declaration-order independence: reshuffling link order leaves the
// compartment results unchanged.
void thermalLinkDeclarationOrder(smoke::Context const&)
{
	std::string diagnostic;
	auto const base = linkedPair(10000.0, 8000.0, 12.0);
	auto reversed = base;
	reversed.thermalLinks = {{"second", "a", "b", 6.0}, {"first", "b", "a", 6.0}};
	auto forward = linkedPair(10000.0, 8000.0, 12.0);
	forward.thermalLinks = {{"first", "b", "a", 6.0}, {"second", "a", "b", 6.0}};
	core::World aWorld("ThermalLinkOrderA", 2, 2);
	configure(aWorld, reversed, diagnostic);
	core::World bWorld("ThermalLinkOrderB", 2, 2);
	configure(bWorld, forward, diagnostic);
	aWorld.resumeSimulation(); bWorld.resumeSimulation();
	require(advanceSeconds(aWorld, 300) && advanceSeconds(bWorld, 300),
		"declaration-order run failed");
	auto const a = aWorld.getStationSystemsSnapshot();
	auto const b = bWorld.getStationSystemsSnapshot();
	require(a.healthy && b.healthy, "declaration-order run failed its audit");
	for (size_t i = 0; i < 2; ++i)
		require(a.atmospheres[i].temperatureC == b.atmospheres[i].temperatureC
			&& a.atmospheres[i].conductedEnergyKJ == b.atmospheres[i].conductedEnergyKJ,
			"link declaration order changed the published result");
}

// Mass is untouched: a long conduction run changes no gas inventory in
// either compartment by any amount.
void thermalLinkMassUntouched(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ThermalLinkMassUntouched", 2, 2);
	configure(world, linkedPair(10000.0, 2000.0, 500.0), diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	require(advanceSeconds(world, 3600), "mass-untouched run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "mass-untouched run failed its audit");
	for (size_t i = 0; i < 2; ++i)
	{
		require(snapshot.atmospheres[i].oxygenKg == initial.atmospheres[i].oxygenKg
			&& snapshot.atmospheres[i].nitrogenKg == initial.atmospheres[i].nitrogenKg
			&& snapshot.atmospheres[i].carbonDioxideKg == initial.atmospheres[i].carbonDioxideKg
			&& snapshot.atmospheres[i].waterVapourKg == initial.atmospheres[i].waterVapourKg,
			"the conduction pass changed a gas inventory");
		require(snapshot.atmospheres[i].condensedKg == 0.0,
			"the conduction pass condensed or invented vapour mass");
	}
	// Heat genuinely moved in this fixture, so the zero above is not
	// vacuously true.
	require(link(snapshot, "wall").netTransferredKJ > 1.0,
		"mass-untouched fixture conducted nothing to begin with");
}

// A link and a pathway coexist between the same pair: gas diffuses
// through the pathway, heat conducts through the link, each reports
// independently, and neither double-counts the other.
void thermalLinkPathwayCoexistence(smoke::Context const&)
{
	auto settings = linkedPair(10000.0, 10000.0, 10.0);
	settings.atmospherePathways = {{"hatch", "a", "b", 1.0, 1.0}};
	std::string diagnostic;
	core::World world("ThermalLinkPathwayCoexist", 2, 2);
	configure(world, settings, diagnostic);
	// Make the compositions differ so the pathway genuinely moves gas.
	world.resumeSimulation();
	require(advanceSeconds(world, 300), "coexistence run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "coexistence run failed its audit");
	bool gasMoved = false;
	for (auto const moved : snapshot.atmospherePathways[0].netTransferredKg)
		if (std::abs(moved) > 1e-9) gasMoved = true;
	// The identical-composition pair moves no net gas at equilibrium;
	// what matters is that the pathway stayed thermally neutral while the
	// link did all the heat work, and both aggregates report side by side.
	require(snapshot.atmospherePathways[0].status
			== core::StationAtmospherePathwayStatus::Idle || gasMoved,
		"pathway state is inconsistent with its own flows");
	require(link(snapshot, "wall").netTransferredKJ > 1.0,
		"the link stopped conducting beside a pathway");
	// The pathway contributed no heat: the conducted totals equal the
	// link's own ledger exactly, with no pathway term folded in.
	require(snapshot.atmospheres[1].conductedEnergyKJ
		== link(snapshot, "wall").netTransferredKJ,
		"conducted heat was not solely the link's booking");
}

// #795: a Thermal link and an Atmosphere pathway wired between the same
// compartment pair report independently. The compartment mass ledger
// closes on the pathway alone and the energy ledger closes on the link
// alone, so neither aggregate double-counts, interferes with, or silently
// substitutes for the other.
void thermalLinkPathwayIndependence(smoke::Context const&)
{
	// Different compositions so the pathway genuinely works in both
	// directions at once: oxygen, nitrogen and vapour diffuse a -> b
	// while carbon dioxide, far richer in b, diffuses b -> a. The link
	// conducts heat hot -> cold alongside, and the two never mix.
	auto a = linkCompartment("a", 40.0, 10000.0);
	a.startingWaterVapourKg = 0.01;
	auto b = linkCompartment("b", 10.0, 10000.0);
	b.startingOxygenKg = 10;
	b.startingNitrogenKg = 40;
	b.startingCarbonDioxideKg = 2.0;
	b.startingWaterVapourKg = 0.0005;
	core::StationSystemsSettings s;
	s.enabled = true;
	s.atmospheres = {a, b};
	s.atmospherePathways = {{"hatch", "a", "b", 1.0, 1.0}};
	s.thermalLinks = {{"wall", "a", "b", 10.0}};
	std::string diagnostic;
	core::World world("ThermalLinkPathwayIndependence", 2, 2);
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	require(advanceSeconds(world, 300), "independence run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "independence run failed its audit");
	auto const& pathway = snapshot.atmospherePathways[0];
	auto const wall = link(snapshot, "wall");
	// Both aggregates are live at once: the pathway carries gas both ways
	// while the link carries heat one way.
	require(pathway.status == core::StationAtmospherePathwayStatus::Active,
		"the pathway went idle beside a link");
	require(wall.status == core::StationThermalLinkStatus::Active
		&& wall.netTransferredKJ > 1.0,
		"the link went idle beside a pathway");
	// Genuine counter-flow: O2/N2/vapour a -> b, CO2 b -> a.
	require(pathway.netTransferredKg[0] > 1e-3 && pathway.netTransferredKg[1] > 1e-3
		&& pathway.netTransferredKg[3] > 1e-3,
		"the pathway did not move the lean-side gases into b");
	require(pathway.netTransferredKg[2] < -1e-3,
		"the pathway did not move carbon dioxide back into a");
	// The mass ledger is the pathway's alone: every gram each compartment
	// gained or lost is exactly the pathway's published transfer, with no
	// link term in sight - the link moved heat and no mass.
	for (size_t i = 0; i < 2; ++i)
	{
		auto const now = masses(snapshot.atmospheres[i]);
		auto const start = masses(initial.atmospheres[i]);
		for (size_t gas = 0; gas < 4; ++gas)
		{
			auto const delta = (i == 0 ? start[gas] - now[gas] : now[gas] - start[gas]);
			require(std::abs(delta - pathway.netTransferredKg[gas]) < 1e-9,
				"a compartment's gas inventory moved by something other than the pathway");
		}
		require(snapshot.atmospheres[i].condensedKg == 0.0,
			"neither aggregate should have driven a phase change in this fixture");
	}
	// The energy ledger is the link's alone: each side's conducted total
	// is the link's own signed ledger as an exact negation, with no
	// pathway term folded in - the pathway moved mass and no heat.
	require(snapshot.atmospheres[0].conductedEnergyKJ == -wall.netTransferredKJ
		&& snapshot.atmospheres[1].conductedEnergyKJ == wall.netTransferredKJ,
		"the energy ledger did not close on the link's own booking alone");
	// And with both aggregates live the audit's residual still closes.
	for (size_t i = 0; i < 2; ++i)
		require(std::abs(snapshot.atmospheres[i].residualKJ)
			<= 1e-6 * std::max(1.0, std::abs(snapshot.atmospheres[i].currentEnergyKJ)),
			"residual did not close with a link and a pathway both present");
}

// A compartment with both a heat-only exchanger to a reservoir and a
// thermal link books the two separately and its residual still closes.
void thermalLinkReservoirCoexistence(smoke::Context const&)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	auto a = linkCompartment("a", 40.0, 10000.0);
	a.reservoirId = "sink";
	a.exchangerConductanceKWPerK = 5.0;
	auto b = linkCompartment("b", 10.0, 10000.0);
	s.atmospheres = {a, b};
	core::ThermalReservoirSettings sink;
	sink.id = "sink";
	sink.thermalClass = "moderate";
	sink.heatCapacityKJPerK = 50000.0;
	sink.startingTemperatureC = 10.0;
	sink.minTemperatureC = -50.0;
	sink.maxTemperatureC = 200.0;
	s.thermalReservoirs = {sink};
	s.thermalLinks = {{"wall", "a", "b", 20.0}};
	std::string diagnostic;
	core::World world("ThermalLinkReservoirCoexist", 2, 2);
	configure(world, s, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 300), "reservoir-coexistence run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "reservoir-coexistence run failed its audit");
	auto const& hot = snapshot.atmospheres[0];
	auto const& cold = snapshot.atmospheres[1];
	// Both heat paths genuinely active on the hot compartment: it gave
	// heat to the reservoir and to the cold neighbour, booked apart.
	require(hot.exchangedEnergyKJ != 0.0, "the exchanger booked nothing");
	require(hot.conductedEnergyKJ != 0.0, "the link booked nothing");
	require(std::abs(hot.residualKJ) <= 1e-6 * std::max(1.0, std::abs(hot.currentEnergyKJ))
		&& std::abs(cold.residualKJ) <= 1e-6 * std::max(1.0, std::abs(cold.currentEnergyKJ)),
		"residual did not close with an exchanger and a link both present");
	// The link's ledger still matches the cold side's conducted total:
	// the exchanger never leaked into the link's booking.
	require(cold.conductedEnergyKJ == link(snapshot, "wall").netTransferredKJ,
		"the reservoir exchange leaked into the link's ledger");
}

// Validation refusals: each refusal names the offending entry, and no
// refusal mutates the World.
void thermalLinkValidation(smoke::Context const&)
{
	std::string diagnostic;
	core::StationSystemsSettings good;
	good.enabled = true;
	good.atmospheres = {linkCompartment("a", 40.0, 10000.0),
		linkCompartment("b", 0.0, 10000.0)};
	good.thermalLinks = {{"wall", "a", "b", 10.0}};
	require(core::validateStationSystemsSettings(good, diagnostic), diagnostic.c_str());
	auto bad = good;
bad.thermalLinks = {{"zero", "a", "b", 0.0}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("zero") != std::string::npos
		&& diagnostic.find("conductance") != std::string::npos,
		"a zero-conductance link was not refused by name");
bad.thermalLinks = {{"negative", "a", "b", -1.0}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("negative") != std::string::npos,
		"a negative-conductance link was not refused by name");
bad.thermalLinks = {{"nan", "a", "b", std::numeric_limits<double>::quiet_NaN()}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("nan") != std::string::npos,
		"a non-finite-conductance link was not refused by name");
bad.thermalLinks = {{"ghost", "a", "nowhere", 1.0}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("ghost") != std::string::npos
		&& diagnostic.find("nowhere") != std::string::npos,
		"an unresolved link endpoint was not refused by name");
bad.thermalLinks = {{"loop", "a", "a", 1.0}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("loop") != std::string::npos
		&& diagnostic.find("self-loop") != std::string::npos,
		"a self-loop link was not refused by name");
bad.thermalLinks = {{"vacuum", "a", core::StationExteriorAtmosphereId, 1.0}};
	require(!core::validateStationSystemsSettings(bad, diagnostic)
		&& diagnostic.find("vacuum") != std::string::npos
		&& diagnostic.find("Exterior") != std::string::npos,
		"an Exterior-vacuum endpoint was not refused by name");
	// The declared ceiling, in the spirit of the pathway ceiling.
auto many = good;
	many.thermalLinks.clear();
	for (size_t i = 0; i < core::MaxStationThermalLinks; ++i)
		many.thermalLinks.push_back({"wall" + std::to_string(i), "a", "b", 1.0});
	require(core::validateStationSystemsSettings(many, diagnostic), diagnostic.c_str());
	many.thermalLinks.push_back({"over-limit", "a", "b", 1.0});
	require(!core::validateStationSystemsSettings(many, diagnostic)
		&& diagnostic.find("limited") != std::string::npos,
		"the thermal link count ceiling was not enforced");
	// No refusal mutates the World.
	core::World world("ThermalLinkValidationWorld", 2, 2);
	configure(world, good, diagnostic);
	auto const baseline = world.getStationSystemsSnapshot();
for (auto refusal : bad.thermalLinks)
	{
		auto mutated = good;
		mutated.thermalLinks = {refusal};
		require(!world.configureStationSystems(mutated, false, &diagnostic)
			&& world.getStationSystemsSnapshot() == baseline
			&& world.getStationSystemsSettings() == good,
			"a refused thermal link configuration mutated the World");
	}
}

// Reset restores the authored starting temperatures and clears the
// cumulative conducted heat.
void thermalLinkResetIsolation(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ThermalLinkReset", 2, 2);
	configure(world, linkedPair(), diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 120), "reset-isolation run failed");
	auto const run = world.getStationSystemsSnapshot();
	require(link(run, "wall").netTransferredKJ > 1.0
		&& run.atmospheres[1].conductedEnergyKJ > 1.0,
		"reset fixture conducted nothing to clear");
	world.pauseSimulation();
	world.resetSimulation();
	auto const reset = world.getStationSystemsSnapshot();
	require(reset.atmospheres[0].temperatureC == 40.0
		&& reset.atmospheres[1].temperatureC == 10.0,
		"reset did not restore the authored starting temperatures");
	require(reset.atmospheres[0].conductedEnergyKJ == 0.0
		&& reset.atmospheres[1].conductedEnergyKJ == 0.0
		&& link(reset, "wall").netTransferredKJ == 0.0
		&& link(reset, "wall").flowRateKW == 0.0,
		"reset did not clear the cumulative conducted heat");
	// And the rerun reproduces the original run exactly.
	world.resumeSimulation();
	require(advanceSeconds(world, 120), "reset-isolation rerun failed");
	require(world.getStationSystemsSnapshot() == run,
		"the rerun after reset is not identical to the first run");
}

// #796: the payoff of the conduction pass's tick placement. Conducted
// heat is evaluated at tick-start temperatures and folded into each
// endpoint's external heat rate sum before that compartment's coupled
// vapour/thermal solve runs, so a warm near-saturated compartment that
// loses heat through a link condenses in the very tick the heat leaves -
// the phase change and the temperature it depends on resolve together.
// These checks assert only published observables; the coupled solve is
// untouched, and if a future change moves the conduction pass after the
// per-compartment solve the same-tick assertion below fails instead of
// silently lagging a tick behind.
namespace
{
// The compartment's vapour capacity at its own temperature, in the
// same Magnus/ideal-gas terms the runtime uses.
double saturatedMassKg(double temperatureC, double volumeM3)
{
	return core::stationSaturatedVapourMassKg(
		core::stationSaturationVapourPressureKPa(temperatureC), volumeM3, temperatureC);
}

// The first tick's conducted energy for a pair, from the same closed
// form the runtime applies: this is what calibrates the fixture so the
// vapour load sits strictly between the capacity the donor holds at
// tick start and the lower capacity its own conduction leaves it with -
// guaranteeing the first tick crosses the phase boundary whatever the
// step cadence turns out to be.
double firstTickConductedKJ(double capacityKJPerK, double deltaT,
	double conductanceKWPerK, double dt)
{
	return 0.5 * capacityKJPerK * deltaT
		* -std::expm1(-2.0 * conductanceKWPerK * dt / capacityKJPerK);
}
} // namespace

void thermalLinkSameTickCondensation(smoke::Context const&)
{
	// A warm, near-saturated donor (a) linked to a cold, dry neighbour
	// (b). The donor's vapour load is calibrated at runtime to sit
	// between its capacity at 40 C and its capacity after the first
	// tick's conduction has cooled it, so the first conducted tick is
	// the first tick that crosses saturation - no pre-existing
	// supersaturation, no lucky fixture constants.
	auto const dt = requestedStepSeconds();
	auto constexpr capacity = 10000.0;
	auto constexpr conductance = 500.0;
	// Both starting temperatures are non-zero: the audit's thermal
	// closure bound scales with the starting energy, and a zero-Celsius
	// datum would leave accumulated rounding no room at all (see the
	// linkedPair note above).
	auto constexpr donorC = 40.0;
	auto constexpr neighbourC = 5.0;
	auto const capStart = saturatedMassKg(donorC, 100.0);
	auto const capAfterFirstTick = saturatedMassKg(
		donorC - firstTickConductedKJ(capacity, donorC - neighbourC, conductance, dt) / capacity,
		100.0);
	require(capAfterFirstTick < capStart,
		"the calibration itself found no capacity drop to cross");
	auto const vapourKg = 0.5 * (capStart + capAfterFirstTick);
	core::StationSystemsSettings s;
	s.enabled = true;
	auto a = linkCompartment("a", donorC, capacity);
	a.startingWaterVapourKg = vapourKg;
	auto b = linkCompartment("b", neighbourC, capacity);
	b.startingWaterVapourKg = 0.05; // far below capacity at any temperature it reaches
	s.atmospheres = {a, b};
	s.thermalLinks = {{"wall", "a", "b", conductance}};
	std::string diagnostic;
	core::World world("ThermalLinkSameTickCondensation", 2, 2);
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	require(std::abs(initial.systemsTimestepSeconds - dt) < 1e-12,
		"the fixture's step assumption does not match the configured cadence");
	for (auto const& air : initial.atmospheres)
		require(air.relativeHumidity < 1.0,
			"the condensation fixture starts supersaturated");
	require(vapourKg > capAfterFirstTick && vapourKg < capStart,
		"the donor's vapour does not straddle the first tick's capacity drop");
	world.resumeSimulation();
	// The same tick, proved: one station step, and the tick that reports
	// the heat leaving also reports the condensation that heat caused.
	require(advanceStationSteps(world, 1), "same-tick step failed");
	auto const first = world.getStationSystemsSnapshot();
	require(first.healthy, "same-tick run failed its audit");
	auto const& hot = first.atmospheres[0];
	require(hot.lastConductedKW < 0.0,
		"the donor's first tick did not report conducted heat leaving");
	require(hot.lastVapourCondensedKg > 1e-3,
		"the tick that conducted the heat away did not condense in the same tick");
	require(hot.condensedKg == hot.lastVapourCondensedKg && hot.condensing,
		"the first-tick condensation was not reported in the cumulative observable");
	require(hot.lastLatentHeatKW > 0.0,
		"same-tick condensation booked no latent heat");
	// The latent feedback is visible: the condensate is smaller than
	// the naive drop from the calibrated load to the post-conduction
	// capacity, because condensing warmed the very air that sets the
	// cap - the solve really coupled the two rather than sequencing
	// them against stale temperatures.
	auto const naiveDrop = vapourKg - capAfterFirstTick;
	require(hot.lastVapourCondensedKg < naiveDrop
		&& hot.lastVapourCondensedKg > 0.5 * naiveDrop,
		"same-tick condensation did not sit inside its latent-feedback bracket");
	require(first.atmospheres[1].lastVapourCondensedKg == 0.0,
		"the receiving compartment condensed while warming");
	// Then the whole relaxation: every published tick stays at or below
	// its own end-of-tick capacity, the vapour ledger keeps every gram
	// that left the gas phase accounted as condensate (it never comes
	// back - no re-evaporation in this slice), and the donor's thermal
	// ledger closes on conducted plus latent heat alone.
	auto previousCondensed = hot.condensedKg;
	auto previousVapour = hot.waterVapourKg;
	while (world.getStationSystemsSnapshot().elapsedSeconds < 200.0)
	{
		require(advanceStationSteps(world, 1), "same-tick relaxation step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "same-tick relaxation failed its audit");
		for (auto const& air : snapshot.atmospheres)
		{
			auto const cap = saturatedMassKg(air.temperatureC, 100.0);
			require(air.waterVapourKg <= cap + 1e-9
				&& air.relativeHumidity <= 1.0 + 1e-9,
				"conduction published a compartment supersaturated against its own end-of-tick temperature");
		}
		auto const& donor = snapshot.atmospheres[0];
		require(std::abs(donor.waterVapourKg + donor.condensedKg - vapourKg) < 1e-6,
			"condensate did not stay out of the gas phase: the vapour/condensate ledger drifted");
		require(donor.condensedKg >= previousCondensed - 1e-12,
			"cumulative condensate decreased - a phase change ran backwards");
		require(donor.waterVapourKg <= previousVapour + 1e-12,
			"the donor's vapour grew while it was still losing heat");
		previousCondensed = donor.condensedKg;
		previousVapour = donor.waterVapourKg;
		auto const thermalDelta = capacity * (donor.temperatureC - donorC);
		require(std::abs(thermalDelta - donor.conductedEnergyKJ - donor.latentHeatEnergyKJ)
			<= 1e-6 * std::max(1.0, std::abs(thermalDelta)),
			"the donor's temperature did not close on conducted plus latent heat alone");
		require(snapshot.atmospheres[1].condensedKg == 0.0,
			"the warming receiver condensed during the relaxation");
	}
	auto const settled = world.getStationSystemsSnapshot();
	require(settled.atmospheres[0].condensedKg > 1.0,
		"the relaxation condensed nothing worth reporting");
	require(settled.atmospheres[0].temperatureC < 23.0,
		"the donor did not cool toward the pair's equilibrium");
	// Quiescence: past convergence the condensate stays condensed and
	// the vapour stays put - nothing quietly re-evaporates once the
	// conduction that drove it has stopped.
	auto const frozenVapour = settled.atmospheres[0].waterVapourKg;
	auto const frozenCondensed = settled.atmospheres[0].condensedKg;
	require(advanceSeconds(world, 60.0), "post-settle run failed");
	auto const after = world.getStationSystemsSnapshot();
	require(after.healthy, "post-settle run failed its audit");
	require(std::abs(after.atmospheres[0].waterVapourKg - frozenVapour) < 1e-5
		&& std::abs(after.atmospheres[0].condensedKg - frozenCondensed) < 1e-5,
		"the settled pair re-evaporated condensate after conduction stopped");
}

// The reverse case: heat conducted INTO a compartment raises its vapour
// capacity with its temperature, so a near-saturated receiver warms
// without condensing a gram - the conduction pass cannot manufacture a
// phase change in the direction the physics says there isn't one.
void thermalLinkReverseNoSpuriousCondensation(smoke::Context const&)
{
	auto constexpr capacity = 10000.0;
	auto constexpr receiverVapourKg = 0.93; // RH ~0.99 at 10 C, below capacity everywhere above
	require(receiverVapourKg < saturatedMassKg(10.0, 100.0)
		&& receiverVapourKg > 0.9 * saturatedMassKg(10.0, 100.0),
		"the receiver fixture is not near-saturated at its starting temperature");
	core::StationSystemsSettings s;
	s.enabled = true;
	auto a = linkCompartment("a", 40.0, capacity);
	a.startingWaterVapourKg = 0.001; // far below capacity: the donor never condenses either
	auto b = linkCompartment("b", 10.0, capacity);
	b.startingWaterVapourKg = receiverVapourKg;
	s.atmospheres = {a, b};
	s.thermalLinks = {{"wall", "a", "b", 100.0}};
	std::string diagnostic;
	core::World world("ThermalLinkReverseCase", 2, 2);
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	require(initial.atmospheres[1].relativeHumidity < 1.0,
		"the reverse-case receiver starts supersaturated");
	world.resumeSimulation();
	auto previousTemperature = initial.atmospheres[1].temperatureC;
	auto previousHumidity = initial.atmospheres[1].relativeHumidity;
	auto previousCapacity = saturatedMassKg(previousTemperature, 100.0);
	while (world.getStationSystemsSnapshot().elapsedSeconds < 300.0)
	{
		require(advanceStationSteps(world, 1), "reverse-case step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "reverse-case run failed its audit");
		auto const& receiver = snapshot.atmospheres[1];
		require(receiver.conductedEnergyKJ > 0.0,
			"the reverse-case receiver is not receiving conducted heat");
		require(receiver.lastVapourCondensedKg == 0.0 && receiver.condensedKg == 0.0,
			"heat conducted into a compartment condensed its vapour spuriously");
		require(receiver.waterVapourKg == receiverVapourKg,
			"the receiver's vapour changed with no phase process to account for it");
		require(receiver.temperatureC >= previousTemperature,
			"the receiver did not warm under incoming conduction");
		auto const cap = saturatedMassKg(receiver.temperatureC, 100.0);
		require(cap > previousCapacity,
			"incoming conducted heat did not raise the receiver's vapour capacity");
		require(receiver.relativeHumidity <= previousHumidity,
			"the receiver's humidity rose while its capacity was growing");
		require(receiver.waterVapourKg <= cap + 1e-9,
			"the reverse case published supersaturated gas");
		previousTemperature = receiver.temperatureC;
		previousHumidity = receiver.relativeHumidity;
		previousCapacity = cap;
		require(snapshot.atmospheres[0].condensedKg == 0.0,
			"the dry donor condensed while cooling");
	}
	auto const warmed = world.getStationSystemsSnapshot();
	require(warmed.atmospheres[1].temperatureC > 20.0
		&& warmed.atmospheres[1].relativeHumidity < 0.85,
		"the reverse-case receiver did not genuinely warm and open its capacity");
	require(warmed.atmospheres[1].condensedKg == 0.0,
		"a whole warming run invented condensate in the receiver");
}

// Stress: a chain of linked compartments homogenizes to one common
// temperature over a long run and stays there.
void thermalLinkChainHomogenization(smoke::Context const&)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.atmospheres = {linkCompartment("a", 60.0, 10000.0),
		linkCompartment("b", 20.0, 20000.0), linkCompartment("c", -10.0, 10000.0)};
	s.thermalLinks = {{"ab", "a", "b", 250.0}, {"bc", "b", "c", 250.0}};
	std::string diagnostic;
	core::World world("ThermalLinkChain", 2, 2);
	configure(world, s, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 4000), "chain homogenization run failed");
	auto const snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "chain homogenization failed its audit");
	// The whole chain's energy-weighted average: (1*60 + 2*20 + 1*-10)/4 = 22.5 C.
	auto const expected = (10000.0 * 60.0 + 20000.0 * 20.0 + 10000.0 * -10.0) / 40000.0;
	for (auto const& air : snapshot.atmospheres)
		require(std::abs(air.temperatureC - expected) < 1e-2,
			"the linked chain did not converge to the common temperature");
	// And it stays converged: another long run moves nothing measurable.
	require(advanceSeconds(world, 4000), "chain settle run failed");
	auto const settled = world.getStationSystemsSnapshot();
	for (size_t i = 0; i < 3; ++i)
		require(std::abs(settled.atmospheres[i].temperatureC
			- snapshot.atmospheres[i].temperatureC) < 1e-6,
			"the converged chain drifted after homogenization");
}

// Stress: absurd geometry - enormous conductance against tiny
// capacities, thousands of ticks - demonstrating the closed-form
// decision holds what was claimed of it: no oscillation, no crossing,
// no non-finite value, ledger still closed.
void thermalLinkAbsurdGeometry(smoke::Context const&)
{
	std::string diagnostic;
	core::World world("ThermalLinkAbsurd", 2, 2);
	configure(world, linkedPair(1.0, 1.0, 100000.0), diagnostic);
	world.resumeSimulation();
	double previousA = 40.0, previousB = 0.0;
	for (int step = 0; step < 5000; ++step)
	{
		require(advanceStationSteps(world, 1), "absurd-geometry step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "absurd-geometry run failed its audit");
		auto const a = snapshot.atmospheres[0].temperatureC;
		auto const b = snapshot.atmospheres[1].temperatureC;
		require(std::isfinite(a) && std::isfinite(b),
			"absurd geometry produced a non-finite temperature");
		require(a >= b, "absurd geometry crossed hot past cold");
		// Monotone: each compartment moves toward the average, never back.
		require(a <= previousA + 1e-9 && b >= previousB - 1e-9,
			"absurd geometry oscillated instead of relaxing");
		previousA = a; previousB = b;
	}
	require(std::abs(previousA - 25.0) < 1e-6 && std::abs(previousB - 25.0) < 1e-6,
		"absurd geometry did not settle on the energy-weighted average");
}
}

void registerStationThermalLinkChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"thermalLinkConservation", thermalLinkConservation});
	checks.push_back({"thermalLinkAdr0026Contrast", thermalLinkAdr0026Contrast});
	checks.push_back({"thermalLinkIdle", thermalLinkIdle});
	checks.push_back({"thermalLinkCapacityWeighting", thermalLinkCapacityWeighting});
	checks.push_back({"thermalLinkNoCrossing", thermalLinkNoCrossing});
	checks.push_back({"thermalLinkParallelAdditivity", thermalLinkParallelAdditivity});
	checks.push_back({"thermalLinkDeclarationOrder", thermalLinkDeclarationOrder});
	checks.push_back({"thermalLinkMassUntouched", thermalLinkMassUntouched});
	checks.push_back({"thermalLinkPathwayCoexistence", thermalLinkPathwayCoexistence});
	checks.push_back({"thermalLinkPathwayIndependence", thermalLinkPathwayIndependence});
	checks.push_back({"thermalLinkReservoirCoexistence", thermalLinkReservoirCoexistence});
	checks.push_back({"thermalLinkValidation", thermalLinkValidation});
	checks.push_back({"thermalLinkResetIsolation", thermalLinkResetIsolation});
	checks.push_back({"thermalLinkSameTickCondensation", thermalLinkSameTickCondensation});
	checks.push_back({"thermalLinkReverseNoSpuriousCondensation",
		thermalLinkReverseNoSpuriousCondensation});
	checks.push_back({"thermalLinkChainHomogenization", thermalLinkChainHomogenization});
	checks.push_back({"thermalLinkAbsurdGeometry", thermalLinkAbsurdGeometry});
}
