#include "StationRig.h"
#include "Smoke.h"
#include <array>
#include <iostream>
#include "core/StationConfigurationDocument.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"

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
void pathwayPersistence(smoke::Context const&)
{
	auto settings = pathwaySettings();
	settings.atmospherePathways.push_back({"parallel", "b", "a", 0.125, 0});
	settings.atmospherePathways.push_back({"to-space", "b", core::StationExteriorAtmosphereId, 0.25, 0.3});
	settings.atmospherePathways.push_back({"from-space", core::StationExteriorAtmosphereId, "a", 0.1, 0.5});
	// Non-chronological declarations and same-time ties must retain their order.
	settings.pathwayCommands = {{"close", "opening", 0, 1.337, -2},
		{"open", "opening", 0.8, 0.137, 3}, {"tie", "opening", 0.4, 0.137, 3},
		{"pending", "parallel", 1, 10, 0}};
	core::StationConfigurationDocument document; document.settings = settings;
	auto const yaml = core::serializeStationConfiguration(document);
	core::StationConfigurationDocument loaded;
	std::string diagnostic;
	require(core::parseStationConfiguration(yaml, loaded, &diagnostic), diagnostic.c_str());
	require(loaded.settings == settings, "pathway Station configuration changed on round trip");
	for (auto const& [field, id] : {std::pair{"areaM2", "opening"}, std::pair{"priority", "close"}})
	{
		auto bad = yaml;
		auto const at = bad.find(std::string(field) + ":");
		require(at != std::string::npos, "missing pathway wire field");
		bad.insert(at, "unknown_");
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& diagnostic.find(id) != std::string::npos
			&& diagnostic.find(std::string("unknown_") + field) != std::string::npos
			&& diagnostic.find("conversion required") != std::string::npos
			&& loaded.settings == settings, "unknown pathway field was ignored or mutated document");
	}
	for (int version : {104, 106})
	{
		auto bad = yaml;
		bad.replace(bad.find("schemaVersion: 105"), std::string("schemaVersion: 105").size(),
			"schemaVersion: " + std::to_string(version));
		require(!core::parseStationConfiguration(bad, loaded, &diagnostic)
			&& loaded.settings == settings, "unsupported pathway schema was accepted");
	}
	for (bool binary : {false, true})
	{
		core::World original("Pathway persistence", 2, 2);
		original.pauseSimulation(); original.finishBuild();
		require(original.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work;
		original.serialize(*writer, work); writer->serialize();
		auto const text = binary ? static_cast<core::BinarySerializer&>(*writer).getSerializedString()
			: static_cast<core::YamlSerializer&>(*writer).getSerializedString();
		if (!binary)
		{
			for (auto const* field : {"areaM2", "priority"})
			{
				auto bad = text; bad.insert(bad.find(std::string(field) + ":"), "unknown_");
				auto invalid = core::YamlSerializer::fromString(bad); invalid->deserialize();
				auto const baseline = original.getStationSystemsSnapshot();
				bool refused = false;
				try { original.deserialize(*invalid, work); }
				catch (std::exception const& error)
				{
					auto const message = std::string(error.what());
					refused = message.find("conversion required") != std::string::npos
						&& message.find(std::string("unknown_") + field) != std::string::npos
						&& message.find(std::string(field) == "areaM2" ? "opening" : "close") != std::string::npos;
				}
				require(refused && original.getStationSystemsSnapshot() == baseline
					&& original.getStationSystemsSettings() == settings,
					"World ignored an unknown pathway field or changed on refusal");
			}
		}
		auto reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(text))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(text));
		reader->deserialize();
		core::World restored("Restored", 2, 2);
		require(restored.deserialize(*reader, work), "pathway World could not reload");
		require(restored.getStationSystemsSettings() == settings, "World pathway settings changed");
		original.resumeSimulation(); restored.resumeSimulation();
		require(advanceSeconds(original, 2) && advanceSeconds(restored, 2), "pathway replay failed");
		require(original.getStationSystemsSnapshot() == restored.getStationSystemsSnapshot(),
			"pathway round trip changed the run snapshot");
	}
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

void pathwayVacuumEqualization(smoke::Context const&)
{
	// At equal, fixed temperature, equilibrium shares each gas in proportion
	// to volume. Include unequal volumes to rule out a blind half-mass split.
	for (double receiverVolume : {100.0, 200.0})
	{
		auto s = pathwaySettings();
		auto& b = s.atmospheres[1];
		b.volumeM3 = receiverVolume;
		b.startingOxygenKg = b.startingNitrogenKg = b.startingCarbonDioxideKg = b.startingWaterVapourKg = 0;
		// Reach equilibrium cheaply without asserting a particular coefficient.
		s.atmospherePathways[0].areaM2 = 10;
		core::World world("VacuumEqualization", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		auto const total = masses(initial.atmospheres[0]);
		require(initial.atmospheres[0].pressureKPa > 0 && initial.atmospheres[1].pressureKPa == 0,
			"vacuum fixture must start with one pressurized and one evacuated compartment");
		for (double mass : masses(initial.atmospheres[1]))
			require(mass == 0, "evacuated compartment contains gas before exchange");
		double const donorFraction = s.atmospheres[0].volumeM3 / (s.atmospheres[0].volumeM3 + receiverVolume);
		double const equilibriumPressure = initial.atmospheres[0].pressureKPa * donorFraction;
		auto previous = initial;
		world.resumeSimulation();
		while (world.getStationSystemsSnapshot().elapsedSeconds < 240)
		{
			require(advanceStationSteps(world, 1), "vacuum equalization step failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot.healthy, "vacuum equalization failed conservation audit");
			auto const a = masses(snapshot.atmospheres[0]), received = masses(snapshot.atmospheres[1]);
			for (size_t gas = 0; gas < total.size(); ++gas)
			{
				require(std::isfinite(a[gas]) && std::isfinite(received[gas])
					&& a[gas] > 0 && received[gas] > 0, "vacuum exchange produced invalid gas inventory or no inflow");
				close(a[gas] + received[gas], total[gas], "vacuum equalization did not conserve each gas");
				close(snapshot.atmospherePathways[0].netTransferredKg[gas], received[gas],
					"vacuum receiver inventory disagrees with cumulative transfer");
				require(a[gas] <= masses(previous.atmospheres[0])[gas] + 1e-12
					&& received[gas] >= masses(previous.atmospheres[1])[gas] - 1e-12,
					"vacuum equalization reversed direction or oscillated");
			}
			for (size_t i = 0; i < 2; ++i)
			{
				close(snapshot.atmospheres[i].temperatureC, initial.atmospheres[i].temperatureC,
					"vacuum equalization changed compartment temperature");
				require(snapshot.atmospheres[i].condensedKg == 0, "vacuum fixture unexpectedly condensed vapour");
			}
			require(snapshot.atmospheres[0].pressureKPa >= equilibriumPressure - 1e-9
				&& snapshot.atmospheres[1].pressureKPa <= equilibriumPressure + 1e-9,
				"vacuum equalization overshot shared equilibrium pressure");
			previous = snapshot;
		}
		for (size_t gas = 0; gas < total.size(); ++gas)
		{
			require(std::abs(masses(previous.atmospheres[0])[gas] / (total[gas] * donorFraction) - 1) < 1e-7
				&& std::abs(masses(previous.atmospheres[1])[gas] / (total[gas] * (1 - donorFraction)) - 1) < 1e-7,
				"air and vacuum did not reach the volume-weighted per-gas equilibrium");
		}
		for (auto const& air : previous.atmospheres)
			require(std::abs(air.pressureKPa / equilibriumPressure - 1) < 1e-7,
				"air and vacuum did not reach the expected common pressure");
	}
}

void pathwayExteriorDiffusion(smoke::Context const&)
{
	for (bool reverse : {false, true})
	for (double openness : {0.0, 0.25, 1.0})
	for (double area : {1.0, 1e300})
	{
		auto s = pathwaySettings();
		s.atmospherePathways = {{"space", "a", core::StationExteriorAtmosphereId, area, openness}};
		if (reverse) std::swap(s.atmospherePathways[0].aAtmosphereId, s.atmospherePathways[0].bAtmosphereId);
		core::World world("ExteriorDiffusion", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		std::string diagnostic;
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		auto const start = masses(initial.atmospheres[0]);
		world.resumeSimulation();
		require(advanceStationSteps(world, 1), "exterior first step failed");
		auto const first = world.getStationSystemsSnapshot();
		require(first.atmospherePathways[0].status == (openness == 0
			? core::StationAtmospherePathwayStatus::Idle : core::StationAtmospherePathwayStatus::Active),
			"exterior pathway first-step status does not match actual loss");
		for (size_t gas = 0; gas < start.size(); ++gas)
			close(first.atmospherePathways[0].flowRateKgPerS[gas] * first.systemsTimestepSeconds,
				first.atmospherePathways[0].netTransferredKg[gas], "exterior interval rate disagrees with loss");
		require(advanceSeconds(world, 10), "exterior depletion advance failed");
		auto const final = world.getStationSystemsSnapshot();
		require(final.healthy && final.atmospheres.size() == 2, "exterior created an inventory or broke the audit");
		require(final.atmospheres[1] == initial.atmospheres[1], "exterior pathway changed an unconnected compartment");
		for (size_t gas = 0; gas < start.size(); ++gas)
		{
			auto const remaining = masses(final.atmospheres[0])[gas];
			auto const expected = start[gas] * std::exp(-core::StationPathwayDiffusionCoefficients[gas]
				* area * openness * 273.15 / 100 * final.elapsedSeconds);
			require(std::isfinite(remaining) && remaining >= 0, "exterior produced invalid inventory");
			close(remaining, expected, "exterior did not follow exact area/openness-scaled exponential depletion");
			auto const lost = final.atmospheres[0].exteriorLostKg[gas];
			close(remaining + lost, start[gas], "exterior inventory plus external loss did not conserve each gas");
			close((reverse ? -1 : 1) * final.atmospherePathways[0].netTransferredKg[gas], lost,
				"exterior signed flux did not reconcile with compartment loss");
		}
		close(final.atmospheres[0].temperatureC, initial.atmospheres[0].temperatureC, "exterior diffusion moved heat");
		require(final.atmospheres[0].leakedKg == 0 && final.atmospheres[0].lostKg == 0,
			"exterior loss was double-counted as constant leakage or airlock loss");
		if (area > 1e100 && openness > 0)
			require(final.atmospheres[0].pressureKPa == 0
				&& final.atmospherePathways[0].status == core::StationAtmospherePathwayStatus::Idle,
				"enormous exterior opening did not empty safely and become Idle");
	}
}

void pathwayExteriorCommands(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospherePathways = {{"space", "a", core::StationExteriorAtmosphereId, 1, 0}};
	s.pathwayCommands = {{"close", "space", 0, 1.337, 0}, {"open", "space", 0.5, 0.137, 0}};
	core::World world("ExteriorCommands", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = world.getStationSystemsSnapshot();
	world.resumeSimulation();
	require(advanceToLastStepBefore(world, 0.137), "exterior pre-command advance failed");
	require(world.getStationSystemsSnapshot().atmospheres[0] == initial.atmospheres[0],
		"closed exterior opening leaked before its command");
	require(advanceSeconds(world, 3), "exterior command advance failed");
	auto const final = world.getStationSystemsSnapshot();
	require(final.healthy && final.atmospherePathways[0].lastCommandId == "close"
		&& final.atmospherePathways[0].lastCommandSeconds == 1.337
		&& final.atmospherePathways[0].openFraction == 0, "exterior close command/provenance failed");
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const start = masses(initial.atmospheres[0])[gas];
		auto const expected = start * std::exp(-core::StationPathwayDiffusionCoefficients[gas] * 273.15 / 100 * 0.5 * 1.2);
		close(masses(final.atmospheres[0])[gas], expected, "exterior command exposure depends on cadence");
		close(final.atmospheres[0].exteriorLostKg[gas], start - expected, "exterior command loss ledger failed");
	}
	world.pauseSimulation(); world.resetSimulation();
	require(world.getStationSystemsSnapshot() == initial, "reset inherited exterior losses or commands");
	world.resumeSimulation();
	require(advanceToLastStepBefore(world, 0.137) && advanceSeconds(world, 3), "exterior reset replay failed");
	require(world.getStationSystemsSnapshot() == final, "exterior commands failed deterministic reset replay");

	// Internal exchange, exterior loss, constant leakage and airlock cycles coexist.
	world.pauseSimulation();
	s.pathwayCommands.clear();
	s.atmospherePathways = {{"internal", "a", "b", 1, 1}, {"space", "b", core::StationExteriorAtmosphereId, 1, 0.5}};
	s.atmospheres[0].leakRateKgPerS = 0.1;
	core::AirlockCycleSettings cycle;
	cycle.id = "cycle"; cycle.atmosphereId = "a"; cycle.gasMassKg = 1;
	cycle.firstOccurrenceSeconds = 0.5; cycle.recoveredFraction = 0.25;
	s.airlockCycles = {cycle};
	require(world.configureStationSystems(s, true, &diagnostic), diagnostic.c_str());
	world.resumeSimulation();
	require(advanceSeconds(world, 10), "mixed internal/exterior advance failed");
	auto const mixed = world.getStationSystemsSnapshot();
	require(mixed.healthy && mixed.atmospheres[0].leakedKg > 0, "mixed exterior/leak accounting failed");
	double lostToSpace = 0;
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const lost = mixed.atmospheres[1].exteriorLostKg[gas];
		close(lost, mixed.atmospherePathways[1].netTransferredKg[gas], "mixed exterior flux ledger failed");
		lostToSpace += lost;
	}
	double before = 0, after = 0;
	for (size_t i = 0; i < 2; ++i)
		for (size_t gas = 0; gas < 4; ++gas)
		{
			before += masses(initial.atmospheres[i])[gas];
			after += masses(mixed.atmospheres[i])[gas];
		}
	close(mixed.atmospheres[0].recoveredKg, 0.25, "mixed cycle recovery changed");
	close(mixed.atmospheres[0].lostKg, 0.75, "exterior loss was mixed into airlock loss");
	close(before, after + lostToSpace + mixed.atmospheres[0].leakedKg
		+ mixed.atmospheres[0].recoveredKg + mixed.atmospheres[0].lostKg,
		"mixed external losses counted incorrectly");
}

void pathwayReferenceConfigurations(smoke::Context const& context)
{
	for (auto const* file : {"atmosphere-vacuum-equalization.yaml",
		"atmosphere-exterior-partial.yaml", "atmosphere-exterior-isolation.yaml"})
	{
		core::StationConfigurationDocument document, restored;
		std::string diagnostic;
		auto const loaded = core::loadStationConfiguration(context.fixture(std::string("resources/station-systems/scenarios/")
			+ file).string(), document, &diagnostic);
		require(loaded, diagnostic.c_str());
		auto const valid = core::validateStationSystemsSettings(document.settings, diagnostic);
		require(valid, diagnostic.c_str());
		require(core::parseStationConfiguration(core::serializeStationConfiguration(document), restored, &diagnostic)
			&& restored.settings == document.settings && restored.positions == document.positions,
			"pathway reference configuration did not round-trip");
		core::World world(file, 2, 2);
		world.pauseSimulation(); world.finishBuild();
		auto const configured = world.configureStationSystems(document.settings, false, &diagnostic);
		require(configured, diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		world.resumeSimulation();
		require(advanceSeconds(world, 30), "pathway reference initial advance failed");
		auto const before = world.getStationSystemsSnapshot();
		bool const vacuum = std::string_view(file) == "atmosphere-vacuum-equalization.yaml";
		bool const partial = std::string_view(file) == "atmosphere-exterior-partial.yaml";
		if (!vacuum)
			for (size_t i = 0; i < initial.atmospheres.size(); ++i)
				for (size_t gas = 0; gas < 4; ++gas)
					close(masses(before.atmospheres[i])[gas], masses(initial.atmospheres[i])[gas],
						"reference exterior vented before its authored opening");
		// Inspect the sealed chamber before the reconnect command at 240 s;
		// World stepping can already publish an interval starting at a boundary.
		require(advanceSeconds(world, 200), "pathway reference schedule advance failed");
		auto const scheduled = world.getStationSystemsSnapshot();
		require(scheduled.healthy, "pathway reference schedule failed its audit");
		if (!vacuum)
		{
			size_t const donor = partial ? 0 : 1;
			auto const& opening = scheduled.atmospherePathways[partial ? 0 : 1];
			require(opening.openFraction == 0 && opening.lastCommandId == "seal-exterior",
				"reference exterior opening did not seal on schedule");
			for (size_t gas = 0; gas < 4; ++gas)
			{
				auto const start = masses(initial.atmospheres[donor])[gas];
				auto const remaining = start * std::exp(-core::StationPathwayDiffusionCoefficients[gas]
					* 293.15 / initial.atmospheres[donor].volumeM3 * (partial ? 0.25 : 0.5) * 120);
				close(masses(scheduled.atmospheres[donor])[gas], remaining,
					"reference partial opening did not produce the documented depletion");
				close(scheduled.atmospheres[donor].exteriorLostKg[gas], start - remaining,
					"reference exterior loss did not close");
				if (!partial)
					close(masses(scheduled.atmospheres[0])[gas], masses(initial.atmospheres[0])[gas],
						"isolated habitat lost gas while its chamber vented");
			}
		}
		require(advanceSeconds(world, 670), "pathway reference equilibrium advance failed");
		auto const final = world.getStationSystemsSnapshot();
		require(final.healthy, "pathway reference equilibrium failed its audit");
		for (size_t gas = 0; gas < 4; ++gas)
		{
			if (partial)
			{
				close(masses(final.atmospheres[0])[gas], masses(scheduled.atmospheres[0])[gas],
					"sealed reference continued losing gas");
				close(final.atmospheres[0].exteriorLostKg[gas], scheduled.atmospheres[0].exteriorLostKg[gas],
					"sealed reference continued accumulating loss");
				continue;
			}
			double total = 0, remaining = 0, lost = 0, volume = 0;
			for (size_t i = 0; i < 2; ++i)
			{
				total += masses(initial.atmospheres[i])[gas];
				remaining += masses(final.atmospheres[i])[gas];
				lost += final.atmospheres[i].exteriorLostKg[gas];
				volume += final.atmospheres[i].volumeM3;
			}
			close(total, remaining + lost, "reference inventories plus exterior losses did not conserve gas");
			for (auto const& air : final.atmospheres)
				require(std::abs(masses(air)[gas] / (remaining * air.volumeM3 / volume) - 1) < 1e-4,
					"reference compartments did not approach their shared partial pressures");
		}
		for (auto const& air : final.atmospheres)
			require(air.temperatureC == 20 && air.condensedKg == 0 && air.leakedKg == 0,
				"pathway reference introduced unintended thermal, phase or leak effects");
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

// #790: parallel pair relaxation composes exactly, including when one
// opening spells the undirected endpoints in the opposite order.
void pathwayParallelAdditivity(smoke::Context const&)
{
	for (bool reverse : {false, true})
	{
		auto single = pathwaySettings();
		// Unequal volumes and temperatures exercise the fixed compartment
		// conductance ratio, rather than just a symmetric half-mass split.
		single.atmospheres[1].volumeM3 = 150;
		single.atmospheres[1].startingTemperatureC = 20;
		auto parallel = single;
		parallel.atmospherePathways = {{"first", "a", "b", 0.5, 1},
			{"second", reverse ? "b" : "a", reverse ? "a" : "b", 0.5, 1}};
		std::string diagnostic;
		require(core::validateStationSystemsSettings(parallel, diagnostic), diagnostic.c_str());
		core::World one("SingleOpening", 2, 2), two("ParallelOpenings", 2, 2);
		one.pauseSimulation(); one.finishBuild();
		two.pauseSimulation(); two.finishBuild();
		require(one.configureStationSystems(single, false, &diagnostic), diagnostic.c_str());
		require(two.configureStationSystems(parallel, false, &diagnostic), diagnostic.c_str());
		auto const initial = one.getStationSystemsSnapshot();
		auto const authored = two.getStationSystemsSnapshot();
		require(authored.atmospherePathways.size() == 2
			&& authored.atmospherePathways[0].id == "first" && authored.atmospherePathways[1].id == "second",
			"parallel endpoints were deduplicated rather than retaining both named pathways");
		one.resumeSimulation(); two.resumeSimulation();
		for (double duration : {initial.systemsTimestepSeconds, 10.0, 30.0})
		{
			require(advanceSeconds(one, duration) && advanceSeconds(two, duration), "parallel comparison advance failed");
			auto const a = one.getStationSystemsSnapshot(), b = two.getStationSystemsSnapshot();
			require(a.healthy && b.healthy, "parallel openings failed conservation audit");
			for (size_t gas = 0; gas < 4; ++gas)
			{
				for (size_t i = 0; i < 2; ++i)
					close(masses(a.atmospheres[i])[gas], masses(b.atmospheres[i])[gas],
						"two half-area pathways differ from one full-area pathway by >= 1e-9 kg");
				double const sign = reverse ? -1 : 1;
				auto const net = b.atmospherePathways[0].netTransferredKg[gas]
					+ sign * b.atmospherePathways[1].netTransferredKg[gas];
				close(a.atmospherePathways[0].netTransferredKg[gas], net, "parallel cumulative flux is not additive");
				close(a.atmospherePathways[0].flowRateKgPerS[gas], b.atmospherePathways[0].flowRateKgPerS[gas]
					+ sign * b.atmospherePathways[1].flowRateKgPerS[gas], "parallel interval flux is not additive");
				for (auto const& p : b.atmospherePathways)
					require(p.status == core::StationAtmospherePathwayStatus::Active
						&& std::abs(p.netTransferredKg[gas]) > 0, "parallel opening was accepted but never exchanged gas");
				close(masses(b.atmospheres[0])[gas] + masses(b.atmospheres[1])[gas],
					masses(initial.atmospheres[0])[gas] + masses(initial.atmospheres[1])[gas],
					"parallel openings did not conserve a gas");
			}
		}
	}
}

void pathwayChainTopology(smoke::Context const&)
{
	// Both declaration orders are legal, but operator splitting need not give
	// identical results across orders. Each must replay its own result exactly.
	for (bool reverseOrder : {false, true})
	{
		auto s = pathwaySettings();
		auto& b = s.atmospheres[1];
		b.startingOxygenKg = b.startingNitrogenKg = b.startingCarbonDioxideKg = b.startingWaterVapourKg = 0;
		b.volumeM3 = 150;
		auto c = b; c.id = "c"; c.volumeM3 = 75;
		s.atmospheres.push_back(c);
		// IDs intentionally oppose alphabetical order. With BC first, C must
		// stay empty on the first step, then receive gas via B on later steps.
		s.atmospherePathways = {{"z-bc", "b", "c", 1, 1}, {"a-ab", "a", "b", 1, 1}};
		if (reverseOrder) std::swap(s.atmospherePathways[0], s.atmospherePathways[1]);
		size_t const ab = reverseOrder ? 0 : 1, bc = reverseOrder ? 1 : 0;
		core::World world("Chain", 2, 2), replay("ChainReplay", 2, 2);
		world.pauseSimulation(); world.finishBuild();
		replay.pauseSimulation(); replay.finishBuild();
		std::string diagnostic;
		require(core::validateStationSystemsSettings(s, diagnostic), diagnostic.c_str());
		require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		require(replay.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
		auto const initial = world.getStationSystemsSnapshot();
		require(initial == replay.getStationSystemsSnapshot(), "chain initial snapshots differ");
		world.resumeSimulation(); replay.resumeSimulation();
		bool first = true;
		while (world.getStationSystemsSnapshot().elapsedSeconds < 60)
		{
			require(advanceStationSteps(world, 1) && advanceStationSteps(replay, 1), "chain step failed");
			auto const snapshot = world.getStationSystemsSnapshot();
			require(snapshot == replay.getStationSystemsSnapshot(), "same declaration-order chain did not replay exactly");
			require(snapshot.healthy, "chain failed its conservation audit");
			for (size_t gas = 0; gas < 4; ++gas)
			{
				auto const a = masses(snapshot.atmospheres[0])[gas];
				auto const middle = masses(snapshot.atmospheres[1])[gas];
				auto const end = masses(snapshot.atmospheres[2])[gas];
				auto const start = masses(initial.atmospheres[0])[gas];
				auto const in = snapshot.atmospherePathways[ab].netTransferredKg[gas];
				auto const out = snapshot.atmospherePathways[bc].netTransferredKg[gas];
				require(a >= 0 && middle >= 0 && end >= 0, "chain produced a negative gas inventory");
				close(a + middle + end, start, "connected chain did not conserve each gas");
				close(a, start - in, "chain A cumulative ledger failed");
				close(middle, in - out, "shared intermediate compartment double-counted a pathway");
				close(end, out, "chain C received gas outside its sole pathway");
				if (first)
				{
					require(middle > 0, "A did not supply the intermediate compartment");
					require(reverseOrder ? end > 0 : end == 0, "chain did not resolve in declaration order");
				}
			}
			first = false;
		}
		auto const final = world.getStationSystemsSnapshot();
		for (double mass : masses(final.atmospheres[2]))
			require(mass > 1e-5, "gas introduced only at A never propagated through B to C");
	}
}

// #792: infer the effective coefficient from a real public-seam run before
// choosing a long-chain horizon; no test-only solver access is involved.
void pathwayLongHorizon(smoke::Context const&)
{
	auto s = pathwaySettings();
	core::World calibration("PathwayCalibration", 2, 2);
	calibration.pauseSimulation(); calibration.finishBuild();
	std::string diagnostic;
	require(calibration.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = calibration.getStationSystemsSnapshot();
	calibration.resumeSimulation();
	require(advanceSeconds(calibration, 10), "diffusion calibration failed");
	auto const measured = calibration.getStationSystemsSnapshot();
	for (size_t gas = 0; gas < 4; ++gas)
	{
		auto const gap0 = masses(initial.atmospheres[0])[gas] - masses(initial.atmospheres[1])[gas];
		auto const gap = masses(measured.atmospheres[0])[gas] - masses(measured.atmospheres[1])[gas];
		auto const coefficient = -std::log(gap / gap0) / (measured.elapsedSeconds * 2 * 273.15 / 100);
		std::cout << "CALIBRATION " << core::StationPathwayGases[gas] << " coefficient=" << coefficient << '\n';
		require(std::abs(coefficient - 0.002) < 1e-12, "diffusion calibration changed; reconsider the long horizon");
	}

	// Observed k=0.002 for all four gases. A three-volume chain is slower
	// than the calibration pair: allow 3600 s to settle, then inspect every
	// publication for a further 1200 s, not merely the final snapshot.
	s.atmospheres[1].volumeM3 = 150;
	s.atmospheres[1].startingTemperatureC = 20;
	auto c = s.atmospheres[1]; c.id = "c"; c.volumeM3 = 75; c.startingTemperatureC = 40;
	c.startingOxygenKg = c.startingNitrogenKg = c.startingCarbonDioxideKg = c.startingWaterVapourKg = 0;
	s.atmospheres.push_back(c);
	s.atmospherePathways = {{"ab", "a", "b", 1, 1}, {"bc", "b", "c", 1, 1}};
	core::World world("LongChain", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	std::array<double, 4> total{};
	double capacity = 0;
	for (auto const& air : world.getStationSystemsSnapshot().atmospheres)
	{
		capacity += air.volumeM3 / (air.temperatureC + 273.15);
		for (size_t gas = 0; gas < 4; ++gas) total[gas] += masses(air)[gas];
	}
	world.resumeSimulation();
	size_t held = 0;
	while (world.getStationSystemsSnapshot().elapsedSeconds < 4800)
	{
		require(advanceStationSteps(world, 1), "long-chain step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "long-chain conservation audit failed");
		for (size_t gas = 0; gas < 4; ++gas)
		{
			double sum = 0;
			for (size_t i = 0; i < snap.atmospheres.size(); ++i)
			{
				auto const& air = snap.atmospheres[i];
				auto const mass = masses(air)[gas];
				require(std::isfinite(mass) && mass >= 0, "long chain produced invalid inventory");
				require(air.condensedKg == 0 && air.temperatureC == s.atmospheres[i].startingTemperatureC,
					"long-chain fixture changed temperature or condensed");
				sum += mass;
				if (snap.elapsedSeconds >= 3600)
				{
					// m*T/V is partial pressure up to the same per-gas
					// ideal-gas factor; unequal V/T forbids a false equal-mass attractor.
					auto const density = mass * (air.temperatureC + 273.15) / air.volumeM3;
					require(std::abs(density / (total[gas] / capacity) - 1) < 1e-6,
						"chain did not reach and hold uniform partial pressures");
				}
			}
			close(sum, total[gas], "long chain lost a gas");
		}
		if (snap.elapsedSeconds >= 3600) ++held;
	}
	require(held >= 1200, "long-chain equilibrium hold was not sampled sufficiently");
}

void pathwayAbsurdGeometry(smoke::Context const&)
{
	auto s = pathwaySettings();
	s.atmospheres[0].volumeM3 = 1e-6;
	s.atmospheres[1].volumeM3 = 2e-6;
	s.atmospheres[1].startingTemperatureC = 40;
	for (auto& air : s.atmospheres)
	{
		air.startingOxygenKg *= 1e-8; air.startingNitrogenKg *= 1e-8;
		air.startingCarbonDioxideKg *= 1e-8; air.startingWaterVapourKg *= 1e-8;
	}
	// Both directions and an initially empty receiver, without saturation.
	s.atmospheres[0].startingOxygenKg = 0;
	s.atmospheres[1].startingCarbonDioxideKg = 0;
	s.atmospherePathways[0].areaM2 = 1e12;
	core::World world("AbsurdOpening", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string diagnostic;
	require(world.configureStationSystems(s, false, &diagnostic), diagnostic.c_str());
	auto const initial = world.getStationSystemsSnapshot();
	auto const a0 = masses(initial.atmospheres[0]), b0 = masses(initial.atmospheres[1]);
	auto const wa = 273.15 / 1e-6, wb = 313.15 / 2e-6;
	std::array<double, 4> previousError{};
	for (size_t gas = 0; gas < 4; ++gas)
		previousError[gas] = std::abs(a0[gas] - (a0[gas] + b0[gas]) * wb / (wa + wb));
	world.resumeSimulation();
	size_t publications = 0;
	while (world.getStationSystemsSnapshot().elapsedSeconds < 600)
	{
		require(advanceStationSteps(world, 1), "absurd-geometry step failed");
		auto const snap = world.getStationSystemsSnapshot();
		require(snap.healthy, "absurd geometry failed conservation audit");
		for (auto const& air : snap.atmospheres)
			require(air.condensedKg == 0, "absurd geometry fixture unexpectedly condensed");
		for (size_t gas = 0; gas < 4; ++gas)
		{
			auto const a = masses(snap.atmospheres[0])[gas], b = masses(snap.atmospheres[1])[gas];
			auto const total = a0[gas] + b0[gas];
			auto const equilibrium = total * wb / (wa + wb);
			auto const error = a - equilibrium;
			auto const tolerance = total * 1e-12;
			require(std::isfinite(a) && std::isfinite(b) && a >= 0 && b >= 0,
				"absurd geometry produced negative or non-finite gas");
			require(std::abs(a + b - total) <= tolerance, "absurd geometry lost gas");
			require(std::abs(error) <= previousError[gas] + tolerance
				&& (a0[gas] >= equilibrium ? error >= -tolerance : error <= tolerance),
				"absurd geometry oscillated or moved away from equilibrium");
			// This stiffness must reach the analytic interior equilibrium on
			// the FIRST step. A donor clamp can conserve mass but cannot pass.
			require(a > 0 && b > 0 && std::abs(error) <= tolerance,
				"absurd geometry clamped to a boundary instead of equilibrium");
			require(std::abs(snap.atmospherePathways[0].netTransferredKg[gas] - (a0[gas] - a)) <= tolerance,
				"absurd geometry cumulative flux does not reconcile");
			previousError[gas] = std::abs(error);
		}
		++publications;
	}
	require(publications >= 1000, "absurd geometry did not run thousands of steps");
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
	{ auto bad = s; bad.atmospherePathways[0].aAtmosphereId = core::StationExteriorAtmosphereId;
		bad.atmospherePathways[0].bAtmosphereId = core::StationExteriorAtmosphereId; refuse(bad, "self-loop"); }
	{ auto bad = s; bad.atmospheres[0].id = core::StationExteriorAtmosphereId;
		require(!core::validateStationSystemsSettings(bad, diagnostic)
			&& diagnostic.find("reserved") != std::string::npos, "exterior id was allowed as a real compartment");
		require(!world.configureStationSystems(bad, false, &diagnostic)
			&& world.getStationSystemsSnapshot() == baseline, "reserved exterior id refusal changed the World"); }
	{ auto bad = s; bad.atmospherePathways[0].bAtmosphereId.clear(); refuse(bad, "unknown"); }
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
	checks.push_back({"pathwayLongHorizon", pathwayLongHorizon});
	checks.push_back({"pathwayAbsurdGeometry", pathwayAbsurdGeometry});
	checks.push_back({"pathwayPersistence", pathwayPersistence});
	checks.push_back({"pathwayDiffusion", pathwayDiffusion});
	checks.push_back({"pathwayValidation", pathwayValidation});
	checks.push_back({"pathwayCounterFlow", pathwayCounterFlow});
	checks.push_back({"pathwayVacuumEqualization", pathwayVacuumEqualization});
	checks.push_back({"pathwayExteriorDiffusion", pathwayExteriorDiffusion});
	checks.push_back({"pathwayExteriorCommands", pathwayExteriorCommands});
	checks.push_back({"pathwayReferenceConfigurations", pathwayReferenceConfigurations});
	checks.push_back({"pathwayClosed", pathwayClosed});
	checks.push_back({"pathwayOpenness", pathwayOpenness});
	checks.push_back({"pathwayThermalNeutrality", pathwayThermalNeutrality});
	checks.push_back({"pathwayCondensation", pathwayCondensation});
	checks.push_back({"pathwayParallelAdditivity", pathwayParallelAdditivity});
	checks.push_back({"pathwayChainTopology", pathwayChainTopology});
	checks.push_back({"pathwayCommands", pathwayCommands});
	checks.push_back({"pathwayCommandOrdering", pathwayCommandOrdering});
	checks.push_back({"pathwayCommandReset", pathwayCommandReset});
	checks.push_back({"pathwayCommandValidation", pathwayCommandValidation});
}
