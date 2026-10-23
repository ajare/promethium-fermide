#include "StationRig.h"
#include "Smoke.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Advected enthalpy checks (#802): the heat that rides with gas crossing
// an Atmosphere pathway. Every assertion is made on what a configured
// World publishes after being advanced - never on integrator internals -
// so replacing the closed forms with any other scheme producing the same
// published behaviour still passes. The two forms under test:
//   between compartments (difference form, Celsius-safe):
//     dT_dest = |m| * cp_source * (T_source - T_dest) / C_dest
//   venting to Exterior vacuum (Kelvin-mandatory multiplicative form):
//     T_K_new = T_K_old * exp(-cp * m_vented / C)
namespace
{
using namespace smoke::station;

// Vapour is kept far below saturation at the coldest temperature any
// fixture reaches, so no condensation latent heat enters the energy
// accounting and every joule moved in these runs is the pathway's own.
core::AtmosphereSettings advectedCompartment(std::string const& id, double temperatureC,
	double capacityKJPerK, double specificHeatKJPerKgK = core::StationDefaultAdvectedSpecificHeatKJPerKgK)
{
	core::AtmosphereSettings a;
	a.id = id;
	a.volumeM3 = 100;
	a.thermalCapacityKJPerK = capacityKJPerK;
	a.advectedSpecificHeatKJPerKgK = specificHeatKJPerKgK;
	a.startingTemperatureC = temperatureC;
	a.startingOxygenKg = 30;
	a.startingNitrogenKg = 80;
	a.startingCarbonDioxideKg = 0.6;
	a.startingWaterVapourKg = 0.001;
	a.minPressureKPa = 1;
	a.maxPressureKPa = 200;
	return a;
}

core::StationSystemsSettings advectedPair(double aTemperatureC = 40.0, double bTemperatureC = 0.0,
	double aCapacity = 10000.0, double bCapacity = 10000.0)
{
	core::StationSystemsSettings s;
	s.enabled = true;
	s.atmospheres = {advectedCompartment("a", aTemperatureC, aCapacity),
		advectedCompartment("b", bTemperatureC, bCapacity)};
	s.atmospherePathways = {{"hatch", "a", "b", 1.0, 1.0}};
	return s;
}

void configure(core::World& world, core::StationSystemsSettings const& settings,
	std::string& diagnostic)
{
	world.pauseSimulation(); world.finishBuild();
	if (!world.configureStationSystems(settings, false, &diagnostic))
		throw std::runtime_error("configure refused: [" + diagnostic + "]");
}

core::StationAtmosphereSnapshot air(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.atmospheres)
		if (entry.id == id) return entry;
	throw std::runtime_error("atmosphere missing from snapshot");
}

core::StationAtmospherePathwaySnapshot pathway(core::StationSystemsSnapshot const& snap,
	std::string const& id)
{
	for (auto const& entry : snap.atmospherePathways)
		if (entry.id == id) return entry;
	throw std::runtime_error("pathway missing from snapshot");
}

std::array<double, 4> masses(core::StationAtmosphereSnapshot const& a)
{
	return {a.oxygenKg, a.nitrogenKg, a.carbonDioxideKg, a.waterVapourKg};
}

void close(double a, double b, char const* message)
{
	require(std::abs(a - b) < 1e-9, message);
}

// Relative closeness for accumulated ledger sums: a kJ total telescoped
// over hundreds of ticks carries rounding far above an absolute 1e-9.
void closeRel(double a, double b, char const* message)
{
	require(std::abs(a - b) <= 1e-9 * std::max(1.0, std::max(std::abs(a), std::abs(b))),
		message);
}

// Fast lane 1: sign and direction. Hot gas into a cold compartment warms
// it and cools its donor; the reverse flow cools the hot receiver. A
// flipped sign fails here immediately, in both endpoint wirings.
void advectedSignDirection(smoke::Context const&)
{
	for (bool reverseWiring : {false, true})
	{
		auto s = advectedPair(40.0, 0.0);
		if (reverseWiring)
			s.atmospherePathways = {{"hatch", "b", "a", 1.0, 1.0}};
		core::World world("AdvectedSign", 2, 2);
		std::string diagnostic;
		configure(world, s, diagnostic);
		auto const initial = world.getStationSystemsSnapshot();
		world.resumeSimulation();
		require(advanceSeconds(world, 60), "advected sign run failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "advected sign run failed its audit");
		auto const& hot = air(snap, "a");
		auto const& cold = air(snap, "b");
		require(hot.temperatureC < initial.atmospheres[0].temperatureC - 1e-9,
			"the hot donor did not cool as it sent gas out");
		require(cold.temperatureC > initial.atmospheres[1].temperatureC + 1e-9,
			"the cold receiver did not warm as hot gas arrived");
		// The temperature moves are exactly the booked advected terms.
		closeRel(s.atmospheres[0].thermalCapacityKJPerK
			* (hot.temperatureC - initial.atmospheres[0].temperatureC),
			hot.advectedEnergyKJ, "donor temperature move is not its booked advected loss");
		closeRel(s.atmospheres[1].thermalCapacityKJPerK
			* (cold.temperatureC - initial.atmospheres[1].temperatureC),
			cold.advectedEnergyKJ, "receiver temperature move is not its booked advected gain");
		require(hot.advectedEnergyKJ < 0.0 && cold.advectedEnergyKJ > 0.0,
			"advected energy signed the wrong way between donor and receiver");
		// Exact negation per side; the pathway's book carries the same
		// magnitude with the sign its A/B wiring gives the hot-to-cold
		// crossing.
		close(hot.advectedEnergyKJ, -cold.advectedEnergyKJ,
			"advected enthalpy was not booked as an exact negation per side");
		require(std::abs(pathway(snap, "hatch").advectedKJ)
			== cold.advectedEnergyKJ,
			"the pathway's advected book disagrees with the receiver's gain");
		require((reverseWiring ? -1.0 : 1.0) * pathway(snap, "hatch").advectedKJ > 0.0,
			"the pathway's advected sign does not follow its endpoint wiring");
	}
	// And the mirrored world: cold source, hot receiver - the hot room
	// cools when cold gas is drawn into it.
	{
		auto s = advectedPair(0.0, 40.0);
		core::World world("AdvectedSignMirror", 2, 2);
		std::string diagnostic;
		configure(world, s, diagnostic);
		auto const initial = world.getStationSystemsSnapshot();
		world.resumeSimulation();
		require(advanceSeconds(world, 60), "advected sign mirror run failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy && air(snap, "a").temperatureC > initial.atmospheres[0].temperatureC
			&& air(snap, "b").temperatureC < initial.atmospheres[1].temperatureC,
			"cold gas drawn into the hot compartment did not cool it");
	}
}

// Fast lane 2: equal temperature is inert. Two compartments at the same
// temperature exchange gas (different compositions keep the gradients
// alive) with no temperature change at all - mass flow alone is not a
// heat source, and the difference form's zero is pinned exactly.
void advectedEqualTemperatureInert(smoke::Context const&)
{
	auto s = advectedPair(20.0, 20.0);
	// Unequal inventories so the per-species gradients really move gas
	// even with equal temperatures.
	s.atmospheres[0].startingOxygenKg = 60;
	s.atmospheres[0].startingNitrogenKg = 160;
	core::World world("AdvectedInert", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	while (world.getStationSystemsSnapshot().elapsedSeconds < 60)
	{
		require(advanceStationSteps(world, 1), "advected inert step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "advected inert run failed its audit");
		for (size_t i = 0; i < 2; ++i)
		{
			require(snap.atmospheres[i].temperatureC == 20.0,
				"equal-temperature gas exchange changed a temperature");
			require(snap.atmospheres[i].advectedEnergyKJ == 0.0,
				"equal-temperature exchange booked advected heat");
		}
	}
	auto const final = world.getStationSystemsSnapshot();
	bool moved = false;
	for (size_t gas = 0; gas < 4; ++gas)
		if (std::abs(final.atmospherePathways[0].netTransferredKg[gas]) > 1e-3) moved = true;
	require(moved, "the equal-temperature fixture never exchanged gas, so inertness proved nothing");
	require(final.atmospherePathways[0].advectedKJ == 0.0,
		"the pathway booked advected heat across equal temperatures");
}

// Fast lane 3: vent cooling matches the closed form. The published
// temperature is verified against T_old_K * exp(-cp * dm_total / C)
// computed independently in the test from the vented mass the
// compartment itself reports.
void advectedVentClosedForm(smoke::Context const&)
{
	auto s = core::StationSystemsSettings{};
	s.enabled = true;
	s.atmospheres = {advectedCompartment("can", 20.0, 10000.0),
		advectedCompartment("neighbour", 20.0, 10000.0)};
	s.atmospherePathways = {{"vent", "can", core::StationExteriorAtmosphereId, 1.0, 1.0}};
	core::World world("AdvectedVent", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	// Check the invariant at several points along the vent, not just the
	// end: the per-tick factors telescope, so the closed form in the
	// cumulative vented mass must hold at every publication.
	for (double const target : {10.0, 30.0, 60.0})
	{
		while (world.getStationSystemsSnapshot().elapsedSeconds < target)
			require(advanceStationSteps(world, 1), "advected vent step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "advected vent run failed its audit");
		auto const& can = air(snap, "can");
		auto const& canStart = air(initial, "can");
		double ventedKg = 0.0;
		for (auto const lost : can.exteriorLostKg) ventedKg += lost;
		require(ventedKg > 0.0, "the vent pathway never vented anything");
		auto const expectedK = (canStart.temperatureC + 273.15)
			* std::exp(-s.atmospheres[0].advectedSpecificHeatKJPerKgK
				* ventedKg / s.atmospheres[0].thermalCapacityKJPerK);
		closeRel(can.temperatureC + 273.15, expectedK,
			"vent cooling departed from T_old * exp(-cp * dm / C)");
		require(can.temperatureC < canStart.temperatureC,
			"venting did not cool the compartment");
		closeRel(can.advectedEnergyKJ,
			s.atmospheres[0].thermalCapacityKJPerK
			* (can.temperatureC - canStart.temperatureC),
			"the booked advected loss disagrees with capacity x the temperature drop");
	}
}

// Fast lane 4: flimsy versus vault. The headline realism test - the
// same vent through the same opening, two authored capacities, and the
// outcomes differ by what the formula says: a thin inflatable goes
// properly cold, a heavily-built habitation module barely notices.
void advectedFlimsyVersusVault(smoke::Context const&)
{
	auto flimsy = core::StationSystemsSettings{};
	flimsy.enabled = true;
	flimsy.atmospheres = {advectedCompartment("module", 20.0, 500.0),
		advectedCompartment("neighbour", 20.0, 10000.0)};
	flimsy.atmospherePathways = {{"vent", "module", core::StationExteriorAtmosphereId, 1.0, 1.0}};
	auto vault = flimsy;
	vault.atmospheres[0].thermalCapacityKJPerK = 40000.0;

	core::World thin("FlimsyModule", 2, 2), heavy("VaultModule", 2, 2);
	std::string diagnostic;
	configure(thin, flimsy, diagnostic);
	configure(heavy, vault, diagnostic);
	auto const thinInitial = air(thin.getStationSystemsSnapshot(), "module").temperatureC;
	auto const heavyInitial = air(heavy.getStationSystemsSnapshot(), "module").temperatureC;
	thin.resumeSimulation(); heavy.resumeSimulation();
	require(advanceSeconds(thin, 600), "flimsy vent run failed");
	require(advanceSeconds(heavy, 600), "vault vent run failed");
	auto const thinSnap = thin.getStationSystemsSnapshot();
	auto const heavySnap = heavy.getStationSystemsSnapshot();
	require(thinSnap.healthy && heavySnap.healthy, "vent realism runs failed their audits");
	auto const& thinModule = air(thinSnap, "module");
	auto const& heavyModule = air(heavySnap, "module");
	auto const thinDrop = thinInitial - thinModule.temperatureC;
	auto const heavyDrop = heavyInitial - heavyModule.temperatureC;
	// Each matches its own closed form against its own vented mass.
	auto const ventedOf = [](core::StationAtmosphereSnapshot const& a)
	{
		double total = 0.0;
		for (auto const lost : a.exteriorLostKg) total += lost;
		return total;
	};
	closeRel(thinModule.temperatureC + 273.15,
		(thinInitial + 273.15) * std::exp(-flimsy.atmospheres[0].advectedSpecificHeatKJPerKgK
			* ventedOf(thinModule) / flimsy.atmospheres[0].thermalCapacityKJPerK),
		"the flimsy module departed from its closed form");
	closeRel(heavyModule.temperatureC + 273.15,
		(heavyInitial + 273.15) * std::exp(-vault.atmospheres[0].advectedSpecificHeatKJPerKgK
			* ventedOf(heavyModule) / vault.atmospheres[0].thermalCapacityKJPerK),
		"the vault module departed from its closed form");
	// The authored capacity is the realism lever: the thin module goes
	// properly cold (the ticket's -43 C scale), the built module barely
	// notices (sub-degree), and the two differ by more than an order of
	// magnitude.
	require(thinDrop > 30.0, "the thin inflatable did not go properly cold when vented");
	require(heavyDrop < 2.0, "the heavily-built module cooled far more than it should");
	require(thinDrop > 20.0 * heavyDrop,
		"the flimsy-versus-vault contrast did not reach the ratio the formula says");
}

// Fast lane 5: mass untouched. A long advective run with a live
// temperature difference changes no gas inventory by any amount beyond
// what the pathway's own mass ledger already accounts - the enthalpy
// term must not perturb the mass accounting that already works.
void advectedMassUntouched(smoke::Context const&)
{
	auto s = advectedPair(60.0, 5.0);
	core::World world("AdvectedMass", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	auto const initialA = masses(initial.atmospheres[0]);
	auto const initialB = masses(initial.atmospheres[1]);
	world.resumeSimulation();
	require(advanceSeconds(world, 300), "advected mass run failed");
	auto const snap = world.getStationSystemsSnapshot();
	require(snap.healthy, "advected mass run failed its audit");
	// The run was genuinely advective, not an idle comparison.
	require(air(snap, "a").advectedEnergyKJ < -1.0
		&& air(snap, "b").advectedEnergyKJ > 1.0,
		"the advected-mass fixture never actually advected heat");
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const a = masses(air(snap, "a"))[gas];
		auto const b = masses(air(snap, "b"))[gas];
		close(a + b, initialA[gas] + initialB[gas],
			"the advected run lost or invented gas");
		close(b - initialB[gas], pathway(snap, "hatch").netTransferredKg[gas],
			"receiver inventory disagrees with the pathway mass ledger");
		close(initialA[gas] - a, pathway(snap, "hatch").netTransferredKg[gas],
			"donor depletion disagrees with the pathway mass ledger");
	}
}

// Fast lane 6: the compartment energy ledger closes with advectedKJ
// present. The audit's healthy flag is the teeth: if the booking were
// omitted the tick would fail loudly rather than drift - so this check
// asserts closure at the published seam and proves the term is real.
void advectedLedgerCloses(smoke::Context const&)
{
	auto s = advectedPair(50.0, 10.0);
	core::World world("AdvectedLedger", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	world.resumeSimulation();
	while (world.getStationSystemsSnapshot().elapsedSeconds < 120)
	{
		require(advanceStationSteps(world, 1), "advected ledger step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "advected ledger failed the tick audit - a booking is wrong");
		for (auto const& a : snap.atmospheres)
		{
			require(std::abs(a.residualKJ) <= 1e-6 * std::max(1.0, std::abs(a.currentEnergyKJ)),
				"compartment energy residual did not close with the advected term booked");
		}
	}
	auto const final = world.getStationSystemsSnapshot();
	require(air(final, "a").advectedEnergyKJ != 0.0
		&& air(final, "b").advectedEnergyKJ != 0.0,
		"the ledger closed without ever booking advected heat - vacuous");
	close(air(final, "a").advectedEnergyKJ, -air(final, "b").advectedEnergyKJ,
		"the two sides' advected totals are not exact negations");
	close(air(final, "b").advectedEnergyKJ, pathway(final, "hatch").advectedKJ,
		"the receiver's advected total disagrees with the pathway's book");
}

// Fast lane 8: advected and conducted are independent. A link and a
// pathway wired between the same pair both report separately and
// neither double-counts: each compartment's temperature move is exactly
// its conducted plus its advected booking, and each ledger matches its
// own aggregate's book.
void advectedConductedIndependence(smoke::Context const&)
{
	auto s = advectedPair(40.0, 0.0);
	s.thermalLinks = {{"wall", "a", "b", 5.0}};
	core::World world("AdvectedPlusConducted", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	require(advanceSeconds(world, 60), "advected+conducted run failed");
	auto const snap = world.getStationSystemsSnapshot();
	require(snap.healthy, "advected+conducted run failed its audit");
	for (size_t i = 0; i < 2; ++i)
	{
		auto const& a = snap.atmospheres[i];
		require(a.advectedEnergyKJ != 0.0, "the pathway booked no advected heat");
		require(a.conductedEnergyKJ != 0.0, "the link booked no conducted heat");
		// The temperature move is exactly the sum of the two separately
		// booked terms - neither mechanism double-counts the other's
		// contribution.
		closeRel(s.atmospheres[i].thermalCapacityKJPerK
			* (a.temperatureC - initial.atmospheres[i].temperatureC),
			a.advectedEnergyKJ + a.conductedEnergyKJ,
			"temperature move is not the sum of the separately booked advected and conducted terms");
	}
	// Each aggregate's book matches its own side-to-side transfer: the
	// link's conducted total and the pathway's advected total are the
	// receiver-side gains, never blended.
	for (auto const& linkEntry : snap.thermalLinks)
		require(linkEntry.netTransferredKJ == snap.atmospheres[1].conductedEnergyKJ,
			"the link's book disagrees with the receiver's conducted total");
	require(pathway(snap, "hatch").advectedKJ == snap.atmospheres[1].advectedEnergyKJ,
		"the pathway's book disagrees with the receiver's advected total");
}

// Fast lane 10: vent loss booked explicitly. Energy carried out to
// vacuum appears as a loss on the compartment and on the pathway,
// never as nothing.
void advectedVentLossBooked(smoke::Context const&)
{
	auto s = core::StationSystemsSettings{};
	s.enabled = true;
	s.atmospheres = {advectedCompartment("can", 25.0, 2000.0),
		advectedCompartment("neighbour", 25.0, 10000.0)};
	s.atmospherePathways = {{"vent", "can", core::StationExteriorAtmosphereId, 0.5, 1.0}};
	core::World world("AdvectedVentLoss", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	world.resumeSimulation();
	require(advanceSeconds(world, 60), "advected vent-loss run failed");
	auto const snap = world.getStationSystemsSnapshot();
	require(snap.healthy, "advected vent-loss run failed its audit");
	auto const& can = air(snap, "can");
	double ventedKg = 0.0;
	for (auto const lost : can.exteriorLostKg) ventedKg += lost;
	require(ventedKg > 1.0, "the vent-loss fixture did not vent a meaningful mass");
	require(can.advectedEnergyKJ < 0.0,
		"energy carried out to vacuum was not booked as an explicit loss");
	require(std::isfinite(can.advectedEnergyKJ), "the advected loss went non-finite");
	// The pathway reports the same loss with the sign its wiring gives
	// the compartment-to-exterior crossing, and the neighbour - which
	// the vent does not touch - booked nothing.
	require(pathway(snap, "vent").advectedKJ == -can.advectedEnergyKJ,
		"the vent pathway did not book the energy it carried to vacuum");
	require(air(snap, "neighbour").advectedEnergyKJ == 0.0,
		"an untouched compartment booked advected heat from a neighbour's vent");
}

// Fast lane 11: validation refusals. Zero, negative and non-finite
// specific heat are each refused by name, and a refusal mutates nothing.
void advectedValidation(smoke::Context const&)
{
	std::string diagnostic;
	core::StationSystemsSettings good;
	good.enabled = true;
	good.atmospheres = {advectedCompartment("a", 40.0, 10000.0),
		advectedCompartment("b", 0.0, 10000.0)};
	good.atmospherePathways = {{"hatch", "a", "b", 1.0, 1.0}};
	require(core::validateStationSystemsSettings(good, diagnostic), diagnostic.c_str());
	auto const refusal = [&](std::string const& badId, double value)
	{
		auto bad = good;
		bad.atmospheres[0].id = badId;
		bad.atmospheres[0].advectedSpecificHeatKJPerKgK = value;
		return !core::validateStationSystemsSettings(bad, diagnostic)
			&& diagnostic.find(badId) != std::string::npos
			&& diagnostic.find("advected specific heat") != std::string::npos;
	};
	require(refusal("zero", 0.0), "a zero advected specific heat was accepted");
	require(refusal("negative", -1.0), "a negative advected specific heat was accepted");
	require(refusal("nan", std::numeric_limits<double>::quiet_NaN()),
		"a NaN advected specific heat was accepted");
	require(refusal("infinite", std::numeric_limits<double>::infinity()),
		"an infinite advected specific heat was accepted");
	// No refusal mutates the World.
	core::World world("AdvectedValidationWorld", 2, 2);
	configure(world, good, diagnostic);
	auto const baseline = world.getStationSystemsSnapshot();
	for (auto value : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
		std::numeric_limits<double>::infinity()})
	{
		auto mutated = good;
		mutated.atmospheres[0].advectedSpecificHeatKJPerKgK = value;
		require(!world.configureStationSystems(mutated, false, &diagnostic)
			&& world.getStationSystemsSnapshot() == baseline
			&& world.getStationSystemsSettings() == good,
			"a refused advected specific heat mutated the World");
	}
}

// Stress lane 12: vent to near-empty over a long horizon. Temperature
// stays finite and bounded, no negative or zero absolute temperature is
// reached, and no non-finite value is ever published.
void advectedVentNearEmpty(smoke::Context const&)
{
	auto s = core::StationSystemsSettings{};
	s.enabled = true;
	s.atmospheres = {advectedCompartment("can", 20.0, 500.0),
		advectedCompartment("neighbour", 20.0, 10000.0)};
	s.atmospherePathways = {{"vent", "can", core::StationExteriorAtmosphereId, 1.0, 1.0}};
	core::World world("AdvectedNearEmpty", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	world.resumeSimulation();
	size_t ticks = 0;
	while (ticks < 5000)
	{
		require(advanceStationSteps(world, 1), "near-empty vent step failed");
		++ticks;
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "near-empty vent failed the tick audit");
		auto const& can = air(snap, "can");
		require(std::isfinite(can.temperatureC) && std::isfinite(can.advectedEnergyKJ),
			"the near-empty vent published a non-finite thermal value");
		require(can.temperatureC > -273.15,
			"the near-empty vent reached or crossed absolute zero");
		require(can.advectedEnergyKJ <= 0.0,
			"the vent booked a gain while losing air");
	}
	// After five thousand ticks the air is essentially gone and the
	// temperature has settled at a finite cold floor well above zero K.
	auto const final = air(world.getStationSystemsSnapshot(), "can");
	require(final.temperatureC > -200.0,
		"the near-empty vent cooled below its physical floor");
}

// Stress lane 13: absurd specific heat against tiny capacity. The
// closed form must hold what is claimed of it: the exponential simply
// lands on the absolute-zero floor and stays finite, bounded and
// ledger-closed for thousands of ticks, whatever the exponent.
void advectedAbsurdSpecificHeat(smoke::Context const&)
{
	auto s = core::StationSystemsSettings{};
	s.enabled = true;
	s.atmospheres = {advectedCompartment("impossible", 20.0, 1.0, 1.0e6),
		advectedCompartment("neighbour", 20.0, 10000.0)};
	s.atmospherePathways = {{"vent", "impossible", core::StationExteriorAtmosphereId, 1.0, 1.0}};
	core::World world("AdvectedAbsurd", 2, 2);
	std::string diagnostic;
	configure(world, s, diagnostic);
	world.resumeSimulation();
	for (size_t tick = 0; tick < 3000; ++tick)
	{
		require(advanceStationSteps(world, 1), "absurd specific-heat step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "absurd specific heat broke the tick audit");
		auto const& can = air(snap, "impossible");
		require(std::isfinite(can.temperatureC) && std::isfinite(can.advectedEnergyKJ),
			"absurd specific heat published a non-finite thermal value");
		require(can.temperatureC >= -273.15 && can.temperatureC <= 20.0,
			"absurd specific heat drove the temperature outside its physical bounds");
	}
}

} // namespace

void registerStationAdvectionChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"advectedSignDirection", advectedSignDirection});
	checks.push_back({"advectedEqualTemperatureInert", advectedEqualTemperatureInert});
	checks.push_back({"advectedVentClosedForm", advectedVentClosedForm});
	checks.push_back({"advectedFlimsyVersusVault", advectedFlimsyVersusVault});
	checks.push_back({"advectedMassUntouched", advectedMassUntouched});
	checks.push_back({"advectedLedgerCloses", advectedLedgerCloses});
	checks.push_back({"advectedConductedIndependence", advectedConductedIndependence});
	checks.push_back({"advectedVentLossBooked", advectedVentLossBooked});
	checks.push_back({"advectedValidation", advectedValidation});
	checks.push_back({"advectedVentNearEmpty", advectedVentNearEmpty});
	checks.push_back({"advectedAbsurdSpecificHeat", advectedAbsurdSpecificHeat});
}
