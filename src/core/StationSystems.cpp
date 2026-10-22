#include "StationSystemsImpl.h"
#include "BundledStationSystemsSource.h"
namespace core
{
namespace {
StationSystemsSourceLoader sourceLoader;
std::optional<uint32_t> stationSystemsRuntimeStepRatioOverride;
void pushWaterQuality(lua_State* state, WaterQualitySettings const& gate)
{
	lua_createtable(state, 0, 2);
	if (gate.maxTotalDissolved)
	{ lua_pushnumber(state, *gate.maxTotalDissolved); lua_setfield(state, -2, "maxTotalDissolved"); }
	lua_createtable(state, 0, static_cast<int>(gate.maxConcentrations.size()));
	for (auto const& [name, value] : gate.maxConcentrations)
	{ lua_pushnumber(state, value); lua_setfield(state, -2, name.c_str()); }
	lua_setfield(state, -2, "maxConcentrations");
	lua_setfield(state, -2, "waterQuality");
}
		struct Construction
		{
			ScriptObject* object;
			std::string const* source;
			char const* name;
			std::vector<ResourceStoreSettings> const* stores;
			std::vector<ResourceTransferSettings> const* transfers;
			std::vector<ResourceConversionSettings> const* conversions;
			std::vector<SolarArraySettings> const* solar;
			std::vector<EquipmentLoadSettings> const* loads;
			std::vector<BatterySettings> const* batteries;
			std::vector<ResourceDeliverySettings> const* deliveries;
			std::vector<ThermalReservoirSettings> const* reservoirs;
			std::vector<RadiatorSettings> const* radiators;
			std::vector<CoolantLoopSettings> const* coolantLoops;
			std::vector<ChillerSettings> const* chillers;
			std::vector<GeneratorSettings> const* generators;
			std::vector<AtmosphereSettings> const* atmospheres;
			std::vector<AirlockCycleSettings> const* airlockCycles;
			std::vector<CrewSettings> const* crews;
			std::vector<WaterCollectorSettings> const* waterCollectors;
			std::vector<WaterTreatmentSettings> const* waterTreatments;
			std::vector<ElectrolyzerSettings> const* electrolyzers;
			std::vector<Co2CaptureSettings> const* co2Captures;
			std::vector<FuelCellSettings> const* fuelCells;
			std::vector<ThermalBufferSettings> const* thermalBuffers;
			std::vector<WasteEjectionSettings> const* wasteEjections;
			std::vector<InstallationOutageSettings> const* outages;
			std::vector<SabatierReactorSettings> const* sabatierReactors;
			std::vector<MethaneProcessorSettings> const* methaneProcessors;
			std::vector<MicrobialProcessorSettings> const* microbialProcessors;
			std::vector<CropPopulationSettings> const* cropPopulations;
			std::vector<AlgaeCultureSettings> const* algaeCultures;
			char const* dispatchMode{"manual"};
			std::vector<MaintenanceJobSettings> const* maintenanceJobs{};
			static char const* settingsDispatchModeName(StationDispatchMode mode)
			{
				return mode == StationDispatchMode::Automatic ? "automatic" : "manual";
			}
			static void pushProfile(lua_State* state, ProfileSettings const& profile)
			{
				lua_createtable(state, 0, 3);
				lua_pushnumber(state, profile.constantLevel); lua_setfield(state, -2, "constantLevel");
				lua_pushnumber(state, profile.periodSeconds); lua_setfield(state, -2, "periodSeconds");
				lua_createtable(state, static_cast<int>(profile.segments.size()), 0);
				for (size_t index = 0; index < profile.segments.size(); ++index)
				{
					lua_createtable(state, 0, 2);
					lua_pushnumber(state, profile.segments[index].startSeconds); lua_setfield(state, -2, "startSeconds");
					lua_pushnumber(state, profile.segments[index].level); lua_setfield(state, -2, "level");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_setfield(state, -2, "segments");
			}
			int operator()(lua_State* state) const
			{
				script::pushPrivateEnvironment(state);
				if (luaL_loadbufferx(state, source->data(), source->size(), "@station-systems/lifecycle.lua", "t") != LUA_OK)
					return lua_error(state);
				lua_pushvalue(state, -2); lua_setupvalue(state, -2, 1);
				lua_call(state, 0, 1);
				lua_pushstring(state, name);
				// Authored store descriptors cross the sandbox boundary as plain
				// immutable values; the Lua model initialises its own copy.
				lua_createtable(state, static_cast<int>(stores->size()), 0);
				for (size_t index = 0; index < stores->size(); ++index)
				{
					auto const& store = (*stores)[index];
					lua_createtable(state, 0, store.composition ? 7 : 6);
					lua_pushstring(state, store.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, store.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, store.capacity); lua_setfield(state, -2, "capacity");
					lua_pushnumber(state, store.startingQualified); lua_setfield(state, -2, "qualified");
					lua_pushnumber(state, store.startingUnqualified); lua_setfield(state, -2, "unqualified");
					lua_pushnumber(state, store.protectedReserve); lua_setfield(state, -2, "protectedReserve");
					if (store.composition)
					{
						auto const& composition = *store.composition;
						lua_createtable(state, 0, 7);
						lua_pushnumber(state, composition.moisture); lua_setfield(state, -2, "moisture");
						lua_pushnumber(state, composition.nutrientNitrogen); lua_setfield(state, -2, "nutrientNitrogen");
						lua_pushnumber(state, composition.atmosphericNitrogen); lua_setfield(state, -2, "atmosphericNitrogen");
						lua_pushnumber(state, composition.phosphorus); lua_setfield(state, -2, "phosphorus");
						lua_pushnumber(state, composition.potassium); lua_setfield(state, -2, "potassium");
						lua_pushnumber(state, composition.otherMinerals); lua_setfield(state, -2, "otherMinerals");
						lua_pushnumber(state, composition.energyKJPerKgDry); lua_setfield(state, -2, "energyKJPerKgDry");
						lua_setfield(state, -2, "composition");
					}
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored transfer rules cross the same boundary as plain
				// immutable values; each sandbox mirrors and audits them itself.
				lua_createtable(state, static_cast<int>(transfers->size()), 0);
				for (size_t index = 0; index < transfers->size(); ++index)
				{
					auto const& transfer = (*transfers)[index];
					lua_createtable(state, 0, 6);
					lua_pushstring(state, transfer.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, transfer.sourceId.c_str()); lua_setfield(state, -2, "source");
					lua_pushstring(state, transfer.destinationId.c_str()); lua_setfield(state, -2, "destination");
					lua_pushstring(state, transfer.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, transfer.mode == ResourceTransferMode::Continuous ? transfer.ratePerSecond : 0.0); lua_setfield(state, -2, "rate");
					lua_pushboolean(state, transfer.mode == ResourceTransferMode::Scheduled); lua_setfield(state, -2, "scheduled");
					lua_pushnumber(state, transfer.quantity); lua_setfield(state, -2, "quantity");
					lua_pushnumber(state, transfer.firstOccurrenceSeconds); lua_setfield(state, -2, "firstOccurrenceSeconds");
					lua_pushinteger(state, transfer.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored conversion rules cross the same boundary as plain
				// immutable descriptors; each sandbox mirrors and audits them.
				lua_createtable(state, static_cast<int>(conversions->size()), 0);
				for (size_t index = 0; index < conversions->size(); ++index)
				{
					auto const& conversion = (*conversions)[index];
					lua_createtable(state, 0, 3);
					lua_pushstring(state, conversion.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushinteger(state, conversion.priority); lua_setfield(state, -2, "priority");
					lua_createtable(state, static_cast<int>(conversion.inputs.size()), 0);
					for (size_t flow = 0; flow < conversion.inputs.size(); ++flow)
					{
						lua_createtable(state, 0, 2);
						lua_pushstring(state, conversion.inputs[flow].storeId.c_str()); lua_setfield(state, -2, "store");
						lua_pushnumber(state, conversion.inputs[flow].ratePerSecond); lua_setfield(state, -2, "rate");
						lua_rawseti(state, -2, static_cast<int>(flow) + 1);
					}
					lua_setfield(state, -2, "inputs");
					lua_createtable(state, static_cast<int>(conversion.outputs.size()), 0);
					for (size_t flow = 0; flow < conversion.outputs.size(); ++flow)
					{
						lua_createtable(state, 0, 2);
						lua_pushstring(state, conversion.outputs[flow].storeId.c_str()); lua_setfield(state, -2, "store");
						lua_pushnumber(state, conversion.outputs[flow].yieldPerSecond); lua_setfield(state, -2, "yield");
						lua_rawseti(state, -2, static_cast<int>(flow) + 1);
					}
					lua_setfield(state, -2, "outputs");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored solar array and External equipment load descriptors
				// cross the same boundary as plain immutable values (#393);
				// each sandbox mirrors and audits the supply allocation itself.
				lua_createtable(state, static_cast<int>(solar->size()), 0);
				for (size_t index = 0; index < solar->size(); ++index)
				{
					auto const& array = (*solar)[index];
					lua_createtable(state, 0, 3);
					lua_pushstring(state, array.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushnumber(state, array.capacityKW); lua_setfield(state, -2, "capacityKW");
					pushProfile(state, array.illumination); lua_setfield(state, -2, "illumination");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_createtable(state, static_cast<int>(loads->size()), 0);
				for (size_t index = 0; index < loads->size(); ++index)
				{
					auto const& load = (*loads)[index];
					lua_createtable(state, 0, 9);
					lua_pushstring(state, load.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushnumber(state, load.electricalDemandKW); lua_setfield(state, -2, "demandKW");
					pushProfile(state, load.operatingProfile); lua_setfield(state, -2, "profile");
					lua_pushnumber(state, load.heatOutputKW); lua_setfield(state, -2, "heatKW");
					lua_pushstring(state, load.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, load.coolingDisposition == StationCoolingDisposition::Exported
						? "exported" : "coolingService"); lua_setfield(state, -2, "cooling");
					lua_pushnumber(state, load.criticalFloorKW); lua_setfield(state, -2, "criticalFloorKW");
					lua_pushinteger(state, load.priority); lua_setfield(state, -2, "priority");
					lua_pushnumber(state, load.maxCurtailment); lua_setfield(state, -2, "maxCurtailment");
					lua_pushnumber(state, load.recoveryDwellSeconds); lua_setfield(state, -2, "recoveryDwellSeconds");
					lua_pushnumber(state, load.recoveryHysteresisFraction); lua_setfield(state, -2, "recoveryHysteresisFraction");
					lua_pushnumber(state, load.restartSurgePowerKW); lua_setfield(state, -2, "restartSurgePowerKW");
					lua_pushnumber(state, load.restartSurgeSeconds); lua_setfield(state, -2, "restartSurgeSeconds");
					lua_pushstring(state, load.heatSinkId.c_str()); lua_setfield(state, -2, "heatSinkId");
					lua_createtable(state, static_cast<int>(load.materialExchanges.size()), 0);
					for (size_t flow = 0; flow < load.materialExchanges.size(); ++flow)
					{
						auto const& exchange = load.materialExchanges[flow];
						lua_createtable(state, 0, 3);
						lua_pushstring(state, exchange.storeId.c_str()); lua_setfield(state, -2, "store");
						lua_pushnumber(state, exchange.ratePerSecond); lua_setfield(state, -2, "rate");
						lua_pushboolean(state, exchange.consumes ? 1 : 0); lua_setfield(state, -2, "consumes");
						lua_rawseti(state, -2, static_cast<int>(flow) + 1);
					}
					lua_setfield(state, -2, "exchanges");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored battery descriptors cross the same boundary as plain
				// immutable values (#394); each sandbox mirrors the discharge and
				// charge ledger and audits its own energy conservation.
				lua_createtable(state, static_cast<int>(batteries->size()), 0);
				for (size_t index = 0; index < batteries->size(); ++index)
				{
					auto const& battery = (*batteries)[index];
					lua_createtable(state, 0, 8);
					lua_pushstring(state, battery.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushnumber(state, battery.nameplateKWh); lua_setfield(state, -2, "nameplateKWh");
					lua_pushnumber(state, battery.usableKWh); lua_setfield(state, -2, "usableKWh");
					lua_pushnumber(state, battery.startingStoredKWh); lua_setfield(state, -2, "startingStoredKWh");
					lua_pushnumber(state, battery.maxChargeKW); lua_setfield(state, -2, "maxChargeKW");
					lua_pushnumber(state, battery.maxDischargeKW); lua_setfield(state, -2, "maxDischargeKW");
					lua_pushnumber(state, battery.chargeEfficiency); lua_setfield(state, -2, "chargeEfficiency");
					lua_pushnumber(state, battery.dischargeEfficiency); lua_setfield(state, -2, "dischargeEfficiency");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored resupply delivery descriptors cross the same
				// boundary as plain immutable values (#392); each sandbox
				// mirrors the schedule and audits its own arrivals.
				lua_createtable(state, static_cast<int>(deliveries->size()), 0);
				for (size_t index = 0; index < deliveries->size(); ++index)
				{
					auto const& delivery = (*deliveries)[index];
					lua_createtable(state, 0, 8);
					lua_pushstring(state, delivery.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, delivery.storeId.c_str()); lua_setfield(state, -2, "store");
					lua_pushstring(state, delivery.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, delivery.quantity); lua_setfield(state, -2, "quantity");
					lua_pushnumber(state, delivery.firstOccurrenceSeconds); lua_setfield(state, -2, "firstSeconds");
					lua_pushnumber(state, delivery.recurrenceSeconds); lua_setfield(state, -2, "recurrenceSeconds");
					lua_pushboolean(state, delivery.arrivesQualified ? 1 : 0); lua_setfield(state, -2, "qualified");
					lua_pushinteger(state, delivery.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored thermal reservoir and radiator descriptors cross
				// the same boundary as plain immutable values (#395); each
				// sandbox mirrors the integration and audits its own thermal
				// energy conservation.
				lua_createtable(state, static_cast<int>(reservoirs->size()), 0);
				for (size_t index = 0; index < reservoirs->size(); ++index)
				{
					auto const& reservoir = (*reservoirs)[index];
					lua_createtable(state, 0, 7);
					lua_pushstring(state, reservoir.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, reservoir.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushnumber(state, reservoir.heatCapacityKJPerK); lua_setfield(state, -2, "heatCapacityKJPerK");
					lua_pushnumber(state, reservoir.startingTemperatureC); lua_setfield(state, -2, "startingTemperatureC");
					lua_pushnumber(state, reservoir.minTemperatureC); lua_setfield(state, -2, "minTemperatureC");
					lua_pushnumber(state, reservoir.maxTemperatureC); lua_setfield(state, -2, "maxTemperatureC");
					lua_pushnumber(state, reservoir.absorbedHeatKW); lua_setfield(state, -2, "absorbedHeatKW");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_createtable(state, static_cast<int>(radiators->size()), 0);
				for (size_t index = 0; index < radiators->size(); ++index)
				{
					auto const& radiator = (*radiators)[index];
					lua_createtable(state, 0, 6);
					lua_pushstring(state, radiator.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, radiator.reservoirId.c_str()); lua_setfield(state, -2, "reservoir");
					lua_pushnumber(state, radiator.areaM2); lua_setfield(state, -2, "areaM2");
					lua_pushnumber(state, radiator.emissivity); lua_setfield(state, -2, "emissivity");
					lua_pushnumber(state, radiator.viewFactor); lua_setfield(state, -2, "viewFactor");
					lua_pushnumber(state, radiator.sinkTemperatureC); lua_setfield(state, -2, "sinkTemperatureC");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored coolant loop descriptors cross the same boundary as
				// plain immutable values (#396); each sandbox mirrors the
				// circulation, freeze-heating and loop energy ledger and
				// audits its own conservation.
				lua_createtable(state, static_cast<int>(coolantLoops->size()), 0);
				for (size_t index = 0; index < coolantLoops->size(); ++index)
				{
					auto const& loop = (*coolantLoops)[index];
					lua_createtable(state, 0, 12);
					lua_pushstring(state, loop.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, loop.reservoirId.c_str()); lua_setfield(state, -2, "reservoir");
					lua_pushnumber(state, loop.coolantCapacityKg); lua_setfield(state, -2, "coolantCapacityKg");
					lua_pushnumber(state, loop.startingCoolantKg); lua_setfield(state, -2, "startingCoolantKg");
					lua_pushnumber(state, loop.specificHeatKJPerKgK); lua_setfield(state, -2, "specificHeatKJPerKgK");
					lua_pushnumber(state, loop.startingTemperatureC); lua_setfield(state, -2, "startingTemperatureC");
					lua_pushnumber(state, loop.freezeProtectionSetpointC); lua_setfield(state, -2, "freezeProtectionSetpointC");
					lua_pushnumber(state, loop.pumpPowerKW); lua_setfield(state, -2, "pumpPowerKW");
					lua_pushnumber(state, loop.ratedFlowKgPerS); lua_setfield(state, -2, "ratedFlowKgPerS");
					lua_pushnumber(state, loop.exchangerConductanceKWPerK); lua_setfield(state, -2, "exchangerConductanceKWPerK");
					lua_pushnumber(state, loop.heaterCapacityKW); lua_setfield(state, -2, "heaterCapacityKW");
					lua_pushboolean(state, loop.criticalService ? 1 : 0);
					lua_setfield(state, -2, "criticalService");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored chiller descriptors cross the same boundary as
				// plain immutable values (#397); each sandbox mirrors the
				// cold-side extraction, the work draw and the condenser
				// booking, and audits the chiller energy ledger itself.
				lua_createtable(state, static_cast<int>(chillers->size()), 0);
				for (size_t index = 0; index < chillers->size(); ++index)
				{
					auto const& chiller = (*chillers)[index];
					lua_createtable(state, 0, 10);
					lua_pushstring(state, chiller.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, chiller.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, chiller.coldLoopId.c_str()); lua_setfield(state, -2, "coldLoop");
					lua_pushstring(state, chiller.condenserReservoirId.c_str()); lua_setfield(state, -2, "condenser");
					lua_pushnumber(state, chiller.ratedCoolingCapacityKW); lua_setfield(state, -2, "ratedCoolingCapacityKW");
					lua_pushnumber(state, chiller.cop); lua_setfield(state, -2, "cop");
					lua_pushnumber(state, chiller.auxiliaryPowerKW); lua_setfield(state, -2, "auxiliaryPowerKW");
					lua_pushnumber(state, chiller.chilledSetpointC); lua_setfield(state, -2, "chilledSetpointC");
					lua_pushnumber(state, chiller.maxCondensingTemperatureC); lua_setfield(state, -2, "maxCondensingTemperatureC");
					lua_pushboolean(state, chiller.criticalService ? 1 : 0);
					lua_setfield(state, -2, "criticalService");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored generator descriptors cross the same boundary as
				// plain immutable values (#399); each sandbox mirrors the
				// startup/running/shutdown state machine, the fuel draw and
				// the heat routing, and audits the fuel ledger itself.
				lua_createtable(state, static_cast<int>(generators->size()), 0);
				for (size_t index = 0; index < generators->size(); ++index)
				{
					auto const& generator = (*generators)[index];
					lua_createtable(state, 0, 13);
					lua_pushstring(state, generator.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, generator.fuelStoreId.c_str()); lua_setfield(state, -2, "fuelStore");
					lua_pushstring(state, generator.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, generator.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, generator.ratedOutputKW); lua_setfield(state, -2, "ratedKW");
					lua_pushnumber(state, generator.efficiency); lua_setfield(state, -2, "efficiency");
					lua_pushnumber(state, generator.maxRampKWPerSecond); lua_setfield(state, -2, "rampKWPerSecond");
					lua_pushnumber(state, generator.commandedOutputKW); lua_setfield(state, -2, "commandedKW");
					lua_pushnumber(state, generator.startupPowerKW); lua_setfield(state, -2, "startupPowerKW");
					lua_pushnumber(state, generator.startupSeconds); lua_setfield(state, -2, "startupSeconds");
					lua_pushnumber(state, generator.shutdownHeatKW); lua_setfield(state, -2, "shutdownHeatKW");
					lua_pushnumber(state, generator.shutdownSeconds); lua_setfield(state, -2, "shutdownSeconds");
					lua_pushnumber(state, generator.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored compartment atmosphere descriptors cross the same
				// boundary as plain immutable values (#400); each sandbox
				// mirrors the occupancy draw, vapour constraint, leakage,
				// airlock cycling and thermal integration, and audits its
				// own per-component and thermal conservation.
				lua_createtable(state, static_cast<int>(atmospheres->size()), 0);
				for (size_t index = 0; index < atmospheres->size(); ++index)
				{
					auto const& atmosphere = (*atmospheres)[index];
					lua_createtable(state, 0, 19);
					lua_pushstring(state, atmosphere.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushnumber(state, atmosphere.volumeM3); lua_setfield(state, -2, "volumeM3");
					lua_pushnumber(state, atmosphere.startingOxygenKg); lua_setfield(state, -2, "startingOxygenKg");
					lua_pushnumber(state, atmosphere.startingNitrogenKg); lua_setfield(state, -2, "startingNitrogenKg");
					lua_pushnumber(state, atmosphere.startingCarbonDioxideKg); lua_setfield(state, -2, "startingCarbonDioxideKg");
					lua_pushnumber(state, atmosphere.startingWaterVapourKg); lua_setfield(state, -2, "startingWaterVapourKg");
					lua_pushnumber(state, atmosphere.startingTemperatureC); lua_setfield(state, -2, "startingTemperatureC");
					lua_pushnumber(state, atmosphere.thermalCapacityKJPerK); lua_setfield(state, -2, "thermalCapacityKJPerK");
					lua_pushstring(state, atmosphere.reservoirId.c_str()); lua_setfield(state, -2, "reservoir");
					lua_pushnumber(state, atmosphere.exchangerConductanceKWPerK); lua_setfield(state, -2, "exchangerConductanceKWPerK");
					lua_pushinteger(state, atmosphere.occupants); lua_setfield(state, -2, "occupants");
					lua_pushnumber(state, atmosphere.oxygenUsePerOccupantKgPerS); lua_setfield(state, -2, "oxygenUsePerOccupantKgPerS");
					lua_pushnumber(state, atmosphere.co2ProductionPerOccupantKgPerS); lua_setfield(state, -2, "co2ProductionPerOccupantKgPerS");
					lua_pushnumber(state, atmosphere.sensibleHeatPerOccupantKW); lua_setfield(state, -2, "sensibleHeatPerOccupantKW");
					lua_pushnumber(state, atmosphere.moistureAdditionPerOccupantKgPerS); lua_setfield(state, -2, "moistureAdditionPerOccupantKgPerS");
					lua_pushnumber(state, atmosphere.leakRateKgPerS); lua_setfield(state, -2, "leakRateKgPerS");
					lua_pushnumber(state, atmosphere.minPressureKPa); lua_setfield(state, -2, "minPressureKPa");
					lua_pushnumber(state, atmosphere.maxPressureKPa); lua_setfield(state, -2, "maxPressureKPa");
					lua_pushnumber(state, atmosphere.minOxygenPartialPressureKPa); lua_setfield(state, -2, "minOxygenPartialPressureKPa");
					lua_pushnumber(state, atmosphere.maxCarbonDioxidePartialPressureKPa); lua_setfield(state, -2, "maxCarbonDioxidePartialPressureKPa");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored airlock cycle descriptors cross the same boundary
				// as plain immutable values (#400); each sandbox mirrors the
				// schedule and the recovered/lost split and audits its own
				// accounting.
				lua_createtable(state, static_cast<int>(airlockCycles->size()), 0);
				for (size_t index = 0; index < airlockCycles->size(); ++index)
				{
					auto const& cycle = (*airlockCycles)[index];
					lua_createtable(state, 0, 7);
					lua_pushstring(state, cycle.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, cycle.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					lua_pushnumber(state, cycle.gasMassKg); lua_setfield(state, -2, "gasMassKg");
					lua_pushnumber(state, cycle.firstOccurrenceSeconds); lua_setfield(state, -2, "firstSeconds");
					lua_pushnumber(state, cycle.recurrenceSeconds); lua_setfield(state, -2, "recurrenceSeconds");
					lua_pushnumber(state, cycle.recoveredFraction); lua_setfield(state, -2, "recoveredFraction");
					lua_pushinteger(state, cycle.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored crew contracts cross the same boundary as plain
				// immutable descriptors (#401); each sandbox mirrors the
				// uniform-scale metabolism and audits its own closure.
				lua_createtable(state, static_cast<int>(crews->size()), 0);
				for (size_t index = 0; index < crews->size(); ++index)
				{
					auto const& crew = (*crews)[index];
					lua_createtable(state, 0, 14);
					lua_pushstring(state, crew.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, crew.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					lua_pushinteger(state, crew.crewCount); lua_setfield(state, -2, "crewCount");
					pushProfile(state, crew.activityProfile);
					lua_setfield(state, -2, "activity");
					lua_pushstring(state, crew.foodStoreId.c_str()); lua_setfield(state, -2, "foodStore");
					pushWaterQuality(state, crew.waterQuality);
					lua_pushstring(state, crew.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, crew.greywaterStoreId.c_str()); lua_setfield(state, -2, "greywaterStore");
					lua_pushnumber(state, crew.greywaterKgPerCrewDay); lua_setfield(state, -2, "greywaterKgPerCrewDay");
					lua_createtable(state, 0, static_cast<int>(crew.greywaterLoadKgPerCrewDay.size()));
					for (auto const& [name, mass] : crew.greywaterLoadKgPerCrewDay)
					{ lua_pushnumber(state, mass); lua_setfield(state, -2, name.c_str()); }
					lua_setfield(state, -2, "greywaterLoadKgPerCrewDay");
					lua_pushstring(state, crew.urineStoreId.c_str()); lua_setfield(state, -2, "urineStore");
					lua_pushstring(state, crew.organicWasteStoreId.c_str()); lua_setfield(state, -2, "organicWasteStore");
					lua_pushnumber(state, crew.foodPerCrewPerS); lua_setfield(state, -2, "foodRate");
					lua_pushnumber(state, crew.waterPerCrewPerS); lua_setfield(state, -2, "waterRate");
					lua_pushnumber(state, crew.oxygenPerCrewKgPerS); lua_setfield(state, -2, "oxygenRateKgPerS");
					lua_pushnumber(state, crew.co2PerCrewKgPerS); lua_setfield(state, -2, "co2RateKgPerS");
					lua_pushnumber(state, crew.moisturePerCrewKgPerS); lua_setfield(state, -2, "moistureRateKgPerS");
					lua_pushnumber(state, crew.urinePerCrewPerS); lua_setfield(state, -2, "urineRate");
					lua_pushnumber(state, crew.organicWastePerCrewPerS); lua_setfield(state, -2, "organicWasteRate");
					lua_pushnumber(state, crew.metabolicHeatPerCrewKW); lua_setfield(state, -2, "metabolicHeatKW");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored water recovery contracts cross the same boundary as
				// plain immutable descriptors (#402); the sandbox mirrors the
				// collection gate, the recovery split and the quality rule and
				// re-audits its own accounting.
				lua_createtable(state, static_cast<int>(waterCollectors->size()), 0);
				for (size_t index = 0; index < waterCollectors->size(); ++index)
				{
					auto const& collector = (*waterCollectors)[index];
					lua_createtable(state, 0, 8);
					lua_pushstring(state, collector.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, collector.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					lua_pushstring(state, collector.coolingLoopId.c_str()); lua_setfield(state, -2, "coolingLoop");
					lua_pushstring(state, collector.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, collector.destStoreId.c_str()); lua_setfield(state, -2, "destStore");
					lua_pushnumber(state, collector.maxRateKgPerS); lua_setfield(state, -2, "maxRateKgPerS");
					lua_pushnumber(state, collector.targetRelativeHumidity); lua_setfield(state, -2, "targetRelativeHumidity");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_createtable(state, static_cast<int>(waterTreatments->size()), 0);
				for (size_t index = 0; index < waterTreatments->size(); ++index)
				{
					auto const& treatment = (*waterTreatments)[index];
					lua_createtable(state, 0, 12);
					lua_pushstring(state, treatment.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, treatment.sourceStoreId.c_str()); lua_setfield(state, -2, "sourceStore");
					lua_pushstring(state, treatment.treatedStoreId.c_str()); lua_setfield(state, -2, "treatedStore");
					lua_pushstring(state, treatment.concentrateStoreId.c_str()); lua_setfield(state, -2, "concentrateStore");
					lua_pushstring(state, treatment.consumableStoreId.c_str()); lua_setfield(state, -2, "consumableStore");
					lua_pushnumber(state, treatment.throughputKgPerS); lua_setfield(state, -2, "throughputKgPerS");
					lua_pushnumber(state, treatment.recoveryRatio); lua_setfield(state, -2, "recoveryRatio");
					lua_createtable(state, 0, static_cast<int>(treatment.removalFractions.size()));
					for (auto const& [name, fraction] : treatment.removalFractions)
					{ lua_pushnumber(state, fraction); lua_setfield(state, -2, name.c_str()); }
					lua_setfield(state, -2, "removalFractions");
					lua_pushnumber(state, treatment.specificEnergyKWhPerKg); lua_setfield(state, -2, "specificEnergyKWhPerKg");
					lua_pushnumber(state, treatment.consumablePerKg); lua_setfield(state, -2, "consumablePerKg");
					lua_pushinteger(state, treatment.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored oxygen production and CO2 capture contracts cross
				// the same boundary as plain immutable descriptors (#404); the
				// sandbox mirrors the stoichiometric split, the energy and heat
				// booking and the finite capture rule, and re-audits its own
				// accounting.
				lua_createtable(state, static_cast<int>(electrolyzers->size()), 0);
				for (size_t index = 0; index < electrolyzers->size(); ++index)
				{
					auto const& electrolyzer = (*electrolyzers)[index];
					lua_createtable(state, 0, 12);
					lua_pushstring(state, electrolyzer.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, electrolyzer.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, electrolyzer.oxygenStoreId.c_str()); lua_setfield(state, -2, "oxygenStore");
					lua_pushstring(state, electrolyzer.hydrogenStoreId.c_str()); lua_setfield(state, -2, "hydrogenStore");
					lua_pushstring(state, electrolyzer.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					lua_pushstring(state, electrolyzer.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, electrolyzer.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, electrolyzer.rateKgPerS); lua_setfield(state, -2, "rateKgPerS");
					lua_pushnumber(state, electrolyzer.specificEnergyKWhPerKgWater); lua_setfield(state, -2, "specificEnergyKWhPerKgWater");
					lua_pushnumber(state, electrolyzer.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					lua_pushinteger(state, electrolyzer.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_createtable(state, static_cast<int>(co2Captures->size()), 0);
				for (size_t index = 0; index < co2Captures->size(); ++index)
				{
					auto const& capture = (*co2Captures)[index];
					lua_createtable(state, 0, 9);
					lua_pushstring(state, capture.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, capture.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					lua_pushstring(state, capture.captureStoreId.c_str()); lua_setfield(state, -2, "captureStore");
					lua_pushstring(state, capture.consumableStoreId.c_str()); lua_setfield(state, -2, "consumableStore");
					lua_pushnumber(state, capture.maxRateKgPerS); lua_setfield(state, -2, "maxRateKgPerS");
					lua_pushnumber(state, capture.specificEnergyKWhPerKg); lua_setfield(state, -2, "specificEnergyKWhPerKg");
					lua_pushnumber(state, capture.consumablePerKg); lua_setfield(state, -2, "consumablePerKg");
					lua_pushinteger(state, capture.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored fuel-cell backup descriptors cross the same boundary
				// as plain immutable values (#406); each sandbox mirrors the
				// reactant ledger and audits its own mass and energy closure.
				lua_createtable(state, static_cast<int>(fuelCells->size()), 0);
				for (size_t index = 0; index < fuelCells->size(); ++index)
				{
					auto const& cell = (*fuelCells)[index];
					lua_createtable(state, 0, 10);
					lua_pushstring(state, cell.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, cell.hydrogenStoreId.c_str()); lua_setfield(state, -2, "hydrogenStore");
					lua_pushstring(state, cell.oxygenStoreId.c_str()); lua_setfield(state, -2, "oxygenStore");
					lua_pushstring(state, cell.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, cell.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, cell.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, cell.ratedOutputKW); lua_setfield(state, -2, "ratedOutputKW");
					lua_pushnumber(state, cell.efficiency); lua_setfield(state, -2, "efficiency");
					lua_pushnumber(state, cell.commandedOutputKW); lua_setfield(state, -2, "commandedOutputKW");
					lua_pushnumber(state, cell.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored thermal buffer descriptors cross the same boundary
				// as plain immutable values (#398); each sandbox mirrors the
				// finite store, the access gating and audits its own storage
				// ledger.
				lua_createtable(state, static_cast<int>(thermalBuffers->size()), 0);
				for (size_t index = 0; index < thermalBuffers->size(); ++index)
				{
					auto const& buffer = (*thermalBuffers)[index];
					lua_createtable(state, 0, 11);
					lua_pushstring(state, buffer.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, buffer.reservoirId.c_str()); lua_setfield(state, -2, "reservoir");
					lua_pushstring(state, buffer.accessMode.c_str()); lua_setfield(state, -2, "accessMode");
					lua_pushstring(state, buffer.circulationLoopId.c_str()); lua_setfield(state, -2, "circulationLoop");
					lua_pushnumber(state, buffer.capacityKJ); lua_setfield(state, -2, "capacityKJ");
					lua_pushnumber(state, buffer.startingEnergyKJ); lua_setfield(state, -2, "startingEnergyKJ");
					lua_pushnumber(state, buffer.minUsableTemperatureC); lua_setfield(state, -2, "minUsableTemperatureC");
					lua_pushnumber(state, buffer.maxUsableTemperatureC); lua_setfield(state, -2, "maxUsableTemperatureC");
					lua_pushnumber(state, buffer.maxChargeKW); lua_setfield(state, -2, "maxChargeKW");
					lua_pushnumber(state, buffer.maxDischargeKW); lua_setfield(state, -2, "maxDischargeKW");
					lua_pushnumber(state, buffer.transferConductanceKWPerK); lua_setfield(state, -2, "transferConductanceKWPerK");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored terminal-waste ejection descriptors cross the same
				// boundary as plain immutable values (#403); each sandbox
				// mirrors the eligible-only removal and audits its own loss
				// booking.
				lua_createtable(state, static_cast<int>(wasteEjections->size()), 0);
				for (size_t index = 0; index < wasteEjections->size(); ++index)
				{
					auto const& ejection = (*wasteEjections)[index];
					lua_createtable(state, 0, 7);
					lua_pushstring(state, ejection.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, ejection.eligibility == WasteEjectionEligibility::HeldOnly
						? "heldOnly" : ejection.eligibility == WasteEjectionEligibility::QualifiedAndHeld
						? "qualifiedAndHeld" : "qualifiedOnly"); lua_setfield(state, -2, "eligibility");
					lua_pushstring(state, ejection.storeId.c_str()); lua_setfield(state, -2, "store");
					lua_pushstring(state, ejection.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, ejection.quantity); lua_setfield(state, -2, "quantity");
					lua_pushnumber(state, ejection.firstOccurrenceSeconds); lua_setfield(state, -2, "firstSeconds");
					lua_pushnumber(state, ejection.recurrenceSeconds); lua_setfield(state, -2, "recurrenceSeconds");
					lua_pushinteger(state, ejection.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored Sabatier reactor descriptors cross the same boundary
				// as plain immutable values (#405); each sandbox mirrors the
				// stoichiometric conversion and audits its own mass and energy
				// closure.
				lua_createtable(state, static_cast<int>(sabatierReactors->size()), 0);
				for (size_t index = 0; index < sabatierReactors->size(); ++index)
				{
					auto const& reactor = (*sabatierReactors)[index];
					lua_createtable(state, 0, 11);
					lua_pushstring(state, reactor.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, reactor.co2StoreId.c_str()); lua_setfield(state, -2, "co2Store");
					lua_pushstring(state, reactor.hydrogenStoreId.c_str()); lua_setfield(state, -2, "hydrogenStore");
					lua_pushstring(state, reactor.methaneStoreId.c_str()); lua_setfield(state, -2, "methaneStore");
					lua_pushstring(state, reactor.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, reactor.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, reactor.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, reactor.rateKgPerS); lua_setfield(state, -2, "rateKgPerS");
					lua_pushnumber(state, reactor.specificEnergyKWhPerKgCO2); lua_setfield(state, -2, "specificEnergyKWhPerKgCO2");
					lua_pushnumber(state, reactor.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					lua_pushinteger(state, reactor.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored methane processor descriptors cross the same
				// boundary as plain immutable values (#405); each sandbox
				// mirrors the stoichiometric split and audits its own mass and
				// energy closure.
				lua_createtable(state, static_cast<int>(methaneProcessors->size()), 0);
				for (size_t index = 0; index < methaneProcessors->size(); ++index)
				{
					auto const& processor = (*methaneProcessors)[index];
					lua_createtable(state, 0, 10);
					lua_pushstring(state, processor.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, processor.methaneStoreId.c_str()); lua_setfield(state, -2, "methaneStore");
					lua_pushstring(state, processor.hydrogenStoreId.c_str()); lua_setfield(state, -2, "hydrogenStore");
					lua_pushstring(state, processor.carbonStoreId.c_str()); lua_setfield(state, -2, "carbonStore");
					lua_pushstring(state, processor.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, processor.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, processor.rateKgPerS); lua_setfield(state, -2, "rateKgPerS");
					lua_pushnumber(state, processor.specificEnergyKWhPerKgCH4); lua_setfield(state, -2, "specificEnergyKWhPerKgCH4");
					lua_pushnumber(state, processor.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					lua_pushinteger(state, processor.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored microbial processor descriptors cross the same
				// boundary as plain immutable values (#407); each sandbox
				// mirrors the constituent-funded recovery, the balance-derived
				// residue and audits its own mass and energy closure.
				lua_createtable(state, static_cast<int>(microbialProcessors->size()), 0);
				for (size_t index = 0; index < microbialProcessors->size(); ++index)
				{
					auto const& processor = (*microbialProcessors)[index];
					lua_createtable(state, 0, 35);
					lua_pushstring(state, processor.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, stationRateBasisName(processor.rateBasis)); lua_setfield(state, -2, "rateBasis");
					lua_pushnumber(state, processor.organicOxidationFraction); lua_setfield(state, -2, "organicOxidationFraction");
					lua_pushnumber(state, processor.o2PerKgOrganicOxidized); lua_setfield(state, -2, "o2PerKgOrganicOxidized");
					lua_pushnumber(state, processor.nitrificationFraction); lua_setfield(state, -2, "nitrificationFraction");
					lua_pushnumber(state, processor.o2PerKgNitrifiedN); lua_setfield(state, -2, "o2PerKgNitrifiedN");
					lua_pushnumber(state, processor.co2PerKgOrganicOxidized); lua_setfield(state, -2, "co2PerKgOrganicOxidized");
					lua_pushnumber(state, processor.processWaterTargetMoisture); lua_setfield(state, -2, "processWaterTargetMoisture");
					lua_pushnumber(state, processor.heatReleaseFraction); lua_setfield(state, -2, "heatReleaseFraction");
					lua_pushnumber(state, processor.consumablePerKgOrganicOxidized); lua_setfield(state, -2, "consumablePerKgOrganicOxidized");
					lua_pushstring(state, processor.feedStoreId.c_str()); lua_setfield(state, -2, "feedStore");
					lua_pushstring(state, processor.oxygenStoreId.c_str()); lua_setfield(state, -2, "oxygenStore");
					pushWaterQuality(state, processor.waterQuality);
					lua_pushstring(state, processor.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, processor.consumableStoreId.c_str()); lua_setfield(state, -2, "consumableStore");
					lua_pushstring(state, processor.nutrientStoreId.c_str()); lua_setfield(state, -2, "nutrientStore");
					lua_createtable(state, 0, static_cast<int>(processor.recoveredWaterCarryoverFractions.size()));
					for (auto const& [name, fraction] : processor.recoveredWaterCarryoverFractions)
					{ lua_pushnumber(state, fraction); lua_setfield(state, -2, name.c_str()); }
					lua_setfield(state, -2, "recoveredWaterCarryoverFractions");
					lua_pushstring(state, processor.recoveredWaterStoreId.c_str()); lua_setfield(state, -2, "recoveredWaterStore");
					lua_pushstring(state, processor.co2StoreId.c_str()); lua_setfield(state, -2, "co2Store");
					lua_pushstring(state, processor.residueStoreId.c_str()); lua_setfield(state, -2, "residueStore");
					lua_pushstring(state, processor.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, processor.heatSinkId.c_str()); lua_setfield(state, -2, "heatSink");
					lua_pushnumber(state, processor.rateKgPerS); lua_setfield(state, -2, "rateKgPerS");
					lua_pushnumber(state, processor.specificEnergyKWhPerKgFeed); lua_setfield(state, -2, "specificEnergyKWhPerKgFeed");
					lua_pushnumber(state, processor.oxygenPerKgFeedKg); lua_setfield(state, -2, "oxygenPerKgFeedKg");
					lua_pushnumber(state, processor.processWaterPerKgFeedKg); lua_setfield(state, -2, "processWaterPerKgFeedKg");
					lua_pushnumber(state, processor.consumablePerKgFeed); lua_setfield(state, -2, "consumablePerKgFeed");
					lua_pushnumber(state, processor.co2PerKgFeedKg); lua_setfield(state, -2, "co2PerKgFeedKg");
					lua_pushnumber(state, processor.nitrogenRecovery); lua_setfield(state, -2, "nitrogenRecovery");
					lua_pushnumber(state, processor.phosphorusRecovery); lua_setfield(state, -2, "phosphorusRecovery");
					lua_pushnumber(state, processor.potassiumRecovery); lua_setfield(state, -2, "potassiumRecovery");
					lua_pushnumber(state, processor.mineralsRecovery); lua_setfield(state, -2, "mineralsRecovery");
					lua_pushnumber(state, processor.waterRecovery); lua_setfield(state, -2, "waterRecovery");
					lua_pushnumber(state, processor.reactionHeatKJPerKgFeed); lua_setfield(state, -2, "reactionHeatKJPerKgFeed");
					lua_pushnumber(state, processor.minOperatingTemperatureC); lua_setfield(state, -2, "minOperatingTemperatureC");
					lua_pushnumber(state, processor.maxSinkTemperatureC); lua_setfield(state, -2, "maxSinkTemperatureC");
					// #408 condition/recovery coefficients cross as plain
					// values; the sandbox mirrors the degradation rules and the
					// bounded, stock-funded recovery.
					lua_pushnumber(state, processor.initialCondition); lua_setfield(state, -2, "initialCondition");
					lua_pushnumber(state, processor.starvationDegradePerHour); lua_setfield(state, -2, "starvationDegradePerHour");
					lua_pushnumber(state, processor.consumableExhaustionDegradePerHour); lua_setfield(state, -2, "consumableExhaustionDegradePerHour");
					lua_pushnumber(state, processor.coldStressDegradePerHour); lua_setfield(state, -2, "coldStressDegradePerHour");
					lua_pushnumber(state, processor.heatStressDegradePerHour); lua_setfield(state, -2, "heatStressDegradePerHour");
					lua_pushnumber(state, processor.recoveryPerHour); lua_setfield(state, -2, "recoveryPerHour");
					lua_pushstring(state, processor.stockStoreId.c_str()); lua_setfield(state, -2, "stockStore");
					lua_pushnumber(state, processor.stockUnitsPerConditionPoint); lua_setfield(state, -2, "stockUnitsPerConditionPoint");
					lua_pushinteger(state, processor.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored crop population descriptors cross the same
				// boundary as plain immutable values (#409); each sandbox
				// mirrors the resource-limited growth, the day-and-night
				// respiration and audits its own mass and energy closure.
				lua_createtable(state, static_cast<int>(cropPopulations->size()), 0);
				for (size_t index = 0; index < cropPopulations->size(); ++index)
				{
					auto const& crop = (*cropPopulations)[index];
					lua_createtable(state, 0, 20);
					lua_pushstring(state, crop.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, crop.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					pushWaterQuality(state, crop.waterQuality);
					lua_createtable(state, 0, static_cast<int>(crop.irrigationUptakeFractions.size()));
					for (auto const& [name, fraction] : crop.irrigationUptakeFractions)
					{ lua_pushnumber(state, fraction); lua_setfield(state, -2, name.c_str()); }
					lua_setfield(state, -2, "irrigationUptakeFractions");
					lua_pushstring(state, crop.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, crop.nitrogenStoreId.c_str()); lua_setfield(state, -2, "nitrogenStore");
					lua_pushstring(state, crop.phosphorusStoreId.c_str()); lua_setfield(state, -2, "phosphorusStore");
					lua_pushstring(state, crop.potassiumStoreId.c_str()); lua_setfield(state, -2, "potassiumStore");
					lua_pushstring(state, crop.mineralsStoreId.c_str()); lua_setfield(state, -2, "mineralsStore");
					lua_createtable(state, 0, 7);
					lua_pushnumber(state, crop.composition.moisture); lua_setfield(state, -2, "moisture");
					lua_pushnumber(state, crop.composition.nutrientNitrogen); lua_setfield(state, -2, "nutrientNitrogen");
					lua_pushnumber(state, crop.composition.atmosphericNitrogen); lua_setfield(state, -2, "atmosphericNitrogen");
					lua_pushnumber(state, crop.composition.phosphorus); lua_setfield(state, -2, "phosphorus");
					lua_pushnumber(state, crop.composition.potassium); lua_setfield(state, -2, "potassium");
					lua_pushnumber(state, crop.composition.otherMinerals); lua_setfield(state, -2, "otherMinerals");
					lua_pushnumber(state, crop.composition.energyKJPerKgDry); lua_setfield(state, -2, "energyKJPerKgDry");
					lua_setfield(state, -2, "composition");
					lua_pushnumber(state, crop.growingAreaM2); lua_setfield(state, -2, "growingAreaM2");
					lua_pushnumber(state, crop.maxBiomassDensityKgPerM2); lua_setfield(state, -2, "maxBiomassDensityKgPerM2");
					lua_pushnumber(state, crop.initialBiomassKg); lua_setfield(state, -2, "initialBiomassKg");
					lua_pushnumber(state, crop.lampRatedKW); lua_setfield(state, -2, "lampRatedKW");
					pushProfile(state, crop.illuminationProfile);
					lua_setfield(state, -2, "illumination");
					lua_pushnumber(state, crop.circulationPowerKW); lua_setfield(state, -2, "circulationPowerKW");
					lua_pushnumber(state, crop.growthYieldKgPerKWh); lua_setfield(state, -2, "growthYieldKgPerKWh");
					lua_pushnumber(state, crop.respirationPerHour); lua_setfield(state, -2, "respirationPerHour");
					lua_pushnumber(state, crop.transpirationKgPerKgGrowth); lua_setfield(state, -2, "transpirationKgPerKgGrowth");
					lua_pushnumber(state, crop.minGrowingTemperatureC); lua_setfield(state, -2, "minGrowingTemperatureC");
					lua_pushnumber(state, crop.maxGrowingTemperatureC); lua_setfield(state, -2, "maxGrowingTemperatureC");
					lua_pushstring(state, crop.foodStoreId.c_str()); lua_setfield(state, -2, "foodStore");
					lua_pushstring(state, crop.wasteStoreId.c_str()); lua_setfield(state, -2, "wasteStore");
					lua_pushnumber(state, crop.edibleFraction); lua_setfield(state, -2, "edibleFraction");
					lua_pushnumber(state, crop.harvestTriggerKg); lua_setfield(state, -2, "harvestTriggerKg");
					lua_pushnumber(state, crop.harvestFraction); lua_setfield(state, -2, "harvestFraction");
					lua_pushinteger(state, crop.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored algae culture descriptors (#411) cross the same
				// boundary as plain immutable values; the sandbox mirrors the
				// culture lifecycle and audits it itself. The culture keeps its
				// own volume, density, coefficients and destinations - nothing
				// here is shared with the crop declarations.
				lua_createtable(state, static_cast<int>(algaeCultures->size()), 0);
				for (size_t index = 0; index < algaeCultures->size(); ++index)
				{
					auto const& algae = (*algaeCultures)[index];
					lua_createtable(state, 0, 2);
					lua_pushstring(state, algae.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, algae.atmosphereId.c_str()); lua_setfield(state, -2, "atmosphere");
					pushWaterQuality(state, algae.waterQuality);
					lua_pushstring(state, algae.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, algae.nitrogenStoreId.c_str()); lua_setfield(state, -2, "nitrogenStore");
					lua_pushstring(state, algae.phosphorusStoreId.c_str()); lua_setfield(state, -2, "phosphorusStore");
					lua_pushstring(state, algae.potassiumStoreId.c_str()); lua_setfield(state, -2, "potassiumStore");
					lua_pushstring(state, algae.mineralsStoreId.c_str()); lua_setfield(state, -2, "mineralsStore");
					lua_createtable(state, 0, 7);
					lua_pushnumber(state, algae.composition.moisture); lua_setfield(state, -2, "moisture");
					lua_pushnumber(state, algae.composition.nutrientNitrogen); lua_setfield(state, -2, "nutrientNitrogen");
					lua_pushnumber(state, algae.composition.atmosphericNitrogen); lua_setfield(state, -2, "atmosphericNitrogen");
					lua_pushnumber(state, algae.composition.phosphorus); lua_setfield(state, -2, "phosphorus");
					lua_pushnumber(state, algae.composition.potassium); lua_setfield(state, -2, "potassium");
					lua_pushnumber(state, algae.composition.otherMinerals); lua_setfield(state, -2, "otherMinerals");
					lua_pushnumber(state, algae.composition.energyKJPerKgDry); lua_setfield(state, -2, "energyKJPerKgDry");
					lua_setfield(state, -2, "composition");
					lua_pushnumber(state, algae.cultureVolumeL); lua_setfield(state, -2, "cultureVolumeL");
					lua_pushnumber(state, algae.maxBiomassDensityKgPerL); lua_setfield(state, -2, "maxBiomassDensityKgPerL");
					lua_pushnumber(state, algae.initialBiomassKg); lua_setfield(state, -2, "initialBiomassKg");
					lua_pushnumber(state, algae.lampRatedKW); lua_setfield(state, -2, "lampRatedKW");
					pushProfile(state, algae.illuminationProfile);
					lua_setfield(state, -2, "illumination");
					lua_pushnumber(state, algae.circulationPowerKW); lua_setfield(state, -2, "circulationPowerKW");
					lua_pushnumber(state, algae.growthYieldKgPerKWh); lua_setfield(state, -2, "growthYieldKgPerKWh");
					lua_pushnumber(state, algae.respirationPerHour); lua_setfield(state, -2, "respirationPerHour");
					lua_pushnumber(state, algae.evaporationKgPerKgGrowth); lua_setfield(state, -2, "evaporationKgPerKgGrowth");
					lua_pushnumber(state, algae.minGrowingTemperatureC); lua_setfield(state, -2, "minGrowingTemperatureC");
					lua_pushnumber(state, algae.maxGrowingTemperatureC); lua_setfield(state, -2, "maxGrowingTemperatureC");
					lua_pushstring(state, algae.foodStoreId.c_str()); lua_setfield(state, -2, "foodStore");
					lua_pushstring(state, algae.recoveryStoreId.c_str()); lua_setfield(state, -2, "recoveryStore");
					lua_pushstring(state, algae.wasteStoreId.c_str()); lua_setfield(state, -2, "wasteStore");
					lua_pushnumber(state, algae.foodFraction); lua_setfield(state, -2, "foodFraction");
					lua_pushnumber(state, algae.recoveryFraction); lua_setfield(state, -2, "recoveryFraction");
					lua_pushnumber(state, algae.harvestTriggerKg); lua_setfield(state, -2, "harvestTriggerKg");
					lua_pushnumber(state, algae.harvestFraction); lua_setfield(state, -2, "harvestFraction");
					lua_pushinteger(state, algae.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// Authored installation outage schedules cross the same
				// boundary as plain immutable descriptors; the sandbox mirrors
				// the same availability rule and audits it (#413).
				lua_createtable(state, static_cast<int>(outages->size()), 0);
				for (size_t index = 0; index < outages->size(); ++index)
				{
					auto const& outage = (*outages)[index];
					lua_createtable(state, 0, 7);
					lua_pushstring(state, outage.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, outage.installationId.c_str()); lua_setfield(state, -2, "installation");
					lua_pushnumber(state, outage.capacityFraction); lua_setfield(state, -2, "capacityFraction");
					lua_pushnumber(state, outage.firstOccurrenceSeconds); lua_setfield(state, -2, "firstSeconds");
					lua_pushnumber(state, outage.durationSeconds); lua_setfield(state, -2, "durationSeconds");
					lua_pushnumber(state, outage.recurrenceSeconds); lua_setfield(state, -2, "recurrenceSeconds");
					lua_pushinteger(state, outage.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				// The dispatch controller mode (#415) crosses as a plain
				// immutable string; the sandbox mirrors the same controller
				// and refuses any other value.
				lua_pushstring(state, dispatchMode);
				// Authored maintenance job schedules cross the same boundary
				// as plain immutable descriptors; the sandbox mirrors the
				// same reservation, isolation and spend rules (#414).
				lua_createtable(state, maintenanceJobs ? static_cast<int>(maintenanceJobs->size()) : 0, 0);
				for (size_t index = 0; maintenanceJobs
					&& index < maintenanceJobs->size(); ++index)
				{
					auto const& job = (*maintenanceJobs)[index];
					lua_createtable(state, 0, 7);
					lua_pushstring(state, job.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, job.installationId.c_str()); lua_setfield(state, -2, "installation");
					lua_pushnumber(state, job.firstOccurrenceSeconds); lua_setfield(state, -2, "firstSeconds");
					lua_pushnumber(state, job.durationSeconds); lua_setfield(state, -2, "durationSeconds");
					lua_pushnumber(state, job.recurrenceSeconds); lua_setfield(state, -2, "recurrenceSeconds");
					lua_pushinteger(state, job.priority); lua_setfield(state, -2, "priority");
					lua_pushstring(state, job.wasteStoreId.c_str()); lua_setfield(state, -2, "wasteStore");
					lua_createtable(state, static_cast<int>(job.requirements.size()), 0);
					for (size_t requirementIndex = 0;
						requirementIndex < job.requirements.size(); ++requirementIndex)
					{
						auto const& requirement = job.requirements[requirementIndex];
						lua_createtable(state, 0, 3);
						lua_pushstring(state, requirement.storeId.c_str()); lua_setfield(state, -2, "store");
						lua_pushnumber(state, requirement.quantity); lua_setfield(state, -2, "quantity");
						lua_pushboolean(state, requirement.consumed ? 1 : 0); lua_setfield(state, -2, "consumed");
						lua_rawseti(state, -2, static_cast<int>(requirementIndex) + 1);
					}
					lua_setfield(state, -2, "requirements");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_call(state, 31, 1);
				if (!lua_istable(state, -1)) return luaL_error(state, "Station factory must return an object");
				object->reference = luaL_ref(state, LUA_REGISTRYINDEX);
				return 0;
			}
		};
		struct Step
		{
			ScriptObject* object;
			uint64_t tick;
			double seconds;
			uint64_t completed{};
			bool invalidOutput{false};
			int operator()(lua_State* state)
			{
				lua_rawgeti(state, LUA_REGISTRYINDEX, object->reference);
				lua_getfield(state, -1, "advance"); lua_pushvalue(state, -2);
				lua_pushinteger(state, static_cast<lua_Integer>(tick));
				lua_pushnumber(state, seconds); lua_call(state, 3, 1);
				if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) < 0)
				{
					invalidOutput = true;
					if (lua_type(state, -1) == LUA_TNUMBER && !std::isfinite(lua_tonumber(state, -1)))
						return luaL_error(state, "Station lifecycle returned non-finite completed ticks");
					return luaL_error(state, "Station lifecycle returned invalid completed ticks");
				}
				completed = static_cast<uint64_t>(lua_tointeger(state, -1));
				return 0;
			}
		};
}
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader) { sourceLoader = std::move(loader); }
	std::string bundledStationSystemsSource() { return std::string(kStationSystemsLua); }
	// #418/#603: the public seam over the bounded trend-history
	// primitive. The World runtime records through the same code from
	// the committed snapshot at the observation cadence; the seam lets
	// regression coverage exercise many successive coarsenings, the
	// level cap and retention expiry directly - the World-driven
	// bounds (retention <= 86400 s, observation >= 0.1 s) stop the
	// level long before the cap in any feasible run.
	void appendBoundedTrendSample(std::vector<StationTrendSeries>& series,
		std::string const& subsystem, std::string const& id,
		std::string const& unit, double baseIntervalSeconds,
		double retentionSeconds,
		uint64_t tick, double elapsedSeconds, double requested, double actual)
	{
		appendBoundedTrendSampleImpl(series, subsystem, id, unit,
			baseIntervalSeconds, retentionSeconds,
			tick, elapsedSeconds, requested, actual);
	}
	std::optional<uint32_t> stationSystemsRuntimeStepRatioOverrideForTesting()
	{
		return stationSystemsRuntimeStepRatioOverride;
	}

	void setStationSystemsRuntimeStepRatioOverrideForTesting(std::optional<uint32_t> ratio)
	{
		stationSystemsRuntimeStepRatioOverride = ratio;
	}

	StationSystemsRuntime::StationSystemsRuntime(StationSystemsSettings settings, float baseStepSeconds)
	{
		static std::atomic<uint64_t> nextRun{0};
		mRun = ++nextRun;
		if (stationSystemsRuntimeStepRatioOverride) settings.systemsStepWorldTicks = *stationSystemsRuntimeStepRatioOverride;
		std::string diagnostic;
		if (!validateStationSystemsSettings(settings, diagnostic)) throw SerializationException(diagnostic);
		mImpl = std::make_unique<Impl>(settings.systemsStepWorldTicks, baseStepSeconds);
		mImpl->settings = settings; mImpl->snapshot.enabled = settings.enabled;
		mImpl->snapshot.systemsStepWorldTicks = settings.systemsStepWorldTicks;
		mImpl->snapshot.systemsTimestepSeconds = mImpl->clock.stepSeconds();
		std::vector<StationRecordingSlot> slots;
		auto addSlot = [&](std::string subsystem, std::string id, std::string unit, std::string metricId) {
			StationRecordingSlot slot;
			slot.subsystem = std::move(subsystem); slot.id = std::move(id); slot.unit = std::move(unit);
			slot.metric.id = std::move(metricId);
			slot.metric.instanceId = slot.id;
			slot.metric.requestedSemantics = StationRequestedSemantics::Demand;
			slots.push_back(std::move(slot));
		};
		addSlot("power", "", "kW", "station.power.service");
		slots.back().metric.role = StationMetricRole::Input;
		addSlot("power-generation", "", "kW", "station.power.generation");
		slots.back().metric.role = StationMetricRole::Generation;
		slots.back().metric.requestedSemantics.reset();
		for (auto const& load : settings.equipmentLoads)
		{
			addSlot("equipment-load", load.id, "kW", "equipment-load/" + load.id);
			auto& d = slots.back().metric;
			d.equipmentType = "External equipment load group"; d.exchangeRole = "Electrical service";
			d.role = StationMetricRole::Input;
		}
		for (auto const& transfer : settings.transfers)
		{
			auto const source = std::ranges::find(settings.stores, transfer.sourceId, &ResourceStoreSettings::id);
			addSlot("transfer", transfer.id, source->unit, "transfer/" + transfer.id);
			// Legacy transfer observations are per-observation quantities, not power.
			auto& d = slots.back().metric;
			d.quantity = StationPhysicalQuantity::Unspecified; d.unit = StationCanonicalUnit::Unspecified;
			if (source->unit == "kg") { d.quantity = StationPhysicalQuantity::Mass; d.unit = StationCanonicalUnit::Kilogram; }
			if (source->unit == "m3") { d.quantity = StationPhysicalQuantity::Volume; d.unit = StationCanonicalUnit::CubicMetre; }
			if (source->unit == "kWh") { d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour; }
		}
		for (auto const& battery : settings.batteries)
			for (bool capacity : {false, true})
			{
				StationRecordingSlot slot;
				auto& d = slot.metric;
				d.id = "battery/" + battery.id + (capacity ? "/capacity" : "/stored-energy");
				d.equipmentType = "Battery"; d.instanceId = battery.id;
				d.exchangeRole = capacity ? "capacity" : "stored-energy";
				d.role = capacity ? StationMetricRole::Capacity : StationMetricRole::Inventory;
				d.quantity = StationPhysicalQuantity::Energy; d.unit = StationCanonicalUnit::KilowattHour;
				slots.push_back(std::move(slot));
			}
		for (auto& observation : stationStoreObservations(settings))
		{
			mImpl->storeObservationScales.push_back(observation.sourceToCanonicalScale);
			StationRecordingSlot slot; slot.metric = std::move(observation.metric);
			slots.push_back(slot);
			slot.metric.id += "/usable";
			slot.metric.exchangeRole = "usable-inventory";
			slots.push_back(std::move(slot));
		}
		// Retained ownership only: neither circulation nor processing throughput
		// is additional stock. Water vapour is water mass, not a second material.
		auto retained = [&](std::string owner, std::string type, std::string id,
			std::string material) {
			StationRecordingSlot slot;
			auto& d = slot.metric;
			d.id = owner + "/" + id + "/" + material + "/inventory";
			d.equipmentType = std::move(type); d.instanceId = std::move(id);
			d.materialId = std::move(material);
			d.exchangeRole = "retained-inventory"; d.role = StationMetricRole::Inventory;
			d.quantity = StationPhysicalQuantity::Mass; d.unit = StationCanonicalUnit::Kilogram;
			slots.push_back(std::move(slot));
		};
		for (auto const& a : settings.atmospheres)
			for (auto material : {"oxygen", "nitrogen", "carbon-dioxide", "water"})
				retained("atmosphere", "Atmosphere compartment", a.id, material);
		for (auto const& loop : settings.coolantLoops)
			retained("coolant-loop", "Coolant loop", loop.id, "coolant");
		for (auto const& store : settings.stores) if (store.composition)
			for (auto const* key : {StationMassConstituents[0], StationMassConstituents[1],
				StationMassConstituents[2], StationMassConstituents[3], StationMassConstituents[4],
				StationMassConstituents[5], StationMassConstituents[6], StationEnergyConstituent})
			{
				StationRecordingSlot slot;
				auto& d = slot.metric;
				d.id = "store/" + store.id + "/constituent/" + key;
				d.equipmentType = "Resource store"; d.instanceId = store.id;
				d.constituentId = key;
				d.exchangeRole = "stored-constituent"; d.role = StationMetricRole::Inventory;
				bool const energy = key == StationEnergyConstituent;
				d.quantity = energy ? StationPhysicalQuantity::Energy : StationPhysicalQuantity::Mass;
				d.unit = energy ? StationCanonicalUnit::KilowattHour : StationCanonicalUnit::Kilogram;
				slots.push_back(slot);
				d.id += "/usable"; d.exchangeRole = "usable-constituent";
				slots.push_back(std::move(slot));
			}
		// Only crops and algae have independent living inventory ledgers.
		// Culture volume is Growing capacity, not an inventory of water.
		auto living = [&](auto const& populations, std::string const& owner, std::string const& type) {
			for (auto const& p : populations)
			{
				retained(owner, type, p.id, "living-biomass");
				for (auto const* key : {"dryOrganics", "nutrientNitrogen", "phosphorus", "potassium", "otherMinerals", "energy"})
				{
					StationRecordingSlot slot;
					auto& d = slot.metric;
					d.id = owner + "/" + p.id + "/constituent/" + key;
					d.equipmentType = type; d.instanceId = p.id;
					d.constituentId = key;
					d.exchangeRole = "retained-constituent"; d.role = StationMetricRole::Inventory;
					bool const energy = std::string_view(key) == "energy";
					d.quantity = energy ? StationPhysicalQuantity::Energy : StationPhysicalQuantity::Mass;
					d.unit = energy ? StationCanonicalUnit::KilowattHour : StationCanonicalUnit::Kilogram;
					slots.push_back(std::move(slot));
				}
			}
		};
		living(settings.cropPopulations, "crop-population", "Crop population");
		living(settings.algaeCultures, "algae-culture", "Algae culture");
		mImpl->exchangeObservations = stationExchangeObservations(settings);
		// Committed cumulative event ledgers start at zero in each applied run.
		mImpl->previousExchangeTotals.assign(mImpl->exchangeObservations.size(), 0.0);
		mImpl->previousExchangeSeconds = 0;
		for (auto const& o : mImpl->exchangeObservations)
		{
			StationRecordingSlot slot; slot.metric = o.metric;
			slots.push_back(std::move(slot));
		}
		auto const slotCount = slots.size();
		auto const stepSeconds = mImpl->clock.stepSeconds();
		// Share one integer observation stride between sampling and metadata.
		// Float cadence products diverge from the count-derived timestamps and
		// the discrepancy grows when the recorder coarsens its history.
		mImpl->observationIntervalSteps = std::max(uint64_t{1},
			static_cast<uint64_t>(std::ceil(settings.observationIntervalSeconds / stepSeconds)));
		mImpl->history = StationHistory(std::move(slots),
			static_cast<double>(mImpl->observationIntervalSteps) * stepSeconds,
			settings.trendRetentionSeconds, {}, mImpl->clock.stepSeconds());
		if (!mImpl->history.status().limited) mImpl->recordingValues.resize(slotCount);
		else
		{
			// Refusal must not retain a second, over-budget adapter catalogue.
			std::vector<StationExchangeObservation>{}.swap(mImpl->exchangeObservations);
			std::vector<std::optional<double>>{}.swap(mImpl->previousExchangeTotals);
			std::vector<double>{}.swap(mImpl->storeObservationScales);
		}
		mImpl->connectionGraph = station_connections::build(settings);
		for (auto const& store : settings.stores)
		{
			mImpl->stores.push_back({store, store.startingQualified, store.startingUnqualified,
				0.0, 0.0, {}});
			mImpl->startingTotals[store.unit] += store.startingQualified + store.startingUnqualified;
			// #419: the endurance window starts anchored at the authored t = 0
			// drawable, so the first observation window measures from the
			// authored beginning. Runtime-only: Reset/load reconstruct.
			Impl::EnduranceWindow enduranceWindow;
			enduranceWindow.windowStartDrawable = std::max(0.0,
				store.startingQualified - store.protectedReserve);
			mImpl->storeEndurance.push_back(enduranceWindow);
		}
		std::map<std::string, size_t> storeIndex;
		for (size_t index = 0; index < mImpl->stores.size(); ++index)
			storeIndex[mImpl->stores[index].authored.id] = index;
		for (auto const& transfer : settings.transfers)
			mImpl->transfers.push_back({transfer, storeIndex[transfer.sourceId], storeIndex[transfer.destinationId]});
		mImpl->transferOrder.resize(mImpl->transfers.size());
		std::iota(mImpl->transferOrder.begin(), mImpl->transferOrder.end(), size_t{0});
		std::stable_sort(mImpl->transferOrder.begin(), mImpl->transferOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->transfers[left].authored.priority > mImpl->transfers[right].authored.priority; });
		for (auto const& conversion : settings.conversions)
		{
			Impl::ConversionState state;
			state.authored = conversion;
			for (auto const& flow : conversion.inputs)
				state.inputs.push_back({storeIndex[flow.storeId], flow.ratePerSecond});
			for (auto const& flow : conversion.outputs)
				state.outputs.push_back({storeIndex[flow.storeId], flow.yieldPerSecond});
			mImpl->conversions.push_back(std::move(state));
		}
		mImpl->conversionOrder.resize(mImpl->conversions.size());
		std::iota(mImpl->conversionOrder.begin(), mImpl->conversionOrder.end(), size_t{0});
		std::stable_sort(mImpl->conversionOrder.begin(), mImpl->conversionOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->conversions[left].authored.priority > mImpl->conversions[right].authored.priority; });
		for (auto const& array : settings.solarArrays)
			mImpl->solarArrays.push_back({array});
		// Thermal reservoirs start at their authored starting temperature;
		// the limit flags start consistent with it so the first tick
		// reports transitions, not the initial condition (#395).
		std::map<std::string, size_t> reservoirIndex;
		for (auto const& reservoir : settings.thermalReservoirs)
		{
			Impl::ReservoirState state;
			state.authored = reservoir;
			state.temperatureC = reservoir.startingTemperatureC;
			reservoirIndex[reservoir.id] = mImpl->reservoirs.size();
			mImpl->reservoirs.push_back(std::move(state));
		}
		for (auto const& radiator : settings.radiators)
		{
			Impl::RadiatorState state;
			state.authored = radiator;
			state.reservoirIndex = reservoirIndex[radiator.reservoirId];
			mImpl->radiators.push_back(std::move(state));
			mImpl->reservoirs[state.reservoirIndex].radiatorIndices.push_back(
				mImpl->radiators.size() - 1);
		}
		// Coolant loops start charged with their authored coolant at their
		// authored temperature; the inventory is runtime state initialised
		// from the authored values and never written back (#396). A loop
		// with zero starting coolant is disabled: it pumps no flow and
		// heats nothing until it somehow has fluid, which nothing in this
		// slice provides.
		for (auto const& loop : settings.coolantLoops)
		{
			Impl::LoopState state;
			state.authored = loop;
			state.reservoirIndex = reservoirIndex[loop.reservoirId];
			state.coolantKg = loop.startingCoolantKg;
			state.temperatureC = loop.startingTemperatureC;
			mImpl->coolantLoops.push_back(std::move(state));
		}
		// Chillers start idle with their contracts resolved to the loop
		// and sink they couple to; every service result and ledger is
		// runtime state reconstructed from the authored values on load
		// and Reset (#397).
		std::map<std::string, size_t> loopIndex;
		for (size_t index = 0; index < mImpl->coolantLoops.size(); ++index)
			loopIndex[mImpl->coolantLoops[index].authored.id] = index;
		// The chiller id-to-index lookup is filled as the authored chillers
		// register, so an outage resolves the chiller it names instead of
		// default-inserting index 0 into a map that was built before any
		// chiller existed and came up empty (#575).
		std::map<std::string, size_t> chillerIndex;
		for (auto const& chiller : settings.chillers)
		{
			Impl::ChillerState state;
			state.authored = chiller;
			state.loopIndex = loopIndex[chiller.coldLoopId];
			state.sinkIndex = reservoirIndex[chiller.condenserReservoirId];
			chillerIndex[chiller.id] = mImpl->chillers.size();
			mImpl->chillers.push_back(std::move(state));
		}
		// Thermal buffers start holding their authored starting energy with
		// their contracts resolved to the host reservoir and, in circulation
		// mode, the gating coolant loop; the store, ledgers and exposure are
		// runtime state reconstructed from the authored values on load and
		// Reset (#398).
		for (auto const& buffer : settings.thermalBuffers)
		{
			Impl::BufferState state;
			state.authored = buffer;
			state.reservoirIndex = reservoirIndex[buffer.reservoirId];
			if (buffer.accessMode == "circulation")
				state.gateLoopIndex = loopIndex[buffer.circulationLoopId];
			state.storedKJ = buffer.startingEnergyKJ;
			mImpl->thermalBuffers.push_back(std::move(state));
			mImpl->reservoirs[state.reservoirIndex].bufferIndices.push_back(
				mImpl->thermalBuffers.size() - 1);
		}
		// Generators start stopped with their contracts resolved to the
		// fuel store and heat-sink reservoir they couple to; every phase,
		// draw and ledger is runtime state reconstructed from the authored
		// values on load and Reset (#399).
		for (auto const& generator : settings.generators)
		{
			Impl::GeneratorState state;
			state.authored = generator;
			state.fuelStoreIndex = storeIndex[generator.fuelStoreId];
			state.sinkIndex = reservoirIndex[generator.heatSinkId];
			mImpl->generators.push_back(std::move(state));
		}
		// Compartments start with their authored gas inventory at their
		// authored temperature; the starting absolute pressure is derived
		// from the starting conditions so the snapshot can expose initial
		// versus current state side by side (#400). The inventory is
		// runtime state initialised from the authored values and never
		// written back; load and Reset reconstruct it.
		std::map<std::string, size_t> atmosphereIndex;
		for (auto const& atmosphere : settings.atmospheres)
		{
			Impl::AtmosphereState state;
			state.authored = atmosphere;
			state.oxygenKg = atmosphere.startingOxygenKg;
			state.nitrogenKg = atmosphere.startingNitrogenKg;
			state.carbonDioxideKg = atmosphere.startingCarbonDioxideKg;
			state.waterVapourKg = atmosphere.startingWaterVapourKg;
			state.temperatureC = atmosphere.startingTemperatureC;
			state.startingPressureKPa = stationAtmospherePartialPressureKPa(
				atmosphere.startingOxygenKg, StationMolarMassOxygenKgPerMol,
				atmosphere.volumeM3, atmosphere.startingTemperatureC)
				+ stationAtmospherePartialPressureKPa(atmosphere.startingNitrogenKg,
					StationMolarMassNitrogenKgPerMol, atmosphere.volumeM3,
					atmosphere.startingTemperatureC)
				+ stationAtmospherePartialPressureKPa(atmosphere.startingCarbonDioxideKg,
					StationMolarMassCarbonDioxideKgPerMol, atmosphere.volumeM3,
					atmosphere.startingTemperatureC)
				+ stationAtmospherePartialPressureKPa(atmosphere.startingWaterVapourKg,
					StationMolarMassWaterKgPerMol, atmosphere.volumeM3,
					atmosphere.startingTemperatureC);
			if (!atmosphere.reservoirId.empty())
				state.reservoirIndex = reservoirIndex[atmosphere.reservoirId];
			atmosphereIndex[atmosphere.id] = mImpl->atmospheres.size();
			mImpl->atmospheres.push_back(std::move(state));
		}
		for (auto const& pathway : settings.atmospherePathways)
		{
			Impl::AtmospherePathwayState state;
			state.authored = pathway;
			if (pathway.aAtmosphereId != StationExteriorAtmosphereId)
				state.aIndex = atmosphereIndex.at(pathway.aAtmosphereId);
			if (pathway.bAtmosphereId != StationExteriorAtmosphereId)
				state.bIndex = atmosphereIndex.at(pathway.bAtmosphereId);
			mImpl->atmospherePathways.push_back(std::move(state));
		}
		// Airlock cycle schedules start unfired: the authored first
		// occurrence is the next cycle, and nothing is drawn before its
		// time. Reset and load reconstruct this state, so a replay
		// restarts every schedule from its authored beginning (#400).
		for (auto const& cycle : settings.airlockCycles)
		{
			Impl::AirlockCycleState state;
			state.authored = cycle;
			state.atmosphereIndex = atmosphereIndex[cycle.atmosphereId];
			state.declarationIndex = mImpl->airlockCycles.size();
			mImpl->airlockCycles.push_back(std::move(state));
		}
		// Crew ledgers start empty of consumption: the authored population,
		// stores and compartment are resolved to indices at construction so
		// the tick ledger never re-looks-up references, and every pathway
		// total reconstructs from zero on Reset and load (#401).
		for (auto const& crew : settings.crews)
		{
			Impl::CrewState state;
			state.authored = crew;
			state.foodStoreIndex = storeIndex[crew.foodStoreId];
			state.waterStoreIndex = storeIndex[crew.waterStoreId];
			if (!crew.greywaterStoreId.empty()) state.greywaterStoreIndex = storeIndex.at(crew.greywaterStoreId);
			state.urineStoreIndex = storeIndex[crew.urineStoreId];
			state.organicWasteStoreIndex = storeIndex[crew.organicWasteStoreId];
			state.atmosphereIndex = atmosphereIndex[crew.atmosphereId];
			mImpl->crews.push_back(std::move(state));
		}
		// Water recovery ledgers start empty of recovery (#402): the
		// authored contracts are resolved to compartment, loop and store
		// indices at construction so the tick ledger never re-looks-up
		// references, and every recovery total reconstructs from zero on
		// Reset and load. Treatments compete for the same power pool and the
		// same stores, so they run in the declared deterministic order:
		// priority descending, declaration order ascending.
		for (auto const& collector : settings.waterCollectors)
		{
			Impl::WaterCollectorState state;
			state.authored = collector;
			state.atmosphereIndex = atmosphereIndex[collector.atmosphereId];
			state.loopIndex = loopIndex[collector.coolingLoopId];
			state.destStoreIndex = storeIndex[collector.destStoreId];
			mImpl->waterCollectors.push_back(std::move(state));
		}
		for (auto const& treatment : settings.waterTreatments)
		{
			Impl::WaterTreatmentState state;
			state.authored = treatment;
			state.sourceIndex = storeIndex[treatment.sourceStoreId];
			state.treatedIndex = storeIndex[treatment.treatedStoreId];
			state.concentrateIndex = storeIndex[treatment.concentrateStoreId];
			state.consumableIndex = storeIndex[treatment.consumableStoreId];
			state.declarationIndex = mImpl->waterTreatments.size();
			mImpl->waterTreatments.push_back(std::move(state));
		}
		mImpl->waterTreatmentOrder.resize(mImpl->waterTreatments.size());
		std::iota(mImpl->waterTreatmentOrder.begin(), mImpl->waterTreatmentOrder.end(), size_t{0});
		std::stable_sort(mImpl->waterTreatmentOrder.begin(), mImpl->waterTreatmentOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->waterTreatments[left].authored.priority
				> mImpl->waterTreatments[right].authored.priority; });
		// Oxygen production and CO2 capture ledgers start empty (#404): the
		// authored contracts are resolved to store, compartment and sink
		// indices at construction so the tick ledger never re-looks-up
		// references, and every production and capture total reconstructs
		// from zero on Reset and load. Cells and capture machines compete
		// for the same power pool and the same stores, so each runs in the
		// declared deterministic order: priority descending, declaration
		// order ascending.
		for (auto const& electrolyzer : settings.electrolyzers)
		{
			Impl::ElectrolyzerState state;
			state.authored = electrolyzer;
			state.waterStoreIndex = storeIndex[electrolyzer.waterStoreId];
			if (!electrolyzer.oxygenStoreId.empty())
				state.oxygenStoreIndex = storeIndex[electrolyzer.oxygenStoreId];
			state.hydrogenStoreIndex = storeIndex[electrolyzer.hydrogenStoreId];
			if (!electrolyzer.atmosphereId.empty())
				state.atmosphereIndex = atmosphereIndex[electrolyzer.atmosphereId];
			state.sinkIndex = reservoirIndex[electrolyzer.heatSinkId];
			state.declarationIndex = mImpl->electrolyzers.size();
			mImpl->electrolyzers.push_back(std::move(state));
		}
		mImpl->electrolyzerOrder.resize(mImpl->electrolyzers.size());
		std::iota(mImpl->electrolyzerOrder.begin(), mImpl->electrolyzerOrder.end(), size_t{0});
		std::stable_sort(mImpl->electrolyzerOrder.begin(), mImpl->electrolyzerOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->electrolyzers[left].authored.priority
				> mImpl->electrolyzers[right].authored.priority; });
		for (auto const& capture : settings.co2Captures)
		{
			Impl::Co2CaptureState state;
			state.authored = capture;
			state.atmosphereIndex = atmosphereIndex[capture.atmosphereId];
			state.captureStoreIndex = storeIndex[capture.captureStoreId];
			if (!capture.consumableStoreId.empty())
				state.consumableStoreIndex = storeIndex[capture.consumableStoreId];
			state.declarationIndex = mImpl->co2Captures.size();
			mImpl->co2Captures.push_back(std::move(state));
		}
		mImpl->co2CaptureOrder.resize(mImpl->co2Captures.size());
		std::iota(mImpl->co2CaptureOrder.begin(), mImpl->co2CaptureOrder.end(), size_t{0});
		std::stable_sort(mImpl->co2CaptureOrder.begin(), mImpl->co2CaptureOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->co2Captures[left].authored.priority
				> mImpl->co2Captures[right].authored.priority; });
		// Carbon-recovery ledgers start empty (#405): the authored
		// contracts are resolved to store and sink indices at construction
		// so the tick ledger never re-looks-up references, and every
		// conversion, split and heat total reconstructs from zero on Reset
		// and load. Reactors and processors compete for the same power pool
		// and share the hydrogen and methane stores, so each runs in the
		// declared deterministic order: priority descending, declaration
		// order ascending.
		for (auto const& reactor : settings.sabatierReactors)
		{
			Impl::SabatierState state;
			state.authored = reactor;
			state.co2StoreIndex = storeIndex[reactor.co2StoreId];
			state.hydrogenStoreIndex = storeIndex[reactor.hydrogenStoreId];
			state.methaneStoreIndex = storeIndex[reactor.methaneStoreId];
			state.waterStoreIndex = storeIndex[reactor.waterStoreId];
			state.sinkIndex = reservoirIndex[reactor.heatSinkId];
			state.declarationIndex = mImpl->sabatierReactors.size();
			mImpl->sabatierReactors.push_back(std::move(state));
		}
		mImpl->sabatierOrder.resize(mImpl->sabatierReactors.size());
		std::iota(mImpl->sabatierOrder.begin(), mImpl->sabatierOrder.end(), size_t{0});
		std::stable_sort(mImpl->sabatierOrder.begin(), mImpl->sabatierOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->sabatierReactors[left].authored.priority
				> mImpl->sabatierReactors[right].authored.priority; });
		for (auto const& processor : settings.methaneProcessors)
		{
			Impl::MethaneProcessorState state;
			state.authored = processor;
			state.methaneStoreIndex = storeIndex[processor.methaneStoreId];
			state.hydrogenStoreIndex = storeIndex[processor.hydrogenStoreId];
			state.carbonStoreIndex = storeIndex[processor.carbonStoreId];
			state.sinkIndex = reservoirIndex[processor.heatSinkId];
			state.declarationIndex = mImpl->methaneProcessors.size();
			mImpl->methaneProcessors.push_back(std::move(state));
		}
		mImpl->methaneProcessorOrder.resize(mImpl->methaneProcessors.size());
		std::iota(mImpl->methaneProcessorOrder.begin(), mImpl->methaneProcessorOrder.end(), size_t{0});
		std::stable_sort(mImpl->methaneProcessorOrder.begin(), mImpl->methaneProcessorOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->methaneProcessors[left].authored.priority
				> mImpl->methaneProcessors[right].authored.priority; });
		// Nutrient-recovery ledgers start empty (#407): the authored
		// contracts are resolved to store and sink indices at construction
		// so the tick ledger never re-looks-up references, and every
		// consumption, recovery and heat total reconstructs from zero on
		// Reset and load. Processors compete for the same power pool and
		// share the waste, water and CO2 stores, so each runs in the
		// declared deterministic order: priority descending, declaration
		// order ascending.
		for (auto const& processor : settings.microbialProcessors)
		{
			Impl::MicrobialProcessorState state;
			state.authored = processor;
			state.feedStoreIndex = storeIndex[processor.feedStoreId];
			state.oxygenStoreIndex = storeIndex[processor.oxygenStoreId];
			state.waterStoreIndex = storeIndex[processor.waterStoreId];
			state.consumableStoreIndex = storeIndex[processor.consumableStoreId];
			state.nutrientStoreIndex = storeIndex[processor.nutrientStoreId];
			state.recoveredWaterStoreIndex = storeIndex[processor.recoveredWaterStoreId];
			state.co2StoreIndex = storeIndex[processor.co2StoreId];
			state.residueStoreIndex = storeIndex[processor.residueStoreId];
			state.sinkIndex = reservoirIndex[processor.heatSinkId];
			// #408: the replacement stock reference resolves like any other
			// consumed store, and the culture starts at its authored
			// condition; condition history and stock totals reconstruct
			// from the authored start on Reset and load.
			state.hasStock = !processor.stockStoreId.empty();
			if (state.hasStock)
				state.stockStoreIndex = storeIndex[processor.stockStoreId];
			state.condition = processor.initialCondition;
			state.declarationIndex = mImpl->microbialProcessors.size();
			mImpl->microbialProcessors.push_back(std::move(state));
		}
		mImpl->microbialProcessorOrder.resize(mImpl->microbialProcessors.size());
		std::iota(mImpl->microbialProcessorOrder.begin(), mImpl->microbialProcessorOrder.end(), size_t{0});
		std::stable_sort(mImpl->microbialProcessorOrder.begin(), mImpl->microbialProcessorOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->microbialProcessors[left].authored.priority
				> mImpl->microbialProcessors[right].authored.priority; });
		// Crop production ledgers start from the authored Living biomass
		// (#409): the Growing capacity is derived from the declared area
		// and density at construction, the population starts at its
		// authored dry biomass, and every growth, respiration, uptake and
		// heat total reconstructs from zero on Reset and load. Populations
		// compete for the same power pool and share the compartment and
		// uptake stores, so each runs in the declared deterministic
		// order: priority descending, declaration order ascending.
		for (auto const& crop : settings.cropPopulations)
		{
			Impl::CropPopulationState state;
			state.authored = crop;
			state.atmosphereIndex = atmosphereIndex[crop.atmosphereId];
			state.waterStoreIndex = storeIndex[crop.waterStoreId];
			state.nitrogenStoreIndex = storeIndex[crop.nitrogenStoreId];
			state.phosphorusStoreIndex = storeIndex[crop.phosphorusStoreId];
			state.potassiumStoreIndex = storeIndex[crop.potassiumStoreId];
			state.mineralsStoreIndex = storeIndex[crop.mineralsStoreId];
			// Harvest destinations resolve only when named; the harvest is
			// disabled while the trigger is zero (#410).
			if (!crop.foodStoreId.empty())
				state.foodStoreIndex = storeIndex[crop.foodStoreId];
			if (!crop.wasteStoreId.empty())
				state.wasteStoreIndex = storeIndex[crop.wasteStoreId];
			state.capacityKg = crop.growingAreaM2 * crop.maxBiomassDensityKgPerM2;
			state.livingBiomassKg = crop.initialBiomassKg;
			// The living constituent inventory (#571) opens at the declared
			// initial biomass under the declared composition, so the organic
			// and bound-nutrient amounts the canopy starts with are real
			// quantities rather than a fraction re-applied every tick.
			state.livingConstituents = constituentsOf(crop.initialBiomassKg, crop.composition);
			mImpl->cropPopulations.push_back(std::move(state));
		}
		mImpl->cropOrder.resize(mImpl->cropPopulations.size());
		std::iota(mImpl->cropOrder.begin(), mImpl->cropOrder.end(), size_t{0});
		std::stable_sort(mImpl->cropOrder.begin(), mImpl->cropOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->cropPopulations[left].authored.priority
				> mImpl->cropPopulations[right].authored.priority; });
		// Algae culture ledgers start from the authored Living biomass
		// (#411): the culture capacity is derived from the declared
		// volume and density per litre at construction, the culture starts
		// at its authored dry biomass, and every growth, respiration,
		// uptake, harvest and heat total reconstructs from zero on Reset
		// and load. Cultures compete for the same power pool and share the
		// compartment and uptake stores, so each runs in the declared
		// deterministic order: priority descending, declaration order
		// ascending.
		for (auto const& algae : settings.algaeCultures)
		{
			Impl::AlgaeCultureState state;
			state.authored = algae;
			state.atmosphereIndex = atmosphereIndex[algae.atmosphereId];
			state.waterStoreIndex = storeIndex[algae.waterStoreId];
			state.nitrogenStoreIndex = storeIndex[algae.nitrogenStoreId];
			state.phosphorusStoreIndex = storeIndex[algae.phosphorusStoreId];
			state.potassiumStoreIndex = storeIndex[algae.potassiumStoreId];
			state.mineralsStoreIndex = storeIndex[algae.mineralsStoreId];
			// Harvest destinations resolve only when named; the harvest is
			// disabled while the trigger is zero.
			if (!algae.foodStoreId.empty())
				state.foodStoreIndex = storeIndex[algae.foodStoreId];
			if (!algae.recoveryStoreId.empty())
				state.recoveryStoreIndex = storeIndex[algae.recoveryStoreId];
			if (!algae.wasteStoreId.empty())
				state.wasteStoreIndex = storeIndex[algae.wasteStoreId];
			state.capacityKg = algae.cultureVolumeL * algae.maxBiomassDensityKgPerL;
			state.livingBiomassKg = algae.initialBiomassKg;
			// The living constituent inventory (#571) opens at the declared
			// initial biomass under the declared composition, so the organic
			// and bound-nutrient amounts the culture starts with are real
			// quantities rather than a fraction re-applied every tick.
			state.livingConstituents = constituentsOf(algae.initialBiomassKg, algae.composition);
			mImpl->algaeCultures.push_back(std::move(state));
		}
		mImpl->algaeOrder.resize(mImpl->algaeCultures.size());
		std::iota(mImpl->algaeOrder.begin(), mImpl->algaeOrder.end(), size_t{0});
		std::stable_sort(mImpl->algaeOrder.begin(), mImpl->algaeOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->algaeCultures[left].authored.priority
				> mImpl->algaeCultures[right].authored.priority; });
		// Fuel-cell backup units run on the supply side in declaration
		// order (#406); their reactant and product stores are resolved to
		// the same store indices the ledger mutates.
		for (auto const& cell : settings.fuelCells)
		{
			Impl::FuelCellState state;
			state.authored = cell;
			state.hydrogenStoreIndex = storeIndex[cell.hydrogenStoreId];
			state.oxygenStoreIndex = storeIndex[cell.oxygenStoreId];
			state.waterStoreIndex = storeIndex[cell.waterStoreId];
			state.sinkIndex = reservoirIndex[cell.heatSinkId];
			state.declarationIndex = mImpl->fuelCells.size();
			mImpl->fuelCells.push_back(std::move(state));
		}
		for (auto const& load : settings.equipmentLoads)
		{
			Impl::LoadState state;
			state.authored = load;
			for (auto const& exchange : load.materialExchanges)
				state.exchanges.push_back({storeIndex[exchange.storeId], exchange.ratePerSecond,
					exchange.consumes});
			// A declared heat sink routes this load's emitted heat into the
			// reservoir exactly once instead of direct export (#395).
			if (!load.heatSinkId.empty())
			{
				state.hasHeatSink = true;
				state.heatSinkIndex = reservoirIndex[load.heatSinkId];
			}
			mImpl->loads.push_back(std::move(state));
		}
		mImpl->loadOrder.resize(mImpl->loads.size());
		std::iota(mImpl->loadOrder.begin(), mImpl->loadOrder.end(), size_t{0});
		std::stable_sort(mImpl->loadOrder.begin(), mImpl->loadOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->loads[left].authored.priority > mImpl->loads[right].authored.priority; });
		// Stored energy is runtime state initialised from the authored
		// starting value; the reserve and saturation flags start consistent
		// with it so the first tick reports transitions, not the initial
		// condition (#394).
		for (auto const& battery : settings.batteries)
		{
			Impl::BatteryState state;
			state.authored = battery;
			state.stored = battery.startingStoredKWh;
			if (battery.usableKWh > 0.0)
			{
				state.lastAtReserve = state.stored <= battery.nameplateKWh - battery.usableKWh + 1e-12;
				state.lastSaturated = state.stored >= battery.nameplateKWh - 1e-12;
			}
			mImpl->batteries.push_back(std::move(state));
			// #419: the battery's endurance window tracks drawable energy -
			// stored above the reserve floor - never the nameplate total and
			// never the discharge power rating.
			Impl::EnduranceWindow enduranceWindow;
			enduranceWindow.windowStartDrawable = std::max(0.0,
				battery.startingStoredKWh - (battery.nameplateKWh - battery.usableKWh));
			mImpl->batteryEndurance.push_back(enduranceWindow);
		}
		// Delivery schedules start unfired: the authored first occurrence is
		// the next arrival, and nothing is brought in before its time. Reset
		// and load reconstruct this state, so a replay restarts every
		// schedule from the authored beginning rather than resuming a
		// partially delivered one (#392).
		for (auto const& delivery : settings.deliveries)
		{
			Impl::DeliveryState state;
			state.authored = delivery;
			state.storeIndex = storeIndex[delivery.storeId];
			state.declarationIndex = mImpl->deliveries.size();
			mImpl->deliveries.push_back(std::move(state));
		}
		// Waste ejection schedules start unfired for the same reason
		// delivery schedules do: reset and load reconstruct the authored
		// beginning, so a replay restarts every ejection schedule rather
		// than resuming a partially ejected one (#403).
		for (auto const& ejection : settings.wasteEjections)
		{
			Impl::WasteEjectionState state;
			state.authored = ejection;
			state.storeIndex = storeIndex[ejection.storeId];
			state.declarationIndex = mImpl->wasteEjections.size();
			mImpl->wasteEjections.push_back(std::move(state));
		}
		// Installation outage schedules start with no window open: the
		// authored first occurrence is the first outage, and nothing is
		// reduced before its time. Each outage resolves its target's stable
		// identity to the installation state it caps, so the tick pass can
		// read the effective availability factor without a per-tick name
		// lookup. Reset and load reconstruct the authored beginning, so a
		// replay restarts every outage schedule from its first window
		// rather than resuming a partially elapsed one (#413).
		std::map<std::string, size_t> generatorIndex;
		for (size_t index = 0; index < mImpl->generators.size(); ++index)
			generatorIndex[mImpl->generators[index].authored.id] = index;
		for (auto const& outage : settings.outages)
		{
			Impl::OutageState state;
			state.authored = outage;
			// The target must resolve to exactly one registered installation
			// (#576). Validation already refuses a cross-kind id collision,
			// so a hit in more than one kind here means the namespace broke
			// somewhere - refuse loudly rather than silently choosing the
			// first kind consulted.
			auto const generatorHits = generatorIndex.count(outage.installationId);
			auto const loopHits = loopIndex.count(outage.installationId);
			auto const chillerHits = chillerIndex.count(outage.installationId);
			if (generatorHits + loopHits + chillerHits > 1)
				throw SerializationException("Station installation outage '"
					+ outage.id + "' targets installation id '" + outage.installationId
					+ "' that is ambiguous across generator, coolant loop and chiller registrations");
			if (generatorHits != 0)
			{
				state.target = Impl::OutageState::Target::Generator;
				state.targetIndex = generatorIndex[outage.installationId];
				state.installationKind = "generator";
			}
			else if (loopIndex.count(outage.installationId) != 0)
			{
				state.target = Impl::OutageState::Target::CoolantLoop;
				state.targetIndex = loopIndex[outage.installationId];
				state.installationKind = "coolant loop";
			}
			else if (chillerIndex.count(outage.installationId) != 0)
			{
				state.target = Impl::OutageState::Target::Chiller;
				state.targetIndex = chillerIndex[outage.installationId];
				state.installationKind = "chiller";
			}
			else
				// Validation admits only registered installations, so an
				// unresolved id here means the resolution itself broke;
				// refuse loudly rather than silently cap the wrong
				// machine through a default-inserted index (#575).
				throw SerializationException("Station installation outage '"
					+ outage.id + "' targets installation '" + outage.installationId
					+ "' that resolved to no registered generator, coolant loop or chiller");
			mImpl->outages.push_back(std::move(state));
		}
		// Maintenance job schedules start idle with their first window
		// still ahead (#414). Each job resolves its target installation
		// the same way outages do - exactly one registered kind must
		// match - and its requirement and waste stores to indices into
		// the live store ledger, so the tick pass never does name
		// lookups. Reset and load reconstruct this starting state, so a
		// replay restarts every maintenance schedule from its authored
		// beginning rather than resuming a half-spent one.
		for (auto const& job : settings.maintenanceJobs)
		{
			Impl::MaintenanceState state;
			state.authored = job;
			auto const generatorHits = generatorIndex.count(job.installationId);
			auto const loopHits = loopIndex.count(job.installationId);
			auto const chillerHits = chillerIndex.count(job.installationId);
			if (generatorHits + loopHits + chillerHits > 1)
				throw SerializationException("Station maintenance job '"
					+ job.id + "' targets installation id '" + job.installationId
					+ "' that is ambiguous across generator, coolant loop and chiller registrations");
			if (generatorHits != 0)
			{
				state.target = Impl::MaintenanceState::Target::Generator;
				state.targetIndex = generatorIndex[job.installationId];
				state.installationKind = "generator";
			}
			else if (loopHits != 0)
			{
				state.target = Impl::MaintenanceState::Target::CoolantLoop;
				state.targetIndex = loopIndex[job.installationId];
				state.installationKind = "coolant loop";
			}
			else if (chillerHits != 0)
			{
				state.target = Impl::MaintenanceState::Target::Chiller;
				state.targetIndex = chillerIndex[job.installationId];
				state.installationKind = "chiller";
			}
			else
				throw SerializationException("Station maintenance job '"
					+ job.id + "' targets installation '" + job.installationId
					+ "' that resolved to no registered generator, coolant loop or chiller");
			for (auto const& requirement : job.requirements)
			{
				auto const it = storeIndex.find(requirement.storeId);
				if (it == storeIndex.end())
					throw SerializationException("Station maintenance job '"
						+ job.id + "' requirement store '" + requirement.storeId
						+ "' resolved to no configured store");
				Impl::MaintenanceState::RequirementState requirementState;
				requirementState.storeIndex = it->second;
				state.requirements.push_back(requirementState);
			}
			if (!job.wasteStoreId.empty())
			{
				auto const it = storeIndex.find(job.wasteStoreId);
				if (it == storeIndex.end())
					throw SerializationException("Station maintenance job '"
						+ job.id + "' waste store '" + job.wasteStoreId
						+ "' resolved to no configured store");
				state.wasteStoreIndex = it->second;
			}
			mImpl->maintenanceJobs.push_back(std::move(state));
		}
		// Maintenance jobs compete for the same tools, parts and waste
		// headroom, so the reservation pass runs them in the declared
		// deterministic order: priority descending, declaration order
		// ascending (#414).
		mImpl->maintenanceOrder.resize(mImpl->maintenanceJobs.size());
		std::iota(mImpl->maintenanceOrder.begin(), mImpl->maintenanceOrder.end(), 0);
		std::stable_sort(mImpl->maintenanceOrder.begin(), mImpl->maintenanceOrder.end(),
			[&](size_t left, size_t right)
			{
				return mImpl->maintenanceJobs[left].authored.priority
					> mImpl->maintenanceJobs[right].authored.priority;
			});
		// The compound conservation boundary is fixed at run start from the
		// authored starting inventory of every compound store (#391).
		for (auto const& store : mImpl->stores)
			if (store.authored.composition)
				addComposition(mImpl->startingConstituents, *store.authored.composition,
					massToKg(store.authored.unit,
						store.authored.startingQualified + store.authored.startingUnqualified), 1.0);
		if (!settings.enabled) return;
		auto const source = sourceLoader ? sourceLoader() : bundledStationSystemsSource();
		// No upper bound on source size. The compiled footprint is trivial —
		// the bundled model loads to ~74 KiB per sandbox against a 512 KiB
		// budget — and a cap on the length of the text guards nothing the
		// ScratchBudget's allocation and instruction limits do not already
		// cover properly: compile time is not VM instructions, and per-tick
		// cost follows the configured scenario rather than the script length.
		// A cap here only ever produced a confusing construction refusal
		// whenever the lifecycle model legitimately grew.
		if (source.empty())
			throw SerializationException("Station systems source is empty");
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto object = std::make_unique<ScriptObject>();
			Construction construction{object.get(), &source, StationSystemNames[i],
				&settings.stores, &settings.transfers, &settings.conversions,
				&settings.solarArrays, &settings.equipmentLoads, &settings.batteries,
				&settings.deliveries, &settings.thermalReservoirs, &settings.radiators,
				&settings.coolantLoops, &settings.chillers, &settings.generators,
				&settings.atmospheres, &settings.airlockCycles, &settings.crews,
				&settings.waterCollectors, &settings.waterTreatments,
				&settings.electrolyzers, &settings.co2Captures, &settings.fuelCells,
				&settings.thermalBuffers, &settings.wasteEjections,
				&settings.outages,
				&settings.sabatierReactors, &settings.methaneProcessors,
				&settings.microbialProcessors, &settings.cropPopulations,
				&settings.algaeCultures,
				Construction::settingsDispatchModeName(settings.dispatchMode),
				&settings.maintenanceJobs};
			script::beginInstructionBudget(object->state.get(), object->budget);
			auto status = script::protectedLuaOperation(object->state.get(), construction, 0, 0);
			script::endInstructionBudget(object->state.get());
			if (status != LUA_OK || script::budgetExhausted(object->budget))
			{
				throw SerializationException(std::string(StationSystemNames[i]) + ": "
					+ (script::budgetExhausted(object->budget) ? std::string(script::budgetDiagnostic(object->budget))
						: errorMessage(object->state.get(), "Station constructor failed")));
			}
			lua_settop(object->state.get(), 0);
			mImpl->objects[i] = std::move(object);
		}
		appendBoundedEvent(mImpl->events, 0, "station", "Station systems initialized with "
			+ std::to_string(settings.stores.size()) + " resource store(s), "
			+ std::to_string(settings.transfers.size()) + " transfer(s), "
			+ std::to_string(settings.conversions.size()) + " conversion(s), "
			+ std::to_string(settings.solarArrays.size()) + " solar array(s), "
			+ std::to_string(settings.equipmentLoads.size()) + " external equipment load(s), "
			+ std::to_string(settings.batteries.size()) + " battery(ies), "
			+ std::to_string(settings.deliveries.size()) + " resupply delivery(ies), "
				+ std::to_string(settings.thermalReservoirs.size()) + " thermal reservoir(s), "
			+ std::to_string(settings.radiators.size()) + " radiator(s), "
			+ std::to_string(settings.coolantLoops.size()) + " coolant loop(s), "
			+ std::to_string(settings.chillers.size()) + " chiller(s) and "
			+ std::to_string(settings.generators.size()) + " fuelled generator(s), "
			+ std::to_string(settings.atmospheres.size()) + " atmosphere compartment(s) and "
			+ std::to_string(settings.airlockCycles.size()) + " scheduled airlock cycle(s) and "
			+ std::to_string(settings.crews.size()) + " crew(s)"
			+ (settings.waterCollectors.empty() ? std::string()
				: ", " + std::to_string(settings.waterCollectors.size()) + " water collector(s)")
			+ (settings.waterTreatments.empty() ? std::string()
				: ", " + std::to_string(settings.waterTreatments.size()) + " water treatment(s)")
			+ (settings.electrolyzers.empty() ? std::string()
				: ", " + std::to_string(settings.electrolyzers.size()) + " electrolyzer(s)")
			+ (settings.co2Captures.empty() ? std::string()
				: ", " + std::to_string(settings.co2Captures.size()) + " CO2 capture machine(s)")
			+ (settings.fuelCells.empty() ? std::string()
				: ", " + std::to_string(settings.fuelCells.size()) + " fuel-cell backup unit(s)")
			+ (settings.sabatierReactors.empty() ? std::string()
				: ", " + std::to_string(settings.sabatierReactors.size()) + " Sabatier reactor(s)")
			+ (settings.methaneProcessors.empty() ? std::string()
				: ", " + std::to_string(settings.methaneProcessors.size()) + " methane processor(s)")
			+ (settings.thermalBuffers.empty() ? std::string()
				: ", " + std::to_string(settings.thermalBuffers.size()) + " thermal buffer(s)")
			+ (settings.wasteEjections.empty() ? std::string()
				: ", " + std::to_string(settings.wasteEjections.size()) + " terminal-waste ejection(s)"));
	}
	StationSystemsRuntime::~StationSystemsRuntime() = default;
	bool StationSystemsRuntime::advanceTo(double simulatedSeconds)
	{
		if (!std::isfinite(simulatedSeconds) || simulatedSeconds < 0) return false;
		if (!mImpl->settings.enabled) return true;
		if (!mImpl->snapshot.healthy) return false;
		// Reject unrepresentable targets before any work or integer overflow.
		if (simulatedSeconds / mImpl->clock.stepSeconds() >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return false;
		while (mImpl->clock.elapsedSeconds() < simulatedSeconds)
			if (!advanceStep()) return false;
		return true;
	}
	bool StationSystemsRuntime::advanceStep()
	{
		auto const stepStartedAt = SimulationStepTiming::Clock::now();
		auto const tick = mImpl->clock.steps() + 1;
		auto const seconds = mImpl->clock.stepSeconds();
		auto next = mImpl->snapshot;
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto& object = *mImpl->objects[i];
			Step step{&object, tick, mImpl->clock.stepSeconds()};
			try
			{
				script::beginInstructionBudget(object.state.get(), object.budget);
				auto status = script::protectedLuaOperation(object.state.get(), step, 0, 0);
				script::endInstructionBudget(object.state.get());
				if (status != LUA_OK || script::budgetExhausted(object.budget))
				{
					throw std::runtime_error(script::budgetExhausted(object.budget)
						? std::string(script::budgetDiagnostic(object.budget)) : errorMessage(object.state.get(), "Station step failed"));
				}
				// Every sandbox must acknowledge this Station step exactly once,
				// regardless of how often advanceTo was called by its driver.
				if (step.completed != tick)
				{
					step.invalidOutput = true;
					// Keep the ratio-1 diagnostic byte-identical for existing runs.
					throw std::runtime_error(mImpl->settings.systemsStepWorldTicks == 1
						? "Station lifecycle tick accounting mismatch"
						: "Station lifecycle step accounting mismatch");
				}
				next.completedTicks[i] = step.completed;
				lua_settop(object.state.get(), 0);
			}
			catch (std::exception const& error)
			{
				script::endInstructionBudget(object.state.get());
				mImpl->snapshot.healthy = false;
				mImpl->diagnostics.push_back({tick, StationSystemNames[i],
					script::failureKind(object.budget, step.invalidOutput
						? ScriptExecutionFailure::ConversionError : ScriptExecutionFailure::LuaError),
					std::string(error.what(), std::min(std::char_traits<char>::length(error.what()), DiagnosticByteLimit))});
				// Retire all mutated objects. No failed step can be retried, and no
				// partial result escapes into the public committed snapshot.
				for (auto& owned : mImpl->objects) owned.reset();
				return false;
			}
		}
		// The authoritative transfer ledger runs once every sandbox step has
		// succeeded: its staged inventory and events commit atomically, so a
		// conservation or limit violation never publishes a partial result
		// and never clamps material. It fails the whole tick through the
		// integrated error path instead.
		if (!mImpl->transferOrder.empty() || !mImpl->conversionOrder.empty()
			|| !mImpl->loads.empty() || !mImpl->solarArrays.empty() || !mImpl->batteries.empty()
			|| !mImpl->deliveries.empty() || !mImpl->wasteEjections.empty()
			|| !mImpl->maintenanceJobs.empty()
			|| !mImpl->reservoirs.empty()
			|| !mImpl->coolantLoops.empty() || !mImpl->chillers.empty()
			|| !mImpl->generators.empty() || !mImpl->atmospheres.empty()
			|| !mImpl->airlockCycles.empty() || !mImpl->crews.empty()
			|| !mImpl->waterCollectors.empty() || !mImpl->waterTreatments.empty()
			|| !mImpl->electrolyzers.empty() || !mImpl->co2Captures.empty()
			|| !mImpl->fuelCells.empty() || !mImpl->thermalBuffers.empty()
			|| !mImpl->sabatierReactors.empty() || !mImpl->methaneProcessors.empty()
			|| !mImpl->microbialProcessors.empty())
		{
			std::string violationSystem, violation;
			if (!mImpl->applyLedger(tick, seconds, violationSystem, violation))
			{
				mImpl->snapshot.healthy = false;
				mImpl->diagnostics.push_back({tick, violationSystem,
					ScriptExecutionFailure::ConversionError, violation});
				for (auto& owned : mImpl->objects) owned.reset();
				return false;
			}
		}
		next.worldTick = tick;
		mImpl->clock.commitStep();
		next.elapsedSeconds = mImpl->clock.elapsedSeconds();
		mImpl->snapshot = next;
		if (tick % mImpl->observationIntervalSteps == 0)
		{
			appendBoundedEvent(mImpl->events, tick, "observation",
				"All four Station systems completed the Station step");
			// #418: trends are sampled on the same existing observation
			// cadence from the just-committed snapshot - no second clock,
			// no per-tick history, and only successful ticks are sampled.
			mImpl->recordTrends(tick, snapshot());
			// #419: endurance windows advance on the same declared
			// observation interval, from the same committed ledger.
			mImpl->updateEndurance(tick);
		}
		// Recorded only on the success path, matching the World tick metric, which
		// likewise never samples a tick that aborted. A failed Station step is
		// reported through the diagnostics, not through the timing.
		auto const stepCompletedAt = SimulationStepTiming::Clock::now();
		mImpl->stepTiming.record(stepCompletedAt - stepStartedAt, stepCompletedAt);
		return true;
	}
	std::vector<StationSystemsEvent> const& StationSystemsRuntime::events() const { return mImpl->events; }
	double StationSystemsRuntime::averageStepMicroseconds() const
	{
		return mImpl->stepTiming.averageMicroseconds();
	}
	std::vector<StationTrendSeries> StationSystemsRuntime::trends() const { return mImpl->history.trends(); }
	std::vector<StationMetricHistory> StationSystemsRuntime::metrics() const { return mImpl->history.metrics(); }
	StationRecordingStatus StationSystemsRuntime::recordingStatus() const { return mImpl->history.status(); }
	StationObservationRevision StationSystemsRuntime::observationRevision() const { return {mRun, mImpl->history.revision()}; }
	std::vector<StationChartDefinition> const& StationSystemsRuntime::catalogue() const { return mImpl->history.catalogue(); }
	StationChartData const& StationSystemsRuntime::query(std::string const& id) const { return mImpl->history.query(id); }
	StationQueryWork StationSystemsRuntime::queryWork() const
	{
		auto work = mImpl->history.queryWork();
		work.observationLookups = mImpl->observationIndex.lookups;
		work.observationIndexEntries = mImpl->observationIndex.entriesIndexed;
		return work;
	}
	std::vector<StationSystemsDiagnostic> const& StationSystemsRuntime::diagnostics() const { return mImpl->diagnostics; }
}
