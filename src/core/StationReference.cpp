// #422: the packaged reference station.
//
// Shared stationSystems document reader/writer and reference metadata parser.
// Managed YAML resources and persisted Worlds use this same settings format;
// this module contains no bundled configuration data or resource inventory.
#include "core/StationReference.h"

#include "core/SerializationException.h"
#include "core/WorldDocument.h"
#include "core/YamlSerializer.h"

#include <cmath>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace core
{
	namespace
	{
	void writeWaterQuality(Serializer& s, WaterQualitySettings const& gate)
	{
		if (gate.maxConcentrations.empty() && !gate.maxTotalDissolved) return;
		s.beginMap("waterQuality");
		if (gate.maxTotalDissolved) s.writeDouble("maxTotalDissolved", *gate.maxTotalDissolved);
		if (!gate.maxConcentrations.empty())
		{
			s.beginMap("maxConcentrations");
			for (auto const& [name, value] : gate.maxConcentrations) s.writeDouble(name, value);
			s.endMap();
		}
		s.endMap();
	}
	void readWaterQuality(Serializer& s, WaterQualitySettings& gate)
	{
		if (!s.hasField("waterQuality")) return;
		s.beginMap("waterQuality");
		if (s.hasField("maxTotalDissolved")) gate.maxTotalDissolved = s.readDouble("maxTotalDissolved");
		if (s.hasField("maxConcentrations"))
		{
			auto const keys = s.mapKeys("maxConcentrations");
			s.beginMap("maxConcentrations");
			for (auto const& key : keys) gate.maxConcentrations[key] = s.readDouble(key);
			s.endMap();
		}
		s.endMap();
	}
	// #843: the shared Wear settings block. Written only when the machine
	// opts in, so a feature-absent entry carries no new field and existing
	// fixtures stay byte-identical.
	void writeWearBlock(Serializer& s, std::string const& name,
		InstallationWearSettings const& model)
	{
		s.beginMap(name);
		s.writeDouble("fadePerCycle", model.fadePerCycle);
		s.writeDouble("fadePerOperatingHour", model.fadePerOperatingHour);
		s.writeDouble("fadePerThroughputKg", model.fadePerThroughputKg);
		s.writeDouble("fadePerCalendarHour", model.fadePerCalendarHour);
		s.writeDouble("maxWearFraction", model.maxWearFraction);
		s.endMap();
	}
	bool readWearBlock(Serializer& s, std::string const& name, uint32_t version,
		char const* feature, InstallationWearSettings& model)
	{
		if (!s.hasField(name)) return false;
		if (version < 116)
			throw SerializationException(std::string(feature) + " requires schema 116");
		s.beginMap(name);
		model.fadePerCycle = s.readDouble("fadePerCycle");
		model.fadePerOperatingHour = s.readDouble("fadePerOperatingHour");
		model.fadePerThroughputKg = s.readDouble("fadePerThroughputKg");
		model.fadePerCalendarHour = s.readDouble("fadePerCalendarHour");
		model.maxWearFraction = s.readDouble("maxWearFraction");
		s.endMap();
		return true;
	}
	// #848: one machine's catalogue Wear opt-in on the wire. The field is
	// named for the mode the machine opted into, so a document says which
	// mode it authored instead of leaving a bare `wear` to be guessed from
	// the collection. An absent field is an absent opt-in; a document older
	// than the catalogue is refused rather than silently read unworn.
	void readMachineWear(Serializer& s, std::string const& name, uint32_t version,
		std::optional<InstallationWearSettings>& field)
	{
		if (!s.hasField(name)) return;
		if (version < 121)
			throw SerializationException("Machine Wear opt-in '" + name + "' requires schema 121");
		InstallationWearSettings model;
		if (readWearBlock(s, name, version, "Machine Wear", model)) field = model;
	}
	// #844: the shared repair-parts wiring block, written only when the
	// machine opts in - the same shape the coolant loop carries since
	// #842, so one writer and one reader serve every repairable kind.
	void writeRepairPartsBlock(Serializer& s, RepairPartsSettings const& wiring)
	{
		s.beginMap("repairParts");
		s.writeString("partsStoreId", wiring.partsStoreId);
		s.beginArray("repairs");
		for (auto const& plan : wiring.repairs)
		{
			s.beginMap("");
			s.writeString("modeId", plan.modeId);
			s.writeDouble("durationSeconds", plan.durationSeconds);
			s.writeDouble("partsQuantity", plan.partsQuantity);
			s.writeString("wasteStoreId", plan.wasteStoreId);
			s.endMap();
		}
		s.endArray();
		s.endMap();
	}
	bool readRepairPartsBlock(Serializer& s, uint32_t version, RepairPartsSettings& wiring)
	{
		if (!s.hasField("repairParts")) return false;
		if (version < 117)
			throw SerializationException("Thermal-plant repair-parts wiring requires schema 117");
		s.beginMap("repairParts");
		wiring.partsStoreId = s.readString("partsStoreId");
		s.beginArray("repairs");
		while (s.nextArrayItem())
		{
			s.beginMap("");
			FaultRepairSettings plan;
			plan.modeId = s.readString("modeId");
			plan.durationSeconds = s.readDouble("durationSeconds");
			plan.partsQuantity = s.readDouble("partsQuantity");
			plan.wasteStoreId = s.readString("wasteStoreId");
			wiring.repairs.push_back(std::move(plan));
			s.endMap();
		}
		s.endArray();
		s.endMap();
		return true;
	}
	// #844: the shared finite-charge leak settings block for the
	// refrigerant leak and the panel puncture: same field shape, written
	// only when the machine opts in.
	void writeChargeLeakBlock(Serializer& s, std::string const& name,
		std::string const& compartmentId, double chargeKg, double leakRateKgPerS,
		double specificHeatKJPerKgK)
	{
		s.beginMap(name);
		s.writeString("compartmentId", compartmentId);
		s.writeDouble("chargeKg", chargeKg);
		s.writeDouble("leakRateKgPerS", leakRateKgPerS);
		s.writeDouble("specificHeatKJPerKgK", specificHeatKJPerKgK);
		s.endMap();
	}
	bool readChargeLeakBlock(Serializer& s, std::string const& name, uint32_t version,
		char const* feature, std::string& compartmentId, double& chargeKg,
		double& leakRateKgPerS, double& specificHeatKJPerKgK)
	{
		if (!s.hasField(name)) return false;
		if (version < 117)
			throw SerializationException(std::string(feature) + " requires schema 117");
		s.beginMap(name);
		compartmentId = s.readString("compartmentId");
		chargeKg = s.readDouble("chargeKg");
		leakRateKgPerS = s.readDouble("leakRateKgPerS");
		specificHeatKJPerKgK = s.readDouble("specificHeatKJPerKgK");
		s.endMap();
		return true;
	}
	}
	void writeStationSystemsSettingsMap(Serializer& serializer, StationSystemsSettings const& settings)
	{
		serializer.beginMap("stationSystems");
		serializer.writeBool("enabled", settings.enabled);
		if (!settings.monitors.empty())
		{
			serializer.beginArray("monitors");
			for (auto const& monitor : settings.monitors)
			{
				serializer.beginMap("");
				serializer.writeString("id", monitor.id);
				serializer.writeString("subjectType", monitor.subjectType);
				serializer.writeString("subjectId", monitor.subjectId);
				serializer.writeString("metric", monitor.metric);
				serializer.beginMap("rule");
				serializer.writeDouble("threshold", monitor.rule.threshold);
				serializer.writeString("direction", std::string(stationAlertDirectionTag(monitor.rule.direction)));
				serializer.writeDouble("triggerOnSeconds", monitor.rule.triggerOnSeconds);
				serializer.writeDouble("triggerOffSeconds", monitor.rule.triggerOffSeconds);
				serializer.endMap();
				serializer.writeString("severity", std::string(stationAlertSeverityTag(monitor.severity)));
				serializer.writeBool("enabled", monitor.enabled);
				serializer.writeString("provenance", std::string(stationMonitorProvenanceTag(monitor.provenance)));
				serializer.endMap();
			}
			serializer.endArray();
		}
		serializer.writeUint32("systemsStepWorldTicks", settings.systemsStepWorldTicks);
		serializer.writeFloat("observationIntervalSeconds", settings.observationIntervalSeconds);
		serializer.writeFloat("trendRetentionSeconds", settings.trendRetentionSeconds);
		// #415: the dispatch controller mode is authored settings; the
		// controller's runtime scalars and Alerts never travel.
		serializer.writeString("dispatchMode",
			settings.dispatchMode == StationDispatchMode::Automatic
				? "automatic" : "manual");
		if (!settings.connections.empty())
		{
			serializer.beginArray("connections");
			for (auto const& [key, properties] : settings.connections)
			{
				serializer.beginMap("");
				serializer.writeString("ownerType", key.ownerType);
				serializer.writeString("ownerId", key.ownerId);
				serializer.writeString("reference", key.reference);
				serializer.writeString("relation", key.relation);
				serializer.writeString("peerType", key.peerType);
				serializer.writeString("peerId", key.peerId);
				serializer.writeString("physicalType", StationPhysicalConnectionNames.at(static_cast<size_t>(properties.physicalType)));
				serializer.writeDouble("maximumThroughput", properties.maximumThroughput);
				serializer.writeDouble("efficiency", properties.efficiency);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.stores.empty())
		{
			// Only authored starting conditions travel; current inventory is
			// runtime state and is reconstructed from these values on load.
			serializer.beginArray("stores");
			for (auto const& store : settings.stores)
			{
				serializer.beginMap("");
				serializer.writeString("id", store.id);
				serializer.writeString("unit", store.unit);
				if (!store.materialId.empty()) serializer.writeString("materialId", store.materialId);
				serializer.writeDouble("capacity", store.capacity);
				serializer.writeDouble("startingQualified", store.startingQualified);
				serializer.writeDouble("startingUnqualified", store.startingUnqualified);
				serializer.writeDouble("protectedReserve", store.protectedReserve);
				if (store.composition)
				{
					// Authored composition only; constituent ledgers and
					// losses are runtime state reconstructed on load.
					serializer.beginMap("composition");
					serializer.writeDouble("moisture", store.composition->moisture);
					serializer.writeDouble("nutrientNitrogen", store.composition->nutrientNitrogen);
					serializer.writeDouble("atmosphericNitrogen", store.composition->atmosphericNitrogen);
					serializer.writeDouble("phosphorus", store.composition->phosphorus);
					serializer.writeDouble("potassium", store.composition->potassium);
					serializer.writeDouble("otherMinerals", store.composition->otherMinerals);
					serializer.writeDouble("energyKJPerKgDry", store.composition->energyKJPerKgDry);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.transfers.empty())
		{
			// Authored transfer rules only; per-tick results are runtime state.
			serializer.beginArray("transfers");
			for (auto const& transfer : settings.transfers)
			{
				serializer.beginMap("");
				serializer.writeString("id", transfer.id);
				serializer.writeString("sourceId", transfer.sourceId);
				serializer.writeString("destinationId", transfer.destinationId);
				serializer.writeString("unit", transfer.unit);
				serializer.writeDouble("ratePerSecond", transfer.ratePerSecond);
				if (transfer.mode != ResourceTransferMode::Continuous || transfer.quantity != 1.0
					|| transfer.firstOccurrenceSeconds != 0.0 || transfer.recurrenceSeconds != 0.0)
				{
					serializer.writeString("mode", transfer.mode == ResourceTransferMode::Scheduled ? "Scheduled" : "Continuous");
					serializer.writeDouble("quantity", transfer.quantity);
					serializer.writeDouble("firstOccurrenceSeconds", transfer.firstOccurrenceSeconds);
					serializer.writeDouble("recurrenceSeconds", transfer.recurrenceSeconds);
				}
				serializer.writeInt32("priority", transfer.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.conversions.empty())
		{
			// Authored conversion rules only; completed scales, flows and
			// constituent losses are runtime state.
			serializer.beginArray("conversions");
			for (auto const& conversion : settings.conversions)
			{
				serializer.beginMap("");
				serializer.writeString("id", conversion.id);
				serializer.writeInt32("priority", conversion.priority);
				serializer.beginArray("inputs");
				for (auto const& flow : conversion.inputs)
				{
					serializer.beginMap("");
					serializer.writeString("storeId", flow.storeId);
					serializer.writeDouble("ratePerSecond", flow.ratePerSecond);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.beginArray("outputs");
				for (auto const& flow : conversion.outputs)
				{
					serializer.beginMap("");
					serializer.writeString("storeId", flow.storeId);
					serializer.writeDouble("yieldPerSecond", flow.yieldPerSecond);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.solarArrays.empty())
		{
			// Authored solar configuration only; evaluated illumination and
			// delivered supply are runtime state.
			serializer.beginArray("solarArrays");
			for (auto const& array : settings.solarArrays)
			{
				serializer.beginMap("");
				serializer.writeString("id", array.id);
				if (array.cellDegradation) writeWearBlock(serializer, "cellDegradation", *array.cellDegradation);
				serializer.writeDouble("capacityKW", array.capacityKW);
				serializer.beginMap("illumination");
				serializer.writeDouble("constantLevel", array.illumination.constantLevel);
				serializer.writeDouble("periodSeconds", array.illumination.periodSeconds);
				serializer.beginArray("segments");
				for (auto const& segment : array.illumination.segments)
				{
					serializer.beginMap("");
					serializer.writeDouble("startSeconds", segment.startSeconds);
					serializer.writeDouble("level", segment.level);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				// #845: the array's fault opt-ins travel only when set, so
				// a feature-absent array keeps its pre-118 byte shape.
				if (array.stringLoss)
				{
					serializer.beginMap("stringLoss");
					serializer.writeDouble("lossFraction", array.stringLoss->lossFraction);
					serializer.endMap();
				}
				if (array.trackingFailure)
				{
					serializer.beginMap("trackingFailure");
					serializer.writeDouble("availability", array.trackingFailure->availability);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.equipmentLoads.empty())
		{
			// Authored load contracts only; allocation results, delivered
			// fractions and exchange flows are runtime state.
			serializer.beginArray("equipmentLoads");
			for (auto const& load : settings.equipmentLoads)
			{
				serializer.beginMap("");
				serializer.writeString("id", load.id);
				serializer.writeDouble("electricalDemandKW", load.electricalDemandKW);
				serializer.beginMap("operatingProfile");
				serializer.writeDouble("constantLevel", load.operatingProfile.constantLevel);
				serializer.writeDouble("periodSeconds", load.operatingProfile.periodSeconds);
				serializer.beginArray("segments");
				for (auto const& segment : load.operatingProfile.segments)
				{
					serializer.beginMap("");
					serializer.writeDouble("startSeconds", segment.startSeconds);
					serializer.writeDouble("level", segment.level);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				serializer.writeDouble("heatOutputKW", load.heatOutputKW);
				serializer.writeString("thermalClass", load.thermalClass);
				serializer.writeString("coolingDisposition",
					load.coolingDisposition == StationCoolingDisposition::Exported
						? "exported" : "coolingService");
				serializer.writeDouble("criticalFloorKW", load.criticalFloorKW);
				serializer.writeInt32("priority", load.priority);
				serializer.writeDouble("maxCurtailment", load.maxCurtailment);
				// #417 authored recovery rules. Only written when set, so
				// a load with no recovery stays byte-identical to a pre-93
				// load and never trips the pre-93 refusal on the read side.
				if (load.recoveryDwellSeconds != 0.0 || load.recoveryHysteresisFraction != 0.0
					|| load.restartSurgePowerKW != 0.0 || load.restartSurgeSeconds != 0.0)
				{
					serializer.writeDouble("recoveryDwellSeconds", load.recoveryDwellSeconds);
					serializer.writeDouble("recoveryHysteresisFraction", load.recoveryHysteresisFraction);
					serializer.writeDouble("restartSurgePowerKW", load.restartSurgePowerKW);
					serializer.writeDouble("restartSurgeSeconds", load.restartSurgeSeconds);
				}
				serializer.beginArray("materialExchanges");
				for (auto const& exchange : load.materialExchanges)
				{
					serializer.beginMap("");
					serializer.writeString("storeId", exchange.storeId);
					serializer.writeDouble("ratePerSecond", exchange.ratePerSecond);
					serializer.writeBool("consumes", exchange.consumes);
					serializer.endMap();
				}
				serializer.endArray();
				// Authored heat-sink routing (#395): which reservoir
				// receives this load's emitted heat. Routed temperatures
				// and ledgers are runtime state.
				serializer.writeString("heatSinkId", load.heatSinkId);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.thermalReservoirs.empty())
		{
			// Authored reservoir contracts only; integrated temperature
			// and the energy ledger are runtime state (#395).
			serializer.beginArray("thermalReservoirs");
			for (auto const& reservoir : settings.thermalReservoirs)
			{
				serializer.beginMap("");
				serializer.writeString("id", reservoir.id);
				serializer.writeString("thermalClass", reservoir.thermalClass);
				serializer.writeDouble("heatCapacityKJPerK", reservoir.heatCapacityKJPerK);
				serializer.writeDouble("startingTemperatureC", reservoir.startingTemperatureC);
				serializer.writeDouble("minTemperatureC", reservoir.minTemperatureC);
				serializer.writeDouble("maxTemperatureC", reservoir.maxTemperatureC);
				serializer.writeDouble("absorbedHeatKW", reservoir.absorbedHeatKW);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.radiators.empty())
		{
			// Authored radiator contracts only; per-tick rejection is
			// runtime state (#395).
			serializer.beginArray("radiators");
			for (auto const& radiator : settings.radiators)
			{
				serializer.beginMap("");
				serializer.writeString("id", radiator.id);
				serializer.writeString("reservoirId", radiator.reservoirId);
				serializer.writeDouble("areaM2", radiator.areaM2);
				serializer.writeDouble("emissivity", radiator.emissivity);
				serializer.writeDouble("viewFactor", radiator.viewFactor);
				serializer.writeDouble("sinkTemperatureC", radiator.sinkTemperatureC);
				// #844: the puncture opt-in and repair wiring travel only
				// when present, so a feature-absent radiator keeps its
				// previous byte shape.
				if (radiator.panelPuncture)
					writeChargeLeakBlock(serializer, "panelPuncture",
						radiator.panelPuncture->compartmentId,
						radiator.panelPuncture->coolantKg,
						radiator.panelPuncture->leakRateKgPerS,
						radiator.panelPuncture->specificHeatKJPerKgK);
				if (radiator.repairParts)
					writeRepairPartsBlock(serializer, *radiator.repairParts);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.coolantLoops.empty())
		{
			// Authored loop contracts only; coolant inventory,
			// circulation, freeze exposure and the loop energy ledger
			// are runtime state reconstructed on load (#396).
			serializer.beginArray("coolantLoops");
			for (auto const& loop : settings.coolantLoops)
			{
				serializer.beginMap("");
				serializer.writeString("id", loop.id);
				serializer.writeString("reservoirId", loop.reservoirId);
				serializer.writeDouble("coolantCapacityKg", loop.coolantCapacityKg);
				serializer.writeDouble("startingCoolantKg", loop.startingCoolantKg);
				serializer.writeDouble("specificHeatKJPerKgK", loop.specificHeatKJPerKgK);
				serializer.writeDouble("startingTemperatureC", loop.startingTemperatureC);
				serializer.writeDouble("freezeProtectionSetpointC", loop.freezeProtectionSetpointC);
				serializer.writeDouble("pumpPowerKW", loop.pumpPowerKW);
				serializer.writeDouble("ratedFlowKgPerS", loop.ratedFlowKgPerS);
				serializer.writeDouble("exchangerConductanceKWPerK", loop.exchangerConductanceKWPerK);
				serializer.writeDouble("heaterCapacityKW", loop.heaterCapacityKW);
				// #416: the protected flag travels only when set, so a
				// plain loop never carries a field older readers (and
				// older-version guard tests) would trip over; a document
				// that does carry it must be schema 92 or newer.
				if (loop.criticalService)
					serializer.writeBool("criticalService", true);
				if (loop.coolantLeak)
				{
					serializer.beginMap("coolantLeak");
					serializer.writeString("compartmentId", loop.coolantLeak->compartmentId);
					serializer.writeDouble("releasedKg", loop.coolantLeak->releasedKg);
					serializer.endMap();
				}
				// #840: the Condition model travels only when the loop has
				// one, so a feature-absent loop carries no new field and
				// existing fixtures stay byte-identical.
				if (loop.condition)
				{
					serializer.beginMap("condition");
					serializer.writeDouble("initialCondition", loop.condition->initialCondition);
					serializer.writeDouble("overheatStressC", loop.condition->overheatStressC);
					serializer.writeDouble("overheatDegradePerHour", loop.condition->overheatDegradePerHour);
					serializer.writeDouble("cavitationFillFraction", loop.condition->cavitationFillFraction);
					serializer.writeDouble("cavitationDegradePerHour", loop.condition->cavitationDegradePerHour);
					serializer.writeDouble("seizureThreshold", loop.condition->seizureThreshold);
					serializer.writeString("restoreStoreId", loop.condition->restoreStoreId);
					serializer.writeDouble("restorePerHour", loop.condition->restorePerHour);
					serializer.writeDouble("stockUnitsPerConditionPoint", loop.condition->stockUnitsPerConditionPoint);
					serializer.endMap();
				}
				// #842: the repair-parts wiring travels only when the loop
				// has one, so a feature-absent loop carries no new field.
				if (loop.repairParts)
				{
					serializer.beginMap("repairParts");
					serializer.writeString("partsStoreId", loop.repairParts->partsStoreId);
					serializer.beginArray("repairs");
					for (auto const& plan : loop.repairParts->repairs)
					{
						serializer.beginMap("");
						serializer.writeString("modeId", plan.modeId);
						serializer.writeDouble("durationSeconds", plan.durationSeconds);
						serializer.writeDouble("partsQuantity", plan.partsQuantity);
						serializer.writeString("wasteStoreId", plan.wasteStoreId);
						serializer.endMap();
					}
					serializer.endArray();
					serializer.endMap();
				}
				// #843: the Wear opt-in travels only when the loop has one.
				if (loop.wear)
					writeWearBlock(serializer, "wear", *loop.wear);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.chillers.empty())
		{
			// Authored chiller contracts only; cold-side duty, work,
			// condenser heat and the chiller energy ledger are runtime
			// state reconstructed on load (#397).
			serializer.beginArray("chillers");
			for (auto const& chiller : settings.chillers)
			{
				serializer.beginMap("");
				serializer.writeString("id", chiller.id);
				serializer.writeString("thermalClass", chiller.thermalClass);
				serializer.writeString("coldLoopId", chiller.coldLoopId);
				serializer.writeString("condenserReservoirId", chiller.condenserReservoirId);
				serializer.writeDouble("ratedCoolingCapacityKW", chiller.ratedCoolingCapacityKW);
				serializer.writeDouble("cop", chiller.cop);
				serializer.writeDouble("auxiliaryPowerKW", chiller.auxiliaryPowerKW);
				serializer.writeDouble("chilledSetpointC", chiller.chilledSetpointC);
				serializer.writeDouble("maxCondensingTemperatureC", chiller.maxCondensingTemperatureC);
				// #416: the protected flag travels only when set; see
				// the coolant loop note above.
				if (chiller.criticalService)
					serializer.writeBool("criticalService", true);
				// #844: the fault opt-ins travel only when present, so a
				// feature-absent chiller keeps its previous byte shape.
				if (chiller.compressorFailure)
					serializer.writeBool("compressorFailure", true);
				if (chiller.refrigerantLeak)
					writeChargeLeakBlock(serializer, "refrigerantLeak",
						chiller.refrigerantLeak->compartmentId,
						chiller.refrigerantLeak->chargeKg,
						chiller.refrigerantLeak->leakRateKgPerS,
						chiller.refrigerantLeak->specificHeatKJPerKgK);
				if (chiller.repairParts)
					writeRepairPartsBlock(serializer, *chiller.repairParts);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.heatEngines.empty())
		{
			// Authored heat engine contracts only (#804): the intake, net
			// output, rejection heat, status and the conversion ledger are
			// runtime state reconstructed on load, like every other ledger.
			serializer.beginArray("heatEngines");
			for (auto const& engine : settings.heatEngines)
			{
				serializer.beginMap("");
				serializer.writeString("id", engine.id);
				if (engine.efficiencyDrift) writeWearBlock(serializer, "efficiencyDrift", *engine.efficiencyDrift);
				serializer.writeString("hotReservoirId", engine.hotReservoirId);
				serializer.writeString("rejectionReservoirId", engine.rejectionReservoirId);
				serializer.writeString("thermalClass", engine.thermalClass);
				serializer.writeDouble("ratedThermalInputKW", engine.ratedThermalInputKW);
				// #805: the intake dispatch travels only when the engine is
				// actually throttled below rated; an absent field means full
				// rated, so an unthrottled engine keeps the compact shape.
				if (engine.commandedInputKW != engine.ratedThermalInputKW)
					serializer.writeDouble("commandedInputKW", engine.commandedInputKW);
				serializer.writeDouble("carnotFraction", engine.carnotFraction);
				serializer.writeDouble("auxiliaryPowerKW", engine.auxiliaryPowerKW);
				serializer.writeDouble("minHotTemperatureC", engine.minHotTemperatureC);
				serializer.writeDouble("maxSinkTemperatureC", engine.maxSinkTemperatureC);
				// #844: the seizure opt-in and repair wiring travel only
				// when present, so a feature-absent engine keeps its
				// previous byte shape.
				if (engine.mechanicalSeizure)
					serializer.writeBool("mechanicalSeizure", true);
				if (engine.repairParts)
					writeRepairPartsBlock(serializer, *engine.repairParts);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.generators.empty())
		{
			// Authored generator contracts only; phase, output, fuel
			// draws and the fuel/heat ledgers are runtime state
			// reconstructed on load (#399).
			serializer.beginArray("generators");
			for (auto const& generator : settings.generators)
			{
				serializer.beginMap("");
				serializer.writeString("id", generator.id);
				if (generator.fouledInjector) writeWearBlock(serializer, "fouledInjector", *generator.fouledInjector);
				serializer.writeString("fuelStoreId", generator.fuelStoreId);
				serializer.writeString("thermalClass", generator.thermalClass);
				serializer.writeString("heatSinkId", generator.heatSinkId);
				serializer.writeDouble("ratedOutputKW", generator.ratedOutputKW);
				serializer.writeDouble("efficiency", generator.efficiency);
				serializer.writeDouble("maxRampKWPerSecond", generator.maxRampKWPerSecond);
				serializer.writeDouble("commandedOutputKW", generator.commandedOutputKW);
				serializer.writeDouble("startupPowerKW", generator.startupPowerKW);
				serializer.writeDouble("startupSeconds", generator.startupSeconds);
				serializer.writeDouble("shutdownHeatKW", generator.shutdownHeatKW);
				serializer.writeDouble("shutdownSeconds", generator.shutdownSeconds);
				serializer.writeDouble("maxSinkTemperatureC", generator.maxSinkTemperatureC);
				// #845: the failed-start opt-in travels only when set.
				if (generator.failedStart)
					serializer.writeBool("failedStart", true);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.atmospheres.empty())
		{
			// Authored compartment contracts only; current gas
			// inventories, derived pressures, humidity and the
			// containment ledgers are runtime state reconstructed on
			// load (#400).
			serializer.beginArray("atmospheres");
			for (auto const& atmosphere : settings.atmospheres)
			{
				serializer.beginMap("");
				serializer.writeString("id", atmosphere.id);
				serializer.writeDouble("volumeM3", atmosphere.volumeM3);
				serializer.writeDouble("startingOxygenKg", atmosphere.startingOxygenKg);
				serializer.writeDouble("startingNitrogenKg", atmosphere.startingNitrogenKg);
				serializer.writeDouble("startingCarbonDioxideKg", atmosphere.startingCarbonDioxideKg);
				serializer.writeDouble("startingWaterVapourKg", atmosphere.startingWaterVapourKg);
				serializer.writeDouble("startingTemperatureC", atmosphere.startingTemperatureC);
				serializer.writeDouble("thermalCapacityKJPerK", atmosphere.thermalCapacityKJPerK);
				// Advected bulk specific heat (#802), new at schema 108.
				serializer.writeDouble("advectedSpecificHeatKJPerKgK",
					atmosphere.advectedSpecificHeatKJPerKgK);
				serializer.writeString("reservoirId", atmosphere.reservoirId);
				serializer.writeDouble("exchangerConductanceKWPerK", atmosphere.exchangerConductanceKWPerK);
				serializer.writeInt32("occupants", atmosphere.occupants);
				serializer.writeDouble("oxygenUsePerOccupantKgPerS", atmosphere.oxygenUsePerOccupantKgPerS);
				serializer.writeDouble("co2ProductionPerOccupantKgPerS", atmosphere.co2ProductionPerOccupantKgPerS);
				serializer.writeDouble("sensibleHeatPerOccupantKW", atmosphere.sensibleHeatPerOccupantKW);
				serializer.writeDouble("moistureAdditionPerOccupantKgPerS", atmosphere.moistureAdditionPerOccupantKgPerS);
				serializer.writeDouble("leakRateKgPerS", atmosphere.leakRateKgPerS);
				serializer.writeDouble("minPressureKPa", atmosphere.minPressureKPa);
				serializer.writeDouble("maxPressureKPa", atmosphere.maxPressureKPa);
				serializer.writeDouble("minOxygenPartialPressureKPa", atmosphere.minOxygenPartialPressureKPa);
				serializer.writeDouble("maxCarbonDioxidePartialPressureKPa", atmosphere.maxCarbonDioxidePartialPressureKPa);
				if (atmosphere.fire)
				{
					serializer.beginMap("fire");
					serializer.writeDouble("oxygenKgPerS", atmosphere.fire->oxygenKgPerS);
					serializer.writeDouble("carbonMonoxideKgPerS", atmosphere.fire->carbonMonoxideKgPerS);
					serializer.writeDouble("carbonDioxideKgPerS", atmosphere.fire->carbonDioxideKgPerS);
					serializer.writeDouble("heatKW", atmosphere.fire->heatKW);
					serializer.writeDouble("oxygenThresholdKg", atmosphere.fire->oxygenThresholdKg);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.atmospherePathways.empty())
		{
			serializer.beginArray("atmospherePathways");
			for (auto const& pathway : settings.atmospherePathways)
			{
				serializer.beginMap("");
				serializer.writeString("id", pathway.id);
				serializer.writeString("aAtmosphereId", pathway.aAtmosphereId);
				serializer.writeString("bAtmosphereId", pathway.bAtmosphereId);
				serializer.writeDouble("areaM2", pathway.areaM2);
				serializer.writeDouble("openFraction", pathway.openFraction);
				serializer.endMap();
			}
			serializer.endArray();
		}
		// Authored Thermal link contracts only (#794): the conducted flow,
		// cumulative energy and status are runtime state reconstructed
		// on load, like every other ledger.
		if (!settings.thermalLinks.empty())
		{
			serializer.beginArray("thermalLinks");
			for (auto const& link : settings.thermalLinks)
			{
				serializer.beginMap("");
				serializer.writeString("id", link.id);
				serializer.writeString("aAtmosphereId", link.aAtmosphereId);
				serializer.writeString("bAtmosphereId", link.bAtmosphereId);
				auto writeKind = [&](char const* field, ThermalNodeKind kind) {
					if (kind != ThermalNodeKind::Atmosphere)
						serializer.writeString(field, kind == ThermalNodeKind::Reservoir ? "reservoir" : "machineBody");
				};
				writeKind("aKind", link.aKind);
				writeKind("bKind", link.bKind);
				serializer.writeDouble("conductanceKWPerK", link.conductanceKWPerK);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.airlockCycles.empty())
		{
			// Authored airlock cycle contracts only; fired occurrences
			// and the removed/recovered/lost amounts are runtime state
			// reconstructed on load (#400).
			serializer.beginArray("airlockCycles");
			for (auto const& cycle : settings.airlockCycles)
			{
				serializer.beginMap("");
				serializer.writeString("id", cycle.id);
				serializer.writeString("atmosphereId", cycle.atmosphereId);
				serializer.writeDouble("gasMassKg", cycle.gasMassKg);
				serializer.writeDouble("firstOccurrenceSeconds", cycle.firstOccurrenceSeconds);
				serializer.writeDouble("recurrenceSeconds", cycle.recurrenceSeconds);
				serializer.writeDouble("recoveredFraction", cycle.recoveredFraction);
				serializer.writeInt32("priority", cycle.priority);
				if (cycle.interruptedCycle) serializer.writeBool("interruptedCycle", true);
				if (cycle.interlockFailure)
				{
					serializer.beginMap("interlockFailure");
					serializer.writeString("pathwayId", cycle.interlockFailure->pathwayId);
					serializer.writeDouble("breachAreaM2", cycle.interlockFailure->breachAreaM2);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.crews.empty())
		{
			// Authored crew contracts only; consumption, returns,
			// the metabolic scale and the retained-mass ledger are
			// runtime state reconstructed on load (#401).
			serializer.beginArray("crews");
			for (auto const& crew : settings.crews)
			{
				serializer.beginMap("");
				serializer.writeString("id", crew.id);
				serializer.writeString("atmosphereId", crew.atmosphereId);
				serializer.writeInt32("crewCount", crew.crewCount);
				serializer.beginMap("activity");
				serializer.writeDouble("constantLevel", crew.activityProfile.constantLevel);
				serializer.writeDouble("periodSeconds", crew.activityProfile.periodSeconds);
				serializer.beginArray("segments");
				for (auto const& segment : crew.activityProfile.segments)
				{
					serializer.beginMap("");
					serializer.writeDouble("startSeconds", segment.startSeconds);
					serializer.writeDouble("level", segment.level);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				serializer.writeString("foodStoreId", crew.foodStoreId);
				serializer.writeString("waterStoreId", crew.waterStoreId);
				writeWaterQuality(serializer, crew.waterQuality);
				serializer.writeString("greywaterStoreId", crew.greywaterStoreId);
				serializer.writeDouble("greywaterKgPerCrewDay", crew.greywaterKgPerCrewDay);
				serializer.beginMap("greywaterLoadKgPerCrewDay");
				for (auto const& [name, mass] : crew.greywaterLoadKgPerCrewDay) serializer.writeDouble(name, mass);
				serializer.endMap();
				serializer.writeString("urineStoreId", crew.urineStoreId);
				serializer.writeString("organicWasteStoreId", crew.organicWasteStoreId);
				serializer.writeDouble("foodPerCrewPerS", crew.foodPerCrewPerS);
				serializer.writeDouble("waterPerCrewPerS", crew.waterPerCrewPerS);
				serializer.writeDouble("oxygenPerCrewKgPerS", crew.oxygenPerCrewKgPerS);
				serializer.writeDouble("co2PerCrewKgPerS", crew.co2PerCrewKgPerS);
				serializer.writeDouble("moisturePerCrewKgPerS", crew.moisturePerCrewKgPerS);
				serializer.writeDouble("urinePerCrewPerS", crew.urinePerCrewPerS);
				serializer.writeDouble("organicWastePerCrewPerS", crew.organicWastePerCrewPerS);
				serializer.writeDouble("metabolicHeatPerCrewKW", crew.metabolicHeatPerCrewKW);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.waterCollectors.empty())
		{
			// Authored water collector contracts only; the collected
			// mass, the latent heat routed and the gating conditions are
			// runtime state reconstructed on load (#402).
			serializer.beginArray("waterCollectors");
			for (auto const& collector : settings.waterCollectors)
			{
				serializer.beginMap("");
				serializer.writeString("id", collector.id);
				serializer.writeString("atmosphereId", collector.atmosphereId);
				serializer.writeString("coolingLoopId", collector.coolingLoopId);
				serializer.writeString("thermalClass", collector.thermalClass);
				serializer.writeString("destStoreId", collector.destStoreId);
				serializer.writeDouble("maxRateKgPerS", collector.maxRateKgPerS);
				serializer.writeDouble("targetRelativeHumidity", collector.targetRelativeHumidity);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.waterTreatments.empty())
		{
			// Authored water treatment contracts only; the processed,
			// treated, accepted, held and concentrate totals, the energy
			// and consumables drawn and the quality holds are runtime
			// state reconstructed on load (#402).
			serializer.beginArray("waterTreatments");
			for (auto const& treatment : settings.waterTreatments)
			{
				serializer.beginMap("");
				serializer.writeString("id", treatment.id);
				if (treatment.membraneFouling) writeWearBlock(serializer, "membraneFouling", *treatment.membraneFouling);
				serializer.writeString("sourceStoreId", treatment.sourceStoreId);
				serializer.writeString("treatedStoreId", treatment.treatedStoreId);
				serializer.writeString("concentrateStoreId", treatment.concentrateStoreId);
				serializer.writeString("consumableStoreId", treatment.consumableStoreId);
				serializer.writeDouble("throughputKgPerS", treatment.throughputKgPerS);
				serializer.writeDouble("recoveryRatio", treatment.recoveryRatio);
				if (!treatment.removalFractions.empty())
				{
					serializer.beginMap("removalFractions");
					for (auto const& [name, fraction] : treatment.removalFractions)
						serializer.writeDouble(name, fraction);
					serializer.endMap();
				}
				serializer.writeDouble("specificEnergyKWhPerKg", treatment.specificEnergyKWhPerKg);
				serializer.writeDouble("consumablePerKg", treatment.consumablePerKg);
				serializer.writeInt32("priority", treatment.priority);
				if (treatment.breakthrough) serializer.writeBool("breakthrough", true);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.electrolyzers.empty())
		{
			// Authored electrolyzer contracts only; the water split,
			// the products made, the energy supplied and the process
			// heat routed are runtime state reconstructed on load
			// (#404).
			serializer.beginArray("electrolyzers");
			for (auto const& electrolyzer : settings.electrolyzers)
			{
				serializer.beginMap("");
				serializer.writeString("id", electrolyzer.id);
				if (electrolyzer.membraneDegradation) writeWearBlock(serializer, "membraneDegradation", *electrolyzer.membraneDegradation);
				serializer.writeString("waterStoreId", electrolyzer.waterStoreId);
				serializer.writeString("oxygenStoreId", electrolyzer.oxygenStoreId);
				serializer.writeString("hydrogenStoreId", electrolyzer.hydrogenStoreId);
				serializer.writeString("atmosphereId", electrolyzer.atmosphereId);
				serializer.writeString("thermalClass", electrolyzer.thermalClass);
				serializer.writeString("heatSinkId", electrolyzer.heatSinkId);
				serializer.writeDouble("rateKgPerS", electrolyzer.rateKgPerS);
				serializer.writeDouble("specificEnergyKWhPerKgWater",
					electrolyzer.specificEnergyKWhPerKgWater);
				serializer.writeDouble("maxSinkTemperatureC", electrolyzer.maxSinkTemperatureC);
				serializer.writeInt32("priority", electrolyzer.priority);
				if (electrolyzer.catalystPoisoning)
				{
					serializer.beginMap("catalystPoisoning");
					serializer.writeDouble("triggerConcentration", electrolyzer.catalystPoisoning->triggerConcentration);
					serializer.writeDouble("wearPerHour", electrolyzer.catalystPoisoning->wearPerHour);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.co2Captures.empty())
		{
			// Authored CO2 capture contracts only; the captured mass,
			// the unmet atmospheric service and the electricity and
			// consumables drawn are runtime state reconstructed on
			// load (#404).
			serializer.beginArray("co2Captures");
			for (auto const& capture : settings.co2Captures)
			{
				serializer.beginMap("");
				serializer.writeString("id", capture.id);
				if (capture.absorbentDegradation) writeWearBlock(serializer, "absorbentDegradation", *capture.absorbentDegradation);
				serializer.writeString("atmosphereId", capture.atmosphereId);
				serializer.writeString("captureStoreId", capture.captureStoreId);
				serializer.writeString("consumableStoreId", capture.consumableStoreId);
				serializer.writeDouble("maxRateKgPerS", capture.maxRateKgPerS);
				serializer.writeDouble("specificEnergyKWhPerKg", capture.specificEnergyKWhPerKg);
				serializer.writeDouble("consumablePerKg", capture.consumablePerKg);
				serializer.writeInt32("priority", capture.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.fuelCells.empty())
		{
			// Authored fuel-cell backup contracts only; the consumed
			// reactants, delivered water and power, routed heat and
			// the endurance clock are runtime state reconstructed on
			// load (#406).
			serializer.beginArray("fuelCells");
			for (auto const& cell : settings.fuelCells)
			{
				serializer.beginMap("");
				serializer.writeString("id", cell.id);
				if (cell.stackDegradation) writeWearBlock(serializer, "stackDegradation", *cell.stackDegradation);
				serializer.writeString("hydrogenStoreId", cell.hydrogenStoreId);
				serializer.writeString("oxygenStoreId", cell.oxygenStoreId);
				serializer.writeString("waterStoreId", cell.waterStoreId);
				serializer.writeString("thermalClass", cell.thermalClass);
				serializer.writeString("heatSinkId", cell.heatSinkId);
				serializer.writeDouble("ratedOutputKW", cell.ratedOutputKW);
				serializer.writeDouble("efficiency", cell.efficiency);
				serializer.writeDouble("commandedOutputKW", cell.commandedOutputKW);
				serializer.writeDouble("maxSinkTemperatureC", cell.maxSinkTemperatureC);
				if (cell.catalystPoisoning)
				{
					serializer.beginMap("catalystPoisoning");
					serializer.writeDouble("triggerConcentration", cell.catalystPoisoning->triggerConcentration);
					serializer.writeDouble("wearPerHour", cell.catalystPoisoning->wearPerHour);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.sabatierReactors.empty())
		{
			// Authored Sabatier reactor contracts only; the consumed
			// reactants, produced products and the energy and heat
			// ledgers are runtime state reconstructed on load (#405).
			serializer.beginArray("sabatierReactors");
			for (auto const& reactor : settings.sabatierReactors)
			{
				serializer.beginMap("");
				serializer.writeString("id", reactor.id);
				if (reactor.catalystDeactivation) writeWearBlock(serializer, "catalystDeactivation", *reactor.catalystDeactivation);
				serializer.writeString("co2StoreId", reactor.co2StoreId);
				serializer.writeString("hydrogenStoreId", reactor.hydrogenStoreId);
				serializer.writeString("methaneStoreId", reactor.methaneStoreId);
				serializer.writeString("waterStoreId", reactor.waterStoreId);
				serializer.writeString("thermalClass", reactor.thermalClass);
				serializer.writeString("heatSinkId", reactor.heatSinkId);
				serializer.writeDouble("rateKgPerS", reactor.rateKgPerS);
				serializer.writeDouble("specificEnergyKWhPerKgCO2",
					reactor.specificEnergyKWhPerKgCO2);
				serializer.writeDouble("maxSinkTemperatureC", reactor.maxSinkTemperatureC);
				if (reactor.overtempRunaway)
				{
					serializer.beginMap("overtempRunaway");
					serializer.writeDouble("triggerTemperatureC", reactor.overtempRunaway->triggerTemperatureC);
					serializer.writeDouble("heatSpikeKJ", reactor.overtempRunaway->heatSpikeKJ);
					serializer.endMap();
				}
				serializer.writeInt32("priority", reactor.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.methaneProcessors.empty())
		{
			// Authored methane processor contracts only; the split,
			// returned hydrogen, produced carbon and the energy and
			// heat ledgers are runtime state reconstructed on load (#405).
			serializer.beginArray("methaneProcessors");
			for (auto const& processor : settings.methaneProcessors)
			{
				serializer.beginMap("");
				serializer.writeString("id", processor.id);
				if (processor.catalystDeactivation) writeWearBlock(serializer, "catalystDeactivation", *processor.catalystDeactivation);
				serializer.writeString("methaneStoreId", processor.methaneStoreId);
				serializer.writeString("hydrogenStoreId", processor.hydrogenStoreId);
				serializer.writeString("carbonStoreId", processor.carbonStoreId);
				serializer.writeString("thermalClass", processor.thermalClass);
				serializer.writeString("heatSinkId", processor.heatSinkId);
				serializer.writeDouble("rateKgPerS", processor.rateKgPerS);
				serializer.writeDouble("specificEnergyKWhPerKgCH4",
					processor.specificEnergyKWhPerKgCH4);
				serializer.writeDouble("maxSinkTemperatureC", processor.maxSinkTemperatureC);
				if (processor.overtempRunaway)
				{
					serializer.beginMap("overtempRunaway");
					serializer.writeDouble("triggerTemperatureC", processor.overtempRunaway->triggerTemperatureC);
					serializer.writeDouble("heatSpikeKJ", processor.overtempRunaway->heatSpikeKJ);
					serializer.endMap();
				}
				serializer.writeInt32("priority", processor.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.microbialProcessors.empty())
		{
			// Authored microbial processor contracts only; the consumed
			// feeds, recovered products, per-constituent recovery and
			// the energy and heat ledgers are runtime state
			// reconstructed on load (#407).
			serializer.beginArray("microbialProcessors");
			for (auto const& processor : settings.microbialProcessors)
			{
				serializer.beginMap("");
				serializer.writeString("id", processor.id);
				serializer.writeString("rateBasis", stationRateBasisName(processor.rateBasis));
				serializer.writeDouble("organicOxidationFraction", processor.organicOxidationFraction);
				serializer.writeDouble("o2PerKgOrganicOxidized", processor.o2PerKgOrganicOxidized);
				serializer.writeDouble("nitrificationFraction", processor.nitrificationFraction);
				serializer.writeDouble("o2PerKgNitrifiedN", processor.o2PerKgNitrifiedN);
				serializer.writeDouble("co2PerKgOrganicOxidized", processor.co2PerKgOrganicOxidized);
				serializer.writeDouble("processWaterTargetMoisture", processor.processWaterTargetMoisture);
				serializer.writeDouble("heatReleaseFraction", processor.heatReleaseFraction);
				serializer.writeDouble("consumablePerKgOrganicOxidized", processor.consumablePerKgOrganicOxidized);
				serializer.writeString("feedStoreId", processor.feedStoreId);
				serializer.writeString("oxygenStoreId", processor.oxygenStoreId);
				serializer.writeString("waterStoreId", processor.waterStoreId);
				writeWaterQuality(serializer, processor.waterQuality);
				serializer.writeString("consumableStoreId", processor.consumableStoreId);
				serializer.writeString("nutrientStoreId", processor.nutrientStoreId);
				serializer.beginMap("recoveredWaterCarryoverFractions");
				for (auto const& [name, fraction] : processor.recoveredWaterCarryoverFractions) serializer.writeDouble(name, fraction);
				serializer.endMap();
				serializer.writeString("recoveredWaterStoreId", processor.recoveredWaterStoreId);
				serializer.writeString("co2StoreId", processor.co2StoreId);
				serializer.writeString("residueStoreId", processor.residueStoreId);
				serializer.writeString("thermalClass", processor.thermalClass);
				serializer.writeString("heatSinkId", processor.heatSinkId);
				serializer.writeDouble("rateKgPerS", processor.rateKgPerS);
				serializer.writeDouble("specificEnergyKWhPerKgFeed",
					processor.specificEnergyKWhPerKgFeed);
				serializer.writeDouble("oxygenPerKgFeedKg", processor.oxygenPerKgFeedKg);
				serializer.writeDouble("processWaterPerKgFeedKg",
					processor.processWaterPerKgFeedKg);
				serializer.writeDouble("consumablePerKgFeed", processor.consumablePerKgFeed);
				serializer.writeDouble("co2PerKgFeedKg", processor.co2PerKgFeedKg);
				serializer.writeDouble("nitrogenRecovery", processor.nitrogenRecovery);
				serializer.writeDouble("phosphorusRecovery", processor.phosphorusRecovery);
				serializer.writeDouble("potassiumRecovery", processor.potassiumRecovery);
				serializer.writeDouble("mineralsRecovery", processor.mineralsRecovery);
				serializer.writeDouble("waterRecovery", processor.waterRecovery);
				serializer.writeDouble("reactionHeatKJPerKgFeed",
					processor.reactionHeatKJPerKgFeed);
				serializer.writeDouble("minOperatingTemperatureC",
					processor.minOperatingTemperatureC);
				serializer.writeDouble("maxSinkTemperatureC", processor.maxSinkTemperatureC);
				serializer.writeDouble("initialCondition", processor.initialCondition);
				serializer.writeDouble("starvationDegradePerHour",
					processor.starvationDegradePerHour);
				serializer.writeDouble("consumableExhaustionDegradePerHour",
					processor.consumableExhaustionDegradePerHour);
				serializer.writeDouble("coldStressDegradePerHour",
					processor.coldStressDegradePerHour);
				serializer.writeDouble("heatStressDegradePerHour",
					processor.heatStressDegradePerHour);
				serializer.writeDouble("recoveryPerHour", processor.recoveryPerHour);
				serializer.writeString("stockStoreId", processor.stockStoreId);
				serializer.writeDouble("stockUnitsPerConditionPoint",
					processor.stockUnitsPerConditionPoint);
				serializer.writeInt32("priority", processor.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.cropPopulations.empty())
		{
			// Authored crop population contracts only; the Living
			// biomass trajectory, the growth and respiration pathway
			// ledgers and the energy and heat totals are runtime state
			// reconstructed on load (#409).
			serializer.beginArray("cropPopulations");
			for (auto const& crop : settings.cropPopulations)
			{
				serializer.beginMap("");
				serializer.writeString("id", crop.id);
				serializer.writeString("atmosphereId", crop.atmosphereId);
				serializer.writeString("waterStoreId", crop.waterStoreId);
				writeWaterQuality(serializer, crop.waterQuality);
				if (!crop.irrigationUptakeFractions.empty())
				{
					serializer.beginMap("irrigationUptakeFractions");
					for (auto const& [name, fraction] : crop.irrigationUptakeFractions)
						serializer.writeDouble(name, fraction);
					serializer.endMap();
				}
				serializer.writeString("nitrogenStoreId", crop.nitrogenStoreId);
				serializer.writeString("phosphorusStoreId", crop.phosphorusStoreId);
				serializer.writeString("potassiumStoreId", crop.potassiumStoreId);
				serializer.writeString("mineralsStoreId", crop.mineralsStoreId);
				serializer.beginMap("composition");
				serializer.writeDouble("moisture", crop.composition.moisture);
				serializer.writeDouble("nutrientNitrogen", crop.composition.nutrientNitrogen);
				serializer.writeDouble("atmosphericNitrogen", crop.composition.atmosphericNitrogen);
				serializer.writeDouble("phosphorus", crop.composition.phosphorus);
				serializer.writeDouble("potassium", crop.composition.potassium);
				serializer.writeDouble("otherMinerals", crop.composition.otherMinerals);
				serializer.writeDouble("energyKJPerKgDry", crop.composition.energyKJPerKgDry);
				serializer.endMap();
				serializer.writeDouble("growingAreaM2", crop.growingAreaM2);
				serializer.writeDouble("maxBiomassDensityKgPerM2", crop.maxBiomassDensityKgPerM2);
				serializer.writeDouble("initialBiomassKg", crop.initialBiomassKg);
				serializer.writeDouble("lampRatedKW", crop.lampRatedKW);
				serializer.beginMap("illumination");
				serializer.writeDouble("constantLevel", crop.illuminationProfile.constantLevel);
				serializer.writeDouble("periodSeconds", crop.illuminationProfile.periodSeconds);
				serializer.beginArray("segments");
				for (auto const& segment : crop.illuminationProfile.segments)
				{
					serializer.beginMap("");
					serializer.writeDouble("startSeconds", segment.startSeconds);
					serializer.writeDouble("level", segment.level);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				serializer.writeDouble("circulationPowerKW", crop.circulationPowerKW);
				serializer.writeDouble("growthYieldKgPerKWh", crop.growthYieldKgPerKWh);
				serializer.writeDouble("respirationPerHour", crop.respirationPerHour);
				serializer.writeDouble("transpirationKgPerKgGrowth", crop.transpirationKgPerKgGrowth);
				serializer.writeDouble("minGrowingTemperatureC", crop.minGrowingTemperatureC);
				serializer.writeDouble("maxGrowingTemperatureC", crop.maxGrowingTemperatureC);
				serializer.writeString("foodStoreId", crop.foodStoreId);
				serializer.writeString("wasteStoreId", crop.wasteStoreId);
				serializer.writeDouble("edibleFraction", crop.edibleFraction);
				serializer.writeDouble("harvestTriggerKg", crop.harvestTriggerKg);
				serializer.writeDouble("harvestFraction", crop.harvestFraction);
				serializer.writeInt32("priority", crop.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.algaeCultures.empty())
		{
			// Authored algae culture contracts only; the Living
			// biomass trajectory, the growth, respiration, harvest
			// partition and heat ledgers are runtime state
			// reconstructed on load (#411).
			serializer.beginArray("algaeCultures");
			for (auto const& algae : settings.algaeCultures)
			{
				serializer.beginMap("");
				serializer.writeString("id", algae.id);
				serializer.writeString("atmosphereId", algae.atmosphereId);
				serializer.writeString("waterStoreId", algae.waterStoreId);
				writeWaterQuality(serializer, algae.waterQuality);
				serializer.writeString("nitrogenStoreId", algae.nitrogenStoreId);
				serializer.writeString("phosphorusStoreId", algae.phosphorusStoreId);
				serializer.writeString("potassiumStoreId", algae.potassiumStoreId);
				serializer.writeString("mineralsStoreId", algae.mineralsStoreId);
				serializer.beginMap("composition");
				serializer.writeDouble("moisture", algae.composition.moisture);
				serializer.writeDouble("nutrientNitrogen", algae.composition.nutrientNitrogen);
				serializer.writeDouble("atmosphericNitrogen", algae.composition.atmosphericNitrogen);
				serializer.writeDouble("phosphorus", algae.composition.phosphorus);
				serializer.writeDouble("potassium", algae.composition.potassium);
				serializer.writeDouble("otherMinerals", algae.composition.otherMinerals);
				serializer.writeDouble("energyKJPerKgDry", algae.composition.energyKJPerKgDry);
				serializer.endMap();
				serializer.writeDouble("cultureVolumeL", algae.cultureVolumeL);
				serializer.writeDouble("maxBiomassDensityKgPerL", algae.maxBiomassDensityKgPerL);
				serializer.writeDouble("initialBiomassKg", algae.initialBiomassKg);
				serializer.writeDouble("lampRatedKW", algae.lampRatedKW);
				serializer.beginMap("illumination");
				serializer.writeDouble("constantLevel", algae.illuminationProfile.constantLevel);
				serializer.writeDouble("periodSeconds", algae.illuminationProfile.periodSeconds);
				serializer.beginArray("segments");
				for (auto const& segment : algae.illuminationProfile.segments)
				{
					serializer.beginMap("");
					serializer.writeDouble("startSeconds", segment.startSeconds);
					serializer.writeDouble("level", segment.level);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
				serializer.writeDouble("circulationPowerKW", algae.circulationPowerKW);
				serializer.writeDouble("growthYieldKgPerKWh", algae.growthYieldKgPerKWh);
				serializer.writeDouble("respirationPerHour", algae.respirationPerHour);
				serializer.writeDouble("evaporationKgPerKgGrowth", algae.evaporationKgPerKgGrowth);
				serializer.writeDouble("minGrowingTemperatureC", algae.minGrowingTemperatureC);
				serializer.writeDouble("maxGrowingTemperatureC", algae.maxGrowingTemperatureC);
				serializer.writeString("foodStoreId", algae.foodStoreId);
				serializer.writeString("recoveryStoreId", algae.recoveryStoreId);
				serializer.writeString("wasteStoreId", algae.wasteStoreId);
				serializer.writeDouble("foodFraction", algae.foodFraction);
				serializer.writeDouble("recoveryFraction", algae.recoveryFraction);
				serializer.writeDouble("harvestTriggerKg", algae.harvestTriggerKg);
				serializer.writeDouble("harvestFraction", algae.harvestFraction);
				serializer.writeInt32("priority", algae.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.thermalBuffers.empty())
		{
			// Authored thermal buffer contracts only; the stored energy,
			// the charge and discharge actually moved and the storage
			// ledgers are runtime state reconstructed on load (#398).
			serializer.beginArray("thermalBuffers");
			for (auto const& buffer : settings.thermalBuffers)
			{
				serializer.beginMap("");
				serializer.writeString("id", buffer.id);
				serializer.writeString("reservoirId", buffer.reservoirId);
				serializer.writeString("accessMode", buffer.accessMode);
				serializer.writeString("circulationLoopId", buffer.circulationLoopId);
				serializer.writeDouble("capacityKJ", buffer.capacityKJ);
				serializer.writeDouble("startingEnergyKJ", buffer.startingEnergyKJ);
				serializer.writeDouble("minUsableTemperatureC", buffer.minUsableTemperatureC);
				serializer.writeDouble("maxUsableTemperatureC", buffer.maxUsableTemperatureC);
				serializer.writeDouble("maxChargeKW", buffer.maxChargeKW);
				serializer.writeDouble("maxDischargeKW", buffer.maxDischargeKW);
				serializer.writeDouble("transferConductanceKWPerK", buffer.transferConductanceKWPerK);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.batteries.empty())
		{
			// Authored battery limits only; stored energy, loss ledgers and
			// per-tick charge/discharge results are runtime state.
			serializer.beginArray("batteries");
			for (auto const& battery : settings.batteries)
			{
				serializer.beginMap("");
				serializer.writeString("id", battery.id);
				serializer.writeDouble("nameplateKWh", battery.nameplateKWh);
				serializer.writeDouble("usableKWh", battery.usableKWh);
				serializer.writeDouble("startingStoredKWh", battery.startingStoredKWh);
				serializer.writeDouble("maxChargeKW", battery.maxChargeKW);
				serializer.writeDouble("maxDischargeKW", battery.maxDischargeKW);
				serializer.writeDouble("chargeEfficiency", battery.chargeEfficiency);
				serializer.writeDouble("dischargeEfficiency", battery.dischargeEfficiency);
				// #843 pilot: the capacity-fade opt-in travels only when
				// the bank has one.
				if (battery.capacityFade)
					writeWearBlock(serializer, "capacityFade", *battery.capacityFade);
				// #845: the thermal-runaway opt-in travels only when set.
				if (battery.thermalRunaway)
				{
					serializer.beginMap("thermalRunaway");
					serializer.writeString("compartmentId", battery.thermalRunaway->compartmentId);
					serializer.writeDouble("heatKJ", battery.thermalRunaway->heatKJ);
					serializer.writeDouble("toxicKg", battery.thermalRunaway->toxicKg);
					serializer.endMap();
				}
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.deliveries.empty())
		{
			// Authored schedules only; fired occurrences, accepted and
			// rejected quantities are runtime state reconstructed on load.
			serializer.beginArray("deliveries");
			for (auto const& delivery : settings.deliveries)
			{
				serializer.beginMap("");
				serializer.writeString("id", delivery.id);
				serializer.writeString("storeId", delivery.storeId);
				serializer.writeString("unit", delivery.unit);
				serializer.writeDouble("quantity", delivery.quantity);
				serializer.writeDouble("firstOccurrenceSeconds", delivery.firstOccurrenceSeconds);
				serializer.writeDouble("recurrenceSeconds", delivery.recurrenceSeconds);
				serializer.writeBool("arrivesQualified", delivery.arrivesQualified);
				serializer.writeInt32("priority", delivery.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.wasteEjections.empty())
		{
			// Authored schedules only; fired occurrences and the
			// ejected/unmet split are runtime state reconstructed on
			// load, exactly as for resupply deliveries (#403).
			serializer.beginArray("wasteEjections");
			for (auto const& ejection : settings.wasteEjections)
			{
				serializer.beginMap("");
				serializer.writeString("id", ejection.id);
				serializer.writeString("storeId", ejection.storeId);
				if (ejection.eligibility != WasteEjectionEligibility::QualifiedOnly)
					serializer.writeString("eligibility", ejection.eligibility == WasteEjectionEligibility::HeldOnly
						? "heldOnly" : "qualifiedAndHeld");
				serializer.writeString("unit", ejection.unit);
				serializer.writeDouble("quantity", ejection.quantity);
				serializer.writeDouble("firstOccurrenceSeconds", ejection.firstOccurrenceSeconds);
				serializer.writeDouble("recurrenceSeconds", ejection.recurrenceSeconds);
				serializer.writeInt32("priority", ejection.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.outages.empty())
		{
			// Authored schedules only; the window state, the started and
			// completed occurrence counts and each installation's
			// effective availability are runtime state reconstructed on
			// load from the authored schedule and the World tick (#413).
			serializer.beginArray("outages");
			for (auto const& outage : settings.outages)
			{
				serializer.beginMap("");
				serializer.writeString("id", outage.id);
				serializer.writeString("installationId", outage.installationId);
				serializer.writeDouble("capacityFraction", outage.capacityFraction);
				serializer.writeDouble("firstOccurrenceSeconds", outage.firstOccurrenceSeconds);
				serializer.writeDouble("durationSeconds", outage.durationSeconds);
				serializer.writeDouble("recurrenceSeconds", outage.recurrenceSeconds);
				serializer.writeInt32("priority", outage.priority);
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (!settings.maintenanceJobs.empty())
		{
			// Authored schedules and requirements only; the reservation
			// state, progress, phase and spend are runtime state
			// reconstructed on load from the authored schedule (#414).
			serializer.beginArray("maintenanceJobs");
			for (auto const& job : settings.maintenanceJobs)
			{
				serializer.beginMap("");
				serializer.writeString("id", job.id);
				serializer.writeString("installationId", job.installationId);
				serializer.writeDouble("firstOccurrenceSeconds", job.firstOccurrenceSeconds);
				serializer.writeDouble("durationSeconds", job.durationSeconds);
				serializer.writeDouble("recurrenceSeconds", job.recurrenceSeconds);
				serializer.writeInt32("priority", job.priority);
				serializer.writeString("wasteStoreId", job.wasteStoreId);
				serializer.beginArray("requirements");
				for (auto const& requirement : job.requirements)
				{
					serializer.beginMap("");
					serializer.writeString("storeId", requirement.storeId);
					serializer.writeDouble("quantity", requirement.quantity);
					serializer.writeBool("consumed", requirement.consumed);
					serializer.endMap();
				}
				serializer.endArray();
				serializer.endMap();
			}
			serializer.endArray();
		}
		serializer.endMap();
	}

	void readStationSystemsSettingsMap(Serializer& serializer,
		std::uint32_t const version, StationSystemsSettings& stationSettings, bool validate)
	{
		serializer.beginMap("stationSystems");
		// Retired fields take precedence over all schema/feature diagnostics.
		// This policy is independent of which entries support waterQuality.
		for (auto const* array : {"stores", "transfers", "conversions", "solarArrays", "equipmentLoads",
			"batteries", "thermalReservoirs", "radiators", "coolantLoops", "chillers", "generators",
			"atmospheres", "airlockCycles", "waterCollectors", "waterTreatments", "electrolyzers",
			"co2Captures", "fuelCells", "thermalBuffers", "sabatierReactors", "methaneProcessors",
			"deliveries", "wasteEjections", "outages", "maintenanceJobs", "microbialProcessors"})
		{
			if (!serializer.hasField(array)) continue;
			serializer.beginArray(array);
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				if (serializer.hasField("hazardClass"))
					throw SerializationException(
						"Station entry '" + serializer.readString("id")
						+ "' field 'hazardClass' was removed (#757, #758); conversion required");
				if (std::string_view(array) == "waterTreatments")
					for (auto const* field : {"clearanceStoreId", "clearancePerKg", "releaseToNormalClass"})
						if (serializer.hasField(field))
							throw SerializationException(
								"Station water treatment '" + serializer.readString("id")
								+ "' field '" + field + "' was removed (#753); conversion required");
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (version < 65) throw SerializationException("Station systems require World schema 65");
		if (serializer.hasField("monitors"))
		{
			if (version < 122) throw SerializationException("Station Monitors require schema 122");
			serializer.beginArray("monitors");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				StationMonitorSettings monitor;
				monitor.id = serializer.readString("id");
				monitor.subjectType = serializer.readString("subjectType");
				monitor.subjectId = serializer.readString("subjectId");
				monitor.metric = serializer.readString("metric");
				serializer.beginMap("rule");
				monitor.rule.threshold = serializer.readDouble("threshold");
				auto const direction = serializer.readString("direction");
				if (direction != "Above" && direction != "Below") throw SerializationException("Unknown Monitor direction");
				monitor.rule.direction = direction == "Above" ? AlertDirection::Above : AlertDirection::Below;
				monitor.rule.triggerOnSeconds = serializer.readDouble("triggerOnSeconds");
				monitor.rule.triggerOffSeconds = serializer.readDouble("triggerOffSeconds");
				serializer.endMap();
				auto const severity = serializer.readString("severity");
				if (severity != "Critical" && severity != "Warning" && severity != "Advisory")
					throw SerializationException("Unknown Monitor severity");
				monitor.severity = severity == "Critical" ? AlertSeverity::Critical
					: severity == "Warning" ? AlertSeverity::Warning : AlertSeverity::Advisory;
				if (serializer.hasField("enabled")) monitor.enabled = serializer.readBool("enabled");
				// Documents written before #867 carry no tag: the value is then
				// exactly what it always was, a hand-authored number making no
				// provenance claim.
				if (serializer.hasField("provenance"))
				{
					auto const tag = serializer.readString("provenance");
					if (tag != "iss" && tag != "who" && tag != "rated" && tag != "prov" && tag != "custom")
						throw SerializationException("Unknown Monitor provenance");
					monitor.provenance = tag == "iss" ? MonitorProvenance::Iss
						: tag == "who" ? MonitorProvenance::Who
						: tag == "rated" ? MonitorProvenance::Rated
						: tag == "prov" ? MonitorProvenance::Provisional
						: MonitorProvenance::Custom;
				}
				serializer.endMap();
				stationSettings.monitors.push_back(std::move(monitor));
			}
			serializer.endArray();
		}
		// Reject misplaced gates instead of silently discarding authored intent.
		for (auto const* array : {"stores", "transfers", "conversions", "solarArrays", "equipmentLoads",
			"batteries", "thermalReservoirs", "radiators", "coolantLoops", "chillers", "generators",
			"atmospheres", "airlockCycles", "waterCollectors", "waterTreatments", "electrolyzers",
			"co2Captures", "fuelCells", "thermalBuffers", "sabatierReactors", "methaneProcessors",
			"deliveries", "wasteEjections", "outages", "maintenanceJobs"})
		{
			if (!serializer.hasField(array)) continue;
			serializer.beginArray(array);
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				if (serializer.hasField("waterQuality"))
					throw SerializationException(std::string(array) + " has no configurable water quality gate (electrolyzer purity is built in)");
				serializer.endMap();
			}
			serializer.endArray();
		}
		stationSettings.enabled = serializer.readBool("enabled");
		stationSettings.observationIntervalSeconds = serializer.readFloat("observationIntervalSeconds");
		if (serializer.hasField("connections"))
		{
			if (version < 97) throw SerializationException("Station connection properties require schema 97");
			serializer.beginArray("connections");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				StationConnectionKey key{serializer.readString("ownerType"), serializer.readString("ownerId"),
					serializer.readString("reference"), serializer.readString("relation"),
					serializer.readString("peerType"), serializer.readString("peerId")};
				StationConnectionProperties properties;
				auto type = serializer.readString("physicalType");
				auto found = std::find(StationPhysicalConnectionNames.begin(), StationPhysicalConnectionNames.end(), type);
				if (found == StationPhysicalConnectionNames.end()) throw SerializationException("Unknown physical connection type: " + type);
				properties.physicalType = static_cast<StationPhysicalConnection>(found - StationPhysicalConnectionNames.begin());
				properties.maximumThroughput = serializer.readDouble("maximumThroughput");
				if (serializer.hasField("efficiency")) properties.efficiency = serializer.readDouble("efficiency");
				if (key.ownerType.empty() || key.ownerId.empty() || key.reference.empty() || key.relation.empty()
					|| key.peerType.empty() || key.peerId.empty()
					|| !std::isfinite(properties.maximumThroughput) || properties.maximumThroughput <= 0
					|| !std::isfinite(properties.efficiency) || properties.efficiency < 0 || properties.efficiency > 1
					|| !stationSettings.connections.emplace(std::move(key), properties).second)
					throw SerializationException("Invalid or duplicate Station connection properties");
				serializer.endMap();
			}
			serializer.endArray();
		}
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
				if (serializer.hasField("materialId"))
				{
					if (version < 98) throw SerializationException("Station material identity requires schema 98");
					store.materialId = serializer.readString("materialId");
				}
				store.capacity = serializer.readDouble("capacity");
				store.startingQualified = serializer.readDouble("startingQualified");
				store.startingUnqualified = serializer.readDouble("startingUnqualified");
				if (serializer.hasField("protectedReserve"))
				{
					if (version < 67) throw SerializationException(
						"Station protected reserves require World schema 67");
					store.protectedReserve = serializer.readDouble("protectedReserve");
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
				transfer.ratePerSecond = serializer.hasField("ratePerSecond") ? serializer.readDouble("ratePerSecond") : 0.0;
				if (version < 99 && (serializer.hasField("mode") || serializer.hasField("quantity")
					|| serializer.hasField("firstOccurrenceSeconds") || serializer.hasField("recurrenceSeconds")))
					throw SerializationException("Scheduled transfer fields require schema 99");
				if (serializer.hasField("mode"))
				{
					auto const mode = serializer.readString("mode");
					if (mode != "Continuous" && mode != "Scheduled") throw SerializationException("Invalid transfer mode");
					transfer.mode = mode == "Scheduled" ? ResourceTransferMode::Scheduled : ResourceTransferMode::Continuous;
				}
				if (serializer.hasField("quantity")) transfer.quantity = serializer.readDouble("quantity");
				if (serializer.hasField("firstOccurrenceSeconds")) transfer.firstOccurrenceSeconds = serializer.readDouble("firstOccurrenceSeconds");
				if (serializer.hasField("recurrenceSeconds")) transfer.recurrenceSeconds = serializer.readDouble("recurrenceSeconds");
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
				readMachineWear(serializer, "cellDegradation", version, array.cellDegradation);
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
				// #845: a pre-118 document carrying a solar fault opt-in is
				// refused rather than silently dropped.
				if (serializer.hasField("stringLoss"))
				{
					if (version < 118) throw SerializationException(
						"Solar string-loss opt-in requires schema 118");
					serializer.beginMap("stringLoss");
					array.stringLoss = SolarStringLossSettings{serializer.readDouble("lossFraction")};
					serializer.endMap();
				}
				if (serializer.hasField("trackingFailure"))
				{
					if (version < 118) throw SerializationException(
						"Solar tracking-failure opt-in requires schema 118");
					serializer.beginMap("trackingFailure");
					array.trackingFailure = SolarTrackingFailureSettings{serializer.readDouble("availability")};
					serializer.endMap();
				}
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
				// #844: a pre-117 document carrying the puncture opt-in or
				// repair wiring is refused rather than silently dropped.
				{
					std::string compartmentId;
					double chargeKg{0.0}, leakRateKgPerS{0.0}, specificHeatKJPerKgK{0.0};
					if (readChargeLeakBlock(serializer, "panelPuncture", version,
						"Radiator panel puncture", compartmentId, chargeKg,
						leakRateKgPerS, specificHeatKJPerKgK))
						radiator.panelPuncture = PanelPunctureSettings{std::move(compartmentId),
							chargeKg, leakRateKgPerS, specificHeatKJPerKgK};
				}
				{
					RepairPartsSettings wiring;
					if (readRepairPartsBlock(serializer, version, wiring))
						radiator.repairParts = std::move(wiring);
				}
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
				if (serializer.hasField("coolantLeak"))
				{
					if (version < 112) throw SerializationException("Coolant faults require schema 112");
					serializer.beginMap("coolantLeak");
					loop.coolantLeak = CoolantLeakSettings{serializer.readString("compartmentId"),
						serializer.readDouble("releasedKg")};
					serializer.endMap();
				}
				// #840: a pre-113 document carrying a Condition model is
				// refused rather than silently dropped.
				if (serializer.hasField("condition"))
				{
					if (version < 113) throw SerializationException("Station Condition models require schema 113");
					serializer.beginMap("condition");
					InstallationConditionSettings model;
					model.initialCondition = serializer.readDouble("initialCondition");
					model.overheatStressC = serializer.readDouble("overheatStressC");
					model.overheatDegradePerHour = serializer.readDouble("overheatDegradePerHour");
					model.cavitationFillFraction = serializer.readDouble("cavitationFillFraction");
					model.cavitationDegradePerHour = serializer.readDouble("cavitationDegradePerHour");
					model.seizureThreshold = serializer.readDouble("seizureThreshold");
					model.restoreStoreId = serializer.readString("restoreStoreId");
					model.restorePerHour = serializer.readDouble("restorePerHour");
					model.stockUnitsPerConditionPoint = serializer.readDouble("stockUnitsPerConditionPoint");
					serializer.endMap();
					loop.condition = model;
				}
				// #842: a pre-115 document carrying repair-parts wiring is
				// refused rather than silently dropped.
				if (serializer.hasField("repairParts"))
				{
					if (version < 115) throw SerializationException("Repair-parts wiring requires schema 115");
					serializer.beginMap("repairParts");
					RepairPartsSettings wiring;
					wiring.partsStoreId = serializer.readString("partsStoreId");
					serializer.beginArray("repairs");
					while (serializer.nextArrayItem())
					{
						serializer.beginMap("");
						FaultRepairSettings plan;
						plan.modeId = serializer.readString("modeId");
						plan.durationSeconds = serializer.readDouble("durationSeconds");
						plan.partsQuantity = serializer.readDouble("partsQuantity");
						plan.wasteStoreId = serializer.readString("wasteStoreId");
						serializer.endMap();
						wiring.repairs.push_back(std::move(plan));
					}
					serializer.endArray();
					serializer.endMap();
					loop.repairParts = std::move(wiring);
				}
				// #843: a pre-116 document carrying a Wear model is
				// refused rather than silently dropped.
				{
					InstallationWearSettings model;
					if (readWearBlock(serializer, "wear", version, "Coolant loop wear models", model))
						loop.wear = model;
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
				// #844: a pre-117 document carrying the thermal-plant
				// fault opt-ins or repair wiring is refused rather than
				// silently dropped.
				if (serializer.hasField("compressorFailure"))
				{
					if (version < 117) throw SerializationException(
						"Chiller compressor-failure opt-in requires schema 117");
					chiller.compressorFailure = serializer.readBool("compressorFailure");
				}
				{
					std::string compartmentId;
					double chargeKg{0.0}, leakRateKgPerS{0.0}, specificHeatKJPerKgK{0.0};
					if (readChargeLeakBlock(serializer, "refrigerantLeak", version,
						"Chiller refrigerant leak", compartmentId, chargeKg,
						leakRateKgPerS, specificHeatKJPerKgK))
						chiller.refrigerantLeak = RefrigerantLeakSettings{std::move(compartmentId),
							chargeKg, leakRateKgPerS, specificHeatKJPerKgK};
				}
				{
					RepairPartsSettings wiring;
					if (readRepairPartsBlock(serializer, version, wiring))
						chiller.repairParts = std::move(wiring);
				}
				stationSettings.chillers.push_back(std::move(chiller));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("heatEngines"))
		{
			if (version < 109) throw SerializationException(
				"Station heat engines require World schema 109; conversion required");
			serializer.beginArray("heatEngines");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto const id = serializer.readString("id");
				std::vector<std::string> fields{"id", "hotReservoirId",
					"rejectionReservoirId", "thermalClass", "ratedThermalInputKW",
					"commandedInputKW", "carnotFraction", "auxiliaryPowerKW",
					"minHotTemperatureC", "maxSinkTemperatureC",
					// #844: the seizure opt-in and repair wiring join the
					// known-field set; #848 adds the Wear opt-in.
					"mechanicalSeizure", "repairParts", "efficiencyDrift"};
				for (auto const& field : serializer.mapKeys(""))
					if (std::find(fields.begin(), fields.end(), field) == fields.end())
						throw SerializationException("Station entry '" + id + "' field '" + field
							+ "' is unknown; conversion required");
				HeatEngineSettings engine;
				engine.id = id;
				readMachineWear(serializer, "efficiencyDrift", version, engine.efficiencyDrift);
				engine.hotReservoirId = serializer.readString("hotReservoirId");
				engine.rejectionReservoirId = serializer.readString("rejectionReservoirId");
				engine.thermalClass = serializer.readString("thermalClass");
				engine.ratedThermalInputKW = serializer.readDouble("ratedThermalInputKW");
				// #805: the intake dispatch defaults to full rated; a
				// pre-110 document carrying the field is refused rather than
				// silently dropping the throttle the author commanded.
				engine.commandedInputKW = engine.ratedThermalInputKW;
				if (serializer.hasField("commandedInputKW"))
				{
					if (version < 110) throw SerializationException(
						"Heat engine commanded input requires World schema 110; conversion required");
					engine.commandedInputKW = serializer.readDouble("commandedInputKW");
				}
				engine.carnotFraction = serializer.readDouble("carnotFraction");
				engine.auxiliaryPowerKW = serializer.readDouble("auxiliaryPowerKW");
				engine.minHotTemperatureC = serializer.readDouble("minHotTemperatureC");
				engine.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				// #844: a pre-117 document carrying the seizure opt-in or
				// repair wiring is refused rather than silently dropped.
				if (serializer.hasField("mechanicalSeizure"))
				{
					if (version < 117) throw SerializationException(
						"Heat engine mechanical-seizure opt-in requires schema 117");
					engine.mechanicalSeizure = serializer.readBool("mechanicalSeizure");
				}
				{
					RepairPartsSettings wiring;
					if (readRepairPartsBlock(serializer, version, wiring))
						engine.repairParts = std::move(wiring);
				}
				stationSettings.heatEngines.push_back(std::move(engine));
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
				readMachineWear(serializer, "fouledInjector", version, generator.fouledInjector);
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
				// #845: a pre-118 document carrying the failed-start
				// opt-in is refused rather than silently dropped.
				if (serializer.hasField("failedStart"))
				{
					if (version < 118) throw SerializationException(
						"Generator failed-start opt-in requires schema 118");
					generator.failedStart = serializer.readBool("failedStart");
				}
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
				atmosphere.volumeM3 = serializer.readDouble("volumeM3");
				atmosphere.startingOxygenKg = serializer.readDouble("startingOxygenKg");
				atmosphere.startingNitrogenKg = serializer.readDouble("startingNitrogenKg");
				atmosphere.startingCarbonDioxideKg = serializer.readDouble("startingCarbonDioxideKg");
				atmosphere.startingWaterVapourKg = serializer.readDouble("startingWaterVapourKg");
				atmosphere.startingTemperatureC = serializer.readDouble("startingTemperatureC");
				atmosphere.thermalCapacityKJPerK = serializer.readDouble("thermalCapacityKJPerK");
				// Advected bulk specific heat arrived at schema 108 (#802);
				// an older document carries no field and the compartment
				// keeps the dry-air default.
				atmosphere.advectedSpecificHeatKJPerKgK = version >= 108
					? serializer.readDouble("advectedSpecificHeatKJPerKgK")
					: StationDefaultAdvectedSpecificHeatKJPerKgK;
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
				if (serializer.hasField("fire"))
				{
					if (version < 120) throw SerializationException("fire requires schema 120");
					serializer.beginMap("fire");
					atmosphere.fire = AtmosphereFireSettings{serializer.readDouble("oxygenKgPerS"),
						serializer.readDouble("carbonMonoxideKgPerS"), serializer.readDouble("carbonDioxideKgPerS"),
						serializer.readDouble("heatKW"), serializer.readDouble("oxygenThresholdKg")};
					serializer.endMap();
				}
				stationSettings.atmospheres.push_back(std::move(atmosphere));
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("atmospherePathways"))
		{
			if (version < 105) throw SerializationException("atmospherePathways requires World schema 105; conversion required");
			serializer.beginArray("atmospherePathways");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto const id = serializer.readString("id");
				std::vector<std::string> fields{"id", "aAtmosphereId", "bAtmosphereId", "areaM2"};
				// Openness became a live parameter at schema 106; an older
				// document carried only the starting value, which is now the
				// same field.
				bool const hasOpen = version >= 106;
				if (hasOpen) fields.push_back("openFraction");
				for (auto const& field : serializer.mapKeys(""))
					if (std::find(fields.begin(), fields.end(), field) == fields.end())
						throw SerializationException("Station entry '" + id + "' field '" + field + "' is unknown; conversion required");
				stationSettings.atmospherePathways.push_back({id,
					serializer.readString("aAtmosphereId"), serializer.readString("bAtmosphereId"),
					serializer.readDouble("areaM2"),
					hasOpen ? serializer.readDouble("openFraction") : 1.0});
				serializer.endMap();
			}
			serializer.endArray();
		}
		if (serializer.hasField("thermalLinks"))
		{
			if (version < 107) throw SerializationException(
				"Station thermal links require World schema 107; conversion required");
			serializer.beginArray("thermalLinks");
			while (serializer.nextArrayItem())
			{
				serializer.beginMap("");
				auto const id = serializer.readString("id");
				std::vector<std::string> fields{"id", "aAtmosphereId", "bAtmosphereId",
					"conductanceKWPerK", "aKind", "bKind"};
				for (auto const& field : serializer.mapKeys(""))
					if (std::find(fields.begin(), fields.end(), field) == fields.end())
						throw SerializationException("Station entry '" + id + "' field '" + field
							+ "' is unknown; conversion required");
				ThermalLinkSettings link;
				link.id = id;
				link.aAtmosphereId = serializer.readString("aAtmosphereId");
				link.bAtmosphereId = serializer.readString("bAtmosphereId");
				auto readKind = [&](char const* field) {
					if (!serializer.hasField(field)) return ThermalNodeKind::Atmosphere;
					auto const kind = serializer.readString(field);
					if (kind == "atmosphere") return ThermalNodeKind::Atmosphere;
					if (kind == "reservoir") return ThermalNodeKind::Reservoir;
					if (kind == "machineBody") return ThermalNodeKind::MachineBody;
					throw SerializationException("Station thermal link '" + id + "' has unknown endpoint kind '" + kind + "'");
				};
				link.aKind = readKind("aKind");
				link.bKind = readKind("bKind");
				link.conductanceKWPerK = serializer.readDouble("conductanceKWPerK");
				stationSettings.thermalLinks.push_back(std::move(link));
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
				if (serializer.hasField("interruptedCycle"))
				{
					if (version < 120) throw SerializationException("interruptedCycle requires schema 120");
					cycle.interruptedCycle = serializer.readBool("interruptedCycle");
				}
				if (serializer.hasField("interlockFailure"))
				{
					if (version < 120) throw SerializationException("interlockFailure requires schema 120");
					serializer.beginMap("interlockFailure");
					cycle.interlockFailure = AirlockInterlockFailureSettings{
						serializer.readString("pathwayId"), serializer.readDouble("breachAreaM2")};
					serializer.endMap();
				}
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
				readWaterQuality(serializer, crew.waterQuality);
				if (serializer.hasField("greywaterStoreId")) crew.greywaterStoreId = serializer.readString("greywaterStoreId");
				if (serializer.hasField("greywaterKgPerCrewDay")) crew.greywaterKgPerCrewDay = serializer.readDouble("greywaterKgPerCrewDay");
				if (serializer.hasField("greywaterLoadKgPerCrewDay"))
				{
					auto const keys = serializer.mapKeys("greywaterLoadKgPerCrewDay");
					serializer.beginMap("greywaterLoadKgPerCrewDay");
					for (auto const& name : keys) crew.greywaterLoadKgPerCrewDay[name] = serializer.readDouble(name);
					serializer.endMap();
				}
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
				readMachineWear(serializer, "membraneFouling", version, treatment.membraneFouling);
				treatment.sourceStoreId = serializer.readString("sourceStoreId");
				treatment.treatedStoreId = serializer.readString("treatedStoreId");
				treatment.concentrateStoreId = serializer.readString("concentrateStoreId");
				treatment.consumableStoreId = serializer.readString("consumableStoreId");
				treatment.throughputKgPerS = serializer.readDouble("throughputKgPerS");
				treatment.recoveryRatio = serializer.readDouble("recoveryRatio");
				if (serializer.hasField("removalFractions"))
				{
					if (version < 100) throw SerializationException("Water removal fractions require schema 100");
					auto const keys = serializer.mapKeys("removalFractions");
					serializer.beginMap("removalFractions");
					for (auto const& name : keys)
						treatment.removalFractions[name] = serializer.readDouble(name);
					serializer.endMap();
				}
				treatment.specificEnergyKWhPerKg = serializer.readDouble("specificEnergyKWhPerKg");
				treatment.consumablePerKg = serializer.readDouble("consumablePerKg");
				treatment.priority = serializer.readInt32("priority");
				if (serializer.hasField("breakthrough"))
				{
					if (version < 119) throw SerializationException("breakthrough requires schema 119");
					treatment.breakthrough = serializer.readBool("breakthrough");
				}
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
				readMachineWear(serializer, "membraneDegradation", version, electrolyzer.membraneDegradation);
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
				if (serializer.hasField("catalystPoisoning"))
				{
					if (version < 119) throw SerializationException("catalyst-poisoning requires schema 119");
					serializer.beginMap("catalystPoisoning");
					electrolyzer.catalystPoisoning = CatalystPoisoningSettings{serializer.readDouble("triggerConcentration"), serializer.readDouble("wearPerHour")};
					serializer.endMap();
				}
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
				readMachineWear(serializer, "absorbentDegradation", version, capture.absorbentDegradation);
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
				readMachineWear(serializer, "stackDegradation", version, cell.stackDegradation);
				cell.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				cell.oxygenStoreId = serializer.readString("oxygenStoreId");
				cell.waterStoreId = serializer.readString("waterStoreId");
				cell.thermalClass = serializer.readString("thermalClass");
				cell.heatSinkId = serializer.readString("heatSinkId");
				cell.ratedOutputKW = serializer.readDouble("ratedOutputKW");
				cell.efficiency = serializer.readDouble("efficiency");
				cell.commandedOutputKW = serializer.readDouble("commandedOutputKW");
				cell.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				if (serializer.hasField("catalystPoisoning"))
				{
					if (version < 119) throw SerializationException("catalyst-poisoning requires schema 119");
					serializer.beginMap("catalystPoisoning");
					cell.catalystPoisoning = CatalystPoisoningSettings{serializer.readDouble("triggerConcentration"), serializer.readDouble("wearPerHour")};
					serializer.endMap();
				}
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
				readMachineWear(serializer, "catalystDeactivation", version, reactor.catalystDeactivation);
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
				if (serializer.hasField("overtempRunaway"))
				{
					if (version < 114) throw SerializationException("overtempRunaway requires schema 114");
					serializer.beginMap("overtempRunaway");
					reactor.overtempRunaway = OvertempRunawaySettings{serializer.readDouble("triggerTemperatureC"), serializer.readDouble("heatSpikeKJ")};
					serializer.endMap();
				}
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
				readMachineWear(serializer, "catalystDeactivation", version, processor.catalystDeactivation);
				processor.methaneStoreId = serializer.readString("methaneStoreId");
				processor.hydrogenStoreId = serializer.readString("hydrogenStoreId");
				processor.carbonStoreId = serializer.readString("carbonStoreId");
				processor.thermalClass = serializer.readString("thermalClass");
				processor.heatSinkId = serializer.readString("heatSinkId");
				processor.rateKgPerS = serializer.readDouble("rateKgPerS");
				processor.specificEnergyKWhPerKgCH4 =
					serializer.readDouble("specificEnergyKWhPerKgCH4");
				processor.maxSinkTemperatureC = serializer.readDouble("maxSinkTemperatureC");
				if (serializer.hasField("overtempRunaway"))
				{
					if (version < 114) throw SerializationException("overtempRunaway requires schema 114");
					serializer.beginMap("overtempRunaway");
					processor.overtempRunaway = OvertempRunawaySettings{serializer.readDouble("triggerTemperatureC"), serializer.readDouble("heatSpikeKJ")};
					serializer.endMap();
				}
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
				if (serializer.hasField("rateBasis"))
				{
					auto const basis = serializer.readString("rateBasis");
					if (basis == "CompositionDerived") processor.rateBasis = StationRateBasis::CompositionDerived;
					else if (basis != "FixedPerFeedMass") throw SerializationException("Unknown microbial rateBasis: " + basis);
				}
				if (serializer.hasField("organicOxidationFraction")) processor.organicOxidationFraction = serializer.readDouble("organicOxidationFraction");
				if (serializer.hasField("o2PerKgOrganicOxidized")) processor.o2PerKgOrganicOxidized = serializer.readDouble("o2PerKgOrganicOxidized");
				if (serializer.hasField("nitrificationFraction")) processor.nitrificationFraction = serializer.readDouble("nitrificationFraction");
				if (serializer.hasField("o2PerKgNitrifiedN")) processor.o2PerKgNitrifiedN = serializer.readDouble("o2PerKgNitrifiedN");
				if (serializer.hasField("co2PerKgOrganicOxidized")) processor.co2PerKgOrganicOxidized = serializer.readDouble("co2PerKgOrganicOxidized");
				if (serializer.hasField("processWaterTargetMoisture")) processor.processWaterTargetMoisture = serializer.readDouble("processWaterTargetMoisture");
				if (serializer.hasField("heatReleaseFraction")) processor.heatReleaseFraction = serializer.readDouble("heatReleaseFraction");
				if (serializer.hasField("consumablePerKgOrganicOxidized")) processor.consumablePerKgOrganicOxidized = serializer.readDouble("consumablePerKgOrganicOxidized");
				processor.feedStoreId = serializer.readString("feedStoreId");
				processor.oxygenStoreId = serializer.readString("oxygenStoreId");
				processor.waterStoreId = serializer.readString("waterStoreId");
				readWaterQuality(serializer, processor.waterQuality);
				processor.consumableStoreId = serializer.readString("consumableStoreId");
				processor.nutrientStoreId = serializer.readString("nutrientStoreId");
				if (serializer.hasField("recoveredWaterCarryoverFractions"))
				{
					auto const keys = serializer.mapKeys("recoveredWaterCarryoverFractions");
					serializer.beginMap("recoveredWaterCarryoverFractions");
					for (auto const& name : keys) processor.recoveredWaterCarryoverFractions[name] = serializer.readDouble(name);
					serializer.endMap();
				}
				processor.recoveredWaterStoreId = serializer.readString("recoveredWaterStoreId");
				processor.co2StoreId = serializer.readString("co2StoreId");
				processor.residueStoreId = serializer.readString("residueStoreId");
				processor.thermalClass = serializer.readString("thermalClass");
				processor.heatSinkId = serializer.readString("heatSinkId");
				processor.rateKgPerS = serializer.readDouble("rateKgPerS");
				processor.specificEnergyKWhPerKgFeed =
					serializer.readDouble("specificEnergyKWhPerKgFeed");
				if (processor.rateBasis == StationRateBasis::FixedPerFeedMass
					|| serializer.hasField("oxygenPerKgFeedKg")) processor.oxygenPerKgFeedKg = serializer.readDouble("oxygenPerKgFeedKg");
				if (processor.rateBasis == StationRateBasis::FixedPerFeedMass
					|| serializer.hasField("processWaterPerKgFeedKg")) processor.processWaterPerKgFeedKg =
					serializer.readDouble("processWaterPerKgFeedKg");
				if (processor.rateBasis == StationRateBasis::FixedPerFeedMass
					|| serializer.hasField("consumablePerKgFeed")) processor.consumablePerKgFeed = serializer.readDouble("consumablePerKgFeed");
				if (processor.rateBasis == StationRateBasis::FixedPerFeedMass
					|| serializer.hasField("co2PerKgFeedKg")) processor.co2PerKgFeedKg = serializer.readDouble("co2PerKgFeedKg");
				processor.nitrogenRecovery = serializer.readDouble("nitrogenRecovery");
				processor.phosphorusRecovery = serializer.readDouble("phosphorusRecovery");
				processor.potassiumRecovery = serializer.readDouble("potassiumRecovery");
				processor.mineralsRecovery = serializer.readDouble("mineralsRecovery");
				processor.waterRecovery = serializer.readDouble("waterRecovery");
				if (processor.rateBasis == StationRateBasis::FixedPerFeedMass
					|| serializer.hasField("reactionHeatKJPerKgFeed")) processor.reactionHeatKJPerKgFeed =
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
				readWaterQuality(serializer, crop.waterQuality);
				if (serializer.hasField("irrigationUptakeFractions"))
				{
					if (version < 101) throw SerializationException("Selective crop irrigation requires schema 101");
					auto const keys = serializer.mapKeys("irrigationUptakeFractions");
					serializer.beginMap("irrigationUptakeFractions");
					for (auto const& name : keys) crop.irrigationUptakeFractions[name] = serializer.readDouble(name);
					serializer.endMap();
				}
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
				readWaterQuality(serializer, algae.waterQuality);
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
				// #843: a pre-116 document carrying a capacity-fade model
				// is refused rather than silently dropped.
				{
					InstallationWearSettings model;
					if (readWearBlock(serializer, "capacityFade", version,
						"Battery capacity-fade models", model))
						battery.capacityFade = model;
				}
				// #845: a pre-118 document carrying a thermal-runaway
				// opt-in is refused rather than silently dropped.
				if (serializer.hasField("thermalRunaway"))
				{
					if (version < 118) throw SerializationException(
						"Battery thermal-runaway opt-in requires schema 118");
					serializer.beginMap("thermalRunaway");
					battery.thermalRunaway = BatteryThermalRunawaySettings{serializer.readString("compartmentId"),
						serializer.readDouble("heatKJ"), serializer.readDouble("toxicKg")};
					serializer.endMap();
				}
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
				if (serializer.hasField("eligibility"))
				{
					if (version < 102) throw SerializationException("Waste ejection eligibility requires schema 102");
					auto const value = serializer.readString("eligibility");
					if (value != "qualifiedOnly" && value != "heldOnly" && value != "qualifiedAndHeld") throw SerializationException(
						"Waste ejection eligibility must be qualifiedOnly, heldOnly or qualifiedAndHeld");
					ejection.eligibility = value == "heldOnly" ? WasteEjectionEligibility::HeldOnly
						: value == "qualifiedAndHeld" ? WasteEjectionEligibility::QualifiedAndHeld
						: WasteEjectionEligibility::QualifiedOnly;
				}
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
		// Missing timing fields belong to legacy 60 Hz documents, not the
		// default cadence for newly authored settings.
		stationSettings.systemsStepWorldTicks = 1;
		if (serializer.hasField("systemsStepWorldTicks"))
		{
			if (version < 104)
				throw SerializationException("Station systems step ratio requires World schema 104");
			stationSettings.systemsStepWorldTicks = serializer.readUint32("systemsStepWorldTicks");
		}
		serializer.endMap();
		std::string diagnostic;
		if (validate && !validateStationSystemsSettings(stationSettings, diagnostic))
			throw SerializationException(diagnostic);
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
			if (document.schemaVersion < 65 || document.schemaVersion > kWorldSchemaVersion)
				return refuse("Reference station schema version must be from 65 to "
					+ std::to_string(kWorldSchemaVersion));
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
				if (document.scenarioKind != "reference" && document.scenarioKind != "emergency"
					&& document.scenarioKind != "stability")
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

}
