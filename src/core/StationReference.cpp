// #422: the packaged reference station.
//
// This module owns two things. First, the authoritative reader for the
// "stationSystems" document map, extracted from the World document reader
// so the packaged reference station and persisted Worlds are read by the
// exact same code - the reference configuration format and the World
// persistence format can never diverge. Second, the reference station
// document itself: one reconciled, editable configuration with its
// coefficient provenance, provisional assumptions and qualification
// limits, packaged as an application resource and parsed once on demand.
#include "core/StationReference.h"

#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "BundledStationReferenceSource.h"
#include "BundledStationScenariosSource.h"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace core
{
	void readStationSystemsSettingsMap(Serializer& serializer,
		std::uint32_t const version, StationSystemsSettings& stationSettings)
	{
		if (version < 65) throw SerializationException("Station systems require World schema 65");
		serializer.beginMap("stationSystems");
		stationSettings.enabled = serializer.readBool("enabled");
		stationSettings.observationIntervalSeconds = serializer.readFloat("observationIntervalSeconds");
		// Guard ordering: the dispatch mode field is read before the
		// cultures array, so a pre-89 document carrying algae cultures
		// would otherwise be refused with the dispatch schema message
		// and never name the feature it actually predates. Check the
		// algae schema for algae-carrying documents up front so the
		// refusal identifies the right guard (#416 test-hygiene fix
		// for the pre-89 refusal broken by the #415 field order).
		if (version < 89 && serializer.hasField("algaeCultures"))
			throw SerializationException(
				"Station algae cultures require World schema 89");
		// #415: the dispatch controller mode. A pre-91 document
		// carrying it is refused rather than silently dropped; an
		// unknown mode string is refused as a malformed setting.
		if (serializer.hasField("dispatchMode"))
		{
			if (version < 91) throw SerializationException(
				"Station dispatch controller requires World schema 91");
			auto const mode = serializer.readString("dispatchMode");
			if (mode == "manual") stationSettings.dispatchMode = StationDispatchMode::Manual;
			else if (mode == "automatic") stationSettings.dispatchMode = StationDispatchMode::Automatic;
			else throw SerializationException("Unknown Station dispatch mode: " + mode);
		}
		if (serializer.hasField("stores"))
		{
			if (version < 66) throw SerializationException("Station resource stores require World schema 66");
			serializer.beginArray("stores");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ResourceStoreSettings store;
				store.id = serializer.readString("id");
				store.unit = serializer.readString("unit");
				store.capacity = serializer.readDouble("capacity");
				store.startingQualified = serializer.readDouble("startingQualified");
				store.startingUnqualified = serializer.readDouble("startingUnqualified");
				if (serializer.hasField("protectedReserve"))
				{
					if (version < 67) throw SerializationException(
						"Station protected reserves require World schema 67");
					store.protectedReserve = serializer.readDouble("protectedReserve");
				}
				if (serializer.hasField("hazardClass"))
				{
					if (version < 88) throw SerializationException(
						"Station store hazard classes require World schema 88");
					store.hazardClass = serializer.readString("hazardClass");
				}
				if (serializer.hasField("composition"))
				{
					if (version < 68) throw SerializationException(
						"Station resource compositions require World schema 68");
					serializer.beginMap("composition");
					ResourceCompositionSettings composition;
					composition.moisture = serializer.readDouble("moisture");
					composition.nutrientNitrogen = serializer.readDouble("nutrientNitrogen");
					composition.atmosphericNitrogen = serializer.readDouble("atmosphericNitrogen");
					composition.phosphorus = serializer.readDouble("phosphorus");
					composition.potassium = serializer.readDouble("potassium");
					composition.otherMinerals = serializer.readDouble("otherMinerals");
					composition.energyKJPerKgDry = serializer.readDouble("energyKJPerKgDry");
					serializer.endMap();
					store.composition = composition;
				}
				stationSettings.stores.push_back(std::move(store));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("transfers"))
		{
			if (version < 67) throw SerializationException(
				"Station resource transfers require World schema 67");
			serializer.beginArray("transfers");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ResourceTransferSettings transfer;
				transfer.id = serializer.readString("id");
				transfer.sourceId = serializer.readString("sourceId");
				transfer.destinationId = serializer.readString("destinationId");
				transfer.unit = serializer.readString("unit");
				transfer.ratePerSecond = serializer.readDouble("ratePerSecond");
				transfer.priority = serializer.readInt32("priority");
				stationSettings.transfers.push_back(std::move(transfer));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("conversions"))
		{
			if (version < 68) throw SerializationException(
				"Station resource conversions require World schema 68");
			serializer.beginArray("conversions");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ResourceConversionSettings conversion;
				conversion.id = serializer.readString("id");
				conversion.priority = serializer.readInt32("priority");
				serializer.beginArray("inputs");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ResourceConversionInputSettings flow;
					flow.storeId = serializer.readString("storeId");
					flow.ratePerSecond = serializer.readDouble("ratePerSecond");
					conversion.inputs.push_back(std::move(flow));
					serializer.endMap();
				}
				serializer.endArray();
				serializer.beginArray("outputs");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ResourceConversionOutputSettings flow;
					flow.storeId = serializer.readString("storeId");
					flow.yieldPerSecond = serializer.readDouble("yieldPerSecond");
					conversion.outputs.push_back(std::move(flow));
					serializer.endMap();
				}
				serializer.endArray();
				stationSettings.conversions.push_back(std::move(conversion));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("solarArrays"))
		{
			if (version < 69) throw SerializationException(
				"Station solar arrays require World schema 69");
			serializer.beginArray("solarArrays");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				SolarArraySettings array;
				array.id = serializer.readString("id");
				array.capacityKW = serializer.readDouble("capacityKW");
				serializer.beginMap("illumination");
				array.illumination.constantLevel = serializer.readDouble("constantLevel");
				array.illumination.periodSeconds = serializer.readDouble("periodSeconds");
				serializer.beginArray("segments");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ProfileSegmentSettings segment;
					segment.startSeconds = serializer.readDouble("startSeconds");
					segment.level = serializer.readDouble("level");
					array.illumination.segments.push_back(segment);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				stationSettings.solarArrays.push_back(std::move(array));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("equipmentLoads"))
		{
			if (version < 69) throw SerializationException(
				"Station external equipment loads require World schema 69");
			serializer.beginArray("equipmentLoads");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				EquipmentLoadSettings load;
				load.id = serializer.readString("id");
				load.electricalDemandKW = serializer.readDouble("electricalDemandKW");
				serializer.beginMap("operatingProfile");
				load.operatingProfile.constantLevel = serializer.readDouble("constantLevel");
				load.operatingProfile.periodSeconds = serializer.readDouble("periodSeconds");
				serializer.beginArray("segments");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ProfileSegmentSettings segment;
					segment.startSeconds = serializer.readDouble("startSeconds");
					segment.level = serializer.readDouble("level");
					load.operatingProfile.segments.push_back(segment);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				load.heatOutputKW = serializer.readDouble("heatOutputKW");
				load.thermalClass = serializer.readString("thermalClass");
				auto const disposition = serializer.readString("coolingDisposition");
				if (disposition == "exported") load.coolingDisposition = StationCoolingDisposition::Exported;
				else if (disposition == "coolingService") load.coolingDisposition = StationCoolingDisposition::CoolingService;
				else throw SerializationException(
					"Unknown Station load cooling disposition '" + disposition + "'");
				load.criticalFloorKW = serializer.readDouble("criticalFloorKW");
				load.priority = serializer.readInt32("priority");
				load.maxCurtailment = serializer.readDouble("maxCurtailment");
				// #417 authored recovery rules. A pre-93 document that
				// carries any of them is refused rather than silently
				// dropped; a load without them keeps the zero defaults.
				if (serializer.hasField("recoveryDwellSeconds"))
				{
					if (version < 93) throw SerializationException(
						"Station load recovery rules require World schema 93");
					load.recoveryDwellSeconds = serializer.readDouble("recoveryDwellSeconds");
					load.recoveryHysteresisFraction = serializer.readDouble("recoveryHysteresisFraction");
					load.restartSurgePowerKW = serializer.readDouble("restartSurgePowerKW");
					load.restartSurgeSeconds = serializer.readDouble("restartSurgeSeconds");
				}
				serializer.beginArray("materialExchanges");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					EquipmentMaterialExchangeSettings exchange;
					exchange.storeId = serializer.readString("storeId");
					exchange.ratePerSecond = serializer.readDouble("ratePerSecond");
					exchange.consumes = serializer.readBool("consumes");
					load.materialExchanges.push_back(std::move(exchange));
					serializer.endMap();
				}
				serializer.endArray();
				if (serializer.hasField("heatSinkId"))
				{
					if (version < 72) throw SerializationException(
						"Station load heat sinks require World schema 72");
					load.heatSinkId = serializer.readString("heatSinkId");
				}
				stationSettings.equipmentLoads.push_back(std::move(load));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("thermalReservoirs"))
		{
			if (version < 72) throw SerializationException(
				"Station thermal reservoirs require World schema 72");
			serializer.beginArray("thermalReservoirs");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ThermalReservoirSettings reservoir;
				reservoir.id = serializer.readString("id");
				reservoir.thermalClass = serializer.readString("thermalClass");
				reservoir.heatCapacityKJPerK = serializer.readDouble("heatCapacityKJPerK");
				reservoir.startingTemperatureC = serializer.readDouble("startingTemperatureC");
				reservoir.minTemperatureC = serializer.readDouble("minTemperatureC");
				reservoir.maxTemperatureC = serializer.readDouble("maxTemperatureC");
				reservoir.absorbedHeatKW = serializer.readDouble("absorbedHeatKW");
				stationSettings.thermalReservoirs.push_back(std::move(reservoir));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("radiators"))
		{
			if (version < 72) throw SerializationException(
				"Station radiators require World schema 72");
			serializer.beginArray("radiators");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				RadiatorSettings radiator;
				radiator.id = serializer.readString("id");
				radiator.reservoirId = serializer.readString("reservoirId");
				radiator.areaM2 = serializer.readDouble("areaM2");
				radiator.emissivity = serializer.readDouble("emissivity");
				radiator.viewFactor = serializer.readDouble("viewFactor");
				radiator.sinkTemperatureC = serializer.readDouble("sinkTemperatureC");
				stationSettings.radiators.push_back(std::move(radiator));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("coolantLoops"))
		{
			if (version < 73) throw SerializationException(
				"Station coolant loops require World schema 73");
			serializer.beginArray("coolantLoops");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				CoolantLoopSettings loop;
				loop.id = serializer.readString("id");
				loop.hazardClass = serializer.readString("hazardClass");
				loop.reservoirId = serializer.readString("reservoirId");
				loop.coolantCapacityKg = serializer.readDouble("coolantCapacityKg");
				loop.startingCoolantKg = serializer.readDouble("startingCoolantKg");
				loop.specificHeatKJPerKgK = serializer.readDouble("specificHeatKJPerKgK");
				loop.startingTemperatureC = serializer.readDouble("startingTemperatureC");
				loop.freezeProtectionSetpointC = serializer.readDouble("freezeProtectionSetpointC");
				loop.pumpPowerKW = serializer.readDouble("pumpPowerKW");
				loop.ratedFlowKgPerS = serializer.readDouble("ratedFlowKgPerS");
				loop.exchangerConductanceKWPerK = serializer.readDouble("exchangerConductanceKWPerK");
				loop.heaterCapacityKW = serializer.readDouble("heaterCapacityKW");
				// #416: the protected critical service flag. A pre-92
				// document carrying it is refused rather than silently
				// dropped.
				if (serializer.hasField("criticalService"))
				{
					if (version < 92) throw SerializationException(
						"Protected station service flags require World schema 92");
					loop.criticalService = serializer.readBool("criticalService");
				}
				stationSettings.coolantLoops.push_back(std::move(loop));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("chillers"))
		{
			if (version < 74) throw SerializationException(
				"Station chillers require World schema 74");
			serializer.beginArray("chillers");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ChillerSettings chiller;
				chiller.id = serializer.readString("id");
				chiller.thermalClass = serializer.readString("thermalClass");
				chiller.coldLoopId = serializer.readString("coldLoopId");
				chiller.condenserReservoirId = serializer.readString("condenserReservoirId");
				chiller.ratedCoolingCapacityKW = serializer.readDouble("ratedCoolingCapacityKW");
				chiller.cop = serializer.readDouble("cop");
				chiller.auxiliaryPowerKW = serializer.readDouble("auxiliaryPowerKW");
				chiller.chilledSetpointC = serializer.readDouble("chilledSetpointC");
				chiller.maxCondensingTemperatureC = serializer.readDouble("maxCondensingTemperatureC");
				// #416: the protected critical service flag. A pre-92
				// document carrying it is refused rather than silently
				// dropped.
				if (serializer.hasField("criticalService"))
				{
					if (version < 92) throw SerializationException(
						"Protected station service flags require World schema 92");
					chiller.criticalService = serializer.readBool("criticalService");
				}
				stationSettings.chillers.push_back(std::move(chiller));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("generators"))
		{
			if (version < 75) throw SerializationException(
				"Station generators require World schema 75");
			serializer.beginArray("generators");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				GeneratorSettings generator;
				generator.id = serializer.readString("id");
				generator.fuelStoreId = serializer.readString("fuelStoreId");
				generator.thermalClass = serializer.readString("thermalClass");
				generator.heatSinkId = serializer.readString("heatSinkId");
				generator.ratedOutputKW = serializer.readDouble("ratedOutputKW");
				generator.efficiency = serializer.readDouble("efficiency");
				generator.maxRampKWPerSecond = serializer.readDouble("maxRampKWPerSecond");
				generator.commandedOutputKW = serializer.readDouble("commandedOutputKW");
				generator.startupPowerKW = serializer.readDouble("startupPowerKW");
				generator.startupSeconds = serializer.readDouble("startupSeconds");
				generator.shutdownHeatKW = serializer.readDouble("shutdownHeatKW");
				generator.shutdownSeconds = serializer.readDouble("shutdownSeconds");
				generator.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				stationSettings.generators.push_back(std::move(generator));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("atmospheres"))
		{
			if (version < 76) throw SerializationException(
				"Station atmospheres require World schema 76");
			serializer.beginArray("atmospheres");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				AtmosphereSettings atmosphere;
				atmosphere.id = serializer.readString("id");
				atmosphere.hazardClass = serializer.readString("hazardClass");
				atmosphere.volumeM3 = serializer.readDouble("volumeM3");
				atmosphere.startingOxygenKg = serializer.readDouble("startingOxygenKg");
				atmosphere.startingNitrogenKg = serializer.readDouble("startingNitrogenKg");
				atmosphere.startingCarbonDioxideKg = serializer.readDouble("startingCarbonDioxideKg");
				atmosphere.startingWaterVapourKg = serializer.readDouble("startingWaterVapourKg");
				atmosphere.startingTemperatureC = serializer.readDouble("startingTemperatureC");
				atmosphere.thermalCapacityKJPerK = serializer.readDouble("thermalCapacityKJPerK");
				atmosphere.reservoirId = serializer.readString("reservoirId");
				atmosphere.exchangerConductanceKWPerK = serializer.readDouble("exchangerConductanceKWPerK");
				atmosphere.occupants = serializer.readInt32("occupants");
				atmosphere.oxygenUsePerOccupantKgPerS = serializer.readDouble("oxygenUsePerOccupantKgPerS");
				atmosphere.co2ProductionPerOccupantKgPerS = serializer.readDouble("co2ProductionPerOccupantKgPerS");
				atmosphere.sensibleHeatPerOccupantKW = serializer.readDouble("sensibleHeatPerOccupantKW");
				atmosphere.moistureAdditionPerOccupantKgPerS = serializer.readDouble("moistureAdditionPerOccupantKgPerS");
				atmosphere.leakRateKgPerS = serializer.readDouble("leakRateKgPerS");
				atmosphere.minPressureKPa = serializer.readDouble("minPressureKPa");
				atmosphere.maxPressureKPa = serializer.readDouble("maxPressureKPa");
				atmosphere.minOxygenPartialPressureKPa = serializer.readDouble("minOxygenPartialPressureKPa");
				atmosphere.maxCarbonDioxidePartialPressureKPa = serializer.readDouble("maxCarbonDioxidePartialPressureKPa");
				stationSettings.atmospheres.push_back(std::move(atmosphere));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("airlockCycles"))
		{
			if (version < 76) throw SerializationException(
				"Station airlock cycles require World schema 76");
			serializer.beginArray("airlockCycles");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				AirlockCycleSettings cycle;
				cycle.id = serializer.readString("id");
				cycle.atmosphereId = serializer.readString("atmosphereId");
				cycle.gasMassKg = serializer.readDouble("gasMassKg");
				cycle.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				cycle.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
				cycle.recoveredFraction = serializer.readDouble("recoveredFraction");
				cycle.priority = serializer.readInt32("priority");
				stationSettings.airlockCycles.push_back(std::move(cycle));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("crews"))
		{
			if (version < 77) throw SerializationException(
				"Station crews require World schema 77");
			serializer.beginArray("crews");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				CrewSettings crew;
				crew.id = serializer.readString("id");
				crew.atmosphereId = serializer.readString("atmosphereId");
				crew.crewCount = serializer.readInt32("crewCount");
				serializer.beginMap("activity");
				crew.activityProfile.constantLevel = serializer.readDouble("constantLevel");
				crew.activityProfile.periodSeconds = serializer.readDouble("periodSeconds");
				serializer.beginArray("segments");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ProfileSegmentSettings segment;
					segment.startSeconds = serializer.readDouble("startSeconds");
					segment.level = serializer.readDouble("level");
					serializer.endMap();
					crew.activityProfile.segments.push_back(segment);
				}
				serializer.endArray();
				serializer.endMap();
				crew.foodStoreId = serializer.readString("foodStoreId");
				crew.waterStoreId = serializer.readString("waterStoreId");
				crew.urineStoreId = serializer.readString("urineStoreId");
				crew.organicWasteStoreId = serializer.readString("organicWasteStoreId");
				crew.foodPerCrewPerS = serializer.readDouble("foodPerCrewPerS");
				crew.waterPerCrewPerS = serializer.readDouble("waterPerCrewPerS");
				crew.oxygenPerCrewKgPerS = serializer.readDouble("oxygenPerCrewKgPerS");
				crew.co2PerCrewKgPerS = serializer.readDouble("co2PerCrewKgPerS");
				crew.moisturePerCrewKgPerS = serializer.readDouble("moisturePerCrewKgPerS");
				crew.urinePerCrewPerS = serializer.readDouble("urinePerCrewPerS");
				crew.organicWastePerCrewPerS = serializer.readDouble("organicWastePerCrewPerS");
				crew.metabolicHeatPerCrewKW = serializer.readDouble("metabolicHeatPerCrewKW");
				stationSettings.crews.push_back(std::move(crew));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("waterCollectors"))
		{
			if (version < 78) throw SerializationException(
				"Station water collectors require World schema 78");
			serializer.beginArray("waterCollectors");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				WaterCollectorSettings collector;
				collector.id = serializer.readString("id");
				collector.hazardClass = serializer.readString("hazardClass");
				collector.atmosphereId = serializer.readString("atmosphereId");
				collector.coolingLoopId = serializer.readString("coolingLoopId");
				collector.thermalClass = serializer.readString("thermalClass");
				collector.destStoreId = serializer.readString("destStoreId");
				collector.maxRateKgPerS = serializer.readDouble("maxRateKgPerS");
				collector.targetRelativeHumidity = serializer.readDouble("targetRelativeHumidity");
				stationSettings.waterCollectors.push_back(std::move(collector));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("waterTreatments"))
		{
			if (version < 78) throw SerializationException(
				"Station water treatments require World schema 78");
			serializer.beginArray("waterTreatments");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				WaterTreatmentSettings treatment;
				treatment.id = serializer.readString("id");
				treatment.hazardClass = serializer.readString("hazardClass");
				treatment.sourceStoreId = serializer.readString("sourceStoreId");
				treatment.treatedStoreId = serializer.readString("treatedStoreId");
				treatment.concentrateStoreId = serializer.readString("concentrateStoreId");
				treatment.consumableStoreId = serializer.readString("consumableStoreId");
				treatment.throughputKgPerS = serializer.readDouble("throughputKgPerS");
				treatment.recoveryRatio = serializer.readDouble("recoveryRatio");
				treatment.specificEnergyKWhPerKg = serializer.readDouble("specificEnergyKWhPerKg");
				treatment.consumablePerKg = serializer.readDouble("consumablePerKg");
				if (serializer.hasField("clearanceStoreId"))
				{
					if (version < 88) throw SerializationException(
						"Station release-clearance contracts require World schema 88");
					treatment.clearanceStoreId = serializer.readString("clearanceStoreId");
				}
				if (serializer.hasField("clearancePerKg"))
				{
					if (version < 88) throw SerializationException(
						"Station release-clearance contracts require World schema 88");
					treatment.clearancePerKg = serializer.readDouble("clearancePerKg");
				}
				treatment.priority = serializer.readInt32("priority");
				stationSettings.waterTreatments.push_back(std::move(treatment));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("electrolyzers"))
		{
			if (version < 79) throw SerializationException(
				"Station electrolyzers require World schema 79");
			serializer.beginArray("electrolyzers");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ElectrolyzerSettings electrolyzer;
				electrolyzer.id = serializer.readString("id");
				electrolyzer.hazardClass = serializer.readString("hazardClass");
				electrolyzer.waterStoreId = serializer.readString("waterStoreId");
				electrolyzer.oxygenStoreId = serializer.readString("oxygenStoreId");
				electrolyzer.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				electrolyzer.atmosphereId = serializer.readString("atmosphereId");
				electrolyzer.thermalClass = serializer.readString("thermalClass");
				electrolyzer.heatSinkId = serializer.readString("heatSinkId");
				electrolyzer.rateKgPerS = serializer.readDouble("rateKgPerS");
				electrolyzer.specificEnergyKWhPerKgWater =
					serializer.readDouble("specificEnergyKWhPerKgWater");
				electrolyzer.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				electrolyzer.priority = serializer.readInt32("priority");
				stationSettings.electrolyzers.push_back(std::move(electrolyzer));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("co2Captures"))
		{
			if (version < 79) throw SerializationException(
				"Station CO2 capture machines require World schema 79");
			serializer.beginArray("co2Captures");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				Co2CaptureSettings capture;
				capture.id = serializer.readString("id");
				capture.hazardClass = serializer.readString("hazardClass");
				capture.atmosphereId = serializer.readString("atmosphereId");
				capture.captureStoreId = serializer.readString("captureStoreId");
				capture.consumableStoreId = serializer.readString("consumableStoreId");
				capture.maxRateKgPerS = serializer.readDouble("maxRateKgPerS");
				capture.specificEnergyKWhPerKg = serializer.readDouble("specificEnergyKWhPerKg");
				capture.consumablePerKg = serializer.readDouble("consumablePerKg");
				capture.priority = serializer.readInt32("priority");
				stationSettings.co2Captures.push_back(std::move(capture));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("fuelCells"))
		{
			if (version < 80) throw SerializationException(
				"Station fuel cells require World schema 80");
			serializer.beginArray("fuelCells");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				FuelCellSettings cell;
				cell.id = serializer.readString("id");
				cell.hazardClass = serializer.readString("hazardClass");
				cell.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				cell.oxygenStoreId = serializer.readString("oxygenStoreId");
				cell.waterStoreId = serializer.readString("waterStoreId");
				cell.thermalClass = serializer.readString("thermalClass");
				cell.heatSinkId = serializer.readString("heatSinkId");
				cell.ratedOutputKW = serializer.readDouble("ratedOutputKW");
				cell.efficiency = serializer.readDouble("efficiency");
				cell.commandedOutputKW = serializer.readDouble("commandedOutputKW");
				cell.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				stationSettings.fuelCells.push_back(std::move(cell));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("thermalBuffers"))
		{
			if (version < 81) throw SerializationException(
				"Station thermal buffers require World schema 81");
			serializer.beginArray("thermalBuffers");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ThermalBufferSettings buffer;
				buffer.id = serializer.readString("id");
				buffer.reservoirId = serializer.readString("reservoirId");
				buffer.accessMode = serializer.readString("accessMode");
				buffer.circulationLoopId = serializer.readString("circulationLoopId");
				buffer.capacityKJ = serializer.readDouble("capacityKJ");
				buffer.startingEnergyKJ = serializer.readDouble("startingEnergyKJ");
				buffer.minUsableTemperatureC = serializer.readDouble("minUsableTemperatureC");
				buffer.maxUsableTemperatureC = serializer.readDouble("maxUsableTemperatureC");
				buffer.maxChargeKW = serializer.readDouble("maxChargeKW");
				buffer.maxDischargeKW = serializer.readDouble("maxDischargeKW");
				buffer.transferConductanceKWPerK = serializer.readDouble("transferConductanceKWPerK");
				stationSettings.thermalBuffers.push_back(std::move(buffer));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("sabatierReactors"))
		{
			if (version < 83) throw SerializationException(
				"Station Sabatier reactors require World schema 83");
			serializer.beginArray("sabatierReactors");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				SabatierReactorSettings reactor;
				reactor.id = serializer.readString("id");
				reactor.hazardClass = serializer.readString("hazardClass");
				reactor.co2StoreId = serializer.readString("co2StoreId");
				reactor.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				reactor.methaneStoreId = serializer.readString("methaneStoreId");
				reactor.waterStoreId = serializer.readString("waterStoreId");
				reactor.thermalClass = serializer.readString("thermalClass");
				reactor.heatSinkId = serializer.readString("heatSinkId");
				reactor.rateKgPerS = serializer.readDouble("rateKgPerS");
				reactor.specificEnergyKWhPerKgCO2 =
					serializer.readDouble("specificEnergyKWhPerKgCO2");
				reactor.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				reactor.priority = serializer.readInt32("priority");
				stationSettings.sabatierReactors.push_back(std::move(reactor));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("methaneProcessors"))
		{
			if (version < 83) throw SerializationException(
				"Station methane processors require World schema 83");
			serializer.beginArray("methaneProcessors");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				MethaneProcessorSettings processor;
				processor.id = serializer.readString("id");
				processor.hazardClass = serializer.readString("hazardClass");
				processor.methaneStoreId = serializer.readString("methaneStoreId");
				processor.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				processor.carbonStoreId = serializer.readString("carbonStoreId");
				processor.thermalClass = serializer.readString("thermalClass");
				processor.heatSinkId = serializer.readString("heatSinkId");
				processor.rateKgPerS = serializer.readDouble("rateKgPerS");
				processor.specificEnergyKWhPerKgCH4 =
					serializer.readDouble("specificEnergyKWhPerKgCH4");
				processor.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				processor.priority = serializer.readInt32("priority");
				stationSettings.methaneProcessors.push_back(std::move(processor));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("microbialProcessors"))
		{
			if (version < 85) throw SerializationException(
				"Station microbial processors require World schema 85");
			serializer.beginArray("microbialProcessors");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				MicrobialProcessorSettings processor;
				processor.id = serializer.readString("id");
				processor.hazardClass = serializer.readString("hazardClass");
				processor.feedStoreId = serializer.readString("feedStoreId");
				processor.oxygenStoreId = serializer.readString("oxygenStoreId");
				processor.waterStoreId = serializer.readString("waterStoreId");
				processor.consumableStoreId = serializer.readString("consumableStoreId");
				processor.nutrientStoreId = serializer.readString("nutrientStoreId");
				processor.recoveredWaterStoreId = serializer.readString("recoveredWaterStoreId");
				processor.co2StoreId = serializer.readString("co2StoreId");
				processor.residueStoreId = serializer.readString("residueStoreId");
				processor.thermalClass = serializer.readString("thermalClass");
				processor.heatSinkId = serializer.readString("heatSinkId");
				processor.rateKgPerS = serializer.readDouble("rateKgPerS");
				processor.specificEnergyKWhPerKgFeed =
					serializer.readDouble("specificEnergyKWhPerKgFeed");
				processor.oxygenPerKgFeedKg = serializer.readDouble("oxygenPerKgFeedKg");
				processor.processWaterPerKgFeedKg =
					serializer.readDouble("processWaterPerKgFeedKg");
				processor.consumablePerKgFeed = serializer.readDouble("consumablePerKgFeed");
				processor.co2PerKgFeedKg = serializer.readDouble("co2PerKgFeedKg");
				processor.nitrogenRecovery = serializer.readDouble("nitrogenRecovery");
				processor.phosphorusRecovery = serializer.readDouble("phosphorusRecovery");
				processor.potassiumRecovery = serializer.readDouble("potassiumRecovery");
				processor.mineralsRecovery = serializer.readDouble("mineralsRecovery");
				processor.waterRecovery = serializer.readDouble("waterRecovery");
				processor.reactionHeatKJPerKgFeed =
					serializer.readDouble("reactionHeatKJPerKgFeed");
				processor.minOperatingTemperatureC =
					serializer.readDouble("minOperatingTemperatureC");
				processor.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				processor.initialCondition = serializer.readDouble("initialCondition");
				processor.starvationDegradePerHour =
					serializer.readDouble("starvationDegradePerHour");
				processor.consumableExhaustionDegradePerHour =
					serializer.readDouble("consumableExhaustionDegradePerHour");
				processor.coldStressDegradePerHour =
					serializer.readDouble("coldStressDegradePerHour");
				processor.heatStressDegradePerHour =
					serializer.readDouble("heatStressDegradePerHour");
				processor.recoveryPerHour = serializer.readDouble("recoveryPerHour");
				processor.stockStoreId = serializer.readString("stockStoreId");
				processor.stockUnitsPerConditionPoint =
					serializer.readDouble("stockUnitsPerConditionPoint");
				processor.priority = serializer.readInt32("priority");
				stationSettings.microbialProcessors.push_back(std::move(processor));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("cropPopulations"))
		{
			if (version < 86) throw SerializationException(
				"Station crop populations require World schema 86");
			serializer.beginArray("cropPopulations");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				CropPopulationSettings crop;
				crop.id = serializer.readString("id");
				crop.atmosphereId = serializer.readString("atmosphereId");
				crop.waterStoreId = serializer.readString("waterStoreId");
				crop.nitrogenStoreId = serializer.readString("nitrogenStoreId");
				crop.phosphorusStoreId = serializer.readString("phosphorusStoreId");
				crop.potassiumStoreId = serializer.readString("potassiumStoreId");
				crop.mineralsStoreId = serializer.readString("mineralsStoreId");
				serializer.beginMap("composition");
				crop.composition.moisture = serializer.readDouble("moisture");
				crop.composition.nutrientNitrogen = serializer.readDouble("nutrientNitrogen");
				crop.composition.atmosphericNitrogen =
					serializer.readDouble("atmosphericNitrogen");
				crop.composition.phosphorus = serializer.readDouble("phosphorus");
				crop.composition.potassium = serializer.readDouble("potassium");
				crop.composition.otherMinerals = serializer.readDouble("otherMinerals");
				crop.composition.energyKJPerKgDry = serializer.readDouble("energyKJPerKgDry");
				serializer.endMap();
				crop.growingAreaM2 = serializer.readDouble("growingAreaM2");
				crop.maxBiomassDensityKgPerM2 = serializer.readDouble("maxBiomassDensityKgPerM2");
				crop.initialBiomassKg = serializer.readDouble("initialBiomassKg");
				crop.lampRatedKW = serializer.readDouble("lampRatedKW");
				serializer.beginMap("illumination");
				crop.illuminationProfile.constantLevel = serializer.readDouble("constantLevel");
				crop.illuminationProfile.periodSeconds = serializer.readDouble("periodSeconds");
				serializer.beginArray("segments");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ProfileSegmentSettings segment;
					segment.startSeconds = serializer.readDouble("startSeconds");
					segment.level = serializer.readDouble("level");
					serializer.endMap();
					crop.illuminationProfile.segments.push_back(segment);
				}
				serializer.endArray();
				serializer.endMap();
				crop.circulationPowerKW = serializer.readDouble("circulationPowerKW");
				crop.growthYieldKgPerKWh = serializer.readDouble("growthYieldKgPerKWh");
				crop.respirationPerHour = serializer.readDouble("respirationPerHour");
				crop.transpirationKgPerKgGrowth = serializer.readDouble("transpirationKgPerKgGrowth");
				crop.minGrowingTemperatureC = serializer.readDouble("minGrowingTemperatureC");
				crop.maxGrowingTemperatureC = serializer.readDouble("maxGrowingTemperatureC");
				// Harvest contract fields (#410): a pre-87 document
				// carries none of them, and the defaults keep the
				// harvest disabled so existing Worlds are unchanged.
				if (version >= 87 && serializer.hasField("foodStoreId"))
					crop.foodStoreId = serializer.readString("foodStoreId");
				if (version >= 87 && serializer.hasField("wasteStoreId"))
					crop.wasteStoreId = serializer.readString("wasteStoreId");
				if (version >= 87 && serializer.hasField("edibleFraction"))
					crop.edibleFraction = serializer.readDouble("edibleFraction");
				if (version >= 87 && serializer.hasField("harvestTriggerKg"))
					crop.harvestTriggerKg = serializer.readDouble("harvestTriggerKg");
				if (version >= 87 && serializer.hasField("harvestFraction"))
					crop.harvestFraction = serializer.readDouble("harvestFraction");
				crop.priority = serializer.readInt32("priority");
				stationSettings.cropPopulations.push_back(std::move(crop));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("algaeCultures"))
		{
			if (version < 89) throw SerializationException(
				"Station algae cultures require World schema 89");
			serializer.beginArray("algaeCultures");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				AlgaeCultureSettings algae;
				algae.id = serializer.readString("id");
				algae.atmosphereId = serializer.readString("atmosphereId");
				algae.waterStoreId = serializer.readString("waterStoreId");
				algae.nitrogenStoreId = serializer.readString("nitrogenStoreId");
				algae.phosphorusStoreId = serializer.readString("phosphorusStoreId");
				algae.potassiumStoreId = serializer.readString("potassiumStoreId");
				algae.mineralsStoreId = serializer.readString("mineralsStoreId");
				serializer.beginMap("composition");
				algae.composition.moisture = serializer.readDouble("moisture");
				algae.composition.nutrientNitrogen = serializer.readDouble("nutrientNitrogen");
				algae.composition.atmosphericNitrogen =
					serializer.readDouble("atmosphericNitrogen");
				algae.composition.phosphorus = serializer.readDouble("phosphorus");
				algae.composition.potassium = serializer.readDouble("potassium");
				algae.composition.otherMinerals = serializer.readDouble("otherMinerals");
				algae.composition.energyKJPerKgDry = serializer.readDouble("energyKJPerKgDry");
				serializer.endMap();
				algae.cultureVolumeL = serializer.readDouble("cultureVolumeL");
				algae.maxBiomassDensityKgPerL = serializer.readDouble("maxBiomassDensityKgPerL");
				algae.initialBiomassKg = serializer.readDouble("initialBiomassKg");
				algae.lampRatedKW = serializer.readDouble("lampRatedKW");
				serializer.beginMap("illumination");
				algae.illuminationProfile.constantLevel = serializer.readDouble("constantLevel");
				algae.illuminationProfile.periodSeconds = serializer.readDouble("periodSeconds");
				serializer.beginArray("segments");
				while (serializer.nextArrayItem())
				{
					serializer.beginMap("");
					ProfileSegmentSettings segment;
					segment.startSeconds = serializer.readDouble("startSeconds");
					segment.level = serializer.readDouble("level");
					serializer.endMap();
					algae.illuminationProfile.segments.push_back(segment);
				}
				serializer.endArray();
				serializer.endMap();
				algae.circulationPowerKW = serializer.readDouble("circulationPowerKW");
				algae.growthYieldKgPerKWh = serializer.readDouble("growthYieldKgPerKWh");
				algae.respirationPerHour = serializer.readDouble("respirationPerHour");
				algae.evaporationKgPerKgGrowth = serializer.readDouble("evaporationKgPerKgGrowth");
				algae.minGrowingTemperatureC = serializer.readDouble("minGrowingTemperatureC");
				algae.maxGrowingTemperatureC = serializer.readDouble("maxGrowingTemperatureC");
				algae.foodStoreId = serializer.readString("foodStoreId");
				algae.recoveryStoreId = serializer.readString("recoveryStoreId");
				algae.wasteStoreId = serializer.readString("wasteStoreId");
				algae.foodFraction = serializer.readDouble("foodFraction");
				algae.recoveryFraction = serializer.readDouble("recoveryFraction");
				algae.harvestTriggerKg = serializer.readDouble("harvestTriggerKg");
				algae.harvestFraction = serializer.readDouble("harvestFraction");
				algae.priority = serializer.readInt32("priority");
				stationSettings.algaeCultures.push_back(std::move(algae));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("batteries"))
		{
			if (version < 70) throw SerializationException(
				"Station batteries require World schema 70");
			serializer.beginArray("batteries");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				BatterySettings battery;
				battery.id = serializer.readString("id");
				battery.nameplateKWh = serializer.readDouble("nameplateKWh");
				battery.usableKWh = serializer.readDouble("usableKWh");
				battery.startingStoredKWh = serializer.readDouble("startingStoredKWh");
				battery.maxChargeKW = serializer.readDouble("maxChargeKW");
				battery.maxDischargeKW = serializer.readDouble("maxDischargeKW");
				battery.chargeEfficiency = serializer.readDouble("chargeEfficiency");
				battery.dischargeEfficiency = serializer.readDouble("dischargeEfficiency");
				stationSettings.batteries.push_back(std::move(battery));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("deliveries"))
		{
			if (version < 71) throw SerializationException(
				"Station resupply deliveries require World schema 71");
			serializer.beginArray("deliveries");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				ResourceDeliverySettings delivery;
				delivery.id = serializer.readString("id");
				delivery.storeId = serializer.readString("storeId");
				delivery.unit = serializer.readString("unit");
				delivery.quantity = serializer.readDouble("quantity");
				delivery.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				delivery.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
				delivery.arrivesQualified = serializer.readBool("arrivesQualified");
				delivery.priority = serializer.readInt32("priority");
				stationSettings.deliveries.push_back(std::move(delivery));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("wasteEjections"))
		{
			if (version < 82) throw SerializationException(
				"Station waste ejections require World schema 82");
			serializer.beginArray("wasteEjections");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				WasteEjectionSettings ejection;
				ejection.id = serializer.readString("id");
				ejection.storeId = serializer.readString("storeId");
				ejection.unit = serializer.readString("unit");
				ejection.quantity = serializer.readDouble("quantity");
				ejection.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				ejection.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
				ejection.priority = serializer.readInt32("priority");
				stationSettings.wasteEjections.push_back(std::move(ejection));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("outages"))
		{
			if (version < 90) throw SerializationException(
				"Station installation outages require World schema 90");
			serializer.beginArray("outages");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				InstallationOutageSettings outage;
				outage.id = serializer.readString("id");
				outage.installationId = serializer.readString("installationId");
				outage.capacityFraction = serializer.readDouble("capacityFraction");
				outage.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				outage.durationSeconds = serializer.readDouble("durationSeconds");
				outage.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
				outage.priority = serializer.readInt32("priority");
				stationSettings.outages.push_back(std::move(outage));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("maintenanceJobs"))
		{
			if (version < 94) throw SerializationException(
				"Station maintenance jobs require World schema 94");
			serializer.beginArray("maintenanceJobs");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				MaintenanceJobSettings job;
				job.id = serializer.readString("id");
				job.installationId = serializer.readString("installationId");
				job.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				job.durationSeconds = serializer.readDouble("durationSeconds");
				job.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
				job.priority = serializer.readInt32("priority");
				job.wasteStoreId = serializer.readString("wasteStoreId");
				if (serializer.hasField("requirements"))
				{
					serializer.beginArray("requirements");
					while (serializer.nextArrayItem())
					{
						serializer.beginMap("");
						MaintenanceRequirementSettings requirement;
						requirement.storeId = serializer.readString("storeId");
						requirement.quantity = serializer.readDouble("quantity");
						requirement.consumed = serializer.readBool("consumed");
						job.requirements.push_back(std::move(requirement));
						serializer.endMap();
					}
					serializer.endArray();
				}
				stationSettings.maintenanceJobs.push_back(std::move(job));
				serializer.endMap();
			}
			serializer.endArray();
		}
		// #418: the authored trend retention, read last so every older
		// feature's guard names its own schema first (see the guard-order
		// note above the dispatch mode read).
		if (serializer.hasField("trendRetentionSeconds"))
		{
			if (version < 95) throw SerializationException(
				"Station trend retention requires World schema 95");
			stationSettings.trendRetentionSeconds = serializer.readFloat("trendRetentionSeconds");
			if (!std::isfinite(stationSettings.trendRetentionSeconds)
				|| stationSettings.trendRetentionSeconds < 1.0f
				|| stationSettings.trendRetentionSeconds > 86400.0f)
				throw SerializationException(
					"Station trend retention must be finite, from 1 to 86400 seconds");
		}
		serializer.endMap();
		std::string diagnostic;
		if (!validateStationSystemsSettings(stationSettings, diagnostic)) throw SerializationException(diagnostic);
	}

	bool parseStationReference(std::string const& text,
		StationReferenceDocument& document, std::string* diagnostic)
	{
		auto const refuse = [&](std::string const& message)
		{
			if (diagnostic) *diagnostic = message;
			return false;
		};
		try
		{
			auto serializer = YamlSerializer::fromString(text);
			serializer->deserialize();
			// The reference document's root map is opened descriptively,
			// exactly like the World reader opens "world": the meta fields
			// live at the document root and the stationSystems child is
			// resolved against it by the authoritative reader.
			if (!serializer->hasField("stationSystems"))
				return refuse("Reference station document has no 'stationSystems' section");
			serializer->beginMap("referenceStation");
			document = StationReferenceDocument{};
			document.name = serializer->readString("name");
			if (document.name.empty())
				return refuse("Reference station name must not be empty");
			document.summary = serializer->readString("summary");
			document.schemaVersion = static_cast<std::uint32_t>(
				serializer->readInt64("schemaVersion"));
			// The reference is authored against one World schema and must
			// be re-validated when the station document format changes:
			// the lower bound is the first schema that carries station
			// systems, the upper bound is the newest schema this reader
			// knows. A reference outside that range is refused rather than
			// read under assumptions the reader has not been checked for.
			if (document.schemaVersion < 65 || document.schemaVersion > 95)
				return refuse("Reference station schema version must be from 65 to 95");
			if (serializer->hasField("provenance"))
			{
				serializer->beginArray("provenance");
				while (serializer->nextArrayItem())
				{
					serializer->beginMap("");
					StationReferenceProvenance entry;
					entry.coefficient = serializer->readString("coefficient");
					entry.value = serializer->readString("value");
					entry.units = serializer->readString("units");
					entry.source = serializer->readString("source");
					if (entry.coefficient.empty())
						return refuse("Reference provenance entry with an empty coefficient");
					document.provenance.push_back(std::move(entry));
					serializer->endMap();
				}
				serializer->endArray();
			}
			if (serializer->hasField("assumptions"))
			{
				serializer->beginArray("assumptions");
				while (serializer->nextArrayItem())
				{
					serializer->beginMap("");
					auto topic = serializer->readString("topic");
					auto const body = serializer->readString("text");
					if (topic.empty())
						return refuse("Reference assumption entry with an empty topic");
					document.assumptions.emplace_back(std::move(topic), body);
					serializer->endMap();
				}
				serializer->endArray();
			}
			document.qualification = serializer->readString("qualification");
			document.scenarioSeconds = serializer->readDouble("scenarioSeconds");
			if (!std::isfinite(document.scenarioSeconds) || document.scenarioSeconds <= 0.0)
				return refuse("Reference scenario duration must be finite and positive");
			document.scenarioNotes = serializer->readString("scenarioNotes");
			// #423: the scenario kind and the documented expectations.
			// Optional: the normal-operation reference carries neither and
			// keeps the "reference" default with empty expectation lists.
			// An unknown kind is refused so a typo cannot silently reclass
			// a drill as the baseline or vice versa.
			if (serializer->hasField("scenarioKind"))
			{
				document.scenarioKind = serializer->readString("scenarioKind");
				if (document.scenarioKind != "reference" && document.scenarioKind != "emergency")
					throw SerializationException(
						"Unknown reference scenario kind: " + document.scenarioKind);
			}
			auto const readExpectations = [&](char const* field,
				std::vector<StationScenarioExpectation>& expectations)
			{
				if (!serializer->hasField(field)) return;
				serializer->beginArray(field);
				while (serializer->nextArrayItem())
				{
					serializer->beginMap("");
					StationScenarioExpectation expectation;
					expectation.item = serializer->readString("item");
					expectation.detail = serializer->readString("detail");
					if (expectation.item.empty())
						throw SerializationException(
							std::string("Reference scenario expectation with an empty item in '")
							+ field + "'");
					if (expectation.detail.empty())
						throw SerializationException(
							std::string("Reference scenario expectation '") + expectation.item
							+ "' carries no documented bound or rationale");
					expectations.push_back(std::move(expectation));
					serializer->endMap();
				}
				serializer->endArray();
			};
			readExpectations("limitingServices", document.limitingServices);
			readExpectations("eventOrdering", document.eventOrdering);
			readExpectations("outcomeRanges", document.outcomeRanges);
			// The settings themselves are read by the authoritative World
			// station reader, including its full physical-consistency
			// validation: intentional operational shortages (thin but
			// legal inventories) pass, impossible declarations are refused
			// here exactly as they would be for a saved World.
			readStationSystemsSettingsMap(*serializer, document.schemaVersion,
			document.settings);
			serializer->endMap();
			if (diagnostic) diagnostic->clear();
			return true;
		}
		catch (SerializationException const& error)
		{
			return refuse(error.what());
		}
	}

	std::string const& bundledStationReferenceText()
	{
		static std::string const text{ kStationReferenceYaml };
		return text;
	}

	std::vector<BundledStationScenario> const& bundledStationScenarios()
	{
		// #423: the packaged emergency/recovery variants are parsed once,
		// each through the same authoritative reader and validation the
		// base reference passes - a scenario resource that does not parse
		// is a build defect, never a user error.
		static std::vector<BundledStationScenario> const scenarios = []
		{
			std::vector<BundledStationScenario> parsed;
			for (auto const& resource : kBundledStationScenarios)
			{
				BundledStationScenario scenario;
				scenario.id = resource.id;
				scenario.text = resource.text;
				std::string diagnostic;
				if (!parseStationReference(resource.text, scenario.document, &diagnostic))
					throw std::runtime_error("Bundled station scenario '" + scenario.id
						+ "' failed to parse: " + diagnostic);
				parsed.push_back(std::move(scenario));
			}
			return parsed;
		}();
		return scenarios;
	}

	StationReferenceDocument const& bundledStationReference()
	{
		static StationReferenceDocument const document = []
		{
			StationReferenceDocument parsed;
			std::string diagnostic;
			if (!parseStationReference(bundledStationReferenceText(), parsed, &diagnostic))
				throw std::runtime_error(
					"Bundled reference station failed to parse: " + diagnostic);
			return parsed;
		}();
		return document;
	}
}
