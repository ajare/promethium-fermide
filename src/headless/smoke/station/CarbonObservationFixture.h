#pragma once
#include "core/StationSystems.h"

namespace smoke::station
{
inline core::StationSystemsSettings carbonObservationFixture()
{
	core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
	core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
	for (auto id : {"co2", "hydrogen", "methane", "water", "carbon", "cell-h2", "cell-o2", "cell-water", "breathing-oxygen"}) {
		core::ResourceStoreSettings stock; stock.id = id; stock.unit = "kg";
		stock.capacity = 100; stock.startingQualified = 10;
		if (stock.id == "breathing-oxygen") stock.materialId = "oxygen";
		s.stores.push_back(stock);
	}
	core::ThermalReservoirSettings sink; sink.id = "sink"; sink.thermalClass = "moderate";
	sink.heatCapacityKJPerK = 100000; sink.startingTemperatureC = 20; s.thermalReservoirs = {sink};
	core::SabatierReactorSettings r; r.id = "reactor"; r.hazardClass = "normal";
	r.co2StoreId = "co2"; r.hydrogenStoreId = "hydrogen"; r.methaneStoreId = "methane";
	r.waterStoreId = "water"; r.heatSinkId = "sink"; r.thermalClass = "moderate"; r.rateKgPerS = 0.001;
	s.sabatierReactors = {r};
	core::MethaneProcessorSettings p; p.id = "processor"; p.hazardClass = "normal";
	p.methaneStoreId = "methane"; p.hydrogenStoreId = "hydrogen"; p.carbonStoreId = "carbon";
	p.heatSinkId = "sink"; p.thermalClass = "moderate"; p.rateKgPerS = 0.001; s.methaneProcessors = {p};
	core::FuelCellSettings c; c.id = "cell"; c.hazardClass = "normal";
	c.hydrogenStoreId = "cell-h2"; c.oxygenStoreId = "cell-o2"; c.waterStoreId = "cell-water";
	c.heatSinkId = "sink"; c.thermalClass = "moderate"; c.ratedOutputKW = 5; c.commandedOutputKW = 2;
	s.fuelCells = {c};
	return s;
}
}
