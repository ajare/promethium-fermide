#pragma once
#include "core/StationSystems.h"

namespace smoke::station
{
inline core::StationSystemsSettings cropObservationFixture(int scenario = 0)
{
	core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
	core::ResourceCompositionSettings composition;
	composition.nutrientNitrogen = 0.5; composition.phosphorus = 0.1;
	composition.potassium = 0.1; composition.otherMinerals = 0.1; composition.energyKJPerKgDry = 3600;
	for (auto id : {"water", "N", "P", "K", "minerals", "food", "waste"}) {
		core::ResourceStoreSettings stock; stock.id = id; stock.unit = "kg"; stock.capacity = 1000;
		stock.startingQualified = s.stores.size() < 5 ? 100 : 0;
		stock.materialId = stock.id == "water" ? "water" : "compound";
		if (stock.id != "water") stock.composition = composition;
		s.stores.push_back(stock);
	}
	for (size_t i = 1; i <= 4; ++i) {
		core::ResourceCompositionSettings pure;
		if (i == 1) pure.nutrientNitrogen = 1;
		if (i == 2) pure.phosphorus = 1;
		if (i == 3) pure.potassium = 1;
		if (i == 4) pure.otherMinerals = 1;
		s.stores[i].composition = pure;
	}
	core::AtmosphereSettings air; air.id = "habitat"; air.hazardClass = "normal"; air.volumeM3 = 100;
	air.startingOxygenKg = 40; air.startingNitrogenKg = 80; air.startingCarbonDioxideKg = 5;
	air.startingWaterVapourKg = 1; air.startingTemperatureC = 22; air.thermalCapacityKJPerK = 20000;
	air.minPressureKPa = 1; air.maxPressureKPa = 200; s.atmospheres = {air};
	core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
	core::CropPopulationSettings c; c.id = "canopy"; c.atmosphereId = "habitat";
	c.waterStoreId = "water"; c.nitrogenStoreId = "N"; c.phosphorusStoreId = "P";
	c.potassiumStoreId = "K"; c.mineralsStoreId = "minerals"; c.composition = composition;
	c.initialBiomassKg = 1; c.growingAreaM2 = 10; c.lampRatedKW = 3.6;
	c.circulationPowerKW = 0.1; c.growthYieldKgPerKWh = 0.5; c.transpirationKgPerKgGrowth = 0.2;
	c.respirationPerHour = scenario >= 2 ? 600 : 0;
	c.illuminationProfile.constantLevel = scenario >= 2 ? 0 : 1;
	c.foodStoreId = "food"; c.wasteStoreId = scenario == 3 ? "" : "waste";
	c.harvestTriggerKg = scenario == 1 ? 1 : 0; c.harvestFraction = 0.5;
	s.cropPopulations = {c}; return s;
}
}
