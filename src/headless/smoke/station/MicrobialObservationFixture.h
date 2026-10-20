#pragma once
#include "core/StationSystems.h"

namespace smoke::station
{
inline core::StationSystemsSettings microbialObservationFixture()
{
	core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
	core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
	for (auto id : {"waste", "oxygen", "water", "media", "nutrients", "recovered-water", "co2", "residue", "stock"}) {
		core::ResourceStoreSettings stock; stock.id = id; stock.unit = "kg";
		stock.capacity = 100; stock.startingQualified = 10;
		if (stock.id == "waste") stock.materialId = "organic-waste";
		if (stock.id == "nutrients") stock.materialId = "recovered-nutrients";
		if (stock.id == "residue") stock.materialId = "microbial-residue";
		if (stock.id == "media") stock.materialId = "culture-medium";
		if (stock.id == "stock") stock.materialId = "replacement-stock";
		s.stores.push_back(stock);
	}
	core::ResourceCompositionSettings feed; feed.moisture = 0.9; feed.nutrientNitrogen = 0.05;
	feed.atmosphericNitrogen = 0.02; feed.phosphorus = 0.02; feed.potassium = 0.02;
	feed.otherMinerals = 0.03; feed.energyKJPerKgDry = 8000;
	s.stores[0].composition = feed;
	core::ThermalReservoirSettings sink; sink.id = "sink"; sink.thermalClass = "low";
	sink.heatCapacityKJPerK = 100000; sink.startingTemperatureC = 20; s.thermalReservoirs = {sink};
	core::MicrobialProcessorSettings p; p.id = "digester"; p.hazardClass = "normal";
	p.feedStoreId = "waste"; p.oxygenStoreId = "oxygen"; p.waterStoreId = "water";
	p.consumableStoreId = "media"; p.stockStoreId = "stock"; p.nutrientStoreId = "nutrients";
	p.recoveredWaterStoreId = "recovered-water"; p.co2StoreId = "co2"; p.residueStoreId = "residue";
	p.heatSinkId = "sink"; p.thermalClass = "low"; p.rateKgPerS = 0.001;
	p.reactionHeatKJPerKgFeed = 600; p.initialCondition = 0.75; p.recoveryPerHour = 1;
	s.microbialProcessors = {p};
	return s;
}
}
