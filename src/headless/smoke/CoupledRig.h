#pragma once
// #421: the coupled station rig - one authored World where the carbon loop,
// the microbial nutrient loop, the biology and the crew all share the same
// stores, compartments and power pool, so a change in one stage visibly
// propagates through the others. Shared by the station smoke checks and the
// editor live-view check so both drive the exact same coupled scenario.
//
// The topology (all normal hazard class by default):
//
//   crew (hab) --CO2--> scrubHab --> [co2] ----------------------\
//   crops (greenhouse) --respire CO2--> scrubGreen --> [co2]      |
//   digester --CO2 product---------------------------------------> |
//                          [co2] + [h2] --> sabatier --> [methane] + [rawWater]
//   pyrolysis: [methane] --> [h2] (recycled feed) + [carbonWaste] --> ejection
//   treatment: [rawWater] --> [fresh] + [concentrate]
//   electrolyzer: [fresh] --> O2 into hab + [h2]
//   digester: [organicWaste] + [oxygen] + [fresh] + [digesterMedia]
//             --> [nutrients] + [rawWater] + [co2] + [residue]
//   fertPump: [nutrients] --> [fertN] (crop bound-nitrogen uptake)
//   crops/algae: light + CO2 + [fresh] + nutrients --> living biomass
//             --> harvest --> [food] --> crew
//
// Every rate is provisional model arithmetic chosen so the nominal rig runs
// balanced for the test durations below; the stress scenarios deliberately
// break one stage and watch the effect travel.
#include "core/StationSystems.h"

#include <string>
#include <vector>

namespace coupled_rig
{
	inline core::ResourceCompositionSettings cropComposition()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.0;
		composition.nutrientNitrogen = 0.05;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.03;
		composition.otherMinerals = 0.05;
		composition.energyKJPerKgDry = 1700.0;
		return composition;
	}
	inline core::ResourceCompositionSettings drinkingComposition()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.999;
		composition.otherMinerals = 0.001;
		return composition;
	}
	inline core::ResourceCompositionSettings urineComposition()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.95;
		composition.nutrientNitrogen = 0.1;
		composition.phosphorus = 0.02;
		composition.potassium = 0.05;
		composition.otherMinerals = 0.05;
		composition.energyKJPerKgDry = 500.0;
		return composition;
	}
	// The crew organic-waste feedstock: a wet organic waste whose dry mass
	// carries bound nutrients the digester can recover.
	inline core::ResourceCompositionSettings feedComposition()
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.5;
		composition.nutrientNitrogen = 0.08;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.02;
		composition.otherMinerals = 0.03;
		composition.energyKJPerKgDry = 10000.0;
		return composition;
	}

	inline core::ResourceStoreSettings plainStore(std::string const& id, double capacity,
		double starting, double protectedReserve = 0.0)
	{
		core::ResourceStoreSettings settings;
		settings.id = id;
		settings.unit = "kg";
		settings.capacity = capacity;
		settings.startingQualified = starting;
		settings.protectedReserve = protectedReserve;
		return settings;
	}
	inline core::ResourceStoreSettings compoundStore(std::string const& id, double capacity,
		double starting, core::ResourceCompositionSettings const& composition)
	{
		auto settings = plainStore(id, capacity, starting);
		settings.composition = composition;
		return settings;
	}

	inline core::ProfileSettings constantProfile(double level)
	{
		core::ProfileSettings profile;
		profile.constantLevel = level;
		return profile;
	}

	// The nominal coupled rig. Callers copy and mutate the returned settings
	// to build the stress variants (a depleted store, a disabled stage, a
	// tighter capacity) without touching the nominal baseline.
	inline core::StationSystemsSettings nominal()
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;

		// --- Stores -----------------------------------------------------
		settings.stores = {
			compoundStore("food", 50.0, 20.0, cropComposition()),
			compoundStore("drinking", 100.0, 50.0, drinkingComposition()),
			compoundStore("urine", 50.0, 0.0, urineComposition()),
			compoundStore("organicWaste", 20.0, 5.0, feedComposition()),
			plainStore("co2", 20.0, 5.0),
			plainStore("h2", 10.0, 2.0),
			plainStore("methane", 10.0, 0.0),
			plainStore("rawWater", 50.0, 5.0),
			plainStore("fresh", 50.0, 10.0),
			plainStore("concentrate", 50.0, 0.0),
			plainStore("treatmentMedia", 10.0, 5.0),
			plainStore("captureMedia", 10.0, 5.0),
			plainStore("digesterMedia", 10.0, 5.0),
			plainStore("oxygen", 20.0, 10.0),
			plainStore("fertN", 5.0, 0.5),
			plainStore("fertP", 5.0, 0.5),
			plainStore("fertK", 5.0, 0.5),
			plainStore("fertMin", 5.0, 0.5),
			plainStore("nutrients", 5.0, 0.5),
			plainStore("aFertN", 5.0, 0.5),
			plainStore("aFertP", 5.0, 0.5),
			plainStore("aFertK", 5.0, 0.5),
			plainStore("aFertMin", 5.0, 0.5),
			plainStore("carbonWaste", 10.0, 0.0),
			plainStore("residue", 20.0, 0.0),
		};

		// --- Compartments -----------------------------------------------
		core::AtmosphereSettings hab;
		hab.id = "hab";
		hab.volumeM3 = 200.0;
		hab.startingOxygenKg = 60.0;
		hab.startingNitrogenKg = 200.0;
		hab.startingCarbonDioxideKg = 1.0;
		hab.startingWaterVapourKg = 2.0;
		hab.startingTemperatureC = 22.0;
		hab.thermalCapacityKJPerK = 40000.0;
		hab.minPressureKPa = 1.0;
		hab.maxPressureKPa = 200.0;
		hab.minOxygenPartialPressureKPa = 12.0;
		hab.maxCarbonDioxidePartialPressureKPa = 0.4;
		core::AtmosphereSettings greenhouse;
		greenhouse.id = "greenhouse";
		greenhouse.volumeM3 = 400.0;
		greenhouse.startingOxygenKg = 120.0;
		greenhouse.startingNitrogenKg = 400.0;
		greenhouse.startingCarbonDioxideKg = 3.0;
		greenhouse.startingWaterVapourKg = 2.0;
		greenhouse.startingTemperatureC = 24.0;
		greenhouse.thermalCapacityKJPerK = 60000.0;
		greenhouse.minPressureKPa = 1.0;
		greenhouse.maxPressureKPa = 200.0;
		core::AtmosphereSettings pond;
		pond.id = "pond";
		pond.volumeM3 = 100.0;
		pond.startingOxygenKg = 30.0;
		pond.startingNitrogenKg = 100.0;
		pond.startingCarbonDioxideKg = 2.0;
		pond.startingWaterVapourKg = 1.0;
		pond.startingTemperatureC = 25.0;
		pond.thermalCapacityKJPerK = 30000.0;
		pond.minPressureKPa = 1.0;
		pond.maxPressureKPa = 200.0;
		settings.atmospheres = { hab, greenhouse, pond };

		// --- Crew: the human CO2 source ---------------------------------
		core::CrewSettings crew;
		crew.id = "crew";
		crew.atmosphereId = "hab";
		crew.crewCount = 2;
		crew.activityProfile = constantProfile(1.0);
		crew.foodStoreId = "food";
		crew.waterStoreId = "drinking";
		crew.urineStoreId = "urine";
		crew.organicWasteStoreId = "organicWaste";
		crew.foodPerCrewPerS = 0.002;
		crew.waterPerCrewPerS = 0.0025;
		crew.oxygenPerCrewKgPerS = 0.001;
		crew.co2PerCrewKgPerS = 0.0011;
		crew.moisturePerCrewKgPerS = 0.0005;
		crew.urinePerCrewPerS = 0.0015;
		crew.organicWastePerCrewPerS = 0.0002;
		crew.metabolicHeatPerCrewKW = 0.1;
		settings.crews = { crew };

		// --- Carbon loop -------------------------------------------------
		core::Co2CaptureSettings scrubHab;
		scrubHab.id = "scrubHab";
		scrubHab.atmosphereId = "hab";
		scrubHab.captureStoreId = "co2";
		scrubHab.consumableStoreId = "captureMedia";
		scrubHab.maxRateKgPerS = 0.004;
		scrubHab.specificEnergyKWhPerKg = 0.5;
		scrubHab.consumablePerKg = 0.001;
		scrubHab.priority = 1;
		core::Co2CaptureSettings scrubGreen;
		scrubGreen.id = "scrubGreen";
		scrubGreen.atmosphereId = "greenhouse";
		scrubGreen.captureStoreId = "co2";
		scrubGreen.maxRateKgPerS = 0.004;
		scrubGreen.specificEnergyKWhPerKg = 0.5;
		settings.co2Captures = { scrubHab, scrubGreen };

		core::SabatierReactorSettings sabatier;
		sabatier.id = "sab";
		sabatier.co2StoreId = "co2";
		sabatier.hydrogenStoreId = "h2";
		sabatier.methaneStoreId = "methane";
		sabatier.waterStoreId = "rawWater";
		sabatier.thermalClass = "low";
		sabatier.heatSinkId = "processSink";
		sabatier.rateKgPerS = 0.001;
		sabatier.specificEnergyKWhPerKgCO2 = 1.0;
		sabatier.maxSinkTemperatureC = 60.0;
		sabatier.priority = 1;
		settings.sabatierReactors = { sabatier };

		core::MethaneProcessorSettings pyrolysis;
		pyrolysis.id = "pyro";
		pyrolysis.methaneStoreId = "methane";
		pyrolysis.hydrogenStoreId = "h2";
		pyrolysis.carbonStoreId = "carbonWaste";
		pyrolysis.thermalClass = "low";
		pyrolysis.heatSinkId = "processSink";
		pyrolysis.rateKgPerS = 0.002;
		pyrolysis.specificEnergyKWhPerKgCH4 = 2.0;
		pyrolysis.maxSinkTemperatureC = 60.0;
		pyrolysis.priority = 1;
		settings.methaneProcessors = { pyrolysis };

		// --- Water chain -------------------------------------------------
		core::WaterTreatmentSettings treatment;
		treatment.id = "wtr";
		treatment.sourceStoreId = "rawWater";
		treatment.treatedStoreId = "fresh";
		treatment.concentrateStoreId = "concentrate";
		treatment.consumableStoreId = "treatmentMedia";
		treatment.throughputKgPerS = 0.01;
		treatment.recoveryRatio = 0.9;
		treatment.specificEnergyKWhPerKg = 0.5;
		treatment.consumablePerKg = 0.001;
		settings.waterTreatments = { treatment };

		core::ElectrolyzerSettings electrolyzer;
		electrolyzer.id = "lyzer";
		electrolyzer.waterStoreId = "fresh";
		electrolyzer.oxygenStoreId = ""; // delivered to the hab compartment
		electrolyzer.hydrogenStoreId = "h2";
		electrolyzer.atmosphereId = "hab";
		electrolyzer.thermalClass = "low";
		electrolyzer.heatSinkId = "processSink";
		electrolyzer.rateKgPerS = 0.001;
		electrolyzer.specificEnergyKWhPerKgWater = 4.5;
		electrolyzer.maxSinkTemperatureC = 60.0;
		settings.electrolyzers = { electrolyzer };

		// --- Microbial nutrient loop -------------------------------------
		core::MicrobialProcessorSettings digester;
		digester.id = "dig";
		digester.feedStoreId = "organicWaste";
		digester.oxygenStoreId = "oxygen";
		digester.waterStoreId = "fresh";
		digester.consumableStoreId = "digesterMedia";
		digester.nutrientStoreId = "nutrients";
		digester.recoveredWaterStoreId = "rawWater";
		digester.co2StoreId = "co2";
		digester.residueStoreId = "residue";
		digester.thermalClass = "low";
		digester.heatSinkId = "processSink";
		digester.rateKgPerS = 0.001;
		digester.specificEnergyKWhPerKgFeed = 2.0;
		digester.oxygenPerKgFeedKg = 0.8;
		digester.processWaterPerKgFeedKg = 0.5;
		digester.consumablePerKgFeed = 0.01;
		digester.co2PerKgFeedKg = 0.3;
		digester.nitrogenRecovery = 0.8;
		digester.phosphorusRecovery = 0.8;
		digester.potassiumRecovery = 0.8;
		digester.mineralsRecovery = 0.7;
		digester.waterRecovery = 0.7;
		digester.reactionHeatKJPerKgFeed = 600.0;
		digester.minOperatingTemperatureC = 15.0;
		digester.maxSinkTemperatureC = 45.0;
		settings.microbialProcessors = { digester };

		// --- Biology -----------------------------------------------------
		core::CropPopulationSettings crop;
		crop.id = "canopy";
		crop.atmosphereId = "greenhouse";
		crop.waterStoreId = "fresh";
		crop.nitrogenStoreId = "fertN";
		crop.phosphorusStoreId = "fertP";
		crop.potassiumStoreId = "fertK";
		crop.mineralsStoreId = "fertMin";
		crop.composition = cropComposition();
		crop.growingAreaM2 = 10.0;
		crop.maxBiomassDensityKgPerM2 = 2.0;
		crop.initialBiomassKg = 0.1;
		crop.lampRatedKW = 3.6;
		crop.illuminationProfile = constantProfile(1.0);
		crop.circulationPowerKW = 0.1;
		crop.growthYieldKgPerKWh = 2.0;
		crop.respirationPerHour = 0.5;
		crop.transpirationKgPerKgGrowth = 2.0;
		crop.minGrowingTemperatureC = 10.0;
		crop.maxGrowingTemperatureC = 35.0;
		crop.foodStoreId = "food";
		crop.wasteStoreId = "residue";
		crop.edibleFraction = 0.8;
		crop.harvestTriggerKg = 0.3;
		crop.harvestFraction = 0.5;
		settings.cropPopulations = { crop };

		core::AlgaeCultureSettings algae;
		algae.id = "pondAlgae";
		algae.atmosphereId = "pond";
		algae.waterStoreId = "fresh";
		algae.nitrogenStoreId = "aFertN";
		algae.phosphorusStoreId = "aFertP";
		algae.potassiumStoreId = "aFertK";
		algae.mineralsStoreId = "aFertMin";
		algae.composition = cropComposition();
		algae.cultureVolumeL = 1000.0;
		algae.maxBiomassDensityKgPerL = 0.002;
		algae.initialBiomassKg = 0.1;
		algae.lampRatedKW = 1.2;
		algae.illuminationProfile = constantProfile(1.0);
		algae.circulationPowerKW = 0.05;
		algae.growthYieldKgPerKWh = 2.0;
		algae.respirationPerHour = 0.5;
		algae.evaporationKgPerKgGrowth = 2.0;
		algae.minGrowingTemperatureC = 10.0;
		algae.maxGrowingTemperatureC = 35.0;
		algae.foodStoreId = "food";
		algae.recoveryStoreId = "nutrients";
		algae.wasteStoreId = "residue";
		algae.foodFraction = 0.5;
		algae.recoveryFraction = 0.3;
		algae.harvestTriggerKg = 0.15;
		algae.harvestFraction = 0.5;
		settings.algaeCultures = { algae };

		// --- Power -------------------------------------------------------
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		field.illumination = constantProfile(1.0);
		settings.solarArrays = { field };

		core::BatterySettings bank;
		bank.id = "bank";
		bank.nameplateKWh = 100.0;
		bank.usableKWh = 80.0;
		bank.startingStoredKWh = 80.0;
		bank.maxChargeKW = 10.0;
		bank.maxDischargeKW = 10.0;
		bank.chargeEfficiency = 1.0;
		bank.dischargeEfficiency = 1.0;
		settings.batteries = { bank };

		// --- Thermal -----------------------------------------------------
		core::ThermalReservoirSettings sink;
		sink.id = "processSink";
		sink.thermalClass = "low";
		sink.heatCapacityKJPerK = 10000.0;
		sink.startingTemperatureC = 25.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 200.0;
		settings.thermalReservoirs = { sink };

		// --- Boundary exchanges ------------------------------------------
		core::ResourceTransferSettings fertPump;
		fertPump.id = "fertPump";
		fertPump.sourceId = "nutrients";
		fertPump.destinationId = "fertN";
		fertPump.unit = "kg";
		fertPump.ratePerSecond = 0.0001;
		fertPump.priority = 1;
		core::ResourceTransferSettings fertPumpA;
		fertPumpA.id = "fertPumpA";
		fertPumpA.sourceId = "nutrients";
		fertPumpA.destinationId = "aFertN";
		fertPumpA.unit = "kg";
		fertPumpA.ratePerSecond = 0.00005;
		fertPumpA.priority = 1;
		settings.transfers = { fertPump, fertPumpA };

		core::ResourceDeliverySettings waterDrop;
		waterDrop.id = "waterDrop";
		waterDrop.storeId = "drinking";
		waterDrop.unit = "kg";
		waterDrop.quantity = 50.0;
		waterDrop.firstOccurrenceSeconds = 0.0;
		core::ResourceDeliverySettings oxygenDrop;
		oxygenDrop.id = "oxygenDrop";
		oxygenDrop.storeId = "oxygen";
		oxygenDrop.unit = "kg";
		oxygenDrop.quantity = 10.0;
		oxygenDrop.firstOccurrenceSeconds = 0.0;
		settings.deliveries = { waterDrop, oxygenDrop };

		core::WasteEjectionSettings ejectCarbon;
		ejectCarbon.id = "ejectCarbon";
		ejectCarbon.storeId = "carbonWaste";
		ejectCarbon.unit = "kg";
		ejectCarbon.quantity = 0.01;
		ejectCarbon.firstOccurrenceSeconds = 60.0;
		ejectCarbon.recurrenceSeconds = 60.0;
		settings.wasteEjections = { ejectCarbon };

		return settings;
	}
}
