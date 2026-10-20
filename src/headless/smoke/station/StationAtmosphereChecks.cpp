#include "StationAtmosphereChecks.h"
#include "StationRig.h"
#include <limits>

namespace
{
	using namespace smoke::station;
	void atmospheres(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto atmosphere = [](std::string const& id,
			double volumeM3, double oxygenKg, double nitrogenKg,
			double carbonDioxideKg, double vapourKg, double temperatureC)
		{
			core::AtmosphereSettings settings;
			settings.id = id;
			settings.volumeM3 = volumeM3;
			settings.startingOxygenKg = oxygenKg;
			settings.startingNitrogenKg = nitrogenKg;
			settings.startingCarbonDioxideKg = carbonDioxideKg;
			settings.startingWaterVapourKg = vapourKg;
			settings.startingTemperatureC = temperatureC;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			settings.minOxygenPartialPressureKPa = 16.0;
			settings.maxCarbonDioxidePartialPressureKPa = 5.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		auto airlockCycle = [](std::string const& id, std::string const& atmosphereId,
			double gasMassKg, double firstSeconds, double recurrenceSeconds,
			double recoveredFraction, int priority = 0)
		{
			core::AirlockCycleSettings settings;
			settings.id = id;
			settings.atmosphereId = atmosphereId;
			settings.gasMassKg = gasMassKg;
			settings.firstOccurrenceSeconds = firstSeconds;
			settings.recurrenceSeconds = recurrenceSeconds;
			settings.recoveredFraction = recoveredFraction;
			settings.priority = priority;
			return settings;
		};
		auto atmosConfigure = [](core::World& world,
			std::vector<core::AtmosphereSettings> compartments,
			std::vector<core::AirlockCycleSettings> cycles = {},
			std::vector<core::ThermalReservoirSettings> reservoirs = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.atmospheres = std::move(compartments);
			settings.airlockCycles = std::move(cycles);
			settings.thermalReservoirs = std::move(reservoirs);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed compartment or cycle
		// contract is rejected before anything runs, and none of the
		// refusals mutates the World.
		{
			core::World bad("AtmosphereBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			std::string diagnostic;
			auto compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "ghost";
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"compartment coupled to an unknown reservoir was accepted");
			compartment = atmosphere("c", 0.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("volume must be finite and positive") != std::string::npos,
				"zero compartment volume was accepted");
			compartment = atmosphere("c", 100.0, -1.0, 80.0, 0.0, 0.0, 20.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("starting gas masses") != std::string::npos,
				"negative starting gas mass was accepted");
			compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, -300.0);
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a sub-absolute-zero compartment temperature was accepted");
			compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 0.0;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("thermal capacity must be finite and positive") != std::string::npos,
				"zero effective thermal capacity was accepted");
			compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.minPressureKPa = 150.0;
			compartment.maxPressureKPa = 100.0;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("min below max") != std::string::npos,
				"inverted pressure limits were accepted");
			compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = -1;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("occupant count") != std::string::npos,
				"a negative occupant count was accepted");
			compartment = atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = -0.1;
			settings.atmospheres = { compartment };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("leak rate") != std::string::npos,
				"a negative leak rate was accepted");
			settings.atmospheres = { atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) };
			settings.airlockCycles = { airlockCycle("x", "ghost", 1.0, 0.0, 60.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"an airlock cycle on an unknown compartment was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 0.0, 0.0, 60.0, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("cycled gas mass must be finite and positive") != std::string::npos,
				"a zero cycled gas mass was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 1.0, 0.0, 60.0, 1.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("recovered fraction") != std::string::npos,
				"a recovered fraction above one was accepted");
			settings.airlockCycles = { airlockCycle("x", "c", 1.0, 0.0, dt * 0.5, 0.5) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("at least one World fixed tick") != std::string::npos,
				"an airlock recurrence faster than one fixed tick was accepted");
			std::vector<core::AtmosphereSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationAtmospheres; ++index)
				tooMany.push_back(atmosphere("c" + std::to_string(index),
					100.0, 20.0, 80.0, 0.0, 0.0, 20.0));
			settings.airlockCycles = {};
			settings.atmospheres = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared compartment budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused atmosphere configuration mutated the World");
		}
		// Analytic mixture: absolute pressure is the sum of the ideal-gas
		// partial pressures, each derived from the declared mass, molar
		// mass, volume and temperature. With no flows configured the
		// starting derived pressure is also the current pressure.
		{
			core::World world("AtmosphereAnalytic", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(atmosConfigure(world, { atmosphere("lab",
				100.0, 23.2, 79.0, 1.0, 0.5, 20.0) }).first, "analytic atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "analytic run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres.size() == 1, "compartment missing from snapshot");
			auto const& lab = snap.atmospheres[0];
			auto const expectedOxygen = 23.2 / 0.0319988 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedNitrogen = 79.0 / 0.0280134 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedCo2 = 1.0 / 0.0440095 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			auto const expectedVapour = 0.5 / 0.0180153 * 8.314462618 * 293.15 / 100.0 / 1000.0;
			require(approx(lab.oxygenPartialPressureKPa, expectedOxygen, 1e-9),
				"oxygen partial pressure does not match the ideal-gas law");
			require(approx(lab.nitrogenPartialPressureKPa, expectedNitrogen, 1e-9),
				"nitrogen partial pressure does not match the ideal-gas law");
			require(approx(lab.carbonDioxidePartialPressureKPa, expectedCo2, 1e-9),
				"carbon dioxide partial pressure does not match the ideal-gas law");
			require(approx(lab.waterVapourPartialPressureKPa, expectedVapour, 1e-9),
				"water vapour partial pressure does not match the ideal-gas law");
			require(approx(lab.pressureKPa,
				expectedOxygen + expectedNitrogen + expectedCo2 + expectedVapour, 1e-9),
				"absolute pressure is not the sum of the partial pressures");
			require(approx(lab.startingPressureKPa, lab.pressureKPa, 1e-9),
				"starting pressure differs from current with no flows configured");
			require(approx(lab.relativeHumidity,
				expectedVapour / lab.saturationVapourPressureKPa, 1e-9),
				"relative humidity does not match the vapour/saturation ratio");
			require(lab.relativeHumidity <= 1.0 + 1e-9, "relative humidity above saturation");
			require(std::abs(lab.residualKJ) < 1e-6, "compartment thermal ledger not closed at start");
		}
		// Leakage: a constant total-mass rate, composition-proportional,
		// capped by the inventory, booked as explicit leaked mass with
		// the per-component ledger closing.
		{
			core::World world("AtmosphereLeak", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("hold", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = 0.06;
			require(atmosConfigure(world, { compartment }).first, "leaky atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "leak run failed");
			auto const& hold = world.getStationSystemsSnapshot().atmospheres[0];
			auto const leakedExpected = 0.06 * 600.0 * dt;
			require(approx(hold.leakedKg, leakedExpected, 1e-9),
				"leaked mass does not match the authored rate times elapsed time");
			require(approx(hold.oxygenKg + hold.nitrogenKg, 100.0 - leakedExpected, 1e-9),
				"remaining gas mass does not close over the leak");
			// Composition-proportional: the oxygen fraction is unchanged.
			auto const fraction = hold.oxygenKg / (hold.oxygenKg + hold.nitrogenKg);
			require(approx(fraction, 0.2, 1e-9),
				"leak did not preserve the gas composition proportionally");
			require(hold.pressureKPa < hold.startingPressureKPa,
				"pressure did not fall as the compartment leaked");
		}
		// A leak larger than the inventory drains the compartment to
		// empty and never below: the leak is capped by what is there.
		{
			core::World world("AtmosphereDrain", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("tiny", 10.0, 0.3, 0.2, 0.0, 0.0, 20.0);
			compartment.leakRateKgPerS = 1.0;
			require(atmosConfigure(world, { compartment }).first, "draining atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "drain run failed");
			auto const& tiny = world.getStationSystemsSnapshot().atmospheres[0];
			require(std::abs(tiny.oxygenKg) < 1e-9 && std::abs(tiny.nitrogenKg) < 1e-9,
				"the leak did not drain the compartment to empty");
			require(approx(tiny.leakedKg, 0.5, 1e-9),
				"leaked mass does not equal the whole starting inventory");
			require(approx(tiny.pressureKPa, 0.0, 1e-9),
				"an empty compartment still reports pressure");
		}
		// Zero/low inventory: the occupancy oxygen draw is capped by the
		// oxygen actually present, the shortfall stays visible as unmet
		// demand, and CO2 production scales with what was consumed.
		{
			core::World world("AtmosphereStarved", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("small", 100.0, 0.01, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = 10;
			compartment.oxygenUsePerOccupantKgPerS = 0.001;
			compartment.co2ProductionPerOccupantKgPerS = 0.001;
			compartment.sensibleHeatPerOccupantKW = 0.1;
			require(atmosConfigure(world, { compartment }).first, "starved atmosphere refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "starved run failed");
			auto const& small = world.getStationSystemsSnapshot().atmospheres[0];
			require(std::abs(small.oxygenKg) < 1e-9, "oxygen draw went below zero inventory");
			require(small.lastOxygenUnmetKg > 0.0,
				"the oxygen shortfall is not visible as unmet demand");
			// CO2 produced must never exceed the oxygen consumed: a
			// starved compartment cannot breathe out more than it breathed in.
			require(small.co2ProducedKg <= small.oxygenConsumedKg + 1e-9,
				"CO2 production exceeded oxygen consumption under starvation");
			require(small.status == core::StationTransferStatus::Rejected
				|| small.status == core::StationTransferStatus::Partial,
				"a starved compartment does not report its life-support shortfall");
			require(sawEvent(world, "life-support minimum"),
				"the low-oxygen condition was never announced");
		}
		// Vapour constraint: vapour above saturation condenses down to
		// the saturation mass, the latent heat warms the compartment, and
		// the condensate is reported rather than destroyed.
		{
			core::World world("AtmosphereVapour", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// 3 kg of vapour in 100 m3 at 20 C is far above the ~1.7 kg
			// saturation mass.
			require(atmosConfigure(world, { atmosphere("steam",
				100.0, 20.0, 80.0, 0.0, 3.0, 20.0) }).first, "vapour compartment refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "vapour run failed");
			auto const& steam = world.getStationSystemsSnapshot().atmospheres[0];
			// #564: the constraint is the saturation at the published,
			// coupled end-of-tick temperature - the latent heat warmed the
			// air, so the capacity it holds is larger than the tick-start
			// 20 C value the stale ordering used.
			auto const saturationKPa = core::stationSaturationVapourPressureKPa(steam.temperatureC);
			auto const saturatedMass = core::stationSaturatedVapourMassKg(
				saturationKPa, 100.0, steam.temperatureC);
			require(steam.condensedKg > 0.0, "supersaturated vapour did not condense");
			require(steam.waterVapourKg <= saturatedMass + 1e-9,
				"vapour stayed above the saturation mass");
			require(steam.temperatureC > 20.0,
				"condensation latent heat did not warm the compartment");
			require(steam.condensing, "condensation condition not exposed");
			require(sawEvent(world, "condensing"), "condensation onset not announced");
			require(steam.relativeHumidity <= 1.0 + 1e-9,
				"relative humidity above saturation after condensation");
			// The vapour ledger closes: starting = current + condensed.
			require(approx(3.0, steam.waterVapourKg + steam.condensedKg, 1e-9),
				"the vapour ledger does not close over condensation");
		}
		// #564: cooling saturated air integrates the temperature and the
		// phase change together. The published vapour inventory satisfies
		// saturation at the published temperature - not a clamped display
		// value over a supersaturated gas - the condensate and its latent
		// heat are booked exactly, and the latent feedback slows the
		// cooling by the heat the phase change absorbed.
		{
			core::World world("AtmosphereSaturatedCooling", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "cold";
			compartment.exchangerConductanceKWPerK = 30.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first, "saturated cooling setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(1), "saturated cooling tick failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& air = snap.atmospheres[0];
			auto const& cold = snap.thermal.reservoirs[0];
			require(air.temperatureC < 20.0 - 1e-3, "the cold exchanger did not cool the air");
			require(air.lastVapourCondensedKg > 0.0,
				"cooling saturated air booked no condensate at the published temperature");
			// The published gas satisfies saturation at its own
			// temperature: the vapour partial pressure never exceeds the
			// published saturation pressure. This is the supersaturation
			// the old tick-start ordering hid behind the clamped humidity.
			require(air.waterVapourPartialPressureKPa
				<= air.saturationVapourPressureKPa * (1.0 + 1e-9),
				"the published gas is supersaturated at its own temperature");
			require(approx(air.relativeHumidity, 1.0, 1e-9),
				"a cooling saturated compartment does not report its saturated state");
			// The latent feedback: the air ends warmer than a phase-change-
			// free tick would leave it. The exchange is the implicit star
			// solve against the cold bus, so the free end temperature is
			// known in closed form.
			auto const weightKJPerK = dt * 30.0 / (1.0 + dt * 30.0 / 1.0);
			auto const coupledBusC = (1000.0 * 0.0 + weightKJPerK * 20.0)
				/ (1000.0 + weightKJPerK);
			auto const freeTemperatureC = 20.0 + weightKJPerK * (coupledBusC - 20.0) / 1.0;
			require(air.temperatureC > freeTemperatureC + 1.0,
				"condensation latent heat did not slow the cooling");
			// The condensate is booked, not destroyed, and its latent heat
			// is in the compartment's thermal ledger.
			require(approx(compartment.startingWaterVapourKg,
				air.waterVapourKg + air.condensedKg, 1e-12),
				"the vapour ledger does not close over the coupled condensation");
			require(approx(air.latentHeatEnergyKJ, air.condensedKg * 2257.0, 1e-9),
				"the latent heat ledger does not match the booked condensate");
			require(std::abs(air.residualKJ) < 1e-9 && std::abs(cold.residualKJ) < 1e-6,
				"the coupled compartment or reservoir ledger did not close");
			require(approx(cold.lastAtmosphereHeatKW, -air.lastExchangeIntoCompartmentKW, 1e-9),
				"the reservoir did not book the exact negation of the compartment exchange");
		}
		// The latent heat feedback is a real thermal inertia: two
		// identical compartments cooled by identical buses, one holding
		// saturated vapour and one bone dry, do not cool alike - the wet
		// air gives up vapour and every gram of condensate spends the
		// compartment's cooling against the latent heat, so the dry air
		// reaches the cold side far sooner while the wet air tracks its
		// saturation curve the whole way down.
		{
			core::World world("AtmosphereLatentInertia", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto wet = atmosphere("wet", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			wet.thermalCapacityKJPerK = 1.0;
			wet.minOxygenPartialPressureKPa = 0.0;
			wet.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			wet.reservoirId = "cold-wet";
			wet.exchangerConductanceKWPerK = 1.0;
			auto dry = atmosphere("dry", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			dry.thermalCapacityKJPerK = 1.0;
			dry.minOxygenPartialPressureKPa = 0.0;
			dry.reservoirId = "cold-dry";
			dry.exchangerConductanceKWPerK = 1.0;
			require(atmosConfigure(world, { wet, dry }, {},
				{ reservoir("cold-wet", 1000.0, 0.0), reservoir("cold-dry", 1000.0, 0.0) }).first,
				"latent inertia setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(300), "latent inertia run failed");
			auto const& wetAir = world.getStationSystemsSnapshot().atmospheres[0];
			auto const& dryAir = world.getStationSystemsSnapshot().atmospheres[1];
			require(wetAir.condensedKg > 0.0 && dryAir.condensedKg == 0.0,
				"the latent inertia comparison did not condense only the wet air");
			require(wetAir.temperatureC > dryAir.temperatureC + 0.5,
				"the condensing air cooled as fast as the dry air - no latent feedback");
			require(wetAir.waterVapourPartialPressureKPa
				<= wetAir.saturationVapourPressureKPa * (1.0 + 1e-9)
				&& approx(wetAir.relativeHumidity, 1.0, 1e-9),
				"the wet air left its saturation curve during the long cool-down");
			require(std::abs(wetAir.residualKJ) < 1e-6 && std::abs(dryAir.residualKJ) < 1e-6,
				"a latent inertia ledger did not close");
		}
		// Heating: warming air raises its saturation capacity, so a
		// compartment starting saturated and gaining heat holds its
		// vapour, books no condensation, and its humidity falls below
		// saturation - the constraint binds only downward, and the
		// coupled solve does not invent condensation on the warming
		// branch.
		{
			core::World world("AtmosphereHeating", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "hot";
			compartment.exchangerConductanceKWPerK = 1.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("hot", 1000.0, 40.0) }).first, "heating setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "heating run failed");
			auto const& air = world.getStationSystemsSnapshot().atmospheres[0];
			require(air.temperatureC > 25.0, "the warm bus did not heat the air");
			require(air.condensedKg == 0.0 && air.lastVapourCondensedKg == 0.0,
				"warming air booked condensation it should not have");
			require(approx(air.waterVapourKg, compartment.startingWaterVapourKg, 1e-12),
				"the warming air lost vapour it never condensed");
			require(air.relativeHumidity < 1.0 - 1e-3,
				"warming saturated air still reports full humidity");
			require(std::abs(air.residualKJ) < 1e-6, "the heating ledger did not close");
		}
		// Replay: the coupled solve is a fixed-count bisection, so the
		// saturated cooling scenario replays identically after a reset -
		// the phase change is deterministic, not solver drift.
		{
			core::World world("AtmosphereCoolingReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("air", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.minOxygenPartialPressureKPa = 0.0;
			compartment.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			compartment.reservoirId = "cold";
			compartment.exchangerConductanceKWPerK = 30.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first, "cooling replay setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "cooling replay first pass failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.atmospheres[0].condensedKg > 0.0,
				"the replay scenario condensed nothing to replay");
			world.pauseSimulation();
			world.resetSimulation();
			world.resumeSimulation();
			require(world.advanceTicks(120), "cooling replay second pass failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the saturated cooling scenario is not deterministic across reset replay");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the saturated cooling event history is not deterministic across reset replay");
		}
		// Compartment isolation under condensation: a cooling,
		// condensing compartment shares the World with an untouched
		// neighbour and never moves a gram or a joule into it - the
		// condensate, the latent heat and the exchange all stay inside
		// the compartment that booked them.
		{
			core::World world("AtmosphereCondensationIsolation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto cooling = atmosphere("condensingSide", 1.0, 0.2, 0.8, 0.0, 0.0, 20.0);
			cooling.thermalCapacityKJPerK = 1.0;
			cooling.minOxygenPartialPressureKPa = 0.0;
			cooling.startingWaterVapourKg = core::stationSaturatedVapourMassKg(
				core::stationSaturationVapourPressureKPa(20.0), 1.0, 20.0);
			cooling.reservoirId = "cold";
			cooling.exchangerConductanceKWPerK = 30.0;
			auto const neighbour = atmosphere("hazardSide", 100.0,
				15.0, 85.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { cooling, neighbour }, {},
				{ reservoir("cold", 1000.0, 0.0) }).first,
				"condensation isolation pair refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "condensation isolation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres[0].condensedKg > 0.0,
				"the isolation scenario's cooling side condensed nothing");
			auto const& hazard = snap.atmospheres[1];
			require(approx(hazard.oxygenKg, 15.0, 1e-12)
				&& approx(hazard.nitrogenKg, 85.0, 1e-12)
				&& approx(hazard.waterVapourKg, 0.0, 1e-12)
				&& approx(hazard.condensedKg, 0.0, 1e-12)
				&& approx(hazard.leakedKg, 0.0, 1e-12),
				"the neighbour gained mass, condensate or loss from the condensing side");
			require(approx(hazard.temperatureC, 20.0, 1e-12)
				&& approx(hazard.lastExchangeIntoCompartmentKW, 0.0, 1e-12),
				"the neighbour's thermal state moved with the other compartment's cooling");
			require(approx(hazard.pressureKPa, hazard.startingPressureKPa, 1e-12),
				"the neighbour's pressure moved without any process");
		}
		// Compartment isolation: a leaking, cycled compartment never
		// moves a gram into its neighbour - normal and hazardous
		// atmospheres are segregated by construction.
		{
			core::World world("AtmosphereIsolation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto leaking = atmosphere("normalSide", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			leaking.leakRateKgPerS = 0.06;
			auto const neighbour = atmosphere("hazardSide", 100.0, 15.0, 85.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { leaking, neighbour },
				{ airlockCycle("vent", "normalSide", 1.0, 1.0, 0.0, 0.5) }).first,
				"isolation pair refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "isolation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.atmospheres.size() == 2, "isolation pair lost a compartment");
			auto const& hazard = snap.atmospheres[1];
			require(approx(hazard.oxygenKg, 15.0, 1e-12)
				&& approx(hazard.nitrogenKg, 85.0, 1e-12)
				&& approx(hazard.leakedKg, 0.0, 1e-12)
				&& approx(hazard.recoveredKg, 0.0, 1e-12)
				&& approx(hazard.lostKg, 0.0, 1e-12),
				"the neighbouring compartment was touched by the other's leak or cycle");
			require(approx(hazard.pressureKPa, hazard.startingPressureKPa, 1e-12),
				"the neighbouring compartment pressure moved without any process");
		}
		// Deterministic scheduled airlock losses: occurrences fire on
	// schedule with the recovered-versus-lost split, a partial cycle
		// when the compartment cannot supply the full mass, a refusal
		// when it holds nothing, and a reset replays the whole scenario
		// identically.
		{
			core::World world("AtmosphereAirlock", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(atmosConfigure(world,
				{ atmosphere("dock", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0),
				  atmosphere("stub", 10.0, 0.4, 0.1, 0.0, 0.0, 20.0) },
				{ airlockCycle("crew", "dock", 1.0, 10.0, 5.0, 0.9),
				  airlockCycle("last", "stub", 10.0, 5.0, 0.0, 0.5) }).first,
				"airlock scenario refused");
			world.resumeSimulation();
			require(world.advanceTicks(1800), "airlock scenario run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.airlockCycles.size() == 2, "airlock cycles missing from snapshot");
			auto const& crew = snap.airlockCycles[0];
			// Occurrences every 5 s from 10 s: at 10, 15, 20, 25 and 30 s
			// inside the 30-second (1800-tick) run.
			require(crew.occurrencesCompleted == 5,
				"the recurring airlock cycle did not fire the expected occurrences");
			require(approx(crew.removedTotal, 5.0, 1e-9),
				"the recurring cycle did not remove the scheduled mass");
			require(approx(crew.recoveredTotal, 4.5, 1e-9),
				"the recovered total does not match the authored fraction");
			require(approx(crew.lostTotal, 0.5, 1e-9),
				"the lost total does not match the removal minus recovery");
			auto const& last = snap.airlockCycles[1];
			require(last.occurrencesCompleted == 1 && last.exhausted,
				"the one-off airlock cycle did not complete and exhaust");
			// The stub holds 0.5 kg; the 10 kg cycle removes what is
			// there and reports the partial.
			require(approx(last.lastRemovedKg, 0.5, 1e-9),
				"the one-off cycle did not remove the capped partial mass");
			require(last.status == core::StationTransferStatus::Partial,
				"a capped cycle does not report a partial status");
			require(approx(snap.atmospheres[1].oxygenKg, 0.0, 1e-9)
				&& approx(snap.atmospheres[1].nitrogenKg, 0.0, 1e-9),
				"the capped cycle left mass behind that it said it removed");
			require(approx(snap.atmospheres[0].recoveredKg, 4.5, 1e-9)
				&& approx(snap.atmospheres[0].lostKg, 0.5, 1e-9),
				"the compartment recovered/lost ledger does not match the cycle history");
			require(sawEvent(world, "Airlock cycle 'crew'")
				&& sawEvent(world, "recovered"),
				"airlock cycle occurrences were not announced with their split");
			// Reset and replay: identical snapshots and events.
			auto const expected = snap;
			auto const expectedEvents = world.getStationSystemsEvents();
			world.pauseSimulation();
			world.resetSimulation();
			require(world.getStationSystemsSnapshot().atmospheres.empty()
				|| world.getStationSystemsSnapshot().atmospheres[0].leakedKg == 0.0,
				"reset did not reconstruct the compartment starting state");
			world.resumeSimulation();
			require(world.advanceTicks(1800), "airlock replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"the airlock scenario is not deterministic across reset replay");
			require(world.getStationSystemsEvents() == expectedEvents,
				"the airlock event history is not deterministic across reset replay");
		}
		// Thermal coupling: a compartment coupled to a warmer reservoir
		// gains exactly what the reservoir loses through the heat-only
		// exchanger; an uncoupled compartment exchanges nothing.
		{
			core::World world("AtmosphereThermal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto warm = atmosphere("warmSide", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			warm.reservoirId = "bus";
			warm.exchangerConductanceKWPerK = 2.0;
			auto const adiabatic = atmosphere("sealed", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			require(atmosConfigure(world, { warm, adiabatic }, {},
				{ reservoir("bus", 50000.0, 40.0) }).first, "thermal coupling refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "thermal coupling run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& warmSide = snap.atmospheres[0];
			auto const& sealed = snap.atmospheres[1];
			auto const& bus = snap.thermal.reservoirs[0];
			require(warmSide.thermallyCoupled && !sealed.thermallyCoupled,
				"coupling flags wrong on the compartment snapshots");
			require(warmSide.temperatureC > 20.0,
				"the coupled compartment did not warm from the warmer reservoir");
			require(bus.temperatureC < 40.0,
				"the reservoir did not cool by giving heat to the compartment");
			require(approx(bus.lastAtmosphereHeatKW, -warmSide.lastExchangeIntoCompartmentKW, 1e-9),
				"the reservoir did not book the exact negation of the compartment exchange");
			require(std::abs(warmSide.residualKJ) < 1e-6
				&& std::abs(bus.residualKJ) < 1e-6,
				"the compartment or reservoir energy ledger did not close");
			require(approx(sealed.temperatureC, 20.0, 1e-12)
				&& approx(sealed.lastExchangeIntoCompartmentKW, 0.0, 1e-12),
				"the adiabatic compartment exchanged heat it should not have");
			require(approx(bus.atmosphereEnergyKJ, -warmSide.exchangedEnergyKJ, 1e-6),
				"the cumulative atmosphere exchange terms are not exact negations");
		}
		// #559: a stiff exchanger between a small compartment and a small
		// reservoir. A tick-start rate applied whole drove the air from
		// 20 C to -163 C in one tick, which reads out as a negative
		// absolute pressure. The exchange is solved implicitly against the
		// reservoir's end-of-tick temperature, so the air and the bus
		// approach their shared equilibrium and cannot cross it, reverse,
		// or leave the range the tick started inside — and the pressure
		// stays physical because the temperature does.
		{
			core::World world("AtmosphereStiffExchange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("stiffAir", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			require(atmosConfigure(world, { compartment }, {},
				{ reservoir("bus", 1.0, 0.0) }).first, "stiff air coupling refused");
			world.resumeSimulation();
			// C_eff for the air/bus pair bounds the heat one tick of
			// passive exchange may move however stiff the conductance.
			auto const effectiveCapacityKJPerK = 0.5;
			auto previousGapK = 20.0;
			for (int tick = 0; tick < 120; ++tick)
			{
				require(world.advanceTicks(1), "stiff air tick failed");
				auto const& air = world.getStationSystemsSnapshot().atmospheres[0];
				auto const& bus = world.getStationSystemsSnapshot().thermal.reservoirs[0];
				// The air never leaves the range the exchange started in:
				// no plunge below the cold side, no overshoot past its own
				// starting warmth, and nothing near absolute zero.
				require(air.temperatureC > 0.0
					&& air.temperatureC <= 20.0 + 1e-9,
					"stiff exchange drove the compartment outside its starting range");
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"stiff exchange drove the reservoir outside the starting range");
				// A physical temperature keeps a physical pressure: the
				// reported absolute pressure stays positive.
				require(air.pressureKPa > 0.0,
					"stiff exchange produced a non-physical compartment pressure");
				// No reversal and no amplification: the warm side stays the
				// warm side and the gap only ever closes.
				auto const gapK = air.temperatureC - bus.temperatureC;
				require(gapK >= -1e-9, "stiff exchange reversed warm air and cold bus");
				require(gapK <= previousGapK + 1e-9,
					"stiff exchange amplified the air/bus separation");
				require(std::abs(air.lastExchangeIntoCompartmentKW) * dt
					<= effectiveCapacityKJPerK * previousGapK + 1e-9,
					"air exchange moved more heat than the pair's equilibrium allows");
				require(approx(bus.atmosphereEnergyKJ, -air.exchangedEnergyKJ, 1e-6),
					"stiff air exchange is not equal and opposite across the ledgers");
				require(std::abs(air.residualKJ) < 1e-6
					&& std::abs(bus.residualKJ) < 1e-6,
					"stiff-air compartment or reservoir ledger did not close");
				previousGapK = gapK;
			}
			auto const& settled = world.getStationSystemsSnapshot();
			require(settled.atmospheres[0].temperatureC
				- settled.thermal.reservoirs[0].temperatureC < 0.05,
				"stiff air exchange never settled onto the shared equilibrium");
		}
		// A reservoir shared by a loop and a compartment is one star, not
		// two independent edges: both leaves drain the same finite heat,
		// so neither the bus nor either leaf leaves the range they started
		// in, and the bus books exactly what the two together gave up.
		{
			core::World world("AtmosphereSharedReservoir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = { reservoir("bus", 1.0, 0.0) };
			auto compartment = atmosphere("air", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			settings.atmospheres = { compartment };
			core::CoolantLoopSettings hotLoop;
			hotLoop.id = "hot-loop";
			hotLoop.reservoirId = "bus";
			hotLoop.coolantCapacityKg = 1.0;
			hotLoop.startingCoolantKg = 1.0;
			hotLoop.specificHeatKJPerKgK = 1.0;
			hotLoop.startingTemperatureC = 20.0;
			hotLoop.freezeProtectionSetpointC = 5.0;
			hotLoop.pumpPowerKW = 1.0;
			hotLoop.ratedFlowKgPerS = 1.0;
			hotLoop.exchangerConductanceKWPerK = 1000.0;
			hotLoop.heaterCapacityKW = 0.0;
			settings.coolantLoops = { hotLoop };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			for (int tick = 0; tick < 60; ++tick)
			{
				require(world.advanceTicks(1), "shared air/loop tick failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& bus = snapshot.thermal.reservoirs[0];
				auto const& air = snapshot.atmospheres[0];
				auto const& loopSnap = snapshot.coolantLoops[0];
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"the shared bus left the starting temperature range");
				require(air.temperatureC > 0.0 && air.temperatureC <= 20.0 + 1e-9,
					"the shared compartment left the starting temperature range");
				require(loopSnap.loopTemperatureC > 0.0
					&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
					"the shared loop left the starting temperature range");
				require(approx(bus.atmosphereEnergyKJ + bus.loopEnergyKJ,
					-(air.exchangedEnergyKJ + loopSnap.exchangedEnergyKJ), 1e-6),
					"the shared bus did not book exactly what its leaves gave up");
				require(std::abs(bus.residualKJ) < 1e-6,
					"shared air/loop bus ledger did not close");
			}
		}
		// #559 in the sandbox: the bundled Lua model solves the same star
		// for compartments, asserting its own air temperature stays inside
		// the starting range and never reverses, while the host reports
		// the exact tick-one exchange the closed form predicts.
		{
			auto const coupling = dt * 100.0 / 1.0;
			auto const weightKJPerK = dt * 100.0 / (1.0 + coupling);
			auto const coupledTemperatureC = (1.0 * 0.0 + weightKJPerK * 20.0)
				/ (1.0 + weightKJPerK);
			auto const expectedExchangeKW = weightKJPerK * (coupledTemperatureC - 20.0) / dt;
			char literal[64];
			std::snprintf(literal, sizeof(literal), "%.17g", expectedExchangeKW);
			auto const expectedLiteral = std::string(literal);
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled, expectedLiteral] {
				return std::string("local factory = (function()\n") + bundled
					+ "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local air, bus = self.atmospheres[1], self.reservoirs[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs((air.pendingExchangeKW or 0) - " + expectedLiteral
					+ ") < 1e-9, 'sandbox air exchange did not follow the implicit star solve')\n"
					"end\n"
					"assert(air.temperature > 0.0 and air.temperature <= 20.0 + 1e-9,\n"
					"  'sandbox stiff exchange drove the compartment outside its starting range')\n"
					"assert(bus.temperature >= 0.0 and bus.temperature < 20.0,\n"
					"  'sandbox stiff exchange drove the reservoir outside the starting range')\n"
					"assert(air.temperature >= bus.temperature - 1e-9,\n"
					"  'sandbox stiff exchange reversed warm air and cold bus')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandboxStiff("SandboxStiffAir", 2, 2);
			sandboxStiff.pauseSimulation(); sandboxStiff.finishBuild();
			auto compartment = atmosphere("stiffAir", 100.0, 30.0, 70.0, 0.0, 0.0, 20.0);
			compartment.reservoirId = "bus";
			compartment.thermalCapacityKJPerK = 1.0;
			compartment.exchangerConductanceKWPerK = 100.0;
			require(atmosConfigure(sandboxStiff, { compartment }, {},
				{ reservoir("bus", 1.0, 0.0) }).first, "sandbox stiff air coupling refused");
			sandboxStiff.resumeSimulation();
			require(sandboxStiff.advanceTicks(60),
				"Bundled Lua implicit air exchange failed its in-sandbox assertions");
			require(sandboxStiff.getStationSystemsSnapshot().atmospheres[0].exchangedEnergyKJ < 0.0,
				"host did not mirror the sandbox's heat-out-of-compartment exchange");
			core::setStationSystemsSourceLoader({});
		}
		// The compartment's occupancy heat and the reservoir coupling
		// both land in the same integrated thermal state, and the
		// initial-versus-current exposure makes the drift visible.
		{
			core::World world("AtmosphereOccupancyHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto compartment = atmosphere("crew", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0);
			compartment.occupants = 4;
			compartment.sensibleHeatPerOccupantKW = 0.1;
			require(atmosConfigure(world, { compartment }).first, "occupancy heat refused");
			world.resumeSimulation();
			require(world.advanceTicks(600), "occupancy heat run failed");
			auto const& crew = world.getStationSystemsSnapshot().atmospheres[0];
			// 4 x 0.1 kW for 10 s into 10000 kJ/K: +0.004 C.
			require(approx(crew.temperatureC, 20.0 + 0.4 * 10.0 / 10000.0, 1e-9),
				"occupant sensible heat did not integrate as authored");
			require(crew.temperatureC > crew.startingTemperatureC,
				"initial versus current temperature does not show the drift");
			require(std::abs(crew.residualKJ) < 1e-6, "occupancy heat ledger not closed");
		}
		// The Lua sandbox mirror: a corrupt atmosphere descriptor crossing
		// is contained at construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(atmospheres) == 'table' and #atmospheres == 1, 'atmosphere descriptor crossing lost')\n"
					"assert(atmospheres[1].volumeM3 > 0, 'corrupt atmosphere volume crossing')\n"
					"error('corrupt atmosphere descriptor crossing')\n"
					"end";
			});
			core::World corrupt("AtmosphereCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = atmosConfigure(corrupt,
				{ atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) });
			require(!result.first && result.second.find("corrupt atmosphere descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt atmosphere sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
		// A corrupt airlock cycle crossing is contained the same way.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(airlockCycles) == 'table' and #airlockCycles == 1, 'airlock cycle descriptor crossing lost')\n"
					"error('corrupt airlock cycle descriptor crossing')\n"
					"end";
			});
			core::World corrupt("AirlockCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const result = atmosConfigure(corrupt,
				{ atmosphere("c", 100.0, 20.0, 80.0, 0.0, 0.0, 20.0) },
				{ airlockCycle("x", "c", 1.0, 0.0, 60.0, 0.5) });
			require(!result.first && result.second.find("corrupt airlock cycle descriptor crossing")
				!= std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt airlock cycle sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}

	// Cross-class wiring moves mass. Water treatment qualifies output from
	// complete energy evidence alone (#753); partial doses remain held.
	// Heat-only interfaces move heat, never mass.
	void containment(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto store = [](std::string const& id,
			double capacity, double qualified, double held = 0.0,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = qualified;
			settings.startingUnqualified = held;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto hazTreatment = [](std::string const& id)
		{
			core::WaterTreatmentSettings settings;
			settings.id = id;
			settings.hazardClass = "hazardous";
			settings.sourceStoreId = "hazRaw";
			settings.treatedStoreId = "hazTreated";
			settings.concentrateStoreId = "hazConcentrate";
			settings.consumableStoreId = "hazConsumable";
			settings.throughputKgPerS = 0.005;
			settings.recoveryRatio = 0.8;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			return settings;
		};
		auto hazStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(store("potable", 100.0, 50.0));
			stores.push_back(store("normTreated", 100.0, 0.0));
			stores.push_back(store("normConcentrate", 100.0, 0.0));
			stores.push_back(store("normConsumable", 100.0, 10.0));
			stores.push_back(store("hazRaw", 200.0, 100.0));
			stores.push_back(store("hazTreated", 100.0, 0.0));
			stores.push_back(store("hazConcentrate", 100.0, 0.0));
			stores.push_back(store("hazConsumable", 100.0, 10.0));
			stores.push_back(store("hazReserve", 100.0, 0.0));
			return stores;
		};
		auto hazConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::WaterTreatmentSettings> treatments,
			std::vector<core::ResourceTransferSettings> transfers,
			double solarKW)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.waterTreatments = std::move(treatments);
			settings.transfers = std::move(transfers);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: nothing that misdeclares a class
		// reaches runtime, and a refusal
		// never mutates the World.
		{
			core::World world("ContainmentRefused", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::string diagnostic;
			{
				// A normal-class treatment drawing from a hazardous store
				// is accepted (#751): the class no longer binds the
				// process, and the crossing is reported by the moved mass.
				// The consequence is asserted in the CrossClassTreatment
				// block below.
				core::WaterTreatmentSettings normalCrossClass;
				normalCrossClass.id = "potablePlant";
				normalCrossClass.hazardClass = "normal";
				normalCrossClass.sourceStoreId = "hazRaw";
				normalCrossClass.treatedStoreId = "potable";
				normalCrossClass.concentrateStoreId = "hazConcentrate";
				normalCrossClass.consumableStoreId = "hazConsumable";
				normalCrossClass.throughputKgPerS = 0.001;
				normalCrossClass.recoveryRatio = 0.8;
				normalCrossClass.specificEnergyKWhPerKg = 0.5;
				normalCrossClass.consumablePerKg = 0.01;
				{
					auto settings = core::StationSystemsSettings{};
					settings.enabled = true;
					settings.stores = hazStores();
					settings.waterTreatments = { normalCrossClass };
					require(core::validateStationSystemsSettings(settings, diagnostic),
						"a normal-class treatment drawing from a hazardous store was refused");
				}
				core::World crossWorld("CrossClassTreatment", 2, 2);
				crossWorld.pauseSimulation(); crossWorld.finishBuild();
				require(hazConfigure(crossWorld, hazStores(), { normalCrossClass }, {}, 20.0).first,
					"a cross-class treatment was refused at configuration");
				crossWorld.resumeSimulation();
				require(crossWorld.advanceTicks(60), "cross-class treatment run failed");
				auto const elapsed = 60.0 * dt;
				auto const snap = crossWorld.getStationSystemsSnapshot();
				require(snap.waterTreatments.size() == 1,
					"cross-class treatment missing from snapshot");
				auto const processed = 0.001 * elapsed;
				require(approx(findStore(snap, "potable").currentQualified, 50.0 + 0.8 * processed),
					"the normal destination did not receive the treated hazardous mass");
				require(approx(findStore(snap, "hazConcentrate").currentUnqualified, 0.2 * processed),
					"the retained concentrate did not book the unrecovered load");
				require(approx(findStore(snap, "hazRaw").currentQualified, 100.0 - processed),
					"the hazardous source did not drain by the processed mass");
				for (auto const& balance : snap.balances)
					require(std::abs(balance.residual) <= 1e-6,
						("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
			}
			require(!world.getStationSystemsSnapshot().enabled && world.getSimulationTick() == 0,
				"a refused containment configuration mutated the World");
		}
		// Cross-class transfers are legal (#750): wiring that the class
		// boundary once refuses now configures, runs, and moves mass in
		// both directions; the destination fills and the ledger closes.
		{
			core::World world("CrossClassTransfer", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceTransferSettings down;
			down.id = "down";
			down.sourceId = "potable";
			down.destinationId = "hazReserve";
			down.unit = "kg";
			down.ratePerSecond = 0.001;
			core::ResourceTransferSettings up;
			up.id = "up";
			up.sourceId = "hazRaw";
			up.destinationId = "normTreated";
			up.unit = "kg";
			up.ratePerSecond = 0.002;
			std::string diagnostic;
			require(hazConfigure(world, hazStores(), {}, { down, up }, 20.0).first,
				"a cross-class transfer pair was refused at configuration");
			world.resumeSimulation();
			require(world.advanceTicks(60), "cross-class transfer run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.transfers.size() == 2, "cross-class transfers missing from snapshot");
			require(approx(snap.transfers[0].lastCompleted, 0.001 * dt)
				&& snap.transfers[0].status == core::StationTransferStatus::Full,
				"the normal-to-hazardous transfer did not run at full rate");
			require(approx(snap.transfers[1].lastCompleted, 0.002 * dt)
				&& snap.transfers[1].status == core::StationTransferStatus::Full,
				"the hazardous-to-normal transfer did not run at full rate");
			require(approx(findStore(snap, "hazReserve").currentQualified, 0.001 * elapsed),
				"the hazardous destination did not fill with transferred normal mass");
			require(approx(findStore(snap, "normTreated").currentQualified, 0.002 * elapsed),
				"the normal destination did not fill with transferred hazardous mass");
			require(approx(findStore(snap, "potable").currentQualified, 50.0 - 0.001 * elapsed),
				"the normal source did not drain by the transferred mass");
			require(approx(findStore(snap, "hazRaw").currentQualified, 100.0 - 0.002 * elapsed),
				"the hazardous source did not drain by the transferred mass");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Cross-class conversions are legal (#750): a conversion whose
		// endpoints differ in class is accepted and produces its declared
		// outputs; the loss below the input is booked, not created.
		{
			core::World world("CrossClassConversion", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazStores();
			core::ResourceCompositionSettings composition;
			composition.moisture = 0.5;
			for (auto& entry : settings.stores)
			{
				if (entry.id == "hazRaw" || entry.id == "potable")
					entry.composition = composition;
			}
			core::ResourceConversionSettings cross;
			cross.id = "launder";
			cross.inputs.push_back({ "hazRaw", 0.001 });
			cross.outputs.push_back({ "potable", 0.0005 });
			settings.conversions = { cross };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 20.0f;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(core::validateStationSystemsSettings(settings, diagnostic), diagnostic.c_str());
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "cross-class conversion run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.conversions.size() == 1, "cross-class conversion missing from snapshot");
			auto const& launder = snap.conversions[0];
			require(launder.lastScale > 0.99 && launder.status == core::StationTransferStatus::Full,
				"the cross-class conversion did not run at full declared throughput");
			require(approx(launder.inputs[0].completed, 0.001 * dt)
				&& approx(launder.outputs[0].completed, 0.0005 * dt),
				"the cross-class conversion did not produce its declared outputs");
			require(approx(findStore(snap, "potable").currentQualified, 50.0 + 0.0005 * elapsed),
				"the normal destination did not fill with converted hazardous mass");
			require(approx(findStore(snap, "hazRaw").currentQualified, 100.0 - 0.001 * elapsed),
				"the hazardous source did not drain by the converted mass");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Fully evidenced recovery qualifies output and permits ordinary transfers.
		{
			core::World world("ClearedRecovery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceTransferSettings recover;
			recover.id = "recover";
			recover.sourceId = "hazTreated";
			recover.destinationId = "hazReserve";
			recover.unit = "kg";
			recover.ratePerSecond = 0.002;
			require(hazConfigure(world, hazStores(),
				{ hazTreatment("plant") },
				{ recover }, 20.0).first, "cleared recovery setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "cleared recovery run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.waterTreatments.size() == 1, "treatment missing from snapshot");
			auto const& plant = snap.waterTreatments[0];
			require(plant.filtrationCompleted && plant.treatmentEvidenced
				&& plant.qualityAccepted,
				"a full-dose, cleared hazardous treatment is not reported as released");
			auto const treated = 0.005 * elapsed * 0.8;
			require(approx(plant.acceptedKg, treated) && approx(plant.heldKg, 0.0),
				"cleared hazardous output was not fully accepted");
			// The same-class recovery transfer moved qualified material.
			// The treatment runs before the transfer in the tick order, so
			// the transfer moves its full rate from the first tick on.
			require(approx(snap.transfers[0].lastCompleted, 0.002 * dt)
				&& snap.transfers[0].status == core::StationTransferStatus::Full,
				"the hazardous-class recovery transfer did not run");
			require(approx(findStore(snap, "hazReserve").currentQualified,
				0.002 * elapsed),
				"the hazardous reserve did not receive the recovered material");
			// The normal inventory never saw a gram of the hazardous stream.
			auto const potable = findStore(snap, "potable");
			require(approx(potable.currentQualified, 50.0)
				&& potable.lastTickIn == 0.0 && potable.lastTickOut == 0.0,
				"the normal inventory was touched by the hazardous chain");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// No release-test supply exists: energy alone qualifies every batch.
		{
			core::World world("EnergyOnlyQualification", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, hazStores(), {hazTreatment("plant")}, {}, 20.0).first,
				"energy-only treatment refused");
			world.resumeSimulation(); require(world.advanceTicks(120), "energy-only run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.qualityAccepted && plant.treatmentEvidenced && plant.heldKg == 0
				&& approx(plant.acceptedKg, plant.treatedKg), "complete evidence did not qualify all output");
			require(approx(plant.consumablesUsed, plant.processedKg * 0.01),
				"treatment spent more than its process consumable");
		}
		// A curtailed power dose leaves the whole output unqualified.
		{
			core::World world("EvidenceFailed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, hazStores(),
				{ hazTreatment("plant") },
				{ }, 1.0).first, "evidence-failed setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "evidence-failed run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(plant.filtrationCompleted, "the process ran without a filtration pass");
			require(!plant.treatmentEvidenced && !plant.qualityAccepted,
				"a curtailed dose was reported as evidenced or cleared");
			require(plant.holdReason.find("energy curtailed") != std::string::npos,
				"the evidence failure carries no stated hold reason");
			require(approx(plant.heldKg, plant.treatedKg)
				&& approx(plant.acceptedKg, 0.0),
				"the unevidenced output was not held whole");
		}
		// Process consumables constrain throughput and preserve their reserve.
		auto processKitStores = [&](double kitStarting, double kitReserve)
		{
			auto stores = hazStores();
			for (auto& entry : stores)
				if (entry.id == "hazConsumable")
				{
					entry.startingQualified = kitStarting;
					entry.protectedReserve = kitReserve;
				}
			return stores;
		};
		{
			// Sufficient stock funds the full process, with no additional test draw.
			core::World world("ProcessKitCovered", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, processKitStores(1.0, 0.0),
				{ hazTreatment("plant") },
				{ }, 20.0).first,
				"the process consumable was refused at configuration");
			world.resumeSimulation();
			require(world.advanceTicks(60), "process-kit-covered run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			auto const processed = 0.005 * elapsed;
			auto const treated = processed * 0.8;
			require(plant.qualityAccepted && plant.treatmentEvidenced
				&& approx(plant.acceptedKg, treated) && approx(plant.heldKg, 0.0),
				"the process kit did not release the treated output at full scale");
			require(approx(plant.consumablesUsed, processed * 0.01),
				"the treatment-role draw did not track the processed mass");
			require(approx(findStore(snap, "hazConsumable").currentQualified,
				1.0 - processed * 0.01),
				"the process kit was not drawn by exactly the process demand");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		{
			// Scarce stock throttles throughput without overdraw or lost mass.
			core::World world("ProcessKitThrottled", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto treatment = hazTreatment("plant");
			treatment.throughputKgPerS = 1.0;
			treatment.recoveryRatio = 1.0;
			treatment.specificEnergyKWhPerKg = 0.0;
			treatment.consumablePerKg = 1.0;
			require(hazConfigure(world, processKitStores(0.02, 0.0),
				{ treatment }, { }, 20.0).first,
				"the throttled process-kit setup was refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "process-kit-throttled run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(approx(plant.acceptedKg, 0.02) && approx(plant.heldKg, 0.0),
				"the throttled process-kit run did not release exactly the paid-for output");
			require(approx(plant.consumablesUsed, 0.02),
				"the consumable draw did not account for the processed mass");
			require(findStore(snap, "hazConsumable").currentQualified >= 0.0
				&& approx(findStore(snap, "hazConsumable").currentQualified, 0.0),
				"the process kit was overdrawn below zero");
			require(plant.scale == 0.0
				&& plant.limiter.find("treatment consumable store")
					!= std::string::npos,
				"the exhausted process kit is not reported as the limiter");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		{
			// The protected reserve is never spent: the reservation is
			// made above it, so the process drains only the drawable
			// share and the reserve stays whole behind it.
			core::World world("ProcessKitReserve", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto treatment = hazTreatment("plant");
			treatment.throughputKgPerS = 1.0;
			treatment.recoveryRatio = 1.0;
			treatment.specificEnergyKWhPerKg = 0.0;
			treatment.consumablePerKg = 1.0;
			require(hazConfigure(world, processKitStores(0.02, 0.01),
				{ treatment }, { }, 20.0).first,
				"the reserve-protected process-kit setup was refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "reserve-protected process-kit run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& plant = snap.waterTreatments[0];
			require(approx(plant.acceptedKg, 0.01) && approx(plant.heldKg, 0.0),
				"the reserve-protected run did not release the drawable-paid output");
			require(approx(plant.consumablesUsed, 0.01),
				"the combined draw exceeded the drawable share above the reserve");
			require(findStore(snap, "hazConsumable").currentQualified >= 0.01 - 1e-9
				&& approx(findStore(snap, "hazConsumable").currentQualified, 0.01),
				"the protected reserve was not preserved intact behind the draw");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Heat-only interfaces (#752): the heat-only exchanger, through
		// which no coolant ever crossed, is the containment. A loop
		// exchanges heat with its reservoir and its freeze heater runs,
		// but the loop's coolant inventory never changes and no Resource
		// store is ever touched - heat rides the interface, mass never
		// does.
		{
			core::World world("HeatOnly", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = hazStores();
			core::ThermalReservoirSettings sink;
			sink.id = "sink";
			sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 10000.0;
			sink.startingTemperatureC = 10.0;
			sink.minTemperatureC = -50.0;
			sink.maxTemperatureC = 200.0;
			settings.thermalReservoirs = { sink };
			core::CoolantLoopSettings loop;
			loop.id = "hazLoop";
			loop.reservoirId = "sink";
			loop.coolantCapacityKg = 50.0;
			loop.startingCoolantKg = 40.0;
			loop.specificHeatKJPerKgK = 4.0;
			loop.startingTemperatureC = 0.0;
			loop.freezeProtectionSetpointC = 5.0;
			loop.pumpPowerKW = 0.1;
			loop.ratedFlowKgPerS = 5.0;
			loop.exchangerConductanceKWPerK = 1.0;
			loop.heaterCapacityKW = 1.0;
			settings.coolantLoops = { loop };
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 5.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				("heat-only setup refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(120), "heat-only run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.coolantLoops.size() == 1, "loop missing from snapshot");
			auto const& live = snap.coolantLoops[0];
			require(approx(live.coolantKg, 40.0),
				"the heat-only interface changed the loop's coolant inventory");
			require(live.heaterSuppliedKW > 0.0 && live.pumpSuppliedKW > 0.0,
				"the freeze-protection heater or pump did not run");
			require(live.loopTemperatureC > 0.0,
				"the heater did not warm the loop");
			for (auto const& entry : snap.stores)
				require(entry.lastTickIn == 0.0 && entry.lastTickOut == 0.0
					&& approx(entry.currentQualified, entry.startingQualified)
					&& approx(entry.currentUnqualified, entry.startingUnqualified),
					("the heat-only interface moved mass through store '" + entry.id + "'").c_str());
			require(snap.thermal.reservoirs[0].loopEnergyKJ != 0.0,
				"the loop-reservoir heat exchange did not happen at all");
		}
		// Disposal-ready hazardous waste stays segregated: each ejection
		// contract removes only its own store's qualified contents, the
		// held stock stays behind, and one class's ejection never touches
		// the other class's store.
		{
			core::World world("WasteSegregation", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = {
				store("normWaste", 100.0, 10.0, 5.0),
				store("hazWaste", 100.0, 10.0, 5.0) };
			core::WasteEjectionSettings ejectNorm;
			ejectNorm.id = "ejectNorm";
			ejectNorm.storeId = "normWaste";
			ejectNorm.unit = "kg";
			ejectNorm.quantity = 4.0;
			ejectNorm.firstOccurrenceSeconds = 0.5;
			core::WasteEjectionSettings ejectHaz;
			ejectHaz.id = "ejectHaz";
			ejectHaz.storeId = "hazWaste";
			ejectHaz.unit = "kg";
			ejectHaz.quantity = 4.0;
			ejectHaz.firstOccurrenceSeconds = 0.5;
			settings.wasteEjections = { ejectNorm, ejectHaz };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				("waste segregation setup refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(60), "waste segregation run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.wasteEjections.size() == 2, "ejection contracts missing");
			for (auto const& ejection : snap.wasteEjections)
				require(approx(ejection.ejectedTotal, 4.0) && ejection.unmetTotal == 0.0,
					("ejection '" + ejection.id + "' did not remove its scheduled quantity").c_str());
			auto const normal = findStore(snap, "normWaste");
			auto const hazardous = findStore(snap, "hazWaste");
			require(approx(normal.currentQualified, 6.0) && approx(normal.currentUnqualified, 5.0),
				"the normal ejection touched the wrong pool or the wrong store");
			require(approx(hazardous.currentQualified, 6.0)
				&& approx(hazardous.currentUnqualified, 5.0),
				"the hazardous ejection touched the wrong pool or the wrong store");
		}
		// The bundled Lua mirror checks the same energy-evidence and mass ledgers.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"local t = self.water_treatments[1]\n"
					"assert(not t.qualityAccepted or t.treatmentEvidenced, 'sandbox qualified without treatment evidence')\n"
					"assert(not t.treatmentEvidenced or t.filtrationCompleted, 'sandbox evidence without filtration')\n"
					"assert(math.abs(t.consumablesUsedTotal - t.processedTotal * 0.01) < 1e-9, 'sandbox spent extra process media')\n"
					"assert(math.abs(t.acceptedTotal - t.processedTotal * 0.8) < 1e-9, 'sandbox full dose did not qualify all product')\n"
					"assert(t.acceptedTotal + t.heldTotal >= t.treatedTotal - 1e-9, 'sandbox quality ledger open')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("ContainmentSandbox", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			require(hazConfigure(sandbox, hazStores(),
				{ hazTreatment("plant") },
				{ }, 20.0).first, "sandbox containment setup refused");
			sandbox.resumeSimulation();
			require(sandbox.advanceTicks(120),
				"Bundled Lua treatment model failed its in-sandbox assertions");
			auto const snap = sandbox.getStationSystemsSnapshot();
			require(snap.healthy && snap.waterTreatments[0].heldKg == 0.0
				&& approx(snap.waterTreatments[0].acceptedKg, snap.waterTreatments[0].treatedKg),
				"host and sandbox treatment ledgers disagree for the same scenario");
			core::setStationSystemsSourceLoader({});
		}
		// Deterministic replay: the same authored scenario reset and run
		// again produces the same qualified/held split and store state.
		{
			core::World world("ContainmentReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(hazConfigure(world, hazStores(),
				{ hazTreatment("plant") },
				{ }, 20.0).first, "replay containment setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(90), "replay containment run failed");
			auto const first = world.getStationSystemsSnapshot();
			world.pauseSimulation();
			world.resetSimulation();
			world.resumeSimulation();
			require(world.advanceTicks(90), "replay containment rerun failed");
			auto const second = world.getStationSystemsSnapshot();
			require(first.waterTreatments == second.waterTreatments,
				"the treatment ledgers did not replay identically after reset");
			require(first.stores == second.stores,
				"the store state did not replay identically after reset");
			require(first.waterTreatments[0].acceptedKg > 0.0,
				"the replay scenario did not exercise qualified output");
		}
	}
	// Oxygen production and CO2 capture (#404): the electrolyzer's split
	// closes on mass and on energy, the cooling gate and the energy-
	// feasibility cap bound production instead of gifting products, a
	// full product store caps rather than vents, and captured CO2 lands as
	// inventory in a finite store with the unmet atmospheric service left
	// visible.
	void gas(smoke::Context const&)
	{
		auto const dt = static_cast<double>(core::World::getFixedTimestep());
		auto approx = [](double actual, double expected)
		{
			auto const tolerance = 1e-6 * std::max(1.0, std::abs(expected));
			return std::abs(actual - expected) <= tolerance;
		};
		auto const o2Ratio = core::StationElectrolysisOxygenMassPerWaterKg;
		auto const chemPerKgWater = core::StationElectrolysisChemicalEnergyKJPerKgWater;
		auto const h2ChemPerKg = core::StationElectrolysisChemicalEnergyHydrogenKJPerKg;
		auto plainStore = [](std::string const& id, double capacity, double starting,
			double protectedReserve = 0.0)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.capacity = capacity;
			settings.startingQualified = starting;
			settings.protectedReserve = protectedReserve;
			return settings;
		};
		auto compartment = [](double startingCo2Kg)
		{
			core::AtmosphereSettings settings;
			settings.id = "habitat";
			settings.volumeM3 = 100.0;
			settings.startingOxygenKg = 20.0;
			settings.startingNitrogenKg = 80.0;
			settings.startingCarbonDioxideKg = startingCo2Kg;
			settings.startingTemperatureC = 25.0;
			settings.thermalCapacityKJPerK = 10000.0;
			settings.minPressureKPa = 1.0;
			settings.maxPressureKPa = 200.0;
			return settings;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = 10000.0;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 200.0;
			return settings;
		};
		// 0.001 kg/s split at 4.5 kWh/kg: 16.2 kW drawn, oxygen at the
		// declared stoichiometric share, hydrogen as the remainder, and the
		// difference between the dose and the retained chemical energy is
		// process heat into the sink.
		auto electrolyzer = [](std::string const& id, std::string const& hazard)
		{
			core::ElectrolyzerSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.waterStoreId = "water";
			settings.oxygenStoreId = "o2";
			settings.hydrogenStoreId = "h2";
			settings.thermalClass = "low";
			settings.heatSinkId = "sink";
			settings.rateKgPerS = 0.001;
			settings.specificEnergyKWhPerKgWater = 4.5;
			settings.maxSinkTemperatureC = 60.0;
			return settings;
		};
		auto capture = [](std::string const& id, std::string const& hazard)
		{
			core::Co2CaptureSettings settings;
			settings.id = id;
			settings.hazardClass = hazard;
			settings.atmosphereId = "habitat";
			settings.captureStoreId = "captured";
			settings.consumableStoreId = "scrubberMedia";
			settings.maxRateKgPerS = 0.001;
			settings.specificEnergyKWhPerKg = 0.5;
			settings.consumablePerKg = 0.01;
			return settings;
		};
		auto gasStores = [&]()
		{
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("water", 100.0, 50.0, 10.0));
			stores.push_back(plainStore("o2", 100.0, 0.0));
			stores.push_back(plainStore("h2", 100.0, 0.0));
			stores.push_back(plainStore("captured", 100.0, 0.0));
			stores.push_back(plainStore("scrubberMedia", 100.0, 10.0));
			return stores;
		};
		auto findStore = [](core::StationSystemsSnapshot const& snap, std::string const& id)
		{
			for (auto const& entry : snap.stores)
				if (entry.id == id) return entry;
			return core::StationSystemsStoreSnapshot{};
		};
		auto gasConfigure = [&](core::World& world,
			std::vector<core::ResourceStoreSettings> stores,
			std::vector<core::ElectrolyzerSettings> cells,
			std::vector<core::Co2CaptureSettings> captures,
			double solarKW,
			double sinkStartingC = 10.0)
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = std::move(stores);
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", sinkStartingC) };
			settings.electrolyzers = std::move(cells);
			settings.co2Captures = std::move(captures);
			if (solarKW > 0.0)
			{
				core::SolarArraySettings field;
				field.id = "field";
				field.capacityKW = solarKW;
				settings.solarArrays = { field };
			}
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed oxygen-production and CO2
		// capture contract is rejected before anything runs, and none of the
		// refusals mutates the World.
		{
			core::World bad("GasBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			std::string diagnostic;
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].waterStoreId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("feed water store 'ghost' does not exist") != std::string::npos,
				"an electrolyzer fed by an unknown store was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].atmosphereId = "habitat";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exactly one oxygen destination") != std::string::npos,
				"an electrolyzer configured with both a store and a compartment was accepted");
			settings.electrolyzers[0].oxygenStoreId = "";
			settings.electrolyzers[0].atmosphereId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exactly one oxygen destination") != std::string::npos,
				"an electrolyzer with no oxygen destination was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].specificEnergyKWhPerKgWater = 3.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("below the chemical energy") != std::string::npos,
				"a dose below the retained chemical energy was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].thermalClass = "moderate";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("does not match heat sink") != std::string::npos,
				"a thermal class mismatch against the heat sink was accepted");
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			settings.electrolyzers[0].maxSinkTemperatureC = -300.0;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a maximum sink temperature below absolute zero was accepted");
			settings.electrolyzers = {};
			settings.co2Captures = { capture("scrub", "normal") };
			settings.co2Captures[0].atmosphereId = "ghost";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown atmosphere compartment") != std::string::npos,
				"a capture machine scrubbing an unknown compartment was accepted");
			settings.co2Captures = { capture("scrub", "normal") };
			settings.co2Captures[0].consumableStoreId = "";
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("names no consumable store") != std::string::npos,
				"a consumable demand with no consumable store was accepted");
			// A gas store shared between a normal and a hazardous process
			// is accepted (#751): the class no longer reserves a store for
			// one process, so both machines wire to it and the shared
			// inventory is the consequence.
			settings.co2Captures = { capture("scrub", "normal") };
			settings.electrolyzers = { electrolyzer("hcell", "hazardous") };
			settings.electrolyzers[0].hydrogenStoreId = "captured";
			require(core::validateStationSystemsSettings(settings, diagnostic),
				"a shared gas store across hazard classes was refused");
			std::vector<core::ElectrolyzerSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationElectrolyzers; ++index)
				tooMany.push_back(electrolyzer("cell" + std::to_string(index), "normal"));
			settings = core::StationSystemsSettings{};
			settings.enabled = true;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.electrolyzers = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared electrolyzer budget was accepted");
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused gas configuration mutated the World");
		}
		// Aliased product stores (#585) through the public World
		// configuration/tick/snapshot seam: a cell that names one store for
		// both oxygen and hydrogen used to be accepted, and the first tick
		// then sized each product against the same headroom, added both, and
		// broke the finite-capacity audit - a terminal failure that paused
		// the World with the tick at zero. The alias is refused at
		// configuration instead, and the World never reaches the tick that
		// could overfill the store.
		{
			core::World world("GasAliasedProducts", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			std::vector<core::ResourceStoreSettings> stores;
			stores.push_back(plainStore("water", 1.0, 1.0));
			stores.push_back(plainStore("gas", 0.001, 0.0));
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = stores;
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.electrolyzers = { electrolyzer("split", "normal") };
			settings.electrolyzers[0].oxygenStoreId = "gas";
			settings.electrolyzers[0].hydrogenStoreId = "gas";
			settings.electrolyzers[0].rateKgPerS = 1.0;
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 20000.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("both the oxygen and the hydrogen product") != std::string::npos,
				"an electrolyzer aliasing its oxygen and hydrogen product stores was accepted");
			require(!world.getStationSystemsSnapshot().enabled
				&& world.getSimulationTick() == 0 && world.getStationSystemsDiagnostics().empty(),
				"the refused aliased-product configuration mutated the World");
			// The same alias through the feed side: one store named as both
			// the water drawn and a gas produced is refused too.
			settings.electrolyzers = { electrolyzer("split", "normal") };
			settings.electrolyzers[0].waterStoreId = "h2";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("feed water and a gas product") != std::string::npos,
				"an electrolyzer aliasing its feed water store with its hydrogen product store was accepted");
			settings.electrolyzers = { electrolyzer("split", "normal") };
			settings.electrolyzers[0].waterStoreId = "o2";
			require(!world.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("feed water and a gas product") != std::string::npos,
				"an electrolyzer aliasing its feed water store with its oxygen product store was accepted");
			// A cell that delivers oxygen straight into a compartment has no
			// oxygen store to alias, so the distinct-store rule does not
			// refuse it; the tick it then runs must stay clean.
			settings.electrolyzers = { electrolyzer("split", "normal") };
			settings.electrolyzers[0].oxygenStoreId = "";
			settings.electrolyzers[0].atmosphereId = "habitat";
			settings.electrolyzers[0].hydrogenStoreId = "gas";
			settings.electrolyzers[0].rateKgPerS = 0.0001;
			require(world.configureStationSystems(settings, false, &diagnostic),
				("a compartment-delivering cell with a distinct hydrogen store was refused: " + diagnostic).c_str());
			world.resumeSimulation();
			require(world.advanceTicks(5), "the compartment-delivering cell run failed");
			require(world.getStationSystemsDiagnostics().empty(),
				"the compartment-delivering cell raised a runtime diagnostic");
			require(approx(findStore(world.getStationSystemsSnapshot(), "gas").currentQualified,
				0.0001 * (1.0 - o2Ratio) * 5.0 * dt)
					&& findStore(world.getStationSystemsSnapshot(), "gas").currentQualified < 0.001,
				"the distinct hydrogen store did not fill as expected or breached its capacity");
		}
		// Nominal electrolysis to stores: the split closes on mass, the
		// energy closes over supplied = chemical + heat, the protected
		// reserve is never touched, and the sink carries the process heat.
		{
			core::World world("GasNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 100.0).first,
				"nominal electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const split = 0.001 * elapsed;
			auto const o2 = split * o2Ratio;
			auto const h2 = split - o2;
			auto const energy = 4.5 * 3600.0 * split;
			auto const chemical = h2 * h2ChemPerKg;
			auto const heat = energy - chemical;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.electrolyzers.size() == 1, "electrolyzer missing from the snapshot");
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Full
				&& approx(cell.scale, 1.0) && cell.coolingAvailable,
				"the electrolyzer is not running at full rate with a qualified sink");
			require(approx(cell.waterSplitKg, split) && approx(cell.oxygenProducedKg, o2)
				&& approx(cell.hydrogenProducedKg, h2),
				"the split did not follow the declared stoichiometry");
			require(std::abs(cell.residualKg) < 1e-6,
				"the water/oxygen/hydrogen ledger does not close");
			require(approx(cell.energySuppliedKJ, energy) && approx(cell.chemicalEnergyKJ, chemical)
				&& approx(cell.heatRoutedKJ, heat) && std::abs(cell.residualKJ) < 1e-6,
				"the supplied/chemical/heat energy ledger does not close");
			require(chemical <= cell.energySuppliedKJ + 1e-6,
				"more chemical energy was booked than the dose supplied");
			require(approx(findStore(snap, "water").currentQualified, 50.0 - split)
				&& findStore(snap, "water").currentQualified > 10.0,
				"the feed water store lost the wrong amount or ate into its protected reserve");
			require(approx(findStore(snap, "o2").currentQualified, o2)
				&& approx(findStore(snap, "h2").currentQualified, h2),
				"the product stores did not receive the split");
			require(approx(snap.power.electrolyzerPowerKW, 4.5 * 3600.0 * 0.001),
				"the power snapshot does not report the electrolyzer draw");
			require(approx(snap.thermal.reservoirs[0].electrolysisEnergyKJ, heat)
				&& approx(snap.thermal.reservoirs[0].lastElectrolysisHeatKW,
					heat / elapsed),
				"the process heat did not land in the sink reservoir");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit + "' not closed").c_str());
		}
		// Cooling gate: a sink at or above the maximum sink temperature
		// refuses the cell whole - no split, no power draw, and the reason
		// is stated.
		{
			core::World world("GasHotSink", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {},
				100.0, 70.0).first, "hot-sink electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "hot-sink electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& !cell.coolingAvailable && cell.waterSplitKg == 0.0
				&& cell.lastPowerSuppliedKW == 0.0
				&& cell.blockReason.find("cooling is not qualified") != std::string::npos,
				"a hot sink did not refuse the electrolyzer whole");
			require(approx(findStore(snap, "water").currentQualified, 50.0),
				"the refused electrolyzer still drew feed water");
			bool announced = false;
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find("Electrolyzer 'cell' rejected: cooling") != std::string::npos)
					announced = true;
			require(announced, "the cooling refusal was not announced in the event log");
		}
		// No power: a tick that demands electricity and receives none is
		// refused whole - nothing splits and nothing is gifted.
		{
			core::World world("GasNoPower", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 0.0).first,
				"no-power electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "no-power electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(cell.status == core::StationTransferStatus::Rejected
				&& cell.waterSplitKg == 0.0 && cell.oxygenProducedKg == 0.0
				&& cell.lastPowerDemandKW > 0.0 && cell.lastPowerSuppliedKW == 0.0
				&& cell.blockReason.find("no electrical supply") != std::string::npos,
				"an unpowered electrolyzer was not refused whole");
		}
		// Energy feasibility: curtailed power caps the split at what the
		// supplied energy can actually make; the shortfall stays visible as
		// unmet production and the chemical energy never exceeds the dose.
		{
			core::World world("GasCapped", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			// The array covers only 1 kW of the cell's 16.2 kW demand.
			require(gasConfigure(world, gasStores(), { electrolyzer("cell", "normal") }, {}, 1.0).first,
				"capped electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "capped electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			auto const feasible = 1.0 * elapsed / chemPerKgWater;
			require(cell.status == core::StationTransferStatus::Partial,
				"an energy-capped electrolyzer is not reported partial");
			require(approx(cell.waterSplitKg, feasible)
				&& cell.waterSplitKg < 0.001 * elapsed,
				"the split was not capped at the energy-feasible amount");
			require(approx(cell.chemicalEnergyKJ, cell.waterSplitKg * chemPerKgWater)
				&& cell.chemicalEnergyKJ <= cell.energySuppliedKJ + 1e-6,
				"the capped split booked more chemical energy than was supplied");
			require(cell.lastUnmetProductionKg > 0.0
				&& approx(cell.lastUnmetProductionKg + cell.lastSplitKg, cell.lastRequestedKg),
				"the unmet production is not the explicit remainder of the rated request");
		}
		// Product headroom: a full hydrogen store caps production instead
		// of venting; the limiter names the store that filled.
		{
			core::World world("GasFullH2", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = gasStores();
			for (auto& store : stores)
				if (store.id == "h2") store.capacity = 0.00005;
			require(gasConfigure(world, stores, { electrolyzer("cell", "normal") }, {}, 100.0).first,
				"full-h2 electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "full-h2 electrolysis run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& cell = snap.electrolyzers[0];
			require(approx(findStore(snap, "h2").currentQualified, 0.00005)
				&& findStore(snap, "h2").currentQualified <= 0.00005 + 1e-9,
				"the hydrogen store went over capacity instead of capping production");
			require(cell.hydrogenProducedKg <= 0.00005 + 1e-9
				&& cell.waterSplitKg < 0.001 * 60.0 * dt,
				"production was not capped by the full hydrogen store");
			require(cell.limiter.find("hydrogen store") != std::string::npos
				|| cell.status == core::StationTransferStatus::Rejected,
				"the product-headroom limiter is not visible");
		}
		// Oxygen delivered straight into a compartment: the compartment's
		// own oxygen closure accounts for every gram the cell made, and the
		// store boundary still closes with the delivered mass netted out of
		// the feed-side loss.
		{
			core::World world("GasToAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto cell = electrolyzer("cell", "normal");
			cell.oxygenStoreId = "";
			cell.atmosphereId = "habitat";
			require(gasConfigure(world, gasStores(), { cell }, {}, 100.0).first,
				"compartment-delivery electrolysis setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "compartment-delivery electrolysis run failed");
			auto const elapsed = 60.0 * dt;
			auto const split = 0.001 * elapsed;
			auto const o2 = split * o2Ratio;
			auto const snap = world.getStationSystemsSnapshot();
			auto const& habitat = snap.atmospheres[0];
			require(approx(habitat.electrolysisOxygenKg, o2)
				&& approx(habitat.oxygenKg, 20.0 + o2),
				"the compartment did not book the delivered oxygen");
			require(approx(snap.electrolyzers[0].oxygenProducedKg, o2),
				"the cell did not report the oxygen it delivered to the air");
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit
						+ "' not closed with compartment delivery").c_str());
		}
		// Nominal CO2 capture: the compartment's carbon leaves through a
		// booked pathway and occupies the capture store as inventory; the
		// consumable and the electricity are drawn for it.
		{
			core::World world("GasCapture", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			require(gasConfigure(world, gasStores(), {}, { capture("scrub", "normal") }, 100.0).first,
				"nominal capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(60), "nominal capture run failed");
			auto const elapsed = 60.0 * dt;
			auto const captured = 0.001 * elapsed;
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.co2Captures.size() == 1, "capture machine missing from the snapshot");
			auto const& scrub = snap.co2Captures[0];
			require(scrub.status == core::StationTransferStatus::Full
				&& approx(scrub.capturedKg, captured),
				"the capture machine is not running at its rated rate");
			require(approx(snap.atmospheres[0].carbonDioxideKg, 1.0 - captured)
				&& approx(snap.atmospheres[0].co2CapturedKg, captured),
				"the compartment carbon dioxide did not fall by exactly what was captured");
			require(approx(findStore(snap, "captured").currentQualified, captured),
				"the captured gas did not land in the capture store");
			require(approx(scrub.consumablesUsed, captured * 0.01)
				&& approx(findStore(snap, "scrubberMedia").currentQualified, 10.0 - captured * 0.01),
				"the regeneration consumable was not drawn with the capture");
			// 0.001 kg/s at 0.5 kWh/kg is 1.8 kW in pool units
			// (rate x dose x 3600 s/h, #567).
			require(approx(snap.power.co2CapturePowerKW, 0.5 * 3600.0 * 0.001),
				"the power snapshot does not report the capture draw");
			// The captured gas entered the store network from outside it:
			// the boundary ledger carries it as collected inflow and closes.
			for (auto const& balance : snap.balances)
				require(std::abs(balance.residual) <= 1e-6,
					("per-unit boundary residual for '" + balance.unit
						+ "' not closed with captured inflow").c_str());
			bool collectedBooked = false;
			for (auto const& balance : snap.balances)
				if (balance.unit == "kg" && approx(balance.collected, captured)) collectedBooked = true;
			require(collectedBooked, "the captured gas is missing from the collected boundary ledger");
		}
		// Finite service: the machine can only scrub the CO2 the
		// compartment holds. Once the air is clean the remainder is the
		// whole rated request and the machine is refused with a stated
		// reason.
		{
			core::World world("GasCleanAir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(0.0002) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.co2Captures = { capture("scrub", "normal") };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			std::string diagnostic;
			require(world.configureStationSystems(settings, false, &diagnostic),
				"clean-air capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "clean-air capture run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& scrub = snap.co2Captures[0];
			require(approx(scrub.capturedKg, 0.0002)
				&& snap.atmospheres[0].carbonDioxideKg < 1e-9,
				"the capture machine did not stop at the CO2 the compartment held");
			require(scrub.status == core::StationTransferStatus::Rejected
				&& (scrub.limiter.find("holds no carbon dioxide") != std::string::npos
					|| scrub.blockReason.find("holds no carbon dioxide") != std::string::npos),
				"the exhausted-compartment refusal is not stated");
			require(scrub.capturedKg <= 0.0002 + 1e-9,
				"the capture machine invented carbon dioxide");
		}
		// Consumable exhaustion: the capture scale is capped by the media
		// the store can actually draw, and the total captured never exceeds
		// what the consumable can regenerate.
		{
			core::World world("GasNoMedia", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto stores = gasStores();
			for (auto& store : stores)
				if (store.id == "scrubberMedia") store.startingQualified = 5e-6;
			require(gasConfigure(world, stores, {}, { capture("scrub", "normal") }, 100.0).first,
				"no-media capture setup refused");
			world.resumeSimulation();
			require(world.advanceTicks(120), "no-media capture run failed");
			auto const snap = world.getStationSystemsSnapshot();
			auto const& scrub = snap.co2Captures[0];
			require(scrub.capturedKg <= 5e-4 + 1e-9,
				"capture continued past the consumable the store could sustain");
			require(scrub.consumablesUsed <= 5e-6 + 1e-9
				&& findStore(snap, "scrubberMedia").currentQualified < 1e-9,
				"the consumable store was overdrawn");
			require(scrub.capturedKg > 0.0, "the capture machine captured nothing at all");
		}
		// A sandbox mirror that finds a broken electrolysis split on its own
		// numbers is contained: the tick fails, the runtime is disabled and
		// no partial result escapes.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(electrolyzers) == 'table' and #electrolyzers == 1, 'electrolyzer descriptor crossing lost')\n"
					"assert(electrolyzers[1].rateKgPerS > 0, 'corrupt electrolyzer rate crossing')\n"
					"error('corrupt electrolyzer descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ElectrolyzerCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.stores = gasStores();
			settings.atmospheres = { compartment(1.0) };
			settings.thermalReservoirs = { reservoir("sink", "low", 10.0) };
			settings.electrolyzers = { electrolyzer("cell", "normal") };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			settings.solarArrays = { field };
			auto const result = corrupt.configureStationSystems(settings, false, &diagnostic);
			require(!result && !corrupt.getStationSystemsSnapshot().enabled,
				"corrupt electrolyzer sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
}

void registerStationAtmosphereChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"atmospheres", atmospheres});
	checks.push_back({"containment", containment});
	checks.push_back({"gas", gas});
}
