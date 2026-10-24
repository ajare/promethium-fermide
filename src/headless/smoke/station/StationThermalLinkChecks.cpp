#include "StationRig.h"
#include "Smoke.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/BinarySerializer.h"
#include "core/StationConfigurationDocument.h"
#include "core/YamlSerializer.h"

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

// #835: Celsius zero is a valid starting state. The audit scale must follow
// energy that is subsequently accumulated, not merely the arbitrary starting
// Celsius datum; otherwise ordinary summation roundoff aborts a sound run.
void thermalLinkZeroCelsiusAudit(smoke::Context const&)
{
	struct Scenario
	{
		double coldStartC;
		uint32_t cadenceRatio;
	};
	// The default-cadence zero case is the reported failure. The nearby
	// starts and the offset control make the invariant explicit rather than
	// merely moving the singularity to a convenient fixture value.
	for (auto const scenario : std::array<Scenario, 4>{{
		{0.0, 1u}, {-0.001, 60u}, {0.001, 60u}, {200.0, 60u}}})
	{
		core::ScopedStationStepRatioForTesting cadence(scenario.cadenceRatio);
		auto settings = linkedPair(40000.0, 40000.0, 10.0);
		settings.atmospheres[0].startingTemperatureC = scenario.coldStartC + 40.0;
		settings.atmospheres[1].startingTemperatureC = scenario.coldStartC;
		std::string diagnostic;
		core::World world("ThermalLinkZeroCelsiusAudit", 2, 2);
		configure(world, settings, diagnostic);
		world.resumeSimulation();
		auto const completed = advanceSeconds(world, 10000.0);
		auto const snapshot = world.getStationSystemsSnapshot();
		auto const diagnostics = world.getStationSystemsDiagnostics();
		require(completed && snapshot.healthy && snapshot.elapsedSeconds >= 10000.0,
			(diagnostics.empty() ? "zero-Celsius full-duration run failed its audit at "
					+ std::to_string(snapshot.elapsedSeconds) + " seconds"
				: "zero-Celsius full-duration run failed its audit: "
					+ diagnostics.back().message).c_str());
		auto const& cold = snapshot.atmospheres[1];
		auto scale = std::max({1.0, std::abs(cold.startingEnergyKJ),
			std::abs(cold.currentEnergyKJ), std::abs(cold.occupantHeatEnergyKJ),
			std::abs(cold.crewHeatEnergyKJ), std::abs(cold.cropHeatEnergyKJ),
			std::abs(cold.algaeHeatEnergyKJ), std::abs(cold.latentHeatEnergyKJ),
			std::abs(cold.exchangedEnergyKJ), std::abs(cold.conductedEnergyKJ),
			std::abs(cold.advectedEnergyKJ)});
		auto const bound = snapshot.balanceTolerance * scale;
		require(std::abs(cold.residualKJ) <= bound,
			"zero-Celsius thermal ledger did not close at its energy scale");
		// The tolerance tracks accumulated energy only enough for arithmetic
		// roundoff. Omitting or double-booking this wall's conducted heat
		// still exceeds it by orders of magnitude, preserving the audit as
		// a safety net for both bookkeeping mistakes.
		require(std::abs(cold.residualKJ - cold.conductedEnergyKJ) > bound
			&& std::abs(cold.residualKJ + cold.conductedEnergyKJ) > bound,
			"the thermal audit scale would mask missing or double-booked conducted heat");
	}
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
	// And ADR-0026 as amended by #802 holds: the pathway is still not a
	// conductor - neither compartment books a single joule of conducted
	// heat across thirty minutes of continuous gas exchange. What the
	// pathway does now is carry enthalpy with the mass it moves, so
	// the temperature difference decays through the booked advected
	// term instead of persisting. The contrast with the link below is
	// the mechanism, not the existence of a temperature effect: the
	// pathway's effect is enthalpy riding with mass, the link's is
	// conduction with no mass at all.
	require(isolatedSnap.atmospheres[0].conductedEnergyKJ == 0
		&& isolatedSnap.atmospheres[1].conductedEnergyKJ == 0,
		"the thermally-neutral pathway booked conducted heat");
	require(isolatedSnap.atmospheres[0].advectedEnergyKJ < 0
		&& isolatedSnap.atmospheres[1].advectedEnergyKJ > 0,
		"the pathway did not carry enthalpy from the hot side to the cold side");
	require(isolatedSnap.atmospheres[0].temperatureC
		- isolatedSnap.atmospheres[1].temperatureC < 29.999,
		"advected enthalpy did not begin decaying the temperature difference");
	for (size_t side = 0; side < 2; ++side)
		require(std::abs(isolatedSnap.atmospheres[side].temperatureC
				- isolatedStart.atmospheres[side].temperatureC) > 1e-9,
			"the pathway's enthalpy transport left the published temperatures untouched");
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
	// The conducted ledger is the link's alone: each side's conducted
	// total is the link's own signed ledger as an exact negation, with
	// no pathway term folded in. Since #802 this pathway does carry heat,
	// but it books that separately as advected energy, so conduction
	// remains a clean single-aggregate ledger either way.
	require(snapshot.atmospheres[0].conductedEnergyKJ == -wall.netTransferredKJ
		&& snapshot.atmospheres[1].conductedEnergyKJ == wall.netTransferredKJ,
		"the energy ledger did not close on the link's own booking alone");
	// And with both aggregates live the audit's residual still closes.
	for (size_t i = 0; i < 2; ++i)
		require(std::abs(snapshot.atmospheres[i].residualKJ)
			<= 1e-6 * std::max(1.0, std::abs(snapshot.atmospheres[i].currentEnergyKJ)),
			"residual did not close with a link and a pathway both present");
}

// #833: individually stable passive exchanges must share the state left
// by their predecessors, not spend the same starting heat twice. Keep the
// controls and the combined run at the World/snapshot seam.
void strongReservoirCoexistence()
{
	for (uint32_t const ratio : {1u, 6u, 7u, 60u})
	for (double const sinkTemperatureC : {10.0, -260.0, 80.0})
	for (int const mode : {0, 1, 2, 3}) // exchanger, wall, both, linked network
	{
		core::ScopedStationStepRatioForTesting cadence(ratio);
		auto s = linkedPair(1.0, 1.0, 1e8);
		for (auto& air : s.atmospheres) air.startingWaterVapourKg = 0.0;
		core::ThermalReservoirSettings sink;
		sink.id = "sink";
		sink.thermalClass = "moderate";
		sink.heatCapacityKJPerK = 1e6;
		sink.startingTemperatureC = sinkTemperatureC;
		sink.minTemperatureC = -270.0;
		sink.maxTemperatureC = 200.0;
		s.thermalReservoirs = {sink};
		if (mode != 1)
		{
			s.atmospheres[0].reservoirId = "sink";
			s.atmospheres[0].exchangerConductanceKWPerK = 1e8;
		}
		if (mode == 0) s.thermalLinks.clear();
		if (mode == 3)
		{
			auto c = linkCompartment("c", 60.0, 2.0);
			c.startingWaterVapourKg = 0.0;
			c.reservoirId = "sink";
			c.exchangerConductanceKWPerK = 1e8;
			s.atmospheres.push_back(c);
			s.thermalLinks.push_back({"second-wall", "b", "c", 1e8});
		}
		std::string diagnostic;
		core::World world("StrongReservoirCoexistence", 2, 2);
		configure(world, s, diagnostic);
		world.resumeSimulation();
		for (int step = 0; step < 20; ++step)
		{
			require(advanceStationSteps(world, 1), "strong passive exchange step failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.healthy, "strong passive exchange failed its audit");
			double conducted = 0.0, exchanged = 0.0;
			for (size_t i = 0; i < s.atmospheres.size(); ++i)
			{
				auto const& air = snapshot.atmospheres[i];
				require(std::isfinite(air.temperatureC)
					&& air.temperatureC >= std::min(10.0, sinkTemperatureC) - 1e-8
					&& air.temperatureC <= std::max(60.0, sinkTemperatureC) + 1e-8,
					"passive exchanges overshot the available thermal state");
				require(air.temperatureC + core::StationCelsiusOffset > 0.0,
					"passive exchanges published negative kelvin");
				require(std::abs(air.residualKJ) < 1e-6,
					"strong passive compartment ledger did not close");
				conducted += air.conductedEnergyKJ;
				exchanged += air.exchangedEnergyKJ;
			}
			require(std::abs(conducted) < 1e-9, "network conducted heat was not conserved");
			require(std::abs(exchanged + snapshot.thermal.reservoirs[0].currentEnergyKJ
				- snapshot.thermal.reservoirs[0].startingEnergyKJ) < 1e-5,
				"reservoir and compartment exchange ledgers disagree");
			if (step == 0 && mode < 3)
			{
				auto const afterExchange = 40.0 + snapshot.atmospheres[0].exchangedEnergyKJ;
				require(mode == 1 ? afterExchange == 40.0
					: std::abs(afterExchange - sinkTemperatureC) < 1e-2,
					"strong exchanger control did not approach the reservoir temperature");
				auto const moved = mode == 0 ? 0.0 : (afterExchange - 10.0) / 2.0;
				require(std::abs(snapshot.atmospheres[0].temperatureC - (afterExchange - moved)) < 1e-6
					&& std::abs(snapshot.atmospheres[1].temperatureC - (10.0 + moved)) < 1e-6,
					"wall did not relax the post-exchanger state");
				require(std::abs(snapshot.atmospheres[0].conductedEnergyKJ + moved) < 1e-6,
					"strong wall heat was not separately attributed");
			}
		}
	}
	// The provisional post-exchanger state must not bypass same-step
	// condensation: both passive terms enter the one vapour/thermal solve.
	for (uint32_t const ratio : {1u, 6u, 7u, 60u})
	{
		core::ScopedStationStepRatioForTesting cadence(ratio);
		auto s = linkedPair(10000.0, 10000.0, 1e8);
		s.atmospheres[0].startingWaterVapourKg = 0.99 * core::stationSaturatedVapourMassKg(
			core::stationSaturationVapourPressureKPa(40.0), 100.0, 40.0);
		s.atmospheres[0].reservoirId = "sink";
		s.atmospheres[0].exchangerConductanceKWPerK = 1e8;
		s.atmospheres[1].startingTemperatureC = 5.0;
		core::ThermalReservoirSettings sink;
		sink.id = "sink";
		sink.thermalClass = "moderate";
		sink.heatCapacityKJPerK = 1e6;
		sink.startingTemperatureC = 10.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 200.0;
		s.thermalReservoirs = {sink};
		std::string diagnostic;
		core::World world("StrongReservoirCondensation", 2, 2);
		configure(world, s, diagnostic);
		world.resumeSimulation();
		require(advanceStationSteps(world, 1), "strong wet passive exchange step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "strong wet passive exchange failed its audit");
		auto const& hot = snapshot.atmospheres[0];
		require(hot.exchangedEnergyKJ < 0.0 && hot.conductedEnergyKJ < 0.0
			&& hot.lastVapourCondensedKg > 0.0 && hot.lastLatentHeatKW > 0.0,
			"combined passive cooling did not condense in the same step");
		for (size_t i = 0; i < s.atmospheres.size(); ++i)
		{
			auto const& air = snapshot.atmospheres[i];
			require(air.relativeHumidity <= 1.0 + 1e-9,
				"combined passive cooling published supersaturated air");
			require(std::abs(air.waterVapourKg + air.condensedKg
				- s.atmospheres[i].startingWaterVapourKg) < 1e-9,
				"combined passive cooling lost vapour/condensate mass");
			require(std::abs(air.currentEnergyKJ - air.startingEnergyKJ
				- air.exchangedEnergyKJ - air.conductedEnergyKJ - air.latentHeatEnergyKJ) < 1e-6,
				"combined passive and latent thermal ledgers disagree");
		}
	}
}

// A compartment with both a heat-only exchanger to a reservoir and a
// thermal link books the two separately and its residual still closes.
void thermalLinkReservoirCoexistence(smoke::Context const&)
{
	strongReservoirCoexistence();
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

// #798: the persistence contract for authored Thermal links. A station
// document carrying links saves and reloads to the identical
// configuration, an unknown field is refused with a conversion-required
// diagnostic naming the entry and the field rather than being silently
// defaulted, a pre-107 reader refuses a document carrying links rather
// than silently dropping them, and a round-tripped configuration
// reproduces the original run exactly through both the YAML and the
// binary World persistence paths.
void thermalLinkPersistence(smoke::Context const&)
{
	// Three links over three compartments with distinct non-round values:
	// the round trip must return the exact list, in declaration order,
	// with no field defaulted or drifted.
	core::StationSystemsSettings settings;
	settings.enabled = true;
	settings.atmospheres = {linkCompartment("a", 40.0, 12000.0),
		linkCompartment("b", 10.0, 8000.0), linkCompartment("c", 22.5, 4000.0)};
	settings.thermalLinks = {{"wall-ab", "a", "b", 2.5},
		{"wall-bc", "b", "c", 0.75}, {"wall-ca", "c", "a", 1.25}};
	core::StationConfigurationDocument document;
	document.settings = settings;
	auto const yaml = core::serializeStationConfiguration(document);
	core::StationConfigurationDocument loaded;
	std::string diagnostic;
	require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
	require(loaded.settings == settings,
		"thermal link Station configuration changed on round trip");
	// Unknown-field discipline: a field the loader does not know is
	// refused naming the entry and the field, and the previously loaded
	// settings are left untouched.
	for (auto const* field : {"conductanceKWPerK", "aAtmosphereId"})
	{
		auto bad = yaml;
		auto const at = bad.find(std::string(field) + ":");
		require(at != std::string::npos, "missing thermal link wire field");
		bad.insert(at, "unknown_");
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& diagnostic.find("wall-ab") != std::string::npos
			&& diagnostic.find(std::string("unknown_") + field) != std::string::npos
			&& diagnostic.find("conversion required") != std::string::npos
			&& loaded.settings == settings,
			"unknown thermal link field was ignored or mutated the document");
	}
	// Version discipline: a pre-107 document carrying links is refused
	// as conversion-required rather than dropping them, and a version
	// beyond the accepted range is refused outright.
	for (int version : {106, 113})
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 111"), std::string("schemaVersion: 111").size(),
			"schemaVersion: " + std::to_string(version));
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& loaded.settings == settings, "unsupported thermal link schema was accepted");
	}
	// Both World persistence paths: the reloaded World carries the exact
	// authored links, and after the same number of ticks publishes the
	// identical snapshot to the original run.
	for (bool binary : {false, true})
	{
		core::World original("ThermalLinkPersistence", 2, 2);
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
		if (!binary)
		{
			// An unknown field inside a saved World is refused by name and
			// leaves the already-loaded World exactly as it was.
			auto bad = text;
			bad.insert(bad.find("conductanceKWPerK:"), "unknown_");
			auto invalid = core::YamlSerializer::fromString(bad);
			invalid->deserialize();
			auto const baseline = original.getStationSystemsSnapshot();
			bool refused = false;
			try { original.deserialize(*invalid, work); }
			catch (std::exception const& error)
			{
				auto const message = std::string(error.what());
				refused = message.find("conversion required") != std::string::npos
					&& message.find("unknown_conductanceKWPerK") != std::string::npos
					&& message.find("wall-ab") != std::string::npos;
			}
			require(refused && original.getStationSystemsSnapshot() == baseline
				&& original.getStationSystemsSettings() == settings,
				"World ignored an unknown thermal link field or changed on refusal");
			// A pre-107 World reader refuses a document carrying links
			// rather than silently dropping the station's walls.
			auto older = text;
			older.replace(older.find("version: 111"), std::string("version: 111").size(),
				"version: 106");
			auto olderReader = core::YamlSerializer::fromString(older);
			olderReader->deserialize();
			refused = false;
			try
			{
				core::World stale("StaleReader", 2, 2);
				stale.deserialize(*olderReader, work);
			}
			catch (std::exception const& error)
			{
				refused = std::string(error.what()).find(
					"thermal links require World schema 107") != std::string::npos;
			}
			require(refused, "a pre-107 World reader silently accepted a document carrying links");
		}
		auto reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(text))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(text));
		reader->deserialize();
		core::World restored("Restored", 2, 2);
		require(restored.deserialize(*reader, work), "World carrying thermal links could not reload");
		require(restored.getStationSystemsSettings() == settings,
			"World thermal link settings changed across persistence");
		// The round-tripped configuration reproduces the original run:
		// the same number of ticks publishes the identical snapshot,
		// conducted ledgers included.
		original.resumeSimulation();
		restored.resumeSimulation();
		require(advanceSeconds(original, 120) && advanceSeconds(restored, 120),
			"thermal link replay after round trip failed");
		require(original.getStationSystemsSnapshot() == restored.getStationSystemsSnapshot(),
			"thermal link round trip changed the run snapshot");
		require(link(original.getStationSystemsSnapshot(), "wall-ab").netTransferredKJ > 1.0,
			"persistence fixture conducted too little to prove the round trip");
	}
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
	// The closed form conserves total C*T exactly per link, so the attractor
	// is that average itself, not merely something near it: after 100 slowest
	// time constants the assertion is against machine-precision landing, tight
	// enough that any cadence-dependent drift would show.
	auto const expected = (10000.0 * 60.0 + 20000.0 * 20.0 + 10000.0 * -10.0) / 40000.0;
	for (auto const& air : snapshot.atmospheres)
	{
		require(std::isfinite(air.temperatureC)
				&& air.temperatureC + core::StationCelsiusOffset > 0.0,
			"the homogenized chain published a non-finite or sub-absolute-zero temperature");
		require(std::abs(air.temperatureC - expected) < 1e-6,
			"the linked chain did not converge to the common temperature");
	}
	// And it stays converged: another long run moves nothing measurable -
	// no accumulated-roundoff drift away from the equilibrium it reached.
	require(advanceSeconds(world, 4000), "chain settle run failed");
	auto const settled = world.getStationSystemsSnapshot();
	require(settled.healthy, "chain settle run failed its audit");
	for (size_t i = 0; i < 3; ++i)
		require(std::abs(settled.atmospheres[i].temperatureC
			- snapshot.atmospheres[i].temperatureC) < 1e-6,
			"the converged chain drifted after homogenization");
}

// Stress: absurd geometry - enormous conductance against tiny
// capacities, thousands of ticks, swept across conductance decades so
// the no-crossing claim is tested at every stiffness rather than at one
// authored number. 1 kJ/K compartments against 10 kW/K to 1 TW/K
// walls: the pair relaxation time constant, 1/(G(1/Ca+1/Cb)), spans
// 50 ms down to 0.5 ns against the 100 ms step - from a boundary case
// that still relaxes over several ticks to ones that land on equilibrium
// inside the first. No sane author writes this and nothing refuses it.
// The closed-form decision must hold what was claimed of it at every
// one of them: no oscillation, no crossing, no negative absolute
// temperature, no non-finite value, and the ledger still closed at the
// end to the same tolerance a normal run holds itself to.
void thermalLinkAbsurdGeometry(smoke::Context const&)
{
	for (double const conductance : {1.0e1, 1.0e3, 1.0e5, 1.0e7, 1.0e9})
	{
		std::string diagnostic;
		core::World world("ThermalLinkAbsurd", 2, 2);
		configure(world, linkedPair(1.0, 1.0, conductance), diagnostic);
		world.resumeSimulation();
		double previousA = 40.0, previousB = 10.0;
		for (int step = 0; step < 5000; ++step)
		{
			require(advanceStationSteps(world, 1), "absurd-geometry step failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.healthy, "absurd-geometry run failed its audit");
			auto const a = snapshot.atmospheres[0].temperatureC;
			auto const b = snapshot.atmospheres[1].temperatureC;
			require(std::isfinite(a) && std::isfinite(b),
				"absurd geometry produced a non-finite temperature");
			require(a + core::StationCelsiusOffset > 0.0
					&& b + core::StationCelsiusOffset > 0.0,
				"absurd geometry published a negative absolute temperature");
			require(a >= b, "absurd geometry crossed hot past cold");
			// Monotone: each compartment moves toward the average, never back.
			require(a <= previousA + 1e-9 && b >= previousB - 1e-9,
				"absurd geometry oscillated instead of relaxing");
			previousA = a; previousB = b;
		}
		require(std::abs(previousA - 25.0) < 1e-6 && std::abs(previousB - 25.0) < 1e-6,
			"absurd geometry did not settle on the energy-weighted average");
		// The ledger closes at the end of the absurd run to the same
		// tolerance the ordinary conservation check holds itself to:
		// each side's residual closes, total C*T across the joined pair
		// is invariant, and the two sides' conducted totals are exact
		// negations of each other and of the link's own cumulative book.
		auto const snapshot = world.getStationSystemsSnapshot();
		auto const& a = snapshot.atmospheres[0];
		auto const& b = snapshot.atmospheres[1];
		require(a.conductedEnergyKJ == -b.conductedEnergyKJ,
			"absurd geometry did not book conducted heat as an exact negation");
		require(link(snapshot, "wall").netTransferredKJ == b.conductedEnergyKJ,
			"the absurd-geometry link ledger disagrees with the compartment ledgers");
		auto const total = a.thermalCapacityKJPerK * a.temperatureC
			+ b.thermalCapacityKJPerK * b.temperatureC;
		require(std::abs(total - 50.0) <= 1e-6 * 50.0,
			"absurd geometry did not conserve total energy across the link-joined pair");
		require(std::abs(a.residualKJ) <= 1e-6 * std::max(1.0, std::abs(a.currentEnergyKJ))
				&& std::abs(b.residualKJ) <= 1e-6 * std::max(1.0, std::abs(b.currentEnergyKJ)),
			"the absurd-geometry ledger did not close to the normal-run tolerance");
	}
}

// #797: links compose. Parallel walls add exactly, a chain converges to
// one common temperature with every joule still booked to the link that
// carried it, and the order the entries were declared in cannot move a
// published number. As everywhere in this file, every assertion is on
// what the configured World publishes, never on pass internals.
namespace
{
// The closed form one link applies, evaluated here from published state:
// the heat a link of this conductance moves between two compartments of
// these capacities over dt, given the temperature difference it sees
// when the pass reaches it.
double linkTransferKJ(double capacityAKJPerK, double capacityBKJPerK, double deltaT,
	double conductanceKWPerK, double dt)
{
	return capacityAKJPerK * capacityBKJPerK / (capacityAKJPerK + capacityBKJPerK) * deltaT
		* -std::expm1(-conductanceKWPerK * (1.0 / capacityAKJPerK + 1.0 / capacityBKJPerK) * dt);
}

// How one link's signed total lands on one of its two compartments:
// positive into the B side, negative out of the A side, zero anywhere
// else. A compartment wired to two links gets two such shares and no
// third term from anywhere.
int endpointSign(core::StationThermalLinkSnapshot const& entry, std::string_view atmosphereId)
{
	if (entry.bAtmosphereId == atmosphereId) return 1;
	if (entry.aAtmosphereId == atmosphereId) return -1;
	return 0;
}

core::StationAtmosphereSnapshot const& compartment(
	core::StationSystemsSnapshot const& snapshot, std::string_view atmosphereId)
{
	for (auto const& candidate : snapshot.atmospheres)
		if (candidate.id == atmosphereId) return candidate;
	throw std::runtime_error("compartment missing from snapshot");
}

// The compartment's own conducted total is exactly the signed sum of the
// cumulative transfers of the links that touch it: nothing carried by a
// link it is not an endpoint of, and nothing it carried itself lost.
void requireAttributedToItsLinks(core::StationSystemsSnapshot const& snapshot,
	std::string_view atmosphereId, char const* what)
{
	auto const& air = compartment(snapshot, atmosphereId);
	auto attributed = 0.0;
	for (auto const& entry : snapshot.thermalLinks)
		attributed += static_cast<double>(endpointSign(entry, atmosphereId)) * entry.netTransferredKJ;
	require(std::abs(air.conductedEnergyKJ - attributed)
		<= 1e-9 * std::max(1.0, std::abs(air.conductedEnergyKJ)), what);
}

// Total C*T across every linked compartment: conduction moves heat, it
// never makes or destroys any.
double chainEnergyKJ(core::StationSystemsSnapshot const& snapshot)
{
	auto total = 0.0;
	for (auto const& air : snapshot.atmospheres)
		total += air.thermalCapacityKJPerK * air.temperatureC;
	return total;
}

void requireResidualsClose(core::StationSystemsSnapshot const& snapshot, char const* what)
{
	for (auto const& air : snapshot.atmospheres)
		require(std::abs(air.residualKJ) <= 1e-6 * std::max(1.0, std::abs(air.currentEnergyKJ)), what);
}

// A four-compartment chain, hot at one end and cold at the other, with
// unequal capacities and unequal conductances so nothing can pass by
// symmetry that would not pass by the arithmetic.
core::StationSystemsSettings chainSettings(double aTemperatureC = 70.0, double bTemperatureC = 45.0)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.atmospheres = {linkCompartment("a", aTemperatureC, 12000.0),
		linkCompartment("b", bTemperatureC, 20000.0),
		linkCompartment("c", 5.0, 8000.0), linkCompartment("d", -25.0, 16000.0)};
	s.thermalLinks = {{"ab", "a", "b", 480.0}, {"bc", "b", "c", 300.0},
		{"cd", "c", "d", 720.0}};
	return s;
}

double chainAverageTemperatureC(core::StationSystemsSettings const& s)
{
	auto capacity = 0.0;
	auto weighted = 0.0;
	for (auto const& air : s.atmospheres)
	{
		capacity += air.thermalCapacityKJPerK;
		weighted += air.thermalCapacityKJPerK * air.startingTemperatureC;
	}
	return weighted / capacity;
}
} // namespace

// AC1: two half-conductance links between the same pair conduct exactly
// what one full-conductance link conducts - in the compartments and in
// the ledger, link by link, at every point of the run rather than only
// once everything has settled.
void thermalLinkParallelWallAdditivity(smoke::Context const&)
{
	std::string diagnostic;
	auto const single = linkedPair(10000.0, 8000.0, 12.0);
	// The second wall is authored the other way round on purpose: A and B
	// only fix the sign of a published flow, so a reversed wall must add
	// to the same total rather than subtract from it.
	auto halves = single;
	halves.thermalLinks = {{"half-1", "a", "b", 6.0}, {"half-2", "b", "a", 6.0}};
	// Three thirds as well, so additivity is not a property of the number
	// two and holds for any number of walls in parallel.
	auto thirds = single;
	thirds.thermalLinks = {{"third-1", "a", "b", 4.0}, {"third-2", "b", "a", 4.0},
		{"third-3", "a", "b", 4.0}};
	core::World one("ParallelSingle", 2, 2);
	core::World two("ParallelHalves", 2, 2);
	core::World three("ParallelThirds", 2, 2);
	configure(one, single, diagnostic);
	configure(two, halves, diagnostic);
	configure(three, thirds, diagnostic);
	one.resumeSimulation(); two.resumeSimulation(); three.resumeSimulation();
	for (double const horizon : {requestedStepSeconds(), 60.0, 300.0})
	{
		require(advanceSeconds(one, horizon) && advanceSeconds(two, horizon)
			&& advanceSeconds(three, horizon), "parallel wall additivity run failed");
		auto const base = one.getStationSystemsSnapshot();
		require(base.healthy, "parallel wall single-link run failed its audit");
		std::vector<core::StationSystemsSnapshot> const others = {
			two.getStationSystemsSnapshot(), three.getStationSystemsSnapshot()};
		for (auto const& other : others)
		{
			require(other.healthy, "parallel wall multi-link run failed its audit");
			for (size_t i = 0; i < 2; ++i)
				require(std::abs(other.atmospheres[i].temperatureC - base.atmospheres[i].temperatureC)
						< 1e-11
						&& std::abs(other.atmospheres[i].conductedEnergyKJ
							- base.atmospheres[i].conductedEnergyKJ)
						<= 1e-9 * std::abs(base.atmospheres[i].conductedEnergyKJ) + 1e-9,
					"parallel links did not reproduce the single-link result");
			// The ledger adds up link by link: the shares of the parallel
			// walls' own cumulative transfers are the single wall's
			// cumulative transfer, and the same for the last tick's rate.
			auto net = 0.0;
			auto rate = 0.0;
			for (auto const& entry : other.thermalLinks)
			{
				net += static_cast<double>(endpointSign(entry, "b")) * entry.netTransferredKJ;
				rate += static_cast<double>(endpointSign(entry, "b")) * entry.flowRateKW;
				require(entry.status == core::StationThermalLinkStatus::Active
					&& entry.netTransferredKJ != 0.0,
					"a parallel wall was configured but never conducted");
			}
			auto const wall = link(base, "wall");
			require(std::abs(net - wall.netTransferredKJ)
				<= 1e-9 * std::abs(wall.netTransferredKJ) + 1e-9,
				"the parallel walls' cumulative transfers were not additive");
			require(std::abs(rate - wall.flowRateKW) <= 1e-9 * std::abs(wall.flowRateKW) + 1e-9,
				"the parallel walls' last-tick rates were not additive");
			requireAttributedToItsLinks(other, "b", "a parallel wall's heat reached the wrong ledger");
			requireResidualsClose(other, "parallel walls left a compartment residual open");
		}
	}
}

// AC2: a chain of linked compartments converges to one common
// temperature across the whole chain - no compartment stranded away from
// the energy-weighted average, nothing crossing it, and nothing drifting
// once it has arrived.
void thermalLinkChainConvergence(smoke::Context const&)
{
	std::string diagnostic;
	auto const settings = chainSettings();
	auto const expected = chainAverageTemperatureC(settings);
	core::World world("ChainConvergence", 2, 2);
	configure(world, settings, diagnostic);
	auto const start = world.getStationSystemsSnapshot();
	require(start.thermalLinks.size() == 3, "the chain lost a link on configure");
	world.resumeSimulation();
	require(advanceSeconds(world, 1500), "chain convergence run failed");
	auto const settled = world.getStationSystemsSnapshot();
	require(settled.healthy, "chain convergence failed its audit");
	for (size_t i = 0; i < settled.atmospheres.size(); ++i)
	{
		// Every compartment arrives, and arrives genuinely: each one moved
		// a long way, so "within tolerance" is not "barely moved".
		require(std::abs(settled.atmospheres[i].temperatureC
			- start.atmospheres[i].temperatureC) > 1.0,
			"a compartment in the linked chain never moved toward the common temperature");
		require(std::abs(settled.atmospheres[i].temperatureC - expected) < 1e-6,
			"the linked chain left a compartment stranded away from the common temperature");
		require(settled.atmospheres[i].temperatureC >= std::min(start.atmospheres[i].temperatureC, expected)
			&& settled.atmospheres[i].temperatureC <= std::max(start.atmospheres[i].temperatureC, expected),
			"a compartment crossed the common temperature it was relaxing toward");
	}
	// The chain is quiescent at the average: the conduction that drove it
	// has nothing left to drive.
	for (auto const& entry : settled.thermalLinks)
		require(std::abs(entry.flowRateKW) < 1e-3,
			"a converged chain link was still moving heat");
	requireResidualsClose(settled, "the converged chain left a compartment residual open");
	require(advanceSeconds(world, 1500), "chain settle run failed");
	auto const held = world.getStationSystemsSnapshot();
	require(held.healthy, "chain settle run failed its audit");
	for (size_t i = 0; i < held.atmospheres.size(); ++i)
		require(std::abs(held.atmospheres[i].temperatureC - settled.atmospheres[i].temperatureC) < 1e-6,
			"the converged chain drifted after homogenizing");
	require(std::abs(chainEnergyKJ(held) - chainEnergyKJ(start))
		<= 1e-9 * std::abs(chainEnergyKJ(start)),
		"the chain's total energy was not invariant across the whole run");
}

// AC3: every link in a chain reports its own signed flow and its own
// cumulative transfer, and each is what its own position in the chain
// says it should be - no heat credited to a link that did not carry it.
void thermalLinkChainLedgerAttribution(smoke::Context const&)
{
	std::string diagnostic;
	// a and b start at the same temperature, so the first link of the
	// chain has nothing to conduct on the very first tick while the rest
	// of the chain is already moving: a link reports flow exactly when
	// its own pair has a difference, never because its neighbour did.
	auto const settings = chainSettings(60.0, 60.0);
	core::World world("ChainAttribution", 2, 2);
	configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "chain attribution first step failed");
	auto const first = world.getStationSystemsSnapshot();
	require(first.healthy, "chain attribution first step failed its audit");
	require(link(first, "ab").netTransferredKJ == 0.0
		&& link(first, "ab").flowRateKW == 0.0
		&& link(first, "ab").status == core::StationThermalLinkStatus::Idle,
		"a link between two equal-temperature compartments reported flow");
	require(link(first, "bc").netTransferredKJ > 0.0 && link(first, "cd").netTransferredKJ > 0.0,
		"the chain did not start conducting where its own differences were");
	requireAttributedToItsLinks(first, "a", "first tick: a's conducted total is not its link's");
	requireAttributedToItsLinks(first, "b", "first tick: b's conducted total is not its links'");
	requireAttributedToItsLinks(first, "c", "first tick: c's conducted total is not its links'");
	requireAttributedToItsLinks(first, "d", "first tick: d's conducted total is not its link's");
	// Now run the whole relaxation, checking the books every tick.
	auto const startEnergy = chainEnergyKJ(first);
	while (world.getStationSystemsSnapshot().elapsedSeconds < 1500.0)
	{
		require(advanceStationSteps(world, 1), "chain attribution step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "chain attribution run failed its audit");
		requireAttributedToItsLinks(snapshot, "a", "a's conducted heat was not its own link's");
		requireAttributedToItsLinks(snapshot, "b",
			"the shared compartment double-counted or dropped a link");
		requireAttributedToItsLinks(snapshot, "c",
			"the shared compartment double-counted or dropped a link");
		requireAttributedToItsLinks(snapshot, "d", "d's conducted heat was not its own link's");
		requireResidualsClose(snapshot, "the chain left a compartment residual open mid-run");
		require(std::abs(chainEnergyKJ(snapshot) - startEnergy)
			<= 1e-9 * std::abs(startEnergy),
			"the chain's total energy drifted while it relaxed");
		// The first link woke up: it had nothing to do on tick one and
		// has been conducting ever since a and b separated.
		if (snapshot.elapsedSeconds > first.elapsedSeconds + 1.0)
			require(link(snapshot, "ab").flowRateKW > 0.0
				&& link(snapshot, "ab").status == core::StationThermalLinkStatus::Active,
				"the first link never started conducting once its own pair separated");
	}
	// And each link's whole-run total is exactly the heat that crosses
	// the cut it sits on, computed from the end state: the link between
	// two compartments carries what the compartment on its far side
	// gained or lost, and nothing else.
	auto const done = world.getStationSystemsSnapshot();
	auto const average = chainAverageTemperatureC(settings);
	auto const expectedAb = 12000.0 * (60.0 - average);
	auto const expectedBc = (8000.0 + 16000.0) * average - (8000.0 * 5.0 + 16000.0 * -25.0);
	auto const expectedCd = 16000.0 * (average - (-25.0));
	for (auto const& [id, expected] : std::vector<std::pair<char const*, double>>
		{{"ab", expectedAb}, {"bc", expectedBc}, {"cd", expectedCd}})
		require(std::abs(link(done, id).netTransferredKJ - expected)
			<= 1e-8 * std::abs(expected) + 1e-6,
			"a chain link's cumulative transfer was not the heat crossing its own cut");
}

// AC4: reshuffling the thermalLinks list in the settings document
// leaves every published temperature, rate and cumulative total
// unchanged, so an author can reorder their document freely. The pass
// is still sequential pairwise operator splitting; what #797 pins is
// that the fixed order it splits in belongs to the wiring, not to the
// order the entries were typed.
void thermalLinkDeclarationOrderIndependence(smoke::Context const&)
{
	std::string diagnostic;
	core::StationSystemsSettings base;
	base.enabled = true;
	base.atmospheres = {linkCompartment("a", 70.0, 10000.0),
		linkCompartment("b", 50.0, 15000.0), linkCompartment("c", 20.0, 8000.0),
		linkCompartment("d", -10.0, 12000.0), linkCompartment("e", 35.0, 9000.0)};
	// A chain, a second wall in parallel on the chain's first link, and
	// a pair off by itself: every shape an author can shuffle.
	base.thermalLinks = {{"ab", "a", "b", 200.0}, {"bc", "b", "c", 150.0},
		{"cd", "c", "d", 120.0}, {"ab-2", "a", "b", 100.0}, {"de", "d", "e", 90.0}};
	auto shuffled = base;
	shuffled.thermalLinks = {{"de", "d", "e", 90.0}, {"ab-2", "a", "b", 100.0},
		{"cd", "c", "d", 120.0}, {"bc", "b", "c", 150.0}, {"ab", "a", "b", 200.0}};
	core::World declared("DeclarationOrder", 2, 2);
	core::World reordered("ReorderedDocument", 2, 2);
	configure(declared, base, diagnostic);
	configure(reordered, shuffled, diagnostic);
	// The published list still follows the document, so telemetry keeps
	// the author's ordering even though the pass does not.
	auto const authored = reordered.getStationSystemsSnapshot();
	require(authored.thermalLinks.size() == base.thermalLinks.size(),
		"a reordered document lost a thermal link");
	for (size_t i = 0; i < authored.thermalLinks.size(); ++i)
		require(authored.thermalLinks[i].id == shuffled.thermalLinks[i].id,
			"the published link list no longer follows the document's order");
	declared.resumeSimulation(); reordered.resumeSimulation();
	require(advanceSeconds(declared, 600) && advanceSeconds(reordered, 600),
		"declaration-order independence run failed");
	auto const a = declared.getStationSystemsSnapshot();
	auto const b = reordered.getStationSystemsSnapshot();
	require(a.healthy && b.healthy, "declaration-order independence run failed its audit");
	for (size_t i = 0; i < a.atmospheres.size(); ++i)
		require(a.atmospheres[i].temperatureC == b.atmospheres[i].temperatureC
			&& a.atmospheres[i].conductedEnergyKJ == b.atmospheres[i].conductedEnergyKJ
			&& a.atmospheres[i].lastConductedKW == b.atmospheres[i].lastConductedKW,
			"reshuffling the link list changed a published temperature, rate or total");
	for (auto const& entry : a.thermalLinks)
	{
		auto const& twin = link(b, entry.id);
		require(entry.flowRateKW == twin.flowRateKW
			&& entry.netTransferredKJ == twin.netTransferredKJ && entry.status == twin.status,
			"reshuffling the link list changed a link's own published flow or total");
	}
	// A wall authored the other way round changes only the sign of its
	// own report: the physics it performs is bit for bit the same.
	auto reversed = base;
	reversed.thermalLinks[1] = {"bc", "c", "b", 150.0};
	core::World flipped("ReversedEndpoint", 2, 2);
	configure(flipped, reversed, diagnostic);
	flipped.resumeSimulation();
	require(advanceSeconds(flipped, 600), "reversed-endpoint run failed");
	auto const f = flipped.getStationSystemsSnapshot();
	require(f.healthy, "reversed-endpoint run failed its audit");
	for (size_t i = 0; i < a.atmospheres.size(); ++i)
		require(f.atmospheres[i].temperatureC == a.atmospheres[i].temperatureC
			&& f.atmospheres[i].conductedEnergyKJ == a.atmospheres[i].conductedEnergyKJ,
			"authoring a wall the other way round changed the physics");
	require(link(f, "bc").flowRateKW == -link(a, "bc").flowRateKW
		&& link(f, "bc").netTransferredKJ == -link(a, "bc").netTransferredKJ,
		"a reversed wall did not report the exact negation of its own flow");
}

// #836: the canonical pass order belongs to the named wiring, not to the
// order the atmospheres array was typed. The reported repro: a dry chain
// A/B/C at 40/10/-10 C with 1 kJ/K capacities and two 10 kW/K links
// published a several-kelvin different temperature for compartment a
// when only the compartment declarations were reshuffled - the link list
// and every named wire stayed identical. The pass key is now the pair of
// authored compartment names, so the two documents must publish exactly
// the same physics, compared by compartment and link identity and never
// by snapshot array position: the snapshot still presents compartments
// in the order the author declared them.
void thermalLinkCompartmentOrderIndependence(smoke::Context const&)
{
	std::string diagnostic;
	// The ticket's fixture plus an asymmetric variant: unequal capacities
	// and unequal conductances, so no symmetry can mask an order that
	// still secretly depends on declaration indices.
	struct Scenario
	{
		char const* name;
		std::array<double, 3> capacitiesKJPerK;
		std::array<double, 2> conductancesKWPerK;
	};
	for (auto const& scenario : std::vector<Scenario>{
		Scenario{"reported", {{1.0, 1.0, 1.0}}, {{10.0, 10.0}}},
		Scenario{"asymmetric", {{1.0, 1.5, 0.8}}, {{10.0, 15.0}}}})
	{
		auto const chain = [&](std::vector<std::string> const& declarationOrder)
		{
			core::StationSystemsSettings s;
			s.enabled = true;
			std::vector<core::AtmosphereSettings> rooms = {
				linkCompartment("a", 40.0, scenario.capacitiesKJPerK[0]),
				linkCompartment("b", 10.0, scenario.capacitiesKJPerK[1]),
				linkCompartment("c", -10.0, scenario.capacitiesKJPerK[2])};
			for (auto const& id : declarationOrder)
				for (auto const& room : rooms)
					if (room.id == id) s.atmospheres.push_back(room);
			// The link list is byte-identical in both documents: only the
			// compartment declarations move.
			s.thermalLinks = {{"ab", "a", "b", scenario.conductancesKWPerK[0]},
				{"bc", "b", "c", scenario.conductancesKWPerK[1]}};
			return s;
		};
		auto const forward = chain({"a", "b", "c"});
		auto const reversed = chain({"c", "b", "a"});
		core::World declared("CompartmentOrderForward", 2, 2);
		core::World reshuffled("CompartmentOrderReversed", 2, 2);
		::configure(declared, forward, diagnostic);
		::configure(reshuffled, reversed, diagnostic);
		// Presentation stays with the document: the snapshot lists
		// compartments in the order they were declared, even though the
		// pass no longer cares about that order.
		auto const presented = reshuffled.getStationSystemsSnapshot();
		require(presented.atmospheres.size() == 3
			&& presented.atmospheres[0].id == "c" && presented.atmospheres[1].id == "b"
			&& presented.atmospheres[2].id == "a",
			"reshuffling compartment declarations changed the snapshot's presentation order");
		declared.resumeSimulation(); reshuffled.resumeSimulation();
		auto const requireSamePublishedState = [](core::StationSystemsSnapshot const& base,
			core::StationSystemsSnapshot const& other, char const* what)
		{
			for (auto const& air : base.atmospheres)
			{
				auto const& twin = compartment(other, air.id);
				require(air.temperatureC == twin.temperatureC
					&& air.conductedEnergyKJ == twin.conductedEnergyKJ
					&& air.lastConductedKW == twin.lastConductedKW,
					("reshuffling compartment declarations changed a published "
						"temperature or conducted total (" + std::string(what) + ")").c_str());
			}
			for (auto const& entry : base.thermalLinks)
			{
				auto const& twin = link(other, entry.id);
				require(entry.flowRateKW == twin.flowRateKW
					&& entry.netTransferredKJ == twin.netTransferredKJ
					&& entry.status == twin.status,
					("reshuffling compartment declarations changed a link's own published "
						"flow, total or status (" + std::string(what) + ")").c_str());
			}
		};
		// The reported comparison point: one default Station step, where
		// the ticket's probe diverged by several kelvin.
		require(advanceStationSteps(declared, 1) && advanceStationSteps(reshuffled, 1),
			"compartment-order independence first step failed");
		auto const first = declared.getStationSystemsSnapshot();
		auto const firstReversed = reshuffled.getStationSystemsSnapshot();
		require(first.healthy && firstReversed.healthy,
			"compartment-order independence run failed its audit");
		requireSamePublishedState(first, firstReversed, scenario.name);
		// Non-vacuous: the chain genuinely conducted at the comparison
		// point, so the equality above is equality of moving physics.
		require(link(first, "ab").status == core::StationThermalLinkStatus::Active
			&& link(first, "ab").netTransferredKJ != 0.0
			&& link(first, "bc").status == core::StationThermalLinkStatus::Active
			&& link(first, "bc").netTransferredKJ != 0.0,
			"the compartment-order fixture never conducted");
		require(compartment(first, "a").temperatureC != 40.0
			&& compartment(first, "c").temperatureC != -10.0,
			"the compartment-order fixture's endpoints never moved");
		// And the whole relaxation, checked every step to the horizon.
		while (declared.getStationSystemsSnapshot().elapsedSeconds < 60.0)
		{
			require(advanceStationSteps(declared, 1) && advanceStationSteps(reshuffled, 1),
				"compartment-order independence step failed");
			auto const base = declared.getStationSystemsSnapshot();
			auto const other = reshuffled.getStationSystemsSnapshot();
			require(base.healthy && other.healthy,
				"compartment-order independence run failed its audit mid-run");
			requireSamePublishedState(base, other, scenario.name);
		}
	}
}

// AC5: a compartment wired to two neighbours exchanges with each of
// them on its own terms. Each link's transfer is the closed form applied
// to its own pair's temperature difference, each is booked to itself,
// and the shared compartment's residual still closes every tick.
void thermalLinkSharedNeighbourIndependence(smoke::Context const&)
{
	auto const settings = []
	{
		core::StationSystemsSettings s;
		s.enabled = true;
		s.atmospheres = {linkCompartment("hot", 60.0, 10000.0),
			linkCompartment("shared", 30.0, 12000.0), linkCompartment("cold", 0.0, 8000.0)};
		s.thermalLinks = {{"in", "hot", "shared", 200.0}, {"out", "shared", "cold", 150.0}};
		return s;
	};
	std::string diagnostic;
	core::World world("SharedNeighbour", 2, 2);
	core::World other("SharedNeighbourPerturbed", 2, 2);
	configure(world, settings(), diagnostic);
	auto perturbed = settings();
	perturbed.atmospheres[2].startingTemperatureC = 40.0;
	configure(other, perturbed, diagnostic);
	auto const dt = world.getStationSystemsSnapshot().systemsTimestepSeconds;
	require(dt > 0.0, "the shared-neighbour fixture has no systems step");
	world.resumeSimulation(); other.resumeSimulation();
	require(advanceStationSteps(world, 1) && advanceStationSteps(other, 1),
		"shared-neighbour first step failed");
	auto const stepOne = world.getStationSystemsSnapshot();
	auto const stepOnePerturbed = other.getStationSystemsSnapshot();
	require(stepOne.healthy && stepOnePerturbed.healthy,
		"shared-neighbour first step failed its audit");
	// The neighbour a link does not touch cannot reach into it: changing
	// only the far compartment left this link's first tick untouched,
	// while the link that does touch it answered differently.
	require(link(stepOne, "in").flowRateKW == link(stepOnePerturbed, "in").flowRateKW
		&& link(stepOne, "in").netTransferredKJ == link(stepOnePerturbed, "in").netTransferredKJ,
		"a neighbour the link never touches changed that link's flow");
	require(link(stepOne, "out").flowRateKW != link(stepOnePerturbed, "out").flowRateKW,
		"the link facing the perturbed neighbour did not respond to it");
	// Step one systems tick at a time so every published flow can be
	// checked against the closed form applied to the state the previous
	// publish left: the inbound link saw its pair as the last tick ended
	// it, and the outbound link saw the shared compartment as the inbound
	// link left it. Neither one is a share of some global flow.
	auto previous = stepOne;
	while (world.getStationSystemsSnapshot().elapsedSeconds < 600.0)
	{
		require(advanceStationSteps(world, 1), "shared-neighbour step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "shared-neighbour run failed its audit");
		requireAttributedToItsLinks(snapshot, "hot", "the donor's conducted heat was not its own link's");
		requireAttributedToItsLinks(snapshot, "shared",
			"the shared compartment's conducted total is not the sum of its two links");
		requireAttributedToItsLinks(snapshot, "cold", "the receiver's conducted heat was not its own link's");
		requireResidualsClose(snapshot, "a compartment with two links left its residual open");
		require(std::abs(chainEnergyKJ(snapshot) - chainEnergyKJ(previous))
			<= 1e-9 * std::max(1.0, std::abs(chainEnergyKJ(previous))),
			"the three-compartment set did not conserve energy across a tick");
		auto const inFlow = link(snapshot, "in").flowRateKW;
		auto const outFlow = link(snapshot, "out").flowRateKW;
		require(inFlow > 0.0 && outFlow > 0.0,
			"the shared compartment did not exchange with both neighbours at once");
		require(link(snapshot, "in").status == core::StationThermalLinkStatus::Active
			&& link(snapshot, "out").status == core::StationThermalLinkStatus::Active,
			"a live exchange reported itself idle");
		auto const& hot = compartment(previous, "hot");
		auto const& shared = compartment(previous, "shared");
		auto const& cold = compartment(previous, "cold");
		auto const expectedIn = linkTransferKJ(hot.thermalCapacityKJPerK,
			shared.thermalCapacityKJPerK, hot.temperatureC - shared.temperatureC,
			link(previous, "in").conductanceKWPerK, dt);
		auto const sharedAfterIn = shared.temperatureC + expectedIn / shared.thermalCapacityKJPerK;
		auto const expectedOut = linkTransferKJ(shared.thermalCapacityKJPerK, cold.thermalCapacityKJPerK,
			sharedAfterIn - cold.temperatureC, link(previous, "out").conductanceKWPerK, dt);
		require(std::abs(inFlow * dt - expectedIn) <= 1e-9 * std::abs(expectedIn) + 1e-9,
			"the inbound link's flow was not its own pair's closed-form transfer");
		require(std::abs(outFlow * dt - expectedOut) <= 1e-9 * std::abs(expectedOut) + 1e-9,
			"the outbound link's flow was not its own pair's closed-form transfer");
		// The shared compartment takes from the hot neighbour and gives to
		// the cold one and stays between the two the whole way.
		require(snapshot.atmospheres[1].temperatureC <= snapshot.atmospheres[0].temperatureC + 1e-9
			&& snapshot.atmospheres[2].temperatureC <= snapshot.atmospheres[1].temperatureC + 1e-9,
			"the shared compartment crossed one of its neighbours");
		previous = snapshot;
	}
}
}

// #834: use World::updateStationSystemsSettings rather than the runtime
// directly. The saved settings, published runtime snapshot, and subsequent
// evolution must all describe the same live Thermal link.
void thermalLinkLiveSettings(smoke::Context const&)
{
	std::string diagnostic;
	auto settings = linkedPair();
	auto spare = linkCompartment("spare", 20.0, 10000.0);
	settings.atmospheres.push_back(spare);
	core::World world("ThermalLinkLiveSettings", 2, 2);
	::configure(world, settings, diagnostic);
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "initial live-settings step failed");
	auto const before = world.getStationSystemsSnapshot();
	auto const conductedBefore = before.atmospheres[1].conductedEnergyKJ;
	auto const temperatureBefore = before.atmospheres[1].temperatureC;
	auto const trendCount = world.getStationSystemsTrends().size();

	auto retuned = world.getStationSystemsSettings();
	retuned.thermalLinks[0].conductanceKWPerK = 100.0;
	require(world.updateStationSystemsSettings(retuned, &diagnostic),
		("live conductance update refused: " + diagnostic).c_str());
	auto const updated = world.getStationSystemsSnapshot();
	require(world.getStationSystemsSettings().thermalLinks[0].conductanceKWPerK == 100.0
		&& link(updated, "wall").conductanceKWPerK == 100.0,
		"saved settings and live link snapshot disagree after a conductance update");
	require(updated.atmospheres[1].conductedEnergyKJ == conductedBefore
		&& updated.atmospheres[1].temperatureC == temperatureBefore
		&& world.getStationSystemsTrends().size() == trendCount,
		"a live conductance update disturbed accumulated state or history");
	require(advanceStationSteps(world, 1), "retuned live-settings step failed");
	auto const evolved = world.getStationSystemsSnapshot();
	require(evolved.atmospheres[1].conductedEnergyKJ > conductedBefore
		&& evolved.atmospheres[1].temperatureC > temperatureBefore
		&& link(evolved, "wall").flowRateKW > link(before, "wall").flowRateKW,
		"updated conductance did not affect the next live step");

	// Every frozen-topology refusal is atomic even when accompanied by an
	// otherwise valid conductance edit.
	auto expectRefusalUnchanged = [&](core::StationSystemsSettings offered,
		char const* description)
	{
		offered.thermalLinks[0].conductanceKWPerK = 55.0;
		auto const state = world.getStationSystemsSnapshot();
		require(!world.updateStationSystemsSettings(offered, &diagnostic), description);
		require(diagnostic.find("thermal link") != std::string::npos,
			"thermal-link topology refusal lacked a useful diagnostic");
		auto const after = world.getStationSystemsSnapshot();
		require(world.getStationSystemsSettings().thermalLinks.size() == 1
			&& world.getStationSystemsSettings().thermalLinks[0].id == "wall"
			&& world.getStationSystemsSettings().thermalLinks[0].conductanceKWPerK == 100.0
			&& after.thermalLinks.size() == 1
			&& link(after, "wall").conductanceKWPerK == 100.0
			&& after.atmospheres[1].temperatureC == state.atmospheres[1].temperatureC
			&& after.atmospheres[1].conductedEnergyKJ == state.atmospheres[1].conductedEnergyKJ,
			"a refused thermal-link offer changed authored or runtime state");
	};
	auto removed = world.getStationSystemsSettings();
	removed.thermalLinks.clear();
	// Keep one entry for the mixed-parameter helper; this separate removal
	// verifies the collection cardinality boundary.
	require(!world.updateStationSystemsSettings(removed, &diagnostic),
		"removing a thermal link was accepted");
	require(world.getStationSystemsSettings().thermalLinks.size() == 1,
		"removing a thermal link changed saved settings");
	auto renamed = world.getStationSystemsSettings();
	renamed.thermalLinks[0].id = "renamed";
	expectRefusalUnchanged(renamed, "renaming a thermal link was accepted");
	auto rewired = world.getStationSystemsSettings();
	rewired.thermalLinks[0].bAtmosphereId = "spare";
	expectRefusalUnchanged(rewired, "rewiring a thermal link was accepted");
	auto added = world.getStationSystemsSettings();
	added.thermalLinks.push_back({"second", "a", "b", 10.0});
	require(!world.updateStationSystemsSettings(added, &diagnostic),
		"adding a thermal link was accepted");
	require(world.getStationSystemsSettings().thermalLinks.size() == 1
		&& link(world.getStationSystemsSnapshot(), "wall").conductanceKWPerK == 100.0,
		"adding a thermal link changed the running scenario");
}

void registerStationThermalLinkChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"thermalLinkConservation", thermalLinkConservation});
	checks.push_back({"thermalLinkZeroCelsiusAudit", thermalLinkZeroCelsiusAudit});
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
	checks.push_back({"thermalLinkPersistence", thermalLinkPersistence});
	checks.push_back({"thermalLinkSameTickCondensation", thermalLinkSameTickCondensation});
	checks.push_back({"thermalLinkReverseNoSpuriousCondensation",
		thermalLinkReverseNoSpuriousCondensation});
	checks.push_back({"thermalLinkChainHomogenization", thermalLinkChainHomogenization});
	checks.push_back({"thermalLinkAbsurdGeometry", thermalLinkAbsurdGeometry});
	checks.push_back({"thermalLinkParallelWallAdditivity", thermalLinkParallelWallAdditivity});
	checks.push_back({"thermalLinkChainConvergence", thermalLinkChainConvergence});
	checks.push_back({"thermalLinkChainLedgerAttribution", thermalLinkChainLedgerAttribution});
	checks.push_back({"thermalLinkDeclarationOrderIndependence",
		thermalLinkDeclarationOrderIndependence});
	checks.push_back({"thermalLinkCompartmentOrderIndependence",
		thermalLinkCompartmentOrderIndependence});
	checks.push_back({"thermalLinkSharedNeighbourIndependence",
		thermalLinkSharedNeighbourIndependence});
	checks.push_back({"thermalLinkLiveSettings", thermalLinkLiveSettings});
}
