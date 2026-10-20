#pragma once
#include "core/StationSystems.h"

namespace smoke::station
{
inline core::StationSystemsSettings scheduledObservationFixture(int treatmentMode = 0)
{
	core::StationSystemsSettings s; s.enabled = true; s.observationIntervalSeconds = 0.2f;
	auto stock = [&](std::string id, std::string unit, double capacity, double qualified, double held,
		std::string material, std::string hazard = "normal") {
		core::ResourceStoreSettings x; x.id = id; x.unit = unit; x.capacity = capacity;
		x.startingQualified = qualified; x.startingUnqualified = held; x.materialId = material; x.hazardClass = hazard;
		s.stores.push_back(x);
	};
	stock("arrival", "g", 5, 0, 0, "water");
	stock("hazard-waste", "kg", 100, 2, 4, "residue", "hazardous");
	stock("parts", "kg", 100, 2.5, 0, "parts");
	stock("tools", "kg", 100, 1, 0, "tool");
	stock("spent", "kg", 100, 0, 0, "parts");
	stock("fuel", "kWh", 100, 100, 0, "fuel");
	s.deliveries = {{"recurring", "arrival", "g", 2, 0, 0.1}, {"boundary", "arrival", "g", 1, 0.2}};
	s.wasteEjections = {{"eject", "hazard-waste", "kg", 3, 0, 0.1}};
	core::ThermalReservoirSettings hot; hot.id = "hot"; hot.thermalClass = "generation";
	hot.heatCapacityKJPerK = 100000; hot.startingTemperatureC = 20; hot.minTemperatureC = -50; hot.maxTemperatureC = 200;
	s.thermalReservoirs = {hot};
	core::GeneratorSettings g; g.id = "gen"; g.fuelStoreId = "fuel"; g.heatSinkId = "hot"; g.thermalClass = "generation";
	g.ratedOutputKW = 10; g.commandedOutputKW = 0; g.efficiency = 0.5; g.maxRampKWPerSecond = 100; g.maxSinkTemperatureC = 90;
	s.generators = {g};
	core::MaintenanceJobSettings job; job.id = "service"; job.installationId = "gen";
	job.firstOccurrenceSeconds = 0; job.durationSeconds = 0.05; job.recurrenceSeconds = 0.1;
	job.requirements = {{"tools", 1, false}, {"parts", 1, true}}; job.wasteStoreId = "spent";
	s.maintenanceJobs = {job};
	if (treatmentMode) {
		stock("raw", "kg", 100, 10, 0, "water", "hazardous");
		stock("treated", "kg", 100, 0, 0, "water", "hazardous");
		stock("concentrate", "kg", 100, 0, 0, "brine", "hazardous");
		stock("kit", "kg", 100, 10, 0, "kit", "hazardous");
		core::SolarArraySettings sun; sun.id = "sun"; sun.capacityKW = treatmentMode == 2 ? 100 : 1; s.solarArrays = {sun};
		core::WaterTreatmentSettings t; t.id = "treatment"; t.hazardClass = "hazardous";
		t.sourceStoreId = "raw"; t.treatedStoreId = "treated"; t.concentrateStoreId = "concentrate";
		t.consumableStoreId = "kit";
		t.throughputKgPerS = 1; t.recoveryRatio = 1; t.specificEnergyKWhPerKg = 0.001;
		s.waterTreatments = {t};
		s.wasteEjections.push_back({"treated-ejection", "treated", "kg", 1, 0.2, 0.1});
	}
	return s;
}
}
