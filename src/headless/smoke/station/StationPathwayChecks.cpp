#include "StationRig.h"
#include "Smoke.h"
#include <array>

namespace
{
using namespace smoke::station;

core::StationSystemsSettings pathwaySettings()
{
	core::StationSystemsSettings s;
	s.enabled = true;
	core::AtmosphereSettings a;
	a.id = "a"; a.volumeM3 = 100; a.thermalCapacityKJPerK = 10000;
	a.startingTemperatureC = 0; // Kelvin regression: zero Celsius is not vacuum.
	a.minPressureKPa = 1; a.maxPressureKPa = 200;
	a.startingOxygenKg = 30; a.startingNitrogenKg = 80;
	a.startingCarbonDioxideKg = 0.6; a.startingWaterVapourKg = 0.1;
	auto b = a; b.id = "b";
	b.startingOxygenKg = 10; b.startingNitrogenKg = 40;
	b.startingCarbonDioxideKg = 0.2; b.startingWaterVapourKg = 0.02;
	s.atmospheres = {a, b};
	s.atmospherePathways = {{"opening", "a", "b", 1.0, 1.0}};
	return s;
}

std::array<double, 4> masses(core::StationAtmosphereSnapshot const& a)
{
	return {a.oxygenKg, a.nitrogenKg, a.carbonDioxideKg, a.waterVapourKg};
}
void close(double a, double b, char const* message)
{
	require(std::abs(a - b) < 1e-9, message);
}
void pathwayDiffusion(smoke::Context const&)
{
	core::World world("Pathway", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	auto s = pathwaySettings();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto initial = world.getStationSystemsSnapshot();
	require(initial.atmospherePathways.size() == 1, "pathway missing from snapshot");
	auto const& opening = initial.atmospherePathways[0];
	require(opening.id == "opening" && opening.aAtmosphereId == "a"
		&& opening.bAtmosphereId == "b" && opening.areaM2 == 1 && opening.openFraction == 1
		&& opening.status == core::StationAtmospherePathwayStatus::Idle,
		"initial pathway identity/geometry/status mismatch");
	auto const a0 = masses(initial.atmospheres[0]), b0 = masses(initial.atmospheres[1]);
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "pathway first step failed");
	auto first = world.getStationSystemsSnapshot();
	require(first.healthy && first.atmospherePathways[0].status == core::StationAtmospherePathwayStatus::Active,
		"unequal partial pressures did not produce Active diffusion");
	auto const a1 = masses(first.atmospheres[0]), b1 = masses(first.atmospheres[1]);
	for (size_t gas = 0; gas < a0.size(); ++gas)
	{
		require(a1[gas] < a0[gas] && b1[gas] > b0[gas], "gas did not move down its partial-pressure gradient");
		auto const& p = first.atmospherePathways[0];
		require(p.flowRateKgPerS[gas] > 0, "positive A-to-B flux was not reported");
		close(p.flowRateKgPerS[gas] * first.systemsTimestepSeconds, p.netTransferredKg[gas], "flux rate is not a per-second rate");
		close(a0[gas] - a1[gas], p.netTransferredKg[gas], "pathway transfer missing from donor");
		close(b1[gas] - b0[gas], p.netTransferredKg[gas], "pathway transfer missing from receiver");
	}
	require(advanceSeconds(world, 420), "pathway equalization run failed");
	auto final = world.getStationSystemsSnapshot();
	require(final.healthy, "diffusion broke the station conservation audit");
	auto const af = masses(final.atmospheres[0]), bf = masses(final.atmospheres[1]);
	for (size_t gas = 0; gas < a0.size(); ++gas)
	{
		close(af[gas] + bf[gas], a0[gas] + b0[gas], "per-species mass was not conserved");
		close(a0[gas] - af[gas], final.atmospherePathways[0].netTransferredKg[gas], "cumulative net flux does not match inventory change");
		require(af[gas] >= bf[gas] && af[gas] - bf[gas] < 0.05 * (a0[gas] - b0[gas]),
			"1 m2 opening did not substantially equalize in a few minutes, or overshot equilibrium");
	}
	// Same physical opening with endpoints reversed must only reverse telemetry.
	core::World reversed("Reversed", 2, 2);
	reversed.pauseSimulation(); reversed.finishBuild();
	std::swap(s.atmospherePathways[0].aAtmosphereId, s.atmospherePathways[0].bAtmosphereId);
	require(reversed.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	reversed.resumeSimulation();
	require(advanceStationSteps(reversed, 1), "reverse pathway step failed");
	auto reverse = reversed.getStationSystemsSnapshot();
	for (size_t gas = 0; gas < a0.size(); ++gas)
	{
		close(masses(reverse.atmospheres[0])[gas], a1[gas], "undirected pathway changed physical exchange on reversal");
		close(reverse.atmospherePathways[0].flowRateKgPerS[gas], -first.atmospherePathways[0].flowRateKgPerS[gas], "reversed endpoints did not reverse signed flux");
	}
	// Equal partial pressures publish Idle with genuinely zero flux.
	reversed.pauseSimulation();
	s.atmospheres[1] = s.atmospheres[0]; s.atmospheres[1].id = "b";
	require(reversed.configureStationSystems(s, true, &diagnostic), "equilibrium configuration/reset failed");
	reversed.resumeSimulation();
	require(advanceStationSteps(reversed, 1), "equilibrium step failed");
	auto const idle = reversed.getStationSystemsSnapshot().atmospherePathways[0];
	require(idle.status == core::StationAtmospherePathwayStatus::Idle, "zero gradient was not Idle");
	for (size_t gas = 0; gas < a0.size(); ++gas)
		require(idle.flowRateKgPerS[gas] == 0 && idle.netTransferredKg[gas] == 0, "equilibrium pathway invented flow");
}

// #786: equal total moles at equal volume/temperature, but different
// composition. Swapping equal mole counts (not equal kilograms) balances
// total pressure while leaving opposing O2 and CO2 partial-pressure gradients.
void pathwayCounterFlow(smoke::Context const&)
{
	auto s = pathwaySettings();
	auto& a = s.atmospheres[0];
	auto& b = s.atmospheres[1];
	a.startingOxygenKg = 600 * core::StationMolarMassOxygenKgPerMol;
	b.startingOxygenKg = 200 * core::StationMolarMassOxygenKgPerMol;
	a.startingCarbonDioxideKg = 200 * core::StationMolarMassCarbonDioxideKgPerMol;
	b.startingCarbonDioxideKg = 600 * core::StationMolarMassCarbonDioxideKgPerMol;
	b.startingNitrogenKg = a.startingNitrogenKg;
	b.startingWaterVapourKg = a.startingWaterVapourKg;
	core::World world("CounterFlow", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = world.getStationSystemsSnapshot();
	require(std::abs(initial.atmospheres[0].pressureKPa - initial.atmospheres[1].pressureKPa) < 1e-12,
		"counter-flow fixture does not have equal total pressure to 1e-12 kPa");
	require(initial.atmospheres[0].oxygenPartialPressureKPa > initial.atmospheres[1].oxygenPartialPressureKPa
		&& initial.atmospheres[0].carbonDioxidePartialPressureKPa < initial.atmospheres[1].carbonDioxidePartialPressureKPa,
		"counter-flow fixture lacks opposing partial-pressure gradients");
	auto const a0 = masses(initial.atmospheres[0]), b0 = masses(initial.atmospheres[1]);
	world.resumeSimulation();
	// Check both the first published interval and a cumulative run, so a
	// correct first transfer cannot hide a later ledger/sign error.
	for (double duration : {initial.systemsTimestepSeconds, 30.0})
	{
		require(advanceSeconds(world, duration), "counter-flow advance failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		auto const& p = snapshot.atmospherePathways[0];
		auto const af = masses(snapshot.atmospheres[0]), bf = masses(snapshot.atmospheres[1]);
		require(snapshot.healthy && p.status == core::StationAtmospherePathwayStatus::Active,
			"counter-flow did not publish healthy Active diffusion");
		for (size_t gas = 0; gas < a0.size(); ++gas)
		{
			close(af[gas] + bf[gas], a0[gas] + b0[gas], "counter-flow per-gas inventory did not close");
			close(a0[gas] - af[gas], p.netTransferredKg[gas], "counter-flow A ledger did not close");
			close(bf[gas] - b0[gas], p.netTransferredKg[gas], "counter-flow B ledger did not close");
			if (gas == 0 || gas == 2)
			{
				double const sign = gas == 0 ? 1.0 : -1.0;
				require(sign * p.flowRateKgPerS[gas] > 1e-6 && sign * p.netTransferredKg[gas] > 1e-6
					&& sign * (a0[gas] - af[gas]) > 1e-6 && sign * (bf[gas] - b0[gas]) > 1e-6,
					"O2 and CO2 did not simultaneously flow in opposite directions");
			}
			else
			{
				require(p.flowRateKgPerS[gas] == 0 && p.netTransferredKg[gas] == 0
					&& af[gas] == a0[gas] && bf[gas] == b0[gas],
					"equal-partial-pressure gas moved alongside counter-flowing gases");
			}
		}
	}
}

void pathwayClosed(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospherePathways[0].initialOpenFraction = 0;
	// A large pressure gradient and large area must not bypass closure.
	s.atmospherePathways[0].areaM2 = 1e12;
	auto& b = s.atmospheres[1];
	b.startingOxygenKg = b.startingNitrogenKg = b.startingCarbonDioxideKg = b.startingWaterVapourKg = 0;
	core::World world("ClosedPathway", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = world.getStationSystemsSnapshot();
	require(initial.atmospheres[0].pressureKPa - initial.atmospheres[1].pressureKPa > 50,
		"closed-pathway fixture lacks a large pressure gradient");
	world.resumeSimulation();
	for (double duration : {initial.systemsTimestepSeconds, 30.0})
	{
		require(advanceSeconds(world, duration), "closed pathway advance failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		auto const& p = snapshot.atmospherePathways[0];
		require(snapshot.healthy && p.openFraction == 0 && p.status == core::StationAtmospherePathwayStatus::Idle,
			"closed pathway did not remain healthy and Idle");
		for (size_t gas = 0; gas < p.flowRateKgPerS.size(); ++gas)
			require(p.flowRateKgPerS[gas] == 0 && p.netTransferredKg[gas] == 0,
				"closed pathway reported gas transfer");
		for (size_t i = 0; i < initial.atmospheres.size(); ++i)
			require(masses(snapshot.atmospheres[i]) == masses(initial.atmospheres[i]),
				"closed pathway changed a gas inventory");
	}
}

void pathwayOpenness(smoke::Context const&)
{
	std::array<std::array<double, 4>, 3> flows{};
	size_t index = 0;
	for (double openness : {0.25, 0.5, 1.0})
	{
		auto s = pathwaySettings();
		s.atmospherePathways[0].initialOpenFraction = openness;
		// Snapshot flux is interval-averaged, not instantaneous. A small
		// area keeps gradient relaxation negligible even at step ratio 60;
		// doubling openness should double each gas's flux within 1e-5 relative.
		s.atmospherePathways[0].areaM2 = 1e-4;
		core::World world("PathwayOpenness", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		world.resumeSimulation();
		require(advanceStationSteps(world, 1), "openness step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		auto const& p = snapshot.atmospherePathways[0];
		require(snapshot.healthy && p.status == core::StationAtmospherePathwayStatus::Active && p.openFraction == openness,
			"partly open pathway did not publish Active at authored openness");
		flows[index++] = p.flowRateKgPerS;
		for (size_t gas = 0; gas < p.flowRateKgPerS.size(); ++gas)
		{
			require(p.flowRateKgPerS[gas] > 1e-12, "openness fixture has no measurable gas flow");
			close(p.flowRateKgPerS[gas] * snapshot.systemsTimestepSeconds, p.netTransferredKg[gas],
				"openness flux disagrees with interval transfer");
			close(masses(initial.atmospheres[0])[gas] - masses(snapshot.atmospheres[0])[gas], p.netTransferredKg[gas],
				"openness transfer disagrees with donor inventory");
			close(masses(snapshot.atmospheres[1])[gas] - masses(initial.atmospheres[1])[gas], p.netTransferredKg[gas],
				"openness transfer disagrees with receiver inventory");
		}
	}
	for (size_t i = 1; i < flows.size(); ++i)
		for (size_t gas = 0; gas < flows[i].size(); ++gas)
			require(std::abs(flows[i][gas] / (2 * flows[i - 1][gas]) - 1) < 1e-5,
				"doubling openness did not double gas flow within 1e-5 relative tolerance");
}

// #788: different temperatures must survive actual gas exchange, not just
// an Idle/equal-temperature fixture. Vapour stays well below saturation.
void pathwayThermalNeutrality(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospheres[0].startingTemperatureC = 40;
	s.atmospheres[1].startingTemperatureC = 0;
	core::World world("NeutralDiffusion", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	// Inspect every published step, including the very first one.
	while (world.getStationSystemsSnapshot().elapsedSeconds < 60)
	{
		require(advanceStationSteps(world, 1), "neutral diffusion step failed");
		auto const snapshot = world.getStationSystemsSnapshot();
		require(snapshot.healthy, "neutral diffusion failed its conservation audit");
		for (size_t i = 0; i < 2; ++i)
		{
			auto const& air = snapshot.atmospheres[i];
			require(std::abs(air.temperatureC - initial.atmospheres[i].temperatureC) < 1e-12,
				"diffusion transported sensible heat between compartments");
			require(air.condensedKg == 0 && air.latentHeatEnergyKJ == 0
				&& air.exchangedEnergyKJ == 0,
				"pure diffusion invented a thermal or phase exchange");
		}
	}
	auto const final = world.getStationSystemsSnapshot();
	for (double moved : final.atmospherePathways[0].netTransferredKg)
		require(moved > 1e-3, "neutrality fixture did not actually exchange every gas");
}

void pathwayCondensation(smoke::Context const&)
{
	// Also reverse endpoint and compartment declaration order: latent heat
	// belongs to the physical receiver, not the B slot or the last loop entry.
	for (bool reverse : {false, true})
	{
		auto s = pathwaySettings();
		s.atmospheres[0].startingTemperatureC = 40;
		s.atmospheres[0].startingWaterVapourKg = 3;
		s.atmospheres[1].startingWaterVapourKg = 0.48;
		s.atmospherePathways[0].areaM2 = 100;
		if (reverse)
		{
			std::swap(s.atmospheres[0], s.atmospheres[1]);
			std::swap(s.atmospherePathways[0].aAtmosphereId, s.atmospherePathways[0].bAtmosphereId);
		}
		size_t const donor = reverse ? 1 : 0, receiver = reverse ? 0 : 1;
		core::World world("ReceivingCondensation", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		for (auto const& air : initial.atmospheres)
			require(air.relativeHumidity < 1, "condensation fixture starts supersaturated");
		world.resumeSimulation();
		while (world.getStationSystemsSnapshot().elapsedSeconds < 10)
		{
			require(advanceStationSteps(world, 1), "receiving condensation step failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			auto const& warm = snapshot.atmospheres[donor];
			auto const& cold = snapshot.atmospheres[receiver];
			auto const moved = (reverse ? -1 : 1) * snapshot.atmospherePathways[0].netTransferredKg[3];
			require(snapshot.healthy && moved > 0, "vapour did not diffuse into cold receiver");
			// This must hold already at the first publication. Without the
			// local solve, arriving vapour would exceed the initial cap.
			auto const initialCap = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(initial.atmospheres[receiver].temperatureC),
				100, initial.atmospheres[receiver].temperatureC);
			require(initial.atmospheres[receiver].waterVapourKg + moved > initialCap + 1e-3,
				"incoming vapour did not genuinely cross receiver saturation");
			require(cold.condensedKg > 1e-3 && cold.temperatureC > initial.atmospheres[receiver].temperatureC,
				"incoming vapour did not condense and warm receiver in the same tick");
			for (auto const& air : snapshot.atmospheres)
			{
				auto const cap = core::stationSaturatedVapourMassKg(
					core::stationSaturationVapourPressureKPa(air.temperatureC), 100, air.temperatureC);
				require(air.waterVapourKg <= cap + 1e-9 && air.relativeHumidity <= 1 + 1e-9,
					"pathway published supersaturated gas at its own temperature");
			}
			close(warm.temperatureC, initial.atmospheres[donor].temperatureC, "donor received condensation heat");
			require(warm.condensedKg == 0 && warm.latentHeatEnergyKJ == 0,
				"receiver condensation was booked in donor");
			close(cold.latentHeatEnergyKJ, cold.condensedKg * core::StationLatentHeatVaporizationKJPerKg,
				"receiver latent heat does not match condensate");
			close((cold.temperatureC - initial.atmospheres[receiver].temperatureC)
				* s.atmospheres[receiver].thermalCapacityKJPerK, cold.latentHeatEnergyKJ,
				"receiver temperature does not account for local latent heat");
			close(cold.waterVapourKg + cold.condensedKg,
				initial.atmospheres[receiver].waterVapourKg + moved, "receiver vapour/condensate ledger failed");
			close(warm.waterVapourKg, initial.atmospheres[donor].waterVapourKg - moved,
				"donor vapour ledger failed");
		}
	}
}

void pathwayCommands(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospherePathways[0].initialOpenFraction = 0;
	// Deliberately off-grid, reversed declaration time, and two events inside
	// a coarse step: opening at an endpoint or tick count cannot pass this.
	s.pathwayCommands = {{"close", "opening", 0, 1.337, 0},
		{"open", "opening", 0.8, 0.137, 0}};
	core::World world("CommandLanding", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	require(world.getStationSystemsSnapshot().atmospherePathways[0].lastCommandId.empty(),
		"future command applied at configure time");
	world.resumeSimulation();
	require(advanceToLastStepBefore(world, 0.137), "pre-command advance failed");
	auto before = world.getStationSystemsSnapshot();
	require(before.atmospherePathways[0].openFraction == 0
		&& before.atmospheres[0].oxygenKg == 30, "command landed before its stamp");
	require(advanceStationSteps(world, 1), "landing step failed");
	auto landed = world.getStationSystemsSnapshot();
	require(landed.atmospherePathways[0].lastCommandId == "open"
		&& landed.atmospherePathways[0].lastCommandSeconds == 0.137
		&& landed.atmospherePathways[0].openFraction == 0.8, "open provenance or value wrong");
	auto const exposure = (landed.elapsedSeconds - 0.137) * 0.8;
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const initialA = masses(before.atmospheres[0])[gas];
		auto const initialB = masses(before.atmospheres[1])[gas];
		auto const moved = (initialA - initialB) * 0.5 * (1 - std::exp(
			-2 * core::StationPathwayDiffusionCoefficients[gas] * 273.15 / 100 * exposure));
		close(masses(landed.atmospheres[0])[gas], initialA - moved, "gas responded at wrong command time");
		close(landed.atmospherePathways[0].flowRateKgPerS[gas] * landed.systemsTimestepSeconds,
			moved, "split command step did not publish whole-step flux");
	}
	require(advanceSeconds(world, 3), "post-close advance failed");
	auto final = world.getStationSystemsSnapshot();
	require(final.healthy && final.atmospherePathways[0].lastCommandId == "close"
		&& final.atmospherePathways[0].lastCommandSeconds == 1.337
		&& final.atmospherePathways[0].openFraction == 0, "close did not land once at authored time");
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const a = masses(before.atmospheres[0])[gas], b = masses(before.atmospheres[1])[gas];
		auto const moved = (a - b) * 0.5 * (1 - std::exp(
			-2 * core::StationPathwayDiffusionCoefficients[gas] * 273.15 / 100 * 0.8 * 1.2));
		close(masses(final.atmospheres[0])[gas], a - moved, "command cadence changed integrated exchange");
		close(final.atmospherePathways[0].netTransferredKg[gas], moved, "command flux ledger mismatch");
	}
}

void pathwayCommandOrdering(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.pathwayCommands = {{"later", "opening", 0.1, 2, 100},
		{"first", "opening", 0.2, 0, 10},
		{"winner", "opening", 0.8, 0, -10},
		{"middle", "opening", 0.4, 0, 10}};
	core::World world("CommandOrder", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	world.resumeSimulation();
	// Use a short run at the requested cadence; a zero-time command must
	// affect the first positive interval, not its end.
	require(advanceStationSteps(world, 1), "ordering step failed");
	auto result = world.getStationSystemsSnapshot();
	require(result.atmospherePathways[0].lastCommandId == "winner"
		&& result.atmospherePathways[0].openFraction == 0.8, "priority did not sort descending");
	close(result.atmospheres[0].oxygenKg, 20 + 10 * std::exp(
		-2 * core::StationPathwayDiffusionCoefficients[0] * 273.15 / 100
		* 0.8 * result.elapsedSeconds), "zero-time command missed the first interval");
	require(advanceSeconds(world, 3), "later-command advance failed");
	require(world.getStationSystemsSnapshot().atmospherePathways[0].lastCommandId == "later",
		"priority overrode occurrence time");
	// Same priority: declaration order, last-applied wins.
	world.pauseSimulation();
	s.pathwayCommands = {{"first", "opening", 0.2, 0, 10}, {"last", "opening", 0.7, 0, 10}};
	require(world.configureStationSystems(s, true, &diagnostic), diagnostic.c_str());
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "tie step failed");
	result = world.getStationSystemsSnapshot();
	require(result.atmospherePathways[0].lastCommandId == "last"
		&& result.atmospherePathways[0].lastCommandSeconds == 0
		&& result.atmospherePathways[0].openFraction == 0.7, "declaration tie not last-applied");
}

void pathwayCommandReset(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospherePathways[0].initialOpenFraction = 0.3;
	s.pathwayCommands = {{"open", "opening", 0.8, 0.137, 0},
		{"pending", "opening", 0, 5, 0}};
	core::World world("CommandReset", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	require(advanceSeconds(world, 2), "reset fixture advance failed");
	auto first = world.getStationSystemsSnapshot();
	require(first.atmospherePathways[0].openFraction == 0.8, "reset fixture command did not fire");
	world.pauseSimulation(); world.resetSimulation();
	require(world.getStationSystemsSnapshot() == initial, "reset inherited openness, provenance or pending run state");
	world.resumeSimulation();
	require(advanceSeconds(world, 2), "reset replay advance failed");
	require(world.getStationSystemsSnapshot() == first, "reset did not rebuild authored schedule deterministically");
	require(advanceSeconds(world, 4), "pending command advance failed");
	require(world.getStationSystemsSnapshot().atmospherePathways[0].lastCommandId == "pending",
		"reset lost authored future command");
}

void pathwayCommandValidation(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.pathwayCommands = {{"bad-command", "opening", 0.8, 1, 0}};
	core::World world("CommandRefusal", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	world.resumeSimulation();
	require(advanceSeconds(world, 2), "refusal fixture advance failed");
	world.pauseSimulation();
	auto const initial = world.getStationSystemsSnapshot();
	require(initial.atmospherePathways[0].openFraction == 0.8, "refusal fixture is not actuated");
	auto refuse = [&](auto const& bad, char const* field)
	{
		require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid command accepted");
		require(diagnostic.find("bad-command") != std::string::npos
			&& diagnostic.find(field) != std::string::npos, "command refusal lacks name/field");
		require(!world.configureStationSystems(bad, false, &diagnostic), "invalid command configured");
		require(world.getStationSystemsSnapshot() == initial, "invalid command changed pathway state");
	};
	for (auto v : {-0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; bad.pathwayCommands[0].openFraction = v; refuse(bad, "openFraction");
	}
	for (auto v : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; bad.pathwayCommands[0].occurrenceSeconds = v; refuse(bad, "occurrenceSeconds");
	}
	{ auto bad = s; bad.pathwayCommands[0].pathwayId = "missing"; refuse(bad, "missing"); }
	{ auto bad = s; bad.pathwayCommands.push_back(bad.pathwayCommands[0]); refuse(bad, "duplicated"); }
	{ auto bad = s; bad.pathwayCommands[0].id.clear();
		require(!core::validateStationSystemsSettings(bad, diagnostic), "empty command id accepted"); }
	{ auto bad = s; bad.pathwayCommands[0].id.assign(core::MaxStationStoreIdLength + 1, 'x');
		require(!core::validateStationSystemsSettings(bad, diagnostic), "oversized command id accepted"); }
	{
		auto many = s; many.pathwayCommands.clear();
		for (size_t i = 0; i < core::MaxStationPathwayCommands; ++i)
			many.pathwayCommands.push_back({"c" + std::to_string(i), "opening", 0, 0, 0});
		require(core::validateStationSystemsSettings(many, diagnostic), "command ceiling refused");
		many.pathwayCommands.push_back({"over-limit", "opening", 0, 0, 0});
		require(!core::validateStationSystemsSettings(many, diagnostic), "command ceiling not enforced");
	}
	for (auto v : {0.0, 1.0})
	{
		auto valid = s; valid.pathwayCommands[0].openFraction = v;
		require(core::validateStationSystemsSettings(valid, diagnostic), "boundary openness refused");
	}
}

void pathwayValidation(smoke::Context const&)
{
	core::World world("PathwayRefusal", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	auto s = pathwaySettings();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	world.resumeSimulation();
	require(advanceStationSteps(world, 1), "refusal baseline step failed");
	world.pauseSimulation();
	auto const baseline = world.getStationSystemsSnapshot();
	auto refuse = [&](core::StationSystemsSettings const& bad, char const* field)
	{
		require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid pathway passed pure validation");
		require(diagnostic.find("opening") != std::string::npos && diagnostic.find(field) != std::string::npos,
			"pathway refusal did not name the entry and offending field/reference");
		require(!world.configureStationSystems(bad, false, &diagnostic), "invalid pathway was configured");
		require(world.getStationSystemsSnapshot() == baseline, "refused pathway configuration mutated the World");
	};
	for (double value : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; bad.atmospherePathways[0].areaM2 = value; refuse(bad, "areaM2");
	}
	for (double value : {-0.01, 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = s; bad.atmospherePathways[0].initialOpenFraction = value; refuse(bad, "initialOpenFraction");
	}
	for (bool first : {false, true})
	{
		auto bad = s;
		(first ? bad.atmospherePathways[0].aAtmosphereId : bad.atmospherePathways[0].bAtmosphereId) = "not-a-compartment";
		refuse(bad, "not-a-compartment");
	}
	{ auto bad = s; bad.atmospherePathways[0].bAtmosphereId = "a"; refuse(bad, "self-loop"); }
	{ auto bad = s; bad.atmospherePathways.push_back(bad.atmospherePathways[0]); refuse(bad, "duplicated"); }
	{ auto bad = s; bad.atmospherePathways[0].id.clear();
		require(!core::validateStationSystemsSettings(bad, diagnostic), "empty pathway id accepted"); }
	// Closed openings and parallel pairs are coherent; only the count is bounded.
	auto many = s;
	many.atmospherePathways.clear();
	for (size_t i = 0; i < core::MaxStationPathways; ++i)
		many.atmospherePathways.push_back({"p" + std::to_string(i), "a", "b", 1, 0});
	require(core::validateStationSystemsSettings(many, diagnostic), diagnostic.c_str());
	many.atmospherePathways.push_back({"over-limit", "a", "b", 1, 1});
	require(!core::validateStationSystemsSettings(many, diagnostic) && diagnostic.find("limited") != std::string::npos,
		"pathway count ceiling was not enforced");
	require(!world.configureStationSystems(many, false, &diagnostic) && world.getStationSystemsSnapshot() == baseline,
		"over-limit pathway configuration mutated the World");
}
}

void registerStationPathwayChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"pathwayDiffusion", pathwayDiffusion});
	checks.push_back({"pathwayValidation", pathwayValidation});
	checks.push_back({"pathwayCounterFlow", pathwayCounterFlow});
	checks.push_back({"pathwayClosed", pathwayClosed});
	checks.push_back({"pathwayOpenness", pathwayOpenness});
	checks.push_back({"pathwayThermalNeutrality", pathwayThermalNeutrality});
	checks.push_back({"pathwayCondensation", pathwayCondensation});
	checks.push_back({"pathwayCommands", pathwayCommands});
	checks.push_back({"pathwayCommandOrdering", pathwayCommandOrdering});
	checks.push_back({"pathwayCommandReset", pathwayCommandReset});
	checks.push_back({"pathwayCommandValidation", pathwayCommandValidation});
}
