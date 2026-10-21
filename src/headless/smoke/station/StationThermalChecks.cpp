#include "StationThermalChecks.h"
#include "StationRig.h"
#include <limits>

namespace
{
	using namespace smoke::station;
	// #395: thermal reservoirs accumulate heat and radiators reject it.
	// Temperature is integrated over each Station step from the authored
	// capacity, starting temperature and absorbed heat; radiator rejection
	// follows the Stefan-Boltzmann law at the emitting temperature with
	// the authored sink, emissivity and view factor. Equipment heat routes
	// exactly once into a class-compatible reservoir or exports directly,
	// never both. Limits are reported, never clamped; the energy ledger
	// closes within the declared tolerance in host and sandbox alike.
	void thermal(smoke::Context const&)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, std::string const& thermalClass,
			double capacityKJPerK, double startingC, double minC, double maxC, double absorbedKW)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = thermalClass;
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = minC;
			settings.maxTemperatureC = maxC;
			settings.absorbedHeatKW = absorbedKW;
			return settings;
		};
		auto radiator = [](std::string const& id, std::string const& reservoirId,
			double areaM2, double emissivity, double viewFactor, double sinkC)
		{
			core::RadiatorSettings settings;
			settings.id = id;
			settings.reservoirId = reservoirId;
			settings.areaM2 = areaM2;
			settings.emissivity = emissivity;
			settings.viewFactor = viewFactor;
			settings.sinkTemperatureC = sinkC;
			return settings;
		};
		auto constantProfile = [](double level)
		{
			core::ProfileSettings profile;
			profile.constantLevel = level;
			return profile;
		};
		auto thermalConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::RadiatorSettings> radiators,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::EquipmentLoadSettings> loads = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.radiators = std::move(radiators);
			settings.solarArrays = std::move(solar);
			settings.equipmentLoads = std::move(loads);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed reservoir, radiator or
		// heat-sink contract is rejected before anything runs.
		{
			core::World bad("ThermalBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 0.0, 20.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("capacity must be finite and positive") != std::string::npos,
				"Zero thermal capacity accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 30.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("min below max") != std::string::npos,
				"Inverted temperature limits accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 45.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("outside its declared limits") != std::string::npos,
				"Starting temperature outside limits accepted");
			settings.thermalReservoirs = { reservoir("cabin", "cryogenic", 100.0, 20.0, 10.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal service class") != std::string::npos,
				"Unknown reservoir thermal class accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, -300.0, -310.0, 30.0, 0.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"Sub-absolute-zero temperature accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 30.0, -1.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("absorbed heat must be finite and non-negative") != std::string::npos,
				"Negative absorbed heat accepted");
			settings.thermalReservoirs = { reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 30.0, 0.0) };
			settings.radiators = { radiator("panel", "ghost", 100.0, 0.9, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal reservoir 'ghost'") != std::string::npos,
				"Radiator on unknown reservoir accepted");
			settings.radiators = { radiator("panel", "cabin", 0.0, 0.9, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("area must be finite and positive") != std::string::npos,
				"Zero radiator area accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.0, 1.0, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("emissivity") != std::string::npos,
				"Zero emissivity accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.9, 1.5, -270.0) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("view factor") != std::string::npos,
				"View factor above one accepted");
			settings.radiators = { radiator("panel", "cabin", 100.0, 0.9, 1.0, -273.15) };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"Sink at absolute zero accepted");
			// Heat-sink contracts on loads: unknown sink, incompatible class
			// and a sink on a cooling-service load are all refused.
			core::EquipmentLoadSettings sinkless;
			sinkless.id = "shop";
			sinkless.electricalDemandKW = 5.0;
			sinkless.thermalClass = "moderate";
			sinkless.heatOutputKW = 2.0;
			sinkless.heatSinkId = "nowhere";
			settings.radiators = {};
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("unknown thermal reservoir 'nowhere'") != std::string::npos,
				"Load heat sink referencing unknown reservoir accepted");
			sinkless.heatSinkId = "cabin";
			sinkless.thermalClass = "warm";
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("incompatible") != std::string::npos,
				"Cross-class heat routing accepted");
			sinkless.thermalClass = "moderate";
			sinkless.coolingDisposition = core::StationCoolingDisposition::CoolingService;
			settings.equipmentLoads = { sinkless };
			require(!bad.configureStationSystems(settings, false, &diagnostic)
				&& diagnostic.find("not a heat sink") != std::string::npos,
				"Heat sink on a cooling-service load accepted");
			require(!bad.getStationSystemsSnapshot().enabled,
				"Refused thermal configuration constructed a runtime");
		}
		// Analytic heating: with no radiator and a constant absorbed heat,
		// temperature rises exactly linearly in World time, and the energy
		// ledger closes over the absorbed term.
		{
			core::World world("ThermalHeat", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 2.0) }, {});
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 60.0), "Analytic heating run failed");
			auto const snap = world.getStationSystemsSnapshot();

			require(snap.thermal.reservoirs.size() == 1
				&& approx(snap.thermal.reservoirs[0].temperatureC, 20.0 + 2.0 * snap.elapsedSeconds / 100.0, 1e-6)
				&& approx(snap.thermal.reservoirs[0].lastRejectedKW, 0.0)
				&& approx(snap.thermal.reservoirs[0].absorbedEnergyKJ, 2.0 * snap.elapsedSeconds, 1e-4)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Analytic linear heating or ledger closure failed");
			// Reset reconstructs the authored starting temperature.
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& world.getStationSystemsSnapshot().thermal.reservoirs[0].absorbedEnergyKJ == 0.0,
				"Reset did not reconstruct the authored thermal state");
		}
		// Analytic single-tick radiator rejection: the rejection law is
		// evaluated at the emitting temperature with the authored sink,
		// emissivity and view factor, and one tick of integration matches
		// the closed-form step exactly.
		{
			core::World world("ThermalTick", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("panel-host", "warm", 50.0, 20.0, -50.0, 200.0, 0.0) },
				{ radiator("panel", "panel-host", 1000.0, 0.9, 0.8, -270.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(advanceStationSteps(world, 1), "Single-step thermal run failed");
			auto const temperatureK = 20.0 + core::StationCelsiusOffset;
			auto const sinkK = -270.0 + core::StationCelsiusOffset;
			auto const expectedRejection = 0.9 * 0.8 * 1000.0
				* core::StationStefanBoltzmannKWPerM2K4
				* (temperatureK * temperatureK * temperatureK * temperatureK
					- sinkK * sinkK * sinkK * sinkK);
			auto const snap = world.getStationSystemsSnapshot();
			auto const expectedTemperature = 20.0 - expectedRejection * snap.systemsTimestepSeconds / 50.0;
			require(approx(snap.thermal.radiators[0].lastRejectedKW, expectedRejection, 1e-12)
				&& approx(snap.thermal.reservoirs[0].temperatureC, expectedTemperature, 1e-9)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Radiator rejection law or one-tick integration mismatch");
		}
		// Cooling toward the sink: an unpowered reservoir with a radiator
		// cools monotonically, never crosses its coldest sink, and the
		// rejection shrinks as the emitting temperature falls.
		{
			core::World world("ThermalCool", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("hot", "warm", 100.0, 27.0, -50.0, 100.0, 0.0) },
				{ radiator("big", "hot", 20000.0, 1.0, 1.0, -270.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			double previousTemperature = 27.0;
			double previousRejection = std::numeric_limits<double>::max();
			for (int chunk = 0; chunk < 12; ++chunk)
			{
				require(smoke::station::advanceSeconds(world, 5.0), "Cooling run failed");
				auto const snap = world.getStationSystemsSnapshot();
				auto const temperature = snap.thermal.reservoirs[0].temperatureC;
				auto const rejection = snap.thermal.reservoirs[0].lastRejectedKW;
				require(temperature < previousTemperature,
					"Reservoir did not cool toward its sink");
				require(temperature > -270.0,
					"Reservoir crossed below its sink temperature");
				require(rejection < previousRejection,
					"Rejection did not fall with the emitting temperature");
				require(approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
					"Cooling ledger failed to close");
				previousTemperature = temperature;
				previousRejection = rejection;
			}
		}
		// Heat routing: an admitted load with a declared heat sink sends
		// its emitted heat into the reservoir exactly once - nothing is
		// also booked as direct export - and the reservoir warms by
		// exactly the routed energy. A load without a sink keeps the
		// direct-export behaviour untouched.
		{
			core::World world("ThermalRoute", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::EquipmentLoadSettings routed;
			routed.id = "shop";
			routed.electricalDemandKW = 5.0;
			routed.thermalClass = "moderate";
			routed.heatOutputKW = 2.0;
			routed.heatSinkId = "cabin";
			core::EquipmentLoadSettings direct;
			direct.id = "beacon";
			direct.electricalDemandKW = 1.0;
			direct.thermalClass = "low";
			direct.heatOutputKW = 1.0;
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			auto const [ok, diagnostic] = thermalConfigure(world,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {},
				{ field }, { routed, direct });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 60.0), "Routing run failed");
			auto const snap = world.getStationSystemsSnapshot();
			require(approx(snap.power.loads[0].heatRoutedKW, 2.0)
				&& approx(snap.power.loads[0].heatExportedKW, 0.0)
				&& approx(snap.power.loads[1].heatExportedKW, 1.0)
				&& approx(snap.power.loads[1].heatRoutedKW, 0.0),
				"Emitted heat was not routed exactly once");
			require(approx(snap.power.exportedHeatKW, 1.0),
				"Station direct-export total double-counted routed heat");
			require(approx(snap.thermal.totalRoutedKW, 2.0)
				&& approx(snap.thermal.reservoirs[0].temperatureC, 20.0 + 2.0 * snap.elapsedSeconds / 100.0, 1e-6)
				&& approx(snap.thermal.reservoirs[0].routedEnergyKJ, 2.0 * snap.elapsedSeconds, 1e-4)
				&& approx(snap.thermal.reservoirs[0].residualKJ, 0.0, 1e-6),
				"Routed heat did not warm the reservoir by exactly its energy");
		}
		// Hot and cold operating profiles: the same reservoir under a
		// full-duty load warms; under a zero-duty load it stays flat.
		{
			core::World hot("ThermalHot", 2, 2), cold("ThermalCold", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			cold.pauseSimulation(); cold.finishBuild();
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			for (bool isHot : { true, false })
			{
				auto& world = isHot ? hot : cold;
				core::EquipmentLoadSettings shop;
				shop.id = "shop";
				shop.electricalDemandKW = 5.0;
				shop.thermalClass = "moderate";
				shop.heatOutputKW = 3.0;
				shop.heatSinkId = "cabin";
				shop.operatingProfile = constantProfile(isHot ? 1.0 : 0.0);
				auto const [ok, diagnostic] = thermalConfigure(world,
					{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {},
					{ field }, { shop });
				require(ok, diagnostic.c_str());
			}
			hot.resumeSimulation(); cold.resumeSimulation();
			require(smoke::station::advanceSeconds(hot, 30.0) && smoke::station::advanceSeconds(cold, 30.0), "Profile run failed");
			auto const hotSnapshot = hot.getStationSystemsSnapshot();
			require(approx(hotSnapshot.thermal.reservoirs[0].temperatureC, 20.0 + 3.0 * hotSnapshot.elapsedSeconds / 100.0, 1e-6),
				"Hot profile did not warm the reservoir as declared");
			require(cold.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& cold.getStationSystemsSnapshot().thermal.reservoirs[0].lastHeatInKW == 0.0,
				"Cold profile produced heat it should not have");
		}
		// Equivalent update batching: the same thermal scenario integrated
		// tick-by-tick and through accelerated updates is identical - the
		// integration is World-time based, never wall-clock based.
		{
			core::World steady("ThermalSteady", 2, 2), batched("ThermalBatched", 2, 2);
			steady.pauseSimulation(); steady.finishBuild();
			batched.pauseSimulation(); batched.finishBuild();
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			for (auto* world : { &steady, &batched })
			{
				core::EquipmentLoadSettings shop;
				shop.id = "shop";
				shop.electricalDemandKW = 5.0;
				shop.thermalClass = "moderate";
				shop.heatOutputKW = 2.0;
				shop.heatSinkId = "cabin";
				auto const [ok, diagnostic] = thermalConfigure(*world,
					{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 1.0) },
					{ radiator("panel", "cabin", 5000.0, 0.9, 1.0, -270.0) },
					{ field }, { shop });
				require(ok, diagnostic.c_str());
			}
			steady.resumeSimulation();
			require(smoke::station::advanceSeconds(steady, 10.0), "Steady thermal run failed");
			batched.resumeSimulation();
			batched.setSimulationTimeScale(2.0f);
			// Stay inside World's wall-frame clamp while exercising catch-up.
			for (unsigned frame = 0; frame < 10; ++frame) batched.update(0.5f);
			require(batched.getSimulationTick() == steady.getSimulationTick(), "Batched update tick mismatch");
			require(steady.getStationSystemsSnapshot() == batched.getStationSystemsSnapshot(),
				"Thermal integration differs between tick-by-tick and batched updates");
		}
		// Declared limits are observable: an overheat and an overcool are
		// announced on their transitions and reported in the snapshot,
		// while the integrated state is never clamped.
		{
			core::World hot("LimitHot", 2, 2);
			hot.pauseSimulation(); hot.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(hot,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 10.0, 21.0, 1.0) }, {});
			require(ok, diagnostic.c_str());
			hot.resumeSimulation();
			require(smoke::station::advanceSeconds(hot, 150.0), "Overheat run failed");
			auto const snap = hot.getStationSystemsSnapshot();
			require(snap.thermal.reservoirs[0].aboveLimit
				&& !snap.thermal.reservoirs[0].belowLimit
				&& approx(snap.thermal.reservoirs[0].temperatureC, 20.0 + snap.elapsedSeconds / 100.0, 1e-6),
				"Overheat not reported or wrongly clamped to the limit");
			require(sawEvent(hot, "Thermal reservoir 'cabin' above its declared maximum"),
				"Overheat transition missing from the event log");
			core::World cold("LimitCold", 2, 2);
			cold.pauseSimulation(); cold.finishBuild();
			auto const [coldOk, coldDiagnostic] = thermalConfigure(cold,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 19.0, 30.0, 0.0) },
				{ radiator("big", "cabin", 20000.0, 1.0, 1.0, -270.0) });
			require(coldOk, coldDiagnostic.c_str());
			cold.resumeSimulation();
			require(smoke::station::advanceSeconds(cold, 100.0), "Overcool run failed");
			auto const coldSnap = cold.getStationSystemsSnapshot();
			require(coldSnap.thermal.reservoirs[0].belowLimit
				&& !coldSnap.thermal.reservoirs[0].aboveLimit,
				"Overcool not reported in the snapshot");
			require(sawEvent(cold, "Thermal reservoir 'cabin' below its declared minimum"),
				"Overcool transition missing from the event log");
		}
		// The bundled Lua model receives the reservoir, radiator and
		// heat-sink descriptors intact and mirrors the routing inside the
		// sandbox; its own conservation audit passes on every tick.
		{
			auto const bundled = core::bundledStationSystemsSource();
			core::setStationSystemsSourceLoader([bundled] {
				return "local factory = (function()\n" + bundled + "\nend)()\n"
					"return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(#reservoirs == 1 and reservoirs[1].id == 'cabin'\n"
					"\tand reservoirs[1].heatCapacityKJPerK == 100 and reservoirs[1].absorbedHeatKW == 0)\n"
					"assert(#radiators == 1 and radiators[1].reservoir == 'cabin'\n"
					"\tand radiators[1].areaM2 == 5000)\n"
					"assert(#loads == 1 and loads[1].heatSinkId == 'cabin')\n"
					"local model = factory(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(model.reservoirs[1].temperature == 20)\n"
					"local original = model.advance\n"
					"function model.advance(self, tick, seconds)\n"
					"local result = original(self, tick, seconds)\n"
					"assert(math.abs(self.reservoirs[1].lastRouted - 2.0) < 1e-9, 'sandbox routed heat mismatch')\n"
					"assert(math.abs(self.loads[1].heatRouted - 2.0) < 1e-9 and self.loads[1].heatExported == 0,\n"
					"\t'sandbox heat was not routed exactly once')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandbox("ThermalSandbox", 2, 2);
			sandbox.pauseSimulation(); sandbox.finishBuild();
			core::EquipmentLoadSettings shop;
			shop.id = "shop";
			shop.electricalDemandKW = 5.0;
			shop.thermalClass = "moderate";
			shop.heatOutputKW = 2.0;
			shop.heatSinkId = "cabin";
			core::SolarArraySettings field;
			field.id = "field";
			field.capacityKW = 10.0;
			field.illumination = constantProfile(1.0);
			auto const [ok, diagnostic] = thermalConfigure(sandbox,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) },
				{ radiator("panel", "cabin", 5000.0, 0.9, 1.0, -270.0) },
				{ field }, { shop });
			require(ok, diagnostic.c_str());
			sandbox.resumeSimulation();
			require(smoke::station::advanceSeconds(sandbox, 2.0),
				"Bundled Lua thermal mirror failed its in-sandbox assertions");
			core::setStationSystemsSourceLoader({});
		}
		// A corrupt descriptor crossing fails contained: a sandbox that
		// cannot trust its thermal inputs stops the tick through the
		// integrated error path rather than publishing anything.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators)\n"
					"assert(type(reservoirs) == 'table' and #reservoirs == 1)\n"
					"assert(reservoirs[1].heatCapacityKJPerK > 0, 'corrupt thermal capacity crossing')\n"
					"error('corrupt thermal descriptor crossing')\n"
					"end";
			});
			core::World corrupt("ThermalCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = thermalConfigure(corrupt,
				{ reservoir("cabin", "moderate", 100.0, 20.0, 0.0, 100.0, 0.0) }, {});
			require(!ok && diagnostic.find("corrupt thermal descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt thermal sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
	// #396: coolant loops circulate a finite, loop-specific coolant and
	// provide freeze-protection heating, only while appropriate power and
	// coolant are available. The pump circulates only the coolant the
	// loop actually holds — an underfilled loop flows less, a dry loop
	// flows nothing — and the freeze heater thermostats the loop up to its
	// setpoint within its capacity. Pump work and heater heat enter the
	// loop exactly once and cross to the reservoir only through the
	// circulation-gated heat-only exchanger; the loop draw rides the same
	// electrical pool as external loads. Freeze exposure is reported,
	// never clamped, and every ledger closes.
	void coolant(smoke::Context const&)
	{
		auto const dt = requestedStepSeconds();
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto sawEvent = [](core::World& world, std::string const& needle)
		{
			for (auto const& event : world.getStationSystemsEvents())
				if (event.message.find(needle) != std::string::npos) return true;
			return false;
		};
		auto reservoir = [](std::string const& id, double capacityKJPerK, double startingC)
		{
			core::ThermalReservoirSettings settings;
			settings.id = id;
			settings.thermalClass = "moderate";
			settings.heatCapacityKJPerK = capacityKJPerK;
			settings.startingTemperatureC = startingC;
			settings.minTemperatureC = -50.0;
			settings.maxTemperatureC = 100.0;
			return settings;
		};
		auto loop = [](std::string const& id,
			std::string const& reservoirId, double capacityKg, double startingKg,
			double cp, double startingC, double setpointC, double pumpKW,
			double ratedFlow, double conductance, double heaterKW)
		{
			core::CoolantLoopSettings settings;
			settings.id = id;
			settings.reservoirId = reservoirId;
			settings.coolantCapacityKg = capacityKg;
			settings.startingCoolantKg = startingKg;
			settings.specificHeatKJPerKgK = cp;
			settings.startingTemperatureC = startingC;
			settings.freezeProtectionSetpointC = setpointC;
			settings.pumpPowerKW = pumpKW;
			settings.ratedFlowKgPerS = ratedFlow;
			settings.exchangerConductanceKWPerK = conductance;
			settings.heaterCapacityKW = heaterKW;
			return settings;
		};
		auto coolantConfigure = [](core::World& world,
			std::vector<core::ThermalReservoirSettings> reservoirs,
			std::vector<core::CoolantLoopSettings> loops,
			std::vector<core::SolarArraySettings> solar = {},
			std::vector<core::BatterySettings> batteries = {})
		{
			std::string diagnostic;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.thermalReservoirs = std::move(reservoirs);
			settings.coolantLoops = std::move(loops);
			settings.solarArrays = std::move(solar);
			settings.batteries = std::move(batteries);
			auto const ok = world.configureStationSystems(settings, false, &diagnostic);
			return std::make_pair(ok, diagnostic);
		};
		// Refused configurations: every malformed loop contract is
		// rejected before anything runs.
		{
			core::World bad("CoolantBad", 2, 2);
			bad.pauseSimulation(); bad.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.thermalReservoirs = { reservoir("bus", 1000.0, 20.0) };
			std::string diagnostic;
			settings.coolantLoops = { loop("l", "ghost", 100, 100, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("unknown thermal reservoir") != std::string::npos,
				"loop coupled to an unknown reservoir was accepted");
			settings.coolantLoops = { loop("l", "bus", 0, 0, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("capacity must be finite and positive") != std::string::npos,
				"zero coolant capacity was accepted");
			settings.coolantLoops = { loop("l", "bus", 100, 150, 4, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("exceeds the loop capacity") != std::string::npos,
				"starting coolant above capacity was accepted");
			settings.coolantLoops = { loop("l", "bus", 100, 100, 0, 20, 5, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("specific heat") != std::string::npos,
				"zero specific heat was accepted");
			settings.coolantLoops = { loop("l", "bus", 100, 100, 4, 20, -300, 1, 1, 1, 1) };
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("freeze-protection setpoint") != std::string::npos
				&& diagnostic.find("above absolute zero") != std::string::npos,
				"a sub-absolute-zero freeze setpoint was accepted");
			std::vector<core::CoolantLoopSettings> tooMany;
			for (size_t index = 0; index <= core::MaxStationCoolantLoops; ++index)
				tooMany.push_back(loop("loop" + std::to_string(index), "bus",
					100, 100, 4, 20, 5, 1, 1, 1, 1));
			settings.coolantLoops = tooMany;
			require(!core::validateStationSystemsSettings(settings, diagnostic)
				&& diagnostic.find("limited to") != std::string::npos,
				"more than the declared loop budget was accepted");
			// None of the refusals left a runtime or changed the World.
			require(!bad.getStationSystemsSnapshot().enabled && bad.getSimulationTick() == 0,
				"a refused coolant configuration mutated the World");
		}
		// Nominal full service: a full loop with ample power circulates at
		// its rated flow, and the pump work warms the loop and crosses
		// into the reservoir exactly once through the exchanger. The loop
		// and reservoir ledgers close, and the combined thermal gain
		// equals exactly the pump work energy — no duplicate booking.
		{
			core::World world("CoolantNominal", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 6.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "nominal coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(snapshot.coolantLoops.size() == 1, "loop snapshot missing");
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(loopSnap.status == core::StationTransferStatus::Full
				&& approx(loopSnap.pumpSuppliedKW, 1.2) && approx(loopSnap.pumpUnmetKW, 0.0)
				&& approx(loopSnap.circulationFraction, 1.0)
				&& approx(loopSnap.flowKgPerS, 3.0) && approx(loopSnap.unmetFlowKgPerS, 0.0)
				&& approx(loopSnap.fillFraction, 1.0) && !loopSnap.disabled,
				"full loop with ample power did not circulate at rated flow");
			require(loopSnap.heaterDemandKW == 0.0 && !loopSnap.belowFreezeSetpoint,
				"heater demanded on a warm loop");
			require(loopSnap.loopTemperatureC > 20.0, "pump work did not warm the loop");
			require(snapshot.thermal.reservoirs[0].temperatureC > 20.0,
				"exchanged pump work heat did not reach the reservoir");
			require(std::abs(loopSnap.residualKJ) < 1e-6
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"coolant or reservoir energy ledger did not close");
			// Exactly-once booking: the only energy that entered the
			// loop-plus-reservoir system this run is the pump work.
			auto const loopGain = loopSnap.currentEnergyKJ - loopSnap.startingEnergyKJ;
			auto const reservoirGain = snapshot.thermal.reservoirs[0].currentEnergyKJ
				- snapshot.thermal.reservoirs[0].startingEnergyKJ;
			require(approx(loopGain + reservoirGain, loopSnap.pumpWorkEnergyKJ, 1e-5),
				"combined thermal gain does not equal the pump work energy exactly once");
			require(approx(snapshot.thermal.totalCirculationKgPerS, 3.0),
				"station circulation total wrong");
			require(approx(snapshot.power.coolantPumpKW, 1.2)
				&& snapshot.power.coolantHeaterKW == 0.0,
				"coolant electrical service totals wrong");
			require(sawEvent(world, "circulating"), "circulation event not announced");
		}
		// Power-limited circulation: a starved pool reduces the actual
		// circulation fraction and leaves the remainder visible as unmet
		// flow — the loop never receives flow it was not powered for.
		{
			core::World world("CoolantPowerLimited", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 0.6;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 0.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "power-limited coolant run failed");
			auto const loopSnap = world.getStationSystemsSnapshot().coolantLoops[0];
			require(approx(loopSnap.pumpDemandKW, 1.2) && approx(loopSnap.pumpSuppliedKW, 0.6),
				"pump draw did not follow the starved pool");
			require(approx(loopSnap.circulationFraction, 0.5)
				&& approx(loopSnap.flowKgPerS, 1.5) && approx(loopSnap.unmetFlowKgPerS, 1.5),
				"electrical starvation did not scale circulation with unmet flow visible");
			require(loopSnap.status == core::StationTransferStatus::Partial,
				"partially supplied loop status wrong");
		}
		// Blackout with a freeze exposure: no generation and no battery
		// means no pump draw, no flow, and no heating — the freeze
		// condition and the whole heating demand stay visible as unmet
		// service, and the loop temperature is unchanged rather than
		// silently clamped or warmed.
		{
			core::World world("CoolantBlackout", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 10, 10, 4, 3.0, 5.0, 1.0, 2.0, 0.0, 4.0) });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "blackout coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(loopSnap.pumpDemandKW, 1.0) && loopSnap.pumpSuppliedKW == 0.0
				&& loopSnap.flowKgPerS == 0.0 && approx(loopSnap.unmetFlowKgPerS, 2.0),
				"blackout invented pump flow");
			require(approx(loopSnap.heaterDemandKW, 4.0) && loopSnap.heaterSuppliedKW == 0.0
				&& approx(loopSnap.heaterUnmetKW, 4.0),
				"blackout freeze-heater demand not fully unmet");
			require(loopSnap.belowFreezeSetpoint
				&& approx(loopSnap.loopTemperatureC, 3.0),
				"freeze exposure not visible or state changed without energy");
			require(loopSnap.status == core::StationTransferStatus::Rejected,
				"blackout loop status wrong");
			require(snapshot.power.coolantPumpKW == 0.0 && snapshot.power.coolantHeaterKW == 0.0,
				"blackout coolant power totals not zero");
			require(sawEvent(world, "service refused") && sawEvent(world, "freeze-protection setpoint"),
				"blackout refusal or freeze exposure not announced");
		}
		// Coolant loss: a dry loop is disabled and never flows or heats,
		// even with ample power — circulation is never invented from an
		// empty inventory. A half-filled loop is capped by its inventory
		// fraction: half the coolant means half the sustainable draw and
		// half the flow.
		{
			core::World world("CoolantLoss", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("dry", "bus", 100, 0, 4, 3.0, 5.0, 1.0, 2.0, 5.0, 4.0),
				  loop("half", "bus", 100, 50, 4, 20.0, 5.0, 1.0, 2.0, 0.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "coolant-loss run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& dry = snapshot.coolantLoops[0];
			require(dry.disabled && dry.coolantKg == 0.0 && dry.fillFraction == 0.0
				&& dry.pumpDemandKW == 0.0 && dry.flowKgPerS == 0.0
				&& dry.heaterDemandKW == 0.0 && dry.status == core::StationTransferStatus::Idle,
				"a dry loop drew power, flowed or heated anyway");
			auto const& half = snapshot.coolantLoops[1];
			require(approx(half.fillFraction, 0.5)
				&& approx(half.pumpDemandKW, 0.5) && approx(half.pumpSuppliedKW, 0.5)
				&& approx(half.circulationFraction, 0.5) && approx(half.flowKgPerS, 1.0),
				"a half-filled loop was not capped by its inventory fraction");
			require(half.status == core::StationTransferStatus::Full,
				"half-filled loop within its sustainable draw should be fully served");
			// The dry loop's exchange stayed exactly zero: no fluid, no
			// heat crossing, no matter what the reservoir does.
			require(dry.exchangeIntoLoopKW == 0.0 && dry.exchangedEnergyKJ == 0.0
				&& dry.residualKJ == 0.0,
				"dry loop booked a heat crossing it could not make");
		}
		// Zero rated flow (#558): a filled, powered loop whose pump is
		// rated at zero flow moves no coolant, so the circulation-gated
		// exchanger transfers no heat. The equipment limit binds the
		// exchange, not just the reported flow: the pump still draws its
		// power and dissipates it into the loop, but the reservoir sees
		// nothing across the exchanger and both ledgers still close.
		{
			core::World world("CoolantZeroFlow", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 30.0) },
				{ loop("loop", "bus", 1.0, 1.0, 4.0, 20.0, 5.0, 1.0, 0.0, 1.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 1.0), "zero-rated-flow coolant run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			auto const& loopSnap = snapshot.coolantLoops[0];
			require(approx(loopSnap.fillFraction, 1.0) && !loopSnap.disabled
				&& approx(loopSnap.pumpSuppliedKW, 1.0),
				"zero-rated-flow loop was not filled and powered as authored");
			require(loopSnap.flowKgPerS == 0.0 && approx(loopSnap.unmetFlowKgPerS, 0.0)
				&& approx(snapshot.thermal.totalCirculationKgPerS, 0.0),
				"a zero rated-flow pump reported flow it cannot move");
			require(loopSnap.exchangeIntoLoopKW == 0.0 && loopSnap.exchangedEnergyKJ == 0.0,
				"zero flow carried exchanger heat with no coolant moving");
			// The loop warms only by the pump work actually dissipated
			// into it (1 kW x 1 s over 4 kJ/K = +0.25 K); the reservoir
			// keeps its starting temperature because nothing crossed.
			require(approx(loopSnap.loopTemperatureC, 20.25, 1e-3),
				"zero-flow loop temperature did not follow pump work alone");
			require(snapshot.thermal.reservoirs[0].temperatureC == 30.0
				&& snapshot.thermal.reservoirs[0].loopEnergyKJ == 0.0,
				"zero-flow exchange leaked into the reservoir");
			require(std::abs(loopSnap.residualKJ) < 1e-4
				&& std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"zero-flow loop or reservoir ledger did not close");
		}
		// Heater capacity and the thermostat: the heater runs at exactly
		// its capacity while the loop is below the setpoint, warms the
		// loop analytically, drops its demand to zero once the setpoint is
		// reached, and none of its heat is double-booked into the
		// reservoir (zero conductance keeps the loop thermally alone).
		{
			core::World world("CoolantHeater", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 10, 10, 4, 0.0, 5.0, 0.0, 0.0, 0.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// 10 kg x 4 kJ/kg K = 40 kJ/K; 4 kW warms it 0.1 K/s, so
			// 50 simulated seconds lift it from 0 to the 5 C setpoint,
			// plus a few ticks past the crossing to show the thermostat
			// drops its demand to zero once the setpoint is held.
			require(smoke::station::advanceSeconds(world, 50.0666666666667),
				"freeze-heating run failed");
			auto const loopSnap = world.getStationSystemsSnapshot().coolantLoops[0];
			require(approx(loopSnap.loopTemperatureC, 5.0, 1e-3),
				"heater capacity did not warm the loop by the closed form");
			require(!loopSnap.belowFreezeSetpoint && loopSnap.heaterDemandKW == 0.0
				&& loopSnap.heaterUnmetKW == 0.0,
				"heater kept demanding at or above the setpoint");
			require(approx(loopSnap.heaterEnergyKJ, 200.0, 1e-3)
				&& std::abs(loopSnap.residualKJ) < 1e-4,
				"heater energy ledger did not close at capacity");
			require(world.getStationSystemsSnapshot().thermal.reservoirs[0].temperatureC == 20.0
				&& world.getStationSystemsSnapshot().thermal.reservoirs[0].loopEnergyKJ == 0.0,
				"heater heat escaped the loop without circulation — exchange must be circulation-gated");
			require(approx(world.getStationSystemsSnapshot().thermal.totalFreezeHeatingKW, 0.0),
				"freeze heating total should be zero after reaching the setpoint");
		}
		// Isolated inventories (#752/#757): loops never pool because each
		// loop owns its own coolant by construction - no class rule is
		// needed, and a loop carries no class at all. A dry loop stays dry
		// and inert while the charged loop on the
		// same reservoir circulates; neither loop's inventory moves
		// toward the other, and the reservoir sees only the circulating
		// loop's exchange through the heat-only exchanger.
		{
			core::World world("CoolantIsolated", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("charged-loop", "bus", 100, 100, 4, 20, 5, 1.0, 2.0, 5.0, 0.0),
				  loop("dry-loop", "bus", 100, 0, 4, 20, 5, 1.0, 2.0, 5.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "isolated-inventory run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.coolantLoops[0].coolantKg, 100.0)
				&& snapshot.coolantLoops[0].flowKgPerS > 0.0
				&& snapshot.coolantLoops[0].status == core::StationTransferStatus::Full,
				"the charged loop did not keep circulating");
			require(snapshot.coolantLoops[1].coolantKg == 0.0
				&& snapshot.coolantLoops[1].flowKgPerS == 0.0
				&& snapshot.coolantLoops[1].disabled,
				"the dry loop gained coolant or flow");
			require(std::abs(snapshot.coolantLoops[0].residualKJ) < 1e-6
				&& std::abs(snapshot.coolantLoops[1].residualKJ) < 1e-6,
				"isolated coolant loop energy ledgers did not close");
			// The reservoir's loop term is only the charged loop's exchange:
			// the dry loop contributed exactly nothing.
			require(std::abs(snapshot.thermal.reservoirs[0].loopEnergyKJ
				+ snapshot.coolantLoops[0].exchangedEnergyKJ) < 1e-6,
				"reservoir loop heat is not the negation of the circulating loop's exchange");
			require(std::abs(snapshot.thermal.reservoirs[0].residualKJ) < 1e-6,
				"reservoir ledger with loop exchange did not close");
		}
		// Blackout ride-through on stored energy: the freeze heater runs
		// off the battery on the same pool, the battery ledger closes, and
		// the loop warms by the heater's energy.
		{
			core::World world("CoolantBattery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 8.0; bank.maxChargeKW = 5.0; bank.maxDischargeKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 10, 10, 4, 0.0, 5.0, 0.0, 0.0, 0.0, 4.0) },
				{}, { bank });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "battery freeze-heating run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.coolantLoops[0].heaterSuppliedKW, 4.0),
				"freeze heater did not run off stored energy");
			require(approx(snapshot.power.batteryDischargeKW, 4.0)
				&& approx(snapshot.power.coolantHeaterKW, 4.0),
				"battery discharge and coolant heater totals disagree");
			require(approx(snapshot.coolantLoops[0].loopTemperatureC, 1.0, 1e-3),
				"loop did not warm by the heater energy over 10 s");
			require(std::abs(snapshot.coolantLoops[0].residualKJ) < 1e-4
				&& std::abs(snapshot.power.batteries[0].residualKWh) < 1e-9,
				"loop or battery ledger did not close through the outage");
		}
		// The loop draw precedes battery charging: the pump takes its
		// demand from generation first and only the surplus no loop used
		// charges the battery — the loop never charges itself nor steals
		// back the charge.
		{
			core::World world("CoolantSurplus", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			core::BatterySettings bank;
			bank.id = "bank"; bank.nameplateKWh = 10.0; bank.usableKWh = 8.0;
			bank.startingStoredKWh = 4.0; bank.maxChargeKW = 5.0; bank.maxDischargeKW = 5.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 100, 100, 4, 20, 5, 1.0, 1.0, 0.0, 0.0) },
				{ field }, { bank });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "surplus-ordering run failed");
			auto const& snapshot = world.getStationSystemsSnapshot();
			require(approx(snapshot.coolantLoops[0].pumpSuppliedKW, 1.0),
				"pump did not take its full demand from generation");
			require(approx(snapshot.power.batteryChargeKW, 1.0),
				"battery did not charge from exactly the surplus the loop left");
			require(approx(snapshot.power.surplusKW, 0.0, 1e-9),
				"surplus remained after the loop draw and charging");
		}
		// Eclipse transition: circulation is announced lost when the
		// pool goes dark and restored when the sun returns, with the
		// freeze exposure appearing while unpowered and below setpoint.
		{
			core::World world("CoolantEclipse", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ProfileSettings cycle;
			cycle.periodSeconds = 2.0;
			cycle.segments = { {0.0, 1.0}, {1.0, 0.0} };
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 2.0;
			field.illumination = cycle;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 10, 10, 4, 3.0, 5.0, 1.0, 2.0, 0.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 6.0), "eclipse coolant run failed");
			require(sawEvent(world, "circulating") && sawEvent(world, "circulation lost"),
				"circulation loss and recovery not announced across the eclipse boundary");
			require(sawEvent(world, "freeze-protection setpoint"),
				"freeze exposure during eclipse not announced");
		}
		// #559: a stiff exchanger on a small loop against a small
		// reservoir. A tick-start rate applied whole moves the loop far
		// past the equilibrium it shares with the reservoir — the
		// reported case drove 1 kg of coolant from 20 C to -966 C in one
		// tick, below absolute zero. The exchange is solved implicitly
		// against the reservoir's end-of-tick temperature instead, so it
		// approaches the shared equilibrium as closely as the conductance
		// allows and cannot cross it, reverse the two bodies, or leave the
		// range of the temperatures the tick started inside.
		{
			core::World world("CoolantStiffExchange", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("stiff", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			// The effective capacity of the pair bounds how much heat one
			// tick of passive exchange may move however stiff the
			// conductance: C_eff = C_loop x C_bus / (C_loop + C_bus).
			auto const effectiveCapacityKJPerK = 0.5;
			auto previousGapK = 20.0;
			for (uint64_t tick = 0; tick < static_cast<uint64_t>(std::ceil(2.0 / dt)); ++tick)
			{
				require(advanceStationSteps(world, 1), "stiff-exchange step failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& loopSnap = snapshot.coolantLoops[0];
				auto const& bus = snapshot.thermal.reservoirs[0];
				// Neither body leaves the range the exchange started with:
				// no undershoot below the cold side, no overshoot past the
				// hot side, and nothing near absolute zero.
				require(loopSnap.loopTemperatureC > 0.0
					&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
					"stiff exchange drove the loop outside its starting range");
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"stiff exchange drove the reservoir outside the starting range");
				// No reversal and no amplification: the hot side stays the
				// hot side and the gap only ever closes.
				auto const gapK = loopSnap.loopTemperatureC - bus.temperatureC;
				require(gapK >= -1e-9, "stiff exchange reversed hot and cold");
				require(gapK <= previousGapK + 1e-9,
					"stiff exchange amplified the separation instead of closing it");
				// The heat that crossed this tick is bounded by the
				// equilibrium-limited transfer for the pair at the gap the
				// tick started with — the exact ceiling the implicit solve
				// approaches but never passes.
				require(std::abs(loopSnap.exchangeIntoLoopKW) * dt
					<= effectiveCapacityKJPerK * previousGapK + 1e-9,
					"exchange moved more heat than the pair's equilibrium allows");
				require(approx(bus.loopEnergyKJ, -loopSnap.exchangedEnergyKJ, 1e-6),
					"stiff exchange is not equal and opposite across the ledgers");
				require(std::abs(loopSnap.residualKJ) < 1e-6
					&& std::abs(bus.residualKJ) < 1e-6,
					"stiff-exchange loop or reservoir ledger did not close");
				previousGapK = gapK;
			}
			// For two 1 kJ/K bodies, implicit exchange contracts the
			// gap by a=1/(1+2*G*dt). The 1 kW pump then adds dt K:
			// gap[n] = a*gap[n-1] + dt, not a timestep-independent 0.05 K.
			auto const& settled = world.getStationSystemsSnapshot();
			auto const contraction = 1.0 / (1.0 + 2.0 * 1000.0 * dt);
			auto const steadyGap = dt / (1.0 - contraction);
			auto const expectedGap = steadyGap + (20.0 - steadyGap)
				* std::pow(contraction, std::ceil(2.0 / dt));
			require(approx(settled.coolantLoops[0].loopTemperatureC
				- settled.thermal.reservoirs[0].temperatureC, expectedGap, 1e-9),
				"stiff exchange did not follow the analytical pump/exchange recurrence");
		}
		// Several loops sharing one reservoir must compete for the same
		// finite heat, not each drain a temperature none of them ever sees
		// move. Independent per-edge caps would let every loop pull as if
		// the bus stayed at its tick-start temperature, collectively
		// overdrawing it many times over. The star is solved as one system,
		// so the bus and all its loops stay inside the range they started
		// in and the bus books exactly what the loops together gave up.
		{
			core::World world("CoolantSharedReservoir", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("hot-a", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0),
				  loop("hot-b", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			for (uint64_t tick = 0; tick < static_cast<uint64_t>(std::ceil(1.0 / dt)); ++tick)
			{
				require(advanceStationSteps(world, 1), "shared-reservoir step failed");
				auto const& snapshot = world.getStationSystemsSnapshot();
				auto const& bus = snapshot.thermal.reservoirs[0];
				for (auto const& loopSnap : snapshot.coolantLoops)
				{
					require(loopSnap.loopTemperatureC > 0.0
						&& loopSnap.loopTemperatureC <= 20.0 + 1e-9,
						"a shared-reservoir loop left the starting temperature range");
				}
				require(bus.temperatureC >= 0.0 && bus.temperatureC < 20.0,
					"the shared bus left the starting temperature range");
				// The bus's whole loop term is the sum of both loops'
				// exchange, negated: no loop's heat counted twice, and no
				// loop drew heat the bus never received.
				require(approx(bus.loopEnergyKJ,
					-(snapshot.coolantLoops[0].exchangedEnergyKJ
						+ snapshot.coolantLoops[1].exchangedEnergyKJ), 1e-6),
					"the shared bus did not book exactly what its loops gave up");
				require(std::abs(bus.residualKJ) < 1e-6,
					"shared-reservoir bus ledger did not close");
			}
		}
		// #559 in the sandbox: the bundled Lua model runs the same
		// implicit star and asserts its own loop temperatures stay inside
		// the starting range and never reverse, while the host reports the
		// exact tick-one exchange the closed form predicts — the two sides
		// agree on the rule and on the numbers, not just on not crashing.
		{
			auto const coupling = dt * 1000.0 / 1.0;
			auto const weightKJPerK = dt * 1000.0 / (1.0 + coupling);
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
					"local loop, bus = self.coolant_loops[1], self.reservoirs[1]\n"
					"if tick == 1 then\n"
					"assert(math.abs(loop.lastExchange - " + expectedLiteral
					+ ") < 1e-9, 'sandbox exchange did not follow the implicit star solve')\n"
					"end\n"
					"assert(loop.temperature > 0.0 and loop.temperature <= 20.0 + 1e-9,\n"
					"  'sandbox stiff exchange drove the loop outside its starting range')\n"
					"assert(bus.temperature >= 0.0 and bus.temperature < 20.0,\n"
					"  'sandbox stiff exchange drove the reservoir outside the starting range')\n"
					"assert(loop.temperature >= bus.temperature - 1e-9,\n"
					"  'sandbox stiff exchange reversed hot and cold')\n"
					"return result\n"
					"end\n"
					"return model\n"
					"end";
			});
			core::World sandboxStiff("SandboxStiffExchange", 2, 2);
			sandboxStiff.pauseSimulation(); sandboxStiff.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 100.0;
			auto const [ok, diagnostic] = coolantConfigure(sandboxStiff,
				{ reservoir("bus", 1.0, 0.0) },
				{ loop("stiff", "bus", 1.0, 1.0, 1.0, 20.0,
					5.0, 1.0, 1.0, 1000.0, 0.0) },
				{ field });
			require(ok, diagnostic.c_str());
			sandboxStiff.resumeSimulation();
			require(smoke::station::advanceSeconds(sandboxStiff, 1.0),
				"Bundled Lua implicit exchange failed its in-sandbox assertions");
			require(std::abs(sandboxStiff.getStationSystemsSnapshot()
					.coolantLoops[0].loopTemperatureC - 20.0) < 20.0
				&& sandboxStiff.getStationSystemsSnapshot().coolantLoops[0].exchangedEnergyKJ < 0.0,
				"host did not mirror the sandbox's heat-out-of-loop exchange");
			core::setStationSystemsSourceLoader({});
		}
		// Deterministic replay: the same authored scenario ticked twice
		// from Reset produces identical snapshots and event history.
		{
			core::World world("CoolantReplay", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::SolarArraySettings field; field.id = "field"; field.capacityKW = 10.0;
			auto const [ok, diagnostic] = coolantConfigure(world,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 100, 100, 4, 20, 5, 1.2, 3.0, 6.0, 2.0),
				  loop("dry", "bus", 100, 0, 4, 3.0, 5.0, 1.0, 2.0, 5.0, 4.0) },
				{ field });
			require(ok, diagnostic.c_str());
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "replay original run failed");
			auto const expected = world.getStationSystemsSnapshot();
			auto const expectedEvents = world.getStationSystemsEvents();
			require(expected.coolantLoops.size() == 2, "replay scenario snapshot incomplete");
			world.pauseSimulation(); world.resetSimulation();
			require(world.getStationSystemsSnapshot().coolantLoops.empty()
				|| world.getStationSystemsSnapshot().elapsedSeconds == 0.0,
				"reset did not clear the coolant runtime");
			world.resumeSimulation();
			require(smoke::station::advanceSeconds(world, 10.0), "replay replay run failed");
			require(world.getStationSystemsSnapshot() == expected,
				"coolant scenario is not deterministic across Reset");
			require(world.getStationSystemsEvents() == expectedEvents,
				"coolant event history is not deterministic across Reset");
		}
		// The bundled Lua sandbox receives the loop descriptors and
		// mirrors the model: a corrupt crossing fails contained at
		// construction through the integrated error path.
		{
			core::setStationSystemsSourceLoader([] {
				return "return function(name, stores, transfers, conversions, solar, loads, batteries, deliveries, reservoirs, radiators, coolantLoops, chillers, generators, atmospheres, airlockCycles, crews, waterCollectors, waterTreatments, electrolyzers, co2Captures, fuelCells, thermalBuffers, wasteEjections, sabatierReactors, methaneProcessors, microbialProcessors, cropPopulations, algaeCultures)\n"
					"assert(type(coolantLoops) == 'table' and #coolantLoops == 1, 'coolant loop descriptor crossing lost')\n"
					"assert(coolantLoops[1].coolantCapacityKg > 0, 'corrupt coolant capacity crossing')\n"
					"error('corrupt coolant descriptor crossing')\n"
					"end";
			});
			core::World corrupt("CoolantCorrupt", 2, 2);
			corrupt.pauseSimulation(); corrupt.finishBuild();
			auto const [ok, diagnostic] = coolantConfigure(corrupt,
				{ reservoir("bus", 1000.0, 20.0) },
				{ loop("loop", "bus", 100, 100, 4, 20, 5, 1.0, 1.0, 1.0, 1.0) });
			require(!ok && diagnostic.find("corrupt coolant descriptor crossing") != std::string::npos
				&& !corrupt.getStationSystemsSnapshot().enabled,
				"Corrupt coolant sandbox crossing not contained at construction");
			core::setStationSystemsSourceLoader({});
		}
	}
}

void registerStationThermalChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"thermal", thermal});
	checks.push_back({"coolant", coolant});
}
