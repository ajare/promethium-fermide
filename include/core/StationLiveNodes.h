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

#include <string_view>
#include <vector>

namespace core
{
	template <class Settings>
	struct LiveNode; // undefined: specialise below via PF_STATION_LIVE_NODE

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
	PF_STATION_LIVE_NODE(SolarArraySettings, solarArrays, "Solar")
	PF_STATION_LIVE_NODE(EquipmentLoadSettings, equipmentLoads, "Equipment")
	PF_STATION_LIVE_NODE_WITH(BatterySettings, batteries, "Battery",
		&BatterySettings::startingStoredKWh)
	PF_STATION_LIVE_NODE(ResourceDeliverySettings, deliveries, "Delivery")
	PF_STATION_LIVE_NODE(WasteEjectionSettings, wasteEjections, "Waste ejection")
	PF_STATION_LIVE_NODE(InstallationOutageSettings, outages, "Outage")
	PF_STATION_LIVE_NODE_WITH(ThermalReservoirSettings, thermalReservoirs, "Reservoir",
		&ThermalReservoirSettings::startingTemperatureC)
	PF_STATION_LIVE_NODE(RadiatorSettings, radiators, "Radiator")
	template <> struct LiveNode<CoolantLoopSettings>
	{
		static constexpr std::string_view nodeType = "Coolant loop";
		static auto& of(auto& settings) { return settings.coolantLoops; }
		static bool restoreInitial(CoolantLoopSettings const& running, CoolantLoopSettings& offered)
		{ return restoreStationInitialFields(running, offered, &CoolantLoopSettings::startingCoolantKg,
			&CoolantLoopSettings::startingTemperatureC); }
		static bool topologyChanged(CoolantLoopSettings const& a, CoolantLoopSettings const& b)
		{
			return a.coolantLeak.has_value() != b.coolantLeak.has_value()
				|| (a.coolantLeak && b.coolantLeak && a.coolantLeak->compartmentId != b.coolantLeak->compartmentId);
		}
	};
	PF_STATION_LIVE_NODE(ChillerSettings, chillers, "Chiller")
	template <>
	struct LiveNode<HeatEngineSettings>
	{
		static constexpr std::string_view nodeType = "Heat engine";
		static auto& of(StationSystemsSettings& settings) { return settings.heatEngines; }
		static auto const& of(StationSystemsSettings const& settings) { return settings.heatEngines; }
		static bool restoreInitial(HeatEngineSettings const&, HeatEngineSettings&) { return false; }
		static bool topologyChanged(HeatEngineSettings const& before, HeatEngineSettings const& after)
		{
			return before.id != after.id || before.hotReservoirId != after.hotReservoirId
				|| before.rejectionReservoirId != after.rejectionReservoirId;
		}
	};
	PF_STATION_LIVE_NODE(GeneratorSettings, generators, "Generator")
	PF_STATION_LIVE_NODE_WITH(AtmosphereSettings, atmospheres, "Atmosphere",
		&AtmosphereSettings::startingCarbonDioxideKg,
		&AtmosphereSettings::startingNitrogenKg,
		&AtmosphereSettings::startingOxygenKg,
		&AtmosphereSettings::startingTemperatureC,
		&AtmosphereSettings::startingWaterVapourKg)
	// Openness is a live parameter, not an initial condition: a pathway owns
	// no field a live edit must be refused.
	PF_STATION_LIVE_NODE(AtmospherePathwaySettings, atmospherePathways, "Atmosphere pathway")
	// A Thermal link has no initial-condition fields: conductance is a live
	// thermal parameter while its identity and endpoints stay frozen topology.
	PF_STATION_LIVE_NODE(ThermalLinkSettings, thermalLinks, "Thermal link")
	PF_STATION_LIVE_NODE(AirlockCycleSettings, airlockCycles, "Airlock cycle")
	PF_STATION_LIVE_NODE(CrewSettings, crews, "Crew")
	PF_STATION_LIVE_NODE(WaterCollectorSettings, waterCollectors, "Water collector")
	PF_STATION_LIVE_NODE(WaterTreatmentSettings, waterTreatments, "Water treatment")
	PF_STATION_LIVE_NODE(ElectrolyzerSettings, electrolyzers, "Electrolyzer")
	PF_STATION_LIVE_NODE(Co2CaptureSettings, co2Captures, "CO2 capture")
	PF_STATION_LIVE_NODE(FuelCellSettings, fuelCells, "Fuel cell")
	PF_STATION_LIVE_NODE(SabatierReactorSettings, sabatierReactors, "Sabatier")
	PF_STATION_LIVE_NODE(MethaneProcessorSettings, methaneProcessors, "Methane processor")
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
