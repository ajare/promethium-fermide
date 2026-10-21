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
}
