#include "StationForecastChecks.h"
#include "StationRig.h"
#include "core/AgentTagRegistryDocument.h"
#include <cstdio>
#include <optional>

namespace
{
	using namespace smoke::station;

	// #420: the bounded scenario forecast. The static estimate is
	// extended from the observed current rate to the configured
	// demand/production profiles and the authored future delivery
	// schedule: projected inventory at resupply, shortfall onset with
	// the limiting service, capacity-capped accepted delivery
	// quantities, and scenario/uncertainty labels that never present
	// biology as guaranteed recovery. The forecast mutates no live
	// inventory, advances no World clock, consumes no deterministic
	// stream, and is bounded by the declared horizon and segment cap.
	void forecast(smoke::Context const& context)
	{
		auto const approx = [](double actual, double expected, double tolerance = 1e-6)
		{ return std::abs(actual - expected) <= tolerance; };
		auto const runOrDiagnose = [](core::World& world, uint64_t ticks, char const* what)
		{
			if (world.advanceTicks(ticks)) return;
			auto const diagnostics = world.getStationSystemsDiagnostics();
			std::fprintf(stderr, "%s failed: %s\n", what,
				diagnostics.empty() ? "(no Station diagnostic)" : diagnostics.back().message.c_str());
			throw std::runtime_error(std::string(what) + " run failed");
		};
		auto const findForecast = [](std::vector<core::StationForecastEntry> const& entries,
			std::string const& id) -> core::StationForecastEntry const*
		{
			for (auto const& entry : entries)
				if (entry.storeId == id) return &entry;
			return nullptr;
		};
		auto const findStore = [](std::vector<core::StationSystemsStoreSnapshot> const& stores,
			std::string const& id) -> core::StationSystemsStoreSnapshot const*
		{
			for (auto const& store : stores)
				if (store.id == id) return &store;
			return nullptr;
		};
		auto const chainStores = [](std::string const& id, double capacity,
			double starting) -> core::ResourceStoreSettings
		{
			core::ResourceStoreSettings settings;
			settings.id = id; settings.unit = "kg";
			settings.capacity = capacity; settings.startingQualified = starting;
			return settings;
		};
		auto const chainTransfer = [](std::string const& id, std::string const& src,
			std::string const& dst, double rate, int priority)
			-> core::ResourceTransferSettings
		{
			core::ResourceTransferSettings settings;
			settings.id = id; settings.sourceId = src; settings.destinationId = dst;
			settings.unit = "kg"; settings.ratePerSecond = rate;
			settings.priority = priority;
			return settings;
		};
		std::string diagnostic;
		// Scenario A: a known analytic profile with a timely one-off
		// resupply. A 2 kg/s drain on 180 drawable at t=10 is rescued at
		// t=50 by a 400 kg arrival (projected 100 drawable remaining at
		// the arrival), then runs out at t=300 with the transfer named as
		// the limiting service and the uncovered drain reported.
		{
			core::World world("ForecastTimely", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 100.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast timely resupply"); // t = 10 s
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available,
				"the scenario forecast is not available for an enabled Station");
			require(approx(snap.forecast.asOfSeconds, 10.0),
				"the forecast is not as of the snapshot's simulation time");
			require(approx(snap.forecast.horizonSeconds, core::StationForecastHorizonSeconds),
				"the forecast horizon is not the declared bound");
			require(!snap.forecast.truncated, "a sparse schedule truncated the forecast");
			require(snap.forecast.scenarioLabel.find("declared-profile scenario") == 0,
				"the forecast carries no scenario label");
			require(snap.forecast.uncertaintyLabel.find("provisional biology") == 0,
				"the forecast carries no uncertainty label");
			auto const* entry = findForecast(snap.forecast.stores, "water");
			require(entry != nullptr, "no forecast entry for the drained store");
			require(entry->unit == "kg", "the forecast entry lost the store's declared unit");
			require(approx(entry->drawableAtStart, 180.0, 1e-4)
				&& approx(entry->physicalAtStart, 280.0, 1e-4),
				"the forecast start state does not match the committed inventory");
			require(approx(entry->netRateAtStart, -2.0),
				"the projected net rate does not match the authored drain");
			require(entry->basis == core::StationForecastBasis::Shortfall
				&& entry->hasShortfall,
				"a store that runs out inside the horizon did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 300.0, 1e-5),
				"the projected depletion onset is not drawable-after-arrival / rate");
			require(entry->limitingServiceId == "pump"
				&& entry->limitingServiceKind == "transfer",
				"the limiting service is not the binding transfer");
			require(approx(entry->projectedDrawableAtHorizon, 0.0),
				"a depleted store did not project zero drawable at the horizon");
			require(entry->arrivals.size() == 1, "the authored arrival is missing");
			if (entry->arrivals.size() == 1)
			{
				auto const& arrival = entry->arrivals[0];
				require(arrival.deliveryId == "tanker" && arrival.storeId == "water",
					"the projected arrival lost its identity");
				require(approx(arrival.arrivalSeconds, 50.0),
					"the projected arrival is not on the authored schedule");
				require(approx(arrival.projectedDrawableBefore, 100.0, 1e-5),
					"the projected inventory at resupply is wrong");
				require(approx(arrival.acceptedQuantity, 400.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"a delivery that fits was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"a timely qualified arrival did not read as a rescue");
			}
			require(approx(entry->unmetOutflowTotal,
				2.0 * (10.0 + core::StationForecastHorizonSeconds - 300.0), 1e-3),
				"the uncovered drain after depletion is not reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the projected exchanges do not conserve for the drained store");
			auto const* tankEntry = findForecast(snap.forecast.stores, "tank");
			require(tankEntry != nullptr
				&& tankEntry->basis == core::StationForecastBasis::Sustained
				&& approx(tankEntry->netRateAtStart, 2.0)
				&& approx(tankEntry->projectedDrawableAtHorizon, 700.0, 1e-3),
				"the receiving store did not project as sustained with the delivered total");
			// The forecast mutated nothing: the live store snapshot is
			// unchanged by projection, and a second projection from the
			// same state is identical.
			auto const before = snap.stores;
			auto const again = world.getStationSystemsSnapshot();
			require(again.stores == before,
				"computing the forecast mutated the live store inventory");
			require(again.forecast == snap.forecast,
				"the forecast is not a deterministic function of the committed state");
			require(world.getSimulationTick() == snap.worldTick,
				"computing the forecast advanced the World");
			require(world.getStationSystemsDiagnostics().empty(),
				"the forecast produced a Station diagnostic");
			// Determinism across two identical worlds: the same scenario
			// projects identically, so no hidden stream was consumed.
			core::World twin("ForecastTimelyTwin", 2, 2);
			twin.pauseSimulation(); twin.finishBuild();
			require(twin.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			twin.resumeSimulation();
			runOrDiagnose(twin, 600, "forecast twin run");
			require(twin.getStationSystemsSnapshot().forecast == snap.forecast,
				"two identical scenario runs projected differently");
			// Persistence: the forecast is derived, never stored. A reset
			// re-derives it from the initial state, and a saved and
			// reopened World - which starts from that same initial state -
			// re-derives the identical projection.
			world.pauseSimulation();
			world.resetSimulation();
			auto const reset = world.getStationSystemsSnapshot().forecast;
			require(reset.asOfSeconds == 0.0
				&& approx(reset.stores.size(), 2.0)
				&& approx(findForecast(reset.stores, "water")->drawableAtStart, 200.0, 1e-4)
				&& approx(findForecast(reset.stores, "water")->shortfallOnsetSeconds, 300.0, 1e-5),
				"reset did not re-derive the forecast from the initial state");
			auto const path = context.temporaryRoot() / "forecast-roundtrip.world.yaml";
			world.saveTo(path.string());
			auto const reopened = core::loadWorldDocument(path);
			require(reopened != nullptr, "the forecast document failed to reopen");
			require(reopened->getStationSystemsSettings() == settings,
				"the station settings did not survive the forecast round trip");
			require(reopened->getStationSystemsSnapshot().forecast == reset,
				"the reopened forecast is not re-derived identically from the initial state");
			require(reopened->getStationSystemsSnapshot().forecast.available,
				"the reopened forecast is not available");
		}
		// Scenario B: delayed resupply. The arrival lands after the
		// projected depletion: the shortfall onset stands, the arrival is
		// not a rescue, and the drain blocked between depletion and
		// arrival is reported as uncovered demand.
		{
			core::World world("ForecastDelayed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 10.0f;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 1000.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 400.0; tanker.firstOccurrenceSeconds = 150.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast delayed resupply"); // t = 10 s
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->hasShortfall,
				"a delayed resupply did not forecast a shortfall");
			require(approx(entry->shortfallOnsetSeconds, 100.0, 1e-5),
				"the depletion onset ignored the delayed arrival");
			require(entry->arrivals.size() == 1 && !entry->arrivals[0].rescuesStore,
				"an arrival after depletion read as a rescue");
			require(approx(entry->arrivals[0].projectedDrawableBefore, 0.0),
				"the projected inventory at a late arrival is not zero");
			require(approx(entry->arrivals[0].acceptedQuantity, 400.0),
				"a late arrival that fits was not accepted");
			// Blocked 100..150 plus blocked again after the second drain
			// ends at 350: the uncovered drain is the whole blocked span.
			require(approx(entry->unmetOutflowTotal,
				2.0 * (50.0 + (10.0 + core::StationForecastHorizonSeconds - 350.0)), 1e-3),
				"the blocked drain around the delayed arrival is not fully reported");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the delayed-resupply projection does not conserve");
		}
		// Scenario C: accepted delivery quantity is capped by the store's
		// projected capacity; what cannot fit is refused at the boundary
		// and reported, never overflowed.
		{
			core::World world("ForecastCapacity", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings water;
			water.id = "water"; water.unit = "kg";
			water.capacity = 400.0; water.startingQualified = 300.0;
			water.protectedReserve = 100.0;
			core::ResourceStoreSettings tank;
			tank.id = "tank"; tank.unit = "kg";
			tank.capacity = 10000.0; tank.startingQualified = 0.0;
			settings.stores = { water, tank };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "water"; pump.destinationId = "tank";
			pump.unit = "kg"; pump.ratePerSecond = 2.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ResourceDeliverySettings tanker;
			tanker.id = "tanker"; tanker.storeId = "water"; tanker.unit = "kg";
			tanker.quantity = 900.0; tanker.firstOccurrenceSeconds = 50.0;
			settings.deliveries = { tanker };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const* entry = findForecast(world.getStationSystemsSnapshot().forecast.stores, "water");
			require(entry != nullptr && entry->arrivals.size() == 1,
				"the capacity-capped arrival is missing");
			require(approx(entry->arrivals[0].acceptedQuantity, 200.0),
				"the accepted quantity is not the projected headroom");
			require(approx(entry->arrivals[0].rejectedQuantity, 700.0),
				"the refused quantity is not reported");
			require(approx(entry->shortfallOnsetSeconds, 200.0, 1e-5),
				"the depletion after the capped arrival is wrong");
			require(std::abs(entry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the capped-arrival projection does not conserve");
		}
		// Scenario D: a configured crew with a stepped activity profile.
		// The food draw follows the profile, the food store runs out
		// inside the horizon with the crew named as the limiting service,
		// and - matching the live whole-metabolism rule - the urine and
		// organic-waste returns stop together with the draw, so the waste
		// stores freeze at the value the last fed moment produced.
		{
			core::World world("ForecastCrew", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 100.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {50.0, 1.0}};
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 101.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 100000.0, 1.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			// The projection is read at t=0 from the initial state.
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr, "no forecast entry for the crew food store");
			require(approx(food->netRateAtStart, -0.05),
				"the initial food draw is not crew x rate x half-activity");
			require(food->basis == core::StationForecastBasis::Shortfall,
				"the stepping crew draw did not forecast a food shortfall");
			// 13 full periods burn 97.5 kg, the next half-activity leg
			// burns 2.5, and the full-activity leg burns the last 1.0 kg
			// 10 s in: onset 1360 s.
			require(approx(food->shortfallOnsetSeconds, 1360.0, 1e-4),
				"the profiled food depletion onset is wrong");
			require(food->limitingServiceId == "crew"
				&& food->limitingServiceKind == "crew",
				"the limiting service is not the crew");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& urine->basis == core::StationForecastBasis::Sustained
				&& approx(urine->projectedDrawableAtHorizon, 152.5, 1e-3),
				"the urine return did not freeze at the last fed moment");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(waste != nullptr
				&& approx(waste->projectedDrawableAtHorizon, 21.2, 1e-3),
				"the organic-waste return did not freeze at the last fed moment");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr && water->basis == core::StationForecastBasis::Sustained
				&& approx(water->projectedDrawableAtHorizon, 798.0, 1e-3),
				"the crew water draw over the profiled window is wrong");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew-profile projection does not conserve");
			// The live run stays healthy: the forecast is pure observability
			// and the Lua audit is undisturbed.
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast crew live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the crew forecast scenario made the Station unhealthy");
			require(world.getStationSystemsDiagnostics().empty(),
				"the crew forecast scenario produced a diagnostic");
		}
		// Scenario E: uncertainty labels. A store that a crop harvest and
		// a water treatment could replenish is flagged: the possible
		// recovery is visible but is NOT counted as guaranteed, so the
		// projection of the destination stores stays exactly what the
		// declared non-biology flows say.
		{
			core::World world("ForecastUncertainty", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings cropComposition;
			cropComposition.moisture = 0.0;
			cropComposition.nutrientNitrogen = 0.05;
			cropComposition.atmosphericNitrogen = 0.0;
			cropComposition.phosphorus = 0.02;
			cropComposition.potassium = 0.03;
			cropComposition.otherMinerals = 0.05;
			cropComposition.energyKJPerKgDry = 1700.0;
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 40.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingCarbonDioxideKg = 5.0; habitat.startingWaterVapourKg = 1.0;
			habitat.startingTemperatureC = 22.0;
			habitat.thermalCapacityKJPerK = 20000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CropPopulationSettings canopy;
			canopy.id = "canopy"; canopy.atmosphereId = "habitat";
			canopy.waterStoreId = "irrigation";
			canopy.nitrogenStoreId = "fertN"; canopy.phosphorusStoreId = "fertP";
			canopy.potassiumStoreId = "fertK"; canopy.mineralsStoreId = "fertMin";
			canopy.composition = cropComposition;
			canopy.growingAreaM2 = 10.0; canopy.maxBiomassDensityKgPerM2 = 2.0;
			canopy.initialBiomassKg = 1.0;
			canopy.lampRatedKW = 3.6; canopy.illuminationProfile.constantLevel = 1.0;
			canopy.circulationPowerKW = 0.1;
			canopy.growthYieldKgPerKWh = 2.0; canopy.respirationPerHour = 0.5;
			canopy.transpirationKgPerKgGrowth = 5.0;
			canopy.minGrowingTemperatureC = 10.0; canopy.maxGrowingTemperatureC = 35.0;
			canopy.foodStoreId = "food"; canopy.wasteStoreId = "residue";
			canopy.edibleFraction = 0.5; canopy.harvestTriggerKg = 5.0;
			canopy.harvestFraction = 1.0;
			core::WaterTreatmentSettings treatment;
			treatment.id = "polish"; treatment.hazardClass = "normal";
			treatment.sourceStoreId = "raw"; treatment.treatedStoreId = "treated";
			treatment.concentrateStoreId = "concentrate";
			treatment.consumableStoreId = "consumable";
			treatment.throughputKgPerS = 0.005; treatment.recoveryRatio = 0.8;
			treatment.specificEnergyKWhPerKg = 0.5; treatment.consumablePerKg = 0.01;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				plain("irrigation", 200.0, 150.0), plain("fertN", 10.0, 5.0),
				plain("fertP", 10.0, 5.0), plain("fertK", 10.0, 5.0),
				plain("fertMin", 10.0, 5.0),
				plain("food", 1000.0, 100.0), plain("residue", 1000.0, 1.0),
				plain("raw", 1000.0, 100.0), plain("treated", 1000.0, 1.0),
				plain("concentrate", 1000.0, 0.0), plain("consumable", 100.0, 10.0) };
			settings.atmospheres = { habitat };
			settings.cropPopulations = { canopy };
			settings.waterTreatments = { treatment };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->biologyRecoveryPossible,
				"a crop-destined store is not flagged for uncertain biology recovery");
			require(food->basis == core::StationForecastBasis::Sustained
				&& approx(food->netRateAtStart, 0.0)
				&& food->arrivals.empty(),
				"the uncertain harvest was counted into the guaranteed projection");
			auto const* residue = findForecast(snap.forecast.stores, "residue");
			require(residue != nullptr && residue->biologyRecoveryPossible,
				"the crop waste destination is not flagged for uncertain biology");
			auto const* treated = findForecast(snap.forecast.stores, "treated");
			require(treated != nullptr && treated->processProductionPossible,
				"a treatment-destined store is not flagged for unprojected process production");
			require(!treated->biologyRecoveryPossible
				&& treated->basis == core::StationForecastBasis::Sustained,
				"the treatment destination carries the wrong uncertainty mix");
			// #594: the consuming side of the same scenario - the crop's
			// uptake stores and the treatment's source and consumable are
			// drained by pathways the forecast does not project, and must
			// carry the matching consumption flags.
			for (auto const* id : {"irrigation", "fertN", "fertP", "fertK", "fertMin"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->biologyConsumptionPossible
					&& !entry->processConsumptionPossible,
					"a crop uptake store is not flagged for unprojected biology consumption");
			}
			for (auto const* id : {"raw", "consumable"})
			{
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a treatment feed store is not flagged for unprojected process consumption");
			}
			require(food != nullptr && !food->biologyConsumptionPossible
				&& !food->processConsumptionPossible,
				"the crop food destination carries a consumption flag it does not earn");
			world.resumeSimulation();
			runOrDiagnose(world, 600, "forecast uncertainty live run");
			require(world.getStationSystemsSnapshot().healthy,
				"the uncertainty forecast scenario made the Station unhealthy");
		}
		// Scenario F: the computation is bounded. A one-second
		// recurrence across the whole horizon cannot make the forecast
		// run forever: collection stops at the declared segment cap and
		// the truncation is reported. #604: a truncated store with no
		// shortfall inside its complete range reports Incomplete, never
		// Sustained, and the snapshot exposes the complete-through bound
		// so the unavailable range beyond it is explicit.
		{
			core::World world("ForecastBounded", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings ballast;
			ballast.id = "ballast"; ballast.unit = "kg";
			ballast.capacity = 1.0e12; ballast.startingQualified = 1.0;
			settings.stores = { ballast };
			core::ResourceDeliverySettings drip;
			drip.id = "drip"; drip.storeId = "ballast"; drip.unit = "kg";
			drip.quantity = 1.0; drip.firstOccurrenceSeconds = 1.0;
			drip.recurrenceSeconds = 1.0;
			settings.deliveries = { drip };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.truncated,
				"a schedule denser than the segment cap did not truncate the forecast");
			require(snap.forecast.segmentsComputed <= core::StationForecastMaxSegments,
				"the forecast computed more segments than the declared cap");
			require(snap.forecast.completeThroughSeconds > snap.forecast.asOfSeconds
				&& snap.forecast.completeThroughSeconds
					< snap.forecast.asOfSeconds + core::StationForecastHorizonSeconds,
				"a truncated forecast does not report a partial complete-through bound");
			auto const* entry = findForecast(snap.forecast.stores, "ballast");
			require(entry != nullptr
				&& entry->basis == core::StationForecastBasis::Incomplete,
				"a truncated store with no shortfall inside its complete range "
				"did not report Incomplete instead of an unjustified Sustained");
			require(!entry->arrivals.empty(),
				"the truncated projection dropped every already-collected arrival");
			for (auto const& storeEntry : snap.forecast.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= snap.forecast.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
		}
		// Scenario #604-A: the ticket reproduction. A dense unrelated
		// drip (0.1 s recurrence into "noisy") is declared before a
		// one-off 10 kg rescue of the "feed" store, which starts with
		// 2 drawable and drains 1 kg/s to the sink. Without the rescue
		// feed runs out at t=2; with the rescue landing at t=1 it runs
		// out at t=12. The drip still overruns the event budget - the
		// forecast truncates - but the chronological frontier merge
		// keeps the rescue inside the complete-through range, so the
		// depletion onset reads 12 in both declaration orders and
		// matches the sparse control that has no drip at all.
		{
			core::World dense("ForecastDenseRescue", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("noisy", 1.0e9, 1.0),
				chainStores("feed", 100.0, 2.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::ResourceDeliverySettings noise;
			noise.id = "noise"; noise.storeId = "noisy"; noise.unit = "kg";
			noise.quantity = 1.0; noise.firstOccurrenceSeconds = 0.1;
			noise.recurrenceSeconds = 0.1;
			core::ResourceDeliverySettings rescue;
			rescue.id = "rescue"; rescue.storeId = "feed"; rescue.unit = "kg";
			rescue.quantity = 10.0; rescue.firstOccurrenceSeconds = 1.0;
			settings.deliveries = { noise, rescue };
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense drip schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& feed->basis == core::StationForecastBasis::Shortfall,
				"the rescued feed store did not forecast a shortfall");
			require(approx(feed->shortfallOnsetSeconds, 12.0, 1e-3),
				("the dense unrelated schedule dropped the early rescue: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 12").c_str());
			require(feed->arrivals.size() == 1
				&& feed->arrivals[0].deliveryId == "rescue"
				&& approx(feed->arrivals[0].arrivalSeconds, 1.0)
				&& approx(feed->arrivals[0].acceptedQuantity, 10.0)
				&& feed->arrivals[0].rescuesStore,
				"the rescue arrival is missing or misapplied behind the dense schedule");
			require(denseSnap.completeThroughSeconds > 12.0
				&& denseSnap.completeThroughSeconds
					< denseSnap.asOfSeconds + core::StationForecastHorizonSeconds,
				"the truncated forecast does not cover the rescue but stop short of the horizon");
			for (auto const& storeEntry : denseSnap.stores)
				for (auto const& arrival : storeEntry.arrivals)
					require(arrival.arrivalSeconds
						<= denseSnap.completeThroughSeconds + 1e-6,
						"an arrival outside the complete-through range was reported");
			// Declaration permutation: the rescue declared first must
			// project identically - the frontier merge never lets the
			// drip's budget position depend on declaration order.
			settings.deliveries = { rescue, noise };
			core::World permuted("ForecastDenseRescuePermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			require(permutedSnap.truncated, "the permuted forecast stopped truncating");
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the declaration order changed the projected depletion onset");
			// Sparse control: the same rig without the dense drip must
			// project the same depletion onset over the full horizon.
			settings.deliveries = { rescue };
			core::World sparse("ForecastDenseRescueControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			require(!sparseSnap.truncated,
				"the sparse control forecast truncated without a dense schedule");
			require(approx(sparseSnap.completeThroughSeconds,
				sparseSnap.asOfSeconds + sparseSnap.horizonSeconds),
				"a complete forecast does not report complete-through at the horizon");
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-schedule projection disagrees with the sparse control");
		}
		// Scenario #604-B: a dense ejection schedule declared first
		// must not drop a later one-off ejection. The feed store starts
		// with 10 drawable and drains 1 kg/s; a one-off 3 kg ejection
		// at t=1 leaves 6, so depletion lands at t=7. A dense unrelated
		// bilge ejection every 0.1 s overruns the event budget; the
		// merged frontier must still include the one-off whatever the
		// declaration order, matching the sparse control.
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = { chainStores("bilge", 1.0e9, 1.0e8),
				chainStores("feed", 100.0, 10.0),
				chainStores("sink", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("drain", "feed", "sink", 1.0, 0) };
			core::WasteEjectionSettings bilgeEject;
			bilgeEject.id = "bilgeEject"; bilgeEject.storeId = "bilge";
			bilgeEject.unit = "kg"; bilgeEject.quantity = 1.0;
			bilgeEject.firstOccurrenceSeconds = 0.1; bilgeEject.recurrenceSeconds = 0.1;
			core::WasteEjectionSettings feedEject;
			feedEject.id = "feedEject"; feedEject.storeId = "feed";
			feedEject.unit = "kg"; feedEject.quantity = 3.0;
			feedEject.firstOccurrenceSeconds = 1.0;
			settings.wasteEjections = { bilgeEject, feedEject };
			core::World dense("ForecastDenseEject", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense ejection schedule did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 7.0, 1e-3),
				("the dense ejection schedule dropped the one-off ejection: onset is "
				+ std::to_string(feed->shortfallOnsetSeconds) + ", expected 7").c_str());
			settings.wasteEjections = { feedEject, bilgeEject };
			core::World permuted("ForecastDenseEjectPermuted", 2, 2);
			permuted.pauseSimulation(); permuted.finishBuild();
			require(permuted.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const permutedSnap = permuted.getStationSystemsSnapshot().forecast;
			auto const* permutedFeed = findForecast(permutedSnap.stores, "feed");
			require(permutedSnap.truncated && permutedFeed != nullptr
				&& approx(permutedFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"swapping the ejection declaration order changed the onset");
			settings.wasteEjections = { feedEject };
			core::World sparse("ForecastDenseEjectControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-9),
				"the dense-ejection projection disagrees with the sparse control");
		}
		// Scenario #604-C: a dense demand profile must not drop another
		// profile's breakpoints. A crew with a maximally dense activity
		// profile (8 segments per second) is declared before an
		// equipment load whose operating profile (period 100 s) steps on
		// at t=50.05 and drains a feed store holding 62 drawable at
		// 1 kg/s. The crew never touches the feed store; only the load's
		// step at 50.05 matters, and the crew's 0.125 s breakpoints
		// cannot stand in for it. The load draws 49.95 kg during
		// [50.05, 100), idles through [100, 150.05) and takes the last
		// 12.05 kg from 150.05, depleting the store at t=162.1. The
		// crew alone overruns the breakpoint budget so the forecast
		// truncates, but the merged frontier must still have collected
		// the load's 50.05 step, putting depletion at t=162.1 exactly
		// as in the sparse control with no crew. The old
		// declaration-order collection dropped every load breakpoint
		// once the crew filled the budget, starting the drain no earlier
		// than the next 0.125 s crew breakpoint and reporting the
		// depletion late.
		{
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto compound = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "denseCrew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.periodSeconds = 1.0;
			crew.activityProfile.segments = {{0.0, 0.5}, {0.125, 0.6},
				{0.25, 0.7}, {0.375, 0.8}, {0.5, 0.9}, {0.625, 1.0},
				{0.75, 0.9}, {0.875, 0.8}};
			crew.foodStoreId = "rations"; crew.waterStoreId = "potable";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::EquipmentLoadSettings drill;
			drill.id = "drill"; drill.electricalDemandKW = 0.0;
			drill.operatingProfile.periodSeconds = 100.0;
			drill.operatingProfile.segments = {{0.0, 0.0}, {50.05, 1.0}};
			drill.heatOutputKW = 0.0; drill.thermalClass = "low";
			drill.coolingDisposition = core::StationCoolingDisposition::Exported;
			drill.criticalFloorKW = 0.0; drill.priority = 0; drill.maxCurtailment = 1.0;
			core::EquipmentMaterialExchangeSettings draw;
			draw.storeId = "feed"; draw.ratePerSecond = 1.0; draw.consumes = true;
			drill.materialExchanges = { draw };
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("rations", 100000.0, 100000.0, foodComposition),
				compound("potable", 100000.0, 100000.0, waterComposition),
				compound("urine", 100000.0, 1.0, urineComposition),
				compound("organicWaste", 100000.0, 1.0, wasteComposition),
				chainStores("feed", 1000.0, 62.0) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.equipmentLoads = { drill };
			core::World dense("ForecastDenseProfile", 2, 2);
			dense.pauseSimulation(); dense.finishBuild();
			require(dense.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const denseSnap = dense.getStationSystemsSnapshot().forecast;
			require(denseSnap.truncated,
				"the dense crew profile did not truncate the forecast");
			auto const* feed = findForecast(denseSnap.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 162.1, 1e-3),
				("the dense crew profile dropped the load's step breakpoint: onset is "
					+ std::to_string(feed->shortfallOnsetSeconds)
					+ ", expected 162.1").c_str());
			settings.crews.clear();
			settings.atmospheres.clear();
			core::World sparse("ForecastDenseProfileControl", 2, 2);
			sparse.pauseSimulation(); sparse.finishBuild();
			require(sparse.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const sparseSnap = sparse.getStationSystemsSnapshot().forecast;
			auto const* controlFeed = findForecast(sparseSnap.stores, "feed");
			require(!sparseSnap.truncated && controlFeed != nullptr
				&& approx(controlFeed->shortfallOnsetSeconds,
					feed->shortfallOnsetSeconds, 1e-6),
				"the dense-profile projection disagrees with the sparse control");
		}
		// Scenario G: a store that starts depleted reads as a shortfall
		// from the forecast instant, never as sustained supply.
		{
			core::World world("ForecastDepletedStart", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings dry;
			dry.id = "dry"; dry.unit = "kg";
			dry.capacity = 100.0; dry.startingQualified = 0.0;
			core::ResourceStoreSettings held;
			held.id = "held"; held.unit = "kg";
			held.capacity = 100.0; held.startingQualified = 50.0;
			held.protectedReserve = 50.0;
			settings.stores = { dry, held };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* dryEntry = findForecast(snap.forecast.stores, "dry");
			auto const* heldEntry = findForecast(snap.forecast.stores, "held");
			require(dryEntry != nullptr && dryEntry->hasShortfall
				&& approx(dryEntry->shortfallOnsetSeconds, snap.forecast.asOfSeconds),
				"a store with nothing drawable did not forecast an immediate shortfall");
			require(heldEntry != nullptr && heldEntry->hasShortfall
				&& approx(heldEntry->drawableAtStart, 0.0),
				"a store fully inside its protected reserve did not forecast a shortfall");
		}
		// Scenario H: destination backpressure. A transfer into a store
		// that fills inside the horizon stops at the fill-up: the source
		// keeps the material it never got to send, and neither store is
		// a shortage - the capacity limit is the account.
		{
			core::World world("ForecastBackpressure", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings src;
			src.id = "src"; src.unit = "kg";
			src.capacity = 1000.0; src.startingQualified = 200.0;
			core::ResourceStoreSettings dst;
			dst.id = "dst"; dst.unit = "kg";
			dst.capacity = 100.0; dst.startingQualified = 90.0;
			settings.stores = { src, dst };
			core::ResourceTransferSettings feed;
			feed.id = "feed"; feed.sourceId = "src"; feed.destinationId = "dst";
			feed.unit = "kg"; feed.ratePerSecond = 1.0; feed.priority = 1;
			settings.transfers = { feed };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* srcEntry = findForecast(snap.forecast.stores, "src");
			auto const* dstEntry = findForecast(snap.forecast.stores, "dst");
			require(srcEntry != nullptr && srcEntry->basis == core::StationForecastBasis::Sustained
				&& approx(srcEntry->projectedDrawableAtHorizon, 190.0, 1e-3),
				"the source did not keep the material the full destination back-pressured");
			require(approx(srcEntry->unmetOutflowTotal, 0.0),
				"destination backpressure was misreported as a source shortage");
			require(dstEntry != nullptr && dstEntry->basis == core::StationForecastBasis::Sustained
				&& approx(dstEntry->projectedDrawableAtHorizon, 100.0, 1e-3),
				"the backpressured destination did not project exactly full");
			require(std::abs(srcEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6
				&& std::abs(dstEntry->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the backpressure projection does not conserve");
		}
		// Scenario I: a full waste destination back-pressures the whole
		// crew metabolism. Once the urine store fills the crew draws no
		// food or water and returns nothing - the live whole-metabolism
		// rule (#401) - and the held-back food draw is reported as
		// uncovered demand.
		{
			core::World world("ForecastWasteFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto store = [](std::string const& id, double capacity, double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				settings.composition = std::move(composition);
				return settings;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				store("food", 100000.0, 100.0, foodComposition),
				store("water", 100000.0, 1000.0, waterComposition),
				store("urine", 3.0, 0.0, urineComposition),
				store("organicWaste", 100000.0, 1.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			// Urine fills 3 kg at 0.15 kg/s after 20 s; the crew then
			// stops whole: food stays at 100 - 0.1 x 20 = 98 and the
			// held-back draw is uncovered demand.
			auto const* food = findForecast(snap.forecast.stores, "food");
			require(food != nullptr && food->basis == core::StationForecastBasis::Sustained
				&& approx(food->projectedDrawableAtHorizon, 98.0, 1e-2),
				"the full waste destination did not stop the crew food draw");
			require(approx(food->unmetOutflowTotal,
				0.1 * (core::StationForecastHorizonSeconds - 20.0), 1e-1),
				"the held-back crew draw is not reported as uncovered demand");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			require(urine != nullptr
				&& approx(urine->projectedDrawableAtHorizon, 3.0, 1e-2),
				"the urine store did not project exactly to its capacity");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(water != nullptr
				&& approx(water->projectedDrawableAtHorizon, 996.0, 1e-2),
				"the crew water draw did not stop with the metabolism");
		}
		// Scenario J: a transfer chain through a dry intermediate store
		// (#593). A -> B -> C at equal 1 kg/s rates with B starting
		// empty must flow straight through. Gating each transfer on the
		// inventory at the segment start made the solve alternate fill
		// and drain half-duty phases: it truncated the projection at the
		// segment cap, delivered only half the through-flow to C, and
		// invented an unmet transfer demand at B that the live chain
		// never has. The forecast must instead carry the full horizon of
		// through-flow, report no shortage and no uncovered outflow at
		// the dry intermediate, and agree with the live trajectory.
		{
			core::World world("ForecastChainDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available
				&& !snap.forecast.truncated,
				"a sparse constant-rate transfer chain truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a steady through-flow chain needed more than a couple of segments");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(a != nullptr && b != nullptr && c != nullptr,
				"a transfer-chain store is missing from the forecast");
			require(a->netRateAtStart == -1.0
				&& !a->hasShortfall
				&& approx(a->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the chain source did not project a full-horizon drain");
			require(!b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0),
				"the dry intermediate invented an unmet transfer demand");
			require(c->netRateAtStart == 1.0
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the chain end did not receive the full horizon of through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the dry-intermediate chain projection does not conserve");
			// Agreement with the live trajectory: 1200 fixed ticks (20 s)
			// of continuous through-flow, then the forecast from that
			// state starts exactly where the live stores are and continues
			// the same line.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain dry live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "A")->currentQualified, 99980.0, 1e-3)
				&& approx(findStore(live.stores, "B")->currentQualified, 0.0, 1e-3)
				&& approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live dry-intermediate chain did not flow continuously");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the through-flow chain truncated after the live run");
			require(approx(findForecast(f2.stores, "A")->drawableAtStart, 99980.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->drawableAtStart, 20.0, 1e-3),
				"the re-forecast start state does not match the live stores");
			require(approx(findForecast(f2.stores, "A")->projectedDrawableAtHorizon,
				13580.0, 1e-3)
				&& approx(findForecast(f2.stores, "C")->projectedDrawableAtHorizon,
					86420.0, 1e-3),
				"the re-forecast did not continue the live through-flow trajectory");
			require(!findForecast(f2.stores, "B")->hasShortfall
				&& approx(findForecast(f2.stores, "B")->unmetOutflowTotal, 0.0),
				"the dry intermediate still invents unmet demand after the live run");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run dry-intermediate chain projection does not conserve");
		}
		// Scenario K: the same chain with the intermediate store starting
		// full. The incoming leg must not stop while the outgoing leg
		// drains it: the through-flow is symmetric with the dry case and
		// C again receives the full horizon.
		{
			core::World world("ForecastChainFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 10.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate through-flow chain truncated the forecast");
			auto const* b = findForecast(snap.forecast.stores, "B");
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(b != nullptr && !b->hasShortfall
				&& approx(b->netRateAtStart, 0.0)
				&& approx(b->unmetOutflowTotal, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under symmetric through-flow");
			require(c != nullptr
				&& approx(c->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the full-intermediate chain end did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate chain projection does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain full live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "C")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate chain did not deliver the through-flow");
			require(approx(findForecast(live.forecast.stores, "C")->projectedDrawableAtHorizon,
				findStore(live.stores, "C")->currentQualified + 86400.0, 1e-3),
				"the full-intermediate re-forecast did not continue the live trajectory");
			require(!findForecast(live.forecast.stores, "B")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "B")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario L: projected inventory at a later delivery on a
		// through-flow chain. A drop lands on C at t=40; the forecast
		// taken after the 20 s live run must see C at 20 plus 20 s of
		// continued through-flow (40 kg drawable) at the arrival, accept
		// the whole drop, and read it as a rescue.
		{
			core::World world("ForecastChainDelivery", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1.0e6, 100000.0),
				chainStores("B", 10.0, 0.0),
				chainStores("C", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("AB", "A", "B", 1.0, 2),
				chainTransfer("BC", "B", "C", 1.0, 1) };
			core::ResourceDeliverySettings drop;
			drop.id = "drop"; drop.storeId = "C"; drop.unit = "kg";
			drop.quantity = 100.0; drop.firstOccurrenceSeconds = 40.0;
			settings.deliveries = { drop };
			require(world.configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "chain delivery live run"); // t = 20 s
			auto const snap = world.getStationSystemsSnapshot();
			auto const* c = findForecast(snap.forecast.stores, "C");
			require(c != nullptr && c->arrivals.size() == 1,
				"the later delivery is missing from the through-flow chain forecast");
			if (c != nullptr && c->arrivals.size() == 1)
			{
				auto const& arrival = c->arrivals[0];
				require(arrival.deliveryId == "drop"
					&& approx(arrival.arrivalSeconds, 40.0),
					"the later delivery lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 40.0, 1e-3),
					"the projected inventory at the later delivery ignored the through-flow");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0)
					&& arrival.rescuesStore,
					"the later delivery was not accepted as a rescue");
			}
			require(approx(c->projectedDrawableAtHorizon, 86520.0, 1e-3),
				"the through-flow chain horizon total did not include the accepted delivery");
			require(std::abs(c->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the through-flow delivery projection does not conserve");
		}
		// Scenario M: generator fuel is projected demand, never
		// sustained inventory (#594). The ticket's minimal generator: a
		// manually commanded 1 kW machine at efficiency 1 burns a
		// 0.001 kWh store dry in 3.6 s. The forecast must report the
		// shortfall with the generator as the limiting service and the
		// post-depletion draw as uncovered demand, and the live run must
		// agree: the fuel really is gone inside the window the forecast
		// named.
		{
			core::World world("ForecastGeneratorFuel", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr,
				"no forecast entry for the generator fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, -1.0 / 3600.0, 1e-12),
					"the commanded generator fuel draw is not in the projected net rate");
				require(fuelEntry->basis == core::StationForecastBasis::Shortfall
					&& fuelEntry->hasShortfall,
					"a finite fuel store under a commanded generator read as sustained");
				require(approx(fuelEntry->shortfallOnsetSeconds, 3.6, 1e-6),
					"the projected fuel depletion onset is wrong");
				require(fuelEntry->limitingServiceId == "gen"
					&& fuelEntry->limitingServiceKind == "generator",
					"the limiting service is not the generator");
				require(approx(fuelEntry->unmetOutflowTotal,
					(core::StationForecastHorizonSeconds - 3.6) / 3600.0, 1e-6),
					"the fuel demand after depletion is not reported as uncovered");
				require(!fuelEntry->processConsumptionPossible
					&& !fuelEntry->biologyConsumptionPossible,
					"a projected manual generator draw must not also read as unprojected consumption");
				require(std::abs(fuelEntry->conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the generator-fuel projection does not conserve");
			}
			// The live run agrees: the whole 0.001 kWh inventory is gone
			// well inside the window the forecast called short, and the
			// re-forecast from the empty store reads an immediate
			// shortfall rather than sustained supply.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "generator fuel live run"); // t = 20 s
			auto const live = world.getStationSystemsSnapshot();
			auto const* liveFuel = findStore(live.stores, "fuel");
			require(liveFuel != nullptr && liveFuel->currentQualified <= 1e-12,
				"the live run did not consume the fuel the forecast projected away");
			auto const* reEntry = findForecast(live.forecast.stores, "fuel");
			require(reEntry != nullptr && reEntry->hasShortfall
				&& approx(reEntry->drawableAtStart, 0.0)
				&& approx(reEntry->shortfallOnsetSeconds, live.forecast.asOfSeconds),
				"the re-forecast of the empty fuel store is not an immediate shortfall");
			require(live.healthy && world.getStationSystemsDiagnostics().empty(),
				"the generator-fuel forecast scenario produced a Station diagnostic");
		}
		// Scenario N: under Automatic dispatch the generator's draw
		// follows the unmodelled net load, so it is not projected - but
		// the fuel store must carry the unprojected-consumption flag so
		// its Sustained basis is never read as a promise (#594).
		{
			core::World world("ForecastGeneratorAuto", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.dispatchMode = core::StationDispatchMode::Automatic;
			core::ResourceStoreSettings fuel;
			fuel.id = "fuel"; fuel.unit = "kWh";
			fuel.capacity = 1.0; fuel.startingQualified = 0.001;
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "generation";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings generator;
			generator.id = "gen"; generator.fuelStoreId = "fuel";
			generator.thermalClass = "generation"; generator.heatSinkId = "sink";
			generator.ratedOutputKW = 1.0; generator.commandedOutputKW = 1.0;
			generator.efficiency = 1.0; generator.maxRampKWPerSecond = 60.0;
			settings.stores = { fuel };
			settings.thermalReservoirs = { sink };
			settings.generators = { generator };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* fuelEntry = findForecast(snap.forecast.stores, "fuel");
			require(fuelEntry != nullptr, "no forecast entry for the fuel store");
			if (fuelEntry != nullptr)
			{
				require(approx(fuelEntry->netRateAtStart, 0.0),
					"an Automatic-dispatch generator must not project a fixed fuel draw");
				require(fuelEntry->processConsumptionPossible,
					"the fuel store under Automatic dispatch is not flagged for unprojected consumption");
				require(!fuelEntry->biologyConsumptionPossible,
					"the generator fuel store carries a biology consumption flag");
			}
			require(snap.forecast.uncertaintyLabel.find("processConsumptionPossible")
				!= std::string::npos,
				"the uncertainty label does not explain the unprojected consuming pathways");
		}
		// Scenario O: every deliberately-excluded consuming pathway
		// flags the stores it drains (#594): electrolysis feed water,
		// fuel-cell reactants and consumed maintenance spares carry
		// processConsumptionPossible; pure product destinations and an
		// uncommanded manual generator's fuel store carry neither flag.
		{
			core::World world("ForecastConsumptionFlags", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			auto plain = [](std::string const& id, double capacity, double starting)
			{
				core::ResourceStoreSettings settings;
				settings.id = id; settings.unit = "kg";
				settings.capacity = capacity; settings.startingQualified = starting;
				return settings;
			};
			core::ThermalReservoirSettings sink;
			sink.id = "sink"; sink.thermalClass = "moderate";
			sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 20.0;
			sink.minTemperatureC = 0.0; sink.maxTemperatureC = 100.0;
			core::GeneratorSettings idleGen;
			idleGen.id = "idleGen"; idleGen.fuelStoreId = "diesel";
			idleGen.thermalClass = "generation"; idleGen.heatSinkId = "genSink";
			idleGen.ratedOutputKW = 1.0; idleGen.commandedOutputKW = 0.0;
			idleGen.efficiency = 0.35; idleGen.maxRampKWPerSecond = 60.0;
			core::ThermalReservoirSettings genSink;
			genSink.id = "genSink"; genSink.thermalClass = "generation";
			genSink.heatCapacityKJPerK = 10000.0; genSink.startingTemperatureC = 20.0;
			genSink.minTemperatureC = 0.0; genSink.maxTemperatureC = 100.0;
			core::ElectrolyzerSettings electrolyzer;
			electrolyzer.id = "elektro"; electrolyzer.hazardClass = "normal";
			electrolyzer.waterStoreId = "feedWater";
			electrolyzer.oxygenStoreId = "o2"; electrolyzer.hydrogenStoreId = "h2";
			electrolyzer.thermalClass = "moderate"; electrolyzer.heatSinkId = "sink";
			electrolyzer.rateKgPerS = 0.001;
			core::FuelCellSettings fuelCell;
			fuelCell.id = "cell"; fuelCell.hazardClass = "normal";
			fuelCell.hydrogenStoreId = "cellH2"; fuelCell.oxygenStoreId = "cellO2";
			fuelCell.waterStoreId = "cellWater";
			fuelCell.thermalClass = "moderate"; fuelCell.heatSinkId = "sink";
			fuelCell.ratedOutputKW = 1.0; fuelCell.commandedOutputKW = 0.0;
			core::MaintenanceJobSettings job;
			job.id = "service"; job.installationId = "idleGen";
			job.firstOccurrenceSeconds = 0.0; job.durationSeconds = 10.0;
			core::MaintenanceRequirementSettings spare;
			spare.storeId = "spares"; spare.quantity = 1.0; spare.consumed = true;
			job.requirements = { spare };
			job.wasteStoreId = "usedParts";
			core::ResourceStoreSettings diesel;
			diesel.id = "diesel"; diesel.unit = "kWh";
			diesel.capacity = 100.0; diesel.startingQualified = 50.0;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				diesel,
				plain("feedWater", 100.0, 50.0),
				plain("o2", 100.0, 1.0), plain("h2", 100.0, 1.0),
				plain("cellH2", 100.0, 10.0), plain("cellO2", 100.0, 10.0),
				plain("cellWater", 100.0, 0.0),
				plain("spares", 10.0, 5.0), plain("usedParts", 10.0, 0.0) };
			settings.thermalReservoirs = { sink, genSink };
			settings.generators = { idleGen };
			settings.electrolyzers = { electrolyzer };
			settings.fuelCells = { fuelCell };
			settings.maintenanceJobs = { job };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const requireProcess = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && entry->processConsumptionPossible,
					"a store feeding an unprojected consuming pathway is not flagged");
				require(!entry->biologyConsumptionPossible,
					"a process-feed store carries a biology consumption flag"); };
			requireProcess("feedWater");
			requireProcess("cellH2");
			requireProcess("cellO2");
			requireProcess("spares");
			// Product destinations and the uncommanded manual generator's
			// fuel store carry no omitted drain.
			auto const requireClean = [&](char const* id) {
				auto const* entry = findForecast(snap.forecast.stores, id);
				require(entry != nullptr && !entry->processConsumptionPossible
					&& !entry->biologyConsumptionPossible,
					"a store with no omitted consuming pathway carries a consumption flag"); };
			requireClean("diesel");
			requireClean("o2");
			requireClean("h2");
			requireClean("cellWater");
			requireClean("usedParts");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "consumption-flags live run");
			require(world.getStationSystemsSnapshot().healthy
				&& world.getStationSystemsDiagnostics().empty(),
				"the consumption-flag scenario produced a Station diagnostic");
		}
		// Scenario N: exactly-timely resupply (#595). The store runs dry
		// at the very instant the delivery lands; because arrivals run
		// ahead of the tick's drains, that delivery prevents the t=10
		// shortage and the first projected shortfall is the later one.
		// The arrival must classify as a rescue even though the drawable
		// immediately before it is zero - the old drawable-positive rule
		// called it late while the same forecast put the onset at t=110.
		{
			core::World world("ForecastExactResupply", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings exact;
			exact.id = "exact"; exact.storeId = "A"; exact.unit = "kg";
			exact.quantity = 100.0; exact.firstOccurrenceSeconds = 10.0;
			settings.deliveries = { exact };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && snap.forecast.asOfSeconds == 0.0,
				"the exactly-timely forecast was not read from the paused t=0 state");
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall
				&& a->basis == core::StationForecastBasis::Shortfall,
				"the exactly-timely resupply store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 110.0, 1e-5),
				"the exactly-timely delivery did not shift the shortfall onset past itself");
			require(a->arrivals.size() == 1, "the exactly-timely delivery is missing");
			if (a != nullptr && a->arrivals.size() == 1)
			{
				auto const& arrival = a->arrivals[0];
				require(arrival.deliveryId == "exact"
					&& approx(arrival.arrivalSeconds, 10.0),
					"the exactly-timely arrival lost its schedule");
				require(approx(arrival.projectedDrawableBefore, 0.0, 1e-9),
					"the exactly-timely arrival did not see a zero drawable");
				require(approx(arrival.acceptedQuantity, 100.0)
					&& approx(arrival.rejectedQuantity, 0.0),
					"the exactly-timely delivery was not fully accepted");
				require(arrival.arrivesQualified && arrival.rescuesStore,
					"an exactly-timely zero-drawable arrival was not a rescue");
			}
			require(approx(a->unmetOutflowTotal,
				core::StationForecastHorizonSeconds - 110.0, 1e-2),
				"the drain after the exactly-timely rescue is not reported as uncovered");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the exactly-timely resupply projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 110.0, 1e-3),
				"the receiving store did not collect the rescued through-flow total");
		}
		// Scenario O: genuinely late and multiple deliveries after a
		// previous depletion (#595). The store runs dry at t=10; a 100 kg
		// delivery lands at t=20 and a 50 kg delivery at t=60. Neither is
		// a rescue: the t=20 arrival lands after the shortfall with a
		// zero drawable, and the t=60 arrival - despite a positive 60 kg
		// drawable restored by the earlier delivery - still lands after
		// the shortage that already occurred at t=10.
		{
			core::World world("ForecastLateDeliveries", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 1000.0, 10.0),
				chainStores("B", 1.0e6, 0.0) };
			settings.transfers = { chainTransfer("AB", "A", "B", 1.0, 1) };
			core::ResourceDeliverySettings late;
			late.id = "late"; late.storeId = "A"; late.unit = "kg";
			late.quantity = 100.0; late.firstOccurrenceSeconds = 20.0;
			core::ResourceDeliverySettings later;
			later.id = "later"; later.storeId = "A"; later.unit = "kg";
			later.quantity = 50.0; later.firstOccurrenceSeconds = 60.0;
			settings.deliveries = { late, later };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			auto const* a = findForecast(snap.forecast.stores, "A");
			require(a != nullptr && a->hasShortfall,
				"the late-delivery store did not forecast a shortfall");
			require(approx(a->shortfallOnsetSeconds, 10.0, 1e-5),
				"the shortfall onset moved even though every arrival is later");
			require(a->arrivals.size() == 2,
				"the late deliveries are not both in the forecast");
			if (a != nullptr && a->arrivals.size() == 2)
			{
				auto const& first = a->arrivals[0];
				require(first.deliveryId == "late"
					&& approx(first.arrivalSeconds, 20.0)
					&& approx(first.projectedDrawableBefore, 0.0, 1e-9)
					&& approx(first.acceptedQuantity, 100.0)
					&& !first.rescuesStore,
					"the genuinely late t=20 arrival did not read as after-the-shortfall");
				auto const& second = a->arrivals[1];
				require(second.deliveryId == "later"
					&& approx(second.arrivalSeconds, 60.0)
					&& approx(second.projectedDrawableBefore, 60.0, 1e-3)
					&& approx(second.acceptedQuantity, 50.0)
					&& !second.rescuesStore,
					"a delivery after a previous depletion read as a rescue on its positive drawable");
			}
			require(approx(a->unmetOutflowTotal,
				10.0 + (core::StationForecastHorizonSeconds - 170.0), 1e-2),
				"the blocked drain around the late deliveries is not fully reported");
			require(std::abs(a->conservationResidual)
				<= core::StationBalanceTolerance * 1e6,
				"the late-delivery projection does not conserve");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(b != nullptr
				&& approx(b->projectedDrawableAtHorizon, 160.0, 1e-3),
				"the receiving store did not collect the late-delivery through-flow total");
		}
		// Scenario P: equipment-to-transfer through-flow at a dry
		// intermediate store (#605), the ticket fixture. A producer
		// delivers 1 kg/s into a dry 1 kg buffer and a transfer drains
		// the buffer at 1 kg/s. The live station flows straight through;
		// the forecast gated the transfer on the inventory at the
		// segment start and the equipment on the destination at the
		// segment start, so the solve alternated fill/drain half-duty
		// phases: it burned the whole segment cap, invented a buffer
		// shortage and delivered only a fraction of the through-flow.
		// The solve now settles every pathway together: no shortage, no
		// truncation, a segment count bounded well below the cap and
		// independent of the horizon, full-horizon throughput at the
		// sink, closed conservation, and agreement with the live
		// trajectory.
		{
			core::World world("ForecastEquipmentThroughDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("buffer", 1.0, 0.0),
				chainStores("sink", 100000.0, 0.0) };
			core::EquipmentLoadSettings producer;
			producer.id = "producer";
			producer.electricalDemandKW = 1.0;
			producer.thermalClass = "warm";
			producer.materialExchanges = {{"buffer", 1.0, false}};
			settings.equipmentLoads = {producer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			settings.transfers = {
				chainTransfer("drain", "buffer", "sink", 1.0, 0) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(snap.forecast.available && !snap.forecast.truncated,
				"the equipment-to-transfer through-flow truncated the forecast");
			require(snap.forecast.segmentsComputed <= 2u,
				"a constant equipment through-flow needed more than a couple of segments");
			auto const* buffer = findForecast(snap.forecast.stores, "buffer");
			auto const* sink = findForecast(snap.forecast.stores, "sink");
			require(buffer != nullptr && sink != nullptr,
				"an equipment-through-flow store is missing from the forecast");
			require(buffer != nullptr && !buffer->hasShortfall
				&& approx(buffer->netRateAtStart, 0.0)
				&& approx(buffer->unmetOutflowTotal, 0.0),
				"the equipment-fed dry intermediate invented a shortage");
			require(sink != nullptr
				&& approx(sink->netRateAtStart, 1.0)
				&& approx(sink->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the sink did not receive the full horizon of equipment through-flow");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the equipment-through-flow projection does not conserve");
			// Agreement with the live trajectory: the producer adds
			// 1/60 kg per tick and the transfer moves it out the same
			// tick, leaving the buffer empty.
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "equipment through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "buffer")->currentQualified, 0.0, 1e-9)
				&& approx(findStore(live.stores, "sink")->currentQualified, 20.0, 1e-3),
				"the live equipment-to-transfer chain did not flow straight through");
			auto const f2 = live.forecast;
			require(!f2.truncated && f2.segmentsComputed <= 2u,
				"the equipment through-flow truncated after the live run");
			require(!findForecast(f2.stores, "buffer")->hasShortfall
				&& approx(findForecast(f2.stores, "buffer")->unmetOutflowTotal, 0.0),
				"the equipment-fed intermediate still invents demand after the live run");
			require(approx(findForecast(f2.stores, "sink")->projectedDrawableAtHorizon,
				86420.0, 1e-3),
				"the re-forecast did not continue the equipment through-flow trajectory");
			for (auto const& entry : f2.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the post-run equipment-through-flow projection does not conserve");
		}
		// Scenario Q: the dual boundary - a full intermediate store
		// supplied by a transfer and drained by equipment consumption
		// (#605). The incoming leg must not stop while the equipment
		// drains: the through-flow is symmetric with the dry case and
		// the intermediate holds steady at capacity for the horizon.
		{
			core::World world("ForecastEquipmentThroughFull", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("source", 1.0e6, 100000.0),
				chainStores("mid", 10.0, 10.0),
				chainStores("product", 1.0e6, 0.0) };
			settings.transfers = {
				chainTransfer("fill", "source", "mid", 1.0, 1) };
			core::EquipmentLoadSettings drainer;
			drainer.id = "drainer";
			drainer.electricalDemandKW = 1.0;
			drainer.thermalClass = "warm";
			drainer.materialExchanges = {
				{"mid", 1.0, true}, {"product", 1.0, false} };
			settings.equipmentLoads = {drainer};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"a full-intermediate equipment through-flow chain truncated the forecast");
			auto const* mid = findForecast(snap.forecast.stores, "mid");
			auto const* source = findForecast(snap.forecast.stores, "source");
			auto const* product = findForecast(snap.forecast.stores, "product");
			require(mid != nullptr && !mid->hasShortfall
				&& approx(mid->netRateAtStart, 0.0)
				&& approx(mid->unmetOutflowTotal, 0.0)
				&& approx(mid->projectedDrawableAtHorizon, 10.0, 1e-3),
				"the full intermediate did not hold steady under equipment through-flow");
			require(source != nullptr
				&& approx(source->netRateAtStart, -1.0)
				&& approx(source->projectedDrawableAtHorizon, 13600.0, 1e-3),
				"the transfer source did not project the full-horizon through-flow drain");
			require(product != nullptr
				&& approx(product->projectedDrawableAtHorizon, 86400.0, 1e-3),
				"the equipment product store did not receive the full horizon");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the full-intermediate equipment through-flow does not conserve");
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "full-intermediate equipment live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "mid")->currentQualified, 10.0, 1e-3)
				&& approx(findStore(live.stores, "product")->currentQualified, 20.0, 1e-3),
				"the live full-intermediate equipment chain did not hold the store steady");
			require(!findForecast(live.forecast.stores, "mid")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "mid")->unmetOutflowTotal, 0.0),
				"the full intermediate invents unmet demand after the live run");
		}
		// Scenario R: shared-feed competition at a dry store (#605).
		// One producer feeds 1 kg/s into a dry store and two equipment
		// loads each want 1 kg/s from it. Only one load can run: the
		// other stops whole and its demand stays uncovered - the
		// forecast must never promise the same kilogram twice.
		{
			core::World world("ForecastSharedFeed", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("feed", 1.0, 0.0),
				chainStores("out1", 1.0e6, 0.0),
				chainStores("out2", 1.0e6, 0.0) };
			core::EquipmentLoadSettings supplier;
			supplier.id = "supplier";
			supplier.electricalDemandKW = 1.0;
			supplier.thermalClass = "warm";
			supplier.materialExchanges = {{"feed", 1.0, false}};
			core::EquipmentLoadSettings consumer1;
			consumer1.id = "consumer1";
			consumer1.electricalDemandKW = 1.0;
			consumer1.thermalClass = "warm";
			consumer1.materialExchanges = {
				{"feed", 1.0, true}, {"out1", 1.0, false} };
			core::EquipmentLoadSettings consumer2;
			consumer2.id = "consumer2";
			consumer2.electricalDemandKW = 1.0;
			consumer2.thermalClass = "warm";
			consumer2.materialExchanges = {
				{"feed", 1.0, true}, {"out2", 1.0, false} };
			settings.equipmentLoads = {supplier, consumer1, consumer2};
			settings.solarArrays = {{"solar", 100.0, {}}};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the shared-feed competition truncated the forecast");
			auto const* feed = findForecast(snap.forecast.stores, "feed");
			require(feed != nullptr && feed->hasShortfall
				&& approx(feed->shortfallOnsetSeconds, 0.0),
				"the over-subscribed dry feed did not read as short from the start");
			require(approx(feed->unmetOutflowTotal,
				core::StationForecastHorizonSeconds, 1e-2),
				"the losing consumer's demand is not fully reported as uncovered");
			// Exactly one consumer runs: one output store collects the
			// full horizon of through-flow and the other stays empty.
			auto const* out1 = findForecast(snap.forecast.stores, "out1");
			auto const* out2 = findForecast(snap.forecast.stores, "out2");
			require(out1 != nullptr && out2 != nullptr, "a consumer output is missing");
			if (out1 != nullptr && out2 != nullptr)
			{
				auto const one = out1->projectedDrawableAtHorizon;
				auto const other = out2->projectedDrawableAtHorizon;
				require((approx(one, 86400.0, 1e-3) && approx(other, 0.0, 1e-9))
					|| (approx(other, 86400.0, 1e-3) && approx(one, 0.0, 1e-9)),
					"the shared feed did not run exactly one consumer at full rate");
			}
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the shared-feed competition does not conserve");
		}
		// Scenario S: crew metabolism through-flow at dry food and
		// water stores (#605). The crew's food and water stores start
		// empty but transfers cover the draws concurrently, so the
		// metabolism runs at full rate for the whole horizon: the
		// waste streams accumulate at the full crew rate and the dry
		// food stores are a covered pass-through, not a shortage.
		{
			core::World world("ForecastCrewThroughFlow", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::ResourceCompositionSettings foodComposition;
			foodComposition.moisture = 0.8;
			foodComposition.nutrientNitrogen = 0.05;
			foodComposition.phosphorus = 0.01;
			foodComposition.potassium = 0.02;
			foodComposition.otherMinerals = 0.02;
			foodComposition.energyKJPerKgDry = 17000.0;
			core::ResourceCompositionSettings waterComposition;
			waterComposition.moisture = 0.999;
			waterComposition.otherMinerals = 0.001;
			core::ResourceCompositionSettings urineComposition;
			urineComposition.moisture = 0.95;
			urineComposition.nutrientNitrogen = 0.1;
			urineComposition.phosphorus = 0.02;
			urineComposition.potassium = 0.05;
			urineComposition.otherMinerals = 0.05;
			urineComposition.energyKJPerKgDry = 500.0;
			core::ResourceCompositionSettings wasteComposition;
			wasteComposition.moisture = 0.5;
			wasteComposition.energyKJPerKgDry = 10000.0;
			auto const compound = [](std::string const& id, double capacity,
				double starting,
				std::optional<core::ResourceCompositionSettings> composition)
			{
				core::ResourceStoreSettings store;
				store.id = id; store.unit = "kg";
				store.capacity = capacity; store.startingQualified = starting;
				store.composition = std::move(composition);
				return store;
			};
			core::AtmosphereSettings habitat;
			habitat.id = "habitat"; habitat.hazardClass = "normal";
			habitat.volumeM3 = 100.0;
			habitat.startingOxygenKg = 20.0; habitat.startingNitrogenKg = 80.0;
			habitat.startingTemperatureC = 20.0;
			habitat.thermalCapacityKJPerK = 10000.0;
			habitat.minPressureKPa = 1.0; habitat.maxPressureKPa = 200.0;
			core::CrewSettings crew;
			crew.id = "crew"; crew.atmosphereId = "habitat";
			crew.crewCount = 100;
			crew.activityProfile.constantLevel = 1.0;
			crew.foodStoreId = "food"; crew.waterStoreId = "water";
			crew.urineStoreId = "urine"; crew.organicWasteStoreId = "organicWaste";
			crew.foodPerCrewPerS = 0.001; crew.waterPerCrewPerS = 0.002;
			crew.oxygenPerCrewKgPerS = 0.001; crew.co2PerCrewKgPerS = 0.0011;
			crew.moisturePerCrewKgPerS = 0.0005;
			crew.urinePerCrewPerS = 0.0015; crew.organicWastePerCrewPerS = 0.0002;
			crew.metabolicHeatPerCrewKW = 0.1;
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				compound("bulkFood", 1.0e6, 100000.0, foodComposition),
				compound("bulkWater", 1.0e6, 100000.0, waterComposition),
				compound("food", 1.0, 0.0, foodComposition),
				compound("water", 1.0, 0.0, waterComposition),
				compound("urine", 1.0e6, 0.0, urineComposition),
				compound("organicWaste", 1.0e6, 0.0, wasteComposition) };
			settings.atmospheres = { habitat };
			settings.crews = { crew };
			settings.transfers = {
				chainTransfer("foodFill", "bulkFood", "food", 0.1, 1),
				chainTransfer("waterFill", "bulkWater", "water", 0.2, 1) };
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the crew through-flow forecast truncated or churned segments");
			auto const* food = findForecast(snap.forecast.stores, "food");
			auto const* water = findForecast(snap.forecast.stores, "water");
			require(food != nullptr && !food->hasShortfall
				&& approx(food->netRateAtStart, 0.0)
				&& approx(food->unmetOutflowTotal, 0.0),
				"the covered dry food store invented a crew food shortage");
			require(water != nullptr && !water->hasShortfall
				&& approx(water->netRateAtStart, 0.0)
				&& approx(water->unmetOutflowTotal, 0.0),
				"the covered dry water store invented a crew water shortage");
			auto const* urine = findForecast(snap.forecast.stores, "urine");
			auto const* waste = findForecast(snap.forecast.stores, "organicWaste");
			require(urine != nullptr
				&& approx(urine->netRateAtStart, 0.15)
				&& approx(urine->projectedDrawableAtHorizon, 12960.0, 1e-2),
				"the covered crew did not project full-rate urine");
			require(waste != nullptr
				&& approx(waste->netRateAtStart, 0.02)
				&& approx(waste->projectedDrawableAtHorizon, 1728.0, 1e-2),
				"the covered crew did not project full-rate organic waste");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the crew through-flow projection does not conserve");
			// The live station agrees: the crew eats what the transfers
			// deliver each tick and the waste streams accumulate at the
			// full crew rate (minus the first tick the empty food store
			// costs the metabolism).
			world.resumeSimulation();
			runOrDiagnose(world, 1200, "crew through-flow live run");
			auto const live = world.getStationSystemsSnapshot();
			require(approx(findStore(live.stores, "food")->currentQualified, 0.1 / 60.0, 1e-9)
				&& approx(findStore(live.stores, "urine")->currentQualified,
					0.15 * (1199.0 / 60.0), 1e-2),
				"the live crew did not run on the delivered through-flow");
			require(!findForecast(live.forecast.stores, "food")->hasShortfall
				&& approx(findForecast(live.forecast.stores, "food")->unmetOutflowTotal, 0.0),
				"the covered crew invents unmet demand after the live run");
		}
		// Scenario T: a closed equipment cycle between two dry stores
		// cannot bootstrap itself (#605). Load X consumes A and
		// produces B; load Y consumes B and produces A; both stores
		// start empty with no external supply. Neither load is
		// supported: both stop whole, their feed demand stays
		// uncovered, and no phantom material appears anywhere.
		{
			core::World world("ForecastCycleDry", 2, 2);
			world.pauseSimulation(); world.finishBuild();
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.stores = {
				chainStores("A", 10.0, 0.0),
				chainStores("B", 10.0, 0.0) };
			core::EquipmentLoadSettings x;
			x.id = "x";
			x.thermalClass = "warm";
			x.materialExchanges = {{"A", 1.0, true}, {"B", 1.0, false}};
			core::EquipmentLoadSettings y;
			y.id = "y";
			y.thermalClass = "warm";
			y.materialExchanges = {{"B", 1.0, true}, {"A", 1.0, false}};
			settings.equipmentLoads = {x, y};
			require(world.configureStationSystems(settings, false, &diagnostic),
				diagnostic.c_str());
			auto const snap = world.getStationSystemsSnapshot();
			require(!snap.forecast.truncated && snap.forecast.segmentsComputed <= 2u,
				"the unsupported equipment cycle churned the segment budget");
			auto const* a = findForecast(snap.forecast.stores, "A");
			auto const* b = findForecast(snap.forecast.stores, "B");
			require(a != nullptr && a->hasShortfall
				&& approx(a->shortfallOnsetSeconds, 0.0)
				&& approx(a->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(a->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store A instead of reporting its demand uncovered");
			require(b != nullptr && b->hasShortfall
				&& approx(b->shortfallOnsetSeconds, 0.0)
				&& approx(b->projectedDrawableAtHorizon, 0.0, 1e-9)
				&& approx(b->unmetOutflowTotal,
					core::StationForecastHorizonSeconds, 1e-2),
				"the unsupported cycle revived store B instead of reporting its demand uncovered");
			for (auto const& entry : snap.forecast.stores)
				require(std::abs(entry.conservationResidual)
					<= core::StationBalanceTolerance * 1e6,
					"the unsupported equipment cycle does not conserve");
		}
	}

}

void registerStationForecastChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), { { "forecast", forecast } });
}
