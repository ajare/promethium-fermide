#pragma once
#include "core/StationSystems.h"

namespace smoke::station
{
inline core::StationSystemsSettings waterGasObservationFixture()
{
	core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.1f;
	core::SolarArraySettings solar; solar.id = "sun"; solar.capacityKW = 100; s.solarArrays = {solar};
	for (auto id : {"raw", "treated", "concentrate", "kit", "oxygen", "hydrogen", "co2"}) {
		core::ResourceStoreSettings stock; stock.id = id; stock.unit = "kg";
		stock.capacity = 100; stock.startingQualified = 10;
		stock.materialId = std::string(id) == "kit" ? "sorbent" : std::string(id) == "concentrate" ? "brine" : "";
		s.stores.push_back(stock);
	}
	core::ThermalReservoirSettings r; r.id = "sink"; r.thermalClass = "moderate";
	r.heatCapacityKJPerK = 100000; r.startingTemperatureC = 5; s.thermalReservoirs = {r};
	core::CoolantLoopSettings l; l.id = "loop"; l.reservoirId = "sink";
	l.coolantCapacityKg = l.startingCoolantKg = 100; l.specificHeatKJPerKgK = 4;
	l.startingTemperatureC = 5; l.ratedFlowKgPerS = 1; l.pumpPowerKW = 0.1;
	l.exchangerConductanceKWPerK = 10; s.coolantLoops = {l};
	core::AtmosphereSettings a; a.id = "air"; a.volumeM3 = 100;
	a.startingOxygenKg = 20; a.startingNitrogenKg = 80; a.startingCarbonDioxideKg = 2; a.startingWaterVapourKg = 2;
	a.startingTemperatureC = 25; a.thermalCapacityKJPerK = 10000;
	a.minPressureKPa = 1; a.maxPressureKPa = 200; s.atmospheres = {a};
	core::WaterCollectorSettings c; c.id = "collector"; c.hazardClass = "normal"; c.atmosphereId = "air";
	c.coolingLoopId = "loop"; c.thermalClass = "moderate"; c.destStoreId = "raw";
	c.maxRateKgPerS = 0.01; c.targetRelativeHumidity = 0.1; s.waterCollectors = {c};
	core::WaterTreatmentSettings t; t.id = "treatment"; t.hazardClass = "normal";
	t.sourceStoreId = "raw"; t.treatedStoreId = "treated"; t.concentrateStoreId = "concentrate";
	t.consumableStoreId = "kit"; t.throughputKgPerS = 0.01; t.specificEnergyKWhPerKg = 0.1;
	s.waterTreatments = {t};
	core::ElectrolyzerSettings e; e.id = "cell"; e.hazardClass = "normal"; e.waterStoreId = "treated";
	e.oxygenStoreId = "oxygen"; e.hydrogenStoreId = "hydrogen"; e.heatSinkId = "sink";
	e.thermalClass = "moderate"; e.rateKgPerS = 0.001; s.electrolyzers = {e};
	core::Co2CaptureSettings capture; capture.id = "capture"; capture.hazardClass = "normal";
	capture.atmosphereId = "air"; capture.captureStoreId = "co2"; capture.consumableStoreId = "kit";
	capture.consumablePerKg = 0.01; capture.maxRateKgPerS = 0.001; s.co2Captures = {capture};
	return s;
}
}
