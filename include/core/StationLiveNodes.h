// Per-node live-edit bindings.
//
// LiveNode<Settings> is the compile-time map from a settings struct to the
// collection that holds it and the initial-condition fields it owns. It lives
// in a header because updateNode is a template: every translation unit that
// instantiates it must see the specialisations.
//
// The Settings type selects the collection, so a targeted edit is type-checked
// at the call site and never needs a field-name string. The node type label is
// carried alongside for diagnostics that read in the words the flow canvas
// uses, and updateNode refuses if the two disagree.
#pragma once

#include "core/StationSystems.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace core
{
	template <class Settings>
	struct LiveNode; // undefined: specialise below via PF_STATION_LIVE_NODE

	// #844: the shared repair-parts frozen-topology rule (#842): the
	// parts store identity and the per-mode plan identities - which
	// faults are repairable and which stores they draw from - cannot move
	// under a live run. Each plan's duration and parts quantity stay
	// live-tunable parameters; a repair already started keeps the
	// parameters it froze at command time.
	inline bool repairWiringTopologyChanged(
		std::optional<RepairPartsSettings> const& a,
		std::optional<RepairPartsSettings> const& b)
	{
		if (a.has_value() != b.has_value()) return true;
		if (!a) return false;
		return a->partsStoreId != b->partsStoreId
			|| a->repairs.size() != b->repairs.size()
			|| !std::equal(a->repairs.begin(), a->repairs.end(), b->repairs.begin(),
				[](FaultRepairSettings const& x, FaultRepairSettings const& y)
				{
					return x.modeId == y.modeId && x.wasteStoreId == y.wasteStoreId;
				});
	}

	// Restore the fields a live edit may not move. Returns true when the caller
	// actually touched one, so the refusal can be reported rather than dropped.
	// The conservation audit derives its baseline from the authored starting
	// values at snapshot time, so these must stay as the run began.
	template <class T, class... Fields>
	inline bool restoreStationInitialFields(T const& running, T& offered, Fields... fields)
	{
		bool touched = false;
		auto one = [&](auto field) {
			if ((offered.*field) != (running.*field))
			{
				offered.*field = running.*field;
				touched = true;
			}
		};
		(one(fields), ...);
		return touched;
	}

	// A node type whose settings carry no initial-condition fields.
#define PF_STATION_LIVE_NODE(SETTINGS, MEMBER, LABEL) \
	template <> \
	struct LiveNode<SETTINGS> \
	{ \
		static constexpr std::string_view nodeType = LABEL; \
		static std::vector<SETTINGS>& of(StationSystemsSettings& settings) { return settings.MEMBER; } \
		static const std::vector<SETTINGS>& of(StationSystemsSettings const& settings) { return settings.MEMBER; } \
		static bool restoreInitial(SETTINGS const&, SETTINGS&) { return false; } \
	};

#define PF_STATION_LIVE_NODE_WITH(SETTINGS, MEMBER, LABEL, ...) \
	template <> \
	struct LiveNode<SETTINGS> \
	{ \
		static constexpr std::string_view nodeType = LABEL; \
		static std::vector<SETTINGS>& of(StationSystemsSettings& settings) { return settings.MEMBER; } \
		static const std::vector<SETTINGS>& of(StationSystemsSettings const& settings) { return settings.MEMBER; } \
		static bool restoreInitial(SETTINGS const& running, SETTINGS& offered) \
		{ return restoreStationInitialFields(running, offered, __VA_ARGS__); } \
	};

	PF_STATION_LIVE_NODE_WITH(ResourceStoreSettings, stores, "Store",
		&ResourceStoreSettings::startingQualified,
		&ResourceStoreSettings::startingUnqualified)
	PF_STATION_LIVE_NODE(ResourceTransferSettings, transfers, "Transfer")
	PF_STATION_LIVE_NODE(ResourceConversionSettings, conversions, "Conversion")
	// #845: which fault modes a solar array can suffer is a run-shape
	// decision frozen at construction; the loss fraction and the fixed
	// availability inside each block stay live-tunable parameters whose
	// value an accepted injection froze for the Fault it started.
	template <> struct LiveNode<SolarArraySettings>
	{
		static constexpr std::string_view nodeType = "Solar";
		static auto& of(auto& settings) { return settings.solarArrays; }
		static bool restoreInitial(SolarArraySettings const&, SolarArraySettings&) { return false; }
		static bool topologyChanged(SolarArraySettings const& a, SolarArraySettings const& b)
		{
			return a.cellDegradation.has_value() != b.cellDegradation.has_value()
				|| a.stringLoss.has_value() != b.stringLoss.has_value()
				|| a.trackingFailure.has_value() != b.trackingFailure.has_value();
		}
	};
	PF_STATION_LIVE_NODE(EquipmentLoadSettings, equipmentLoads, "Equipment")
	template <> struct LiveNode<BatterySettings>
	{
		static constexpr std::string_view nodeType = "Battery";
		static auto& of(auto& settings) { return settings.batteries; }
		static bool restoreInitial(BatterySettings const& running, BatterySettings& offered)
		{ return restoreStationInitialFields(running, offered, &BatterySettings::startingStoredKWh); }
		static bool topologyChanged(BatterySettings const& a, BatterySettings const& b)
		{
			// #843: whether the bank has a capacity-fade model at all is a
			// run-shape decision frozen at construction; the fade rates
			// inside it stay live-tunable parameters that never reprice the
			// fade already accumulated. #845: the thermal-runaway opt-in
			// and its destination compartment join the frozen shape - the
			// compartment a hazard lands in is decided with the run, not
			// moved under it; the booked heat and toxic mass stay
			// live-tunable until an injection freezes them.
			return a.capacityFade.has_value() != b.capacityFade.has_value()
				|| a.thermalRunaway.has_value() != b.thermalRunaway.has_value()
				|| (a.thermalRunaway && b.thermalRunaway
					&& a.thermalRunaway->compartmentId != b.thermalRunaway->compartmentId);
		}
	};
	PF_STATION_LIVE_NODE(ResourceDeliverySettings, deliveries, "Delivery")
	PF_STATION_LIVE_NODE(WasteEjectionSettings, wasteEjections, "Waste ejection")
	PF_STATION_LIVE_NODE(InstallationOutageSettings, outages, "Outage")
	PF_STATION_LIVE_NODE_WITH(ThermalReservoirSettings, thermalReservoirs, "Reservoir",
		&ThermalReservoirSettings::startingTemperatureC)
	// #844: a radiator's puncture opt-in, its destination compartment and
	// its panel charge size are frozen topology - the charge is the audit
	// baseline for the puncture booking, like a loop's starting coolant.
	// The bleed rate and specific heat inside the block stay live-tunable
	// parameters, and the repair wiring follows the loop's rule: store
	// identities and plan identities frozen, durations and quantities live.
	template <> struct LiveNode<RadiatorSettings>
	{
		static constexpr std::string_view nodeType = "Radiator";
		static auto& of(auto& settings) { return settings.radiators; }
		static bool restoreInitial(RadiatorSettings const& running, RadiatorSettings& offered)
		{
			if (offered.panelPuncture && running.panelPuncture
				&& offered.panelPuncture->coolantKg != running.panelPuncture->coolantKg)
			{
				offered.panelPuncture->coolantKg = running.panelPuncture->coolantKg;
				return true;
			}
			return false;
		}
		static bool topologyChanged(RadiatorSettings const& a, RadiatorSettings const& b)
		{
			return a.panelPuncture.has_value() != b.panelPuncture.has_value()
				|| (a.panelPuncture && b.panelPuncture
					&& a.panelPuncture->compartmentId != b.panelPuncture->compartmentId)
				|| repairWiringTopologyChanged(a.repairParts, b.repairParts);
		}
	};
	// #844: the chiller's fault opt-ins are frozen topology, as for every
	// other fault-carrying kind: which modes a machine can suffer, where a
	// leak lands and what its charge is cannot move under a live run. The
	// leak rate and specific heat inside the block stay live-tunable.
	template <> struct LiveNode<ChillerSettings>
	{
		static constexpr std::string_view nodeType = "Chiller";
		static auto& of(auto& settings) { return settings.chillers; }
		static bool restoreInitial(ChillerSettings const& running, ChillerSettings& offered)
		{
			if (offered.refrigerantLeak && running.refrigerantLeak
				&& offered.refrigerantLeak->chargeKg != running.refrigerantLeak->chargeKg)
			{
				offered.refrigerantLeak->chargeKg = running.refrigerantLeak->chargeKg;
				return true;
			}
			return false;
		}
		static bool topologyChanged(ChillerSettings const& a, ChillerSettings const& b)
		{
			return a.compressorFailure != b.compressorFailure
				|| a.refrigerantLeak.has_value() != b.refrigerantLeak.has_value()
				|| (a.refrigerantLeak && b.refrigerantLeak
					&& a.refrigerantLeak->compartmentId != b.refrigerantLeak->compartmentId)
				|| repairWiringTopologyChanged(a.repairParts, b.repairParts);
		}
	};
	template <> struct LiveNode<CoolantLoopSettings>
	{
		static constexpr std::string_view nodeType = "Coolant loop";
		static auto& of(auto& settings) { return settings.coolantLoops; }
		static bool restoreInitial(CoolantLoopSettings const& running, CoolantLoopSettings& offered)
		{ return restoreStationInitialFields(running, offered, &CoolantLoopSettings::startingCoolantKg,
			&CoolantLoopSettings::startingTemperatureC); }
		static bool topologyChanged(CoolantLoopSettings const& a, CoolantLoopSettings const& b)
		{
			// Whether the loop has a Condition model at all, and which
			// service stock funds its recovery, are run-shape decisions
			// resolved at construction (#840); the rates inside the model
			// stay live-tunable parameters.
			return a.coolantLeak.has_value() != b.coolantLeak.has_value()
				|| (a.coolantLeak && b.coolantLeak && a.coolantLeak->compartmentId != b.coolantLeak->compartmentId)
				|| a.condition.has_value() != b.condition.has_value()
				|| (a.condition && b.condition
					&& a.condition->restoreStoreId != b.condition->restoreStoreId)
				// #843: whether the loop has a Wear model at all is frozen
				// topology; the fade rates inside it stay live-tunable
				// parameters that never reprice the fade already accumulated.
				|| a.wear.has_value() != b.wear.has_value()
				// #842: the repair-parts store identity and the per-mode
				// plan identities are topology - which faults are repairable
				// and which stores they draw from cannot move under a live
				// run. Each plan's duration and parts quantity stay
				// live-tunable parameters; a repair already started keeps the
				// parameters it froze at command time.
				|| a.repairParts.has_value() != b.repairParts.has_value()
				|| (a.repairParts && b.repairParts
					&& (a.repairParts->partsStoreId != b.repairParts->partsStoreId
						|| a.repairParts->repairs.size() != b.repairParts->repairs.size()
						|| !std::equal(a.repairParts->repairs.begin(), a.repairParts->repairs.end(),
							b.repairParts->repairs.begin(),
							[](FaultRepairSettings const& x, FaultRepairSettings const& y)
							{
								return x.modeId == y.modeId && x.wasteStoreId == y.wasteStoreId;
							}))
				);
		}
	};
	template <>
	struct LiveNode<HeatEngineSettings>
	{
		static constexpr std::string_view nodeType = "Heat engine";
		static auto& of(StationSystemsSettings& settings) { return settings.heatEngines; }
		static auto const& of(StationSystemsSettings const& settings) { return settings.heatEngines; }
		static bool restoreInitial(HeatEngineSettings const&, HeatEngineSettings&) { return false; }
		static bool topologyChanged(HeatEngineSettings const& before, HeatEngineSettings const& after)
		{
			// #844: the seizure opt-in and the repair wiring join the
			// frozen topology; every rate inside stays live-tunable.
			return before.efficiencyDrift.has_value() != after.efficiencyDrift.has_value()
				|| before.id != after.id || before.hotReservoirId != after.hotReservoirId
				|| before.rejectionReservoirId != after.rejectionReservoirId
				|| before.mechanicalSeizure != after.mechanicalSeizure
				|| repairWiringTopologyChanged(before.repairParts, after.repairParts);
		}
	};
	// #845: the generator's failed-start opt-in is a run-shape decision
	// frozen at construction; the mode carries no parameters, so there
	// is nothing live inside it to tune.
	template <> struct LiveNode<GeneratorSettings>
	{
		static constexpr std::string_view nodeType = "Generator";
		static auto& of(auto& settings) { return settings.generators; }
		static bool restoreInitial(GeneratorSettings const&, GeneratorSettings&) { return false; }
		static bool topologyChanged(GeneratorSettings const& a, GeneratorSettings const& b)
		{
			return a.fouledInjector.has_value() != b.fouledInjector.has_value() || a.failedStart != b.failedStart;
		}
	};
	template <> struct LiveNode<AtmosphereSettings>
	{
		static constexpr std::string_view nodeType = "Atmosphere";
		static auto& of(auto& settings) { return settings.atmospheres; }
		static bool restoreInitial(AtmosphereSettings const& a, AtmosphereSettings& b)
		{
			return restoreStationInitialFields(a, b,
				&AtmosphereSettings::startingCarbonDioxideKg,
				&AtmosphereSettings::startingNitrogenKg,
				&AtmosphereSettings::startingOxygenKg,
				&AtmosphereSettings::startingTemperatureC,
				&AtmosphereSettings::startingWaterVapourKg);
		}
		static bool topologyChanged(AtmosphereSettings const& a, AtmosphereSettings const& b)
		{ return a.fire.has_value() != b.fire.has_value(); }
	};
	// Openness is a live parameter, not an initial condition: a pathway owns
	// no field a live edit must be refused.
	PF_STATION_LIVE_NODE(AtmospherePathwaySettings, atmospherePathways, "Atmosphere pathway")
	// A Thermal link has no initial-condition fields: conductance is a live
	// thermal parameter while its identity and endpoints stay frozen topology.
	PF_STATION_LIVE_NODE(ThermalLinkSettings, thermalLinks, "Thermal link")
	template <> struct LiveNode<AirlockCycleSettings>
	{
		static constexpr std::string_view nodeType = "Airlock cycle";
		static auto& of(auto& settings) { return settings.airlockCycles; }
		static bool restoreInitial(AirlockCycleSettings const&, AirlockCycleSettings&) { return false; }
		static bool topologyChanged(AirlockCycleSettings const& a, AirlockCycleSettings const& b)
		{
			return a.interruptedCycle != b.interruptedCycle
				|| a.interlockFailure.has_value() != b.interlockFailure.has_value()
				|| (a.interlockFailure && b.interlockFailure
					&& a.interlockFailure->pathwayId != b.interlockFailure->pathwayId);
		}
	};
	PF_STATION_LIVE_NODE(CrewSettings, crews, "Crew")
	PF_STATION_LIVE_NODE(WaterCollectorSettings, waterCollectors, "Water collector")
	template <> struct LiveNode<WaterTreatmentSettings>
	{
		static constexpr std::string_view nodeType = "Water treatment";
		static auto& of(auto& settings) { return settings.waterTreatments; }
		static bool restoreInitial(WaterTreatmentSettings const&, WaterTreatmentSettings&) { return false; }
		static bool topologyChanged(WaterTreatmentSettings const& a, WaterTreatmentSettings const& b)
		{ return a.membraneFouling.has_value() != b.membraneFouling.has_value() || a.breakthrough != b.breakthrough; }
	};
	template <> struct LiveNode<ElectrolyzerSettings>
	{
		static constexpr std::string_view nodeType = "Electrolyzer";
		static auto& of(auto& settings) { return settings.electrolyzers; }
		static bool validateParameters(ElectrolyzerSettings const& entry)
		{
			if (!entry.thermalBody) return true;
			auto const& b = *entry.thermalBody;
			return std::isfinite(b.heatCapacityKJPerK) && b.heatCapacityKJPerK > 0
				&& std::isfinite(b.sinkConductanceKWPerK) && b.sinkConductanceKWPerK > 0
				&& std::isfinite(b.startingTemperatureC) && b.startingTemperatureC > -StationCelsiusOffset;
		}
		static bool restoreInitial(ElectrolyzerSettings const&, ElectrolyzerSettings&) { return false; }
		static bool topologyChanged(ElectrolyzerSettings const& a, ElectrolyzerSettings const& b)
		{ return a.thermalBody.has_value() != b.thermalBody.has_value()
			|| a.membraneDegradation.has_value() != b.membraneDegradation.has_value()
			|| a.catalystPoisoning.has_value() != b.catalystPoisoning.has_value(); }
	};
	template <> struct LiveNode<Co2CaptureSettings>
	{
		static constexpr std::string_view nodeType = "CO2 capture";
		static auto& of(auto& settings) { return settings.co2Captures; }
		static bool restoreInitial(Co2CaptureSettings const&, Co2CaptureSettings&) { return false; }
		static bool topologyChanged(Co2CaptureSettings const& a, Co2CaptureSettings const& b)
		{ return a.absorbentDegradation.has_value() != b.absorbentDegradation.has_value(); }
	};
	template <> struct LiveNode<FuelCellSettings>
	{
		static constexpr std::string_view nodeType = "Fuel cell";
		static auto& of(auto& settings) { return settings.fuelCells; }
		static bool restoreInitial(FuelCellSettings const&, FuelCellSettings&) { return false; }
		static bool topologyChanged(FuelCellSettings const& a, FuelCellSettings const& b)
		{ return a.chemistry != b.chemistry
			|| a.stackDegradation.has_value() != b.stackDegradation.has_value()
			|| a.catalystPoisoning.has_value() != b.catalystPoisoning.has_value(); }
	};
	template <> struct LiveNode<SabatierReactorSettings>
	{
		static constexpr std::string_view nodeType = "Sabatier";
		static auto& of(auto& settings) { return settings.sabatierReactors; }
		static bool restoreInitial(SabatierReactorSettings const&, SabatierReactorSettings&) { return false; }
		static bool topologyChanged(SabatierReactorSettings const& a, SabatierReactorSettings const& b)
		{ return a.catalystDeactivation.has_value() != b.catalystDeactivation.has_value()
			|| a.overtempRunaway.has_value() != b.overtempRunaway.has_value(); }
	};
	template <> struct LiveNode<MethaneProcessorSettings>
	{
		static constexpr std::string_view nodeType = "Methane processor";
		static auto& of(auto& settings) { return settings.methaneProcessors; }
		static bool restoreInitial(MethaneProcessorSettings const&, MethaneProcessorSettings&) { return false; }
		static bool topologyChanged(MethaneProcessorSettings const& a, MethaneProcessorSettings const& b)
		{ return a.catalystDeactivation.has_value() != b.catalystDeactivation.has_value()
			|| a.overtempRunaway.has_value() != b.overtempRunaway.has_value(); }
	};
	PF_STATION_LIVE_NODE_WITH(MicrobialProcessorSettings, microbialProcessors, "Microbial processor",
		&MicrobialProcessorSettings::initialCondition)
	PF_STATION_LIVE_NODE_WITH(ThermalBufferSettings, thermalBuffers, "Thermal buffer",
		&ThermalBufferSettings::startingEnergyKJ)
	PF_STATION_LIVE_NODE_WITH(CropPopulationSettings, cropPopulations, "Crops",
		&CropPopulationSettings::initialBiomassKg)
	PF_STATION_LIVE_NODE_WITH(AlgaeCultureSettings, algaeCultures, "Algae",
		&AlgaeCultureSettings::initialBiomassKg)
	PF_STATION_LIVE_NODE(MaintenanceJobSettings, maintenanceJobs, "Maintenance")

#undef PF_STATION_LIVE_NODE
#undef PF_STATION_LIVE_NODE_WITH
}
