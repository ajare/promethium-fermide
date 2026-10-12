#include "StationSystemsRuntime.h"
#include "core/World.h"
#include "core/SerializationException.h"
#include "BundledStationSystemsSource.h"
#include "ScriptSandbox.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace core
{
	namespace
	{
		StationSystemsSourceLoader sourceLoader;
		constexpr size_t EventLimit = 256;
		constexpr size_t DiagnosticByteLimit = 2048;
		// Every public event producer appends through this one bounded
		// helper (#551): the history never retains more than the newest
		// EventLimit entries, evicting oldest-first, so a tick that
		// announces several transitions at once cannot push the history
		// past the bound, and later appends keep evicting no matter how
		// the vector grew. Declaration order within a tick is preserved.
		void appendBoundedEvent(std::vector<StationSystemsEvent>& events,
			uint64_t tick, std::string message)
		{
			while (events.size() >= EventLimit) events.erase(events.begin());
			events.push_back({tick, std::move(message)});
		}
		// Borrow the Lua error only long enough to copy a bounded string. Never
		// coerce arbitrary error objects outside a protected allocation boundary.
		std::string errorMessage(lua_State* state, char const* fallback)
		{
			if (lua_type(state, -1) != LUA_TSTRING) return fallback;
			size_t length = 0;
			auto const* text = lua_tolstring(state, -1, &length);
			return std::string(text, std::min(length, DiagnosticByteLimit));
		}
		struct ScriptObject
		{
			script::ScratchBudget budget{512 * 1024, 100'000};
			script::ModuleLoader loader;
			std::unique_ptr<lua_State, script::StateCloser> state;
			int reference{LUA_NOREF};
			ScriptObject() : state(lua_newstate(script::budgetedAllocate, &budget))
			{
				if (!state) throw SerializationException("Cannot allocate Station systems sandbox");
				lua_atpanic(state.get(), script::luaPanic);
				std::string diagnostic;
				if (script::runScratchSetup(state.get(), &loader, nullptr, diagnostic) != LUA_OK)
					throw SerializationException(diagnostic);
				lua_settop(state.get(), 0);
			}
		};
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
			std::vector<SabatierReactorSettings> const* sabatierReactors;
			std::vector<MethaneProcessorSettings> const* methaneProcessors;
			std::vector<MicrobialProcessorSettings> const* microbialProcessors;
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
					lua_pushnumber(state, transfer.ratePerSecond); lua_setfield(state, -2, "rate");
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
					lua_pushstring(state, loop.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_createtable(state, 0, 9);
					lua_pushstring(state, chiller.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, chiller.thermalClass.c_str()); lua_setfield(state, -2, "thermalClass");
					lua_pushstring(state, chiller.coldLoopId.c_str()); lua_setfield(state, -2, "coldLoop");
					lua_pushstring(state, chiller.condenserReservoirId.c_str()); lua_setfield(state, -2, "condenser");
					lua_pushnumber(state, chiller.ratedCoolingCapacityKW); lua_setfield(state, -2, "ratedCoolingCapacityKW");
					lua_pushnumber(state, chiller.cop); lua_setfield(state, -2, "cop");
					lua_pushnumber(state, chiller.auxiliaryPowerKW); lua_setfield(state, -2, "auxiliaryPowerKW");
					lua_pushnumber(state, chiller.chilledSetpointC); lua_setfield(state, -2, "chilledSetpointC");
					lua_pushnumber(state, chiller.maxCondensingTemperatureC); lua_setfield(state, -2, "maxCondensingTemperatureC");
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
					lua_createtable(state, 0, 20);
					lua_pushstring(state, atmosphere.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, atmosphere.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_pushstring(state, crew.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
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
					lua_pushstring(state, collector.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_createtable(state, 0, 11);
					lua_pushstring(state, treatment.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, treatment.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
					lua_pushstring(state, treatment.sourceStoreId.c_str()); lua_setfield(state, -2, "sourceStore");
					lua_pushstring(state, treatment.treatedStoreId.c_str()); lua_setfield(state, -2, "treatedStore");
					lua_pushstring(state, treatment.concentrateStoreId.c_str()); lua_setfield(state, -2, "concentrateStore");
					lua_pushstring(state, treatment.consumableStoreId.c_str()); lua_setfield(state, -2, "consumableStore");
					lua_pushnumber(state, treatment.throughputKgPerS); lua_setfield(state, -2, "throughputKgPerS");
					lua_pushnumber(state, treatment.recoveryRatio); lua_setfield(state, -2, "recoveryRatio");
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
					lua_pushstring(state, electrolyzer.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_pushstring(state, capture.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_pushstring(state, cell.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_pushstring(state, reactor.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_pushstring(state, processor.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
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
					lua_createtable(state, 0, 27);
					lua_pushstring(state, processor.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, processor.hazardClass.c_str()); lua_setfield(state, -2, "hazardClass");
					lua_pushstring(state, processor.feedStoreId.c_str()); lua_setfield(state, -2, "feedStore");
					lua_pushstring(state, processor.oxygenStoreId.c_str()); lua_setfield(state, -2, "oxygenStore");
					lua_pushstring(state, processor.waterStoreId.c_str()); lua_setfield(state, -2, "waterStore");
					lua_pushstring(state, processor.consumableStoreId.c_str()); lua_setfield(state, -2, "consumableStore");
					lua_pushstring(state, processor.nutrientStoreId.c_str()); lua_setfield(state, -2, "nutrientStore");
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
					lua_pushinteger(state, processor.priority); lua_setfield(state, -2, "priority");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_call(state, 26, 1);
				if (!lua_istable(state, -1)) return luaL_error(state, "Station factory must return an object");
				object->reference = luaL_ref(state, LUA_REGISTRYINDEX);
				return 0;
			}
		};
		struct Step
		{
			ScriptObject* object;
			uint64_t tick;
			float seconds;
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
		std::string declaredUnitsList()
		{
			std::string list;
			for (auto const* unit : StationStoreUnits)
			{
				if (!list.empty()) list += ", ";
				list += unit;
			}
			return list;
		}
		std::string declaredThermalClasses()
		{
			std::string list;
			for (auto const* thermalClass : StationThermalServiceClasses)
			{
				if (!list.empty()) list += ", ";
				list += thermalClass;
			}
			return list;
		}
		// Compound Resource materials are mass-denominated (#391); their
		// constituents are accounted in kg and the energy term in kJ.
		bool isMassUnit(std::string const& unit)
		{
			return unit == "g" || unit == "kg";
		}
		double massToKg(std::string const& unit, double mass)
		{
			return unit == "g" ? mass * 0.001 : mass;
		}
		// Inverse of massToKg: a magnitude expressed in kilograms rendered
		// in a store's own declared mass unit (#402, where the recovery
		// rates are authored in kg/s against stores that may be grams).
		double kgToMass(std::string const& unit, double kg)
		{
			return unit == "g" ? kg * 1000.0 : kg;
		}
		bool compositionsClose(ResourceCompositionSettings const& left,
			ResourceCompositionSettings const& right)
		{
			auto const close = [](double a, double b)
			{ return std::abs(a - b) <= StationBalanceTolerance; };
			return close(left.moisture, right.moisture)
				&& close(left.nutrientNitrogen, right.nutrientNitrogen)
				&& close(left.atmosphericNitrogen, right.atmosphericNitrogen)
				&& close(left.phosphorus, right.phosphorus)
				&& close(left.potassium, right.potassium)
				&& close(left.otherMinerals, right.otherMinerals)
				&& close(left.energyKJPerKgDry, right.energyKJPerKgDry);
		}
		using ConstituentLedger = std::map<std::string, double>;
		// Add the constituent amounts of a compound wet mass to a ledger.
		// The seven mass constituents partition the wet mass exactly: water
		// is the moisture term and dryOrganics is the dry-mass balance, so
		// constituent accounting closes over the whole inventory. Energy is
		// the declared chemical energy term, in kJ.
		void addComposition(ConstituentLedger& ledger,
			ResourceCompositionSettings const& composition, double wetMassKg, double sign)
		{
			auto const dry = wetMassKg * (1.0 - composition.moisture);
			ledger["water"] += sign * wetMassKg * composition.moisture;
			ledger["nutrientNitrogen"] += sign * dry * composition.nutrientNitrogen;
			ledger["atmosphericNitrogen"] += sign * dry * composition.atmosphericNitrogen;
			ledger["phosphorus"] += sign * dry * composition.phosphorus;
			ledger["potassium"] += sign * dry * composition.potassium;
			ledger["otherMinerals"] += sign * dry * composition.otherMinerals;
			ledger["dryOrganics"] += sign * dry * (1.0 - (composition.nutrientNitrogen
				+ composition.atmosphericNitrogen + composition.phosphorus
				+ composition.potassium + composition.otherMinerals));
			ledger["energy"] += sign * dry * composition.energyKJPerKgDry;
		}
	}
	bool validateStationProfile(ProfileSettings const& profile, std::string const& where,
		std::string& diagnostic)
	{
		auto refuse = [&](std::string text) { diagnostic = where + ": " + std::move(text); return false; };
		if (!std::isfinite(profile.periodSeconds) || profile.periodSeconds < 0.0)
			return refuse("period must be finite and non-negative");
		if (profile.periodSeconds == 0.0)
		{
			if (!profile.segments.empty())
				return refuse("a constant profile must not declare segments");
			if (!std::isfinite(profile.constantLevel)
				|| profile.constantLevel < 0.0 || profile.constantLevel > 1.0)
				return refuse("constant level must be finite and from 0 to 1");
			return true;
		}
		if (profile.segments.empty() || profile.segments.size() > MaxStationProfileSegments)
			return refuse("a periodic profile needs between 1 and "
				+ std::to_string(MaxStationProfileSegments) + " segments");
		if (profile.segments.front().startSeconds != 0.0)
			return refuse("the first segment must start at 0");
		for (size_t index = 0; index < profile.segments.size(); ++index)
		{
			auto const& segment = profile.segments[index];
			if (!std::isfinite(segment.startSeconds) || segment.startSeconds < 0.0
				|| segment.startSeconds >= profile.periodSeconds)
				return refuse("segment start must be finite and inside the period");
			if (!std::isfinite(segment.level) || segment.level < 0.0 || segment.level > 1.0)
				return refuse("segment level must be finite and from 0 to 1");
			if (index > 0 && segment.startSeconds <= profile.segments[index - 1].startSeconds)
				return refuse("segments must start in strictly increasing order");
		}
		return true;
	}
	double evaluateStationProfile(ProfileSettings const& profile, double elapsedSeconds)
	{
		if (profile.periodSeconds <= 0.0) return profile.constantLevel;
		auto const phase = std::fmod(std::max(0.0, elapsedSeconds), profile.periodSeconds);
		double level = profile.segments.front().level;
		for (auto const& segment : profile.segments)
		{
			if (segment.startSeconds > phase) break;
			level = segment.level;
		}
		return level;
	}
	double stationAtmospherePartialPressureKPa(double massKg,
		double molarMassKgPerMol, double volumeM3, double temperatureC)
	{
		// p = (m / M) x R x T / V, converted from Pa to kPa. The ideal
		// gas assumption is the documented model: every compartment
		// pressure derives from this one formula, so an analytic mixture
		// check can verify it directly.
		return massKg / molarMassKgPerMol * StationUniversalGasConstantJPerMolK
			* (temperatureC + StationCelsiusOffset) / volumeM3 / 1000.0;
	}
	double stationSaturationVapourPressureKPa(double temperatureC)
	{
		// Magnus form over liquid water: the documented vapour-capacity
		// assumption for compartment humidity: p_sat = 0.61094 x
		// exp(17.625 t / (t + 243.04)). The expression is singular at
		// -243.04 C and its exponent overflows to infinity below that
		// point, so the documented supported domain is strictly above
		// the singular temperature. At or below it the model carries a
		// finite valid continuation instead of the raw expression: air
		// that cold holds no vapour at all, which is exactly where the
		// in-domain curve is already heading, so the continuation is a
		// vapour capacity of zero rather than an overflow published as
		// a shortage (#565).
		if (temperatureC <= -243.04) return 0.0;
		return 0.61094 * std::exp(17.625 * temperatureC / (temperatureC + 243.04));
	}
	// One body exchanging heat with a shared reservoir through a passive
	// conductance. The weight is the implicit step's coupling coefficient:
	// the heat this body gains is its weight times the gap between its own
	// tick-start temperature and the coupled end-of-tick reservoir
	// temperature the solver computes for the whole star.
	struct PassiveExchangeLeaf
	{
		double capacityKJPerK{0.0};
		double conductanceKWPerK{0.0};
		double temperatureC{0.0};
		double weightKJPerK{0.0};
	};
	// Integrate one reservoir's whole exchange star over the tick
	// implicitly and return the reservoir's coupled end-of-tick
	// temperature, filling each leaf's weight.
	//
	// A passive conductance is solved against the temperature difference
	// *during* the tick, not the one it started with. Solving every edge
	// against the reservoir's end-of-tick temperature rather than its
	// tick-start temperature is what makes all the leaves sharing a
	// reservoir compete for the same finite heat instead of each draining
	// a temperature none of them ever sees move. Eliminating the leaves
	// from that implicit system leaves a closed form: with coupling
	// a_i = dt*G_i/C_i and weight w_i = dt*G_i/(1 + a_i), the reservoir
	// ends at the weighted average of the tick-start temperatures, and
	// each leaf's heat is w_i x (that average - its own temperature).
	//
	// Every weight is non-negative and each new temperature is a weighted
	// average of temperatures that already existed, so the exchange can
	// approach the coupled equilibrium as closely as the conductance
	// allows but can never cross it: no reversing a hot and a cold body,
	// no amplifying their separation, and nothing driven below absolute
	// zero by a stiff conductance. The heat a leaf gains is exactly the
	// negation of what the reservoir gives up, so both ledgers close on
	// the same number rather than on two independently rounded ones.
	double solvePassiveExchange(std::vector<PassiveExchangeLeaf>& leaves,
		double reservoirCapacityKJPerK, double reservoirTemperatureC, double dt)
	{
		double weightedTemperatureKJ{0.0};
		double weightKJPerK{0.0};
		for (auto& leaf : leaves)
		{
			leaf.weightKJPerK = 0.0;
			// A dead conductance or a body with no heat capacity exchanges
			// nothing, and contributes nothing to the coupled temperature.
			if (leaf.conductanceKWPerK <= 0.0 || leaf.capacityKJPerK <= 0.0)
				continue;
			auto const coupling = dt * leaf.conductanceKWPerK / leaf.capacityKJPerK;
			leaf.weightKJPerK = dt * leaf.conductanceKWPerK / (1.0 + coupling);
			weightedTemperatureKJ += leaf.weightKJPerK * leaf.temperatureC;
			weightKJPerK += leaf.weightKJPerK;
		}
		// The reservoir's own capacity is always positive, so the weighted
		// average is always defined; with no live leaves it is just the
		// reservoir's own starting temperature and nothing moves.
		return (reservoirCapacityKJPerK * reservoirTemperatureC + weightedTemperatureKJ)
			/ (reservoirCapacityKJPerK + weightKJPerK);
	}
	// The heat a leaf exchanges over the tick once the coupled end-of-tick
	// reservoir temperature is known. Kept as its own expression so the
	// host and the sandbox book the identical arithmetic.
	double passiveExchangeHeatKJ(PassiveExchangeLeaf const& leaf, double coupledReservoirTemperatureC)
	{
		return leaf.weightKJPerK * (coupledReservoirTemperatureC - leaf.temperatureC);
	}
	double stationSaturatedVapourMassKg(double saturationKPa,
		double volumeM3, double temperatureC)
	{
		// The inverse of the ideal-gas partial pressure at saturation:
		// the water vapour mass a compartment of this volume holds at
		// this temperature before the air cannot hold more.
		return saturationKPa * 1000.0 * volumeM3 * StationMolarMassWaterKgPerMol
			/ (StationUniversalGasConstantJPerMolK * (temperatureC + StationCelsiusOffset));
	}
	// The compartment's temperature and its water vapour inventory are
	// one coupled state (#564): the saturation the air can hold depends
	// on the temperature, and the condensation that constraint forces
	// releases latent heat back into that same temperature. Integrating
	// the two in sequence - condensing against the tick-start
	// temperature, then moving the temperature - leaves the gas
	// supersaturated by however far the temperature fell during the
	// tick, a phase change forever one tick behind its own constraint.
	// Solving them together removes the lag. With C the compartment heat
	// capacity, T0 its tick-start temperature, Q the tick's constant
	// external heat rate (occupant sensible heat plus the exchanger
	// exchange), L the latent heat of vaporization and m0 the vapour
	// mass entering the solve, the end-of-tick equilibrium is either
	//   unsaturated - no phase change, T = T0 + Q*dt/C, or
	//   saturated   - the condensate x satisfies the fixed point
	//       x = m0 - m_sat(T0 + (Q*dt + L*x)/C),
	// where the latent term is the feedback: condensing warms the air,
	// which raises the saturation capacity, which condenses less. The
	// residual F(x) = m0 - m_sat(T0 + (Q*dt + L*x)/C) - x is strictly
	// decreasing - m_sat never decreases with temperature and the
	// temperature grows with x, and it is strictly decreasing in x
	// wherever the saturation is flat because of the explicit -x term -
	// so a fixed-count bisection lands on the unique root
	// deterministically, and the published condensate is the bracket
	// end where F <= 0: the remaining vapour never exceeds what the
	// temperature that condensation itself produced can hold. The
	// temperature is then booked from the condensate,
	// C*(T - T0) = Q*dt + L*x, so the thermal ledger closes over
	// exactly the latent heat that left the gas phase rather than a
	// clamped display value hiding an unsatisfied constraint.
	struct VapourThermalCoupling
	{
		double temperatureC{0.0};
		double condensedKg{0.0};
	};
	VapourThermalCoupling solveVapourThermalCoupling(double capacityKJPerK,
		double startTemperatureC, double externalHeatKW, double vapourKg,
		double volumeM3, double dt)
	{
		auto const saturatedMassAt = [volumeM3](double temperatureC)
		{
			return stationSaturatedVapourMassKg(
				stationSaturationVapourPressureKPa(temperatureC), volumeM3, temperatureC);
		};
		auto const freeTemperatureC = startTemperatureC + externalHeatKW * dt / capacityKJPerK;
		// Air that still holds no more than saturation at the end of a
		// phase-change-free tick never reaches the phase boundary: with a
		// constant heat rate the temperature is monotone across the
		// tick, so the end is its tightest point.
		if (!(vapourKg > saturatedMassAt(freeTemperatureC)))
			return {freeTemperatureC, 0.0};
		// Saturated branch: bisect the condensate fixed point on [0, m0].
		// F(0) > 0 because the unsaturated branch just failed, and
		// F(m0) = -m_sat(...) <= 0, so the root is bracketed.
		auto low = 0.0;
		auto high = vapourKg;
		for (int iteration = 0; iteration < 100; ++iteration)
		{
			auto const mid = 0.5 * (low + high);
			auto const midTemperatureC = freeTemperatureC
				+ StationLatentHeatVaporizationKJPerKg * mid / capacityKJPerK;
			if (vapourKg - saturatedMassAt(midTemperatureC) - mid > 0.0) low = mid;
			else high = mid;
		}
		// `high` is the bracket end where F <= 0: condensing it leaves
		// the vapour at or below the saturation of the temperature the
		// condensation itself produced.
		auto const condensedKg = high;
		return {freeTemperatureC
			+ StationLatentHeatVaporizationKJPerKg * condensedKg / capacityKJPerK,
			condensedKg};
	}
	uint64_t stationScheduleOccurrencesDue(double firstOccurrenceSeconds,
		double recurrenceSeconds, double elapsedSeconds)
	{
		// Nothing is due before the first arrival. The tolerance covers the
		// rounding between an authored second and the fixed-tick grid it is
		// measured against, so an occurrence scheduled exactly on a tick
		// boundary is never missed.
		if (elapsedSeconds + StationScheduleTolerance < firstOccurrenceSeconds) return 0;
		// A zero recurrence is a one-off: exactly one occurrence, ever.
		if (recurrenceSeconds <= 0.0) return 1;
		auto const steps = std::floor((elapsedSeconds + StationScheduleTolerance
			- firstOccurrenceSeconds) / recurrenceSeconds);
		if (!(steps > 0.0)) return 1;
		return static_cast<uint64_t>(steps) + 1;
	}
	bool validateStationSystemsSettings(StationSystemsSettings const& settings, std::string& diagnostic)
	{
		auto refuse = [&](std::string text) { diagnostic = std::move(text); return false; };
		if (!std::isfinite(settings.observationIntervalSeconds)
			|| settings.observationIntervalSeconds < 0.1f || settings.observationIntervalSeconds > 60.0f)
			return refuse("Station observation interval must be finite, from 0.1 to 60 seconds");
		if (settings.stores.size() > MaxStationResourceStores)
			return refuse("Station resource stores are limited to "
				+ std::to_string(MaxStationResourceStores) + " entries");
		std::set<std::string> ids;
		for (auto const& store : settings.stores)
		{
			auto const where = "Station resource store '" + store.id + "'";
			if (store.id.empty()) return refuse("Station resource store id must not be empty");
			if (store.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource store id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!ids.insert(store.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationStoreUnits.begin(), StationStoreUnits.end(), store.unit)
				== StationStoreUnits.end())
				return refuse(where + " has unknown unit '" + store.unit
					+ "'; declared units are: " + declaredUnitsList());
			if (!std::isfinite(store.capacity) || store.capacity <= 0.0)
				return refuse(where + " capacity must be finite and positive");
			if (!std::isfinite(store.startingQualified) || !std::isfinite(store.startingUnqualified))
				return refuse(where + " starting inventory must be finite");
			if (store.startingQualified < 0.0 || store.startingUnqualified < 0.0)
				return refuse(where + " starting inventory must not be negative");
			if (store.startingQualified + store.startingUnqualified > store.capacity)
				return refuse(where + " starting inventory exceeds capacity");
			if (!std::isfinite(store.protectedReserve) || store.protectedReserve < 0.0)
				return refuse(where + " protected reserve must be finite and non-negative");
			if (store.protectedReserve > store.startingQualified)
				return refuse(where + " protected reserve exceeds starting usable inventory");
			if (store.composition)
			{
				auto const& composition = *store.composition;
				if (!isMassUnit(store.unit))
					return refuse(where + " declares a composition but unit '" + store.unit
						+ "' is not a mass unit; compound materials are mass-denominated (g or kg)");
				if (!std::isfinite(composition.moisture)
					|| composition.moisture < 0.0 || composition.moisture >= 1.0)
					return refuse(where + " moisture fraction must be finite and from 0 up to but below 1");
				auto const dryFractions = {composition.nutrientNitrogen,
					composition.atmosphericNitrogen, composition.phosphorus,
					composition.potassium, composition.otherMinerals};
				for (auto const fraction : dryFractions)
					if (!std::isfinite(fraction) || fraction < 0.0)
						return refuse(where + " dry-mass fractions must all be finite and non-negative");
				auto const drySum = composition.nutrientNitrogen + composition.atmosphericNitrogen
					+ composition.phosphorus + composition.potassium + composition.otherMinerals;
				if (drySum > 1.0)
					return refuse(where + " dry-mass fractions sum to " + std::to_string(drySum)
						+ ", exceeding the available dry mass");
				if (!std::isfinite(composition.energyKJPerKgDry) || composition.energyKJPerKgDry < 0.0)
					return refuse(where + " energy term must be finite and non-negative");
			}
		}
		if (settings.transfers.size() > MaxStationResourceTransfers)
			return refuse("Station resource transfers are limited to "
				+ std::to_string(MaxStationResourceTransfers) + " entries");
		std::set<std::string> transferIds;
		for (auto const& transfer : settings.transfers)
		{
			auto const where = "Station resource transfer '" + transfer.id + "'";
			if (transfer.id.empty()) return refuse("Station resource transfer id must not be empty");
			if (transfer.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource transfer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!transferIds.insert(transfer.id).second) return refuse(where + " id is duplicated");
			if (transfer.sourceId == transfer.destinationId)
				return refuse(where + " source and destination must differ");
			auto const findStore = [&](std::string const& id) -> ResourceStoreSettings const*
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == id; });
				return it == settings.stores.end() ? nullptr : &*it;
			};
			auto const* source = findStore(transfer.sourceId);
			auto const* destination = findStore(transfer.destinationId);
			if (!source) return refuse(where + " references unknown source store '" + transfer.sourceId + "'");
			if (!destination)
				return refuse(where + " references unknown destination store '" + transfer.destinationId + "'");
			// Same-unit endpoints only: cross-unit movement is conversion, which
			// later slices introduce with explicit composition accounting.
			if (transfer.unit != source->unit || transfer.unit != destination->unit)
				return refuse(where + " unit '" + transfer.unit
					+ "' does not match both stores ('" + source->unit + "' -> '" + destination->unit + "')");
			if (!std::isfinite(transfer.ratePerSecond) || transfer.ratePerSecond <= 0.0)
				return refuse(where + " rate must be finite and positive");
			// Compound material only moves between identical declared
			// compositions. Crossing a composition boundary — including bound
			// nutrient nitrogen versus atmospheric nitrogen, which are never
			// freely interchangeable — is conversion and requires an
			// explicitly declared, constituent-balanced conversion process.
			if (source->composition || destination->composition)
			{
				if (!source->composition || !destination->composition
					|| !compositionsClose(*source->composition, *destination->composition))
					return refuse(where + " crosses a compound-material boundary; transfers between stores with different declared compositions require an explicit conversion");
			}
		}
		if (settings.conversions.size() > MaxStationResourceConversions)
			return refuse("Station resource conversions are limited to "
				+ std::to_string(MaxStationResourceConversions) + " entries");
		std::set<std::string> conversionIds;
		for (auto const& conversion : settings.conversions)
		{
			auto const where = "Station resource conversion '" + conversion.id + "'";
			if (conversion.id.empty()) return refuse("Station resource conversion id must not be empty");
			if (conversion.id.size() > MaxStationStoreIdLength)
				return refuse("Station resource conversion id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!conversionIds.insert(conversion.id).second) return refuse(where + " id is duplicated");
			if (conversion.inputs.empty() || conversion.inputs.size() > MaxStationConversionFlows)
				return refuse(where + " needs between 1 and "
					+ std::to_string(MaxStationConversionFlows) + " inputs");
			if (conversion.outputs.empty() || conversion.outputs.size() > MaxStationConversionFlows)
				return refuse(where + " needs between 1 and "
					+ std::to_string(MaxStationConversionFlows) + " outputs");
			std::set<std::string> endpoints;
			ConstituentLedger inflow, outflow;
			auto const addFlow = [&](std::string const& storeId, double rate, bool input)
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == storeId; });
				if (it == settings.stores.end())
					return refuse(where + " references unknown " + (input ? "input" : "output")
						+ " store '" + storeId + "'");
				if (!endpoints.insert(storeId).second)
					return refuse(where + " uses store '" + storeId + "' more than once");
				if (!it->composition)
					return refuse(where + " " + (input ? "input" : "output") + " store '" + storeId
						+ "' has no declared composition; conversions require compound stores");
				if (!std::isfinite(rate) || rate <= 0.0)
					return refuse(where + " " + (input ? "input rate" : "output yield")
						+ " for store '" + storeId + "' must be finite and positive");
				addComposition(input ? inflow : outflow, *it->composition,
					massToKg(it->unit, rate), 1.0);
				return true;
			};
			for (auto const& flow : conversion.inputs)
				if (!addFlow(flow.storeId, flow.ratePerSecond, true)) return false;
			for (auto const& flow : conversion.outputs)
				if (!addFlow(flow.storeId, flow.yieldPerSecond, false)) return false;
			// Constituent and energy balance at declared full throughput:
			// an impossible configured yield that would create a tracked
			// constituent is rejected here rather than allowed to create
			// material at runtime. Losses below the input are legal and are
			// reported as explicit losses by the runtime ledger.
			for (auto const& [constituent, produced] : outflow)
			{
				auto const supplied = inflow.count(constituent) ? inflow.at(constituent) : 0.0;
				if (produced > supplied + StationBalanceTolerance * std::max(1.0, std::abs(supplied)))
					return refuse(where + " would create constituent '" + constituent
						+ "': declared output " + std::to_string(produced)
						+ " exceeds declared input " + std::to_string(supplied));
			}
		}
		if (settings.solarArrays.size() > MaxStationSolarArrays)
			return refuse("Station solar arrays are limited to "
				+ std::to_string(MaxStationSolarArrays) + " entries");
		std::set<std::string> solarIds;
		for (auto const& array : settings.solarArrays)
		{
			auto const where = "Station solar array '" + array.id + "'";
			if (array.id.empty()) return refuse("Station solar array id must not be empty");
			if (array.id.size() > MaxStationStoreIdLength)
				return refuse("Station solar array id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!solarIds.insert(array.id).second) return refuse(where + " id is duplicated");
			// Zero installed capacity is legal and disables the array; it is
			// never mistaken for a source that can carry a load.
			if (!std::isfinite(array.capacityKW) || array.capacityKW < 0.0)
				return refuse(where + " installed capacity must be finite and non-negative (0 disables the array)");
			if (!validateStationProfile(array.illumination, where + " illumination profile", diagnostic))
				return false;
		}
		if (settings.equipmentLoads.size() > MaxStationEquipmentLoads)
			return refuse("Station external equipment loads are limited to "
				+ std::to_string(MaxStationEquipmentLoads) + " entries");
		std::set<std::string> loadIds;
		for (auto const& load : settings.equipmentLoads)
		{
			auto const where = "Station equipment load '" + load.id + "'";
			if (load.id.empty()) return refuse("Station equipment load id must not be empty");
			if (load.id.size() > MaxStationStoreIdLength)
				return refuse("Station equipment load id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!loadIds.insert(load.id).second) return refuse(where + " id is duplicated");
			if (!std::isfinite(load.electricalDemandKW) || load.electricalDemandKW < 0.0)
				return refuse(where + " electrical demand must be finite and non-negative");
			if (!std::isfinite(load.heatOutputKW) || load.heatOutputKW < 0.0)
				return refuse(where + " heat output must be finite and non-negative");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				load.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + load.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			if (load.coolingDisposition != StationCoolingDisposition::Exported
				&& load.coolingDisposition != StationCoolingDisposition::CoolingService)
				return refuse(where + " has an unknown cooling disposition");
			if (!std::isfinite(load.criticalFloorKW) || load.criticalFloorKW < 0.0)
				return refuse(where + " critical floor must be finite and non-negative");
			if (load.criticalFloorKW > load.electricalDemandKW)
				return refuse(where + " critical floor exceeds the declared electrical demand");
			if (!std::isfinite(load.maxCurtailment) || load.maxCurtailment < 0.0 || load.maxCurtailment > 1.0)
				return refuse(where + " allowed curtailment must be finite and from 0 to 1");
			if (!validateStationProfile(load.operatingProfile, where + " operating profile", diagnostic))
				return false;
			if (load.materialExchanges.size() > MaxStationLoadMaterialExchanges)
				return refuse(where + " material exchanges are limited to "
					+ std::to_string(MaxStationLoadMaterialExchanges) + " entries");
			std::set<std::string> exchangeStores;
			for (auto const& exchange : load.materialExchanges)
			{
				auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
						[&](ResourceStoreSettings const& store) { return store.id == exchange.storeId; });
				if (it == settings.stores.end())
					return refuse(where + " material exchange references unknown store '" + exchange.storeId + "'");
				if (!exchangeStores.insert(exchange.storeId).second)
					return refuse(where + " exchanges with store '" + exchange.storeId + "' more than once");
				if (!std::isfinite(exchange.ratePerSecond) || exchange.ratePerSecond <= 0.0)
					return refuse(where + " material exchange rate for store '" + exchange.storeId
						+ "' must be finite and positive");
			}
			// Heat sink routing (#395): emitted heat goes to exactly one
			// declared reservoir, of the same thermal service class. Spare
			// capacity in another class is never usable, and a
			// cooling-service load needs a cooling service rather than a
			// passive sink.
			if (!load.heatSinkId.empty())
			{
				if (load.coolingDisposition != StationCoolingDisposition::Exported)
					return refuse(where + " declares a heat sink but requires a cooling service; a cooling-service load needs a cooling service of its class, not a heat sink");
				auto const sink = std::find_if(settings.thermalReservoirs.begin(),
					settings.thermalReservoirs.end(),
					[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == load.heatSinkId; });
				if (sink == settings.thermalReservoirs.end())
					return refuse(where + " heat sink references unknown thermal reservoir '" + load.heatSinkId + "'");
				if (sink->thermalClass != load.thermalClass)
					return refuse(where + " thermal class '" + load.thermalClass
						+ "' is incompatible with heat sink reservoir '" + sink->id + "' of class '"
						+ sink->thermalClass + "'; heat routes only to the same service class");
			}
		}
		if (settings.batteries.size() > MaxStationBatteries)
			return refuse("Station batteries are limited to "
				+ std::to_string(MaxStationBatteries) + " entries");
		std::set<std::string> batteryIds;
		for (auto const& battery : settings.batteries)
		{
			auto const where = "Station battery '" + battery.id + "'";
			if (battery.id.empty()) return refuse("Station battery id must not be empty");
			if (battery.id.size() > MaxStationStoreIdLength)
				return refuse("Station battery id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!batteryIds.insert(battery.id).second) return refuse(where + " id is duplicated");
			if (!std::isfinite(battery.nameplateKWh) || battery.nameplateKWh < 0.0)
				return refuse(where + " nameplate energy must be finite and non-negative");
			// Usable energy is the cyclable slice above the reserve floor; it
			// can never exceed the installed nameplate, so no policy can
			// invent storage the battery does not have.
			if (!std::isfinite(battery.usableKWh) || battery.usableKWh < 0.0)
				return refuse(where + " usable energy must be finite and non-negative");
			if (battery.usableKWh > battery.nameplateKWh)
				return refuse(where + " usable energy exceeds the nameplate energy");
			if (!std::isfinite(battery.startingStoredKWh) || battery.startingStoredKWh < 0.0)
				return refuse(where + " starting stored energy must be finite and non-negative");
			if (battery.startingStoredKWh > battery.nameplateKWh)
				return refuse(where + " starting stored energy exceeds the nameplate energy");
			if (!std::isfinite(battery.maxChargeKW) || battery.maxChargeKW < 0.0)
				return refuse(where + " maximum charge power must be finite and non-negative");
			if (!std::isfinite(battery.maxDischargeKW) || battery.maxDischargeKW < 0.0)
				return refuse(where + " maximum discharge power must be finite and non-negative");
			if (!std::isfinite(battery.chargeEfficiency)
				|| battery.chargeEfficiency <= 0.0 || battery.chargeEfficiency > 1.0)
				return refuse(where + " charge efficiency must be finite and greater than 0 up to 1");
			if (!std::isfinite(battery.dischargeEfficiency)
				|| battery.dischargeEfficiency <= 0.0 || battery.dischargeEfficiency > 1.0)
				return refuse(where + " discharge efficiency must be finite and greater than 0 up to 1");
		}
		if (settings.thermalReservoirs.size() > MaxStationThermalReservoirs)
			return refuse("Station thermal reservoirs are limited to "
				+ std::to_string(MaxStationThermalReservoirs) + " entries");
		std::set<std::string> reservoirIds;
		for (auto const& reservoir : settings.thermalReservoirs)
		{
			auto const where = "Station thermal reservoir '" + reservoir.id + "'";
			if (reservoir.id.empty()) return refuse("Station thermal reservoir id must not be empty");
			if (reservoir.id.size() > MaxStationStoreIdLength)
				return refuse("Station thermal reservoir id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!reservoirIds.insert(reservoir.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				reservoir.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + reservoir.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			if (!std::isfinite(reservoir.heatCapacityKJPerK) || reservoir.heatCapacityKJPerK <= 0.0)
				return refuse(where + " effective thermal capacity must be finite and positive");
			if (!std::isfinite(reservoir.minTemperatureC) || !std::isfinite(reservoir.maxTemperatureC)
				|| reservoir.minTemperatureC >= reservoir.maxTemperatureC)
				return refuse(where + " temperature limits must be finite with min below max");
			if (!std::isfinite(reservoir.startingTemperatureC))
				return refuse(where + " starting temperature must be finite");
			// Temperatures below absolute zero describe no material; refuse
			// them rather than let the integration wander into nonsense.
			if (reservoir.minTemperatureC <= -StationCelsiusOffset
				|| reservoir.maxTemperatureC <= -StationCelsiusOffset
				|| reservoir.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " temperatures must be above absolute zero (-273.15 C)");
			if (reservoir.startingTemperatureC < reservoir.minTemperatureC
				|| reservoir.startingTemperatureC > reservoir.maxTemperatureC)
				return refuse(where + " starting temperature is outside its declared limits");
			if (!std::isfinite(reservoir.absorbedHeatKW) || reservoir.absorbedHeatKW < 0.0)
				return refuse(where + " absorbed heat must be finite and non-negative");
		}
		if (settings.radiators.size() > MaxStationRadiators)
			return refuse("Station radiators are limited to "
				+ std::to_string(MaxStationRadiators) + " entries");
		std::set<std::string> radiatorIds;
		for (auto const& radiator : settings.radiators)
		{
			auto const where = "Station radiator '" + radiator.id + "'";
			if (radiator.id.empty()) return refuse("Station radiator id must not be empty");
			if (radiator.id.size() > MaxStationStoreIdLength)
				return refuse("Station radiator id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!radiatorIds.insert(radiator.id).second) return refuse(where + " id is duplicated");
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == radiator.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return refuse(where + " is attached to unknown thermal reservoir '" + radiator.reservoirId + "'");
			if (!std::isfinite(radiator.areaM2) || radiator.areaM2 <= 0.0)
				return refuse(where + " area must be finite and positive");
			if (!std::isfinite(radiator.emissivity) || radiator.emissivity <= 0.0 || radiator.emissivity > 1.0)
				return refuse(where + " emissivity must be finite and greater than 0 up to 1");
			if (!std::isfinite(radiator.viewFactor) || radiator.viewFactor <= 0.0 || radiator.viewFactor > 1.0)
				return refuse(where + " view factor must be finite and greater than 0 up to 1");
			if (!std::isfinite(radiator.sinkTemperatureC)
				|| radiator.sinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " sink temperature must be finite and above absolute zero (-273.15 C)");
		}
		if (settings.coolantLoops.size() > MaxStationCoolantLoops)
			return refuse("Station coolant loops are limited to "
				+ std::to_string(MaxStationCoolantLoops) + " entries");
		std::set<std::string> loopIds;
		for (auto const& loop : settings.coolantLoops)
		{
			auto const where = "Station coolant loop '" + loop.id + "'";
			if (loop.id.empty()) return refuse("Station coolant loop id must not be empty");
			if (loop.id.size() > MaxStationStoreIdLength)
				return refuse("Station coolant loop id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!loopIds.insert(loop.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationCoolantHazardClasses.begin(), StationCoolantHazardClasses.end(),
				loop.hazardClass) == StationCoolantHazardClasses.end())
				return refuse(where + " has unknown coolant hazard class '" + loop.hazardClass
					+ "'; declared classes are: normal, hazardous");
			// A loop couples heat-only to one declared thermal reservoir.
			// Coolant never touches a Resource store: it is not potable or
			// nutrient water, and the loop's inventory is its own.
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == loop.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return refuse(where + " is coupled to unknown thermal reservoir '" + loop.reservoirId + "'");
			if (!std::isfinite(loop.coolantCapacityKg) || loop.coolantCapacityKg <= 0.0)
				return refuse(where + " coolant capacity must be finite and positive");
			if (!std::isfinite(loop.startingCoolantKg) || loop.startingCoolantKg < 0.0)
				return refuse(where + " starting coolant must be finite and non-negative");
			if (loop.startingCoolantKg > loop.coolantCapacityKg)
				return refuse(where + " starting coolant exceeds the loop capacity");
			if (!std::isfinite(loop.specificHeatKJPerKgK) || loop.specificHeatKJPerKgK <= 0.0)
				return refuse(where + " specific heat must be finite and positive");
			if (!std::isfinite(loop.startingTemperatureC)
				|| loop.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " starting temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(loop.freezeProtectionSetpointC)
				|| loop.freezeProtectionSetpointC <= -StationCelsiusOffset)
				return refuse(where + " freeze-protection setpoint must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(loop.pumpPowerKW) || loop.pumpPowerKW < 0.0)
				return refuse(where + " pump power must be finite and non-negative");
			if (!std::isfinite(loop.ratedFlowKgPerS) || loop.ratedFlowKgPerS < 0.0)
				return refuse(where + " rated flow must be finite and non-negative");
			if (!std::isfinite(loop.exchangerConductanceKWPerK) || loop.exchangerConductanceKWPerK < 0.0)
				return refuse(where + " exchanger conductance must be finite and non-negative");
			if (!std::isfinite(loop.heaterCapacityKW) || loop.heaterCapacityKW < 0.0)
				return refuse(where + " heater capacity must be finite and non-negative");
		}
		// Chiller contracts (#397): the cold-side interface is a declared
		// coolant loop and the hot-side sink a declared thermal reservoir,
		// both of the chiller's own service class — spare capacity in
		// another class is never usable, and a chiller that rejects its
		// condenser heat into the very reservoir its cold side cools is a
		// heater, not a cooling service, so it is refused rather than
		// silently circulating heat in a circle.
		if (settings.chillers.size() > MaxStationChillers)
			return refuse("Station chillers are limited to "
				+ std::to_string(MaxStationChillers) + " entries");
		std::set<std::string> chillerIds;
		std::set<std::string> chilledLoops;
		for (auto const& chiller : settings.chillers)
		{
			auto const where = "Station chiller '" + chiller.id + "'";
			if (chiller.id.empty()) return refuse("Station chiller id must not be empty");
			if (chiller.id.size() > MaxStationStoreIdLength)
				return refuse("Station chiller id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!chillerIds.insert(chiller.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				chiller.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + chiller.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			auto const loop = std::find_if(settings.coolantLoops.begin(),
				settings.coolantLoops.end(),
				[&](CoolantLoopSettings const& candidate) { return candidate.id == chiller.coldLoopId; });
			if (loop == settings.coolantLoops.end())
				return refuse(where + " cold-side interface references unknown coolant loop '"
					+ chiller.coldLoopId + "'");
			if (!chilledLoops.insert(chiller.coldLoopId).second)
				return refuse(where + " shares cold loop '" + chiller.coldLoopId
					+ "' with another chiller; one evaporator serves one chiller");
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == chiller.condenserReservoirId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " condenser sink references unknown thermal reservoir '"
					+ chiller.condenserReservoirId + "'");
			// Class compatibility on both interfaces: the cold loop's host
			// reservoir and the condenser sink must each declare the
			// service class the chiller supplies.
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& candidate) { return candidate.id == loop->reservoirId; });
			if (host->thermalClass != chiller.thermalClass)
				return refuse(where + " thermal class '" + chiller.thermalClass
					+ "' is incompatible with cold loop '" + loop->id + "' serving reservoir '"
					+ host->id + "' of class '" + host->thermalClass
					+ "'; cold services are class-matched");
			if (sink->thermalClass != chiller.thermalClass)
				return refuse(where + " thermal class '" + chiller.thermalClass
					+ "' is incompatible with condenser sink reservoir '" + sink->id
					+ "' of class '" + sink->thermalClass + "'; heat routes only to the same service class");
			if (sink->id == host->id)
				return refuse(where + " rejects its condenser heat into reservoir '" + sink->id
					+ "', the same reservoir its cold side cools; that is a heater, not a cooling service");
			if (!std::isfinite(chiller.ratedCoolingCapacityKW) || chiller.ratedCoolingCapacityKW < 0.0)
				return refuse(where + " rated cooling capacity must be finite and non-negative");
			if (!std::isfinite(chiller.cop) || chiller.cop <= 0.0)
				return refuse(where + " COP must be finite and positive");
			if (!std::isfinite(chiller.auxiliaryPowerKW) || chiller.auxiliaryPowerKW < 0.0)
				return refuse(where + " auxiliary power must be finite and non-negative");
			if (!std::isfinite(chiller.chilledSetpointC)
				|| chiller.chilledSetpointC <= -StationCelsiusOffset)
				return refuse(where + " chilled setpoint must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(chiller.maxCondensingTemperatureC)
				|| chiller.maxCondensingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum condensing temperature must be finite and above absolute zero (-273.15 C)");
			if (chiller.chilledSetpointC >= chiller.maxCondensingTemperatureC)
				return refuse(where + " chilled setpoint must be below the maximum condensing temperature");
		}
		// Thermal buffer contracts (#398): the buffer couples to a declared
		// host reservoir; a circulation-access buffer additionally names a
		// coolant loop coupled to that same host, because the buffer's
		// transfer path runs through the loop's circulation and a gate on
		// a different reservoir would let a blackout on one side absorb
		// through the other. The store is finite and its starting energy
		// sits inside its capacity; the usable temperature band is finite,
		// above absolute zero and properly ordered; the rate limits are
		// finite and non-negative with at least one direction alive; a
		// passive buffer must declare a positive transfer conductance.
		if (settings.thermalBuffers.size() > MaxStationThermalBuffers)
			return refuse("Station thermal buffers are limited to "
				+ std::to_string(MaxStationThermalBuffers) + " entries");
		std::set<std::string> thermalBufferIds;
		for (auto const& buffer : settings.thermalBuffers)
		{
			auto const where = "Station thermal buffer '" + buffer.id + "'";
			if (buffer.id.empty()) return refuse("Station thermal buffer id must not be empty");
			if (buffer.id.size() > MaxStationStoreIdLength)
				return refuse("Station thermal buffer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!thermalBufferIds.insert(buffer.id).second) return refuse(where + " id is duplicated");
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == buffer.reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return refuse(where + " references unknown host thermal reservoir '" + buffer.reservoirId + "'");
			if (std::find(StationThermalBufferAccessModes.begin(),
				StationThermalBufferAccessModes.end(), buffer.accessMode)
				== StationThermalBufferAccessModes.end())
				return refuse(where + " has unknown access mode '" + buffer.accessMode
					+ "'; declared modes are: circulation, passive");
			if (buffer.accessMode == "circulation")
			{
				auto const gate = std::find_if(settings.coolantLoops.begin(),
					settings.coolantLoops.end(),
					[&](CoolantLoopSettings const& loop) { return loop.id == buffer.circulationLoopId; });
				if (gate == settings.coolantLoops.end())
					return refuse(where + " circulation access references unknown coolant loop '"
						+ buffer.circulationLoopId + "'");
				if (gate->reservoirId != buffer.reservoirId)
					return refuse(where + " circulation loop '" + gate->id
						+ "' is coupled to reservoir '" + gate->reservoirId
						+ "', not the buffer's host reservoir '" + buffer.reservoirId + "'");
			}
			else
			{
				if (!std::isfinite(buffer.transferConductanceKWPerK)
					|| buffer.transferConductanceKWPerK <= 0.0)
					return refuse(where + " passive transfer conductance must be finite and positive");
			}
			if (!std::isfinite(buffer.capacityKJ) || buffer.capacityKJ <= 0.0)
				return refuse(where + " capacity must be finite and positive");
			if (!std::isfinite(buffer.startingEnergyKJ) || buffer.startingEnergyKJ < 0.0
				|| buffer.startingEnergyKJ > buffer.capacityKJ)
				return refuse(where + " starting energy must be finite and within capacity");
			if (!std::isfinite(buffer.minUsableTemperatureC)
				|| buffer.minUsableTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum usable temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(buffer.maxUsableTemperatureC)
				|| buffer.maxUsableTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum usable temperature must be finite and above absolute zero (-273.15 C)");
			if (buffer.minUsableTemperatureC >= buffer.maxUsableTemperatureC)
				return refuse(where + " usable temperature band needs the minimum below the maximum");
			if (!std::isfinite(buffer.maxChargeKW) || buffer.maxChargeKW < 0.0)
				return refuse(where + " charge rate must be finite and non-negative");
			if (!std::isfinite(buffer.maxDischargeKW) || buffer.maxDischargeKW < 0.0)
				return refuse(where + " discharge rate must be finite and non-negative");
			if (buffer.maxChargeKW <= 0.0 && buffer.maxDischargeKW <= 0.0)
				return refuse(where + " that can neither charge nor discharge is not a buffer");
		}
		// Generator contracts (#399): the fuel inventory is an energy-unit
		// Resource store, the waste-heat sink a declared thermal reservoir
		// of the generator's own service class - spare capacity in another
		// class is never usable - and the conversion, ramp, startup and
		// shutdown-heat terms are all finite and physically ordered.
		if (settings.generators.size() > MaxStationGenerators)
			return refuse("Station generators are limited to "
				+ std::to_string(MaxStationGenerators) + " entries");
		std::set<std::string> generatorIds;
		for (auto const& generator : settings.generators)
		{
			auto const where = "Station generator '" + generator.id + "'";
			if (generator.id.empty()) return refuse("Station generator id must not be empty");
			if (generator.id.size() > MaxStationStoreIdLength)
				return refuse("Station generator id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!generatorIds.insert(generator.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				generator.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + generator.thermalClass
					+ "'; declared classes are: low, moderate, warm, generation");
			// Fuel is energy the generator can burn: the store must be
			// denominated in an energy unit so the fuel ledger closes in
			// the same energy terms as the electrical output.
			auto const fuel = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == generator.fuelStoreId; });
			if (fuel == settings.stores.end())
				return refuse(where + " fuel inventory references unknown Resource store '"
					+ generator.fuelStoreId + "'");
			if (fuel->unit != "kJ" && fuel->unit != "MJ" && fuel->unit != "Wh" && fuel->unit != "kWh")
				return refuse(where + " fuel store unit '" + fuel->unit
					+ "' is not an energy unit; fuelled generators burn kJ, MJ, Wh or kWh");
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(),
				[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == generator.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink references unknown thermal reservoir '"
					+ generator.heatSinkId + "'");
			if (sink->thermalClass != generator.thermalClass)
				return refuse(where + " thermal class '" + generator.thermalClass
					+ "' is incompatible with heat sink reservoir '" + sink->id
					+ "' of class '" + sink->thermalClass + "'; heat routes only to the same service class");
			// Zero installed capacity is legal and disables the generator;
			// it is never mistaken for a source that can carry a load.
			if (!std::isfinite(generator.ratedOutputKW) || generator.ratedOutputKW < 0.0)
				return refuse(where + " installed capacity must be finite and non-negative (0 disables the generator)");
			if (!std::isfinite(generator.efficiency)
				|| generator.efficiency <= 0.0 || generator.efficiency > 1.0)
				return refuse(where + " conversion efficiency must be finite and greater than 0 up to 1");
			// A generator that cannot change its output at all could never
			// reach any commanded level, so the ramp limit must be positive.
			if (!std::isfinite(generator.maxRampKWPerSecond) || generator.maxRampKWPerSecond <= 0.0)
				return refuse(where + " ramp limit must be finite and positive");
			if (!std::isfinite(generator.commandedOutputKW) || generator.commandedOutputKW < 0.0)
				return refuse(where + " commanded output must be finite and non-negative");
			if (generator.commandedOutputKW > generator.ratedOutputKW)
				return refuse(where + " commanded output exceeds the installed capacity");
			if (!std::isfinite(generator.startupPowerKW) || generator.startupPowerKW < 0.0)
				return refuse(where + " startup power must be finite and non-negative");
			if (!std::isfinite(generator.startupSeconds) || generator.startupSeconds < 0.0)
				return refuse(where + " startup duration must be finite and non-negative");
			if (!std::isfinite(generator.shutdownHeatKW) || generator.shutdownHeatKW < 0.0)
				return refuse(where + " shutdown heat must be finite and non-negative");
			if (!std::isfinite(generator.shutdownSeconds) || generator.shutdownSeconds < 0.0)
				return refuse(where + " shutdown heat duration must be finite and non-negative");
			if (!std::isfinite(generator.maxSinkTemperatureC)
				|| generator.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero (-273.15 C)");
		}
		if (settings.deliveries.size() > MaxStationResourceDeliveries)
			return refuse("Station resupply deliveries are limited to "
				+ std::to_string(MaxStationResourceDeliveries) + " entries");
		std::set<std::string> deliveryIds;
		for (auto const& delivery : settings.deliveries)
		{
			auto const where = "Station resupply delivery '" + delivery.id + "'";
			if (delivery.id.empty()) return refuse("Station resupply delivery id must not be empty");
			if (delivery.id.size() > MaxStationStoreIdLength)
				return refuse("Station resupply delivery id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!deliveryIds.insert(delivery.id).second) return refuse(where + " id is duplicated");
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == delivery.storeId; });
			if (it == settings.stores.end())
				return refuse(where + " targets unknown Resource store '" + delivery.storeId + "'");
			// A delivery brings a store's own material in; it never converts
			// units on the way through the boundary.
			if (delivery.unit != it->unit)
				return refuse(where + " unit '" + delivery.unit
					+ "' does not match destination store unit '" + it->unit + "'");
			// A delivery that brings nothing specifies nothing; refusing it
			// keeps every fired occurrence a real inflow.
			if (!std::isfinite(delivery.quantity) || delivery.quantity <= 0.0)
				return refuse(where + " quantity must be finite and positive");
			if (!std::isfinite(delivery.firstOccurrenceSeconds)
				|| delivery.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			// Zero is a one-off; a recurring schedule must not repeat faster
			// than one World fixed tick, so no tick is ever asked to absorb an
			// unbounded burst of catch-up arrivals.
			if (!std::isfinite(delivery.recurrenceSeconds) || delivery.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (delivery.recurrenceSeconds > 0.0
				&& delivery.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(World::getFixedTimestep()))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(World::getFixedTimestep()) + " seconds)");
		}
		// Terminal-waste ejection schedules (#403): a scenario removal of
		// disposal-ready (qualified) material from one waste store at
		// authored times. The same schedule discipline as resupply
		// deliveries governs them; what makes them different is direction:
		// ejection can only remove what the store actually holds as
		// eligible, so it never needs a capacity-shaped rule, but it must
		// still name a real store in that store's own unit.
		if (settings.wasteEjections.size() > MaxStationWasteEjections)
			return refuse("Station waste ejections are limited to "
				+ std::to_string(MaxStationWasteEjections) + " entries");
		std::set<std::string> ejectionIds;
		for (auto const& ejection : settings.wasteEjections)
		{
			auto const where = "Station waste ejection '" + ejection.id + "'";
			if (ejection.id.empty()) return refuse("Station waste ejection id must not be empty");
			if (ejection.id.size() > MaxStationStoreIdLength)
				return refuse("Station waste ejection id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!ejectionIds.insert(ejection.id).second) return refuse(where + " id is duplicated");
			auto const it = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == ejection.storeId; });
			if (it == settings.stores.end())
				return refuse(where + " targets unknown Resource store '" + ejection.storeId + "'");
			// Ejection carries a store's own material out; it never
			// converts units on the way through the boundary.
			if (ejection.unit != it->unit)
				return refuse(where + " unit '" + ejection.unit
					+ "' does not match target store unit '" + it->unit + "'");
			// An ejection that removes nothing specifies nothing; refusing
			// it keeps every fired occurrence a real outflow.
			if (!std::isfinite(ejection.quantity) || ejection.quantity <= 0.0)
				return refuse(where + " quantity must be finite and positive");
			if (!std::isfinite(ejection.firstOccurrenceSeconds)
				|| ejection.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			// Zero is a one-off; a recurring schedule must not repeat faster
			// than one World fixed tick, so no tick is ever asked to absorb
			// an unbounded burst of catch-up ejections.
			if (!std::isfinite(ejection.recurrenceSeconds) || ejection.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (ejection.recurrenceSeconds > 0.0
				&& ejection.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(World::getFixedTimestep()))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(World::getFixedTimestep()) + " seconds)");
		}
		// Compartment atmosphere contracts (#400): a fixed volume with a
		// finite owned gas inventory, an authored effective thermal
		// capacity, an optional heat-only reservoir coupling and an
		// authored occupancy/load assignment. Compartments are isolated
		// by construction - no process moves gas between them - so the
		// hazard class is a segregation label and every pathway that can
		// remove gas (leak, airlock cycle) must book its mass.
		if (settings.atmospheres.size() > MaxStationAtmospheres)
			return refuse("Station atmospheres are limited to "
				+ std::to_string(MaxStationAtmospheres) + " entries");
		std::set<std::string> atmosphereIds;
		for (auto const& atmosphere : settings.atmospheres)
		{
			auto const where = "Station atmosphere '" + atmosphere.id + "'";
			if (atmosphere.id.empty()) return refuse("Station atmosphere id must not be empty");
			if (atmosphere.id.size() > MaxStationStoreIdLength)
				return refuse("Station atmosphere id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!atmosphereIds.insert(atmosphere.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationAtmosphereHazardClasses.begin(),
				StationAtmosphereHazardClasses.end(),
				atmosphere.hazardClass) == StationAtmosphereHazardClasses.end())
				return refuse(where + " has unknown atmosphere hazard class '" + atmosphere.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (!std::isfinite(atmosphere.volumeM3) || atmosphere.volumeM3 <= 0.0)
				return refuse(where + " volume must be finite and positive");
			auto const masses = {atmosphere.startingOxygenKg, atmosphere.startingNitrogenKg,
				atmosphere.startingCarbonDioxideKg, atmosphere.startingWaterVapourKg};
			for (auto const mass : masses)
				if (!std::isfinite(mass) || mass < 0.0)
					return refuse(where + " starting gas masses must all be finite and non-negative");
			if (!std::isfinite(atmosphere.startingTemperatureC)
				|| atmosphere.startingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " starting temperature must be finite and above absolute zero (-273.15 C)");
			if (!std::isfinite(atmosphere.thermalCapacityKJPerK) || atmosphere.thermalCapacityKJPerK <= 0.0)
				return refuse(where + " effective thermal capacity must be finite and positive");
			if (!atmosphere.reservoirId.empty())
			{
				auto const host = std::find_if(settings.thermalReservoirs.begin(),
					settings.thermalReservoirs.end(),
					[&](ThermalReservoirSettings const& reservoir) { return reservoir.id == atmosphere.reservoirId; });
				if (host == settings.thermalReservoirs.end())
					return refuse(where + " is coupled to unknown thermal reservoir '" + atmosphere.reservoirId + "'");
			}
			if (!std::isfinite(atmosphere.exchangerConductanceKWPerK) || atmosphere.exchangerConductanceKWPerK < 0.0)
				return refuse(where + " exchanger conductance must be finite and non-negative");
			if (atmosphere.occupants < 0)
				return refuse(where + " occupant count must not be negative");
			auto const rates = {atmosphere.oxygenUsePerOccupantKgPerS,
				atmosphere.co2ProductionPerOccupantKgPerS, atmosphere.sensibleHeatPerOccupantKW,
				atmosphere.moistureAdditionPerOccupantKgPerS};
			for (auto const rate : rates)
				if (!std::isfinite(rate) || rate < 0.0)
					return refuse(where + " per-occupant load rates must all be finite and non-negative");
			if (!std::isfinite(atmosphere.leakRateKgPerS) || atmosphere.leakRateKgPerS < 0.0)
				return refuse(where + " leak rate must be finite and non-negative");
			if (!std::isfinite(atmosphere.minPressureKPa) || !std::isfinite(atmosphere.maxPressureKPa)
				|| atmosphere.minPressureKPa <= 0.0 || atmosphere.minPressureKPa >= atmosphere.maxPressureKPa)
				return refuse(where + " pressure limits must be finite and positive with min below max");
			if (!std::isfinite(atmosphere.minOxygenPartialPressureKPa) || atmosphere.minOxygenPartialPressureKPa < 0.0)
				return refuse(where + " minimum oxygen partial pressure must be finite and non-negative");
			if (!std::isfinite(atmosphere.maxCarbonDioxidePartialPressureKPa)
				|| atmosphere.maxCarbonDioxidePartialPressureKPa < 0.0)
				return refuse(where + " maximum carbon dioxide partial pressure must be finite and non-negative");
			// #565: every authored field being finite does not make the
			// starting state publishable. The ideal-gas division by a tiny
			// volume can overflow, and a compartment whose starting
			// inventory already derives a non-finite pressure is an
			// inconsistent authored input: it is refused here, before any
			// runtime exists, so no initial or reset snapshot can ever
			// publish a non-finite derived pressure as a healthy state.
			{
				auto const startingOxygenKPa = stationAtmospherePartialPressureKPa(
					atmosphere.startingOxygenKg, StationMolarMassOxygenKgPerMol,
					atmosphere.volumeM3, atmosphere.startingTemperatureC);
				auto const startingNitrogenKPa = stationAtmospherePartialPressureKPa(
					atmosphere.startingNitrogenKg, StationMolarMassNitrogenKgPerMol,
					atmosphere.volumeM3, atmosphere.startingTemperatureC);
				auto const startingCarbonDioxideKPa = stationAtmospherePartialPressureKPa(
					atmosphere.startingCarbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
					atmosphere.volumeM3, atmosphere.startingTemperatureC);
				auto const startingVapourKPa = stationAtmospherePartialPressureKPa(
					atmosphere.startingWaterVapourKg, StationMolarMassWaterKgPerMol,
					atmosphere.volumeM3, atmosphere.startingTemperatureC);
				if (!std::isfinite(startingOxygenKPa) || !std::isfinite(startingNitrogenKPa)
					|| !std::isfinite(startingCarbonDioxideKPa) || !std::isfinite(startingVapourKPa)
					|| !std::isfinite(startingOxygenKPa + startingNitrogenKPa
						+ startingCarbonDioxideKPa + startingVapourKPa))
					return refuse(where + " starting inventory in this volume and temperature derives a non-finite pressure");
			}
		}
		// Scheduled airlock cycle contracts (#400): each cycle draws from
		// one declared compartment on the shared scenario schedule rule
		// and splits its removal into recovered and lost by the authored
		// recovery fraction.
		if (settings.airlockCycles.size() > MaxStationAirlockCycles)
			return refuse("Station airlock cycles are limited to "
				+ std::to_string(MaxStationAirlockCycles) + " entries");
		std::set<std::string> airlockCycleIds;
		for (auto const& cycle : settings.airlockCycles)
		{
			auto const where = "Station airlock cycle '" + cycle.id + "'";
			if (cycle.id.empty()) return refuse("Station airlock cycle id must not be empty");
			if (cycle.id.size() > MaxStationStoreIdLength)
				return refuse("Station airlock cycle id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!airlockCycleIds.insert(cycle.id).second) return refuse(where + " id is duplicated");
			auto const it = std::find_if(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == cycle.atmosphereId; });
			if (it == settings.atmospheres.end())
				return refuse(where + " draws from unknown atmosphere compartment '" + cycle.atmosphereId + "'");
			if (!std::isfinite(cycle.gasMassKg) || cycle.gasMassKg <= 0.0)
				return refuse(where + " cycled gas mass must be finite and positive");
			if (!std::isfinite(cycle.firstOccurrenceSeconds) || cycle.firstOccurrenceSeconds < 0.0)
				return refuse(where + " first occurrence must be finite and non-negative");
			if (!std::isfinite(cycle.recurrenceSeconds) || cycle.recurrenceSeconds < 0.0)
				return refuse(where + " recurrence must be finite and non-negative (0 is a one-off)");
			if (cycle.recurrenceSeconds > 0.0
				&& cycle.recurrenceSeconds + StationScheduleTolerance
					< static_cast<double>(World::getFixedTimestep()))
				return refuse(where + " recurrence must be zero or at least one World fixed tick ("
					+ std::to_string(World::getFixedTimestep()) + " seconds)");
			if (!std::isfinite(cycle.recoveredFraction) || cycle.recoveredFraction < 0.0
				|| cycle.recoveredFraction > 1.0)
				return refuse(where + " recovered fraction must be finite and from 0 to 1");
		}
		// Crew contracts (#401): the crew is an authored population and
		// activity assignment, never derived from World Agents. All four
		// matter stores must be compound mass stores so the declared
		// compositions can carry the nutrient/element closure: at full
		// activity the emitted mass (CO2 + moisture + urine + organic
		// waste) may not exceed the consumed mass (food + water +
		// oxygen); every tracked constituent returned in the urine and
		// organic-waste streams (plus respired moisture) must be funded
		// by the consumed food and water; and the declared metabolic
		// heat may not exceed the declared food energy minus the energy
		// still bound in the waste streams. A crew that would create
		// mass, constituents or energy out of nothing is refused before
		// anything runs; a crew whose returns fall short of its
		// consumption retains the difference as explicit body mass,
		// reported in the ledger rather than discarded.
		if (settings.crews.size() > MaxStationCrews)
			return refuse("Station crews are limited to "
				+ std::to_string(MaxStationCrews) + " entries");
		std::set<std::string> crewIds;
		for (auto const& crew : settings.crews)
		{
			auto const where = "Station crew '" + crew.id + "'";
			if (crew.id.empty()) return refuse("Station crew id must not be empty");
			if (crew.id.size() > MaxStationStoreIdLength)
				return refuse("Station crew id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!crewIds.insert(crew.id).second) return refuse(where + " id is duplicated");
			if (std::find_if(settings.atmospheres.begin(), settings.atmospheres.end(),
				[&](AtmosphereSettings const& atmosphere) { return atmosphere.id == crew.atmosphereId; })
				== settings.atmospheres.end())
				return refuse(where + " breathes in unknown atmosphere compartment '" + crew.atmosphereId + "'");
			if (crew.crewCount < 0) return refuse(where + " crew count must not be negative");
			if (!validateStationProfile(crew.activityProfile, where + " activity profile", diagnostic))
				return false;
			auto const rates = {crew.foodPerCrewPerS, crew.waterPerCrewPerS,
				crew.oxygenPerCrewKgPerS, crew.co2PerCrewKgPerS, crew.moisturePerCrewKgPerS,
				crew.urinePerCrewPerS, crew.organicWastePerCrewPerS, crew.metabolicHeatPerCrewKW};
			for (auto const rate : rates)
				if (!std::isfinite(rate) || rate < 0.0)
					return refuse(where + " metabolic rates must be finite and non-negative");
			ResourceStoreSettings const* streamStores[4]{nullptr, nullptr, nullptr, nullptr};
			char const* streamRoles[4]{"food", "water", "urine", "organic waste"};
			std::string const streamIds[4]{crew.foodStoreId, crew.waterStoreId,
				crew.urineStoreId, crew.organicWasteStoreId};
			for (size_t stream = 0; stream < 4; ++stream)
			{
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == streamIds[stream]; });
				if (found == settings.stores.end())
					return refuse(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' does not exist");
				if (!isMassUnit(found->unit))
					return refuse(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' must be a mass unit; crew metabolism is mass-denominated (g or kg)");
				if (!found->composition)
					return refuse(where + " " + streamRoles[stream] + " store '" + streamIds[stream]
						+ "' has no declared composition; crew streams require compound stores to close the element balance");
				streamStores[stream] = &*found;
			}
			auto const* food = streamStores[0];
			auto const* water = streamStores[1];
			auto const* urine = streamStores[2];
			auto const* waste = streamStores[3];
			auto const foodKg = massToKg(food->unit, crew.foodPerCrewPerS);
			auto const waterKg = massToKg(water->unit, crew.waterPerCrewPerS);
			auto const urineKg = massToKg(urine->unit, crew.urinePerCrewPerS);
			auto const wasteKg = massToKg(waste->unit, crew.organicWastePerCrewPerS);
			auto const consumedKg = foodKg + waterKg + crew.oxygenPerCrewKgPerS;
			auto const emittedKg = urineKg + wasteKg
				+ crew.moisturePerCrewKgPerS + crew.co2PerCrewKgPerS;
			if (emittedKg > consumedKg + StationBalanceTolerance * std::max(1.0, consumedKg))
				return refuse(where + " declared returns exceed declared consumption: metabolism may not create mass");
			ConstituentLedger funded;
			addComposition(funded, *food->composition, foodKg, 1.0);
			addComposition(funded, *water->composition, waterKg, 1.0);
			ConstituentLedger returned;
			addComposition(returned, *urine->composition, urineKg, 1.0);
			addComposition(returned, *waste->composition, wasteKg, 1.0);
			returned["water"] += crew.moisturePerCrewKgPerS;
			for (auto const& entry : returned)
			{
				auto const available = funded.count(entry.first) ? funded.at(entry.first) : 0.0;
				if (entry.second > available + StationBalanceTolerance
					* std::max(1.0, std::abs(available)))
					return refuse(where + " returns more '" + entry.first
						+ "' than the consumed food and water carry: the element balance may not be created");
			}
			auto const streamEnergy = [](ResourceCompositionSettings const& composition, double wetKg)
			{
				return wetKg * (1.0 - composition.moisture) * composition.energyKJPerKgDry;
			};
			auto const releasedKJPerS = streamEnergy(*food->composition, foodKg)
				- streamEnergy(*urine->composition, urineKg)
				- streamEnergy(*waste->composition, wasteKg);
			if (crew.metabolicHeatPerCrewKW > releasedKJPerS
				+ StationBalanceTolerance * std::max(1.0, std::abs(releasedKJPerS)))
				return refuse(where + " metabolic heat exceeds the consumed food energy minus the energy still bound in the waste streams: heat may not be created from nothing");
		}
		// Water recovery contracts (#402). A collector condenses vapour out
		// of one compartment onto a coolant-loop cold surface; a treatment
		// turns raw water into treated water plus retained concentrate. Both
		// carry a hazard class, and the class is what keeps a potable stream
		// from sharing equipment with a contaminated one: a collector may
		// only serve a compartment of its own class, since condensate
		// inherits the hazard of the air it came from, and no store may be
		// touched by both a normal-class and a hazardous-class water
		// process. Every water store must be a plain mass store: condensate
		// is pure water and the recovery split is mass-denominated, so a
		// compound store would create dry constituents out of nothing. The
		// treatment consumable store is the one exception - it is a supplied
		// consumable, not a water stream, and may be compound.
		if (settings.waterCollectors.size() > MaxStationWaterCollectors)
			return refuse("Station water collectors are limited to "
				+ std::to_string(MaxStationWaterCollectors) + " entries");
		if (settings.waterTreatments.size() > MaxStationWaterTreatments)
			return refuse("Station water treatments are limited to "
				+ std::to_string(MaxStationWaterTreatments) + " entries");
		std::map<std::string, std::string> waterStoreClass;
		auto declareWaterStore = [&](std::string const& storeId, char const* role,
			std::string const& where, std::string const& hazardClass, bool allowCompound) -> bool
		{
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return refuse(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; water recovery is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered water streams are plain mass stores, so no dry constituent can be created from pure condensate");
			auto const existing = waterStoreClass.find(storeId);
			if (existing == waterStoreClass.end())
				waterStoreClass.emplace(storeId, hazardClass);
			else if (existing->second != hazardClass)
				return refuse(where + " " + role + " store '" + storeId + "' is already used by a "
					+ existing->second + "-class water process; hazardous and normal water streams may not share a store");
			return true;
		};
		std::set<std::string> waterIds;
		for (auto const& collector : settings.waterCollectors)
		{
			auto const where = "Station water collector '" + collector.id + "'";
			if (collector.id.empty()) return refuse("Station water collector id must not be empty");
			if (collector.id.size() > MaxStationStoreIdLength)
				return refuse("Station water collector id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!waterIds.insert(collector.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				collector.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown water hazard class '" + collector.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				collector.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + collector.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(collector.maxRateKgPerS) || collector.maxRateKgPerS < 0.0)
				return refuse(where + " maximum collection rate must be finite and non-negative");
			if (!std::isfinite(collector.targetRelativeHumidity)
				|| collector.targetRelativeHumidity < 0.0 || collector.targetRelativeHumidity > 1.0)
				return refuse(where + " target relative humidity must be finite and from 0 to 1");
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
				{ return atmosphere.id == collector.atmosphereId; });
			if (compartment == settings.atmospheres.end())
				return refuse(where + " condenses from unknown atmosphere compartment '" + collector.atmosphereId + "'");
			if (compartment->hazardClass != collector.hazardClass)
				return refuse(where + " hazard class '" + collector.hazardClass + "' does not match compartment '"
					+ compartment->id + "' hazard class '" + compartment->hazardClass
					+ "'; condensate inherits the hazard of the air it is condensed from");
			auto const loop = std::find_if(settings.coolantLoops.begin(), settings.coolantLoops.end(),
				[&](CoolantLoopSettings const& candidate) { return candidate.id == collector.coolingLoopId; });
			if (loop == settings.coolantLoops.end())
				return refuse(where + " cold surface is served by unknown coolant loop '" + collector.coolingLoopId + "'");
			auto const host = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == loop->reservoirId; });
			if (host == settings.thermalReservoirs.end())
				return refuse(where + " coolant loop '" + loop->id + "' has no known host reservoir");
			if (host->thermalClass != collector.thermalClass)
				return refuse(where + " thermal class '" + collector.thermalClass + "' does not match coolant loop '"
					+ loop->id + "' host reservoir class '" + host->thermalClass
					+ "'; the cold surface serves only its own service class");
			if (!declareWaterStore(collector.destStoreId, "raw water destination", where,
				collector.hazardClass, false))
				return false;
		}
		for (auto const& treatment : settings.waterTreatments)
		{
			auto const where = "Station water treatment '" + treatment.id + "'";
			if (treatment.id.empty()) return refuse("Station water treatment id must not be empty");
			if (treatment.id.size() > MaxStationStoreIdLength)
				return refuse("Station water treatment id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!waterIds.insert(treatment.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				treatment.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown water hazard class '" + treatment.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (!std::isfinite(treatment.throughputKgPerS) || treatment.throughputKgPerS < 0.0)
				return refuse(where + " throughput must be finite and non-negative");
			if (!std::isfinite(treatment.recoveryRatio) || treatment.recoveryRatio <= 0.0
				|| treatment.recoveryRatio > 1.0)
				return refuse(where + " recovery ratio must be finite and above zero up to 1: a process that recovers nothing is not a recovery process");
			if (!std::isfinite(treatment.specificEnergyKWhPerKg) || treatment.specificEnergyKWhPerKg < 0.0)
				return refuse(where + " specific energy must be finite and non-negative");
			if (!std::isfinite(treatment.consumablePerKg) || treatment.consumablePerKg < 0.0)
				return refuse(where + " consumable per kg must be finite and non-negative");
			char const* streamRoles[3]{"source", "treated", "concentrate"};
			std::string const streamIds[3]{treatment.sourceStoreId, treatment.treatedStoreId,
				treatment.concentrateStoreId};
			for (size_t stream = 0; stream < 3; ++stream)
			{
				for (size_t other = stream + 1; other < 3; ++other)
					if (streamIds[stream] == streamIds[other])
						return refuse(where + " " + streamRoles[stream] + " store and " + streamRoles[other]
							+ " store are the same store '" + streamIds[stream]
							+ "'; the recovery split needs distinct source, treated and concentrate stores");
				if (!declareWaterStore(streamIds[stream], streamRoles[stream], where,
					treatment.hazardClass, false))
					return false;
			}
			if (!declareWaterStore(treatment.consumableStoreId, "consumable", where,
				treatment.hazardClass, true))
				return false;
		}
		// Oxygen production and CO2 capture contracts (#404). An
		// electrolyzer splits qualified water into oxygen and hydrogen: the
		// split is stoichiometric, so the authored electrical dose must at
		// least cover the chemical energy the produced hydrogen retains -
		// a dose below it would create energy-rich products out of less
		// energy than they carry, which no machine can do. Exactly one
		// oxygen destination must be configured: a qualified gas store or
		// one compartment atmosphere, never both and never neither. All gas
		// and water stores in the chain are plain mass stores: a compound
		// store would create dry constituents out of pure water or pure
		// gas. Hazard class segregates the whole chain - the feed store,
		// both product stores and the delivery compartment must carry the
		// cell's own class - and the heat sink must share the cell's
		// thermal service class, because spare capacity in one class is
		// never usable by another. A CO2 capture machine scrubs one
		// compartment of its own hazard class into a finite capture store;
		// if it declares a consumable demand it must name a consumable
		// store, because a process cannot spend media it has no way to
		// hold.
		if (settings.electrolyzers.size() > MaxStationElectrolyzers)
			return refuse("Station electrolyzers are limited to "
				+ std::to_string(MaxStationElectrolyzers) + " entries");
		if (settings.co2Captures.size() > MaxStationCo2Captures)
			return refuse("Station CO2 capture machines are limited to "
				+ std::to_string(MaxStationCo2Captures) + " entries");
		if (settings.fuelCells.size() > MaxStationFuelCells)
			return refuse("Station fuel cells are limited to "
				+ std::to_string(MaxStationFuelCells) + " entries");
		std::map<std::string, std::string> gasStoreClass;
		auto declareGasStore = [&](std::string const& storeId, char const* role,
			std::string const& where, std::string const& hazardClass, bool allowCompound) -> bool
		{
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return refuse(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; gas handling is mass-denominated (g or kg)");
			if (found->composition && !allowCompound)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; handled gas streams are plain mass stores, so no dry constituent can be created from pure gas");
			auto const existing = gasStoreClass.find(storeId);
			if (existing == gasStoreClass.end())
				gasStoreClass.emplace(storeId, hazardClass);
			else if (existing->second != hazardClass)
				return refuse(where + " " + role + " store '" + storeId + "' is already used by a "
					+ existing->second + "-class gas process; hazardous and normal gas streams may not share a store");
			return true;
		};
		std::set<std::string> gasIds;
		for (auto const& electrolyzer : settings.electrolyzers)
		{
			auto const where = "Station electrolyzer '" + electrolyzer.id + "'";
			if (electrolyzer.id.empty()) return refuse("Station electrolyzer id must not be empty");
			if (electrolyzer.id.size() > MaxStationStoreIdLength)
				return refuse("Station electrolyzer id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(electrolyzer.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				electrolyzer.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + electrolyzer.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				electrolyzer.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + electrolyzer.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(electrolyzer.rateKgPerS) || electrolyzer.rateKgPerS < 0.0)
				return refuse(where + " rated water split must be finite and non-negative");
			if (!std::isfinite(electrolyzer.specificEnergyKWhPerKgWater)
				|| electrolyzer.specificEnergyKWhPerKgWater * 3600.0
					< StationElectrolysisChemicalEnergyKJPerKgWater
					- StationBalanceTolerance * std::max(1.0,
						StationElectrolysisChemicalEnergyKJPerKgWater))
				return refuse(where + " specific energy is below the chemical energy the produced hydrogen retains: the electrical dose may not fund less than the products carry");
			if (!std::isfinite(electrolyzer.maxSinkTemperatureC)
				|| electrolyzer.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (electrolyzer.oxygenStoreId.empty() == electrolyzer.atmosphereId.empty())
				return refuse(where + " must configure exactly one oxygen destination: a qualified gas store or one compartment atmosphere");
			if (!declareWaterStore(electrolyzer.waterStoreId, "feed water", where,
				electrolyzer.hazardClass, false))
				return false;
			if (!electrolyzer.oxygenStoreId.empty()
				&& !declareGasStore(electrolyzer.oxygenStoreId, "oxygen", where,
					electrolyzer.hazardClass, false))
				return false;
			if (!declareGasStore(electrolyzer.hydrogenStoreId, "hydrogen", where,
				electrolyzer.hazardClass, false))
				return false;
			if (!electrolyzer.atmosphereId.empty())
			{
				auto const compartment = std::find_if(settings.atmospheres.begin(),
					settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
					{ return atmosphere.id == electrolyzer.atmosphereId; });
				if (compartment == settings.atmospheres.end())
					return refuse(where + " delivers oxygen to unknown atmosphere compartment '" + electrolyzer.atmosphereId + "'");
				if (compartment->hazardClass != electrolyzer.hazardClass)
					return refuse(where + " hazard class '" + electrolyzer.hazardClass
						+ "' does not match compartment '" + compartment->id
						+ "' hazard class '" + compartment->hazardClass
						+ "'; oxygen may only be delivered to a compartment of the cell's own class");
			}
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == electrolyzer.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink is unknown thermal reservoir '" + electrolyzer.heatSinkId + "'");
			if (sink->thermalClass != electrolyzer.thermalClass)
				return refuse(where + " thermal class '" + electrolyzer.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}
		for (auto const& capture : settings.co2Captures)
		{
			auto const where = "Station CO2 capture machine '" + capture.id + "'";
			if (capture.id.empty()) return refuse("Station CO2 capture machine id must not be empty");
			if (capture.id.size() > MaxStationStoreIdLength)
				return refuse("Station CO2 capture machine id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(capture.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				capture.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + capture.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (!std::isfinite(capture.maxRateKgPerS) || capture.maxRateKgPerS < 0.0)
				return refuse(where + " capture capacity must be finite and non-negative");
			if (!std::isfinite(capture.specificEnergyKWhPerKg) || capture.specificEnergyKWhPerKg < 0.0)
				return refuse(where + " specific energy must be finite and non-negative");
			if (!std::isfinite(capture.consumablePerKg) || capture.consumablePerKg < 0.0)
				return refuse(where + " consumable per kg must be finite and non-negative");
			if (capture.consumablePerKg > 0.0 && capture.consumableStoreId.empty())
				return refuse(where + " declares a consumable demand but names no consumable store: a process cannot spend media it has no way to hold");
			auto const compartment = std::find_if(settings.atmospheres.begin(),
				settings.atmospheres.end(), [&](AtmosphereSettings const& atmosphere)
				{ return atmosphere.id == capture.atmosphereId; });
			if (compartment == settings.atmospheres.end())
				return refuse(where + " scrubs unknown atmosphere compartment '" + capture.atmosphereId + "'");
			if (compartment->hazardClass != capture.hazardClass)
				return refuse(where + " hazard class '" + capture.hazardClass
					+ "' does not match compartment '" + compartment->id
					+ "' hazard class '" + compartment->hazardClass
					+ "'; a hazardous atmosphere's carbon stays on the hazardous side of the capture chain");
			if (!declareGasStore(capture.captureStoreId, "capture", where,
				capture.hazardClass, false))
				return false;
			if (!capture.consumableStoreId.empty()
				&& !declareGasStore(capture.consumableStoreId, "consumable", where,
					capture.hazardClass, true))
				return false;
		}
		// Fuel-cell backup contracts (#406). A cell generates electrical
		// power from two dedicated reactant stores and can only ever see
		// its own stores, so protected breathing oxygen is never drawn
		// implicitly: refilling the oxygen store is a controlled surplus
		// transfer that itself preserves the supplying service's protected
		// reserve. Efficiency is electrical out per chemical energy in and
		// must sit above zero and at most one - a cell delivering more
		// electrical energy than the reactants' chemical energy is a
		// perpetual-motion claim, not a machine. The dispatch setpoint may
		// not exceed the installed rating. All three stores in the chain
		// are plain mass stores carrying the cell's own hazard class, and
		// the heat sink must share the cell's thermal service class.
		for (auto const& cell : settings.fuelCells)
		{
			auto const where = "Station fuel cell '" + cell.id + "'";
			if (cell.id.empty()) return refuse("Station fuel cell id must not be empty");
			if (cell.id.size() > MaxStationStoreIdLength)
				return refuse("Station fuel cell id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(cell.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				cell.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + cell.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				cell.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + cell.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(cell.ratedOutputKW) || cell.ratedOutputKW < 0.0)
				return refuse(where + " rated electrical output must be finite and non-negative");
			if (!std::isfinite(cell.efficiency) || cell.efficiency <= 0.0 || cell.efficiency > 1.0)
				return refuse(where + " efficiency must be greater than 0 and at most 1: a cell cannot deliver more electrical energy than the reactants' chemical energy");
			if (!std::isfinite(cell.commandedOutputKW) || cell.commandedOutputKW < 0.0
				|| cell.commandedOutputKW > cell.ratedOutputKW)
				return refuse(where + " commanded output must be finite and within the installed rating");
			if (!std::isfinite(cell.maxSinkTemperatureC)
				|| cell.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (!declareGasStore(cell.hydrogenStoreId, "hydrogen reactant", where,
				cell.hazardClass, false))
				return false;
			if (!declareGasStore(cell.oxygenStoreId, "oxygen reactant", where,
				cell.hazardClass, false))
				return false;
			if (!declareWaterStore(cell.waterStoreId, "product water", where,
				cell.hazardClass, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == cell.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink is unknown thermal reservoir '" + cell.heatSinkId + "'");
			if (sink->thermalClass != cell.thermalClass)
				return refuse(where + " thermal class '" + cell.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}
		// Carbon recovery contracts (#405). A Sabatier reactor converts
		// captured CO2 and hydrogen into methane and water; the conversion
		// is stoichiometric under the declared molar masses, so the mass
		// ledger closes by construction. All four stores in the chain are
		// plain mass stores carrying the reactor's own hazard class, and
		// the heat sink must share the reactor's thermal service class.
		// A methane processor splits methane into recovered hydrogen and
		// solid carbon; the split is stoichiometric, and because the
		// reaction is endothermic the authored electrical dose must at
		// least cover the declared reaction heat demand - a dose below it
		// would drive a reaction the machine cannot heat. The recovered
		// hydrogen returns to the hydrogen store as recycled feed: the
		// Sabatier stage consumes twice as much hydrogen per methane as
		// the processor returns, so the combined loop retains a net
		// hydrogen demand and the return is never free continuing supply.
		if (settings.sabatierReactors.size() > MaxStationSabatierReactors)
			return refuse("Station Sabatier reactors are limited to "
				+ std::to_string(MaxStationSabatierReactors) + " entries");
		if (settings.methaneProcessors.size() > MaxStationMethaneProcessors)
			return refuse("Station methane processors are limited to "
				+ std::to_string(MaxStationMethaneProcessors) + " entries");
		for (auto const& reactor : settings.sabatierReactors)
		{
			auto const where = "Station Sabatier reactor '" + reactor.id + "'";
			if (reactor.id.empty()) return refuse("Station Sabatier reactor id must not be empty");
			if (reactor.id.size() > MaxStationStoreIdLength)
				return refuse("Station Sabatier reactor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(reactor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				reactor.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + reactor.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				reactor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + reactor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(reactor.rateKgPerS) || reactor.rateKgPerS < 0.0)
				return refuse(where + " rated CO2 conversion must be finite and non-negative");
			if (!std::isfinite(reactor.specificEnergyKWhPerKgCO2)
				|| reactor.specificEnergyKWhPerKgCO2 < 0.0)
				return refuse(where + " specific energy must be finite and non-negative");
			if (!std::isfinite(reactor.maxSinkTemperatureC)
				|| reactor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (!declareGasStore(reactor.co2StoreId, "CO2 feed", where,
				reactor.hazardClass, false))
				return false;
			if (!declareGasStore(reactor.hydrogenStoreId, "hydrogen feed", where,
				reactor.hazardClass, false))
				return false;
			if (!declareGasStore(reactor.methaneStoreId, "methane", where,
				reactor.hazardClass, false))
				return false;
			if (!declareWaterStore(reactor.waterStoreId, "product water", where,
				reactor.hazardClass, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == reactor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink is unknown thermal reservoir '" + reactor.heatSinkId + "'");
			if (sink->thermalClass != reactor.thermalClass)
				return refuse(where + " thermal class '" + reactor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}
		for (auto const& processor : settings.methaneProcessors)
		{
			auto const where = "Station methane processor '" + processor.id + "'";
			if (processor.id.empty()) return refuse("Station methane processor id must not be empty");
			if (processor.id.size() > MaxStationStoreIdLength)
				return refuse("Station methane processor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!gasIds.insert(processor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				processor.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + processor.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				processor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + processor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(processor.rateKgPerS) || processor.rateKgPerS < 0.0)
				return refuse(where + " rated methane processing must be finite and non-negative");
			if (!std::isfinite(processor.specificEnergyKWhPerKgCH4)
				|| processor.specificEnergyKWhPerKgCH4 * 3600.0
					< StationMethaneProcessingReactionHeatKJPerKgMethane
					- StationBalanceTolerance * std::max(1.0,
						StationMethaneProcessingReactionHeatKJPerKgMethane))
				return refuse(where + " specific energy is below the endothermic reaction heat the split must absorb: the electrical dose may not fund less than the reaction demands");
			if (!std::isfinite(processor.maxSinkTemperatureC)
				|| processor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (!declareGasStore(processor.methaneStoreId, "methane feed", where,
				processor.hazardClass, false))
				return false;
			if (!declareGasStore(processor.hydrogenStoreId, "recovered hydrogen", where,
				processor.hazardClass, false))
				return false;
			if (!declareGasStore(processor.carbonStoreId, "solid carbon", where,
				processor.hazardClass, false))
				return false;
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == processor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink is unknown thermal reservoir '" + processor.heatSinkId + "'");
			if (sink->thermalClass != processor.thermalClass)
				return refuse(where + " thermal class '" + processor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}
		// Nutrient recovery contracts (#407). A microbial processor digests
		// a compound organic-waste feedstock and recovers its bound
		// nutrients per constituent: the recovery yields are fractions of
		// the feed's own bound nitrogen, phosphorus, potassium and grouped
		// mineral content, so the recovered nutrients are funded by the
		// feedstock alone. Atmospheric nitrogen in the feed is never a
		// nutrient source - no configured process in this slice converts
		// it, so it passes through to the residue and can never
		// substitute for bound N. The feed must declare a composition:
		// there is nothing to recover from an uncharacterised stream, and
		// the product stores (nutrients, residue) must be plain mass
		// stores, because a compound destination would create tracked
		// constituents out of nothing. The residue rate is the
		// mass-balance remainder of the declared inputs and outputs per
		// kilogram of feed: a configuration whose declared outputs would
	// exceed its inputs is refused rather than gifting mass. The
		// exothermic reaction heat is capped by the feedstock's declared
		// chemical energy per kilogram of feed - the released heat is
		// paid for by the feed, never free - and the operating window
		// must be a real window: minimum below maximum, both above
		// absolute zero.
		if (settings.microbialProcessors.size() > MaxStationMicrobialProcessors)
			return refuse("Station microbial processors are limited to "
				+ std::to_string(MaxStationMicrobialProcessors) + " entries");
		std::map<std::string, std::string> microbialStoreClass;
		auto declareMicrobialStore = [&](std::string const& storeId, char const* role,
			std::string const& where, std::string const& hazardClass,
			bool requireCompound) -> bool
		{
			auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == storeId; });
			if (found == settings.stores.end())
				return refuse(where + " " + role + " store '" + storeId + "' does not exist");
			if (!isMassUnit(found->unit))
				return refuse(where + " " + role + " store '" + storeId
					+ "' must be a mass unit; nutrient recovery is mass-denominated (g or kg)");
			if (requireCompound && !found->composition)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares no composition; a microbial processor recovers bound nutrients from a declared compound feedstock, not from an uncharacterised stream");
			if (!requireCompound && found->composition)
				return refuse(where + " " + role + " store '" + storeId
					+ "' declares a composition; recovered product streams land as plain mass, so a compound destination would create tracked constituents out of nothing");
			auto const existing = microbialStoreClass.find(storeId);
			if (existing == microbialStoreClass.end())
				microbialStoreClass.emplace(storeId, hazardClass);
			else if (existing->second != hazardClass)
				return refuse(where + " " + role + " store '" + storeId + "' is already used by a "
					+ existing->second + "-class nutrient process; hazardous and normal nutrient streams may not share a store");
			return true;
		};
		std::set<std::string> microbialIds;
		for (auto const& processor : settings.microbialProcessors)
		{
			auto const where = "Station microbial processor '" + processor.id + "'";
			if (processor.id.empty()) return refuse("Station microbial processor id must not be empty");
			if (processor.id.size() > MaxStationStoreIdLength)
				return refuse("Station microbial processor id exceeds "
					+ std::to_string(MaxStationStoreIdLength) + " characters");
			if (!microbialIds.insert(processor.id).second) return refuse(where + " id is duplicated");
			if (std::find(StationWaterHazardClasses.begin(), StationWaterHazardClasses.end(),
				processor.hazardClass) == StationWaterHazardClasses.end())
				return refuse(where + " has unknown hazard class '" + processor.hazardClass
					+ "'; declared classes are: normal, hazardous");
			if (std::find(StationThermalServiceClasses.begin(), StationThermalServiceClasses.end(),
				processor.thermalClass) == StationThermalServiceClasses.end())
				return refuse(where + " has unknown thermal service class '" + processor.thermalClass
					+ "'; declared classes are: " + declaredThermalClasses());
			if (!std::isfinite(processor.rateKgPerS) || processor.rateKgPerS < 0.0)
				return refuse(where + " rated feed processing must be finite and non-negative");
			double const rateFields[]{processor.specificEnergyKWhPerKgFeed,
				processor.oxygenPerKgFeedKg, processor.processWaterPerKgFeedKg,
				processor.consumablePerKgFeed, processor.co2PerKgFeedKg,
				processor.reactionHeatKJPerKgFeed};
			char const* rateNames[]{"specific energy", "oxygen per kg feed",
				"process water per kg feed", "consumable per kg feed",
				"CO2 per kg feed", "reaction heat per kg feed"};
			for (size_t field = 0; field < 6; ++field)
				if (!std::isfinite(rateFields[field]) || rateFields[field] < 0.0)
					return refuse(where + " " + rateNames[field] + " must be finite and non-negative");
			double const recoveryFields[]{processor.nitrogenRecovery,
				processor.phosphorusRecovery, processor.potassiumRecovery,
				processor.mineralsRecovery, processor.waterRecovery};
			char const* recoveryNames[]{"nitrogen recovery", "phosphorus recovery",
				"potassium recovery", "minerals recovery", "water recovery"};
			for (size_t field = 0; field < 5; ++field)
				if (!std::isfinite(recoveryFields[field]) || recoveryFields[field] < 0.0
					|| recoveryFields[field] > 1.0)
					return refuse(where + " " + recoveryNames[field]
						+ " must be finite and from 0 to 1: a yield cannot recover more than the feed carries");
			if (!std::isfinite(processor.minOperatingTemperatureC)
				|| processor.minOperatingTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " minimum operating temperature must be finite and above absolute zero");
			if (!std::isfinite(processor.maxSinkTemperatureC)
				|| processor.maxSinkTemperatureC <= -StationCelsiusOffset)
				return refuse(where + " maximum sink temperature must be finite and above absolute zero");
			if (processor.minOperatingTemperatureC >= processor.maxSinkTemperatureC)
				return refuse(where + " minimum operating temperature must be below the maximum sink temperature: an empty operating window is never a service");
			std::string const chainIds[8]{processor.feedStoreId, processor.oxygenStoreId,
				processor.waterStoreId, processor.consumableStoreId, processor.nutrientStoreId,
				processor.recoveredWaterStoreId, processor.co2StoreId, processor.residueStoreId};
			char const* chainRoles[8]{"feed", "oxygen", "process water", "consumable",
				"nutrient", "recovered water", "CO2", "residue"};
			for (size_t stream = 0; stream < 8; ++stream)
				for (size_t other = stream + 1; other < 8; ++other)
					if (chainIds[stream] == chainIds[other])
						return refuse(where + " " + chainRoles[stream] + " store and "
							+ chainRoles[other] + " store are the same store '"
							+ chainIds[stream] + "'; the recovery chain needs distinct stores for every stream");
			if (!declareMicrobialStore(processor.feedStoreId, "feed", where,
				processor.hazardClass, true))
				return false;
			if (!declareGasStore(processor.oxygenStoreId, "oxygen", where,
				processor.hazardClass, false))
				return false;
			if (!declareWaterStore(processor.waterStoreId, "process water", where,
				processor.hazardClass, false))
				return false;
			{
				auto const found = std::find_if(settings.stores.begin(), settings.stores.end(),
					[&](ResourceStoreSettings const& store) { return store.id == processor.consumableStoreId; });
				if (found == settings.stores.end())
					return refuse(where + " consumable store '" + processor.consumableStoreId
						+ "' does not exist");
				auto const existing = microbialStoreClass.find(processor.consumableStoreId);
				if (existing == microbialStoreClass.end())
					microbialStoreClass.emplace(processor.consumableStoreId, processor.hazardClass);
				else if (existing->second != processor.hazardClass)
					return refuse(where + " consumable store '" + processor.consumableStoreId
						+ "' is already used by a " + existing->second
						+ "-class nutrient process; hazardous and normal nutrient streams may not share a store");
			}
			if (!declareMicrobialStore(processor.nutrientStoreId, "nutrient", where,
				processor.hazardClass, false))
				return false;
			if (!declareWaterStore(processor.recoveredWaterStoreId, "recovered water", where,
				processor.hazardClass, false))
				return false;
			if (!declareGasStore(processor.co2StoreId, "CO2", where,
				processor.hazardClass, false))
				return false;
			if (!declareMicrobialStore(processor.residueStoreId, "residue", where,
				processor.hazardClass, false))
				return false;
			auto const feed = std::find_if(settings.stores.begin(), settings.stores.end(),
				[&](ResourceStoreSettings const& store) { return store.id == processor.feedStoreId; });
			auto const& composition = *feed->composition;
			auto const dryPerKgFeed = 1.0 - composition.moisture;
			auto const nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
				* processor.nitrogenRecovery + composition.phosphorus * processor.phosphorusRecovery
				+ composition.potassium * processor.potassiumRecovery
				+ composition.otherMinerals * processor.mineralsRecovery);
			auto const recoveredWaterPerKgFeed = composition.moisture * processor.waterRecovery;
			auto const residuePerKgFeed = 1.0 + processor.oxygenPerKgFeedKg
				+ processor.processWaterPerKgFeedKg - nutrientPerKgFeed
				- recoveredWaterPerKgFeed - processor.co2PerKgFeedKg;
			if (residuePerKgFeed < -StationBalanceTolerance)
				return refuse(where + " declared yields would create mass: the outputs exceed the feed plus oxygen plus process water, and an unrecoverable residue cannot be negative");
			auto const feedEnergyKJPerKgFeed = dryPerKgFeed * composition.energyKJPerKgDry;
			if (processor.reactionHeatKJPerKgFeed > feedEnergyKJPerKgFeed
				+ StationBalanceTolerance * std::max(1.0, std::abs(feedEnergyKJPerKgFeed)))
				return refuse(where + " declared reaction heat exceeds the feedstock's declared chemical energy: the digestion may not release more heat than the feed carries");
			auto const sink = std::find_if(settings.thermalReservoirs.begin(),
				settings.thermalReservoirs.end(), [&](ThermalReservoirSettings const& reservoir)
				{ return reservoir.id == processor.heatSinkId; });
			if (sink == settings.thermalReservoirs.end())
				return refuse(where + " heat sink is unknown thermal reservoir '" + processor.heatSinkId + "'");
			if (sink->thermalClass != processor.thermalClass)
				return refuse(where + " thermal class '" + processor.thermalClass
					+ "' does not match heat sink reservoir '" + sink->id + "' class '"
					+ sink->thermalClass + "'; spare capacity in one class is never usable by another");
		}
		diagnostic.clear(); return true;
	}
	void setStationSystemsSourceLoader(StationSystemsSourceLoader loader) { sourceLoader = std::move(loader); }
	std::string bundledStationSystemsSource() { return std::string(kStationSystemsLua); }
	struct StationSystemsRuntime::Impl
	{
		// Current inventory is runtime state only: it is initialised from the
		// authored starting quantities when the runtime is constructed and is
		// never written back into the authored settings or the document.
		struct StoreState
		{
			ResourceStoreSettings authored;
			double currentQualified{0.0};
			double currentUnqualified{0.0};
			double lastTickIn{0.0};
			double lastTickOut{0.0};
		};
		// The authoritative ledger of one authored transfer. Results are
		// staged per tick and committed atomically; unmet material never moves
		// and is never deleted.
		struct TransferState
		{
			ResourceTransferSettings authored;
			size_t sourceIndex{0};
			size_t destinationIndex{0};
			double lastRequested{0.0};
			double lastCompleted{0.0};
			double lastUnmet{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored conversion. Inputs are
		// consumed and outputs produced at a common completed scale: the
		// fraction of declared full throughput the tick could actually run,
		// bounded by drawable input inventory and output store headroom.
		// Results stage and commit atomically with the transfer ledger.
		struct ConversionFlowState
		{
			size_t storeIndex{0};
			double ratePerSecond{0.0};
			double lastRequested{0.0};
			double lastCompleted{0.0};
		};
		struct ConversionState
		{
			ResourceConversionSettings authored;
			std::vector<ConversionFlowState> inputs;
			std::vector<ConversionFlowState> outputs;
			double lastScale{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
			std::string stallReason;
		};
		// The authoritative ledger of one configured solar array. Output
		// follows installed capacity and evaluated illumination; a
		// zero-capacity array is disabled and contributes nothing.
		struct SolarState
		{
			SolarArraySettings authored;
			double lastIllumination{0.0};
			double lastAvailableKW{0.0};
			bool lastDisabled{false};
			bool lastEclipse{true};
		};
		// The authoritative ledger of one External equipment load group.
		// Allocation runs in two passes: protected critical floors first,
		// then discretionary demand, both in the declared deterministic
		// order (priority descending, declaration order for ties). A load
		// that requires a cooling service is never admitted in this slice:
		// it draws no power and its cooling need stays visible as unmet
		// cooling rather than being silently supplied.
		//
		// The critical floor and the minimum operable level are read
		// together (#549): the floor is a protected slice of this load's
		// own service, not an independently operable sub-service, so
		// protecting it means running the load — and the load cannot run
		// below demand * (1 - allowed curtailment). The protected claim is
		// therefore the larger of the floor and that minimum, and it is
		// granted only when the pools can actually reach the minimum. A
		// claim that cannot reach it is refused whole: the load takes no
		// generation and no stored energy, produces no heat and no
		// material exchange, and its whole floor stays explicit unmet
		// critical demand.
		struct LoadExchangeState
		{
			size_t storeIndex{0};
			double ratePerSecond{0.0};
			bool consumes{true};
			double lastRequested{0.0};
			double lastCompleted{0.0};
			// The amount held back against the staged inventory at the load's
			// capped operating level when a grant was made (#554); the
			// unused remainder is returned at booking.
			double lastReserved{0.0};
			double lastUnmet{0.0};
		};
		struct LoadState
		{
			EquipmentLoadSettings authored;
			std::vector<LoadExchangeState> exchanges;
			// #395: the reservoir this load's emitted heat routes into.
			// A routed load's heat is booked into the reservoir exactly
			// once and never also as direct export.
			bool hasHeatSink{false};
			size_t heatSinkIndex{0};
			double lastHeatRoutedKW{0.0};
			double lastProfileLevel{0.0};
			double lastRequestedKW{0.0};
			double lastSuppliedKW{0.0};
			double lastUnmetKW{0.0};
			double lastUnmetCriticalKW{0.0};
			double lastDeliveredFraction{0.0};
			// #554: the fraction of the load's full-operation material
			// exchanges its declared feed and destination limits sustained
			// this tick; electrical allocation ran against
			// lastRequestedKW * lastMaterialFraction.
			double lastMaterialFraction{1.0};
			double lastCappedRequestKW{0.0};
			bool lastMaterialComputed{false};
			bool lastMaterialRefused{false};
			bool lastMaterialReserved{false};
			double lastHeatExportedKW{0.0};
			double lastUnmetCoolingKW{0.0};
			bool lastAdmitted{false};
			std::string lastBlockReason;
			std::string lastShortfallReason;
			bool lastCriticalUnmet{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one configured battery bank (#394).
		// Discharge covers the load demand generation could not reach, on
		// the same allocation pool — never a second budget — and charging
		// takes only the surplus no load took. The reserve floor
		// (nameplate - usable) is never dischargeable, and both conversion
		// directions book their losses so free energy never appears.
		struct BatteryState
		{
			BatterySettings authored;
			double stored{0.0};
			double lastChargeKW{0.0};
			double lastDischargeKW{0.0};
			double lastDischargeRequestedKW{0.0};
			double lastDischargeShortfallKW{0.0};
			double chargedKWh{0.0};
			double dischargedKWh{0.0};
			double deliveredKWh{0.0};
			double chargeInputKWh{0.0};
			bool lastAtReserve{false};
			bool lastSaturated{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored resupply delivery (#392).
		// A delivery is an external arrival, not a station process: what the
		// destination can accept enters the station's delivered ledger, and
		// what it cannot is refused at the boundary and stays accounted as
		// rejected material. Nothing a delivery brings is ever silently
		// overflowed, absorbed or dropped.
		struct DeliveryState
		{
			ResourceDeliverySettings authored;
			size_t storeIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastScheduled{0.0};
			double lastAccepted{0.0};
			double lastRejected{0.0};
			double acceptedTotal{0.0};
			double rejectedTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored terminal-waste
		// ejection (#403). An ejection is an external departure, not a
		// station process: it removes only disposal-ready (qualified)
		// material the store actually holds, capped at the scheduled
		// quantity, and what it removes is booked as an external loss.
		// Held (unqualified) waste is never touched, and a shortfall —
		// scheduling more than the store can give — is a valid outcome,
		// never a failure and never a deletion of held material.
		struct WasteEjectionState
		{
			WasteEjectionSettings authored;
			size_t storeIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastScheduled{0.0};
			double lastEjected{0.0};
			double lastUnmet{0.0};
			double ejectedTotal{0.0};
			double unmetTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored radiator (#395).
		// Rejection is recomputed every tick from the host reservoir's
		// emitting temperature; the booked value is the net radiative
		// exchange after the reservoir's sink clamp.
		struct RadiatorState
		{
			RadiatorSettings authored;
			size_t reservoirIndex{0};
			double lastRejectedKW{0.0};
		};
		// The authoritative ledger of one thermal reservoir (#395).
		// Temperature is integrated explicitly on the fixed World tick,
		// so the same tick sequence always integrates the same
		// temperature, and the energy ledger closes against capacity x
		// temperature: starting + absorbed + routed - rejected must
		// equal the current energy within the declared tolerance. The
		// declared min/max service limits are reported as crossings,
		// never enforced by clamping the integrated state.
		struct ReservoirState
		{
			ThermalReservoirSettings authored;
			std::vector<size_t> radiatorIndices;
			std::vector<size_t> bufferIndices;
			double temperatureC{0.0};
			double lastHeatRoutedKW{0.0};
			double lastLoopHeatKW{0.0};
			double lastCondenserHeatKW{0.0};
			double lastGeneratorHeatKW{0.0};
			double lastElectrolysisHeatKW{0.0}; // cell process heat routed here this tick (#404)
			double lastFuelCellHeatKW{0.0}; // fuel-cell waste heat routed here this tick (#406)
			double lastSabatierHeatKW{0.0}; // Sabatier reaction + dose heat routed here this tick (#405)
			double lastMethaneProcessingHeatKW{0.0}; // methane-processing waste heat routed here this tick (#405)
			double lastMicrobialHeatKW{0.0}; // microbial digestion reaction + dose heat routed here this tick (#407)
			double lastAtmosphereHeatKW{0.0}; // reservoir side of the compartment exchanger this tick
			double lastBufferChargeKW{0.0}; // heat absorbed out of the reservoir by buffers this tick (#398)
			double lastBufferDischargeKW{0.0}; // heat returned into the reservoir by buffers this tick (#398)
			double lastHeatInKW{0.0};
			double lastRejectedKW{0.0};
			double lastNetKW{0.0};
			double absorbedKJ{0.0};
			double routedKJ{0.0};
			double loopKJ{0.0};
			double condenserKJ{0.0};
			double generatorKJ{0.0};
			double electrolysisKJ{0.0}; // cumulative electrolysis process heat into the reservoir
			double fuelCellKJ{0.0}; // cumulative fuel-cell waste heat into the reservoir (#406)
			double sabatierKJ{0.0}; // cumulative Sabatier reaction + dose heat into the reservoir (#405)
			double methaneProcessingKJ{0.0}; // cumulative methane-processing waste heat into the reservoir (#405)
			double microbialKJ{0.0}; // cumulative microbial digestion reaction + process heat into the reservoir (#407)
			double atmosphereKJ{0.0}; // cumulative net compartment exchange into the reservoir
			double bufferChargeKJ{0.0}; // cumulative heat absorbed out of the reservoir by buffers (#398)
			double bufferDischargeKJ{0.0}; // cumulative heat returned into the reservoir by buffers (#398)
			double rejectedKJ{0.0};
			bool lastAboveLimit{false};
			bool lastBelowLimit{false};
		};
		// The authoritative ledger of one authored coolant loop (#396).
		// The loop owns a finite, loop-specific coolant inventory; the
		// pump circulates only the coolant it actually holds, so an
		// underfilled loop circulates proportionally less and an empty
		// loop circulates nothing — flow is never invented. The freeze-
		// protection heater is a bang-bang thermostat: it demands up to
		// its capacity whenever the loop temperature is below the setpoint.
		// Pump work and heater heat enter the loop exactly once and reach
		// the reservoir only through the circulation-gated exchanger, so
		// the electrical-to-thermal booking never duplicates. The loop's
		// energy ledger closes against capacity x temperature on the same
		// 0 degrees C reference as the reservoirs.
		struct LoopState
		{
			CoolantLoopSettings authored;
			size_t reservoirIndex{0};
			double coolantKg{0.0};
			double temperatureC{0.0};
			double lastPumpDemandKW{0.0};
			double lastPumpSuppliedKW{0.0};
			double lastHeaterDemandKW{0.0};
			double lastHeaterSuppliedKW{0.0};
			double lastCirculationFraction{0.0};
			double lastFlowKgPerS{0.0};
			double lastExchangeIntoLoopKW{0.0};
			// Water-recovery latent heat deposited on this loop's cold surface
			// this tick and cumulatively (#402): an inflow term of the loop's
			// own energy ledger, booked once and never into the compartment.
			double lastCollectorHeatKW{0.0};
			double collectorHeatKJ{0.0};
			double lastChillerExtractionKW{0.0};
			double pumpWorkKJ{0.0};
			double heaterKJ{0.0};
			double exchangedKJ{0.0};
			double extractedKJ{0.0};
			bool lastBelowFreeze{false};
			bool lastCirculating{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored chiller (#397). The
		// chiller supplies its class's cold service by extracting heat
		// from its cold-side coolant loop, capped by the loop's actual
		// circulation, gated by the chilled setpoint on the cold side and
		// the maximum condensing temperature on the sink side. Electrical
		// work plus auxiliaries are drawn from the shared generation-and-
		// battery pool; the condenser rejects extracted heat plus the
		// work and auxiliaries actually supplied into the sink reservoir.
		// The chiller's energy ledger closes over extracted + work =
		// condenser within the declared tolerance: one consistent
		// boundary, never a duplicate booking.
		struct ChillerState
		{
			ChillerSettings authored;
			size_t loopIndex{0};
			size_t sinkIndex{0};
			double lastCoolingDemandKW{0.0};
			double lastCoolingKW{0.0};
			double lastDeliverableKW{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastCondenserKW{0.0};
			double lastDeliveredFraction{0.0};
			bool lastAdmitted{false};
			std::string lastBlockReason;
			bool lastAtSetpoint{false};
			bool lastSinkOver{false};
			double extractedKJ{0.0};
			double workKJ{0.0};
			double condenserKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored thermal buffer (#398).
		// The store holds a finite amount of thermal energy, distinct from
		// any temperature: its temperature is derived from the stored
		// fraction across the authored usable band. A charge moves heat
		// from the host reservoir into the store, capped by the host's
		// remaining cooling deficit, the configured rate limit, the
		// store's headroom and the thermodynamic direction; a discharge
		// returns stored heat into the host, capped by the remaining
		// rejection surplus, the rate limit, the stored energy and the
		// direction. A circulation-access buffer is reachable only while
		// its gating coolant loop circulates, so a blackout cannot absorb
		// through a powered-only store for free; a passive buffer is
		// always reachable but conductance-limited. The storage ledger
		// closes over starting + charged - discharged = stored within the
		// declared tolerance: the store never resets silently and never
		// leaves its finite bounds.
		struct BufferState
		{
			ThermalBufferSettings authored;
			size_t reservoirIndex{0};
			size_t gateLoopIndex{0}; // only meaningful in circulation mode
			double storedKJ{0.0};
			bool lastAccessible{false};
			double lastChargeKW{0.0};
			double lastDischargeKW{0.0};
			bool lastFull{false};
			bool lastDepleted{false};
			std::string lastLimiter;
			double chargedKJ{0.0};
			double dischargedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored generator (#399). The
		// generator converts finite stored fuel energy into bus electrical
		// output at the declared efficiency; the difference is waste heat
		// routed exactly once into the sink reservoir, alongside the
		// cranking dissipation while starting and the configured decay
		// heat after output stops. The fuel ledger closes over
		// fuel = electrical + waste within the declared tolerance: fuel
		// never vanishes without becoming output or heat. Startup only
		// completes on energy actually received from the shared pool while
		// cooling is qualified, so a generator never self-starts on output
		// it has not yet made.
		struct GeneratorState
		{
			GeneratorSettings authored;
			size_t fuelStoreIndex{0};
			size_t sinkIndex{0};
			StationGeneratorPhase phase{StationGeneratorPhase::Stopped};
			double outputKW{0.0};
			double lastFuelDrawKWh{0.0};
			double lastWasteHeatKW{0.0};
			double lastStartupDrawKW{0.0};
			double lastStartupUnmetKW{0.0};
			double lastDecayHeatKW{0.0};
			double startupReceivedKJ{0.0};
			double shutdownElapsedSeconds{0.0};
			bool lastCoolingAvailable{false};
			bool lastCoolingRequired{false};
			std::string lastBlockReason;
			double fuelEnergyKJ{0.0};
			double electricalKJ{0.0};
			double wasteHeatKJ{0.0};
			double startupHeatKJ{0.0};
			double decayHeatKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored compartment atmosphere
		// (#400). The compartment owns a finite gas inventory in a fixed
		// volume; occupancy draws oxygen capped by what is actually there
		// and scales CO2 production and metabolic heat with the fraction
		// consumed; water vapour is capped by saturation at the
		// compartment's own integrated temperature through the coupled
		// phase/thermal solve (#564), the condensate releasing its
		// latent heat into the same thermal state that sets the cap;
		// leakage is a constant total-mass
		// rate capped by the inventory and split composition-
		// proportionally. Every removal pathway books its per-component
		// amounts so the per-component ledger closes exactly: starting +
		// added - removed = current within the declared tolerance, and
		// the thermal ledger closes over occupant heat, latent heat and
		// the reservoir exchange against capacity x temperature. The
		// compartment integrates explicitly on the fixed World tick, so
		// the same tick sequence always integrates the same state.
		struct AtmosphereState
		{
			AtmosphereSettings authored;
			size_t reservoirIndex{SIZE_MAX}; // SIZE_MAX: adiabatic, no coupling
			double oxygenKg{0.0};
			double nitrogenKg{0.0};
			double carbonDioxideKg{0.0};
			double waterVapourKg{0.0};
			double temperatureC{0.0};
			double startingPressureKPa{0.0};
			// Cumulative per-component removals by pathway, the closure
			// terms of the per-component conservation audit.
			double leakOxygenKg{0.0};
			double leakNitrogenKg{0.0};
			double leakCarbonDioxideKg{0.0};
			double leakWaterVapourKg{0.0};
			double cycleOxygenKg{0.0};
			double cycleNitrogenKg{0.0};
			double cycleCarbonDioxideKg{0.0};
			double cycleWaterVapourKg{0.0};
			// Cumulative pathway totals.
			double oxygenConsumedKg{0.0};
			double co2ProducedKg{0.0};
			double vapourAddedKg{0.0};
			double condensedKg{0.0};
			double leakedKg{0.0};
			// Vapour removed by water recovery (#402): a booked disposal
			// pathway of its own, so the condensate never reads as vanished.
			double collectedKg{0.0};
			double lastCollectedKg{0.0};
			// Oxygen added by an electrolyzer delivering into this
			// compartment (#404): a booked addition pathway, so the O2 the
			// cell produced is visible in the compartment's oxygen closure.
			double electrolysisOxygenKg{0.0};
			double lastElectrolysisOxygenKg{0.0};
			// CO2 removed by a capture machine (#404): a booked removal
			// pathway into the capture store, so no carbon vanishes.
			double capturedCarbonDioxideKg{0.0};
			double lastCo2CapturedKg{0.0};
			double recoveredKg{0.0};
			double lostKg{0.0};
			// Thermal ledger terms.
			double occupantHeatKJ{0.0};
			double latentHeatKJ{0.0};
			double exchangedKJ{0.0};
			// Metabolic heat routed into this compartment by authored crews
			// (#401): booked as its own thermal term, never folded into the
			// compartment's own occupancy heat, and the thermal ledger
			// closes over it.
			double crewHeatKJ{0.0};
			double lastCrewHeatKW{0.0};
			// Last-tick observables.
			double lastOxygenDemandKg{0.0};
			double lastOxygenConsumedKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastVapourAddedKg{0.0};
			double lastVapourCondensedKg{0.0};
			double lastLeakedKg{0.0};
			double lastCycleRemovedKg{0.0};
			double lastCycleRecoveredKg{0.0};
			double lastCycleLostKg{0.0};
			double lastOccupantHeatKW{0.0};
			double lastLatentHeatKW{0.0};
			double lastExchangeIntoCompartmentKW{0.0};
			bool lastAbovePressure{false};
			bool lastBelowPressure{false};
			bool lastLowOxygen{false};
			bool lastHighCo2{false};
			bool lastCondensing{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one scheduled airlock cycle (#400).
		// Like a resupply delivery it is a scenario event, not a station
		// process: occurrences are derived from the authored schedule and
		// fire in the shared deterministic order. Each occurrence removes
		// the authored mass capped by what the compartment holds, and
		// splits it into recovered and lost by the authored fraction. A
		// cycle that finds no gas is refused whole and still counts as an
		// occurrence, so a schedule never silently disappears.
		struct AirlockCycleState
		{
			AirlockCycleSettings authored;
			size_t atmosphereIndex{0};
			size_t declarationIndex{0};
			uint64_t occurrencesFired{0};
			uint64_t lastFiredTick{0};
			double lastOccurrenceSeconds{0.0};
			double lastRemovedKg{0.0};
			double lastRecoveredKg{0.0};
			double lastLostKg{0.0};
			double removedTotal{0.0};
			double recoveredTotal{0.0};
			double lostTotal{0.0};
			bool exhausted{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored crew (#401). The crew
		// is an authored population with an activity profile, never
		// derived from World Agents. Its metabolism runs at one uniform
		// metabolic scale per tick: the smallest of the fractions of
		// each input the declared sources can actually supply (food and
		// water above their protected reserves, the compartment's oxygen
		// actually present) and each output the destination stores can
		// actually accept (urine and organic-waste headroom). Every
		// pathway - consumption and return alike - scales by that one
		// number, so absent material never generates nominal waste and
		// a backed-up waste tank never lets consumption run away from
		// its returns. Requested and actual are recorded per pathway;
		// the cumulative ledger closes over consumed = returned +
		// retained body mass, with the retained term explicit.
		struct CrewState
		{
			CrewSettings authored;
			size_t foodStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t urineStoreIndex{0};
			size_t organicWasteStoreIndex{0};
			size_t atmosphereIndex{0};
			double lastActivityLevel{0.0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastFoodRequested{0.0};
			double lastFoodActual{0.0};
			double lastWaterRequested{0.0};
			double lastWaterActual{0.0};
			double lastOxygenRequestedKg{0.0};
			double lastOxygenActualKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastMoistureProducedKg{0.0};
			double lastUrineRequested{0.0};
			double lastUrineActual{0.0};
			double lastOrganicWasteRequested{0.0};
			double lastOrganicWasteActual{0.0};
			double lastHeatKW{0.0};
			double foodConsumed{0.0};
			double waterConsumed{0.0};
			double oxygenConsumedKg{0.0};
			double co2ProducedKg{0.0};
			double moistureProducedKg{0.0};
			double urineProduced{0.0};
			double organicWasteProduced{0.0};
			double heatKJ{0.0};
			double retainedKg{0.0};
			bool lastUnmet{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored water collector (#402).
		// Collection is gated on the coolant loop's actual circulation and
		// on the cold surface being colder than the compartment air; the
		// condensate is capped by the vapour held above the humidity target
		// and by the destination store's headroom. The latent heat the vapour
		// gave up is booked into the loop's ledger and never into the
		// compartment's, so one condensation is one heat source, not two.
		struct WaterCollectorState
		{
			WaterCollectorSettings authored;
			size_t atmosphereIndex{0};
			size_t loopIndex{0};
			size_t destStoreIndex{0};
			double lastRequestedKg{0.0};
			double lastCondensableKg{0.0};
			double lastCollectedKg{0.0};
			double lastLatentHeatKW{0.0};
			double lastCirculationFraction{0.0};
			bool lastColdSurfaceAvailable{false};
			std::string lastBlockReason;
			double collectedTotalKg{0.0};
			double latentHeatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored water treatment (#402).
		// One uniform scale per tick: the smallest fraction of the rated
		// throughput that the drawable raw water, the treated and
		// concentrate headroom and the drawable consumables can sustain.
		// Processing only runs if it can be paid for: with no electrical
		// supply at all the tick is refused whole and nothing is drawn or
		// moved. Curtailed supply runs the process but holds the output
		// unqualified - a treatment that never received its full energy dose
		// has not finished treating. The concentrate lands on the unqualified
		// side of its store: retained, accounted, and unusable until a
		// disposal capability exists.
		struct WaterTreatmentState
		{
			WaterTreatmentSettings authored;
			size_t sourceIndex{0};
			size_t treatedIndex{0};
			size_t concentrateIndex{0};
			size_t consumableIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastProcessedKg{0.0};
			double lastTreatedKg{0.0};
			double lastConcentrateKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			bool lastQualityAccepted{false};
			std::string lastHoldReason;
			double processedTotalKg{0.0};
			double treatedTotalKg{0.0};
			double acceptedTotalKg{0.0};
			double heldTotalKg{0.0};
			double concentrateTotalKg{0.0};
			double energySuppliedKJ{0.0};
			double energyOwedKJ{0.0};
			double consumablesUsedTotal{0.0};
			bool lastCurtailed{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored electrolysis cell (#404).
		// The cell splits qualified water above the feed store's protected
		// reserve into oxygen and hydrogen at the declared stoichiometry;
		// the electrical dose must cover the chemical energy the produced
		// hydrogen retains, and the difference is process heat routed
		// exactly once into the class-matched sink. The cell only splits
		// what the actually supplied energy can make, so curtailed power
		// reduces production instead of gifting products for free, and a
		// full product store caps production instead of venting. Cooling
		// is gated on the sink temperature: at or above the maximum the
		// cell is refused whole and spends nothing. Oxygen lands either in
		// the qualified gas store or in the delivery compartment, where
		// the compartment's own oxygen closure accounts for every gram.
		struct ElectrolyzerState
		{
			ElectrolyzerSettings authored;
			size_t waterStoreIndex{0};
			size_t oxygenStoreIndex{SIZE_MAX}; // SIZE_MAX: delivering to the compartment
			size_t hydrogenStoreIndex{0};
			size_t atmosphereIndex{SIZE_MAX}; // SIZE_MAX: delivering to the store
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastSplitKg{0.0};
			double lastOxygenKg{0.0};
			double lastHydrogenKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProductionKg{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double waterSplitKg{0.0};
			double oxygenProducedKg{0.0};
			double hydrogenProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double chemicalEnergyKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored CO2 capture machine
		// (#404). Capture is finite on every axis - rated rate, the CO2
		// actually present, the capture store's headroom and the drawable
		// consumable - and the unmet atmospheric service is the explicit
		// remainder. Captured mass leaves the compartment through a booked
		// removal pathway and occupies the capture store as inventory; a
		// tick that demands power and receives none is refused whole,
		// drawing no CO2 and no consumable.
		struct Co2CaptureState
		{
			Co2CaptureSettings authored;
			size_t atmosphereIndex{0};
			size_t captureStoreIndex{0};
			size_t consumableStoreIndex{SIZE_MAX}; // SIZE_MAX: the process uses no consumable
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastCapturedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			std::string lastBlockReason;
			double capturedTotalKg{0.0};
			double energySuppliedKJ{0.0};
			double consumablesUsedTotal{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// A fuel-cell backup unit's authoritative ledger (#406): the cell
		// runs on the supply side in declaration order, its output joining
		// the shared generation pool, its reactants drawn only from its own
		// dedicated stores above their protected reserves.
		struct FuelCellState
		{
			FuelCellSettings authored;
			size_t hydrogenStoreIndex{0};
			size_t oxygenStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastOutputKW{0.0};
			double lastHydrogenKg{0.0};
			double lastOxygenKg{0.0};
			double lastWaterKg{0.0};
			double lastWasteHeatKW{0.0};
			double lastUnmetOutputKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastLimiter;
			std::string lastBlockReason;
			double hydrogenConsumedKg{0.0};
			double oxygenConsumedKg{0.0};
			double waterProducedKg{0.0};
			double chemicalEnergyKJ{0.0};
			double electricalKJ{0.0};
			double wasteHeatKJ{0.0};
			double enduranceSeconds{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored Sabatier reactor
		// (#405). The reactor converts captured CO2 and hydrogen into
		// methane and water at the declared stoichiometry; the reaction is
		// exothermic, so the heat the products do not retain rides into
		// the class-matched sink together with the electrical dose the
		// reactor drew, booked exactly once. The reactor only converts the
		// CO2 the actually supplied energy can pay for at the declared
		// dose, feeds are drawn only above each store's protected reserve,
		// and a full product store caps conversion rather than venting. A
		// sink at or above the maximum sink temperature refuses the
		// reactor whole and it spends nothing.
		struct SabatierState
		{
			SabatierReactorSettings authored;
			size_t co2StoreIndex{0};
			size_t hydrogenStoreIndex{0};
			size_t methaneStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastCo2ConsumedKg{0.0};
			double lastHydrogenConsumedKg{0.0};
			double lastMethaneProducedKg{0.0};
			double lastWaterProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetConversionKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double co2ConsumedKg{0.0};
			double hydrogenConsumedKg{0.0};
			double methaneProducedKg{0.0};
			double waterProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored methane processor
		// (#405). The processor splits methane into recovered hydrogen
		// and solid carbon at the declared stoichiometry; the reaction is
		// endothermic, so the authored dose must cover the declared
		// reaction heat demand and the difference is process heat routed
		// exactly once into the class-matched sink. The recovered
		// hydrogen returns to the hydrogen store as recycled feed - the
		// reactor consumes twice as much per methane as the processor
		// returns, so the combined loop retains a net hydrogen demand.
		// A full hydrogen or carbon destination caps processing rather
		// than venting, and a sink at or above the maximum sink
		// temperature refuses the processor whole.
		struct MethaneProcessorState
		{
			MethaneProcessorSettings authored;
			size_t methaneStoreIndex{0};
			size_t hydrogenStoreIndex{0};
			size_t carbonStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastMethaneConsumedKg{0.0};
			double lastHydrogenReturnedKg{0.0};
			double lastCarbonProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProcessingKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastCoolingAvailable{false};
			std::string lastBlockReason;
			double methaneConsumedKg{0.0};
			double hydrogenReturnedKg{0.0};
			double carbonProducedKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		// The authoritative ledger of one authored microbial processor
		// (#407). The processor digests a compound organic-waste feed and
		// recovers its bound N/P/K/minerals at the declared per-
		// constituent yields, its moisture at the declared water recovery,
		// the declared CO2 share, and the unrecoverable residue as the
		// mass-balance remainder - so the digestion closes on mass by
		// construction. It consumes oxygen, process water, consumable
	// medium and electricity per kilogram of feed, and routes its
		// exothermic reaction heat plus the electrical dose exactly once
		// into the class-matched sink. Feeds draw only qualified material
		// above each store's protected reserve; a saturated destination
		// backpressures the whole stage through the uniform scale; the
		// static operating window (minimum operating temperature up to
		// the maximum sink temperature) gates eligibility against the
		// sink reservoir without any dynamic culture condition.
		struct MicrobialProcessorState
		{
			MicrobialProcessorSettings authored;
			size_t feedStoreIndex{0};
			size_t oxygenStoreIndex{0};
			size_t waterStoreIndex{0};
			size_t consumableStoreIndex{0};
			size_t nutrientStoreIndex{0};
			size_t recoveredWaterStoreIndex{0};
			size_t co2StoreIndex{0};
			size_t residueStoreIndex{0};
			size_t sinkIndex{0};
			size_t declarationIndex{0};
			double lastScale{0.0};
			std::string lastLimiter;
			double lastRequestedKg{0.0};
			double lastFeedConsumedKg{0.0};
			double lastOxygenConsumedKg{0.0};
			double lastWaterConsumedKg{0.0};
			double lastConsumableRequested{0.0};
			double lastConsumableUsed{0.0};
			double lastConsumableUnmet{0.0};
			double lastNutrientProducedKg{0.0};
			double lastRecoveredWaterKg{0.0};
			double lastCo2ProducedKg{0.0};
			double lastResidueProducedKg{0.0};
			double lastPowerDemandKW{0.0};
			double lastPowerSuppliedKW{0.0};
			double lastUnmetProcessingKg{0.0};
			double lastReactionHeatKW{0.0};
			bool lastTemperatureEligible{false};
			std::string lastBlockReason;
			double feedConsumedKg{0.0};
			double oxygenConsumedKg{0.0};
			double waterConsumedKg{0.0};
			double consumablesUsed{0.0};
			double nutrientProducedKg{0.0};
			double recoveredWaterKg{0.0};
			double co2ProducedKg{0.0};
			double residueProducedKg{0.0};
			double nitrogenRecoveredKg{0.0};
			double phosphorusRecoveredKg{0.0};
			double potassiumRecoveredKg{0.0};
			double mineralsRecoveredKg{0.0};
			double energySuppliedKJ{0.0};
			double reactionHeatKJ{0.0};
			double heatRoutedKJ{0.0};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		StationSystemsSettings settings;
		std::vector<StoreState> stores;
		std::vector<TransferState> transfers;
		std::vector<ConversionState> conversions;
		std::vector<SolarState> solarArrays;
		std::vector<LoadState> loads;
		std::vector<BatteryState> batteries;
		std::vector<DeliveryState> deliveries;
		// Authored terminal-waste ejection schedules (#403), run in the
		// same scenario-event pass as the deliveries so one ordering
		// governs coincident arrivals and departures.
		std::vector<WasteEjectionState> wasteEjections;
		std::vector<ReservoirState> reservoirs;
		std::vector<RadiatorState> radiators;
		std::vector<LoopState> coolantLoops;
		std::vector<ChillerState> chillers;
		std::vector<BufferState> thermalBuffers;
		std::vector<GeneratorState> generators;
		std::vector<AtmosphereState> atmospheres;
		std::vector<AirlockCycleState> airlockCycles;
		std::vector<CrewState> crews;
		std::vector<WaterCollectorState> waterCollectors;
		std::vector<WaterTreatmentState> waterTreatments;
		std::vector<ElectrolyzerState> electrolyzers;
		std::vector<Co2CaptureState> co2Captures;
		std::vector<FuelCellState> fuelCells;
		std::vector<SabatierState> sabatierReactors;
		std::vector<MethaneProcessorState> methaneProcessors;
		std::vector<MicrobialProcessorState> microbialProcessors;
		// Electrolyzers and capture machines compete for the same power pool
		// and the same stores, so each runs in the declared deterministic
		// order: priority descending, declaration order ascending.
		std::vector<size_t> electrolyzerOrder;
		std::vector<size_t> co2CaptureOrder;
		// Carbon-recovery stages compete for the same power pool and share
		// the hydrogen and methane stores, so each runs in the declared
		// deterministic order: priority descending, declaration order
		// ascending (#405).
		std::vector<size_t> sabatierOrder;
		std::vector<size_t> methaneProcessorOrder;
		// Microbial processors compete for the same power pool and share
		// the waste, water and CO2 stores, so each runs in the declared
		// deterministic order: priority descending, declaration order
		// ascending (#407).
		std::vector<size_t> microbialProcessorOrder;
		// Treatments compete for the same power pool and the same stores, so
		// they run in the declared deterministic order: priority descending,
		// declaration order ascending.
		std::vector<size_t> waterTreatmentOrder;
		// Competing transfers are allocated in priority order (higher first),
		// ties resolved by declaration order: a declared deterministic order.
		std::vector<size_t> transferOrder;
		// Conversions run after all transfers within the same tick, in the
		// same declared deterministic order.
		std::vector<size_t> conversionOrder;
		// Electrical supply is allocated to external loads before resource
		// transfers and conversions run, in the same declared deterministic
		// order, so allocation never depends on iteration accidents.
		std::vector<size_t> loadOrder;
		// Per-unit totals at run start, the conservation boundary (#390),
		// plus the cumulative per-unit mass removed by declared conversion
		// losses (#391): each unit's ledger closes over its own consumed
		// and produced amounts, even across mixed g/kg conversions.
		std::map<std::string, double> startingTotals;
		std::map<std::string, double> unitLoss;
		// Cumulative mass of each unit admitted from outside the station by
		// accepted deliveries (#392). The per-unit boundary closes over it:
		// current + lost - delivered must equal starting.
		std::map<std::string, double> unitDelivered;
		// Cumulative mass of each unit recovered out of a compartment
		// atmosphere by a water collector (#402): the third external side
		// of the per-unit boundary. Condensate arrives from outside the store
		// network just as a delivery does, so the boundary closes as
		// current + lost - delivered - collected == starting.
		std::map<std::string, double> unitCollected;
		// Per-constituent totals at run start and the cumulative declared
		// conversion losses, the compound conservation boundary (#391):
		// current + lost must equal starting within tolerance.
		ConstituentLedger startingConstituents;
		ConstituentLedger constituentLoss;
		// Cumulative constituent admitted by accepted deliveries (#392),
		// the compound side of the same external boundary.
		ConstituentLedger constituentDelivered;
		// Generation left untaken after this tick's allocation and battery
		// charging, for the power snapshot's surplus reporting.
		double lastAvailableGenerationKW{0.0};
		// Discharge need routed to the battery bank this tick that the bank
		// could not deliver (#394). Tracked at allocation level so the bank
		// shortfall never double-counts per-battery routing.
		double lastBatteryShortfallKW{0.0};
		std::array<std::unique_ptr<ScriptObject>, 4> objects;
		StationSystemsSnapshot snapshot;
		std::vector<StationSystemsEvent> events;
		std::vector<StationSystemsDiagnostic> diagnostics;

		// Run the ledger for one tick and roll it back whole unless every
		// audit passed (#555). The ledger mutates the live transfer,
		// conversion, load, solar and battery state while it runs, so a
		// failed audit restores every one of those objects, and the
		// allocation scalars derived from them, to the last committed
		// state. Store inventory and the loss ledgers are staged and only
		// published on success, and events are only appended after the
		// audits pass; this rollback closes the remaining in-place mutation
		// so a failed tick can never expose a partial allocation, energy
		// draw or flow result through the public snapshot. The violated
		// subsystem is reported through `system` so the structured
		// diagnostic names what failed (#553).
		bool applyLedger(uint64_t tick, float seconds, std::string& system, std::string& violation)
		{
			system = "Resource transfer";
			auto const committedTransfers = transfers;
			auto const committedConversions = conversions;
			auto const committedLoads = loads;
			auto const committedSolarArrays = solarArrays;
			auto const committedBatteries = batteries;
			auto const committedDeliveries = deliveries;
			auto const committedWasteEjections = wasteEjections;
			auto const committedReservoirs = reservoirs;
			auto const committedRadiators = radiators;
			auto const committedCoolantLoops = coolantLoops;
			auto const committedChillers = chillers;
			auto const committedThermalBuffers = thermalBuffers;
			auto const committedGenerators = generators;
			auto const committedAtmospheres = atmospheres;
			auto const committedAirlockCycles = airlockCycles;
			auto const committedCrews = crews;
			auto const committedWaterCollectors = waterCollectors;
			auto const committedWaterTreatments = waterTreatments;
			auto const committedElectrolyzers = electrolyzers;
			auto const committedCo2Captures = co2Captures;
			auto const committedFuelCells = fuelCells;
			auto const committedSabatierReactors = sabatierReactors;
			auto const committedMethaneProcessors = methaneProcessors;
			auto const committedMicrobialProcessors = microbialProcessors;
			auto const committedAvailableGenerationKW = lastAvailableGenerationKW;
			auto const committedBatteryShortfallKW = lastBatteryShortfallKW;
			if (runLedger(tick, seconds, system, violation)) return true;
			transfers = committedTransfers;
			conversions = committedConversions;
			loads = committedLoads;
			solarArrays = committedSolarArrays;
			batteries = committedBatteries;
			deliveries = committedDeliveries;
			wasteEjections = committedWasteEjections;
			reservoirs = committedReservoirs;
			radiators = committedRadiators;
			coolantLoops = committedCoolantLoops;
			chillers = committedChillers;
			thermalBuffers = committedThermalBuffers;
			generators = committedGenerators;
			atmospheres = committedAtmospheres;
			airlockCycles = committedAirlockCycles;
			crews = committedCrews;
			waterCollectors = committedWaterCollectors;
			waterTreatments = committedWaterTreatments;
			electrolyzers = committedElectrolyzers;
			co2Captures = committedCo2Captures;
			fuelCells = committedFuelCells;
			sabatierReactors = committedSabatierReactors;
			methaneProcessors = committedMethaneProcessors;
			microbialProcessors = committedMicrobialProcessors;
			lastAvailableGenerationKW = committedAvailableGenerationKW;
			lastBatteryShortfallKW = committedBatteryShortfallKW;
			return false;
		}
		// Apply every authored transfer and conversion to a staged copy of
		// the inventory in the declared deterministic order, transfers
		// first, then conversions, each by priority descending and
		// declaration order; audit the staged result against finite
		// limits, the per-unit conservation boundary and the per-constituent
		// compound boundary, and only then commit. A violation never clamps
		// material into existence or out of it: it fails the whole tick
		// through the integrated error path, and the applyLedger wrapper
		// restores the live ledger state this pass mutated in place (#555).
		bool runLedger(uint64_t tick, float seconds, std::string& system, std::string& violation)
		{
			std::vector<StationTransferStatus> previousStatus;
			previousStatus.reserve(transfers.size());
			for (auto const& transfer : transfers) previousStatus.push_back(transfer.status);
			std::vector<StationTransferStatus> previousConversionStatus;
			previousConversionStatus.reserve(conversions.size());
			for (auto const& conversion : conversions) previousConversionStatus.push_back(conversion.status);
			std::vector<StationTransferStatus> previousLoadStatus;
			previousLoadStatus.reserve(loads.size());
			for (auto const& load : loads) previousLoadStatus.push_back(load.status);
			std::vector<bool> previousCriticalUnmet;
			previousCriticalUnmet.reserve(loads.size());
			for (auto const& load : loads) previousCriticalUnmet.push_back(load.lastCriticalUnmet);
			std::vector<bool> previousEclipse;
			previousEclipse.reserve(solarArrays.size());
			for (auto const& array : solarArrays) previousEclipse.push_back(array.lastEclipse);
			std::vector<StationTransferStatus> previousBatteryStatus;
			previousBatteryStatus.reserve(batteries.size());
			for (auto const& battery : batteries) previousBatteryStatus.push_back(battery.status);
			std::vector<bool> previousBatteryReserve;
			previousBatteryReserve.reserve(batteries.size());
			for (auto const& battery : batteries) previousBatteryReserve.push_back(battery.lastAtReserve);
			std::vector<bool> previousBatterySaturated;
			previousBatterySaturated.reserve(batteries.size());
			for (auto const& battery : batteries) previousBatterySaturated.push_back(battery.lastSaturated);
			std::vector<bool> previousAboveLimit;
			previousAboveLimit.reserve(reservoirs.size());
			for (auto const& reservoir : reservoirs) previousAboveLimit.push_back(reservoir.lastAboveLimit);
			std::vector<bool> previousBelowLimit;
			previousBelowLimit.reserve(reservoirs.size());
			for (auto const& reservoir : reservoirs) previousBelowLimit.push_back(reservoir.lastBelowLimit);
			std::vector<StationTransferStatus> previousLoopStatus;
			previousLoopStatus.reserve(coolantLoops.size());
			for (auto const& loop : coolantLoops) previousLoopStatus.push_back(loop.status);
			std::vector<bool> previousBelowFreeze;
			previousBelowFreeze.reserve(coolantLoops.size());
			for (auto const& loop : coolantLoops) previousBelowFreeze.push_back(loop.lastBelowFreeze);
			std::vector<bool> previousCirculating;
			previousCirculating.reserve(coolantLoops.size());
			for (auto const& loop : coolantLoops) previousCirculating.push_back(loop.lastCirculating);
			std::vector<StationTransferStatus> previousChillerStatus;
			previousChillerStatus.reserve(chillers.size());
			for (auto const& chiller : chillers) previousChillerStatus.push_back(chiller.status);
			std::vector<bool> previousChillerAtSetpoint;
			previousChillerAtSetpoint.reserve(chillers.size());
			for (auto const& chiller : chillers) previousChillerAtSetpoint.push_back(chiller.lastAtSetpoint);
			std::vector<bool> previousChillerSinkOver;
			previousChillerSinkOver.reserve(chillers.size());
			for (auto const& chiller : chillers) previousChillerSinkOver.push_back(chiller.lastSinkOver);
			std::vector<StationGeneratorPhase> previousGeneratorPhase;
			previousGeneratorPhase.reserve(generators.size());
			for (auto const& generator : generators) previousGeneratorPhase.push_back(generator.phase);
			std::vector<bool> previousAbovePressure;
			previousAbovePressure.reserve(atmospheres.size());
			for (auto const& atmosphere : atmospheres) previousAbovePressure.push_back(atmosphere.lastAbovePressure);
			std::vector<bool> previousBelowPressure;
			previousBelowPressure.reserve(atmospheres.size());
			for (auto const& atmosphere : atmospheres) previousBelowPressure.push_back(atmosphere.lastBelowPressure);
			std::vector<bool> previousLowOxygen;
			previousLowOxygen.reserve(atmospheres.size());
			for (auto const& atmosphere : atmospheres) previousLowOxygen.push_back(atmosphere.lastLowOxygen);
			std::vector<bool> previousHighCo2;
			previousHighCo2.reserve(atmospheres.size());
			for (auto const& atmosphere : atmospheres) previousHighCo2.push_back(atmosphere.lastHighCo2);
			std::vector<bool> previousCondensing;
			previousCondensing.reserve(atmospheres.size());
			for (auto const& atmosphere : atmospheres) previousCondensing.push_back(atmosphere.lastCondensing);
			std::vector<bool> previousCrewUnmet;
			previousCrewUnmet.reserve(crews.size());
			for (auto const& crew : crews) previousCrewUnmet.push_back(crew.lastUnmet);
			std::vector<StationTransferStatus> previousElectrolyzerStatus;
			previousElectrolyzerStatus.reserve(electrolyzers.size());
			for (auto const& cell : electrolyzers) previousElectrolyzerStatus.push_back(cell.status);
			std::vector<StationTransferStatus> previousCo2CaptureStatus;
			previousCo2CaptureStatus.reserve(co2Captures.size());
			for (auto const& capture : co2Captures) previousCo2CaptureStatus.push_back(capture.status);
			std::vector<StationTransferStatus> previousFuelCellStatus;
			previousFuelCellStatus.reserve(fuelCells.size());
			for (auto const& cell : fuelCells) previousFuelCellStatus.push_back(cell.status);
			std::vector<StationTransferStatus> previousSabatierStatus;
			previousSabatierStatus.reserve(sabatierReactors.size());
			for (auto const& reactor : sabatierReactors) previousSabatierStatus.push_back(reactor.status);
			std::vector<StationTransferStatus> previousMethaneProcessorStatus;
			previousMethaneProcessorStatus.reserve(methaneProcessors.size());
			for (auto const& processor : methaneProcessors) previousMethaneProcessorStatus.push_back(processor.status);
			std::vector<StationTransferStatus> previousMicrobialProcessorStatus;
			previousMicrobialProcessorStatus.reserve(microbialProcessors.size());
			for (auto const& processor : microbialProcessors) previousMicrobialProcessorStatus.push_back(processor.status);
			std::vector<double> qualified, unqualified;
			qualified.reserve(stores.size());
			unqualified.reserve(stores.size());
			for (auto const& store : stores)
			{
				qualified.push_back(store.currentQualified);
				unqualified.push_back(store.currentUnqualified);
			}
			std::vector<double> flowIn(stores.size(), 0.0), flowOut(stores.size(), 0.0);
			std::vector<std::string> stallReason(transfers.size());
			// The declared loss ledgers are staged up front: a failed tick
			// never books losses, exchanges or allocations it did not commit.
			ConstituentLedger stagedLoss = constituentLoss;
			std::map<std::string, double> stagedUnitLoss = unitLoss;
			// Cumulative external inflow staged alongside the losses, so a
			// failed tick never books deliveries it did not commit (#392).
			ConstituentLedger stagedConstituentDelivered = constituentDelivered;
			std::map<std::string, double> stagedUnitDelivered = unitDelivered;
			// Water recovered out of a compartment atmosphere is a third
			// external side of the per-unit boundary (#402): the condensate
			// arrives from outside the store network, so it is staged and
			// committed like a delivery and the boundary closes as
			// current + lost - delivered - collected == starting.
			std::map<std::string, double> stagedUnitCollected = unitCollected;
			// Solar supply and external equipment allocation run before the
			// resource ledger: the electrical service a load receives bounds
			// its heat and material exchanges, and allocated power can never
			// exceed the generation actually available this tick.
			auto const elapsed = static_cast<double>(tick) * World::getFixedTimestep();
			// Resupply deliveries run ahead of everything else in the tick:
			// material that lands this tick is available to this tick's
			// equipment, transfers and conversions. Every occurrence due
			// since the last committed tick is collected into one scenario
			// event list and applied in the declared deterministic order —
			// occurrence time ascending, then service priority descending,
			// then declaration order — so coincident arrivals from different
			// schedules never depend on an iteration accident, and each
			// occurrence fires exactly once however many ticks an
			// accelerated update batches together (#392).
			struct DueArrival
			{
				StationScenarioEventKey key;
				uint64_t occurrence{0};
				bool isEjection{false};
			};
			std::vector<DueArrival> arrivals;
			for (size_t index = 0; index < deliveries.size(); ++index)
			{
				auto const& delivery = deliveries[index];
				if (delivery.exhausted) continue;
				auto const due = stationScheduleOccurrencesDue(
					delivery.authored.firstOccurrenceSeconds,
					delivery.authored.recurrenceSeconds, elapsed);
				for (uint64_t occurrence = delivery.occurrencesFired; occurrence < due; ++occurrence)
				{
					auto const when = delivery.authored.recurrenceSeconds > 0.0
						? delivery.authored.firstOccurrenceSeconds
							+ static_cast<double>(occurrence) * delivery.authored.recurrenceSeconds
						: delivery.authored.firstOccurrenceSeconds;
					arrivals.push_back({{when, delivery.authored.priority, index}, occurrence});
				}
			}
			// Terminal-waste ejections due since the last committed tick join
			// the same scenario event list (#403): one ordering — occurrence
			// time ascending, then service priority descending, then
			// declaration — governs coincident arrivals and departures, and
			// each occurrence fires exactly once however many ticks an
			// accelerated update batches together.
			for (size_t index = 0; index < wasteEjections.size(); ++index)
			{
				auto const& ejection = wasteEjections[index];
				if (ejection.exhausted) continue;
				auto const due = stationScheduleOccurrencesDue(
					ejection.authored.firstOccurrenceSeconds,
					ejection.authored.recurrenceSeconds, elapsed);
				for (uint64_t occurrence = ejection.occurrencesFired; occurrence < due; ++occurrence)
				{
					auto const when = ejection.authored.recurrenceSeconds > 0.0
						? ejection.authored.firstOccurrenceSeconds
							+ static_cast<double>(occurrence) * ejection.authored.recurrenceSeconds
						: ejection.authored.firstOccurrenceSeconds;
					arrivals.push_back({{when, ejection.authored.priority, index}, occurrence, true});
				}
			}
			std::sort(arrivals.begin(), arrivals.end(),
				[](DueArrival const& left, DueArrival const& right)
				{
					if (left.key.occurrenceSeconds != right.key.occurrenceSeconds)
						return left.key.occurrenceSeconds < right.key.occurrenceSeconds;
					if (left.key.priority != right.key.priority)
						return left.key.priority > right.key.priority;
					// Same time and priority: an arrival lands before an
					// ejection runs, so material delivered this tick is
					// disposal-ready for the ejection that follows it,
					// whatever the two schedules' declaration order (#403).
					if (left.isEjection != right.isEjection)
						return !left.isEjection;
					return left.key.declarationIndex < right.key.declarationIndex;
				});
			for (auto const& arrival : arrivals)
			{
				if (arrival.isEjection)
				{
					auto& ejection = wasteEjections[arrival.key.declarationIndex];
					auto const& store = stores[ejection.storeIndex].authored;
					// Only disposal-ready (qualified) material is eligible:
					// held (unqualified/untreated) waste is never touched.
					// The ejection removes at most what the store can give,
					// so a shortfall is reported, never forced, and never
					// deletes held material (#403).
					auto const eligible = std::max(0.0, qualified[ejection.storeIndex]);
					auto const ejected = std::min(ejection.authored.quantity, eligible);
					auto const unmet = ejection.authored.quantity - ejected;
					qualified[ejection.storeIndex] -= ejected;
					flowOut[ejection.storeIndex] += ejected;
					// Ejected material crosses the station boundary as an
					// external loss: the per-unit and per-constituent
					// ledgers close over it, so what left the station is
					// accounted, never silently dropped.
					stagedUnitLoss[store.unit] += ejected;
					if (store.composition)
						addComposition(stagedLoss, *store.composition,
							massToKg(store.unit, ejected), 1.0);
					ejection.occurrencesFired = arrival.occurrence + 1;
					ejection.lastFiredTick = tick;
					ejection.lastOccurrenceSeconds = arrival.key.occurrenceSeconds;
					ejection.lastScheduled = ejection.authored.quantity;
					ejection.lastEjected = ejected;
					ejection.lastUnmet = unmet;
					ejection.ejectedTotal += ejected;
					ejection.unmetTotal += unmet;
					ejection.status = ejected <= 0.0 ? StationTransferStatus::Rejected
						: (unmet <= 0.0 ? StationTransferStatus::Full
							: StationTransferStatus::Partial);
					if (ejection.authored.recurrenceSeconds <= 0.0) ejection.exhausted = true;
					continue;
				}
				auto& delivery = deliveries[arrival.key.declarationIndex];
				auto const& store = stores[delivery.storeIndex].authored;
				// The destination accepts only what fits inside its total
				// capacity; the remainder is refused at the boundary and
				// stays accounted as rejected material.
				auto const headroom = std::max(0.0, store.capacity
					- (qualified[delivery.storeIndex] + unqualified[delivery.storeIndex]));
				auto const accepted = std::min(delivery.authored.quantity, headroom);
				auto const rejected = delivery.authored.quantity - accepted;
				if (delivery.authored.arrivesQualified)
					qualified[delivery.storeIndex] += accepted;
				else
					unqualified[delivery.storeIndex] += accepted;
				flowIn[delivery.storeIndex] += accepted;
				stagedUnitDelivered[store.unit] += accepted;
				if (store.composition)
					addComposition(stagedConstituentDelivered, *store.composition,
						massToKg(store.unit, accepted), 1.0);
				delivery.occurrencesFired = arrival.occurrence + 1;
				delivery.lastFiredTick = tick;
				delivery.lastOccurrenceSeconds = arrival.key.occurrenceSeconds;
				delivery.lastScheduled = delivery.authored.quantity;
				delivery.lastAccepted = accepted;
				delivery.lastRejected = rejected;
				delivery.acceptedTotal += accepted;
				delivery.rejectedTotal += rejected;
				delivery.status = accepted <= 0.0 ? StationTransferStatus::Rejected
					: (rejected <= 0.0 ? StationTransferStatus::Full : StationTransferStatus::Partial);
				if (delivery.authored.recurrenceSeconds <= 0.0) delivery.exhausted = true;
			}
			// Power is kW and stored energy is kWh: every crossing between
			// them is power x elapsed hours. A fixed tick is a fraction of a
			// second, so its duration must be converted to hours before it
			// multiplies or divides a power value (#547); consuming seconds
			// directly inflates every energy amount 3600x.
			auto const hours = static_cast<double>(seconds) / 3600.0;
			double availableGeneration = 0.0;
			for (auto& array : solarArrays)
			{
				array.lastIllumination = evaluateStationProfile(array.authored.illumination, elapsed);
				array.lastDisabled = array.authored.capacityKW <= 0.0;
				array.lastAvailableKW = array.lastDisabled
					? 0.0 : array.authored.capacityKW * array.lastIllumination;
				array.lastEclipse = !array.lastDisabled && array.lastAvailableKW <= 0.0;
				availableGeneration += array.lastAvailableKW;
				// #553: finite installed capacities do not guarantee a
				// finite total — two finite arrays can sum past the largest
				// representable double into infinity, and an infinite pool
				// drives every later allocation comparison wrong (inf > inf
				// is false). Reject the overflowed pool before it allocates
				// anything, so the impossible magnitude never reaches a
				// committed result.
				if (!std::isfinite(availableGeneration))
				{
					system = "Power allocation";
					return violation = "solar generation sum overflows to a non-finite magnitude", false;
				}
			}
			std::vector<double> supplied(loads.size(), 0.0);
			// Battery discharge feeds the same allocation pool as generation,
			// never a second budget: it covers the remainder of a load's
			// request that generation could not reach, within the state-of-
			// charge reserve floor, the discharge power limit and the
			// discharge efficiency, in declared battery order.
			auto const batteryReserve = [](BatteryState const& battery)
			{ return battery.authored.nameplateKWh - battery.authored.usableKWh; };
			auto const batteryDisabled = [](BatteryState const& battery)
			{ return battery.authored.usableKWh <= 0.0; };
			// dtHours is the crossing duration in HOURS: the energy drawable
			// above the reserve converts into a power ceiling only through it.
			auto const deliverableKW = [&](BatteryState const& battery, double dtHours)
			{
				if (batteryDisabled(battery)) return 0.0;
				auto const drawable = std::max(0.0, battery.stored - batteryReserve(battery));
				auto const byEnergy = drawable * battery.authored.dischargeEfficiency / dtHours;
				// The discharge power limit is per tick: whatever this battery
				// already delivered this tick counts against it, so the two
				// allocation passes can never jointly exceed the cap.
				auto const byPower = battery.authored.maxDischargeKW - battery.lastDischargeKW;
				return std::max(0.0, std::min(byPower, byEnergy));
			};
			// What the whole bank could deliver this tick before any of it is
			// routed: the pool a protected critical claim is measured
			// against (#549).
			auto const batteryDeliverableKW = [&]()
			{
				double total{0.0};
				for (auto const& battery : batteries) total += deliverableKW(battery, hours);
				return total;
			};
			for (auto& battery : batteries)
			{
				battery.lastChargeKW = 0.0;
				battery.lastDischargeKW = 0.0;
				battery.lastDischargeRequestedKW = 0.0;
				battery.lastDischargeShortfallKW = 0.0;
			}
			// Route a power need through the batteries in declaration order
			// and return what they actually delivered. Each battery records
			// the need routed to it and whatever it could not supply.
			auto const drawBatteries = [&](double needKW)
			{
				if (needKW <= 0.0) return 0.0;
				auto remaining = needKW;
				for (auto& battery : batteries)
				{
					if (remaining <= 1e-15) break;
					if (batteryDisabled(battery)) continue;
					auto const cap = deliverableKW(battery, hours);
					auto const give = std::min(remaining, cap);
					if (give > 0.0)
					{
						auto const drawn = give * hours
							/ battery.authored.dischargeEfficiency;
						battery.stored -= drawn;
						battery.dischargedKWh += drawn;
						battery.deliveredKWh += give * hours;
						battery.lastDischargeKW += give;
					}
					battery.lastDischargeRequestedKW += remaining;
					battery.lastDischargeShortfallKW += remaining - give;
					remaining -= give;
				}
				return needKW - remaining;
			};
			double batterySuppliedKW{0.0};
			double batteryShortfallKW{0.0};
			// Fuelled generators run ahead of the load allocation (#399):
			// their electrical output joins the shared generation pool for
			// this tick's loads, their startup cranking draws ride the same
			// pool ahead of discretionary demand, and their waste, startup
			// and decay heat land in the thermal integration below - each
			// booked exactly once at the sink boundary. A generator is a
			// dispatchable source, not an unlimited power setting: output
			// is bounded by the installed rating, the ramp limit and the
			// fuel actually drawable, and it only runs once the startup
			// energy has been received while cooling is qualified.
			std::vector<double> generatorHeatBySink(reservoirs.size(), 0.0);
			double totalGeneratorOutputKW{0.0}, totalGeneratorStartupKW{0.0};
			double totalGeneratorWasteKW{0.0}, totalGeneratorDecayKW{0.0};
			// Fuel energy is authored in the store's unit; the generator's
			// physics runs in kWh. One factor converts store units to kWh
			// and its reciprocal converts the required fuel energy back, so
			// host and sandbox agree on every draw.
			auto const energyUnitsToKWh = [](std::string const& unit) -> double
			{
				if (unit == "kJ") return 1.0 / 3600.0;
				if (unit == "MJ") return 1000.0 / 3600.0;
				if (unit == "Wh") return 1.0 / 1000.0;
				return 1.0;
			};
			for (auto& generator : generators)
			{
				auto const disabled = generator.authored.ratedOutputKW <= 0.0;
				auto const commanded = disabled ? 0.0 : generator.authored.commandedOutputKW;
				auto const dtSeconds = static_cast<double>(seconds);
				auto const sinkTemperatureC = reservoirs[generator.sinkIndex].temperatureC;
				// Cooling is qualified while the sink is below the maximum
				// sink temperature: at or above it the interface cannot
				// accept the generator's heat, so the generator cannot run.
				auto const coolingAvailable = !disabled
					&& sinkTemperatureC + 1e-9 < generator.authored.maxSinkTemperatureC;
				auto const kwhPerUnit = energyUnitsToKWh(stores[generator.fuelStoreIndex].authored.unit);
				auto const drawableKWh = std::max(0.0,
					qualified[generator.fuelStoreIndex] - stores[generator.fuelStoreIndex].authored.protectedReserve)
					* kwhPerUnit;
				auto const startupRequiredKJ = generator.authored.startupPowerKW * generator.authored.startupSeconds;
				generator.lastFuelDrawKWh = 0.0;
				generator.lastWasteHeatKW = 0.0;
				generator.lastStartupDrawKW = 0.0;
				generator.lastStartupUnmetKW = 0.0;
				generator.lastDecayHeatKW = 0.0;
				generator.lastCoolingAvailable = coolingAvailable;
				generator.lastBlockReason.clear();
				// The phase as it stood before this tick's transitions, so
				// the decay bookkeeping below can tell a generator that has
				// been in Shutdown since last tick from one that just
				// entered it (#563).
				auto const phaseBeforeSwitch = generator.phase;
				switch (generator.phase)
				{
				case StationGeneratorPhase::Stopped:
					generator.outputKW = 0.0;
					if (commanded > 0.0)
					{
						// A commanded start proceeds only when every
						// precondition holds this tick: qualified cooling,
						// drawable fuel above the protected reserve, and a
						// pool that can actually supply the cranking draw.
						// A start is never self-funded: a stopped
						// generator produces nothing, so its own output
						// can never appear in the pool that starts it.
						if (!coolingAvailable)
							generator.lastBlockReason = "cooling is not qualified: sink at or above the maximum sink temperature";
						else if (drawableKWh <= 0.0)
							generator.lastBlockReason = "no fuel is drawable above the protected reserve";
						else if (startupRequiredKJ > 0.0
							&& availableGeneration + batteryDeliverableKW() + 1e-9
								< generator.authored.startupPowerKW)
							generator.lastBlockReason = "the pool cannot supply the startup energy draw";
						else
						{
							generator.startupReceivedKJ = 0.0;
							generator.phase = startupRequiredKJ > 0.0
								? StationGeneratorPhase::Starting : StationGeneratorPhase::Running;
						}
					}
					break;
				case StationGeneratorPhase::Starting:
					generator.outputKW = 0.0;
					if (!coolingAvailable)
					{
						// Cooling lost mid-crank: the start fails. The
						// cranking energy already spent is accounted as
						// heat in the sink; the attempt restarts from zero.
						generator.phase = StationGeneratorPhase::Stopped;
						generator.startupReceivedKJ = 0.0;
						generator.lastBlockReason = "cooling lost during startup";
					}
					else if (drawableKWh <= 0.0)
					{
						generator.phase = StationGeneratorPhase::Stopped;
						generator.startupReceivedKJ = 0.0;
						generator.lastBlockReason = "fuel depleted during startup";
					}
					else
					{
						// #562: the crank buys only the energy still owed.
						// A startup duration that is not a whole number of
						// fixed ticks must not round the final purchase up to
						// a full tick at full startup power: the last interval
						// is prorated to the remaining requirement, so the
						// machine receives and dissipates exactly
						// startupPowerKW x startupSeconds, never more. The
						// prorated average bus draw stays within the authored
						// startup power, and the pool only has to fund what is
						// actually owed this tick.
						auto const remainingStartupKJ = std::max(0.0,
							startupRequiredKJ - generator.startupReceivedKJ);
						auto const requestKW = dtSeconds > 0.0
							? std::min(generator.authored.startupPowerKW,
								remainingStartupKJ / dtSeconds)
							: generator.authored.startupPowerKW;
						auto const poolKW = availableGeneration + batteryDeliverableKW();
						if (poolKW + 1e-9 < requestKW)
						{
							// The startup energy support that qualified the
							// start has gone away mid-crank: the start
							// fails whole rather than cranking on a trickle.
							generator.phase = StationGeneratorPhase::Stopped;
							generator.startupReceivedKJ = 0.0;
							generator.lastBlockReason = "startup energy support lost mid-crank";
							break;
						}
						auto const fromGeneration = std::min(requestKW, availableGeneration);
						availableGeneration -= fromGeneration;
						auto const ask = requestKW - fromGeneration;
						auto const fromBattery = drawBatteries(ask);
						batterySuppliedKW += fromBattery;
						batteryShortfallKW += ask - fromBattery;
						auto const suppliedKW = fromGeneration + fromBattery;
						generator.lastStartupDrawKW = suppliedKW;
						// Cranking energy dissipates into the same sink
						// the running machine will use: booked once here.
						generatorHeatBySink[generator.sinkIndex] += suppliedKW;
						generator.startupHeatKJ += suppliedKW * dtSeconds;
						generator.startupReceivedKJ += suppliedKW * dtSeconds;
						if (generator.startupReceivedKJ + 1e-9 >= startupRequiredKJ)
							generator.phase = StationGeneratorPhase::Running;
					}
					break;
				case StationGeneratorPhase::Running:
					if (!coolingAvailable)
					{
						// Cooling lost while running: the generator trips to
						// shutdown. Electrical output and fuel burn stop
						// immediately; the configured decay heat carries
						// on below.
						generator.phase = StationGeneratorPhase::Shutdown;
						generator.shutdownElapsedSeconds = 0.0;
						generator.outputKW = 0.0;
						generator.lastBlockReason = "cooling lost: generator tripped to shutdown";
					}
					else if (commanded <= 0.0)
					{
						generator.phase = StationGeneratorPhase::Shutdown;
						generator.shutdownElapsedSeconds = 0.0;
						generator.outputKW = 0.0;
					}
					else
					{
						// Ramp toward the commanded output: the output may
						// move at most ramp x dt this tick, up or down, so
						// no tick commands an instantaneous step the
						// machine cannot physically make.
						auto const maxStepKW = generator.authored.maxRampKWPerSecond * dtSeconds;
						auto const targetKW = std::min(commanded, generator.authored.ratedOutputKW);
						generator.outputKW += std::max(-maxStepKW,
							std::min(maxStepKW, targetKW - generator.outputKW));
						// Fuel feasibility: the fuel drawable above the
						// protected reserve caps the output this tick. A
						// generator with no fuel left cannot run at all.
						auto const neededFuelKWh = generator.outputKW * hours / generator.authored.efficiency;
						if (neededFuelKWh > drawableKWh)
						{
							if (drawableKWh <= 1e-15)
							{
								// Fuel depletion stops electrical output
								// immediately; the decay period follows.
								generator.phase = StationGeneratorPhase::Shutdown;
								generator.shutdownElapsedSeconds = 0.0;
								generator.outputKW = 0.0;
								generator.lastBlockReason = "fuel depleted: generator shut down";
								break;
							}
							generator.outputKW = drawableKWh * generator.authored.efficiency / hours;
						}
						auto const drawnKWh = generator.outputKW * hours / generator.authored.efficiency;
						auto const drawnUnits = drawnKWh / kwhPerUnit;
						qualified[generator.fuelStoreIndex] -= drawnUnits;
						flowOut[generator.fuelStoreIndex] += drawnUnits;
						stagedUnitLoss[stores[generator.fuelStoreIndex].authored.unit] += drawnUnits;
						generator.lastFuelDrawKWh = drawnKWh;
						auto const fuelKJ = drawnKWh * 3600.0;
						auto const electricalKJ = generator.outputKW * dtSeconds;
						// Waste heat is the fuel energy the conversion did
						// not deliver as electricity: booked once into the
						// sink, never also exported elsewhere.
						auto const wasteKJ = fuelKJ - electricalKJ;
						generator.fuelEnergyKJ += fuelKJ;
						generator.electricalKJ += electricalKJ;
						generator.wasteHeatKJ += wasteKJ;
						auto const wasteKW = wasteKJ / dtSeconds;
						generator.lastWasteHeatKW = wasteKW;
						generatorHeatBySink[generator.sinkIndex] += wasteKW;
						// The generator's output joins the shared pool the
						// loads allocate against this tick - generation
						// earned with fuel, not sunshine.
						availableGeneration += generator.outputKW;
						totalGeneratorOutputKW += generator.outputKW;
						totalGeneratorWasteKW += wasteKW;
					}
					break;
				case StationGeneratorPhase::Shutdown:
					generator.outputKW = 0.0;
					break;
				}
				// #563: decay heat books from the very tick the electrical
				// output stopped - no transition-tick gap - and the final
				// decay interval is prorated to the remaining configured
				// time, so the sink receives exactly
				// shutdownHeatKW * shutdownSeconds, never a rounded-up
				// tick. The decay heat cannot be refused - it happens
				// whether or not cooling is available - so it books
				// regardless, and the sink warms under it. A zero decay
				// period books nothing. The Shutdown phase persists until
				// the tick after the last decay interval so the phase
				// transition events still bracket the decay period.
				bool decayedThisTick = false;
				if (generator.phase == StationGeneratorPhase::Shutdown)
				{
					auto const remainingSeconds = generator.authored.shutdownSeconds
						- generator.shutdownElapsedSeconds;
					if (remainingSeconds <= 1e-9)
					{
						// Nothing left to decay. A generator that entered
						// Shutdown only this tick keeps the phase for it, so
						// the transition events still bracket even an empty
						// decay period; one already in Shutdown stops.
						if (phaseBeforeSwitch == StationGeneratorPhase::Shutdown)
							generator.phase = StationGeneratorPhase::Stopped;
					}
					else
					{
						auto const intervalSeconds = std::min(dtSeconds, remainingSeconds);
						auto const bookedKJ = generator.authored.shutdownHeatKW * intervalSeconds;
						// The published rate is the booked energy spread over
						// the fixed tick, so rate x dt equals the energy the
						// sink actually received this tick even on the
						// prorated final interval.
						generator.lastDecayHeatKW = bookedKJ / dtSeconds;
						generatorHeatBySink[generator.sinkIndex] += generator.lastDecayHeatKW;
						generator.decayHeatKJ += bookedKJ;
						totalGeneratorDecayKW += generator.lastDecayHeatKW;
						generator.shutdownElapsedSeconds += intervalSeconds;
						decayedThisTick = bookedKJ > 0.0;
					}
				}
				// The cooling obligation is exposed on every interval that
				// actually produces decay heat - including the tick on
				// which the last of it is booked, whatever phase that tick
				// ends in - and while the machine is running (#563).
				generator.lastCoolingRequired = generator.phase == StationGeneratorPhase::Running
					|| decayedThisTick;
				totalGeneratorStartupKW += generator.lastStartupDrawKW;
				// Publish the per-generator service status against the
				// commanded output: full at command, partial under ramp
				// or fuel limits, rejected when commanded with nothing
				// produced, idle when not commanded.
				if (disabled || commanded <= 0.0)
					generator.status = StationTransferStatus::Idle;
				else if (generator.outputKW + 1e-12 >= commanded)
					generator.status = StationTransferStatus::Full;
				else if (generator.outputKW > 1e-12)
					generator.status = StationTransferStatus::Partial;
				else generator.status = StationTransferStatus::Rejected;
			}
			// Fuel-cell backup (#406) runs on the supply side alongside the
			// generators: its electrical output joins the shared generation
			// pool the loads allocate against, its waste heat lands in the
			// class-matched sink booked exactly once, and its reactants are
			// drawn only from its own dedicated stores above their protected
			// reserves - the cell can never reach protected breathing oxygen
			// because it can only ever see its own oxygen store, which is
			// refilled only through controlled surplus transfers that
			// themselves preserve the supplying reserve. Output is bounded
			// by the installed rating, the dispatch setpoint, the hydrogen
			// and oxygen actually drawable, and the product water store's
			// remaining headroom: a backed-up water destination caps
			// production rather than vanishing product mass. Cooling is
			// required: a sink at or above the maximum sink temperature
			// refuses the cell whole - heat with nowhere to go is heat that
			// cannot be made. Cells run in declaration order.
			std::vector<double> fuelCellHeatByReservoir(reservoirs.size(), 0.0);
			double totalFuelCellOutputKW{0.0}, totalFuelCellWasteKW{0.0};
			for (auto& cell : fuelCells)
			{
				auto const disabled = cell.authored.ratedOutputKW <= 0.0;
				auto const commanded = disabled ? 0.0 : cell.authored.commandedOutputKW;
				auto const dtSeconds = static_cast<double>(seconds);
				auto const o2PerH2 = StationFuelCellOxygenMassPerHydrogenKg;
				auto const elecKJPerKgH2 = cell.authored.efficiency
					* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
				auto const& hydrogen = stores[cell.hydrogenStoreIndex].authored;
				auto const& oxygen = stores[cell.oxygenStoreIndex].authored;
				auto const& water = stores[cell.waterStoreIndex].authored;
				cell.lastOutputKW = 0.0;
				cell.lastHydrogenKg = 0.0;
				cell.lastOxygenKg = 0.0;
				cell.lastWaterKg = 0.0;
				cell.lastWasteHeatKW = 0.0;
				cell.lastUnmetOutputKW = 0.0;
				cell.lastLimiter.clear();
				cell.lastBlockReason.clear();
				// The backup clock: usable reactants are the qualified
				// contents drawable above each store's own protected
				// reserve, balanced stoichiometrically, converted to the
				// electrical energy this cell's efficiency can extract and
				// divided by the reference output - the cell's actual
				// output this tick, or its installed rating when the cell
				// is not running. Protected oxygen never counts toward it.
				auto const publishEndurance = [&]()
				{
					auto const remainingH2Kg = std::max(0.0, massToKg(hydrogen.unit,
						qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve));
					auto const remainingO2Kg = std::max(0.0, massToKg(oxygen.unit,
						qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve));
					auto const usableH2Kg = std::min(remainingH2Kg, remainingO2Kg / o2PerH2);
					auto const referenceKW = cell.lastOutputKW > 0.0
						? cell.lastOutputKW : cell.authored.ratedOutputKW;
					cell.enduranceSeconds = referenceKW > 0.0
						? usableH2Kg * elecKJPerKgH2 / referenceKW : 0.0;
				};
				auto const& sink = reservoirs[cell.sinkIndex];
				cell.lastCoolingAvailable = !disabled
					&& sink.temperatureC + 1e-9 < cell.authored.maxSinkTemperatureC;
				if (disabled)
				{
					publishEndurance();
					cell.status = StationTransferStatus::Idle;
					continue;
				}
				if (!cell.lastCoolingAvailable)
				{
					cell.lastBlockReason = "cooling is not qualified: sink at or above the maximum sink temperature";
					cell.lastUnmetOutputKW = commanded;
					publishEndurance();
					cell.status = StationTransferStatus::Rejected;
					continue;
				}
				if (commanded <= 0.0)
				{
					publishEndurance();
					cell.status = StationTransferStatus::Idle;
					continue;
				}
				// Every finite axis caps the output the same way: the
				// largest output the axis can sustain this tick. The
				// binding one names itself; nothing is silently clamped.
				auto const h2DrawableKg = std::max(0.0, massToKg(hydrogen.unit,
					qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve));
				auto const o2DrawableKg = std::max(0.0, massToKg(oxygen.unit,
					qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve));
				auto const waterHeadroomKg = std::max(0.0, massToKg(water.unit,
					water.capacity - (qualified[cell.waterStoreIndex]
						+ unqualified[cell.waterStoreIndex])));
				auto const byH2KW = h2DrawableKg * elecKJPerKgH2 / dtSeconds;
				auto const byO2KW = (o2DrawableKg / o2PerH2) * elecKJPerKgH2 / dtSeconds;
				auto const byWaterKW = (waterHeadroomKg / (1.0 + o2PerH2))
					* elecKJPerKgH2 / dtSeconds;
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double capKW, std::string const& reason)
				{
					if (capKW >= scale * commanded) return;
					scale = capKW / commanded;
					limiter = reason;
				};
				constrain(byH2KW, "hydrogen store '" + hydrogen.id
					+ "' has nothing drawable above its protected reserve");
				constrain(byO2KW, "oxygen store '" + oxygen.id
					+ "' has nothing drawable above its protected reserve");
				constrain(byWaterKW, "product water store '" + water.id
					+ "' is full: the water has nowhere to go");
				scale = std::max(0.0, std::min(1.0, scale));
				cell.lastLimiter = limiter;
				auto outputKW = commanded * scale;
				if (outputKW <= 1e-15)
				{
					// Nothing can burn this tick: the cell is refused whole,
					// consumes nothing and delivers nothing, and the whole
					// commanded output stays visible as unmet service.
					cell.lastUnmetOutputKW = commanded;
					cell.lastBlockReason = limiter;
					publishEndurance();
					cell.status = StationTransferStatus::Rejected;
					continue;
				}
				// Derive the reaction from the capped output: hydrogen is
				// the energy-carrying reactant, oxygen rides the declared
				// stoichiometric ratio, and product water is the sum of
				// the reactants actually consumed, so the mass ledger
				// closes by construction. Each flow is clamped to its own
				// drawable or headroom so a one-ulp roundoff can never
				// overshoot (#556), and the delivered output is recomputed
				// from the hydrogen that actually moved - the pool never
				// receives more than the reactants paid for.
				auto h2Kg = std::min(outputKW * dtSeconds / elecKJPerKgH2, h2DrawableKg);
				auto o2Kg = std::min(h2Kg * o2PerH2, o2DrawableKg);
				h2Kg = std::min(h2Kg, o2Kg / o2PerH2);
				auto const waterKg = h2Kg + o2Kg;
				auto const chemicalKJ = h2Kg
					* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
				outputKW = cell.authored.efficiency * chemicalKJ / dtSeconds;
				auto const electricalKJ = outputKW * dtSeconds;
				auto const wasteKJ = chemicalKJ - electricalKJ;
				auto const h2Units = std::min(kgToMass(hydrogen.unit, h2Kg),
					std::max(0.0, qualified[cell.hydrogenStoreIndex] - hydrogen.protectedReserve));
				auto const h2ActualKg = massToKg(hydrogen.unit, h2Units);
				auto const o2Units = std::min(kgToMass(oxygen.unit, o2Kg),
					std::max(0.0, qualified[cell.oxygenStoreIndex] - oxygen.protectedReserve));
				auto const o2ActualKg = massToKg(oxygen.unit, o2Units);
				auto const waterUnits = std::min(kgToMass(water.unit, waterKg),
					std::max(0.0, water.capacity
						- (qualified[cell.waterStoreIndex] + unqualified[cell.waterStoreIndex])));
				auto const waterActualKg = massToKg(water.unit, waterUnits);
				qualified[cell.hydrogenStoreIndex] -= h2Units;
				flowOut[cell.hydrogenStoreIndex] += h2Units;
				stagedUnitLoss[hydrogen.unit] += h2Units;
				qualified[cell.oxygenStoreIndex] -= o2Units;
				flowOut[cell.oxygenStoreIndex] += o2Units;
				stagedUnitLoss[oxygen.unit] += o2Units;
				qualified[cell.waterStoreIndex] += waterUnits;
				flowIn[cell.waterStoreIndex] += waterUnits;
				stagedUnitLoss[water.unit] -= waterUnits;
				// Energy: the reactants' chemical energy left as
				// electricity into the shared pool or as waste heat into
				// the sink - booked once, never a third destination.
				fuelCellHeatByReservoir[cell.sinkIndex] += wasteKJ / dtSeconds;
				availableGeneration += outputKW;
				cell.lastOutputKW = outputKW;
				cell.lastHydrogenKg = h2ActualKg;
				cell.lastOxygenKg = o2ActualKg;
				cell.lastWaterKg = waterActualKg;
				cell.lastWasteHeatKW = wasteKJ / dtSeconds;
				cell.lastUnmetOutputKW = std::max(0.0, commanded - outputKW);
				cell.hydrogenConsumedKg += h2ActualKg;
				cell.oxygenConsumedKg += o2ActualKg;
				cell.waterProducedKg += waterActualKg;
				cell.chemicalEnergyKJ += chemicalKJ;
				cell.electricalKJ += electricalKJ;
				cell.wasteHeatKJ += wasteKJ;
				totalFuelCellOutputKW += outputKW;
				totalFuelCellWasteKW += wasteKJ / dtSeconds;
				publishEndurance();
				if (outputKW + 1e-12 >= commanded)
					cell.status = StationTransferStatus::Full;
				else cell.status = StationTransferStatus::Partial;
			}
			// #554: declared material exchanges are operation dependencies.
			// The material-feasible operating fraction is the largest fraction
			// of the load's full-operation (profile-scaled) exchanges that its
			// declared feed (qualified material above the protected reserve)
			// and destination headroom can sustain this tick. The electrical
			// request is capped to that fraction before allocation, so a
			// starved or backed-up load never draws power it cannot run on,
			// never exports heat it would only make while running, and never
			// books outputs there is no room to deliver. The shortfall stays
			// explicit as unmet service and per-exchange unmet material.
			auto const exchangeFullAmount = [&](LoadExchangeState const& exchange,
				double profileLevel)
			{
				return exchange.ratePerSecond * static_cast<double>(seconds) * profileLevel;
			};
			auto const materialFeasible = [&](LoadState const& load, std::string& limiter)
			{
				double fraction = 1.0;
				for (auto const& exchange : load.exchanges)
				{
					auto const& store = stores[exchange.storeIndex].authored;
					auto const full = exchangeFullAmount(exchange, load.lastProfileLevel);
					if (full <= 0.0) continue;
					if (exchange.consumes)
					{
						auto const drawable = std::max(0.0,
							qualified[exchange.storeIndex] - store.protectedReserve);
						if (drawable / full < fraction)
						{
							fraction = drawable / full;
							limiter = drawable <= 1e-12
								? (store.protectedReserve > 0.0
									? "feed store '" + store.id + "' is at its protected reserve"
									: (unqualified[exchange.storeIndex] > 1e-12
										? "feed store '" + store.id
											+ "' holds only unqualified material"
										: "feed store '" + store.id + "' is empty"))
								: "feed store '" + store.id + "' supply limits the operating level";
						}
					}
					else
					{
						auto const headroom = std::max(0.0, store.capacity
							- (qualified[exchange.storeIndex]
								+ unqualified[exchange.storeIndex]));
						if (headroom / full < fraction)
						{
							fraction = headroom / full;
							limiter = headroom <= 1e-12
								? "destination store '" + store.id + "' is full"
								: "destination store '" + store.id
									+ "' headroom limits the operating level";
						}
					}
				}
				return std::max(0.0, std::min(1.0, fraction));
			};
			// Reserve the capped level's material against the staged inventory
			// when a grant is made: a feed draw and a destination claim that
			// later loads in the allocation order must see, so competing loads
			// never double-book the same kilograms (#554).
			auto const reserveMaterial = [&](LoadState& load, double fraction)
			{
				for (auto& exchange : load.exchanges)
				{
					auto const reserved = exchangeFullAmount(exchange, load.lastProfileLevel)
						* fraction;
					if (exchange.consumes)
						qualified[exchange.storeIndex] -= reserved;
					else
						qualified[exchange.storeIndex] += reserved;
					exchange.lastReserved = reserved;
				}
				load.lastMaterialReserved = true;
			};
			// Pass 1: protected critical floors allocate ahead of every
			// discretionary request, in the declared priority order. A
			// floor that generation cannot meet is offered to the batteries
			// before it remains explicit unmet critical demand; it is never
			// silently reclassified as permissible curtailment. What is
			// protected here is the larger of the floor and the load's
			// minimum operable supply: a floor below that minimum would buy
			// a state the equipment cannot physically run in, so a pool that
			// cannot reach the minimum funds nothing at all (#549).
			for (auto const index : loadOrder)
			{
				auto& load = loads[index];
				load.lastShortfallReason.clear();
				load.lastProfileLevel = evaluateStationProfile(load.authored.operatingProfile, elapsed);
				load.lastRequestedKW = load.authored.electricalDemandKW * load.lastProfileLevel;
				load.lastAdmitted = load.authored.coolingDisposition
					== StationCoolingDisposition::Exported;
				load.lastBlockReason = load.lastAdmitted ? std::string{}
					: "no cooling service of class '" + load.authored.thermalClass + "' is available";
				load.lastUnmetCriticalKW = 0.0;
				load.lastMaterialFraction = 1.0;
				load.lastCappedRequestKW = load.lastRequestedKW;
				load.lastMaterialComputed = false;
				load.lastMaterialRefused = false;
				load.lastMaterialReserved = false;
				for (auto& exchange : load.exchanges) exchange.lastReserved = 0.0;
				if (load.lastRequestedKW <= 0.0) continue;
				auto const floor = std::min(load.authored.criticalFloorKW, load.lastRequestedKW);
				// A cooling-blocked load is refused admission and receives no
				// electrical service at all, so every watt of its protected
				// floor stays unmet alongside the explicit unmet cooling and
				// the refusal: the floor's shortfall must never vanish just
				// because the load was blocked before allocation (#550).
				if (!load.lastAdmitted)
				{
					load.lastUnmetCriticalKW = floor;
					continue;
				}
				if (floor <= 0.0) continue;
				auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
				std::string materialLimiter;
				auto const material = materialFeasible(load, materialLimiter);
				load.lastMaterialFraction = material;
				load.lastCappedRequestKW = load.lastRequestedKW * material;
				load.lastMaterialComputed = true;
				if (load.lastCappedRequestKW <= 1e-12
					|| load.lastCappedRequestKW + 1e-12 < minOperating)
				{
					// The declared feed or destination limits cannot sustain
					// even the minimum operable level: the load is refused
					// whole, spends no power and no material, and its whole
					// protected floor stays explicit unmet demand while the
					// starvation itself stays visible as unmet material on
					// the exchange flows (#554).
					load.lastUnmetCriticalKW = floor;
					load.lastShortfallReason = materialLimiter;
					load.lastMaterialRefused = true;
					continue;
				}
				// The protected claim never exceeds what the material can
				// feed: a floor above the capped request stays partly unmet
				// rather than buying power the equipment cannot use.
				auto const claim = std::min(std::max(floor, minOperating),
					load.lastCappedRequestKW);
				auto const reachable = std::min(claim, availableGeneration + batteryDeliverableKW());
				if (reachable + 1e-12 < minOperating)
				{
					// The pools cannot run this load at all: it is refused
					// whole, spends no scarce energy, and every watt of its
					// protected floor stays unmet rather than just the slice
					// the pool happened to fall short of.
					load.lastUnmetCriticalKW = floor;
					load.lastShortfallReason =
						"critical floor cannot reach the minimum operable supply level";
					continue;
				}
				auto const fromGeneration = std::min(reachable, availableGeneration);
				supplied[index] += fromGeneration;
				availableGeneration -= fromGeneration;
				auto const stillNeeded = reachable - fromGeneration;
				auto const fromBattery = drawBatteries(stillNeeded);
				supplied[index] += fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += stillNeeded - fromBattery;
				reserveMaterial(load, material);
				load.lastUnmetCriticalKW = std::max(0.0, floor - supplied[index]);
			}
			// Pass 2: discretionary demand in the same declared order. A
			// load is never supplied below its minimum operable level,
			// demand * (1 - allowed curtailment): if generation plus any
			// available battery output still cannot reach that level the
			// load takes nothing this tick, leaving both pools for loads
			// that can use them. Generation is taken before battery
			// energy, so stored energy is preserved rather than wasted.
			for (auto const index : loadOrder)
			{
				auto& load = loads[index];
				if (!load.lastAdmitted || load.lastRequestedKW <= 0.0
					|| load.lastMaterialRefused) continue;
				auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
				// Loads with no protected floor were not evaluated in pass 1:
				// their material feasibility is read against the inventory as
				// it stands after the protected grants ahead of them (#554).
				if (!load.lastMaterialComputed)
				{
					std::string materialLimiter;
					auto const material = materialFeasible(load, materialLimiter);
					load.lastMaterialFraction = material;
					load.lastCappedRequestKW = load.lastRequestedKW * material;
					load.lastMaterialComputed = true;
					if (load.lastCappedRequestKW <= 1e-12
					|| load.lastCappedRequestKW + 1e-12 < minOperating)
					{
						load.lastShortfallReason = materialLimiter;
						load.lastMaterialRefused = true;
						continue;
					}
				}
				auto const remaining = load.lastCappedRequestKW - supplied[index];
				if (remaining <= 0.0) continue;
				auto const fromGeneration = std::min(remaining, availableGeneration);
				auto const afterGeneration = remaining - fromGeneration;
				auto const batteryAsk = std::min(afterGeneration, batteryDeliverableKW());
				if (supplied[index] + fromGeneration + batteryAsk + 1e-12 >= minOperating)
				{
					supplied[index] += fromGeneration;
					availableGeneration -= fromGeneration;
					// Route the full remaining need, not just the capped ask,
					// so each battery's recorded request and shortfall show the
					// limitation the allocation actually ran into; the bank
					// still delivers no more than it was capable of.
					auto const drawn = drawBatteries(afterGeneration);
					supplied[index] += drawn;
					batterySuppliedKW += drawn;
					// The shortfall is what the load needed beyond generation
					// that the battery bank could not deliver, not merely what
					// was asked of it after capping by capability.
					batteryShortfallKW += afterGeneration - drawn;
					if (!load.lastMaterialReserved)
						reserveMaterial(load, load.lastMaterialFraction);
				}
				else if (load.lastShortfallReason.empty())
					load.lastShortfallReason = "below the minimum operable supply level";
			}
			for (size_t index = 0; index < loads.size(); ++index)
			{
				auto& load = loads[index];
				load.lastSuppliedKW = supplied[index];
				load.lastUnmetKW = load.lastRequestedKW - supplied[index];
				load.lastDeliveredFraction = load.lastRequestedKW > 0.0
					? supplied[index] / load.lastRequestedKW : 0.0;
				// Exported heat is an explicit external boundary exchange at
				// the full-operation delivered fraction: the authored heat is
				// a full-operation rate, so it scales by both the operating-
				// profile level and the fraction of that profile-scaled request
				// the allocation satisfied (deliveredFraction). A cooling-
				// infeasible load never runs, so it produces no station heat;
				// its cooling need is reported as unmet cooling instead of
				// being absorbed (#548).
				// Emitted heat is booked exactly once (#395): a load with a
				// declared heat sink routes its produced heat into the
				// reservoir and exports nothing directly; without a sink the
				// heat leaves through the declared export boundary as
				// before. Electrical input is never converted into duplicate
				// local heat - the authored heat output is the emitted heat,
				// and energy stored in batteries or bound into material is
				// not heat at all.
				auto const producedHeatKW = load.lastAdmitted
					? load.authored.heatOutputKW * load.lastProfileLevel
					* load.lastDeliveredFraction : 0.0;
				load.lastHeatRoutedKW = load.hasHeatSink ? producedHeatKW : 0.0;
				load.lastHeatExportedKW = load.hasHeatSink ? 0.0 : producedHeatKW;
				load.lastUnmetCoolingKW = load.lastAdmitted
					? 0.0 : load.authored.heatOutputKW * load.lastProfileLevel;
				load.lastCriticalUnmet = load.lastUnmetCriticalKW > 1e-12;
				if (load.lastRequestedKW <= 0.0) load.status = StationTransferStatus::Idle;
				else if (!load.lastAdmitted || supplied[index] <= 1e-12)
					load.status = StationTransferStatus::Rejected;
				else if (supplied[index] >= load.lastRequestedKW - 1e-12)
					load.status = StationTransferStatus::Full;
				else load.status = StationTransferStatus::Partial;
				// Declared material exchanges are full-operation rates and
				// scale by the full-operation delivered fraction — the
				// operating-profile level times the delivered fraction of the
				// profile-scaled request: a duty-cycled or curtailed load
				// consumes and delivers less (#548). The exchange's requested
				// amount is the full-operation profile need (rate * dt *
				// profileLevel) and what completes is that need at the
				// delivered fraction, bounded by the amount reserved against
				// the declared feed and destination limits when the grant ran
				// (#554). A refused load never reserved, so it completes
				// nothing and the whole need stands as unmet material — never
				// drawn from a protected reserve, never forced into a full
				// destination, and never replaced by hidden external makeup.
				// The unused reservation returns to the staged inventory
				// before transfers and conversions run. Completed amounts
				// book at the external boundary — draws are losses, deliveries
				// are negative losses — so the station ledgers stay closed.
				for (auto& exchange : load.exchanges)
				{
					auto const& exchangeStore = stores[exchange.storeIndex].authored;
					auto const requested = exchangeFullAmount(exchange, load.lastProfileLevel);
					auto const completed = std::min(requested * load.lastDeliveredFraction,
						exchange.lastReserved);
					if (exchange.consumes)
					{
						qualified[exchange.storeIndex] += exchange.lastReserved - completed;
						flowOut[exchange.storeIndex] += completed;
					}
					else
					{
						qualified[exchange.storeIndex] -= exchange.lastReserved - completed;
						flowIn[exchange.storeIndex] += completed;
					}
					exchange.lastRequested = requested;
					exchange.lastCompleted = completed;
					exchange.lastUnmet = requested - completed;
					exchange.lastReserved = 0.0;
					stagedUnitLoss[exchangeStore.unit] += exchange.consumes ? completed : -completed;
					if (exchangeStore.composition)
					{
						ConstituentLedger delta;
						addComposition(delta, *exchangeStore.composition,
							massToKg(exchangeStore.unit, completed),
							exchange.consumes ? -1.0 : 1.0);
						for (auto const& [constituent, change] : delta)
							stagedLoss[constituent] -= change;
					}
				}
			}
			// Coolant loops run after the external load allocation and
			// before thermal integration and battery charging (#396): the
			// pump and freeze heater draw from the same generation-and-
			// battery pool as external loads — never a second budget — and
			// their heat must be in the loop before the reservoir
			// integrates this tick's flows. Each loop circulates only the
			// coolant it actually holds: the inventory fraction caps the
			// pump demand, so a dry loop draws nothing and flows nothing,
			// and an underfilled loop is capped proportionally. Pump work
			// and heater heat enter the loop exactly once and reach the
			// reservoir only through the circulation-gated exchanger.
			// Loops run in declaration order; there is no priority because
			// every loop's demand is its own service, not a competing
			// claim class.
			std::vector<double> loopHeatIntoReservoir(reservoirs.size(), 0.0);
			std::vector<double> atmosphereHeatByReservoir(reservoirs.size(), 0.0);
			std::vector<double> chillerExtractionByLoop(coolantLoops.size(), 0.0);
			std::vector<double> condenserHeatBySink(reservoirs.size(), 0.0);
			// The conductance each loop actually presents across its
			// exchanger this tick: gated by circulation, so a loop with no
			// flow exchanges nothing however conductive its exchanger is.
			std::vector<double> loopExchangeConductanceKWPerK(coolantLoops.size(), 0.0);
			double totalCoolantPumpKW{0.0}, totalCoolantHeaterKW{0.0};
			// The loop pass is split so the chillers (#397) can read this
			// tick's circulation and have their extraction land inside the
			// loop's own integration: allocation first, then chillers,
			// then the loop integrates pump work, heater heat, exchanger
			// exchange and chiller extraction together.
			for (size_t loopIndex = 0; loopIndex < coolantLoops.size(); ++loopIndex)
			{
				auto& loop = coolantLoops[loopIndex];
				auto const disabled = loop.coolantKg <= 0.0;
				auto const fillFraction = disabled
					? 0.0 : std::min(1.0, loop.coolantKg / loop.authored.coolantCapacityKg);
				// A dry loop cannot run a pump: no fluid means no draw and
				// no flow, never an invented one.
				auto const pumpDemandKW = disabled
					? 0.0 : loop.authored.pumpPowerKW * fillFraction;
				double pumpSuppliedKW = 0.0;
				{
					auto const fromGeneration = std::min(pumpDemandKW, availableGeneration);
					pumpSuppliedKW += fromGeneration;
					availableGeneration -= fromGeneration;
					auto const ask = pumpDemandKW - fromGeneration;
					auto const fromBattery = drawBatteries(ask);
					pumpSuppliedKW += fromBattery;
					batterySuppliedKW += fromBattery;
					batteryShortfallKW += ask - fromBattery;
				}
				double heaterDemandKW = 0.0;
				if (!disabled && loop.temperatureC < loop.authored.freezeProtectionSetpointC)
					heaterDemandKW = loop.authored.heaterCapacityKW;
				double heaterSuppliedKW = 0.0;
				{
					auto const fromGeneration = std::min(heaterDemandKW, availableGeneration);
					heaterSuppliedKW += fromGeneration;
					availableGeneration -= fromGeneration;
					auto const ask = heaterDemandKW - fromGeneration;
					auto const fromBattery = drawBatteries(ask);
					heaterSuppliedKW += fromBattery;
					batterySuppliedKW += fromBattery;
					batteryShortfallKW += ask - fromBattery;
				}
				// Circulation is capped by the fluid present, the
				// electricity supplied and the pump's rated flow: the
				// fraction of rated flow that is actually moving. A zero
				// rated flow is a zero-flow machine — fill and a powered
				// pump notwithstanding, no coolant moves, so every
				// circulation-gated exchange and evaporator admission is
				// zero rather than a fraction of a flow that does not
				// exist (#558).
				auto const pumpElectricalFraction = pumpDemandKW > 0.0
					? pumpSuppliedKW / pumpDemandKW : 0.0;
				auto const circulationFraction = disabled
						|| loop.authored.ratedFlowKgPerS <= 0.0
					? 0.0 : fillFraction * pumpElectricalFraction;
				loop.lastCirculationFraction = circulationFraction;
				loop.lastFlowKgPerS = loop.authored.ratedFlowKgPerS * circulationFraction;
				// Every watt the pump actually draws ends up as work
				// dissipated into the coolant it actually moves — booked
				// once, here, into the loop ledger.
				loop.lastPumpDemandKW = pumpDemandKW;
				loop.lastPumpSuppliedKW = pumpSuppliedKW;
				loop.lastHeaterDemandKW = heaterDemandKW;
				loop.lastHeaterSuppliedKW = heaterSuppliedKW;
				// What the exchanger is worth this tick. The exchange itself
				// is not computed here: it is solved once per reservoir
				// below, together with every other body coupled to that
				// reservoir, so the whole star shares one finite heat
				// budget (#559). No circulation means no conductance.
				loopExchangeConductanceKWPerK[loopIndex] = circulationFraction
					* loop.authored.exchangerConductanceKWPerK;
				totalCoolantPumpKW += pumpSuppliedKW;
				totalCoolantHeaterKW += heaterSuppliedKW;
			}
			// Passive conductance exchange (#559) is integrated once per
			// reservoir over the whole star of bodies heat-only coupled to
			// it — every coolant loop and every pressurised compartment —
			// rather than edge by edge against a tick-start temperature
			// nobody updates. A tick-start rate applied whole moves a small
			// body past the equilibrium it shares with a large one: past
			// the crossing the formula keeps pushing, so a stiff conductance
			// reverses hot and cold and drives temperatures below absolute
			// zero, and several leaves on one reservoir each drain a
			// temperature none of them ever sees move. Solving the star
			// implicitly instead cannot overshoot whatever the conductance
			// or the capacity ratio; see solvePassiveExchange. The solve
			// runs before the chiller pass below so the exchanger's claim
			// on each loop this tick is known when the chiller's finite
			// cold-side bound is sized (#560); it reads only tick-start
			// temperatures and the circulation-gated conductance, so the
			// chillers change nothing it sees.
			std::vector<double> loopExchangeKJ(coolantLoops.size(), 0.0);
			std::vector<double> atmosphereExchangeKJ(atmospheres.size(), 0.0);
			auto const exchangeDt = static_cast<double>(seconds);
			for (size_t reservoirIndex = 0; reservoirIndex < reservoirs.size(); ++reservoirIndex)
			{
				auto const& reservoir = reservoirs[reservoirIndex];
				std::vector<PassiveExchangeLeaf> leaves;
				std::vector<size_t> loopIndexes;
				std::vector<size_t> atmosphereIndexes;
				for (size_t loopIndex = 0; loopIndex < coolantLoops.size(); ++loopIndex)
				{
					auto const& loop = coolantLoops[loopIndex];
					if (loop.reservoirIndex != reservoirIndex) continue;
					auto const conductanceKWPerK = loopExchangeConductanceKWPerK[loopIndex];
					if (conductanceKWPerK <= 0.0) continue;
					leaves.push_back({loop.coolantKg * loop.authored.specificHeatKJPerKgK,
						conductanceKWPerK, loop.temperatureC, 0.0});
					loopIndexes.push_back(loopIndex);
				}
				for (size_t atmosphereIndex = 0; atmosphereIndex < atmospheres.size(); ++atmosphereIndex)
				{
					auto const& compartment = atmospheres[atmosphereIndex];
					if (compartment.reservoirIndex != reservoirIndex) continue;
					if (compartment.authored.exchangerConductanceKWPerK <= 0.0) continue;
					leaves.push_back({compartment.authored.thermalCapacityKJPerK,
						compartment.authored.exchangerConductanceKWPerK,
						compartment.temperatureC, 0.0});
					atmosphereIndexes.push_back(atmosphereIndex);
				}
				if (leaves.empty()) continue;
				auto const coupledTemperatureC = solvePassiveExchange(leaves,
					reservoir.authored.heatCapacityKJPerK, reservoir.temperatureC, exchangeDt);
				// Each leaf's heat is booked on the leaf as a rate over the
				// tick and negated onto the reservoir, so the two ledgers
				// close on one number rather than two rounded ones.
				for (size_t position = 0; position < leaves.size(); ++position)
				{
					auto const heatKJ = passiveExchangeHeatKJ(leaves[position], coupledTemperatureC);
					if (position < loopIndexes.size())
					{
						auto const index = loopIndexes[position];
						loopExchangeKJ[index] = heatKJ;
						coolantLoops[index].lastExchangeIntoLoopKW = heatKJ / exchangeDt;
						loopHeatIntoReservoir[reservoirIndex] -= heatKJ / exchangeDt;
					}
					else
					{
						auto const index = atmosphereIndexes[position - loopIndexes.size()];
						atmosphereExchangeKJ[index] = heatKJ;
						atmosphereHeatByReservoir[reservoirIndex] -= heatKJ / exchangeDt;
					}
				}
			}
			// Chillers (#397) run between the loop allocation and the
			// loop's own integration, in declaration order: every loop's
			// circulation and exchanger share this tick are known, and the
			// extraction lands in the loop's temperature integration
			// below. A chiller's cold service demand is its rated
			// capacity; the duty it can actually deliver is capped by the
			// cold loop's circulation fraction (heat must reach the
			// evaporator — no circulation, no extracted heat) and by the
			// heat the coolant actually holds that can be extracted above
			// the chilled setpoint (#560), it is gated off at or below
			// the chilled setpoint, and it is refused whole when the
			// condenser sink sits at or above the maximum condensing
			// temperature. The
			// electrical request — work at the configured COP plus the
			// running auxiliaries for the deliverable duty — is drawn
			// from the same generation-and-battery pool as every other
			// load, never a second budget. A curtailed grant duty-cycles
			// the whole machine at one delivered fraction, so extracted
			// heat, work and condenser heat all scale together and the
			// condenser booking equals extracted heat plus the work and
			// auxiliaries actually supplied at one consistent boundary.
			double totalChillerPowerKW{0.0};
			for (auto& chiller : chillers)
			{
				auto const disabled = chiller.authored.ratedCoolingCapacityKW <= 0.0;
				auto const& loop = coolantLoops[chiller.loopIndex];
				auto const& sink = reservoirs[chiller.sinkIndex];
				chiller.lastAtSetpoint = !disabled
					&& loop.temperatureC <= chiller.authored.chilledSetpointC + 1e-9;
				chiller.lastSinkOver = !disabled
					&& sink.temperatureC + 1e-9 >= chiller.authored.maxCondensingTemperatureC;
				auto const coolingDemandKW = disabled || chiller.lastAtSetpoint
					? 0.0 : chiller.authored.ratedCoolingCapacityKW;
				chiller.lastCoolingDemandKW = coolingDemandKW;
				chiller.lastDeliverableKW = 0.0;
				chiller.lastPowerDemandKW = 0.0;
				chiller.lastPowerSuppliedKW = 0.0;
				chiller.lastCoolingKW = 0.0;
				chiller.lastCondenserKW = 0.0;
				chiller.lastDeliveredFraction = 0.0;
				chiller.lastBlockReason.clear();
				if (disabled || coolingDemandKW <= 0.0)
				{
					// Zero capacity disables the chiller outright; a cold
					// side at its chilled setpoint needs no service this
					// tick. Neither draws power nor moves heat.
					chiller.lastAdmitted = !disabled && !chiller.lastSinkOver;
					chiller.status = StationTransferStatus::Idle;
					continue;
				}
				if (chiller.lastSinkOver)
				{
					// The hot-side temperature interface cannot accept the
					// condenser heat: the chiller is refused whole, draws no
					// power and books no heat, and its entire demand stays
					// visible as unmet cold service.
					chiller.lastAdmitted = false;
					chiller.lastBlockReason = "condenser sink '" + chiller.authored.condenserReservoirId
						+ "' is at or above the maximum condensing temperature";
					chiller.status = StationTransferStatus::Rejected;
					continue;
				}
				chiller.lastAdmitted = true;
				// A nameplate rating is not a heat source (#560). Over the
				// tick the cold loop can give up only the heat its own
				// thermal mass holds above the chilled setpoint — the
				// cold-side interface the machine exists to hold — plus the
				// heat legitimately entering it this tick: pump work,
				// freeze-heater heat and the exchanger's implicit-solve
				// share, each counted exactly once. With C the loop's heat
				// capacity, T its tick-start temperature and Q_in the
				// tick's inflows, the bound is the exact integration
				// constraint: extracting more than C x (T - setpoint) +
				// Q_in would end the tick below the setpoint, so the loop
				// may be cooled to the interface and no further. The
				// setpoint is validated above absolute zero, so the bound
				// also keeps the published coolant temperature inside the
				// physical range however lopsided the coolant capacity and
				// the cooling rating. Work, auxiliaries and the condenser
				// booking all size off the capped deliverable, so every
				// ledger scales with the duty that actually moves.
				auto const tickSeconds = static_cast<double>(seconds);
				auto const loopHeatCapacityKJPerK = loop.coolantKg
					* loop.authored.specificHeatKJPerKgK;
				auto const extractableKJ = std::max(0.0,
					loopHeatCapacityKJPerK * std::max(0.0,
						loop.temperatureC - chiller.authored.chilledSetpointC)
					+ (loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW) * tickSeconds
					+ loopExchangeKJ[chiller.loopIndex]);
				auto const circulationCappedKW = chiller.authored.ratedCoolingCapacityKW
					* loop.lastCirculationFraction;
				chiller.lastDeliverableKW = std::min(circulationCappedKW,
					extractableKJ / tickSeconds);
				if (chiller.lastDeliverableKW <= 1e-12)
				{
					// Nothing can move through the evaporator this tick:
					// either no coolant is circulating to bring heat to it,
					// or the cold loop holds no extractable heat above its
					// chilled setpoint once this tick's exchange is counted.
					// Either way nothing is requested and the whole demand
					// stays unmet rather than buying idle power or a
					// temperature the coolant does not have.
					chiller.lastBlockReason = circulationCappedKW <= 1e-12
						? "no coolant circulation at the cold-side interface"
						: "the cold loop holds no extractable heat above the chilled setpoint";
					chiller.status = StationTransferStatus::Rejected;
					continue;
				}
				chiller.lastPowerDemandKW = chiller.lastDeliverableKW / chiller.authored.cop
					+ chiller.authored.auxiliaryPowerKW;
				auto const fromGeneration = std::min(chiller.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = chiller.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				chiller.lastPowerSuppliedKW = suppliedKW;
				auto const deliveredFraction = chiller.lastPowerDemandKW > 0.0
					? suppliedKW / chiller.lastPowerDemandKW : 0.0;
				chiller.lastDeliveredFraction = deliveredFraction;
				auto const coolingKW = chiller.lastDeliverableKW * deliveredFraction;
				chiller.lastCoolingKW = coolingKW;
				// Condenser heat equals extracted heat plus the work and
				// auxiliaries actually supplied: one boundary, booked once.
				chiller.lastCondenserKW = coolingKW + suppliedKW;
				chillerExtractionByLoop[chiller.loopIndex] += coolingKW;
				condenserHeatBySink[chiller.sinkIndex] += chiller.lastCondenserKW;
				chiller.extractedKJ += coolingKW * static_cast<double>(seconds);
				chiller.workKJ += suppliedKW * static_cast<double>(seconds);
				chiller.condenserKJ += chiller.lastCondenserKW * static_cast<double>(seconds);
				totalChillerPowerKW += suppliedKW;
				if (coolingKW + 1e-12 >= coolingDemandKW)
					chiller.status = StationTransferStatus::Full;
				else if (suppliedKW <= 1e-12)
					chiller.status = StationTransferStatus::Rejected;
				else chiller.status = StationTransferStatus::Partial;
			}
			// Water treatment (#402) runs after the chillers and before the
			// loop's second pass: it competes for the same generation-and-
			// battery pool, and the collectors that feed heat into those loops
			// must land before the loops integrate. Treatments run first, so
			// condensate a collector delivers this tick is processed from the
			// next tick on - water is never treated in the same breath in
			// which it appeared. One uniform scale per treatment per tick:
			// the smallest fraction of rated throughput the drawable raw
			// water, the treated and concentrate headroom and the drawable
			// consumables can sustain. The process is paid for before a single
			// kilogram moves; a tick that cannot pay at all is refused whole.
			double totalWaterTreatmentPowerKW{0.0};
			for (size_t orderIndex : waterTreatmentOrder)
			{
				auto& treatment = waterTreatments[orderIndex];
				auto& source = stores[treatment.sourceIndex];
				auto& treated = stores[treatment.treatedIndex];
				auto& concentrate = stores[treatment.concentrateIndex];
				auto& consumable = stores[treatment.consumableIndex];
				treatment.lastScale = 0.0;
				treatment.lastLimiter.clear();
				treatment.lastProcessedKg = 0.0;
				treatment.lastTreatedKg = 0.0;
				treatment.lastConcentrateKg = 0.0;
				treatment.lastPowerDemandKW = 0.0;
				treatment.lastPowerSuppliedKW = 0.0;
				treatment.lastConsumableRequested = 0.0;
				treatment.lastConsumableUsed = 0.0;
				treatment.lastQualityAccepted = false;
				treatment.lastHoldReason.clear();
				if (treatment.authored.throughputKgPerS <= 0.0)
				{
					treatment.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = treatment.authored.throughputKgPerS * tickSeconds;
				auto const drawableSourceUnits = std::max(0.0,
					qualified[treatment.sourceIndex] - source.authored.protectedReserve);
				auto const treatedHeadroomUnits = std::max(0.0, treated.authored.capacity
					- (qualified[treatment.treatedIndex] + unqualified[treatment.treatedIndex]));
				auto const concentrateHeadroomUnits = std::max(0.0, concentrate.authored.capacity
					- (qualified[treatment.concentrateIndex]
						+ unqualified[treatment.concentrateIndex]));
				auto const drawableConsumableUnits = std::max(0.0,
					qualified[treatment.consumableIndex] - consumable.authored.protectedReserve);
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(drawableSourceUnits, kgToMass(source.authored.unit, fullKg),
					"raw water store '" + source.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(treatedHeadroomUnits,
					kgToMass(treated.authored.unit, fullKg * treatment.authored.recoveryRatio),
					"treated water store '" + treated.authored.id + "' is full");
				constrain(concentrateHeadroomUnits,
					kgToMass(concentrate.authored.unit,
						fullKg * (1.0 - treatment.authored.recoveryRatio)),
					"concentrate store '" + concentrate.authored.id
					+ "' is full: the retained stream has nowhere to go");
				constrain(drawableConsumableUnits,
					treatment.authored.consumablePerKg * fullKg,
					"treatment consumable store '" + consumable.authored.id + "' is exhausted");
				scale = std::max(0.0, std::min(1.0, scale));
				treatment.lastScale = scale;
				treatment.lastLimiter = limiter;
				if (scale <= 0.0)
				{
					treatment.status = StationTransferStatus::Rejected;
					continue;
				}
				treatment.lastPowerDemandKW = scale * treatment.authored.throughputKgPerS
					* treatment.authored.specificEnergyKWhPerKg;
				auto const fromGeneration = std::min(treatment.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = treatment.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				treatment.lastPowerSuppliedKW = suppliedKW;
				if (treatment.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					// Nothing was paid: the tick is refused whole. No water
					// moves and no consumable is drawn - a treatment with no
					// power is not a treatment running for free.
					treatment.lastHoldReason = "no electrical supply: the treatment cannot run";
					treatment.status = StationTransferStatus::Rejected;
					continue;
				}
				// Move the water. Each output is clamped to its own headroom
				// so a one-ulp roundoff at the limit can never overshoot
				// capacity (#556), and the kg ledger is recomputed from the
				// units that actually moved.
				auto const processedUnits = std::min(
					kgToMass(source.authored.unit, fullKg * scale), drawableSourceUnits);
				auto const processedKg = massToKg(source.authored.unit, processedUnits);
				auto const treatedUnits = std::min(
					kgToMass(treated.authored.unit, processedKg * treatment.authored.recoveryRatio),
					treatedHeadroomUnits);
				auto const treatedKg = massToKg(treated.authored.unit, treatedUnits);
				auto const concentrateUnits = std::min(
					kgToMass(concentrate.authored.unit, std::max(0.0, processedKg - treatedKg)),
					concentrateHeadroomUnits);
				auto const concentrateKg = massToKg(concentrate.authored.unit, concentrateUnits);
				qualified[treatment.sourceIndex] -= processedUnits;
				flowOut[treatment.sourceIndex] += processedUnits;
				// Quality: the full rated energy dose is what qualifies. A
				// curtailed dose runs the machine but does not finish the
				// water, so the output is held unqualified rather than passed
				// through on a partial treatment.
				auto const curtailed = suppliedKW + 1e-12 < treatment.lastPowerDemandKW;
				treatment.lastQualityAccepted = !curtailed;
				if (curtailed)
				{
					treatment.lastHoldReason = "treatment energy curtailed: the treated output is held unqualified";
					unqualified[treatment.treatedIndex] += treatedUnits;
				}
				else qualified[treatment.treatedIndex] += treatedUnits;
				flowIn[treatment.treatedIndex] += treatedUnits;
				// The concentrate is retained, not usable: it lands on the
				// unqualified side of its store and stays accounted there
				// until a disposal capability exists.
				unqualified[treatment.concentrateIndex] += concentrateUnits;
				flowIn[treatment.concentrateIndex] += concentrateUnits;
				treatment.lastConsumableRequested = treatment.authored.consumablePerKg * processedKg;
				auto const consumableUsed = std::min(treatment.lastConsumableRequested,
					drawableConsumableUnits);
				qualified[treatment.consumableIndex] -= consumableUsed;
				flowOut[treatment.consumableIndex] += consumableUsed;
				treatment.lastConsumableUsed = consumableUsed;
				// Per-unit boundary: the source gave the processed mass up,
				// the treated and concentrate stores took their shares on, and
				// the consumable was used out of existence by the process.
				stagedUnitLoss[source.authored.unit] += processedUnits;
				stagedUnitLoss[treated.authored.unit] -= treatedUnits;
				stagedUnitLoss[concentrate.authored.unit] -= concentrateUnits;
				stagedUnitLoss[consumable.authored.unit] += consumableUsed;
				if (consumable.authored.composition)
				{
					ConstituentLedger netChange;
					addComposition(netChange, *consumable.authored.composition,
						massToKg(consumable.authored.unit, consumableUsed), -1.0);
					for (auto const& entry : netChange)
						stagedLoss[entry.first] -= entry.second;
				}
				treatment.lastProcessedKg = processedKg;
				treatment.lastTreatedKg = treatedKg;
				treatment.lastConcentrateKg = concentrateKg;
				treatment.processedTotalKg += processedKg;
				treatment.treatedTotalKg += treatedKg;
				treatment.concentrateTotalKg += concentrateKg;
				if (curtailed) treatment.heldTotalKg += treatedKg;
				else treatment.acceptedTotalKg += treatedKg;
				treatment.energySuppliedKJ += suppliedKW * tickSeconds;
				treatment.energyOwedKJ += treatment.lastPowerDemandKW * tickSeconds;
				treatment.consumablesUsedTotal += consumableUsed;
				treatment.lastCurtailed = curtailed;
				totalWaterTreatmentPowerKW += suppliedKW;
				if (!curtailed) treatment.status = StationTransferStatus::Full;
				else treatment.status = StationTransferStatus::Partial;
			}
			// Water collectors (#402) run after the treatments and before
			// the loop's second pass, so the latent heat they add is inside
			// the same integration the loop's ledger closes over. The cold
			// surface only works while coolant is actually circulating across
			// it and is colder than the compartment air: no circulation, no
			// condensation, and the reason stays visible rather than the
			// collector quietly idling. The condensate is capped by the vapour
			// held above the humidity target and by the destination's
			// headroom, and the latent heat it gave up is booked into the
			// loop's ledger and never into the compartment's - one
			// condensation, one heat source.
			std::vector<double> collectorHeatByLoop(coolantLoops.size(), 0.0);
			for (auto& collector : waterCollectors)
			{
				auto& compartment = atmospheres[collector.atmosphereIndex];
				auto& loop = coolantLoops[collector.loopIndex];
				auto& dest = stores[collector.destStoreIndex];
				collector.lastRequestedKg = 0.0;
				collector.lastCondensableKg = 0.0;
				collector.lastCollectedKg = 0.0;
				collector.lastLatentHeatKW = 0.0;
				collector.lastBlockReason.clear();
				if (collector.authored.maxRateKgPerS <= 0.0)
				{
					collector.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const requestedKg = collector.authored.maxRateKgPerS * tickSeconds;
				auto const saturatedKg = stationSaturatedVapourMassKg(
					stationSaturationVapourPressureKPa(compartment.temperatureC),
					compartment.authored.volumeM3, compartment.temperatureC);
				auto const condensableKg = std::max(0.0, compartment.waterVapourKg
					- collector.authored.targetRelativeHumidity * saturatedKg);
				collector.lastRequestedKg = requestedKg;
				collector.lastCondensableKg = condensableKg;
				collector.lastCirculationFraction = loop.lastCirculationFraction;
				collector.lastColdSurfaceAvailable = loop.lastCirculationFraction > 1e-12
					&& loop.temperatureC < compartment.temperatureC - 1e-9;
				if (!collector.lastColdSurfaceAvailable)
				{
					collector.lastBlockReason = loop.lastCirculationFraction <= 1e-12
						? "no coolant circulation across the cold surface: nothing can be condensed"
						: "the cold surface is not colder than the compartment air: no condensation";
					collector.status = StationTransferStatus::Rejected;
					continue;
				}
				if (condensableKg <= 1e-12)
				{
					collector.lastBlockReason = "the compartment holds no vapour above the collection target";
					collector.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const destHeadroomUnits = std::max(0.0, dest.authored.capacity
					- (qualified[collector.destStoreIndex] + unqualified[collector.destStoreIndex]));
				auto const collectedUnits = std::min(
					kgToMass(dest.authored.unit, std::min(requestedKg, condensableKg)),
					destHeadroomUnits);
				if (collectedUnits <= 0.0)
				{
					collector.lastBlockReason = "destination raw water store '" + dest.authored.id
						+ "' is full: nowhere to put the condensate";
					collector.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const collectedKg = massToKg(dest.authored.unit, collectedUnits);
				compartment.waterVapourKg -= collectedKg;
				compartment.lastCollectedKg = collectedKg;
				compartment.collectedKg += collectedKg;
				qualified[collector.destStoreIndex] += collectedUnits;
				flowIn[collector.destStoreIndex] += collectedUnits;
				stagedUnitCollected[dest.authored.unit] += collectedUnits;
				// The latent heat leaves the gas and enters the coolant, once.
				auto const latentHeatKJ = collectedKg * StationLatentHeatVaporizationKJPerKg;
				auto const latentHeatKW = latentHeatKJ / tickSeconds;
				collectorHeatByLoop[collector.loopIndex] += latentHeatKW;
				collector.lastLatentHeatKW = latentHeatKW;
				collector.latentHeatRoutedKJ += latentHeatKJ;
				collector.collectedTotalKg += collectedKg;
				if (collectedKg + 1e-12 >= requestedKg) collector.status = StationTransferStatus::Full;
				else collector.status = StationTransferStatus::Partial;
			}
			// Electrolysis (#404) runs after the water collectors and
			// before the loop's second pass: it competes for the same
			// generation-and-battery pool, and its process heat must land
			// in the sink before the reservoir integrates. One uniform
			// scale per cell per tick: the smallest fraction of the rated
			// split the drawable feed water and the product stores'
			// headroom can sustain. The cell is paid for before a kilogram
			// splits, and it only splits water the actually supplied
			// energy can make: the electrical dose either ends up bound in
			// the produced hydrogen or routed into the class-matched sink
			// as process heat, booked exactly once. A tick that demands
			// power and receives none is refused whole, and a sink at or
			// above the maximum sink temperature refuses the cell whole:
			// heat with nowhere to go is heat that cannot be made.
			std::vector<double> electrolysisHeatByReservoir(reservoirs.size(), 0.0);
			double totalElectrolyzerPowerKW{0.0};
			for (size_t orderIndex : electrolyzerOrder)
			{
				auto& cell = electrolyzers[orderIndex];
				auto& water = stores[cell.waterStoreIndex];
				auto& hydrogen = stores[cell.hydrogenStoreIndex];
				cell.lastScale = 0.0;
				cell.lastLimiter.clear();
				cell.lastRequestedKg = 0.0;
				cell.lastSplitKg = 0.0;
				cell.lastOxygenKg = 0.0;
				cell.lastHydrogenKg = 0.0;
				cell.lastPowerDemandKW = 0.0;
				cell.lastPowerSuppliedKW = 0.0;
				cell.lastUnmetProductionKg = 0.0;
				cell.lastCoolingAvailable = false;
				cell.lastBlockReason.clear();
				if (cell.authored.rateKgPerS <= 0.0)
				{
					cell.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = cell.authored.rateKgPerS * tickSeconds;
				// Cooling is qualified while the sink sits below the maximum
				// sink temperature: at or above it the interface cannot
				// accept the cell's process heat, so the cell cannot run.
				auto const& sink = reservoirs[cell.sinkIndex];
				cell.lastCoolingAvailable = sink.temperatureC + 1e-9
					< cell.authored.maxSinkTemperatureC;
				if (!cell.lastCoolingAvailable)
				{
					cell.lastBlockReason = "cooling is not qualified: sink at or above the maximum sink temperature";
					cell.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const drawableWaterUnits = std::max(0.0,
					qualified[cell.waterStoreIndex] - water.authored.protectedReserve);
				auto const hydrogenHeadroomUnits = std::max(0.0, hydrogen.authored.capacity
					- (qualified[cell.hydrogenStoreIndex] + unqualified[cell.hydrogenStoreIndex]));
				double oxygenHeadroomUnits{0.0};
				if (cell.oxygenStoreIndex != SIZE_MAX)
					oxygenHeadroomUnits = std::max(0.0, stores[cell.oxygenStoreIndex].authored.capacity
						- (qualified[cell.oxygenStoreIndex] + unqualified[cell.oxygenStoreIndex]));
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(drawableWaterUnits, kgToMass(water.authored.unit, fullKg),
					"feed water store '" + water.authored.id
					+ "' has nothing drawable above its protected reserve");
				if (cell.oxygenStoreIndex != SIZE_MAX)
					constrain(oxygenHeadroomUnits,
						kgToMass(stores[cell.oxygenStoreIndex].authored.unit,
							fullKg * StationElectrolysisOxygenMassPerWaterKg),
						"oxygen store '" + stores[cell.oxygenStoreIndex].authored.id
						+ "' is full: the product has nowhere to go");
				constrain(hydrogenHeadroomUnits,
					kgToMass(hydrogen.authored.unit,
						fullKg * (1.0 - StationElectrolysisOxygenMassPerWaterKg)),
					"hydrogen store '" + hydrogen.authored.id
					+ "' is full: the product has nowhere to go");
				scale = std::max(0.0, std::min(1.0, scale));
				cell.lastScale = scale;
				cell.lastLimiter = limiter;
				cell.lastRequestedKg = fullKg;
				if (scale <= 0.0)
				{
					cell.status = StationTransferStatus::Rejected;
					continue;
				}
				// The dose is authored in kWh per kilogram of split water,
				// the same physical unit as the declared chemical energy, so
				// the power the cell draws is the dose times the split rate
				// converted to the pool's kW (kWh/s x 3600). The ledger's
				// energy is kJ = kW x s, so at full power the supplied
				// energy always covers the chemical energy the validation
				// floor guarantees (#404).
				cell.lastPowerDemandKW = scale * cell.authored.rateKgPerS
					* cell.authored.specificEnergyKWhPerKgWater * 3600.0;
				auto const fromGeneration = std::min(cell.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = cell.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				cell.lastPowerSuppliedKW = suppliedKW;
				if (cell.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					// Nothing was paid: the tick is refused whole. No water
					// splits and no product appears - a cell with no power
					// is not a cell running for free.
					cell.lastBlockReason = "no electrical supply: the electrolyzer cannot run";
					cell.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const energySuppliedKJ = suppliedKW * tickSeconds;
				// Energy feasibility: the cell only splits water the
				// supplied energy can actually make. The dose may exceed
				// the chemical energy the products retain - the difference
				// is process heat - but never fall below it, so the split
				// never books more chemical energy than was paid for.
				auto const energyFeasibleKg = energySuppliedKJ
					/ StationElectrolysisChemicalEnergyKJPerKgWater;
				auto const feasibleSplitKg = std::min(scale * fullKg, energyFeasibleKg);
				// Move the water. Each flow is clamped to its own drawable
				// or headroom so a one-ulp roundoff at the limit can never
				// overshoot (#556), and the ledger is recomputed from what
				// actually moved.
				auto const waterUnits = std::min(
					kgToMass(water.authored.unit, feasibleSplitKg), drawableWaterUnits);
				auto const splitKg = massToKg(water.authored.unit, waterUnits);
				auto const oxygenKg = splitKg * StationElectrolysisOxygenMassPerWaterKg;
				double oxygenActualKg{oxygenKg};
				if (cell.oxygenStoreIndex != SIZE_MAX)
				{
					auto const& oxygen = stores[cell.oxygenStoreIndex];
					auto const oxygenUnits = std::min(
						kgToMass(oxygen.authored.unit, oxygenKg), oxygenHeadroomUnits);
					oxygenActualKg = massToKg(oxygen.authored.unit, oxygenUnits);
					qualified[cell.oxygenStoreIndex] += oxygenUnits;
					flowIn[cell.oxygenStoreIndex] += oxygenUnits;
					stagedUnitLoss[oxygen.authored.unit] -= oxygenUnits;
				}
				else
				{
					// Delivered straight into the compartment: the oxygen is
					// booked as an addition pathway of the compartment's own
					// closure, so every gram the cell made is visible in the
					// air it was produced for. The feed-side store loss
					// already nets the delivered mass out of the store
					// boundary; nothing vanishes.
					auto& compartment = atmospheres[cell.atmosphereIndex];
					compartment.oxygenKg += oxygenKg;
					compartment.electrolysisOxygenKg += oxygenKg;
					compartment.lastElectrolysisOxygenKg = oxygenKg;
				}
				auto const hydrogenKg = splitKg - oxygenActualKg;
				auto const hydrogenUnits = std::min(
					kgToMass(hydrogen.authored.unit, hydrogenKg), hydrogenHeadroomUnits);
				auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
				qualified[cell.waterStoreIndex] -= waterUnits;
				flowOut[cell.waterStoreIndex] += waterUnits;
				qualified[cell.hydrogenStoreIndex] += hydrogenUnits;
				flowIn[cell.hydrogenStoreIndex] += hydrogenUnits;
				stagedUnitLoss[water.authored.unit] += waterUnits;
				stagedUnitLoss[hydrogen.authored.unit] -= hydrogenUnits;
				// Energy: the dose either stays bound in the produced
				// hydrogen or leaves as process heat routed into the sink,
				// once and nowhere else.
				auto const chemicalKJ = hydrogenActualKg
					* StationElectrolysisChemicalEnergyHydrogenKJPerKg;
				auto const heatKJ = std::max(0.0, energySuppliedKJ - chemicalKJ);
				electrolysisHeatByReservoir[cell.sinkIndex] += heatKJ / tickSeconds;
				cell.lastSplitKg = splitKg;
				cell.lastOxygenKg = oxygenActualKg;
				cell.lastHydrogenKg = hydrogenActualKg;
				cell.lastUnmetProductionKg = std::max(0.0, scale * fullKg - splitKg);
				cell.waterSplitKg += splitKg;
				cell.oxygenProducedKg += oxygenActualKg;
				cell.hydrogenProducedKg += hydrogenActualKg;
				cell.energySuppliedKJ += energySuppliedKJ;
				cell.chemicalEnergyKJ += chemicalKJ;
				cell.heatRoutedKJ += heatKJ;
				totalElectrolyzerPowerKW += suppliedKW;
				if (splitKg + 1e-12 >= fullKg
					&& suppliedKW + 1e-12 >= cell.lastPowerDemandKW)
					cell.status = StationTransferStatus::Full;
				else cell.status = StationTransferStatus::Partial;
			}
			// CO2 removal (#404) runs in the same phase: captured gas is
			// inventory, not vanished carbon. The machine can only scrub
			// the CO2 the compartment actually holds, into the capture
			// store's remaining headroom, on the consumable it can
			// actually draw and the power the pool can actually supply.
			// Every axis of finiteness is a scale cap, and the unmet
			// atmospheric service is the explicit remainder of the rated
			// request against them. A tick that demands power and receives
			// none is refused whole: no CO2 is scrubbed and no consumable
			// is drawn.
			double totalCo2CapturePowerKW{0.0};
			for (size_t orderIndex : co2CaptureOrder)
			{
				auto& capture = co2Captures[orderIndex];
				auto& compartment = atmospheres[capture.atmosphereIndex];
				auto& destination = stores[capture.captureStoreIndex];
				capture.lastScale = 0.0;
				capture.lastLimiter.clear();
				capture.lastRequestedKg = 0.0;
				capture.lastCapturedKg = 0.0;
				capture.lastPowerDemandKW = 0.0;
				capture.lastPowerSuppliedKW = 0.0;
				capture.lastConsumableRequested = 0.0;
				capture.lastConsumableUsed = 0.0;
				capture.lastBlockReason.clear();
				if (capture.authored.maxRateKgPerS <= 0.0)
				{
					capture.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = capture.authored.maxRateKgPerS * tickSeconds;
				auto const co2AvailableKg = std::max(0.0, compartment.carbonDioxideKg);
				auto const captureHeadroomUnits = std::max(0.0, destination.authored.capacity
					- (qualified[capture.captureStoreIndex]
						+ unqualified[capture.captureStoreIndex]));
				double drawableConsumableUnits{0.0};
				if (capture.consumableStoreIndex != SIZE_MAX)
					drawableConsumableUnits = std::max(0.0, qualified[capture.consumableStoreIndex]
						- stores[capture.consumableStoreIndex].authored.protectedReserve);
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(co2AvailableKg, fullKg,
					"compartment '" + compartment.authored.id
					+ "' holds no carbon dioxide to capture");
				constrain(captureHeadroomUnits, kgToMass(destination.authored.unit, fullKg),
					"capture store '" + destination.authored.id
					+ "' is full: the captured gas has nowhere to go");
				if (capture.consumableStoreIndex != SIZE_MAX)
					constrain(drawableConsumableUnits,
						capture.authored.consumablePerKg * fullKg,
						"capture consumable store '"
						+ stores[capture.consumableStoreIndex].authored.id
						+ "' is exhausted");
				scale = std::max(0.0, std::min(1.0, scale));
				capture.lastScale = scale;
				capture.lastLimiter = limiter;
				capture.lastRequestedKg = fullKg;
				if (scale <= 0.0)
				{
					capture.status = StationTransferStatus::Rejected;
					continue;
				}
				capture.lastPowerDemandKW = scale * capture.authored.maxRateKgPerS
					* capture.authored.specificEnergyKWhPerKg;
				auto const fromGeneration = std::min(capture.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = capture.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				capture.lastPowerSuppliedKW = suppliedKW;
				if (capture.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					capture.lastBlockReason = "no electrical supply: the capture machine cannot run";
					capture.status = StationTransferStatus::Rejected;
					continue;
				}
				// Capture only what the compartment holds and the store can
				// take, clamped per flow so roundoff never overshoots.
				auto const wantedKg = std::min(fullKg * scale, co2AvailableKg);
				auto const capturedUnits = std::min(
					kgToMass(destination.authored.unit, wantedKg), captureHeadroomUnits);
				auto const capturedKg = massToKg(destination.authored.unit, capturedUnits);
				compartment.carbonDioxideKg -= capturedKg;
				compartment.capturedCarbonDioxideKg += capturedKg;
				compartment.lastCo2CapturedKg = capturedKg;
				qualified[capture.captureStoreIndex] += capturedUnits;
				flowIn[capture.captureStoreIndex] += capturedUnits;
				// The captured gas enters the store network from outside it,
				// exactly like collector condensate: the boundary closes as
				// current + lost - delivered - collected, so the carbon is
				// accounted inflow, never implicit creation.
				stagedUnitCollected[destination.authored.unit] += capturedUnits;
				if (capture.consumableStoreIndex != SIZE_MAX)
				{
					auto& consumable = stores[capture.consumableStoreIndex];
					capture.lastConsumableRequested = capture.authored.consumablePerKg * capturedKg;
					auto const consumableUsed = std::min(capture.lastConsumableRequested,
						drawableConsumableUnits);
					qualified[capture.consumableStoreIndex] -= consumableUsed;
					flowOut[capture.consumableStoreIndex] += consumableUsed;
					capture.lastConsumableUsed = consumableUsed;
					stagedUnitLoss[consumable.authored.unit] += consumableUsed;
					if (consumable.authored.composition)
					{
						ConstituentLedger netChange;
						addComposition(netChange, *consumable.authored.composition,
							massToKg(consumable.authored.unit, consumableUsed), -1.0);
						for (auto const& entry : netChange)
							stagedLoss[entry.first] -= entry.second;
					}
					capture.consumablesUsedTotal += consumableUsed;
				}
				capture.lastCapturedKg = capturedKg;
				capture.capturedTotalKg += capturedKg;
				capture.energySuppliedKJ += suppliedKW * tickSeconds;
				totalCo2CapturePowerKW += suppliedKW;
				if (capturedKg + 1e-12 >= fullKg) capture.status = StationTransferStatus::Full;
				else if (capturedKg > 1e-12) capture.status = StationTransferStatus::Partial;
				else capture.status = StationTransferStatus::Rejected;
			}
			// Carbon recovery (#405) runs in the same phase after CO2
			// capture: the Sabatier reactor converts captured CO2 and
			// hydrogen into methane and water, and the methane processor
			// splits methane into recovered hydrogen and solid carbon. Both
			// compete for the same generation-and-battery pool, and their
			// heat must land in the sink before the reservoir integrates.
			// Each stage runs at one uniform scale per tick: the fraction of
			// the rated throughput the drawable feeds and the product
			// stores' headroom can sustain. The stage is paid for before a
			// kilogram converts, and it only converts what the actually
			// supplied energy can drive at the declared dose: the reactor
			// routes its electrical dose plus the exothermic reaction heat
			// into the class-matched sink, and the processor routes its dose
			// minus the absorbed endothermic reaction heat into the sink,
			// each booked exactly once. A tick that demands power and
			// receives none is refused whole, and a sink at or above the
			// maximum sink temperature refuses the stage whole: heat with
			// nowhere to go is heat that cannot be made.
			std::vector<double> sabatierHeatByReservoir(reservoirs.size(), 0.0);
			std::vector<double> methaneHeatByReservoir(reservoirs.size(), 0.0);
			double totalSabatierPowerKW{0.0};
			for (size_t orderIndex : sabatierOrder)
			{
				auto& reactor = sabatierReactors[orderIndex];
				auto& co2 = stores[reactor.co2StoreIndex];
				auto& hydrogen = stores[reactor.hydrogenStoreIndex];
				auto& methane = stores[reactor.methaneStoreIndex];
				auto& water = stores[reactor.waterStoreIndex];
				reactor.lastScale = 0.0;
				reactor.lastLimiter.clear();
				reactor.lastRequestedKg = 0.0;
				reactor.lastCo2ConsumedKg = 0.0;
				reactor.lastHydrogenConsumedKg = 0.0;
				reactor.lastMethaneProducedKg = 0.0;
				reactor.lastWaterProducedKg = 0.0;
				reactor.lastPowerDemandKW = 0.0;
				reactor.lastPowerSuppliedKW = 0.0;
				reactor.lastUnmetConversionKg = 0.0;
				reactor.lastReactionHeatKW = 0.0;
				reactor.lastCoolingAvailable = false;
				reactor.lastBlockReason.clear();
				if (reactor.authored.rateKgPerS <= 0.0)
				{
					reactor.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = reactor.authored.rateKgPerS * tickSeconds;
				// Cooling is qualified while the sink sits below the maximum
				// sink temperature: at or above it the interface cannot
				// accept the reactor's dose and reaction heat.
				auto const& sink = reservoirs[reactor.sinkIndex];
				reactor.lastCoolingAvailable = sink.temperatureC + 1e-9
					< reactor.authored.maxSinkTemperatureC;
				if (!reactor.lastCoolingAvailable)
				{
					reactor.lastBlockReason = "cooling is not qualified: sink at or above the maximum sink temperature";
					reactor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const drawableCo2Units = std::max(0.0,
					qualified[reactor.co2StoreIndex] - co2.authored.protectedReserve);
				auto const drawableHydrogenUnits = std::max(0.0,
					qualified[reactor.hydrogenStoreIndex] - hydrogen.authored.protectedReserve);
				auto const methaneHeadroomUnits = std::max(0.0, methane.authored.capacity
					- (qualified[reactor.methaneStoreIndex] + unqualified[reactor.methaneStoreIndex]));
				auto const waterHeadroomUnits = std::max(0.0, water.authored.capacity
					- (qualified[reactor.waterStoreIndex] + unqualified[reactor.waterStoreIndex]));
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(drawableCo2Units, kgToMass(co2.authored.unit, fullKg),
					"CO2 feed store '" + co2.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(drawableHydrogenUnits,
					kgToMass(hydrogen.authored.unit,
						fullKg * StationSabatierHydrogenMassPerCarbonDioxideKg),
					"hydrogen feed store '" + hydrogen.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(methaneHeadroomUnits,
					kgToMass(methane.authored.unit,
						fullKg * StationSabatierMethaneMassPerCarbonDioxideKg),
					"methane store '" + methane.authored.id
					+ "' is full: the product has nowhere to go");
				constrain(waterHeadroomUnits,
					kgToMass(water.authored.unit,
						fullKg * StationSabatierWaterMassPerCarbonDioxideKg),
					"product water store '" + water.authored.id
					+ "' is full: the water has nowhere to go");
				scale = std::max(0.0, std::min(1.0, scale));
				reactor.lastScale = scale;
				reactor.lastLimiter = limiter;
				reactor.lastRequestedKg = fullKg;
				if (scale <= 0.0)
				{
					reactor.lastBlockReason = limiter;
					reactor.status = StationTransferStatus::Rejected;
					continue;
				}
				// The dose is authored in kWh per kilogram of converted CO2,
				// so the power the reactor draws is the dose times the
				// conversion rate converted to the pool's kW (kWh/s x 3600),
				// mirroring the electrolysis contract (#404).
				reactor.lastPowerDemandKW = scale * reactor.authored.rateKgPerS
					* reactor.authored.specificEnergyKWhPerKgCO2 * 3600.0;
				auto const fromGeneration = std::min(reactor.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = reactor.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				reactor.lastPowerSuppliedKW = suppliedKW;
				if (reactor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					// Nothing was paid: the tick is refused whole. No CO2
					// converts and no product appears - a reactor with no
					// power is not a reactor running for free.
					reactor.lastBlockReason = "no electrical supply: the Sabatier reactor cannot run";
					reactor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const energySuppliedKJ = suppliedKW * tickSeconds;
				// Energy feasibility: the reactor only converts the CO2 the
				// supplied energy can pay for at the declared dose. The dose
				// is the operating cost of the conversion; curtailed power
				// reduces conversion rather than gifting products for free.
				auto const doseKJPerKg = reactor.authored.specificEnergyKWhPerKgCO2 * 3600.0;
				auto const energyFeasibleKg = doseKJPerKg > 0.0
					? energySuppliedKJ / doseKJPerKg : scale * fullKg;
				// The extent is capped by the reactants that are actually
				// drawable above their protected reserves, so the stage never
				// plans a conversion the feeds cannot sustain.
				auto extentKg = std::min(energyFeasibleKg,
					massToKg(co2.authored.unit, drawableCo2Units));
				extentKg = std::min(extentKg,
					massToKg(hydrogen.authored.unit, drawableHydrogenUnits)
						/ StationSabatierHydrogenMassPerCarbonDioxideKg);
				// Move the flows. Each flow is clamped to its own drawable
				// or headroom so a one-ulp roundoff at the limit can never
				// overshoot (#556), and the ledger is recomputed from what
				// actually moved.
				auto const co2Units = std::min(kgToMass(co2.authored.unit, extentKg), drawableCo2Units);
				auto const co2ActualKg = massToKg(co2.authored.unit, co2Units);
				auto const hydrogenUnits = std::min(kgToMass(hydrogen.authored.unit,
					co2ActualKg * StationSabatierHydrogenMassPerCarbonDioxideKg),
					drawableHydrogenUnits);
				auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
				// Products follow the limiting reactant that actually moved,
				// so the stage never books more product than the reactants
				// that moved can make.
				auto const extentActualKg = std::min(co2ActualKg,
					hydrogenActualKg / StationSabatierHydrogenMassPerCarbonDioxideKg);
				auto const methaneUnits = std::min(kgToMass(methane.authored.unit,
					extentActualKg * StationSabatierMethaneMassPerCarbonDioxideKg),
					methaneHeadroomUnits);
				auto const methaneActualKg = massToKg(methane.authored.unit, methaneUnits);
				auto const waterUnits = std::min(kgToMass(water.authored.unit,
					extentActualKg * StationSabatierWaterMassPerCarbonDioxideKg),
					waterHeadroomUnits);
				auto const waterActualKg = massToKg(water.authored.unit, waterUnits);
				qualified[reactor.co2StoreIndex] -= co2Units;
				flowOut[reactor.co2StoreIndex] += co2Units;
				qualified[reactor.hydrogenStoreIndex] -= hydrogenUnits;
				flowOut[reactor.hydrogenStoreIndex] += hydrogenUnits;
				qualified[reactor.methaneStoreIndex] += methaneUnits;
				flowIn[reactor.methaneStoreIndex] += methaneUnits;
				qualified[reactor.waterStoreIndex] += waterUnits;
				flowIn[reactor.waterStoreIndex] += waterUnits;
				stagedUnitLoss[co2.authored.unit] += co2Units;
				stagedUnitLoss[hydrogen.authored.unit] += hydrogenUnits;
				stagedUnitLoss[methane.authored.unit] -= methaneUnits;
				stagedUnitLoss[water.authored.unit] -= waterUnits;
				// Energy: the electrical dose plus the exothermic heat the
				// products do not retain leave through the sink, once and
				// nowhere else.
				auto const reactionHeatKJ = extentActualKg
					* StationSabatierReactionHeatKJPerKgCarbonDioxide;
				auto const heatKJ = energySuppliedKJ + reactionHeatKJ;
				sabatierHeatByReservoir[reactor.sinkIndex] += heatKJ / tickSeconds;
				reactor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
				reactor.lastCo2ConsumedKg = co2ActualKg;
				reactor.lastHydrogenConsumedKg = hydrogenActualKg;
				reactor.lastMethaneProducedKg = methaneActualKg;
				reactor.lastWaterProducedKg = waterActualKg;
				reactor.lastUnmetConversionKg = std::max(0.0, scale * fullKg - extentActualKg);
				reactor.co2ConsumedKg += co2ActualKg;
				reactor.hydrogenConsumedKg += hydrogenActualKg;
				reactor.methaneProducedKg += methaneActualKg;
				reactor.waterProducedKg += waterActualKg;
				reactor.energySuppliedKJ += energySuppliedKJ;
				reactor.reactionHeatKJ += reactionHeatKJ;
				reactor.heatRoutedKJ += heatKJ;
				totalSabatierPowerKW += suppliedKW;
				if (extentActualKg + 1e-12 >= fullKg
					&& suppliedKW + 1e-12 >= reactor.lastPowerDemandKW)
					reactor.status = StationTransferStatus::Full;
				else reactor.status = StationTransferStatus::Partial;
			}
			double totalMethaneProcessingPowerKW{0.0};
			for (size_t orderIndex : methaneProcessorOrder)
			{
				auto& processor = methaneProcessors[orderIndex];
				auto& methane = stores[processor.methaneStoreIndex];
				auto& hydrogen = stores[processor.hydrogenStoreIndex];
				auto& carbon = stores[processor.carbonStoreIndex];
				processor.lastScale = 0.0;
				processor.lastLimiter.clear();
				processor.lastRequestedKg = 0.0;
				processor.lastMethaneConsumedKg = 0.0;
				processor.lastHydrogenReturnedKg = 0.0;
				processor.lastCarbonProducedKg = 0.0;
				processor.lastPowerDemandKW = 0.0;
				processor.lastPowerSuppliedKW = 0.0;
				processor.lastUnmetProcessingKg = 0.0;
				processor.lastReactionHeatKW = 0.0;
				processor.lastCoolingAvailable = false;
				processor.lastBlockReason.clear();
				if (processor.authored.rateKgPerS <= 0.0)
				{
					processor.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = processor.authored.rateKgPerS * tickSeconds;
				auto const& sink = reservoirs[processor.sinkIndex];
				processor.lastCoolingAvailable = sink.temperatureC + 1e-9
					< processor.authored.maxSinkTemperatureC;
				if (!processor.lastCoolingAvailable)
				{
					processor.lastBlockReason = "cooling is not qualified: sink at or above the maximum sink temperature";
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const drawableMethaneUnits = std::max(0.0,
					qualified[processor.methaneStoreIndex] - methane.authored.protectedReserve);
				auto const hydrogenHeadroomUnits = std::max(0.0, hydrogen.authored.capacity
					- (qualified[processor.hydrogenStoreIndex] + unqualified[processor.hydrogenStoreIndex]));
				auto const carbonHeadroomUnits = std::max(0.0, carbon.authored.capacity
					- (qualified[processor.carbonStoreIndex] + unqualified[processor.carbonStoreIndex]));
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(drawableMethaneUnits, kgToMass(methane.authored.unit, fullKg),
					"methane feed store '" + methane.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(hydrogenHeadroomUnits,
					kgToMass(hydrogen.authored.unit,
						fullKg * StationMethaneProcessingHydrogenMassPerMethaneKg),
					"recovered hydrogen store '" + hydrogen.authored.id
					+ "' is full: the recovered hydrogen has nowhere to go");
				constrain(carbonHeadroomUnits,
					kgToMass(carbon.authored.unit,
						fullKg * StationMethaneProcessingCarbonMassPerMethaneKg),
					"carbon store '" + carbon.authored.id
					+ "' is full: the solid carbon has nowhere to go");
				scale = std::max(0.0, std::min(1.0, scale));
				processor.lastScale = scale;
				processor.lastLimiter = limiter;
				processor.lastRequestedKg = fullKg;
				if (scale <= 0.0)
				{
					processor.lastBlockReason = limiter;
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				processor.lastPowerDemandKW = scale * processor.authored.rateKgPerS
					* processor.authored.specificEnergyKWhPerKgCH4 * 3600.0;
				auto const fromGeneration = std::min(processor.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = processor.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				processor.lastPowerSuppliedKW = suppliedKW;
				if (processor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					processor.lastBlockReason = "no electrical supply: the methane processor cannot run";
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const energySuppliedKJ = suppliedKW * tickSeconds;
				// Energy feasibility: the processor only splits the methane
				// the supplied energy can drive against the endothermic
				// reaction demand. The validation floor guarantees the dose
				// covers the demand, so the absorbed heat never exceeds what
				// was paid for and the routed waste heat stays non-negative.
				auto const energyFeasibleKg = energySuppliedKJ
					/ StationMethaneProcessingReactionHeatKJPerKgMethane;
				auto const processedKg = std::min(scale * fullKg, energyFeasibleKg);
				// Move the flows, clamped per flow so roundoff never
				// overshoots, and recompute the ledger from what moved.
				auto const methaneUnits = std::min(
					kgToMass(methane.authored.unit, processedKg), drawableMethaneUnits);
				auto const methaneActualKg = massToKg(methane.authored.unit, methaneUnits);
				auto const hydrogenUnits = std::min(kgToMass(hydrogen.authored.unit,
					methaneActualKg * StationMethaneProcessingHydrogenMassPerMethaneKg),
					hydrogenHeadroomUnits);
				auto const hydrogenActualKg = massToKg(hydrogen.authored.unit, hydrogenUnits);
				auto const carbonUnits = std::min(kgToMass(carbon.authored.unit,
					methaneActualKg * StationMethaneProcessingCarbonMassPerMethaneKg),
					carbonHeadroomUnits);
				auto const carbonActualKg = massToKg(carbon.authored.unit, carbonUnits);
				qualified[processor.methaneStoreIndex] -= methaneUnits;
				flowOut[processor.methaneStoreIndex] += methaneUnits;
				qualified[processor.hydrogenStoreIndex] += hydrogenUnits;
				flowIn[processor.hydrogenStoreIndex] += hydrogenUnits;
				qualified[processor.carbonStoreIndex] += carbonUnits;
				flowIn[processor.carbonStoreIndex] += carbonUnits;
				stagedUnitLoss[methane.authored.unit] += methaneUnits;
				stagedUnitLoss[hydrogen.authored.unit] -= hydrogenUnits;
				stagedUnitLoss[carbon.authored.unit] -= carbonUnits;
				// Energy: the dose either drove the endothermic reaction or
				// leaves as process heat routed into the sink, once.
				auto const reactionHeatKJ = methaneActualKg
					* StationMethaneProcessingReactionHeatKJPerKgMethane;
				auto const heatKJ = std::max(0.0, energySuppliedKJ - reactionHeatKJ);
				methaneHeatByReservoir[processor.sinkIndex] += heatKJ / tickSeconds;
				processor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
				processor.lastMethaneConsumedKg = methaneActualKg;
				processor.lastHydrogenReturnedKg = hydrogenActualKg;
				processor.lastCarbonProducedKg = carbonActualKg;
				processor.lastUnmetProcessingKg = std::max(0.0, scale * fullKg - methaneActualKg);
				processor.methaneConsumedKg += methaneActualKg;
				processor.hydrogenReturnedKg += hydrogenActualKg;
				processor.carbonProducedKg += carbonActualKg;
				processor.energySuppliedKJ += energySuppliedKJ;
				processor.reactionHeatKJ += reactionHeatKJ;
				processor.heatRoutedKJ += heatKJ;
				totalMethaneProcessingPowerKW += suppliedKW;
				if (methaneActualKg + 1e-12 >= fullKg
					&& suppliedKW + 1e-12 >= processor.lastPowerDemandKW)
					processor.status = StationTransferStatus::Full;
				else processor.status = StationTransferStatus::Partial;
			}
			// Nutrient recovery (#407) runs in the same phase after carbon
			// recovery: the microbial processor digests the compound
			// organic-waste feed, recovers its bound N/P/K/minerals at the
			// declared per-constituent yields and its moisture at the
			// declared water recovery, releases the declared CO2 share, and
			// leaves the unrecoverable residue as the mass-balance
			// remainder - so the digestion closes on mass by construction
			// and the feed's atmospheric nitrogen can only pass through to
			// the residue, never into the recovered nutrients. The stage
			// competes for the same generation-and-battery pool, pays for
			// itself before a kilogram digests, and only digests what the
			// actually supplied energy can drive at the declared dose. Its
			// exothermic reaction heat plus the electrical dose route into
			// the class-matched sink exactly once. Feeds draw only above
			// their protected reserves; saturated destinations cap the
			// whole stage through the uniform scale instead of venting; the
			// static operating window gates temperature eligibility against
			// the sink reservoir without modelling any dynamic culture
			// condition.
			std::vector<double> microbialHeatByReservoir(reservoirs.size(), 0.0);
			double totalMicrobialProcessingPowerKW{0.0};
			for (size_t orderIndex : microbialProcessorOrder)
			{
				auto& processor = microbialProcessors[orderIndex];
				auto& feed = stores[processor.feedStoreIndex];
				auto& oxygen = stores[processor.oxygenStoreIndex];
				auto& water = stores[processor.waterStoreIndex];
				auto& consumable = stores[processor.consumableStoreIndex];
				auto& nutrient = stores[processor.nutrientStoreIndex];
				auto& recoveredWater = stores[processor.recoveredWaterStoreIndex];
				auto& co2 = stores[processor.co2StoreIndex];
				auto& residue = stores[processor.residueStoreIndex];
				processor.lastScale = 0.0;
				processor.lastLimiter.clear();
				processor.lastRequestedKg = 0.0;
				processor.lastFeedConsumedKg = 0.0;
				processor.lastOxygenConsumedKg = 0.0;
				processor.lastWaterConsumedKg = 0.0;
				processor.lastConsumableRequested = 0.0;
				processor.lastConsumableUsed = 0.0;
				processor.lastConsumableUnmet = 0.0;
				processor.lastNutrientProducedKg = 0.0;
				processor.lastRecoveredWaterKg = 0.0;
				processor.lastCo2ProducedKg = 0.0;
				processor.lastResidueProducedKg = 0.0;
				processor.lastPowerDemandKW = 0.0;
				processor.lastPowerSuppliedKW = 0.0;
				processor.lastUnmetProcessingKg = 0.0;
				processor.lastReactionHeatKW = 0.0;
				processor.lastTemperatureEligible = false;
				processor.lastBlockReason.clear();
				if (processor.authored.rateKgPerS <= 0.0)
				{
					processor.status = StationTransferStatus::Idle;
					continue;
				}
				auto const tickSeconds = static_cast<double>(seconds);
				auto const fullKg = processor.authored.rateKgPerS * tickSeconds;
				// Static operating window: the culture runs at or above the
				// minimum operating temperature and below the maximum sink
				// temperature. Outside the window the stage is refused whole
				// and spends nothing - no dynamic condition is modelled.
				auto const& sink = reservoirs[processor.sinkIndex];
				processor.lastTemperatureEligible = sink.temperatureC + 1e-9
					>= processor.authored.minOperatingTemperatureC
					&& sink.temperatureC + 1e-9 < processor.authored.maxSinkTemperatureC;
				if (!processor.lastTemperatureEligible)
				{
					processor.lastBlockReason = sink.temperatureC + 1e-9
						< processor.authored.minOperatingTemperatureC
						? "below the minimum operating temperature: the culture is dormant"
						: "cooling is not qualified: sink at or above the maximum sink temperature";
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const& composition = *feed.authored.composition;
				auto const dryPerKgFeed = 1.0 - composition.moisture;
				// The recovered nutrients are funded by the feed's bound
				// constituents only: bound nitrogen, phosphorus, potassium
				// and the grouped mineral term, each at its declared
				// recovery fraction. Atmospheric nitrogen is not in this
				// sum - it is not a nutrient and no configured process here
				// converts it.
				auto const nutrientPerKgFeed = dryPerKgFeed * (composition.nutrientNitrogen
					* processor.authored.nitrogenRecovery + composition.phosphorus
					* processor.authored.phosphorusRecovery + composition.potassium
					* processor.authored.potassiumRecovery + composition.otherMinerals
					* processor.authored.mineralsRecovery);
				auto const recoveredWaterPerKgFeed = composition.moisture
					* processor.authored.waterRecovery;
				// The residue is the mass-balance remainder of the declared
				// inputs and outputs per kilogram of feed: validation
				// guaranteed it is non-negative, so the digestion can never
				// create or destroy mass.
				auto const residuePerKgFeed = 1.0 + processor.authored.oxygenPerKgFeedKg
					+ processor.authored.processWaterPerKgFeedKg - nutrientPerKgFeed
					- recoveredWaterPerKgFeed - processor.authored.co2PerKgFeedKg;
				auto const drawableFeedUnits = std::max(0.0,
					qualified[processor.feedStoreIndex] - feed.authored.protectedReserve);
				auto const drawableOxygenUnits = std::max(0.0,
					qualified[processor.oxygenStoreIndex] - oxygen.authored.protectedReserve);
				auto const drawableWaterUnits = std::max(0.0,
					qualified[processor.waterStoreIndex] - water.authored.protectedReserve);
				auto const drawableConsumableUnits = std::max(0.0,
					qualified[processor.consumableStoreIndex] - consumable.authored.protectedReserve);
				auto const nutrientHeadroomUnits = std::max(0.0, nutrient.authored.capacity
					- (qualified[processor.nutrientStoreIndex] + unqualified[processor.nutrientStoreIndex]));
				auto const recoveredWaterHeadroomUnits = std::max(0.0,
					recoveredWater.authored.capacity - (qualified[processor.recoveredWaterStoreIndex]
						+ unqualified[processor.recoveredWaterStoreIndex]));
				auto const co2HeadroomUnits = std::max(0.0, co2.authored.capacity
					- (qualified[processor.co2StoreIndex] + unqualified[processor.co2StoreIndex]));
				auto const residueHeadroomUnits = std::max(0.0, residue.authored.capacity
					- (qualified[processor.residueStoreIndex] + unqualified[processor.residueStoreIndex]));
				double scale = 1.0;
				std::string limiter;
				auto const constrain = [&](double available, double required,
					std::string const& reason)
				{
					if (required <= 0.0) return;
					auto const fraction = available / required;
					if (fraction < scale) { scale = fraction; limiter = reason; }
				};
				constrain(drawableFeedUnits, kgToMass(feed.authored.unit, fullKg),
					"organic waste feed store '" + feed.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(drawableOxygenUnits,
					kgToMass(oxygen.authored.unit,
						fullKg * processor.authored.oxygenPerKgFeedKg),
					"oxygen store '" + oxygen.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(drawableWaterUnits,
					kgToMass(water.authored.unit,
						fullKg * processor.authored.processWaterPerKgFeedKg),
					"process water store '" + water.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(drawableConsumableUnits,
					fullKg * processor.authored.consumablePerKgFeed,
					"consumable store '" + consumable.authored.id
					+ "' has nothing drawable above its protected reserve");
				constrain(nutrientHeadroomUnits,
					kgToMass(nutrient.authored.unit, fullKg * nutrientPerKgFeed),
					"nutrient store '" + nutrient.authored.id
					+ "' is full: the recovered nutrients have nowhere to go");
				constrain(recoveredWaterHeadroomUnits,
					kgToMass(recoveredWater.authored.unit, fullKg * recoveredWaterPerKgFeed),
					"recovered water store '" + recoveredWater.authored.id
					+ "' is full: the recovered water has nowhere to go");
				constrain(co2HeadroomUnits,
					kgToMass(co2.authored.unit, fullKg * processor.authored.co2PerKgFeedKg),
					"CO2 store '" + co2.authored.id
					+ "' is full: the carbon dioxide has nowhere to go");
				constrain(residueHeadroomUnits,
					kgToMass(residue.authored.unit, fullKg * residuePerKgFeed),
					"residue store '" + residue.authored.id
					+ "' is full: the unrecoverable residue has nowhere to go");
				scale = std::max(0.0, std::min(1.0, scale));
				processor.lastScale = scale;
				processor.lastLimiter = limiter;
				processor.lastRequestedKg = fullKg;
				if (scale <= 0.0)
				{
					processor.lastBlockReason = limiter;
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				processor.lastPowerDemandKW = scale * processor.authored.rateKgPerS
					* processor.authored.specificEnergyKWhPerKgFeed * 3600.0;
				auto const fromGeneration = std::min(processor.lastPowerDemandKW, availableGeneration);
				availableGeneration -= fromGeneration;
				auto const ask = processor.lastPowerDemandKW - fromGeneration;
				auto const fromBattery = drawBatteries(ask);
				auto const suppliedKW = fromGeneration + fromBattery;
				batterySuppliedKW += fromBattery;
				batteryShortfallKW += ask - fromBattery;
				processor.lastPowerSuppliedKW = suppliedKW;
				if (processor.lastPowerDemandKW > 1e-12 && suppliedKW <= 1e-12)
				{
					// Nothing was paid: the tick is refused whole. No feed
					// digests, no oxygen is drawn and no product appears - a
					// processor with no power is not a processor running for
					// free.
					processor.lastBlockReason =
						"no electrical supply: the microbial processor cannot run";
					processor.status = StationTransferStatus::Rejected;
					continue;
				}
				auto const energySuppliedKJ = suppliedKW * tickSeconds;
				// Energy feasibility: the processor only digests the feed
				// the supplied energy can pay for at the declared dose;
				// curtailed power reduces digestion rather than gifting
				// products for free.
				auto const doseKJPerKg = processor.authored.specificEnergyKWhPerKgFeed * 3600.0;
				auto const energyFeasibleKg = doseKJPerKg > 0.0
					? energySuppliedKJ / doseKJPerKg : scale * fullKg;
				// The extent is capped by the inputs that are actually
				// drawable above their protected reserves, so the stage never
				// plans a digestion the feeds cannot sustain.
				auto extentKg = std::min(energyFeasibleKg,
					massToKg(feed.authored.unit, drawableFeedUnits));
				if (processor.authored.oxygenPerKgFeedKg > 0.0)
					extentKg = std::min(extentKg,
						massToKg(oxygen.authored.unit, drawableOxygenUnits)
							/ processor.authored.oxygenPerKgFeedKg);
				if (processor.authored.processWaterPerKgFeedKg > 0.0)
					extentKg = std::min(extentKg,
						massToKg(water.authored.unit, drawableWaterUnits)
							/ processor.authored.processWaterPerKgFeedKg);
				if (processor.authored.consumablePerKgFeed > 0.0)
					extentKg = std::min(extentKg,
						drawableConsumableUnits / processor.authored.consumablePerKgFeed);
				// Move the flows, clamped per flow so roundoff never
				// overshoots (#556), and recompute the ledger from what
				// actually moved.
				auto const feedUnits = std::min(kgToMass(feed.authored.unit, extentKg), drawableFeedUnits);
				auto const feedActualKg = massToKg(feed.authored.unit, feedUnits);
				auto const oxygenUnits = std::min(kgToMass(oxygen.authored.unit,
					feedActualKg * processor.authored.oxygenPerKgFeedKg), drawableOxygenUnits);
				auto const oxygenActualKg = massToKg(oxygen.authored.unit, oxygenUnits);
				auto const waterUnits = std::min(kgToMass(water.authored.unit,
					feedActualKg * processor.authored.processWaterPerKgFeedKg), drawableWaterUnits);
				auto const waterActualKg = massToKg(water.authored.unit, waterUnits);
				auto const consumableUnits = std::min(
					feedActualKg * processor.authored.consumablePerKgFeed, drawableConsumableUnits);
				auto const nutrientUnits = std::min(kgToMass(nutrient.authored.unit,
					feedActualKg * nutrientPerKgFeed), nutrientHeadroomUnits);
				auto const nutrientActualKg = massToKg(nutrient.authored.unit, nutrientUnits);
				auto const recoveredWaterUnits = std::min(kgToMass(recoveredWater.authored.unit,
					feedActualKg * recoveredWaterPerKgFeed), recoveredWaterHeadroomUnits);
				auto const recoveredWaterActualKg = massToKg(recoveredWater.authored.unit,
					recoveredWaterUnits);
				auto const co2Units = std::min(kgToMass(co2.authored.unit,
					feedActualKg * processor.authored.co2PerKgFeedKg), co2HeadroomUnits);
				auto const co2ActualKg = massToKg(co2.authored.unit, co2Units);
				auto const residueUnits = std::min(kgToMass(residue.authored.unit,
					feedActualKg * residuePerKgFeed), residueHeadroomUnits);
				auto const residueActualKg = massToKg(residue.authored.unit, residueUnits);
				qualified[processor.feedStoreIndex] -= feedUnits;
				flowOut[processor.feedStoreIndex] += feedUnits;
				qualified[processor.oxygenStoreIndex] -= oxygenUnits;
				flowOut[processor.oxygenStoreIndex] += oxygenUnits;
				qualified[processor.waterStoreIndex] -= waterUnits;
				flowOut[processor.waterStoreIndex] += waterUnits;
				qualified[processor.consumableStoreIndex] -= consumableUnits;
				flowOut[processor.consumableStoreIndex] += consumableUnits;
				qualified[processor.nutrientStoreIndex] += nutrientUnits;
				flowIn[processor.nutrientStoreIndex] += nutrientUnits;
				qualified[processor.recoveredWaterStoreIndex] += recoveredWaterUnits;
				flowIn[processor.recoveredWaterStoreIndex] += recoveredWaterUnits;
				qualified[processor.co2StoreIndex] += co2Units;
				flowIn[processor.co2StoreIndex] += co2Units;
				qualified[processor.residueStoreIndex] += residueUnits;
				flowIn[processor.residueStoreIndex] += residueUnits;
				stagedUnitLoss[feed.authored.unit] += feedUnits;
				stagedUnitLoss[oxygen.authored.unit] += oxygenUnits;
				stagedUnitLoss[water.authored.unit] += waterUnits;
				stagedUnitLoss[consumable.authored.unit] += consumableUnits;
				stagedUnitLoss[nutrient.authored.unit] -= nutrientUnits;
				stagedUnitLoss[recoveredWater.authored.unit] -= recoveredWaterUnits;
				stagedUnitLoss[co2.authored.unit] -= co2Units;
				stagedUnitLoss[residue.authored.unit] -= residueUnits;
				// Constituent boundary: the compound feed's constituents
				// leave the compound-material world with the feed, and the
				// plain product stores carry none back. The recovered share
				// is tracked in the processor's own per-constituent ledger;
				// at the station boundary the whole feed constituent set is
				// booked as leaving the compound stores, never created and
				// never silently destroyed.
				ConstituentLedger netChange;
				addComposition(netChange, composition, feedActualKg, -1.0);
				if (consumable.authored.composition)
					addComposition(netChange, *consumable.authored.composition,
						massToKg(consumable.authored.unit, consumableUnits), -1.0);
				for (auto const& entry : netChange)
					stagedLoss[entry.first] -= entry.second;
				// Energy: the electrical dose plus the exothermic heat the
				// products do not retain leave through the sink, once and
				// nowhere else.
				auto const reactionHeatKJ = feedActualKg
					* processor.authored.reactionHeatKJPerKgFeed;
				auto const heatKJ = energySuppliedKJ + reactionHeatKJ;
				microbialHeatByReservoir[processor.sinkIndex] += heatKJ / tickSeconds;
				processor.lastReactionHeatKW = reactionHeatKJ / tickSeconds;
				// Per-constituent recovery: each recovered nutrient is the
				// feed's bound content times its recovery fraction, so the
				// elemental balance is visible per element and the
				// atmospheric nitrogen share never appears here.
				processor.nitrogenRecoveredKg += feedActualKg * dryPerKgFeed
					* composition.nutrientNitrogen * processor.authored.nitrogenRecovery;
				processor.phosphorusRecoveredKg += feedActualKg * dryPerKgFeed
					* composition.phosphorus * processor.authored.phosphorusRecovery;
				processor.potassiumRecoveredKg += feedActualKg * dryPerKgFeed
					* composition.potassium * processor.authored.potassiumRecovery;
				processor.mineralsRecoveredKg += feedActualKg * dryPerKgFeed
					* composition.otherMinerals * processor.authored.mineralsRecovery;
				processor.lastFeedConsumedKg = feedActualKg;
				processor.lastOxygenConsumedKg = oxygenActualKg;
				processor.lastWaterConsumedKg = waterActualKg;
				processor.lastConsumableRequested = scale * fullKg
					* processor.authored.consumablePerKgFeed;
				processor.lastConsumableUsed = consumableUnits;
				processor.lastConsumableUnmet = processor.lastConsumableRequested - consumableUnits;
				processor.lastNutrientProducedKg = nutrientActualKg;
				processor.lastRecoveredWaterKg = recoveredWaterActualKg;
				processor.lastCo2ProducedKg = co2ActualKg;
				processor.lastResidueProducedKg = residueActualKg;
				processor.lastUnmetProcessingKg = std::max(0.0, scale * fullKg - feedActualKg);
				processor.feedConsumedKg += feedActualKg;
				processor.oxygenConsumedKg += oxygenActualKg;
				processor.waterConsumedKg += waterActualKg;
				processor.consumablesUsed += consumableUnits;
				processor.nutrientProducedKg += nutrientActualKg;
				processor.recoveredWaterKg += recoveredWaterActualKg;
				processor.co2ProducedKg += co2ActualKg;
				processor.residueProducedKg += residueActualKg;
				processor.energySuppliedKJ += energySuppliedKJ;
				processor.reactionHeatKJ += reactionHeatKJ;
				processor.heatRoutedKJ += heatKJ;
				totalMicrobialProcessingPowerKW += suppliedKW;
				if (feedActualKg + 1e-12 >= fullKg
					&& suppliedKW + 1e-12 >= processor.lastPowerDemandKW)
					processor.status = StationTransferStatus::Full;
				else processor.status = StationTransferStatus::Partial;
			}
			// Second loop pass: integrate the loop's temperature from the
			// pump work, heater heat, exchanger exchange and the chiller
			// extraction that actually moved this tick, and book the loop
			// ledger and exposed conditions. The extraction leaves the loop
			// exactly once; the chiller books it as the cold side of its
			// own ledger and the sink gains it through the condenser term,
			// so the heat is never double-counted.
			for (size_t loopIndex = 0; loopIndex < coolantLoops.size(); ++loopIndex)
			{
				auto& loop = coolantLoops[loopIndex];
				auto const disabled = loop.coolantKg <= 0.0;
				auto const extractionKW = chillerExtractionByLoop[loopIndex];
				loop.lastChillerExtractionKW = extractionKW;
				auto const collectorHeatKW = collectorHeatByLoop[loopIndex];
				loop.lastCollectorHeatKW = collectorHeatKW;
				auto const loopCapacityKJ = loop.coolantKg * loop.authored.specificHeatKJPerKgK;
				if (!disabled)
				{
					auto const heatIntoLoopKW = loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW;
					loop.temperatureC += (heatIntoLoopKW + loop.lastExchangeIntoLoopKW
						+ collectorHeatKW - extractionKW)
						* static_cast<double>(seconds) / loopCapacityKJ;
					loop.pumpWorkKJ += loop.lastPumpSuppliedKW * static_cast<double>(seconds);
					loop.heaterKJ += loop.lastHeaterSuppliedKW * static_cast<double>(seconds);
					loop.exchangedKJ += loop.lastExchangeIntoLoopKW * static_cast<double>(seconds);
					loop.extractedKJ += extractionKW * static_cast<double>(seconds);
					loop.collectorHeatKJ += collectorHeatKW * static_cast<double>(seconds);
				}
				// The freeze condition is exposed, never clamped: the loop
				// stays integrated and the shortfall stays visible.
				loop.lastBelowFreeze = !disabled
					&& loop.temperatureC < loop.authored.freezeProtectionSetpointC - 1e-9;
				loop.lastCirculating = loop.lastFlowKgPerS > 1e-12;
				if (disabled || (loop.lastPumpDemandKW <= 0.0 && loop.lastHeaterDemandKW <= 0.0))
					loop.status = StationTransferStatus::Idle;
				else if (loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW <= 1e-12)
					loop.status = StationTransferStatus::Rejected;
				else if (loop.lastPumpSuppliedKW + 1e-12 >= loop.lastPumpDemandKW
					&& loop.lastHeaterSuppliedKW + 1e-12 >= loop.lastHeaterDemandKW)
					loop.status = StationTransferStatus::Full;
				else loop.status = StationTransferStatus::Partial;
			}
			// The loop pump and heater draws rode the shared generation-and-
			// battery pool: their battery usage landed in the same per-battery
			// discharge fields the load passes used and their generation use
			// came out of the same availableGeneration pool, so the
			// single-pool audit downstream sees every watt.
			// Compartment atmospheres run after the coolant loops and
			// chillers and before thermal integration (#400): the
			// compartment's exchange with its coupled reservoir must land
			// inside this tick's reservoir integration, and the compartment
			// integrates against the reservoir temperature as it stands -
			// the same explicit-Euler discipline as the loops. Scheduled
			// airlock cycles run first against the tick-start inventory,
			// in the shared scenario schedule order (occurrence time, then
			// priority descending, then declaration order), so coincident
			// cycles from different schedules never depend on an iteration
			// accident and each occurrence fires exactly once however many
			// ticks an accelerated update batches together.
			struct DueAirlockCycle
			{
				StationScenarioEventKey key;
				uint64_t occurrence{0};
			};
			std::vector<DueAirlockCycle> airlockDue;
			for (size_t index = 0; index < airlockCycles.size(); ++index)
			{
				auto const& cycle = airlockCycles[index];
				if (cycle.exhausted) continue;
				auto const due = stationScheduleOccurrencesDue(
					cycle.authored.firstOccurrenceSeconds,
					cycle.authored.recurrenceSeconds, elapsed);
				for (uint64_t occurrence = cycle.occurrencesFired; occurrence < due; ++occurrence)
				{
					auto const when = cycle.authored.recurrenceSeconds > 0.0
						? cycle.authored.firstOccurrenceSeconds
							+ static_cast<double>(occurrence) * cycle.authored.recurrenceSeconds
						: cycle.authored.firstOccurrenceSeconds;
					airlockDue.push_back({{when, cycle.authored.priority, index}, occurrence});
				}
			}
			std::sort(airlockDue.begin(), airlockDue.end(),
				[](DueAirlockCycle const& left, DueAirlockCycle const& right)
				{ return left.key < right.key; });
			for (auto const& due : airlockDue)
			{
				auto& cycle = airlockCycles[due.key.declarationIndex];
				auto& compartment = atmospheres[cycle.atmosphereIndex];
				auto const totalMassKg = compartment.oxygenKg + compartment.nitrogenKg
					+ compartment.carbonDioxideKg + compartment.waterVapourKg;
				// The cycle can only remove the gas the compartment
				// actually holds: the removal is capped by the current
				// total mass and split composition-proportionally, so a
				// cycle never invents mass and never picks a favourite
				// species. What it removes is booked whole: the recovered
				// fraction into the recovery receiver, the remainder as
				// lost through the outer hatch.
				auto const removedKg = std::min(cycle.authored.gasMassKg, totalMassKg);
				auto const removedOxygenKg = totalMassKg > 0.0
					? removedKg * compartment.oxygenKg / totalMassKg : 0.0;
				auto const removedNitrogenKg = totalMassKg > 0.0
					? removedKg * compartment.nitrogenKg / totalMassKg : 0.0;
				auto const removedCarbonDioxideKg = totalMassKg > 0.0
					? removedKg * compartment.carbonDioxideKg / totalMassKg : 0.0;
				auto const removedWaterVapourKg = totalMassKg > 0.0
					? removedKg * compartment.waterVapourKg / totalMassKg : 0.0;
				auto const recoveredKg = removedKg * cycle.authored.recoveredFraction;
				auto const lostKg = removedKg - recoveredKg;
				compartment.oxygenKg -= removedOxygenKg;
				compartment.nitrogenKg -= removedNitrogenKg;
				compartment.carbonDioxideKg -= removedCarbonDioxideKg;
				compartment.waterVapourKg -= removedWaterVapourKg;
				compartment.cycleOxygenKg += removedOxygenKg;
				compartment.cycleNitrogenKg += removedNitrogenKg;
				compartment.cycleCarbonDioxideKg += removedCarbonDioxideKg;
				compartment.cycleWaterVapourKg += removedWaterVapourKg;
				compartment.recoveredKg += recoveredKg;
				compartment.lostKg += lostKg;
				compartment.lastCycleRemovedKg += removedKg;
				compartment.lastCycleRecoveredKg += recoveredKg;
				compartment.lastCycleLostKg += lostKg;
				cycle.occurrencesFired = due.occurrence + 1;
				cycle.lastFiredTick = tick;
				cycle.lastOccurrenceSeconds = due.key.occurrenceSeconds;
				cycle.lastRemovedKg = removedKg;
				cycle.lastRecoveredKg = recoveredKg;
				cycle.lastLostKg = lostKg;
				cycle.removedTotal += removedKg;
				cycle.recoveredTotal += recoveredKg;
				cycle.lostTotal += lostKg;
				cycle.status = removedKg <= 0.0 ? StationTransferStatus::Rejected
					: (removedKg + StationBalanceTolerance >= cycle.authored.gasMassKg
						? StationTransferStatus::Full : StationTransferStatus::Partial);
				if (cycle.authored.recurrenceSeconds <= 0.0) cycle.exhausted = true;
			}
			// Crew metabolism (#401) runs after the scheduled airlock
			// cycles and before the compartments integrate: the crew draws
			// its oxygen from the compartment inventory as it stands after
			// the cycles, and its carbon dioxide, moisture and metabolic
			// heat land before the compartment's coupled vapour/thermal
			// solve, so the crew's returns are subject to the same
			// saturation and thermal integration as every other term in
			// the volume. One uniform metabolic scale per crew per tick:
			// the smallest of the input-supply fractions (food and water
			// drawable above the protected reserve, the oxygen actually
			// present) and the output-headroom fractions (urine and
			// organic-waste store capacity). Every pathway - consumption
			// and return alike - scales by that one number, so a crew
			// with absent material consumes nothing and returns nothing:
			// no nominal waste from material that is not there, and a
			// backed-up waste tank throttles consumption together with
			// its returns instead of letting material run away from a
			// destination that cannot hold it. The requested-versus-actual
			// remainder is explicit per pathway; unmet demand is a
			// reported result, never a silent clamp. Crew inputs are the
			// authored stores and the authored compartment only: no World
			// Agent is read, mutated, spawned or moved by this process.
			std::vector<double> crewHeatByAtmosphere(atmospheres.size(), 0.0);
			{
				auto const dt = static_cast<double>(seconds);
				for (auto& crew : crews)
				{
					auto& compartment = atmospheres[crew.atmosphereIndex];
					auto const activity = evaluateStationProfile(crew.authored.activityProfile, elapsed);
					auto const crewSeconds = static_cast<double>(crew.authored.crewCount) * activity * dt;
					auto const foodRequested = crew.authored.foodPerCrewPerS * crewSeconds;
					auto const waterRequested = crew.authored.waterPerCrewPerS * crewSeconds;
					auto const oxygenRequestedKg = crew.authored.oxygenPerCrewKgPerS * crewSeconds;
					auto const co2RequestedKg = crew.authored.co2PerCrewKgPerS * crewSeconds;
					auto const moistureRequestedKg = crew.authored.moisturePerCrewKgPerS * crewSeconds;
					auto const urineRequested = crew.authored.urinePerCrewPerS * crewSeconds;
					auto const wasteRequested = crew.authored.organicWastePerCrewPerS * crewSeconds;
					auto const heatFullKW = crew.authored.metabolicHeatPerCrewKW
						* static_cast<double>(crew.authored.crewCount) * activity;
					// #553: finite authored values multiplied by a long
					// crew-second can still overflow; refuse the tick rather
					// than publish an infinite demand.
					if (!std::isfinite(crewSeconds) || !std::isfinite(foodRequested)
						|| !std::isfinite(waterRequested) || !std::isfinite(oxygenRequestedKg)
						|| !std::isfinite(co2RequestedKg) || !std::isfinite(moistureRequestedKg)
						|| !std::isfinite(urineRequested) || !std::isfinite(wasteRequested)
						|| !std::isfinite(heatFullKW))
					{
						system = "Crew metabolism";
						return violation = "crew '" + crew.authored.id
							+ "' demand overflows to a non-finite magnitude", false;
					}
					double scale = 1.0;
					std::string limiter;
					auto const capSupply = [&](size_t storeIndex, double requested, char const* label)
					{
						if (requested <= 0.0) return;
						auto const drawable = std::max(0.0, qualified[storeIndex]
							- stores[storeIndex].authored.protectedReserve);
						auto const fraction = drawable / requested;
						if (fraction < scale)
						{
							scale = fraction;
							limiter = std::string(label) + " store '" + stores[storeIndex].authored.id
								+ (stores[storeIndex].authored.protectedReserve > 0.0
									? "' usable inventory is at its protected reserve"
									: "' usable inventory is empty");
						}
					};
					auto const capHeadroom = [&](size_t storeIndex, double produced, char const* label)
					{
						if (produced <= 0.0) return;
						auto const headroom = stores[storeIndex].authored.capacity
							- (qualified[storeIndex] + unqualified[storeIndex]);
						auto const fraction = headroom / produced;
						if (fraction < scale)
						{
							scale = fraction;
							limiter = std::string(label) + " store '" + stores[storeIndex].authored.id
								+ "' is at capacity";
						}
					};
					capSupply(crew.foodStoreIndex, foodRequested, "food");
					capSupply(crew.waterStoreIndex, waterRequested, "water");
					if (oxygenRequestedKg > 0.0)
					{
						auto const fraction = std::max(0.0, compartment.oxygenKg) / oxygenRequestedKg;
						if (fraction < scale)
						{
							scale = fraction;
							limiter = "compartment '" + compartment.authored.id
								+ "' oxygen inventory cannot supply the draw";
						}
					}
					capHeadroom(crew.urineStoreIndex, urineRequested, "urine");
					capHeadroom(crew.organicWasteStoreIndex, wasteRequested, "organic waste");
					if (!(scale > 0.0)) scale = 0.0;
					if (scale > 1.0) scale = 1.0;
					// #556: apply the binding scale per pathway with a min
					// against that pathway's own drawable or headroom, so
					// roundoff never draws below zero or pushes over capacity.
					auto const drawCapped = [&](size_t storeIndex, double requested)
					{
						auto const drawable = std::max(0.0, qualified[storeIndex]
							- stores[storeIndex].authored.protectedReserve);
						auto const completed = std::min(requested * scale, drawable);
						qualified[storeIndex] -= completed;
						flowOut[storeIndex] += completed;
						return completed;
					};
					auto const deliverCapped = [&](size_t storeIndex, double requested)
					{
						auto const headroom = stores[storeIndex].authored.capacity
							- (qualified[storeIndex] + unqualified[storeIndex]);
						auto const completed = std::min(requested * scale, headroom);
						qualified[storeIndex] += completed;
						flowIn[storeIndex] += completed;
						return completed;
					};
					auto const foodActual = drawCapped(crew.foodStoreIndex, foodRequested);
					auto const waterActual = drawCapped(crew.waterStoreIndex, waterRequested);
					auto const oxygenActualKg = std::min(oxygenRequestedKg * scale,
						std::max(0.0, compartment.oxygenKg));
					compartment.oxygenKg -= oxygenActualKg;
					compartment.oxygenConsumedKg += oxygenActualKg;
					// The returns scale with the metabolism that actually
					// ran, never with the nominal demand: CO2, moisture
					// and heat all carry the same scale the inputs funded.
					auto const co2ActualKg = co2RequestedKg * scale;
					compartment.carbonDioxideKg += co2ActualKg;
					compartment.co2ProducedKg += co2ActualKg;
					auto const moistureActualKg = moistureRequestedKg * scale;
					compartment.waterVapourKg += moistureActualKg;
					compartment.vapourAddedKg += moistureActualKg;
					auto const urineActual = deliverCapped(crew.urineStoreIndex, urineRequested);
					auto const wasteActual = deliverCapped(crew.organicWasteStoreIndex, wasteRequested);
					auto const heatKW = heatFullKW * scale;
					crewHeatByAtmosphere[crew.atmosphereIndex] += heatKW;
					// Store-boundary ledgers: consumption is a loss and the
					// waste returns are negative loss, exactly like
					// conversion flows, so the per-unit boundary closes;
					// the constituent difference is what the returns did
					// not carry away and stays accounted, never silently
					// dropped.
					stagedUnitLoss[stores[crew.foodStoreIndex].authored.unit] += foodActual;
					stagedUnitLoss[stores[crew.waterStoreIndex].authored.unit] += waterActual;
					stagedUnitLoss[stores[crew.urineStoreIndex].authored.unit] -= urineActual;
					stagedUnitLoss[stores[crew.organicWasteStoreIndex].authored.unit] -= wasteActual;
					ConstituentLedger netChange;
					addComposition(netChange, *stores[crew.foodStoreIndex].authored.composition,
						massToKg(stores[crew.foodStoreIndex].authored.unit, foodActual), -1.0);
					addComposition(netChange, *stores[crew.waterStoreIndex].authored.composition,
						massToKg(stores[crew.waterStoreIndex].authored.unit, waterActual), -1.0);
					addComposition(netChange, *stores[crew.urineStoreIndex].authored.composition,
						massToKg(stores[crew.urineStoreIndex].authored.unit, urineActual), 1.0);
					addComposition(netChange, *stores[crew.organicWasteStoreIndex].authored.composition,
						massToKg(stores[crew.organicWasteStoreIndex].authored.unit, wasteActual), 1.0);
					for (auto const& entry : netChange)
						stagedLoss[entry.first] -= entry.second;
					// Cumulative pathway ledger and the explicit retained
					// body mass: consumed minus returned, reported rather
					// than discarded.
					auto const retainedTickKg = massToKg(stores[crew.foodStoreIndex].authored.unit, foodActual)
						+ massToKg(stores[crew.waterStoreIndex].authored.unit, waterActual)
						+ oxygenActualKg - co2ActualKg - moistureActualKg
						- massToKg(stores[crew.urineStoreIndex].authored.unit, urineActual)
						- massToKg(stores[crew.organicWasteStoreIndex].authored.unit, wasteActual);
					crew.lastActivityLevel = activity;
					crew.lastScale = scale;
					crew.lastLimiter = scale >= 1.0 - 1e-12 ? std::string() : limiter;
					crew.lastFoodRequested = foodRequested;
					crew.lastFoodActual = foodActual;
					crew.lastWaterRequested = waterRequested;
					crew.lastWaterActual = waterActual;
					crew.lastOxygenRequestedKg = oxygenRequestedKg;
					crew.lastOxygenActualKg = oxygenActualKg;
					crew.lastCo2ProducedKg = co2ActualKg;
					crew.lastMoistureProducedKg = moistureActualKg;
					crew.lastUrineRequested = urineRequested;
					crew.lastUrineActual = urineActual;
					crew.lastOrganicWasteRequested = wasteRequested;
					crew.lastOrganicWasteActual = wasteActual;
					crew.lastHeatKW = heatKW;
					crew.foodConsumed += foodActual;
					crew.waterConsumed += waterActual;
					crew.oxygenConsumedKg += oxygenActualKg;
					crew.co2ProducedKg += co2ActualKg;
					crew.moistureProducedKg += moistureActualKg;
					crew.urineProduced += urineActual;
					crew.organicWasteProduced += wasteActual;
					crew.heatKJ += heatKW * dt;
					crew.retainedKg += retainedTickKg;
					crew.lastUnmet = scale < 1.0 - 1e-12;
					if (crew.authored.crewCount <= 0)
						crew.status = StationTransferStatus::Idle;
					else if (scale >= 1.0 - 1e-12) crew.status = StationTransferStatus::Full;
					else if (scale <= 1e-12) crew.status = StationTransferStatus::Rejected;
					else crew.status = StationTransferStatus::Partial;
				}
			}
			// Then each compartment: occupancy draw, leakage, the
			// coupled vapour-phase/thermal integration and the derived
			// pressures, in that documented order.
			for (size_t atmosphereIndex = 0; atmosphereIndex < atmospheres.size(); ++atmosphereIndex)
			{
				auto& compartment = atmospheres[atmosphereIndex];
				auto const dt = static_cast<double>(seconds);
				auto const occupants = static_cast<double>(compartment.authored.occupants);
				// Occupancy is an authored load assignment, never derived
				// from World entities in this feature. The oxygen draw is
				// capped by the oxygen actually present, and CO2 production
				// plus metabolic heat scale with the fraction of the demand
				// actually consumed - a starved compartment cannot breathe
				// out more than it breathed in.
				auto const oxygenDemandKg = occupants
					* compartment.authored.oxygenUsePerOccupantKgPerS * dt;
				auto const oxygenConsumedKg = std::min(oxygenDemandKg, compartment.oxygenKg);
				auto const metabolicFraction = oxygenDemandKg > 0.0
					? oxygenConsumedKg / oxygenDemandKg : 1.0;
				auto const co2ProducedKg = occupants
					* compartment.authored.co2ProductionPerOccupantKgPerS * dt * metabolicFraction;
				auto const vapourAddedKg = occupants
					* compartment.authored.moistureAdditionPerOccupantKgPerS * dt;
				compartment.oxygenKg -= oxygenConsumedKg;
				compartment.oxygenConsumedKg += oxygenConsumedKg;
				compartment.carbonDioxideKg += co2ProducedKg;
				compartment.co2ProducedKg += co2ProducedKg;
				compartment.waterVapourKg += vapourAddedKg;
				compartment.vapourAddedKg += vapourAddedKg;
				// Leakage: a constant authored total-mass rate capped by
				// what the compartment holds, split composition-
				// proportionally. The leaked mass leaves the station's
				// account as explicit leaked material - it is never routed
				// into another compartment, and it is never unexplained.
				auto const totalMassKg = compartment.oxygenKg + compartment.nitrogenKg
					+ compartment.carbonDioxideKg + compartment.waterVapourKg;
				auto const leakedKg = std::min(compartment.authored.leakRateKgPerS * dt, totalMassKg);
				if (leakedKg > 0.0 && totalMassKg > 0.0)
				{
					auto const leakedOxygenKg = leakedKg * compartment.oxygenKg / totalMassKg;
					auto const leakedNitrogenKg = leakedKg * compartment.nitrogenKg / totalMassKg;
					auto const leakedCarbonDioxideKg = leakedKg * compartment.carbonDioxideKg / totalMassKg;
					auto const leakedWaterVapourKg = leakedKg * compartment.waterVapourKg / totalMassKg;
					compartment.oxygenKg -= leakedOxygenKg;
					compartment.nitrogenKg -= leakedNitrogenKg;
					compartment.carbonDioxideKg -= leakedCarbonDioxideKg;
					compartment.waterVapourKg -= leakedWaterVapourKg;
					compartment.leakOxygenKg += leakedOxygenKg;
					compartment.leakNitrogenKg += leakedNitrogenKg;
					compartment.leakCarbonDioxideKg += leakedCarbonDioxideKg;
					compartment.leakWaterVapourKg += leakedWaterVapourKg;
				}
				compartment.leakedKg += leakedKg;
				// Thermal and phase state integrated together (#564):
				// occupant sensible heat, the heat-only reservoir exchange
				// and the saturation constraint on the water vapour, whose
				// condensation latent heat feeds the very temperature that
				// sets the cap, so the end-of-tick gas never publishes
				// supersaturated against its own state. The exchange came
				// from the reservoir's implicit star solve above, which
				// already negated it onto the reservoir, so it is only
				// read here (#559). The condensate leaves the gas phase
				// and is reported, never destroyed; it does not
				// re-evaporate in this slice.
				auto const occupantHeatKW = occupants
					* compartment.authored.sensibleHeatPerOccupantKW * metabolicFraction;
				// Crew metabolic heat (#401): routed into this compartment
				// by the crews assigned to it, booked as its own thermal
				// term beside the compartment's own occupancy heat and
				// entering the same coupled vapour/thermal solve.
				auto const crewHeatKW = crewHeatByAtmosphere[atmosphereIndex];
				auto const exchangeKW = atmosphereExchangeKJ[atmosphereIndex] / dt;
				auto const coupled = solveVapourThermalCoupling(
					compartment.authored.thermalCapacityKJPerK, compartment.temperatureC,
					occupantHeatKW + crewHeatKW + exchangeKW, compartment.waterVapourKg,
					compartment.authored.volumeM3, dt);
				auto const condensedKg = coupled.condensedKg;
				auto const latentHeatKW = condensedKg
					* StationLatentHeatVaporizationKJPerKg / dt;
				compartment.waterVapourKg -= condensedKg;
				compartment.condensedKg += condensedKg;
				compartment.temperatureC = coupled.temperatureC;
				compartment.occupantHeatKJ += occupantHeatKW * dt;
				compartment.crewHeatKJ += crewHeatKW * dt;
				compartment.lastCrewHeatKW = crewHeatKW;
				compartment.latentHeatKJ += latentHeatKW * dt;
				compartment.exchangedKJ += exchangeKW * dt;
				// Publish last-tick observables.
				compartment.lastOxygenDemandKg = oxygenDemandKg;
				compartment.lastOxygenConsumedKg = oxygenConsumedKg;
				compartment.lastCo2ProducedKg = co2ProducedKg;
				compartment.lastVapourAddedKg = vapourAddedKg;
				compartment.lastVapourCondensedKg = condensedKg;
				compartment.lastLeakedKg = leakedKg;
				compartment.lastOccupantHeatKW = occupantHeatKW;
				compartment.lastLatentHeatKW = latentHeatKW;
				compartment.lastExchangeIntoCompartmentKW = exchangeKW;
				// Conditions are reported, never clamped: a pressure
				// setpoint alone never proves containment - the closing
				// ledgers and these exposed crossings do.
				auto const oxygenKPa = stationAtmospherePartialPressureKPa(compartment.oxygenKg,
					StationMolarMassOxygenKgPerMol, compartment.authored.volumeM3,
					compartment.temperatureC);
				auto const co2KPa = stationAtmospherePartialPressureKPa(compartment.carbonDioxideKg,
					StationMolarMassCarbonDioxideKgPerMol, compartment.authored.volumeM3,
					compartment.temperatureC);
				auto const vapourKPa = stationAtmospherePartialPressureKPa(compartment.waterVapourKg,
					StationMolarMassWaterKgPerMol, compartment.authored.volumeM3,
					compartment.temperatureC);
				auto const totalKPa = oxygenKPa
					+ stationAtmospherePartialPressureKPa(compartment.nitrogenKg,
						StationMolarMassNitrogenKgPerMol, compartment.authored.volumeM3,
						compartment.temperatureC)
					+ co2KPa + vapourKPa;
				compartment.lastAbovePressure = totalKPa
					> compartment.authored.maxPressureKPa + 1e-9;
				compartment.lastBelowPressure = totalKPa
					< compartment.authored.minPressureKPa - 1e-9;
				compartment.lastLowOxygen = compartment.authored.minOxygenPartialPressureKPa > 0.0
					&& oxygenKPa < compartment.authored.minOxygenPartialPressureKPa - 1e-9;
				compartment.lastHighCo2 = compartment.authored.maxCarbonDioxidePartialPressureKPa > 0.0
					&& co2KPa > compartment.authored.maxCarbonDioxidePartialPressureKPa + 1e-9;
				compartment.lastCondensing = condensedKg > 1e-12;
				if (oxygenDemandKg > 0.0)
					compartment.status = oxygenConsumedKg + 1e-12 >= oxygenDemandKg
						? StationTransferStatus::Full
						: (oxygenConsumedKg <= 1e-12
							? StationTransferStatus::Rejected : StationTransferStatus::Partial);
				else compartment.status = StationTransferStatus::Idle;
			}
			// Thermal reservoirs accumulate the heat the station actually
			// produced this tick (#395): each reservoir's authored
			// environmental absorption plus the equipment heat routed into
			// it, against radiator rejection that follows the emitting
			// temperature and the authored sink. Explicit Euler on the
			// fixed World tick: the same tick sequence always integrates
			// the same state, and the energy ledger closes over exactly
			// the terms that moved.
			{
				auto const dt = static_cast<double>(seconds);
				std::vector<double> routedIn(reservoirs.size(), 0.0);
				for (auto const& load : loads)
					if (load.hasHeatSink) routedIn[load.heatSinkIndex] += load.lastHeatRoutedKW;
				for (size_t index = 0; index < reservoirs.size(); ++index)
				{
					auto& reservoir = reservoirs[index];
					auto const capacity = reservoir.authored.heatCapacityKJPerK;
					auto const temperatureK = reservoir.temperatureC + StationCelsiusOffset;
					// Generator heat (#399): waste, startup dissipation and
					// decay heat routed into the sink exactly once, at the
					// same boundary the reservoir integrates.
					auto const generatorHeatKW = generatorHeatBySink[index];
					// Compartment exchange (#400): the reservoir side of the
					// compartment's heat-only exchanger, the exact negation
					// of what the compartment gained or gave up this tick.
					auto const atmosphereHeatKW = atmosphereHeatByReservoir[index];
					auto const heatInKW = reservoir.authored.absorbedHeatKW + routedIn[index]
						+ loopHeatIntoReservoir[index] + condenserHeatBySink[index]
						+ generatorHeatKW + atmosphereHeatKW
						+ electrolysisHeatByReservoir[index]
						+ fuelCellHeatByReservoir[index]
						+ sabatierHeatByReservoir[index]
						+ methaneHeatByReservoir[index]
						+ microbialHeatByReservoir[index];
					// Net radiative rejection at the current emitting
					// temperature: emissivity x view factor x area x sigma
					// x (T^4 - T_sink^4). The environmental back-radiation
					// term is explicit, so a warmer sink rejects less, and
					// the radiator/view assumption is the declared
					// emissivity-times-view-factor product. No distribution
					// modelling is promised beyond these declared factors.
					std::vector<double> rawRejections;
					rawRejections.reserve(reservoir.radiatorIndices.size());
					double rejectedKW = 0.0;
					double minSinkC = reservoir.temperatureC;
					double maxSinkC = reservoir.temperatureC;
					for (auto const radiatorIndex : reservoir.radiatorIndices)
					{
						auto const& radiator = radiators[radiatorIndex];
						auto const sinkK = radiator.authored.sinkTemperatureC + StationCelsiusOffset;
						auto const rejection = radiator.authored.emissivity
							* radiator.authored.viewFactor * radiator.authored.areaM2
							* StationStefanBoltzmannKWPerM2K4
							* (temperatureK * temperatureK * temperatureK * temperatureK
								- sinkK * sinkK * sinkK * sinkK);
						rawRejections.push_back(rejection);
						rejectedKW += rejection;
						minSinkC = std::min(minSinkC, radiator.authored.sinkTemperatureC);
						maxSinkC = std::max(maxSinkC, radiator.authored.sinkTemperatureC);
					}
					// Finite physical floor: a body cannot radiatively cool
					// below its coldest sink nor warm above its warmest one.
					// The clamp is booked, not hidden: the ledger records
					// the rejection that actually moved, scaled across the
					// radiators that produced it.
					double scale = 1.0;
					if (!reservoir.radiatorIndices.empty())
					{
						if (rejectedKW > 0.0)
						{
							auto const cap = (reservoir.temperatureC - minSinkC) * capacity / dt;
							if (rejectedKW > cap) scale = cap / rejectedKW;
						}
						else if (rejectedKW < 0.0)
						{
							auto const floor = (reservoir.temperatureC - maxSinkC) * capacity / dt;
							if (rejectedKW < floor) scale = floor / rejectedKW;
						}
					}
					for (size_t r = 0; r < reservoir.radiatorIndices.size(); ++r)
						radiators[reservoir.radiatorIndices[r]].lastRejectedKW = rawRejections[r] * scale;
					rejectedKW *= scale;
					// Phase-change buffers ride through the deficit (#398):
					// the running imbalance after rejection is what the
					// buffers hosted on this reservoir see. A charge moves
					// heat from the reservoir into the buffer's finite store,
					// capped by the remaining deficit, the configured rate
					// limit, the store's headroom and the thermodynamic
					// direction (the reservoir must be hotter than the
					// buffer). A discharge returns stored heat into the
					// reservoir, capped by the remaining rejection surplus,
					// the rate limit, the stored energy and the direction.
					// Recovery therefore consumes real rejection capacity:
					// the store resets only as fast as the rejection path can
					// carry the heat away, never instantly. A circulation-
					// access buffer is unreachable while its gating loop is
					// not circulating, so a blackout cannot absorb through a
					// powered-only store for free. Buffers run in declaration
					// order and the running balance carries through them, so
					// two buffers on one host never absorb the same deficit
					// twice. The arithmetic mirrors the sandbox expression
					// for expression so host and Lua integrate identically.
					auto remainingKW = heatInKW - rejectedKW;
					auto bufferChargeKW = 0.0;
					auto bufferDischargeKW = 0.0;
					for (auto const bufferIndex : reservoir.bufferIndices)
					{
						auto& buffer = thermalBuffers[bufferIndex];
						auto const band = buffer.authored.maxUsableTemperatureC
							- buffer.authored.minUsableTemperatureC;
						auto const bufferTemperatureC = buffer.authored.minUsableTemperatureC
							+ buffer.storedKJ * band / buffer.authored.capacityKJ;
						auto const imbalanceKW = remainingKW > 0.0 ? remainingKW : -remainingKW;
						auto chargeKW = 0.0;
						auto dischargeKW = 0.0;
						std::string limiter;
						auto const accessible = buffer.authored.accessMode != "circulation"
							|| coolantLoops[buffer.gateLoopIndex].lastCirculating;
						if (!accessible)
							limiter = "circulation lost: the powered buffer is unreachable";
						else if (remainingKW > 0.0)
						{
							if (reservoir.temperatureC > bufferTemperatureC)
							{
								auto limitKW = buffer.authored.maxChargeKW;
								if (buffer.authored.accessMode == "passive")
									limitKW = std::min(limitKW,
										buffer.authored.transferConductanceKWPerK
											* (reservoir.temperatureC - bufferTemperatureC));
								auto const headroomKW = (buffer.authored.capacityKJ - buffer.storedKJ) / dt;
								chargeKW = std::min({limitKW, remainingKW, headroomKW});
								if (chargeKW <= 0.0)
									limiter = "buffer is full: no headroom left to absorb";
								else if (chargeKW < remainingKW)
									limiter = headroomKW <= chargeKW + 1e-12
										? "buffer headroom: the store is filling up"
										: "buffer charge rate limit";
							}
							else
								limiter = "buffer is not colder than the reservoir: no charge direction";
						}
						else if (remainingKW < 0.0)
						{
							if (bufferTemperatureC > reservoir.temperatureC)
							{
								auto limitKW = buffer.authored.maxDischargeKW;
								if (buffer.authored.accessMode == "passive")
									limitKW = std::min(limitKW,
										buffer.authored.transferConductanceKWPerK
											* (bufferTemperatureC - reservoir.temperatureC));
								auto const drawableKW = buffer.storedKJ / dt;
								dischargeKW = std::min({limitKW, -remainingKW, drawableKW});
								if (dischargeKW <= 0.0)
									limiter = "buffer is depleted: nothing left to return";
								else if (dischargeKW < -remainingKW)
									limiter = drawableKW <= dischargeKW + 1e-12
										? "buffer store: nearly nothing left to return"
										: "buffer discharge rate limit";
							}
							else
								limiter = "buffer is not hotter than the reservoir: no discharge direction";
						}
						buffer.storedKJ += (chargeKW - dischargeKW) * dt;
						// Roundoff never leaves the finite bounds: the store is
						// clamped to [0, capacity] so a one-ulp overshoot
						// cannot accumulate (#556 discipline).
						if (buffer.storedKJ < 0.0) buffer.storedKJ = 0.0;
						if (buffer.storedKJ > buffer.authored.capacityKJ)
							buffer.storedKJ = buffer.authored.capacityKJ;
						buffer.chargedKJ += chargeKW * dt;
						buffer.dischargedKJ += dischargeKW * dt;
						buffer.lastAccessible = accessible;
						buffer.lastChargeKW = chargeKW;
						buffer.lastDischargeKW = dischargeKW;
						buffer.lastFull = buffer.storedKJ >= buffer.authored.capacityKJ - 1e-9;
						buffer.lastDepleted = buffer.storedKJ <= 1e-9;
						buffer.lastLimiter = limiter;
						auto const movedKW = chargeKW + dischargeKW;
						if (imbalanceKW <= 0.0) buffer.status = StationTransferStatus::Idle;
						else if (!accessible) buffer.status = StationTransferStatus::Rejected;
						else if (movedKW + 1e-12 >= imbalanceKW) buffer.status = StationTransferStatus::Full;
						else buffer.status = StationTransferStatus::Partial;
						remainingKW = remainingKW - chargeKW + dischargeKW;
						bufferChargeKW += chargeKW;
						bufferDischargeKW += dischargeKW;
					}
					auto const netKW = remainingKW;
					auto const newTemperatureC = reservoir.temperatureC + netKW * dt / capacity;
					reservoir.lastHeatRoutedKW = routedIn[index];
					reservoir.lastBufferChargeKW = bufferChargeKW;
					reservoir.lastBufferDischargeKW = bufferDischargeKW;
					reservoir.lastLoopHeatKW = loopHeatIntoReservoir[index];
					reservoir.lastCondenserHeatKW = condenserHeatBySink[index];
					reservoir.lastGeneratorHeatKW = generatorHeatKW;
					reservoir.lastElectrolysisHeatKW = electrolysisHeatByReservoir[index];
					reservoir.lastFuelCellHeatKW = fuelCellHeatByReservoir[index];
					reservoir.lastSabatierHeatKW = sabatierHeatByReservoir[index];
					reservoir.lastMethaneProcessingHeatKW = methaneHeatByReservoir[index];
					reservoir.lastMicrobialHeatKW = microbialHeatByReservoir[index];
					reservoir.lastAtmosphereHeatKW = atmosphereHeatKW;
					reservoir.lastHeatInKW = heatInKW;
					reservoir.lastRejectedKW = rejectedKW;
					reservoir.lastNetKW = netKW;
					reservoir.absorbedKJ += reservoir.authored.absorbedHeatKW * dt;
					reservoir.routedKJ += routedIn[index] * dt;
					reservoir.loopKJ += loopHeatIntoReservoir[index] * dt;
					reservoir.condenserKJ += condenserHeatBySink[index] * dt;
					reservoir.generatorKJ += generatorHeatKW * dt;
					reservoir.electrolysisKJ += electrolysisHeatByReservoir[index] * dt;
					reservoir.fuelCellKJ += fuelCellHeatByReservoir[index] * dt;
					reservoir.sabatierKJ += sabatierHeatByReservoir[index] * dt;
					reservoir.methaneProcessingKJ += methaneHeatByReservoir[index] * dt;
					reservoir.microbialKJ += microbialHeatByReservoir[index] * dt;
					reservoir.atmosphereKJ += atmosphereHeatKW * dt;
					reservoir.bufferChargeKJ += bufferChargeKW * dt;
					reservoir.bufferDischargeKJ += bufferDischargeKW * dt;
					reservoir.rejectedKJ += rejectedKW * dt;
					reservoir.temperatureC = newTemperatureC;
					// Declared service limits are reported as crossings,
					// never enforced by clamping the integrated state.
					reservoir.lastAboveLimit = newTemperatureC
						> reservoir.authored.maxTemperatureC + 1e-9;
					reservoir.lastBelowLimit = newTemperatureC
						< reservoir.authored.minTemperatureC - 1e-9;
				}
			}
			// Charge from the surplus left after every allocation: batteries
			// take only generation no load took, within the charge power
			// limit, the headroom below nameplate and the charge efficiency.
			// A battery that discharged this tick never charges in the same
			// tick, so no charge/discharge cycle can circulate energy.
			for (auto& battery : batteries)
			{
				if (batteryDisabled(battery) || availableGeneration <= 0.0) continue;
				if (battery.lastDischargeKW > 0.0) continue;
				auto const headroom = battery.authored.nameplateKWh - battery.stored;
				if (headroom <= 0.0) continue;
				auto const busKW = std::min(battery.authored.maxChargeKW, availableGeneration);
				if (busKW <= 0.0) continue;
				auto const storedGain = std::min(busKW * hours
					* battery.authored.chargeEfficiency, headroom);
				if (storedGain <= 0.0) continue;
				auto const busUsed = storedGain / battery.authored.chargeEfficiency;
				battery.stored += storedGain;
				battery.chargedKWh += storedGain;
				battery.chargeInputKWh += busUsed;
				battery.lastChargeKW = busUsed / hours;
				// The pool is in power units: subtract the bus power the
				// battery actually took, not the tick's energy.
				availableGeneration -= busUsed / hours;
			}
			// Publish each battery's observable state for this tick: the
			// reserve and saturation flags and the discharge service status.
			for (auto& battery : batteries)
			{
				if (batteryDisabled(battery))
				{
					battery.lastAtReserve = false;
					battery.lastSaturated = false;
					battery.status = StationTransferStatus::Idle;
					continue;
				}
				auto const reserve = batteryReserve(battery);
				battery.lastAtReserve = battery.stored <= reserve + 1e-12;
				auto const nameplate = battery.authored.nameplateKWh;
				battery.lastSaturated = battery.stored >= nameplate - 1e-12;
				if (battery.lastDischargeKW > 1e-12)
					battery.status = battery.lastDischargeShortfallKW <= 1e-12
						? StationTransferStatus::Full : StationTransferStatus::Partial;
				else if (battery.lastDischargeRequestedKW > 1e-12)
					battery.status = StationTransferStatus::Rejected;
				else battery.status = StationTransferStatus::Idle;
			}
			lastAvailableGenerationKW = availableGeneration;
			lastBatteryShortfallKW = batteryShortfallKW;
			for (auto const index : transferOrder)
			{
				auto& transfer = transfers[index];
				auto const& source = stores[transfer.sourceIndex];
				auto const& destination = stores[transfer.destinationIndex];
				auto const requested = transfer.authored.ratePerSecond * static_cast<double>(seconds);
				// Only qualified material above the protected reserve may move,
				// and only into the destination's remaining total capacity.
				auto const drawable = std::max(0.0, qualified[transfer.sourceIndex] - source.authored.protectedReserve);
				auto const headroom = destination.authored.capacity
					- (qualified[transfer.destinationIndex] + unqualified[transfer.destinationIndex]);
				auto const completed = std::min({requested, drawable, headroom});
				qualified[transfer.sourceIndex] -= completed;
				qualified[transfer.destinationIndex] += completed;
				flowOut[transfer.sourceIndex] += completed;
				flowIn[transfer.destinationIndex] += completed;
				transfer.lastRequested = requested;
				transfer.lastCompleted = completed;
				transfer.lastUnmet = requested - completed;
				transfer.status = completed <= 0.0 ? StationTransferStatus::Rejected
					: (completed >= requested ? StationTransferStatus::Full : StationTransferStatus::Partial);
				if (transfer.status != StationTransferStatus::Full)
					stallReason[index] = drawable <= 0.0
						? (source.authored.protectedReserve > 0.0
							? "source usable inventory is at its protected reserve"
							: "source usable inventory is empty")
						: (headroom <= 0.0 ? "destination store is at capacity" : "limited supply");
			}
			// Conversions run against the staged post-transfer inventory.
			for (auto const index : conversionOrder)
			{
				auto& conversion = conversions[index];
				double scale = 1.0;
				std::string limiter;
				for (auto const& flow : conversion.inputs)
				{
					auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
					auto const drawable = std::max(0.0, qualified[flow.storeIndex]
						- stores[flow.storeIndex].authored.protectedReserve);
					auto const ratio = drawable / requested;
					if (ratio < scale)
					{
						scale = ratio;
						limiter = stores[flow.storeIndex].authored.protectedReserve > 0.0
							? "input store '" + stores[flow.storeIndex].authored.id
								+ "' is at its protected reserve"
							: "input store '" + stores[flow.storeIndex].authored.id + "' supply limits throughput";
					}
				}
				for (auto const& flow : conversion.outputs)
				{
					auto const produced = flow.ratePerSecond * static_cast<double>(seconds);
					auto const headroom = stores[flow.storeIndex].authored.capacity
						- (qualified[flow.storeIndex] + unqualified[flow.storeIndex]);
					auto const ratio = headroom / produced;
					if (ratio < scale)
					{
						scale = ratio;
						limiter = "output store '" + stores[flow.storeIndex].authored.id
							+ "' headroom limits throughput";
					}
				}
				if (!(scale > 0.0))
				{
					for (auto& flow : conversion.inputs)
					{
						flow.lastRequested = flow.ratePerSecond * static_cast<double>(seconds);
						flow.lastCompleted = 0.0;
					}
					for (auto& flow : conversion.outputs)
					{
						flow.lastRequested = flow.ratePerSecond * static_cast<double>(seconds);
						flow.lastCompleted = 0.0;
					}
					conversion.lastScale = 0.0;
					conversion.status = StationTransferStatus::Rejected;
					conversion.stallReason = limiter.empty()
						? "no input available to run" : limiter;
					continue;
				}
				// #556: the binding limit is applied per flow with a min
				// against that flow's own drawable/headroom, because
				// requested * (limit / requested) can round one ulp above the
				// limit under the fixed float timestep. Clamping each flow to
				// the limit it set keeps the common completed scale within one
				// ulp of exact, never drives a store negative or over capacity
				// from roundoff, and the residual lands in the declared loss
				// inside the documented accounting tolerance — no material is
				// invented or deleted.
				ConstituentLedger netChange;
				for (auto& flow : conversion.inputs)
				{
					auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
					auto const drawable = std::max(0.0, qualified[flow.storeIndex]
						- stores[flow.storeIndex].authored.protectedReserve);
					auto const completed = std::min(requested * scale, drawable);
					qualified[flow.storeIndex] -= completed;
					// Conversion draws and deliveries are store outflow/inflow
					// just like transfers and equipment exchanges: the public
					// per-store last-tick fields describe every flow the tick
					// moved through the store, not transfers only (#552).
					flowOut[flow.storeIndex] += completed;
					flow.lastRequested = requested;
					flow.lastCompleted = completed;
					addComposition(netChange, *stores[flow.storeIndex].authored.composition,
						massToKg(stores[flow.storeIndex].authored.unit, completed), -1.0);
				}
				for (auto& flow : conversion.outputs)
				{
					auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
					auto const headroom = stores[flow.storeIndex].authored.capacity
						- (qualified[flow.storeIndex] + unqualified[flow.storeIndex]);
					auto const completed = std::min(requested * scale, headroom);
					qualified[flow.storeIndex] += completed;
					flowIn[flow.storeIndex] += completed;
					flow.lastRequested = requested;
					flow.lastCompleted = completed;
					addComposition(netChange, *stores[flow.storeIndex].authored.composition,
						massToKg(stores[flow.storeIndex].authored.unit, completed), 1.0);
				}
				// The declared conversion loss is what the outputs did not
				// carry away; it stays accounted for instead of vanishing.
				for (auto const& [constituent, change] : netChange)
					stagedLoss[constituent] -= change;
				for (auto const& flow : conversion.inputs)
					stagedUnitLoss[stores[flow.storeIndex].authored.unit] += flow.lastCompleted;
				for (auto const& flow : conversion.outputs)
					stagedUnitLoss[stores[flow.storeIndex].authored.unit] -= flow.lastCompleted;
				conversion.lastScale = scale;
				conversion.status = scale >= 1.0 - 1e-12
					? StationTransferStatus::Full : StationTransferStatus::Partial;
				if (conversion.status == StationTransferStatus::Partial)
					conversion.stallReason = limiter;
			}
			std::map<std::string, double> currentTotals;
			for (size_t index = 0; index < stores.size(); ++index)
			{
				if (!std::isfinite(qualified[index]) || !std::isfinite(unqualified[index]))
					return violation = "transfer left non-finite inventory in store '" + stores[index].authored.id + "'", false;
				// #556: the finite-limit audit honors the same documented
				// numerical tolerance as the conservation residuals, so a
				// one-ulp roundoff is never a software failure, while any
				// genuine negative or over-capacity breach still fails the
				// whole tick.
				auto const limitBound = StationBalanceTolerance
					* std::max(1.0, stores[index].authored.capacity);
				if (qualified[index] < -limitBound || unqualified[index] < -limitBound)
					return violation = "transfer drove inventory negative in store '" + stores[index].authored.id + "'", false;
				if (qualified[index] + unqualified[index] > stores[index].authored.capacity + limitBound)
					return violation = "transfer exceeded capacity of store '" + stores[index].authored.id + "'", false;
				currentTotals[stores[index].authored.unit] += qualified[index] + unqualified[index];
			}
			for (auto const& [unit, starting] : startingTotals)
			{
				// The boundary now has an inflow side: accepted deliveries
				// bring material in from outside the station, so the ledger
				// closes over current + lost - delivered, never over current
				// alone (#392). Water recovered out of a compartment
				// atmosphere is a second inflow of the same kind (#402): the
				// condensate comes from outside the store network, so it is
				// subtracted on the same side rather than reading as a
				// creation of mass.
				auto const residual = currentTotals[unit] + stagedUnitLoss[unit]
					- stagedUnitDelivered[unit] - stagedUnitCollected[unit] - starting;
				if (!std::isfinite(residual)
					|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
					return violation = "conservation residual " + std::to_string(residual)
						+ " for unit '" + unit + "' exceeds tolerance " + std::to_string(StationBalanceTolerance), false;
			}
			// Compound conservation boundary (#391): every tracked
			// constituent of the compound stores, plus the cumulative
			// declared conversion losses, must account for the starting
			// inventory. A residual beyond tolerance is a violation, never
			// silent creation or deletion of material.
			ConstituentLedger currentConstituents;
			for (size_t index = 0; index < stores.size(); ++index)
				if (stores[index].authored.composition)
					addComposition(currentConstituents, *stores[index].authored.composition,
						massToKg(stores[index].authored.unit,
							qualified[index] + unqualified[index]), 1.0);
			for (auto const& [constituent, starting] : startingConstituents)
			{
				auto const residual = (currentConstituents.count(constituent)
						? currentConstituents.at(constituent) : 0.0)
					+ stagedLoss[constituent] - stagedConstituentDelivered[constituent] - starting;
				if (!std::isfinite(residual)
					|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
					return violation = "constituent conservation residual " + std::to_string(residual)
						+ " for '" + constituent + "' exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
			}
			// Battery energy boundary (#394): stored energy stays within the
			// authored nameplate, the ledger closes over charge and discharge,
			// and neither direction may deliver or store more energy than it
			// took in — free energy is a conservation violation, never a
			// silent rounding. The reserve floor is never drawn below the
			// lower of the starting stored energy and the reserve itself.
			for (auto const& battery : batteries)
			{
				auto const where = "battery '" + battery.authored.id + "'";
				auto const bound = StationBalanceTolerance
					* std::max(1.0, std::abs(battery.authored.startingStoredKWh));
				if (!std::isfinite(battery.stored) || battery.stored < 0.0)
					return violation = "battery " + where + " holds non-finite or negative stored energy", false;
				if (battery.stored > battery.authored.nameplateKWh + bound)
					return violation = "battery " + where + " stored energy exceeds its nameplate capacity", false;
				auto const floor = std::min(battery.authored.startingStoredKWh,
						battery.authored.nameplateKWh - battery.authored.usableKWh);
				if (battery.stored < floor - bound)
					return violation = "battery " + where + " was drawn below its reserve floor", false;
				auto const residual = battery.authored.startingStoredKWh - battery.dischargedKWh
					+ battery.chargedKWh - battery.stored;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = "battery " + where + " energy ledger residual "
						+ std::to_string(residual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
				if (battery.deliveredKWh > battery.dischargedKWh + bound)
					return violation = "battery " + where + " delivered more energy than it drew from storage", false;
				if (battery.chargedKWh > battery.chargeInputKWh + bound)
					return violation = "battery " + where + " stored more energy than the bus supplied to it", false;
			}
			// The single-pool audit: total supplied service never exceeds
			// generation plus what the batteries actually delivered. Coolant
			// pump and heater draws ride the same pool as external loads
			// (#396), so they are inside this audit too — the loop is never
			// a second power budget.
			{
				double totalSupplied = 0.0, totalGeneration = 0.0, totalDischarged = 0.0;
				for (auto const& load : loads) totalSupplied += load.lastSuppliedKW;
				for (auto const& loop : coolantLoops)
					totalSupplied += loop.lastPumpSuppliedKW + loop.lastHeaterSuppliedKW;
				for (auto const& chiller : chillers) totalSupplied += chiller.lastPowerSuppliedKW;
				// Generator startup cranking draws ride the same pool as
				// every other service (#399); generator output is
				// generation inside it. Neither side is ever a second budget.
				for (auto const& generator : generators) totalSupplied += generator.lastStartupDrawKW;
				// Water treatment work rides the same pool (#402): it is
				// never a third budget beside the loads and the thermal plant.
				for (auto const& treatment : waterTreatments)
					totalSupplied += treatment.lastPowerSuppliedKW;
				// Electrolysis and CO2 capture work ride the same pool (#404):
				// oxygen production and carbon removal are services, not a
				// second budget beside the loads and the thermal plant.
				for (auto const& cell : electrolyzers)
					totalSupplied += cell.lastPowerSuppliedKW;
				for (auto const& capture : co2Captures)
					totalSupplied += capture.lastPowerSuppliedKW;
				for (auto const& array : solarArrays) totalGeneration += array.lastAvailableKW;
				for (auto const& generator : generators) totalGeneration += generator.outputKW;
				// Fuel-cell backup output is generation inside the single pool
				// (#406): earned with reactants, never a second budget.
				for (auto const& cell : fuelCells) totalGeneration += cell.lastOutputKW;
				for (auto const& battery : batteries) totalDischarged += battery.lastDischargeKW;
				auto const bound = StationBalanceTolerance * std::max(1.0, totalGeneration + totalDischarged);
				if (totalSupplied > totalGeneration + totalDischarged + bound)
					return violation = "total supplied power exceeds generation plus battery output", false;
			}
			// The minimum-operable audit (#549): a load is either off or at
			// least its minimum operable supply, demand * (1 - allowed
			// curtailment). A grant that lands strictly between nothing and
			// that minimum buys a state the equipment cannot physically run
			// in, so it is a software failure rather than a shortage.
			for (auto const& load : loads)
			{
				if (load.lastRequestedKW <= 0.0 || !load.lastAdmitted) continue;
				auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
				auto const bound = StationBalanceTolerance * std::max(1.0, load.lastRequestedKW);
				if (load.lastSuppliedKW > 1e-12 && load.lastSuppliedKW + bound < minOperating)
					return violation = "load '" + load.authored.id
						+ "' was supplied below its minimum operable supply level", false;
			}
			// #553: finiteness audit of every committed numeric power,
			// material and energy output and ledger intermediate. Authored
			// values are individually validated finite, but sums and
			// products of finite values can overflow to infinity, and
			// inequality audits never notice: an infinite magnitude passes
			// every upper-bound comparison and inf > inf is false. A
			// non-finite derived result fails the whole tick through the
			// same contained path as any other ledger violation — the
			// applyLedger wrapper restores the ledger, nothing is
			// published, and the diagnostic names the value. Ordinary
			// finite shortages are valid outcomes and are untouched here.
			auto requireFinite = [&](double value, std::string const& what)
			{
				if (std::isfinite(value)) return true;
				violation = "non-finite " + what + " is not a publishable physical result";
				return false;
			};
			for (size_t index = 0; index < stores.size(); ++index)
			{
				auto const where = "store '" + stores[index].authored.id + "' ";
				if (!requireFinite(flowIn[index], where + "last-tick inflow")) return false;
				if (!requireFinite(flowOut[index], where + "last-tick outflow")) return false;
			}
			for (auto const& [unit, loss] : stagedUnitLoss)
				if (!requireFinite(loss, "cumulative loss of unit '" + unit + "'")) return false;
			for (auto const& [constituent, loss] : stagedLoss)
				if (!requireFinite(loss, "cumulative loss of constituent '" + constituent + "'")) return false;
			for (auto const& [unit, delivered] : stagedUnitDelivered)
				if (!requireFinite(delivered, "cumulative delivered amount of unit '" + unit + "'")) return false;
			for (auto const& [unit, collected] : stagedUnitCollected)
				if (!requireFinite(collected, "cumulative collected amount of unit '" + unit + "'")) return false;
			for (auto const& [constituent, delivered] : stagedConstituentDelivered)
				if (!requireFinite(delivered, "cumulative delivered constituent '" + constituent + "'")) return false;
			for (auto const& delivery : deliveries)
			{
				auto const where = "delivery '" + delivery.authored.id + "' ";
				if (!requireFinite(delivery.lastAccepted, where + "accepted amount")) return false;
				if (!requireFinite(delivery.lastRejected, where + "rejected amount")) return false;
				if (!requireFinite(delivery.acceptedTotal, where + "cumulative accepted amount")) return false;
				if (!requireFinite(delivery.rejectedTotal, where + "cumulative rejected amount")) return false;
			}
			for (auto const& ejection : wasteEjections)
			{
				auto const where = "waste ejection '" + ejection.authored.id + "' ";
				if (!requireFinite(ejection.lastEjected, where + "ejected amount")) return false;
				if (!requireFinite(ejection.lastUnmet, where + "unmet amount")) return false;
				if (!requireFinite(ejection.ejectedTotal, where + "cumulative ejected amount")) return false;
				if (!requireFinite(ejection.unmetTotal, where + "cumulative unmet amount")) return false;
			}
			for (auto const& transfer : transfers)
			{
				auto const where = "transfer '" + transfer.authored.id + "' ";
				if (!requireFinite(transfer.lastRequested, where + "requested amount")) return false;
				if (!requireFinite(transfer.lastCompleted, where + "completed amount")) return false;
				if (!requireFinite(transfer.lastUnmet, where + "unmet amount")) return false;
			}
			for (auto const& conversion : conversions)
			{
				auto const where = "conversion '" + conversion.authored.id + "' ";
				if (!requireFinite(conversion.lastScale, where + "completed scale")) return false;
				for (auto const& flow : conversion.inputs)
				{
					if (!requireFinite(flow.lastRequested, where + "input requested amount")) return false;
					if (!requireFinite(flow.lastCompleted, where + "input completed amount")) return false;
				}
				for (auto const& flow : conversion.outputs)
				{
					if (!requireFinite(flow.lastRequested, where + "output requested amount")) return false;
					if (!requireFinite(flow.lastCompleted, where + "output completed amount")) return false;
				}
			}
			system = "Thermal control";
			double totalAbsorbedKW = 0.0, totalRoutedKW = 0.0, totalRejectedKW = 0.0;
			for (auto const& reservoir : reservoirs)
			{
				auto const where = "thermal reservoir '" + reservoir.authored.id + "' ";
				if (!requireFinite(reservoir.temperatureC, where +"integrated temperature")) return false;
				if (!requireFinite(reservoir.lastHeatRoutedKW, where + "routed heat power")) return false;
				if (!requireFinite(reservoir.lastLoopHeatKW, where + "coolant loop heat")) return false;
				if (!requireFinite(reservoir.lastCondenserHeatKW, where + "condenser heat")) return false;
				if (!requireFinite(reservoir.lastGeneratorHeatKW, where + "generator heat")) return false;
				if (!requireFinite(reservoir.lastAtmosphereHeatKW, where + "atmosphere exchange")) return false;
				if (!requireFinite(reservoir.lastElectrolysisHeatKW, where + "electrolysis process heat")) return false;
				if (!requireFinite(reservoir.lastFuelCellHeatKW, where + "fuel-cell waste heat")) return false;
				if (!requireFinite(reservoir.lastSabatierHeatKW, where + "Sabatier reaction heat")) return false;
				if (!requireFinite(reservoir.lastMethaneProcessingHeatKW, where + "methane-processing waste heat")) return false;
				if (!requireFinite(reservoir.lastMicrobialHeatKW, where + "microbial digestion heat")) return false;
				if (!requireFinite(reservoir.lastHeatInKW, where + "total heat inflow")) return false;
				if (!requireFinite(reservoir.lastRejectedKW, where + "radiator rejection")) return false;
				if (!requireFinite(reservoir.lastNetKW, where + "net heat balance")) return false;
				if (!requireFinite(reservoir.absorbedKJ, where + "cumulative absorbed energy")) return false;
				if (!requireFinite(reservoir.routedKJ, where + "cumulative routed energy")) return false;
				if (!requireFinite(reservoir.loopKJ, where + "cumulative loop exchange energy")) return false;
				if (!requireFinite(reservoir.condenserKJ, where + "cumulative condenser energy")) return false;
				if (!requireFinite(reservoir.generatorKJ, where + "cumulative generator heat energy")) return false;
				if (!requireFinite(reservoir.electrolysisKJ, where + "cumulative electrolysis process heat energy")) return false;
				if (!requireFinite(reservoir.fuelCellKJ, where + "cumulative fuel-cell waste heat energy")) return false;
				if (!requireFinite(reservoir.sabatierKJ, where + "cumulative Sabatier heat energy")) return false;
				if (!requireFinite(reservoir.methaneProcessingKJ, where + "cumulative methane-processing heat energy")) return false;
				if (!requireFinite(reservoir.microbialKJ, where + "cumulative microbial heat energy")) return false;
				if (!requireFinite(reservoir.atmosphereKJ, where + "cumulative atmosphere exchange energy")) return false;
				if (!requireFinite(reservoir.rejectedKJ, where + "cumulative rejected energy")) return false;
				if (!requireFinite(reservoir.lastBufferChargeKW, where + "buffer charge power")) return false;
				if (!requireFinite(reservoir.lastBufferDischargeKW, where + "buffer discharge power")) return false;
				if (!requireFinite(reservoir.bufferChargeKJ, where + "cumulative buffer charge energy")) return false;
				if (!requireFinite(reservoir.bufferDischargeKJ, where + "cumulative buffer discharge energy")) return false;
				totalAbsorbedKW += reservoir.authored.absorbedHeatKW;
				totalRoutedKW += reservoir.lastHeatRoutedKW;
				totalRejectedKW += reservoir.lastRejectedKW;
			}
			for (auto const& radiator : radiators)
				if (!requireFinite(radiator.lastRejectedKW,
					"radiator '" + radiator.authored.id + "' rejected power")) return false;
			// Thermal buffer storage ledger (#398): the store's starting
			// energy plus what it charged minus what it discharged must equal
			// the stored energy within the declared tolerance: energy enters
			// or leaves the store only through the booked exchange, never a
			// silent reset or an unexplained drain. The store never leaves
			// its finite bounds, and neither direction may exceed its
			// configured rate limit: a violation is a conservation failure,
			// never a silent rounding.
			for (auto const& buffer : thermalBuffers)
			{
				auto const where = "thermal buffer '" + buffer.authored.id + "' ";
				if (!requireFinite(buffer.storedKJ, where + "stored energy")) return false;
				if (!requireFinite(buffer.lastChargeKW, where + "charge power")) return false;
				if (!requireFinite(buffer.lastDischargeKW, where + "discharge power")) return false;
				if (!requireFinite(buffer.chargedKJ, where + "cumulative charged energy")) return false;
				if (!requireFinite(buffer.dischargedKJ, where + "cumulative discharged energy")) return false;
				auto const bound = StationBalanceTolerance * std::max(1.0,
					std::max(std::abs(buffer.authored.startingEnergyKJ),
						std::max(std::abs(buffer.chargedKJ), std::abs(buffer.dischargedKJ))));
				auto const residual = buffer.authored.startingEnergyKJ + buffer.chargedKJ
					- buffer.dischargedKJ - buffer.storedKJ;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = where + "storage ledger residual "
						+ std::to_string(residual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
				if (buffer.storedKJ < -StationBalanceTolerance)
					return violation = where + "stored energy went negative", false;
				if (buffer.storedKJ > buffer.authored.capacityKJ + StationBalanceTolerance
					* std::max(1.0, buffer.authored.capacityKJ))
					return violation = where + "stored energy exceeds the buffer capacity", false;
				if (buffer.lastChargeKW > buffer.authored.maxChargeKW + StationBalanceTolerance)
					return violation = where + "charged above its configured rate limit", false;
				if (buffer.lastDischargeKW > buffer.authored.maxDischargeKW + StationBalanceTolerance)
					return violation = where + "discharged above its configured rate limit", false;
			}
			// #565: the station-wide thermal rollups the snapshot publishes
			// are sums of individually finite terms; the aggregate must
			// itself be finite before the tick that produced it can be
			// committed, or an overflowed total escapes containment through
			// the publish path.
			if (!requireFinite(totalAbsorbedKW, "station total absorbed heat power")) return false;
			if (!requireFinite(totalRoutedKW, "station total routed heat power")) return false;
			if (!requireFinite(totalRejectedKW, "station total rejected heat power")) return false;
			// Coolant loop ledger (#396): starting + pump work + heater +
			// exchanged from the reservoir must equal the loop's current
			// capacity x temperature within the declared tolerance. The
			// loop's coolant inventory is finite and never grows: a dry
			// loop stays dry, and its ledger stays closed at zero service.
			system = "Coolant circulation";
			double totalCirculationKgPerS = 0.0, totalFreezeHeatingKW = 0.0;
			double totalPumpPowerKW = 0.0;
			for (auto const& loop : coolantLoops)
			{
				auto const where = "coolant loop '" + loop.authored.id + "' ";
				if (!requireFinite(loop.coolantKg, where + "coolant inventory")) return false;
				if (!requireFinite(loop.temperatureC, where + "integrated temperature")) return false;
				if (!requireFinite(loop.lastPumpSuppliedKW, where + "pump supplied power")) return false;
				if (!requireFinite(loop.lastHeaterSuppliedKW, where + "heater supplied power")) return false;
				if (!requireFinite(loop.lastCirculationFraction, where + "circulation fraction")) return false;
				if (!requireFinite(loop.lastFlowKgPerS, where + "circulation flow")) return false;
				if (!requireFinite(loop.lastExchangeIntoLoopKW, where + "exchanger exchange")) return false;
				if (!requireFinite(loop.lastChillerExtractionKW, where + "chiller extraction")) return false;
				if (!requireFinite(loop.pumpWorkKJ, where + "cumulative pump work energy")) return false;
				if (!requireFinite(loop.heaterKJ, where + "cumulative heater energy")) return false;
				if (!requireFinite(loop.exchangedKJ, where + "cumulative exchanged energy")) return false;
				if (!requireFinite(loop.extractedKJ, where + "cumulative chiller extracted energy")) return false;
				if (!requireFinite(loop.lastCollectorHeatKW, where + "last water-recovery heat")) return false;
				if (!requireFinite(loop.collectorHeatKJ, where + "cumulative water-recovery heat")) return false;
				if (loop.coolantKg < 0.0)
					return violation = where + "coolant inventory went negative", false;
				if (loop.coolantKg > loop.authored.coolantCapacityKg + StationBalanceTolerance
					* std::max(1.0, loop.authored.coolantCapacityKg))
					return violation = where + "coolant inventory exceeds the loop capacity", false;
				totalCirculationKgPerS += loop.lastFlowKgPerS;
				totalFreezeHeatingKW += loop.lastHeaterSuppliedKW;
				totalPumpPowerKW += loop.lastPumpSuppliedKW;
				auto const startingEnergyKJ = loop.authored.startingCoolantKg
					* loop.authored.specificHeatKJPerKgK * loop.authored.startingTemperatureC;
				auto const bound = StationBalanceTolerance * std::max(1.0, std::abs(startingEnergyKJ));
				auto const residual = startingEnergyKJ + loop.pumpWorkKJ + loop.heaterKJ
					+ loop.exchangedKJ + loop.collectorHeatKJ - loop.extractedKJ
					- loop.coolantKg * loop.authored.specificHeatKJPerKgK
					* loop.temperatureC;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = where + "thermal energy ledger residual "
						+ std::to_string(residual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
			}
			// #565: the published circulation and freeze-heating rollups
			// are sums of individually finite per-loop terms; the aggregate
			// must itself be finite before the tick may commit, or an
			// overflowed station total escapes containment through the
			// publish path.
			if (!requireFinite(totalCirculationKgPerS, "station total coolant circulation flow")) return false;
			if (!requireFinite(totalFreezeHeatingKW, "station total freeze-protection heating power")) return false;
			if (!requireFinite(totalPumpPowerKW, "station total coolant pump power")) return false;
			// Chilled service ledger (#397): the chiller's condenser heat
			// must equal the cold-side heat it extracted plus the work and
			// auxiliaries it was actually supplied, cumulatively and within
			// the declared tolerance. A residual beyond it means heat was
			// booked twice or vanished once — a conservation violation that
			// fails the whole tick, never a silent rounding.
			system = "Chilled thermal service";
			double totalCoolingProvidedKW = 0.0, totalCoolingUnmetKW = 0.0, totalCondenserHeatKW = 0.0;
			double totalChillerSuppliedKW = 0.0;
			// Cold-service rollups segregated by thermal service class: the
			// snapshot publishes one capacity/provided/unmet/work/condenser
			// account per class, and every aggregate must be finite before
			// the tick commits (#565).
			std::map<std::string, std::array<double, 5>> classRollups;
			for (auto const& chiller : chillers)
			{
				auto const where = "chiller '" + chiller.authored.id + "' ";
				if (!requireFinite(chiller.lastCoolingDemandKW, where + "cooling demand")) return false;
				if (!requireFinite(chiller.lastCoolingKW, where + "cold-side duty")) return false;
				if (!requireFinite(chiller.lastDeliverableKW, where + "circulation-capped duty")) return false;
				if (!requireFinite(chiller.lastPowerDemandKW, where + "power demand")) return false;
				if (!requireFinite(chiller.lastPowerSuppliedKW, where + "power supplied")) return false;
				if (!requireFinite(chiller.lastCondenserKW, where + "condenser heat")) return false;
				if (!requireFinite(chiller.lastDeliveredFraction, where + "delivered fraction")) return false;
				if (!requireFinite(chiller.extractedKJ, where + "cumulative extracted energy")) return false;
				if (!requireFinite(chiller.workKJ, where + "cumulative work energy")) return false;
				if (!requireFinite(chiller.condenserKJ, where + "cumulative condenser energy")) return false;
				if (chiller.lastCoolingKW > chiller.lastDeliverableKW + StationBalanceTolerance)
					return violation = where + "extracted more cold-side duty than its circulation could deliver", false;
				if (chiller.lastPowerSuppliedKW > chiller.lastPowerDemandKW + StationBalanceTolerance)
					return violation = where + "was supplied more power than it demanded", false;
				auto const bound = StationBalanceTolerance
					* std::max(1.0, std::max(std::abs(chiller.extractedKJ), std::abs(chiller.condenserKJ)));
				auto const residual = chiller.extractedKJ + chiller.workKJ - chiller.condenserKJ;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = where + "condenser ledger residual "
						+ std::to_string(residual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
				totalCoolingProvidedKW += chiller.lastCoolingKW;
				totalCoolingUnmetKW += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
				totalCondenserHeatKW += chiller.lastCondenserKW;
				totalChillerSuppliedKW += chiller.lastPowerSuppliedKW;
				auto& rollup = classRollups[chiller.authored.thermalClass];
				rollup[0] += chiller.authored.ratedCoolingCapacityKW;
				rollup[1] += chiller.lastCoolingKW;
				rollup[2] += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
				rollup[3] += chiller.lastPowerSuppliedKW;
				rollup[4] += chiller.lastCondenserKW;
			}
			if (!requireFinite(totalCoolingProvidedKW, "station total cooling provided power")) return false;
			if (!requireFinite(totalCoolingUnmetKW, "station total cooling unmet power")) return false;
			if (!requireFinite(totalCondenserHeatKW, "station total condenser heat power")) return false;
			if (!requireFinite(totalChillerSuppliedKW, "station total chiller electrical power")) return false;
			// #565: a cold-class capacity is a sum of authored ratings —
			// two finite 1e308 kW chillers in one class overflow the
			// published capacity even when every per-chiller result is
			// finite and the machines are gated off. The class rollup is
			// audited before the successful state may publish it.
			for (auto const& entry : classRollups)
			{
				auto const where = "cold-service class '" + entry.first + "' ";
				if (!requireFinite(entry.second[0], where + "total cooling capacity")) return false;
				if (!requireFinite(entry.second[1], where + "total cooling provided")) return false;
				if (!requireFinite(entry.second[2], where + "total cooling unmet")) return false;
				if (!requireFinite(entry.second[3], where + "total service work power")) return false;
				if (!requireFinite(entry.second[4], where + "total condenser heat")) return false;
			}
			// Generator fuel ledger (#399): every joule of fuel energy the
			// generator consumed must appear as delivered electrical energy
			// or as waste heat routed to the sink, cumulatively and within
			// the declared tolerance. A residual beyond it means fuel
			// energy vanished or electrical energy appeared from nowhere -
			// a conservation violation that fails the whole tick, never a
			// silent rounding. Output never exceeds the installed rating,
			// never moves faster than the ramp limit, and never runs while
			// its cooling interface is unqualified.
			system = "Generator fuel service";
			for (auto const& generator : generators)
			{
				auto const where = "generator '" + generator.authored.id + "' ";
				if (!requireFinite(generator.outputKW, where + "electrical output")) return false;
				if (!requireFinite(generator.lastFuelDrawKWh, where + "fuel draw")) return false;
				if (!requireFinite(generator.lastWasteHeatKW, where + "waste heat")) return false;
				if (!requireFinite(generator.lastStartupDrawKW, where + "startup draw")) return false;
				if (!requireFinite(generator.lastDecayHeatKW, where + "decay heat")) return false;
				if (!requireFinite(generator.startupReceivedKJ, where + "startup energy received")) return false;
				if (!requireFinite(generator.fuelEnergyKJ, where + "cumulative fuel energy")) return false;
				if (!requireFinite(generator.electricalKJ, where + "cumulative electrical energy")) return false;
				if (!requireFinite(generator.wasteHeatKJ, where + "cumulative waste heat")) return false;
				if (!requireFinite(generator.startupHeatKJ, where + "cumulative startup heat")) return false;
				if (!requireFinite(generator.decayHeatKJ, where + "cumulative decay heat")) return false;
				auto const bound = StationBalanceTolerance
					* std::max(1.0, std::max(std::abs(generator.fuelEnergyKJ), std::abs(generator.electricalKJ)));
				auto const residual = generator.fuelEnergyKJ - generator.electricalKJ - generator.wasteHeatKJ;
				if (!std::isfinite(residual) || std::abs(residual) > bound)
					return violation = where + "fuel ledger residual "
						+ std::to_string(residual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
				if (generator.outputKW > generator.authored.ratedOutputKW + StationBalanceTolerance)
					return violation = where + "delivered above its installed capacity", false;
				if (generator.phase == StationGeneratorPhase::Running && !generator.lastCoolingAvailable)
					return violation = where + "ran while its cooling interface was unqualified", false;
				if (generator.phase == StationGeneratorPhase::Running
					&& generator.lastFuelDrawKWh <= 0.0 && generator.outputKW > 1e-12)
					return violation = where + "delivered electrical output without burning fuel", false;
				totalGeneratorOutputKW += generator.outputKW;
				totalGeneratorStartupKW += generator.lastStartupDrawKW;
				totalGeneratorWasteKW += generator.lastWasteHeatKW;
				totalGeneratorDecayKW += generator.lastDecayHeatKW;
			}
			// #565: the published generator rollups are sums of
			// individually finite per-machine terms; an overflowed
			// aggregate must fail the tick rather than publish infinity
			// through the snapshot's power section.
			if (!requireFinite(totalGeneratorOutputKW, "station total generator output power")) return false;
			if (!requireFinite(totalGeneratorStartupKW, "station total generator startup draw power")) return false;
			if (!requireFinite(totalGeneratorWasteKW, "station total generator waste heat power")) return false;
			if (!requireFinite(totalGeneratorDecayKW, "station total generator decay heat power")) return false;
			// Atmosphere containment ledger (#400): every component of
			// every compartment closes over its own pathways - starting +
			// added - consumed/condensed/leaked/cycled must equal the
			// current inventory within the declared tolerance, and the
			// recovered-plus-lost split must equal exactly what the
			// airlock cycles removed. A residual beyond tolerance means
			// gas vanished or appeared without a booked pathway - an
			// unexplained disposal, a conservation violation that fails
			// the whole tick, never a silent rounding. The thermal ledger
			// closes over occupant heat, latent heat and the reservoir
			// exchange against capacity x temperature, and no derived
			// magnitude may be non-finite.
			system = "Atmosphere containment";
			for (auto const& compartment : atmospheres)
			{
				auto const where = "atmosphere '" + compartment.authored.id + "' ";
				if (!requireFinite(compartment.oxygenKg, where + "oxygen inventory")) return false;
				if (!requireFinite(compartment.nitrogenKg, where + "nitrogen inventory")) return false;
				if (!requireFinite(compartment.carbonDioxideKg, where + "carbon dioxide inventory")) return false;
				if (!requireFinite(compartment.waterVapourKg, where + "water vapour inventory")) return false;
				if (!requireFinite(compartment.temperatureC, where + "integrated temperature")) return false;
				if (!requireFinite(compartment.lastOxygenDemandKg, where + "oxygen demand")) return false;
				if (!requireFinite(compartment.lastOxygenConsumedKg, where + "oxygen consumed")) return false;
				if (!requireFinite(compartment.lastCo2ProducedKg, where + "carbon dioxide produced")) return false;
				if (!requireFinite(compartment.lastVapourAddedKg, where + "vapour added")) return false;
				if (!requireFinite(compartment.lastVapourCondensedKg, where + "vapour condensed")) return false;
				if (!requireFinite(compartment.lastLeakedKg, where + "leaked mass")) return false;
				if (!requireFinite(compartment.lastCycleRemovedKg, where + "airlock removed mass")) return false;
				if (!requireFinite(compartment.lastCollectedKg, where + "water recovered mass")) return false;
				if (!requireFinite(compartment.lastElectrolysisOxygenKg, where + "electrolysis oxygen added")) return false;
				if (!requireFinite(compartment.lastCo2CapturedKg, where + "CO2 captured mass")) return false;
				if (!requireFinite(compartment.lastOccupantHeatKW, where + "occupant heat")) return false;
				if (!requireFinite(compartment.lastLatentHeatKW, where + "latent heat")) return false;
				if (!requireFinite(compartment.lastExchangeIntoCompartmentKW, where + "reservoir exchange")) return false;
				if (!requireFinite(compartment.leakedKg, where + "cumulative leaked mass")) return false;
				if (!requireFinite(compartment.recoveredKg, where + "cumulative recovered mass")) return false;
				if (!requireFinite(compartment.lostKg, where + "cumulative lost mass")) return false;
				if (!requireFinite(compartment.condensedKg, where + "cumulative condensed mass")) return false;
				if (!requireFinite(compartment.collectedKg, where + "cumulative water recovered mass")) return false;
				if (!requireFinite(compartment.electrolysisOxygenKg, where + "cumulative electrolysis oxygen")) return false;
				if (!requireFinite(compartment.capturedCarbonDioxideKg, where + "cumulative CO2 captured mass")) return false;
				// #565: the snapshot publishes the derived partial
				// pressures, absolute pressure and saturation vapour
				// capacity computed from this compartment state. A finite
				// inventory in a tiny volume can overflow the ideal-gas
				// derivation, and an infinite published pressure is not a
				// physical result, so every derived published magnitude is
				// audited before the tick may commit.
				auto const derivedOxygenKPa = stationAtmospherePartialPressureKPa(
					compartment.oxygenKg, StationMolarMassOxygenKgPerMol,
					compartment.authored.volumeM3, compartment.temperatureC);
				auto const derivedNitrogenKPa = stationAtmospherePartialPressureKPa(
					compartment.nitrogenKg, StationMolarMassNitrogenKgPerMol,
					compartment.authored.volumeM3, compartment.temperatureC);
				auto const derivedCarbonDioxideKPa = stationAtmospherePartialPressureKPa(
					compartment.carbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
					compartment.authored.volumeM3, compartment.temperatureC);
				auto const derivedVapourKPa = stationAtmospherePartialPressureKPa(
					compartment.waterVapourKg, StationMolarMassWaterKgPerMol,
					compartment.authored.volumeM3, compartment.temperatureC);
				if (!requireFinite(derivedOxygenKPa, where + "derived oxygen partial pressure")) return false;
				if (!requireFinite(derivedNitrogenKPa, where + "derived nitrogen partial pressure")) return false;
				if (!requireFinite(derivedCarbonDioxideKPa, where + "derived carbon dioxide partial pressure")) return false;
				if (!requireFinite(derivedVapourKPa, where + "derived water vapour partial pressure")) return false;
				if (!requireFinite(derivedOxygenKPa + derivedNitrogenKPa
					+ derivedCarbonDioxideKPa + derivedVapourKPa,
					where + "derived absolute pressure")) return false;
				if (!requireFinite(stationSaturationVapourPressureKPa(compartment.temperatureC),
					where + "derived saturation vapour pressure")) return false;
				auto const startingTotalKg = compartment.authored.startingOxygenKg
					+ compartment.authored.startingNitrogenKg
					+ compartment.authored.startingCarbonDioxideKg
					+ compartment.authored.startingWaterVapourKg;
				auto const bound = StationBalanceTolerance * std::max(1.0, startingTotalKg);
				if (compartment.oxygenKg < -bound || compartment.nitrogenKg < -bound
					|| compartment.carbonDioxideKg < -bound || compartment.waterVapourKg < -bound)
					return violation = where + "a gas inventory went negative", false;
				struct ComponentClosure
				{
					char const* name;
					double starting;
					double added;
					double removed;
					double current;
				};
				std::array<ComponentClosure, 4> const closures{{
					{"oxygen", compartment.authored.startingOxygenKg,
						compartment.electrolysisOxygenKg,
						compartment.oxygenConsumedKg + compartment.leakOxygenKg
							+ compartment.cycleOxygenKg, compartment.oxygenKg},
					{"nitrogen", compartment.authored.startingNitrogenKg, 0.0,
						compartment.leakNitrogenKg + compartment.cycleNitrogenKg,
						compartment.nitrogenKg},
					{"carbonDioxide", compartment.authored.startingCarbonDioxideKg,
						compartment.co2ProducedKg,
						compartment.leakCarbonDioxideKg + compartment.cycleCarbonDioxideKg
							+ compartment.capturedCarbonDioxideKg,
						compartment.carbonDioxideKg},
					{"waterVapour", compartment.authored.startingWaterVapourKg,
						compartment.vapourAddedKg,
						compartment.condensedKg + compartment.leakWaterVapourKg
							+ compartment.cycleWaterVapourKg + compartment.collectedKg,
						compartment.waterVapourKg}}};
				for (auto const& closure : closures)
				{
					auto const residual = closure.starting + closure.added
						- closure.removed - closure.current;
					if (!std::isfinite(residual) || std::abs(residual) > bound)
						return violation = where + std::string(closure.name)
							+ " conservation residual " + std::to_string(residual)
							+ " exceeds tolerance " + std::to_string(StationBalanceTolerance), false;
				}
				// The airlock split closes over the removal: recovered and
				// lost together are exactly the mass the cycles took out of
				// the compartment - no unexplained disposal.
				auto const cycledTotalKg = compartment.cycleOxygenKg + compartment.cycleNitrogenKg
					+ compartment.cycleCarbonDioxideKg + compartment.cycleWaterVapourKg;
				auto const splitResidual = compartment.recoveredKg + compartment.lostKg - cycledTotalKg;
				if (!std::isfinite(splitResidual) || std::abs(splitResidual) > bound)
					return violation = where + "airlock recovered/lost split residual "
						+ std::to_string(splitResidual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
				// Thermal closure: starting + occupant heat + crew heat
				// (#401) + latent heat + exchanged from the reservoir must
				// equal the compartment's current capacity x temperature
				// within the declared tolerance.
				auto const startingEnergyKJ = compartment.authored.thermalCapacityKJPerK
					* compartment.authored.startingTemperatureC;
				auto const thermalBound = StationBalanceTolerance
					* std::max(1.0, std::abs(startingEnergyKJ));
				auto const thermalResidual = startingEnergyKJ + compartment.occupantHeatKJ
					+ compartment.crewHeatKJ
					+ compartment.latentHeatKJ + compartment.exchangedKJ
					- compartment.authored.thermalCapacityKJPerK * compartment.temperatureC;
				if (!std::isfinite(thermalResidual) || std::abs(thermalResidual) > thermalBound)
					return violation = where + "thermal energy ledger residual "
						+ std::to_string(thermalResidual) + " exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
			}
			for (auto const& cycle : airlockCycles)
			{
				auto const where = "airlock cycle '" + cycle.authored.id + "' ";
				if (!requireFinite(cycle.lastRemovedKg, where + "last removed mass")) return false;
				if (!requireFinite(cycle.removedTotal, where + "cumulative removed mass")) return false;
				if (!requireFinite(cycle.recoveredTotal, where + "cumulative recovered mass")) return false;
				if (!requireFinite(cycle.lostTotal, where + "cumulative lost mass")) return false;
				auto const bound = StationBalanceTolerance
					* std::max(1.0, std::abs(cycle.removedTotal));
				if (std::abs(cycle.recoveredTotal + cycle.lostTotal - cycle.removedTotal) > bound)
					return violation = where + "recovered/lost totals do not close over the removed mass", false;
			}
			// Crew metabolism ledger (#401): the crew's own account must
			// close over its pathways - consumed (food + water + oxygen)
			// equals returned (CO2 + moisture + urine + organic waste)
			// plus retained body mass within the declared tolerance - and
			// the retained mass may never be negative: the crew cannot
			// emit more mass than it actually consumed. The metabolic
			// scale stays within [0, 1] and every published magnitude
			// stays finite. Store- and constituent-level closure of the
			// crew's draws and returns is enforced by the unit and
			// constituent boundary audits the crew books into; this
			// closes the crew's own consumption/return/retained account
			// so no metabolic mass is unexplained.
			system = "Crew metabolism";
			for (auto const& crew : crews)
			{
				auto const where = "crew '" + crew.authored.id + "' ";
				if (!requireFinite(crew.lastActivityLevel, where + "activity level")) return false;
				if (!requireFinite(crew.lastScale, where + "metabolic scale")) return false;
				if (!requireFinite(crew.lastFoodRequested, where + "last food requested")) return false;
				if (!requireFinite(crew.lastFoodActual, where + "last food consumed")) return false;
				if (!requireFinite(crew.lastWaterRequested, where + "last water requested")) return false;
				if (!requireFinite(crew.lastWaterActual, where + "last water consumed")) return false;
				if (!requireFinite(crew.lastOxygenRequestedKg, where + "last oxygen requested")) return false;
				if (!requireFinite(crew.lastOxygenActualKg, where + "last oxygen consumed")) return false;
				if (!requireFinite(crew.lastCo2ProducedKg, where + "last CO2 produced")) return false;
				if (!requireFinite(crew.lastMoistureProducedKg, where + "last moisture produced")) return false;
				if (!requireFinite(crew.lastUrineRequested, where + "last urine requested")) return false;
				if (!requireFinite(crew.lastUrineActual, where + "last urine returned")) return false;
				if (!requireFinite(crew.lastOrganicWasteRequested, where + "last organic waste requested")) return false;
				if (!requireFinite(crew.lastOrganicWasteActual, where + "last organic waste returned")) return false;
				if (!requireFinite(crew.lastHeatKW, where + "last metabolic heat")) return false;
				if (!requireFinite(crew.foodConsumed, where + "cumulative food consumed")) return false;
				if (!requireFinite(crew.waterConsumed, where + "cumulative water consumed")) return false;
				if (!requireFinite(crew.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
				if (!requireFinite(crew.co2ProducedKg, where + "cumulative CO2 produced")) return false;
				if (!requireFinite(crew.moistureProducedKg, where + "cumulative moisture produced")) return false;
				if (!requireFinite(crew.urineProduced, where + "cumulative urine returned")) return false;
				if (!requireFinite(crew.organicWasteProduced, where + "cumulative organic waste returned")) return false;
				if (!requireFinite(crew.heatKJ, where + "cumulative metabolic heat")) return false;
				if (!requireFinite(crew.retainedKg, where + "cumulative retained body mass")) return false;
				if (!(crew.lastScale >= 0.0 && crew.lastScale <= 1.0 + 1e-12))
					return violation = where + "metabolic scale left the declared [0, 1] range", false;
				auto const consumedKg = massToKg(stores[crew.foodStoreIndex].authored.unit, crew.foodConsumed)
					+ massToKg(stores[crew.waterStoreIndex].authored.unit, crew.waterConsumed)
					+ crew.oxygenConsumedKg;
				auto const returnedKg = massToKg(stores[crew.urineStoreIndex].authored.unit, crew.urineProduced)
					+ massToKg(stores[crew.organicWasteStoreIndex].authored.unit, crew.organicWasteProduced)
					+ crew.co2ProducedKg + crew.moistureProducedKg;
				auto const bound = StationBalanceTolerance * std::max(1.0, consumedKg);
				if (crew.retainedKg < -bound)
					return violation = where + "retained body mass is negative: the crew emitted more mass than it consumed", false;
				auto const closure = crew.retainedKg - (consumedKg - returnedKg);
				if (!std::isfinite(closure) || std::abs(closure) > bound)
					return violation = where + "consumed/returned/retained ledger does not close within tolerance", false;
			}
			// Water recovery ledgers (#402): the collector may only take what
			// the compartment held above its target, and the treatment's own
			// account closes twice over - processed mass leaves the machine as
			// treated water plus concentrate and nothing else, and every
			// treated kilogram was either qualified or held, never both and
			// never neither. The treatment is never supplied more energy than
			// the process owed, and held output always carries a stated reason.
			system = "Water recovery";
			for (auto const& collector : waterCollectors)
			{
				auto const where = "water collector '" + collector.authored.id + "' ";
				if (!requireFinite(collector.lastRequestedKg, where + "last requested mass")) return false;
				if (!requireFinite(collector.lastCondensableKg, where + "last condensable mass")) return false;
				if (!requireFinite(collector.lastCollectedKg, where + "last collected mass")) return false;
				if (!requireFinite(collector.lastLatentHeatKW, where + "last latent heat")) return false;
				if (!requireFinite(collector.lastCirculationFraction, where + "last circulation fraction")) return false;
				if (!requireFinite(collector.collectedTotalKg, where + "cumulative collected mass")) return false;
				if (!requireFinite(collector.latentHeatRoutedKJ, where + "cumulative latent heat routed")) return false;
				auto const collectBound = StationBalanceTolerance * std::max(1.0, collector.lastCondensableKg);
				if (collector.lastCollectedKg < -collectBound)
					return violation = where + "collected a negative mass", false;
				if (collector.lastCollectedKg > collector.lastCondensableKg + collectBound)
					return violation = where
						+ "collected more condensate than the compartment held above its humidity target", false;
			}
			for (auto const& treatment : waterTreatments)
			{
				auto const where = "water treatment '" + treatment.authored.id + "' ";
				if (!requireFinite(treatment.lastScale, where + "processing scale")) return false;
				if (!requireFinite(treatment.lastProcessedKg, where + "last processed mass")) return false;
				if (!requireFinite(treatment.lastTreatedKg, where + "last treated mass")) return false;
				if (!requireFinite(treatment.lastConcentrateKg, where + "last concentrate mass")) return false;
				if (!requireFinite(treatment.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(treatment.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(treatment.lastConsumableRequested, where + "last consumable requested")) return false;
				if (!requireFinite(treatment.lastConsumableUsed, where + "last consumable used")) return false;
				if (!requireFinite(treatment.processedTotalKg, where + "cumulative processed mass")) return false;
				if (!requireFinite(treatment.treatedTotalKg, where + "cumulative treated mass")) return false;
				if (!requireFinite(treatment.acceptedTotalKg, where + "cumulative accepted mass")) return false;
				if (!requireFinite(treatment.heldTotalKg, where + "cumulative held mass")) return false;
				if (!requireFinite(treatment.concentrateTotalKg, where + "cumulative concentrate mass")) return false;
				if (!requireFinite(treatment.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(treatment.energyOwedKJ, where + "cumulative energy owed")) return false;
				if (!requireFinite(treatment.consumablesUsedTotal, where + "cumulative consumables used")) return false;
				if (!(treatment.lastScale >= 0.0 && treatment.lastScale <= 1.0 + 1e-12))
					return violation = where + "processing scale left the declared [0, 1] range", false;
				auto const splitBound = StationBalanceTolerance * std::max(1.0, treatment.processedTotalKg);
				auto const splitResidual = treatment.processedTotalKg
					- treatment.treatedTotalKg - treatment.concentrateTotalKg;
				if (!std::isfinite(splitResidual) || std::abs(splitResidual) > splitBound)
					return violation = where
						+ "processed/treated/concentrate ledger does not close within tolerance", false;
				auto const qualityResidual = treatment.treatedTotalKg
					- treatment.acceptedTotalKg - treatment.heldTotalKg;
				if (!std::isfinite(qualityResidual) || std::abs(qualityResidual) > splitBound)
					return violation = where
						+ "accepted/held quality ledger does not close within tolerance", false;
				if (treatment.energySuppliedKJ > treatment.energyOwedKJ + splitBound)
					return violation = where + "was supplied more energy than the process owed", false;
				auto const consumableBound = StationBalanceTolerance
					* std::max(1.0, treatment.lastConsumableRequested);
				if (treatment.lastConsumableUsed > treatment.lastConsumableRequested + consumableBound)
					return violation = where + "used more consumable than the processed mass calls for", false;
				if (!treatment.lastQualityAccepted && treatment.lastProcessedKg > 0.0
					&& treatment.lastHoldReason.empty())
					return violation = where + "held treated output without a stated hold reason", false;
			}
			// The oxygen-production audit (#404): the split closes on mass -
			// every kilogram split is oxygen plus hydrogen, never a third
			// destination - and on energy: the electrical dose either ended
			// up bound in the produced hydrogen or routed into the sink as
			// process heat. A cell is never supplied less chemical energy
			// than it booked, and never splits more than the energy it was
			// paid for can make.
			system = "Electrolysis";
			for (auto const& cell : electrolyzers)
			{
				auto const where = "electrolyzer '" + cell.authored.id + "' ";
				if (!requireFinite(cell.lastScale, where + "split scale")) return false;
				if (!requireFinite(cell.lastRequestedKg, where + "last requested split")) return false;
				if (!requireFinite(cell.lastSplitKg, where + "last split mass")) return false;
				if (!requireFinite(cell.lastOxygenKg, where + "last oxygen mass")) return false;
				if (!requireFinite(cell.lastHydrogenKg, where + "last hydrogen mass")) return false;
				if (!requireFinite(cell.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(cell.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(cell.lastUnmetProductionKg, where + "last unmet production")) return false;
				if (!requireFinite(cell.waterSplitKg, where + "cumulative water split")) return false;
				if (!requireFinite(cell.oxygenProducedKg, where + "cumulative oxygen produced")) return false;
				if (!requireFinite(cell.hydrogenProducedKg, where + "cumulative hydrogen produced")) return false;
				if (!requireFinite(cell.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(cell.chemicalEnergyKJ, where + "cumulative chemical energy")) return false;
				if (!requireFinite(cell.heatRoutedKJ, where + "cumulative heat routed")) return false;
				if (!(cell.lastScale >= 0.0 && cell.lastScale <= 1.0 + 1e-12))
					return violation = where + "split scale left the declared [0, 1] range", false;
				auto const massBound = StationBalanceTolerance * std::max(1.0, cell.waterSplitKg);
				auto const massResidual = cell.waterSplitKg
					- cell.oxygenProducedKg - cell.hydrogenProducedKg;
				if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
					return violation = where
						+ "water/oxygen/hydrogen split does not close within tolerance", false;
				auto const energyResidual = cell.energySuppliedKJ
					- cell.chemicalEnergyKJ - cell.heatRoutedKJ;
				if (!std::isfinite(energyResidual) || std::abs(energyResidual) > massBound)
					return violation = where
						+ "supplied/chemical/heat energy ledger does not close within tolerance", false;
				if (cell.chemicalEnergyKJ > cell.energySuppliedKJ + massBound)
					return violation = where
						+ "booked more chemical energy than the electrical dose supplied", false;
				if (cell.lastSplitKg > cell.lastRequestedKg + massBound)
					return violation = where + "split more than the rated capacity allows", false;
			}
			// The CO2-capture audit (#404): captured mass never exceeds what
			// the compartment held, the store could take, the consumable
			// could sustain or the rated rate allows, and the consumable is
			// never drawn beyond what the captured mass calls for. A tick
			// with no electrical supply captures nothing.
			system = "CO2 capture";
			for (auto const& capture : co2Captures)
			{
				auto const where = "CO2 capture machine '" + capture.authored.id + "' ";
				if (!requireFinite(capture.lastScale, where + "capture scale")) return false;
				if (!requireFinite(capture.lastRequestedKg, where + "last requested capture")) return false;
				if (!requireFinite(capture.lastCapturedKg, where + "last captured mass")) return false;
				if (!requireFinite(capture.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(capture.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(capture.lastConsumableRequested, where + "last consumable requested")) return false;
				if (!requireFinite(capture.lastConsumableUsed, where + "last consumable used")) return false;
				if (!requireFinite(capture.capturedTotalKg, where + "cumulative captured mass")) return false;
				if (!requireFinite(capture.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(capture.consumablesUsedTotal, where + "cumulative consumables used")) return false;
				if (!(capture.lastScale >= 0.0 && capture.lastScale <= 1.0 + 1e-12))
					return violation = where + "capture scale left the declared [0, 1] range", false;
				auto const bound = StationBalanceTolerance * std::max(1.0, capture.lastRequestedKg);
				if (capture.lastCapturedKg > capture.lastRequestedKg + bound)
					return violation = where + "captured more than the rated capacity allows", false;
				auto const consumableBound = StationBalanceTolerance
					* std::max(1.0, capture.lastConsumableRequested);
				if (capture.lastConsumableUsed > capture.lastConsumableRequested + consumableBound)
					return violation = where + "used more consumable than the captured mass calls for", false;
				if (capture.lastPowerSuppliedKW <= 1e-12 && capture.lastCapturedKg > bound)
					return violation = where + "captured without any electrical supply", false;
			}
			// The fuel-cell backup audit (#406): the reaction closes on mass
			// - hydrogen + oxygen = water - and on energy - chemical =
			// electrical + waste. A cell never delivers electrical energy
			// it has not burned reactants for, never delivers more than
			// its installed rating, and never consumes reactants that
			// vanish: every gram lands in the product water store.
			system = "Fuel-cell backup";
			for (auto const& cell : fuelCells)
			{
				auto const where = "fuel cell '" + cell.authored.id + "' ";
				if (!requireFinite(cell.lastOutputKW, where + "last output power")) return false;
				if (!requireFinite(cell.lastHydrogenKg, where + "last hydrogen consumed")) return false;
				if (!requireFinite(cell.lastOxygenKg, where + "last oxygen consumed")) return false;
				if (!requireFinite(cell.lastWaterKg, where + "last water produced")) return false;
				if (!requireFinite(cell.lastWasteHeatKW, where + "last waste heat")) return false;
				if (!requireFinite(cell.lastUnmetOutputKW, where + "last unmet output")) return false;
				if (!requireFinite(cell.hydrogenConsumedKg, where + "cumulative hydrogen consumed")) return false;
				if (!requireFinite(cell.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
				if (!requireFinite(cell.waterProducedKg, where + "cumulative water produced")) return false;
				if (!requireFinite(cell.chemicalEnergyKJ, where + "cumulative chemical energy")) return false;
				if (!requireFinite(cell.electricalKJ, where + "cumulative electrical energy")) return false;
				if (!requireFinite(cell.wasteHeatKJ, where + "cumulative waste heat")) return false;
				if (!requireFinite(cell.enduranceSeconds, where + "backup endurance")) return false;
				auto const massBound = StationBalanceTolerance * std::max(1.0, cell.waterProducedKg);
				auto const massResidual = cell.hydrogenConsumedKg + cell.oxygenConsumedKg
					- cell.waterProducedKg;
				if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
					return violation = where
						+ "hydrogen/oxygen/water reaction ledger does not close within tolerance", false;
				auto const energyResidual = cell.chemicalEnergyKJ
					- cell.electricalKJ - cell.wasteHeatKJ;
				if (!std::isfinite(energyResidual) || std::abs(energyResidual) > massBound)
					return violation = where
						+ "chemical/electrical/waste energy ledger does not close within tolerance", false;
				if (cell.electricalKJ > cell.chemicalEnergyKJ + massBound)
					return violation = where
						+ "delivered more electrical energy than the reactants' chemical energy", false;
				if (cell.lastOutputKW > cell.authored.ratedOutputKW + StationBalanceTolerance)
					return violation = where + "delivered above the installed electrical rating", false;
				if (cell.lastOutputKW > 1e-9 && cell.lastHydrogenKg <= 1e-12)
					return violation = where + "delivered power without consuming any hydrogen", false;
			}
			// Carbon recovery audits (#405): every reported magnitude is
			// finite, the Sabatier mass ledger closes over CO2 + hydrogen =
			// methane + water, its energy ledger closes over supplied +
			// released = routed, and the methane split closes on mass with
			// the energy ledger closing over supplied = absorbed + routed.
			system = "Sabatier reactor";
			for (auto const& reactor : sabatierReactors)
			{
				auto const where = "Sabatier reactor '" + reactor.authored.id + "' ";
				if (!requireFinite(reactor.lastScale, where + "conversion scale")) return false;
				if (!requireFinite(reactor.lastRequestedKg, where + "last requested conversion")) return false;
				if (!requireFinite(reactor.lastCo2ConsumedKg, where + "last CO2 consumed")) return false;
				if (!requireFinite(reactor.lastHydrogenConsumedKg, where + "last hydrogen consumed")) return false;
				if (!requireFinite(reactor.lastMethaneProducedKg, where + "last methane produced")) return false;
				if (!requireFinite(reactor.lastWaterProducedKg, where + "last water produced")) return false;
				if (!requireFinite(reactor.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(reactor.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(reactor.lastUnmetConversionKg, where + "last unmet conversion")) return false;
				if (!requireFinite(reactor.lastReactionHeatKW, where + "last reaction heat")) return false;
				if (!requireFinite(reactor.co2ConsumedKg, where + "cumulative CO2 consumed")) return false;
				if (!requireFinite(reactor.hydrogenConsumedKg, where + "cumulative hydrogen consumed")) return false;
				if (!requireFinite(reactor.methaneProducedKg, where + "cumulative methane produced")) return false;
				if (!requireFinite(reactor.waterProducedKg, where + "cumulative water produced")) return false;
				if (!requireFinite(reactor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(reactor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
				if (!requireFinite(reactor.heatRoutedKJ, where + "cumulative heat routed")) return false;
				auto const massBound = StationBalanceTolerance * std::max(1.0,
					reactor.methaneProducedKg + reactor.waterProducedKg);
				auto const massResidual = reactor.co2ConsumedKg + reactor.hydrogenConsumedKg
					- reactor.methaneProducedKg - reactor.waterProducedKg;
				if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
					return violation = where
						+ "CO2/hydrogen/methane/water conversion ledger does not close within tolerance", false;
				auto const energyResidual = reactor.energySuppliedKJ + reactor.reactionHeatKJ
					- reactor.heatRoutedKJ;
				if (!std::isfinite(energyResidual) || std::abs(energyResidual) > massBound)
					return violation = where
						+ "supplied/released/routed energy ledger does not close within tolerance", false;
			}
			system = "Methane processing";
			for (auto const& processor : methaneProcessors)
			{
				auto const where = "methane processor '" + processor.authored.id + "' ";
				if (!requireFinite(processor.lastScale, where + "processing scale")) return false;
				if (!requireFinite(processor.lastRequestedKg, where + "last requested processing")) return false;
				if (!requireFinite(processor.lastMethaneConsumedKg, where + "last methane consumed")) return false;
				if (!requireFinite(processor.lastHydrogenReturnedKg, where + "last hydrogen returned")) return false;
				if (!requireFinite(processor.lastCarbonProducedKg, where + "last carbon produced")) return false;
				if (!requireFinite(processor.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(processor.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(processor.lastUnmetProcessingKg, where + "last unmet processing")) return false;
				if (!requireFinite(processor.lastReactionHeatKW, where + "last reaction heat")) return false;
				if (!requireFinite(processor.methaneConsumedKg, where + "cumulative methane consumed")) return false;
				if (!requireFinite(processor.hydrogenReturnedKg, where + "cumulative hydrogen returned")) return false;
				if (!requireFinite(processor.carbonProducedKg, where + "cumulative carbon produced")) return false;
				if (!requireFinite(processor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(processor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
				if (!requireFinite(processor.heatRoutedKJ, where + "cumulative heat routed")) return false;
				auto const massBound = StationBalanceTolerance * std::max(1.0,
					processor.hydrogenReturnedKg + processor.carbonProducedKg);
				auto const massResidual = processor.methaneConsumedKg
					- processor.hydrogenReturnedKg - processor.carbonProducedKg;
				if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
					return violation = where
						+ "methane/hydrogen/carbon split ledger does not close within tolerance", false;
				auto const energyResidual = processor.energySuppliedKJ
					- processor.reactionHeatKJ - processor.heatRoutedKJ;
				if (!std::isfinite(energyResidual) || std::abs(energyResidual) > massBound)
					return violation = where
						+ "supplied/absorbed/routed energy ledger does not close within tolerance", false;
				if (processor.reactionHeatKJ > processor.energySuppliedKJ + massBound)
					return violation = where
						+ "absorbed more reaction heat than the electrical dose supplied", false;
			}
			// Nutrient recovery audits (#407): every reported magnitude is
			// finite, the digestion closes on mass - feed + oxygen + process
			// water = nutrients + recovered water + CO2 + residue - with the
			// residue as the balance term, its energy ledger closes over
			// supplied + released = routed, the released heat never exceeds
			// the feedstock's declared chemical energy, and the recovered
			// per-constituent totals never exceed the feed's bound content -
			// the atmospheric nitrogen share is never recovered.
			system = "Microbial processing";
			for (auto const& processor : microbialProcessors)
			{
				auto const where = "microbial processor '" + processor.authored.id + "' ";
				if (!requireFinite(processor.lastScale, where + "processing scale")) return false;
				if (!requireFinite(processor.lastRequestedKg, where + "last requested processing")) return false;
				if (!requireFinite(processor.lastFeedConsumedKg, where + "last feed consumed")) return false;
				if (!requireFinite(processor.lastOxygenConsumedKg, where + "last oxygen consumed")) return false;
				if (!requireFinite(processor.lastWaterConsumedKg, where + "last process water consumed")) return false;
				if (!requireFinite(processor.lastConsumableRequested, where + "last consumable requested")) return false;
				if (!requireFinite(processor.lastConsumableUsed, where + "last consumable used")) return false;
				if (!requireFinite(processor.lastConsumableUnmet, where + "last consumable unmet")) return false;
				if (!requireFinite(processor.lastNutrientProducedKg, where + "last nutrients produced")) return false;
				if (!requireFinite(processor.lastRecoveredWaterKg, where + "last recovered water")) return false;
				if (!requireFinite(processor.lastCo2ProducedKg, where + "last CO2 produced")) return false;
				if (!requireFinite(processor.lastResidueProducedKg, where + "last residue produced")) return false;
				if (!requireFinite(processor.lastPowerDemandKW, where + "last power demand")) return false;
				if (!requireFinite(processor.lastPowerSuppliedKW, where + "last power supplied")) return false;
				if (!requireFinite(processor.lastUnmetProcessingKg, where + "last unmet processing")) return false;
				if (!requireFinite(processor.lastReactionHeatKW, where + "last reaction heat")) return false;
				if (!requireFinite(processor.feedConsumedKg, where + "cumulative feed consumed")) return false;
				if (!requireFinite(processor.oxygenConsumedKg, where + "cumulative oxygen consumed")) return false;
				if (!requireFinite(processor.waterConsumedKg, where + "cumulative process water consumed")) return false;
				if (!requireFinite(processor.consumablesUsed, where + "cumulative consumables used")) return false;
				if (!requireFinite(processor.nutrientProducedKg, where + "cumulative nutrients produced")) return false;
				if (!requireFinite(processor.recoveredWaterKg, where + "cumulative recovered water")) return false;
				if (!requireFinite(processor.co2ProducedKg, where + "cumulative CO2 produced")) return false;
				if (!requireFinite(processor.residueProducedKg, where + "cumulative residue produced")) return false;
				if (!requireFinite(processor.nitrogenRecoveredKg, where + "cumulative nitrogen recovered")) return false;
				if (!requireFinite(processor.phosphorusRecoveredKg, where + "cumulative phosphorus recovered")) return false;
				if (!requireFinite(processor.potassiumRecoveredKg, where + "cumulative potassium recovered")) return false;
				if (!requireFinite(processor.mineralsRecoveredKg, where + "cumulative minerals recovered")) return false;
				if (!requireFinite(processor.energySuppliedKJ, where + "cumulative energy supplied")) return false;
				if (!requireFinite(processor.reactionHeatKJ, where + "cumulative reaction heat")) return false;
				if (!requireFinite(processor.heatRoutedKJ, where + "cumulative heat routed")) return false;
				if (processor.lastScale < -StationBalanceTolerance
					|| processor.lastScale > 1.0 + StationBalanceTolerance)
					return violation = where
						+ "processing scale left the declared [0, 1] range", false;
				auto const massBound = StationBalanceTolerance * std::max(1.0,
					processor.nutrientProducedKg + processor.recoveredWaterKg
						+ processor.co2ProducedKg + processor.residueProducedKg);
				auto const massResidual = processor.feedConsumedKg + processor.oxygenConsumedKg
					+ processor.waterConsumedKg - processor.nutrientProducedKg
					- processor.recoveredWaterKg - processor.co2ProducedKg
					- processor.residueProducedKg;
				if (!std::isfinite(massResidual) || std::abs(massResidual) > massBound)
					return violation = where
						+ "feed/oxygen/water/nutrient/water/CO2/residue digestion ledger does not close within tolerance", false;
				auto const energyResidual = processor.energySuppliedKJ + processor.reactionHeatKJ
					- processor.heatRoutedKJ;
				if (!std::isfinite(energyResidual) || std::abs(energyResidual) > massBound)
					return violation = where
						+ "supplied/released/routed energy ledger does not close within tolerance", false;
				auto const& composition = *stores[processor.feedStoreIndex].authored.composition;
				auto const feedEnergyKJ = processor.feedConsumedKg
					* (1.0 - composition.moisture) * composition.energyKJPerKgDry;
				auto const energyCapBound = StationBalanceTolerance * std::max(1.0, std::abs(feedEnergyKJ));
				if (processor.reactionHeatKJ > feedEnergyKJ + energyCapBound)
					return violation = where
						+ "released more reaction heat than the consumed feedstock's declared chemical energy", false;
				auto const boundNutrientKJ = processor.feedConsumedKg
					* (1.0 - composition.moisture) * composition.nutrientNitrogen;
				auto const nutrientCapBound = StationBalanceTolerance * std::max(1.0, std::abs(boundNutrientKJ));
				if (processor.nitrogenRecoveredKg > boundNutrientKJ + nutrientCapBound)
					return violation = where
						+ "recovered more nitrogen than the consumed feed's bound nutrient nitrogen carries", false;
			}
			system = "Power allocation";
			for (auto const& array : solarArrays)
			{
				auto const where = "solar array '" + array.authored.id + "' ";
				if (!requireFinite(array.lastIllumination, where + "evaluated illumination")) return false;
				if (!requireFinite(array.lastAvailableKW, where + "available generation")) return false;
			}
			for (auto const& load : loads)
			{
				auto const where = "equipment load '" + load.authored.id + "' ";
				if (!requireFinite(load.lastProfileLevel, where + "operating profile level")) return false;
				if (!requireFinite(load.lastRequestedKW, where + "requested power")) return false;
				if (!requireFinite(load.lastSuppliedKW, where + "supplied power")) return false;
				if (!requireFinite(load.lastUnmetKW, where + "unmet power")) return false;
				if (!requireFinite(load.lastUnmetCriticalKW, where + "unmet critical power")) return false;
				if (!requireFinite(load.lastDeliveredFraction, where + "delivered fraction")) return false;
				if (!requireFinite(load.lastMaterialFraction, where + "material-feasible fraction")) return false;
				if (!requireFinite(load.lastCappedRequestKW, where + "material-capped request")) return false;
				if (!requireFinite(load.lastHeatExportedKW, where + "exported heat")) return false;
				if (!requireFinite(load.lastUnmetCoolingKW, where + "unmet cooling")) return false;
				for (auto const& exchange : load.exchanges)
				{
					if (!requireFinite(exchange.lastRequested, where + "exchange requested amount")) return false;
					if (!requireFinite(exchange.lastCompleted, where + "exchange completed amount")) return false;
					if (!requireFinite(exchange.lastUnmet, where + "exchange unmet amount")) return false;
				}
			}
			for (auto const& battery : batteries)
			{
				auto const where = "battery '" + battery.authored.id + "' ";
				if (!requireFinite(battery.stored, where + "stored energy")) return false;
				if (!requireFinite(battery.lastChargeKW, where + "charge power")) return false;
				if (!requireFinite(battery.lastDischargeKW, where + "discharge power")) return false;
				if (!requireFinite(battery.lastDischargeRequestedKW, where + "discharge request")) return false;
				if (!requireFinite(battery.lastDischargeShortfallKW, where + "discharge shortfall")) return false;
				if (!requireFinite(battery.chargedKWh, where + "cumulative charged energy")) return false;
				if (!requireFinite(battery.dischargedKWh, where + "cumulative discharged energy")) return false;
				if (!requireFinite(battery.deliveredKWh, where + "cumulative delivered energy")) return false;
				if (!requireFinite(battery.chargeInputKWh, where + "cumulative charge input energy")) return false;
			}
			if (!requireFinite(availableGeneration, "surplus generation pool")) return false;
			if (!requireFinite(batteryShortfallKW, "battery shortfall")) return false;
			// The snapshot publishes station-wide totals summed from the
			// per-item results; audit the same sums so an aggregate can
			// never overflow past the per-item checks.
			{
				double totalRequested = 0.0, totalSupplied = 0.0, totalUnmetCritical = 0.0;
				double totalHeat = 0.0, totalGeneration = 0.0, totalDischarge = 0.0, totalCharge = 0.0;
				for (auto const& load : loads)
				{
					totalRequested += load.lastRequestedKW;
					totalSupplied += load.lastSuppliedKW;
					totalUnmetCritical += load.lastUnmetCriticalKW;
					totalHeat += load.lastHeatExportedKW;
				}
				for (auto const& array : solarArrays) totalGeneration += array.lastAvailableKW;
				for (auto const& battery : batteries)
				{
					totalDischarge += battery.lastDischargeKW;
					totalCharge += battery.lastChargeKW;
				}
				if (!requireFinite(totalRequested, "total requested power")) return false;
				if (!requireFinite(totalSupplied, "total supplied power")) return false;
				if (!requireFinite(totalUnmetCritical, "total unmet critical power")) return false;
				if (!requireFinite(totalHeat, "total exported heat")) return false;
				if (!requireFinite(totalGeneration, "total available generation")) return false;
				if (!requireFinite(totalDischarge, "total battery discharge power")) return false;
				if (!requireFinite(totalCharge, "total battery charge power")) return false;
				if (!requireFinite(totalCoolantPumpKW, "total coolant pump power")) return false;
				if (!requireFinite(totalCoolantHeaterKW, "total freeze-protection heating power")) return false;
				if (!requireFinite(totalChillerPowerKW, "total chiller power")) return false;
				if (!requireFinite(totalWaterTreatmentPowerKW, "total water treatment power")) return false;
				if (!requireFinite(totalGeneratorOutputKW, "total generator output power")) return false;
				if (!requireFinite(totalGeneratorStartupKW, "total generator startup draw")) return false;
				if (!requireFinite(totalGeneratorWasteKW, "total generator waste heat")) return false;
				if (!requireFinite(totalGeneratorDecayKW, "total generator decay heat")) return false;
				if (!requireFinite(totalFuelCellOutputKW, "total fuel-cell backup output")) return false;
				if (!requireFinite(totalFuelCellWasteKW, "total fuel-cell backup waste heat")) return false;
				if (!requireFinite(totalSabatierPowerKW, "total Sabatier reactor power")) return false;
				if (!requireFinite(totalMethaneProcessingPowerKW, "total methane processing power")) return false;
				if (!requireFinite(totalMicrobialProcessingPowerKW, "total microbial processing power")) return false;
			}
			for (size_t index = 0; index < stores.size(); ++index)
			{
				stores[index].currentQualified = qualified[index];
				stores[index].currentUnqualified = unqualified[index];
				stores[index].lastTickIn = flowIn[index];
				stores[index].lastTickOut = flowOut[index];
			}
			constituentLoss = stagedLoss;
			unitLoss = stagedUnitLoss;
			unitDelivered = stagedUnitDelivered;
			unitCollected = stagedUnitCollected;
			constituentDelivered = stagedConstituentDelivered;
			// Rejections and partial completions are visible: announce each
			// status transition (after the commit) so the event log records
			// where material stalled without flooding every tick.
			for (auto const index : transferOrder)
			{
				auto const& transfer = transfers[index];
				if (transfer.status == previousStatus[index]) continue;
				if (transfer.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Transfer '" + transfer.authored.id + "' supplying fully");
					continue;
				}
				auto const& reason = stallReason[index];
				auto const message = transfer.status == StationTransferStatus::Rejected
					? std::string("Transfer '") + transfer.authored.id + "' rejected: " + reason
					: std::string("Transfer '") + transfer.authored.id + "' partial: " + reason;
				appendBoundedEvent(events, tick, message);
			}
			for (auto const index : conversionOrder)
			{
				auto const& conversion = conversions[index];
				if (conversion.status == previousConversionStatus[index]) continue;
				if (conversion.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Conversion '" + conversion.authored.id
						+ "' running at full throughput");
					continue;
				}
				auto const message = conversion.status == StationTransferStatus::Rejected
					? std::string("Conversion '") + conversion.authored.id + "' rejected: " + conversion.stallReason
					: std::string("Conversion '") + conversion.authored.id + "' partial: " + conversion.stallReason;
				appendBoundedEvent(events, tick, message);
			}
			// Solar and external load transitions are announced so the event
			// log records eclipse changes, curtailment and unmet critical
			// floors without flooding every tick.
			for (auto const index : loadOrder)
			{
				auto const& load = loads[index];
				if (load.status != previousLoadStatus[index])
				{
					if (load.status == StationTransferStatus::Full)
						appendBoundedEvent(events, tick, "Load '" + load.authored.id + "' supplying fully");
					else if (load.status == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Load '" + load.authored.id + "' rejected: "
							+ (!load.lastAdmitted ? load.lastBlockReason
								: (load.lastShortfallReason.empty()
									? std::string("electrical supply is unavailable")
									: load.lastShortfallReason)));
					else
						appendBoundedEvent(events, tick, "Load '" + load.authored.id + "' curtailed: supplied "
							+ std::to_string(load.lastSuppliedKW) + " of "
							+ std::to_string(load.lastRequestedKW) + " kW");
				}
				if (load.lastCriticalUnmet != previousCriticalUnmet[index])
					appendBoundedEvent(events, tick, load.lastCriticalUnmet
						? "Load '" + load.authored.id + "' critical floor unmet: short "
							+ std::to_string(load.lastUnmetCriticalKW) + " kW"
						: std::string("Load '") + load.authored.id + "' critical floor restored");
			}
			for (size_t index = 0; index < solarArrays.size(); ++index)
			{
				auto const& array = solarArrays[index];
				if (array.lastEclipse == previousEclipse[index] || array.lastDisabled) continue;
				appendBoundedEvent(events, tick, array.lastEclipse
					? "Solar array '" + array.authored.id + "' entered eclipse"
					: "Solar array '" + array.authored.id + "' returned to sunlight");
			}
			// Battery transitions are announced so the event log records
			// exhaustion, saturation and discharge limitation without
			// flooding every tick.
			for (size_t index = 0; index < batteries.size(); ++index)
			{
				auto const& battery = batteries[index];
				if (battery.authored.usableKWh <= 0.0) continue;
				if (battery.status != previousBatteryStatus[index])
				{
					if (battery.status == StationTransferStatus::Full)
						appendBoundedEvent(events, tick, "Battery '" + battery.authored.id
							+ "' discharging to cover the deficit");
					else if (battery.status == StationTransferStatus::Partial)
						appendBoundedEvent(events, tick, "Battery '" + battery.authored.id
							+ "' discharge limited: supplied " + std::to_string(battery.lastDischargeKW)
							+ " of " + std::to_string(battery.lastDischargeRequestedKW) + " kW");
					else if (battery.status == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Battery '" + battery.authored.id
							+ "' cannot discharge: " + (battery.lastAtReserve
								? std::string("usable energy is at the reserve floor")
								: std::string("no discharge power capability is available")));
				}
				if (battery.lastAtReserve != previousBatteryReserve[index])
					appendBoundedEvent(events, tick, battery.lastAtReserve
						? "Battery '" + battery.authored.id
							+ "' exhausted: usable energy depleted to the reserve floor"
						: "Battery '" + battery.authored.id + "' recovered above the reserve floor");
				if (battery.lastSaturated != previousBatterySaturated[index])
					appendBoundedEvent(events, tick, battery.lastSaturated
						? "Battery '" + battery.authored.id
							+ "' fully charged: stored energy reached nameplate capacity"
						: "Battery '" + battery.authored.id + "' left full charge");
			}
			// Thermal limit crossings are announced so the event log
			// records overheats and overcools without flooding every tick
			// (#395).
			for (size_t index = 0; index < reservoirs.size(); ++index)
			{
				auto const& reservoir = reservoirs[index];
				if (reservoir.lastAboveLimit != previousAboveLimit[index])
					appendBoundedEvent(events, tick, reservoir.lastAboveLimit
						? "Thermal reservoir '" + reservoir.authored.id
							+ "' above its declared maximum: " + std::to_string(reservoir.temperatureC)
							+ " > " + std::to_string(reservoir.authored.maxTemperatureC) + " C"
						: std::string("Thermal reservoir '") + reservoir.authored.id
							+ "' back within its maximum limit");
				if (reservoir.lastBelowLimit != previousBelowLimit[index])
					appendBoundedEvent(events, tick, reservoir.lastBelowLimit
						? "Thermal reservoir '" + reservoir.authored.id
							+ "' below its declared minimum: " + std::to_string(reservoir.temperatureC)
							+ " < " + std::to_string(reservoir.authored.minTemperatureC) + " C"
						: std::string("Thermal reservoir '") + reservoir.authored.id
							+ "' back above its minimum limit");
			}
			// Coolant loop transitions are announced so the event log
			// records circulation loss and freeze exposure without flooding
			// every tick (#396): a loop that stops circulating, a loop that
			// falls below its freeze-protection setpoint with heating it
			// cannot get, and recovery from both.
			for (size_t index = 0; index < coolantLoops.size(); ++index)
			{
				auto const& loop = coolantLoops[index];
				if (loop.status != previousLoopStatus[index])
				{
					if (loop.status == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Coolant loop '" + loop.authored.id
							+ "' service refused: no electrical supply is available for its pump or heater");
					else if (loop.status == StationTransferStatus::Full
						&& previousLoopStatus[index] == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Coolant loop '" + loop.authored.id
							+ "' service restored: pump and freeze heater fully supplied");
				}
				if (loop.lastCirculating != previousCirculating[index])
					appendBoundedEvent(events, tick, loop.lastCirculating
						? "Coolant loop '" + loop.authored.id + "' circulating: "
							+ std::to_string(loop.lastFlowKgPerS) + " kg/s of coolant flowing"
						: "Coolant loop '" + loop.authored.id
							+ "' circulation lost: no coolant flow is being delivered");
				if (loop.lastBelowFreeze != previousBelowFreeze[index])
					appendBoundedEvent(events, tick, loop.lastBelowFreeze
						? "Coolant loop '" + loop.authored.id + "' below its freeze-protection setpoint: "
							+ std::to_string(loop.temperatureC) + " < "
							+ std::to_string(loop.authored.freezeProtectionSetpointC)
							+ " C (heater supplied " + std::to_string(loop.lastHeaterSuppliedKW)
							+ " of " + std::to_string(loop.lastHeaterDemandKW) + " kW)"
						: std::string("Coolant loop '") + loop.authored.id
							+ "' back above its freeze-protection setpoint");
			}
			// Chiller transitions are announced so the event log records
			// refusals, curtailment, the chilled-setpoint gate and the
			// condenser sink temperature interface without flooding every
			// tick (#397).
			for (size_t index = 0; index < chillers.size(); ++index)
			{
				auto const& chiller = chillers[index];
				if (chiller.status != previousChillerStatus[index])
				{
					if (chiller.status == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Chiller '" + chiller.authored.id
							+ "' service refused: " + (chiller.lastBlockReason.empty()
								? std::string("no electrical supply is available for its work or auxiliaries")
								: chiller.lastBlockReason));
					else if (chiller.status == StationTransferStatus::Full
						&& previousChillerStatus[index] == StationTransferStatus::Rejected)
						appendBoundedEvent(events, tick, "Chiller '" + chiller.authored.id
							+ "' service restored: cold service supplied in full");
					else if (chiller.status == StationTransferStatus::Partial)
						appendBoundedEvent(events, tick, "Chiller '" + chiller.authored.id
							+ "' curtailed: supplied " + std::to_string(chiller.lastCoolingKW)
							+ " of " + std::to_string(chiller.lastCoolingDemandKW) + " kW cold service");
				}
				if (chiller.lastAtSetpoint != previousChillerAtSetpoint[index])
					appendBoundedEvent(events, tick, chiller.lastAtSetpoint
						? "Chiller '" + chiller.authored.id
							+ "' cold side at its chilled setpoint: no extraction demanded"
						: std::string("Chiller '") + chiller.authored.id
							+ "' cold side above its chilled setpoint: cooling demand returned");
				if (chiller.lastSinkOver != previousChillerSinkOver[index])
					appendBoundedEvent(events, tick, chiller.lastSinkOver
						? "Chiller '" + chiller.authored.id
							+ "' condenser sink over its maximum condensing temperature: "
							+ std::to_string(reservoirs[chiller.sinkIndex].temperatureC) + " >= "
							+ std::to_string(chiller.authored.maxCondensingTemperatureC)
							+ " C, cold service refused"
						: std::string("Chiller '") + chiller.authored.id
							+ "' condenser sink back below its maximum condensing temperature");
			}
			// Generator phase transitions are announced so the event log
			// records starts, failed starts, trips, fuel depletion and the
			// end of the decay period without flooding every tick (#399).
			for (size_t index = 0; index < generators.size(); ++index)
			{
				auto const& generator = generators[index];
				auto const previous = previousGeneratorPhase[index];
				if (generator.phase == previous) continue;
				switch (generator.phase)
				{
				case StationGeneratorPhase::Stopped:
					if (previous == StationGeneratorPhase::Starting)
						appendBoundedEvent(events, tick, "Generator '" + generator.authored.id
							+ " startup failed: " + (generator.lastBlockReason.empty()
								? std::string("the start could not be completed")
								: generator.lastBlockReason));
					else if (previous == StationGeneratorPhase::Shutdown)
						appendBoundedEvent(events, tick, "Generator '" + generator.authored.id
							+ " shutdown complete: decay heat dissipated after "
							+ std::to_string(generator.authored.shutdownSeconds) + " s");
					break;
				case StationGeneratorPhase::Starting:
					appendBoundedEvent(events, tick, "Generator '" + generator.authored.id
						+ " starting: cranking at " + std::to_string(generator.authored.startupPowerKW)
						+ " kW for " + std::to_string(generator.authored.startupSeconds) + " s");
					break;
				case StationGeneratorPhase::Running:
					appendBoundedEvent(events, tick, "Generator '" + generator.authored.id
						+ " startup complete: running, commanded "
						+ std::to_string(generator.authored.commandedOutputKW) + " kW");
					break;
				case StationGeneratorPhase::Shutdown:
					appendBoundedEvent(events, tick, "Generator '" + generator.authored.id
						+ " shutting down: " + (generator.lastBlockReason.empty()
							? std::string("commanded off") : generator.lastBlockReason)
						+ "; shutdown heat " + std::to_string(generator.authored.shutdownHeatKW)
						+ " kW for " + std::to_string(generator.authored.shutdownSeconds) + " s");
					break;
				}
			}
			// Compartment atmosphere transitions are announced so the event
			// log records pressure excursions, life-support shortfall,
			// air-quality exceedance and condensation onset without
			// flooding every tick (#400). The conditions are exposed, not
			// enforced: the compartment keeps integrating and the ledger
			// keeps closing while the crossing stands.
			for (size_t index = 0; index < atmospheres.size(); ++index)
			{
				auto const& compartment = atmospheres[index];
				if (compartment.lastAbovePressure != previousAbovePressure[index])
					appendBoundedEvent(events, tick, "Atmosphere '" + compartment.authored.id
						+ " above its declared maximum pressure: "
						+ std::to_string(compartment.authored.maxPressureKPa) + " kPa limit");
				if (compartment.lastBelowPressure != previousBelowPressure[index])
					appendBoundedEvent(events, tick, compartment.lastBelowPressure
						? "Atmosphere '" + compartment.authored.id
							+ " below its declared minimum pressure: "
							+ std::to_string(compartment.authored.minPressureKPa) + " kPa limit"
						: std::string("Atmosphere '") + compartment.authored.id
							+ " back above its minimum pressure limit");
				if (compartment.lastLowOxygen != previousLowOxygen[index])
					appendBoundedEvent(events, tick, compartment.lastLowOxygen
						? "Atmosphere '" + compartment.authored.id
							+ " oxygen partial pressure below its life-support minimum: "
							+ std::to_string(compartment.authored.minOxygenPartialPressureKPa)
							+ " kPa limit"
						: std::string("Atmosphere '") + compartment.authored.id
							+ " oxygen back above its life-support minimum");
				if (compartment.lastHighCo2 != previousHighCo2[index])
					appendBoundedEvent(events, tick, compartment.lastHighCo2
						? "Atmosphere '" + compartment.authored.id
							+ " carbon dioxide partial pressure above its air-quality maximum: "
							+ std::to_string(compartment.authored.maxCarbonDioxidePartialPressureKPa)
							+ " kPa limit"
						: std::string("Atmosphere '") + compartment.authored.id
							+ " carbon dioxide back below its air-quality maximum");
				if (compartment.lastCondensing != previousCondensing[index])
					appendBoundedEvent(events, tick, compartment.lastCondensing
						? "Atmosphere '" + compartment.authored.id
							+ " at saturation: excess vapour condensing, "
							+ std::to_string(compartment.lastVapourCondensedKg)
							+ " kg this tick releasing latent heat"
						: std::string("Atmosphere '") + compartment.authored.id
							+ " no longer condensing");
			}
			// Airlock cycle occurrences are discrete scenario events, so
			// every cycle is announced with its recovered-versus-lost
			// split: the vent is an accounted exchange, never an
			// unexplained disposal (#400).
			for (auto const& cycle : airlockCycles)
			{
				if (cycle.lastFiredTick != tick) continue;
				auto const label = "Airlock cycle '" + cycle.authored.id + "' at "
					+ std::to_string(cycle.lastOccurrenceSeconds) + " s: ";
				if (cycle.status == StationTransferStatus::Rejected)
					appendBoundedEvent(events, tick, label + "refused: compartment '"
						+ cycle.authored.atmosphereId + "' holds no gas to cycle");
				else if (cycle.status == StationTransferStatus::Partial)
					appendBoundedEvent(events, tick, label + "partially cycled "
						+ std::to_string(cycle.lastRemovedKg) + " of "
						+ std::to_string(cycle.authored.gasMassKg) + " kg from '"
						+ cycle.authored.atmosphereId + "' (recovered "
						+ std::to_string(cycle.lastRecoveredKg) + " kg, lost "
						+ std::to_string(cycle.lastLostKg) + " kg); compartment "
						"inventory could not supply the full cycle");
				else
					appendBoundedEvent(events, tick, label + "cycled "
						+ std::to_string(cycle.lastRemovedKg) + " kg from '"
						+ cycle.authored.atmosphereId + "' (recovered "
						+ std::to_string(cycle.lastRecoveredKg) + " kg, lost "
						+ std::to_string(cycle.lastLostKg) + " kg)");
			}
			// Crew supply transitions are announced so the event log
			// records when a crew's demand stops being fully met and
			// when it recovers, naming what limited the metabolism
			// (#401). The transition is reported once per change, not
			// every tick the condition stands.
			for (size_t index = 0; index < crews.size(); ++index)
			{
				auto const& crew = crews[index];
				if (crew.lastUnmet == previousCrewUnmet[index]) continue;
				appendBoundedEvent(events, tick, crew.lastUnmet
					? "Crew '" + crew.authored.id + "' metabolic demand unmet: "
						+ crew.lastLimiter
					: std::string("Crew '") + crew.authored.id
						+ "' metabolic demand fully supplied again");
			}
			// Oxygen production and CO2 capture transitions are announced so
			// the event log records when a cell stops producing and why -
			// cooling, feed, product headroom or power - and when it recovers
			// (#404). The transition is reported once per change, not every
			// tick the condition stands.
			for (size_t index = 0; index < electrolyzers.size(); ++index)
			{
				auto const& cell = electrolyzers[index];
				if (cell.status == previousElectrolyzerStatus[index]) continue;
				if (cell.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Electrolyzer '" + cell.authored.id
						+ "' splitting at full rate");
					continue;
				}
				auto const reason = !cell.lastBlockReason.empty() ? cell.lastBlockReason : cell.lastLimiter;
				appendBoundedEvent(events, tick, cell.status == StationTransferStatus::Rejected
					? "Electrolyzer '" + cell.authored.id + "' rejected: " + reason
					: "Electrolyzer '" + cell.authored.id + "' partial: " + reason);
			}
			for (size_t index = 0; index < co2Captures.size(); ++index)
			{
				auto const& capture = co2Captures[index];
				if (capture.status == previousCo2CaptureStatus[index]) continue;
				if (capture.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "CO2 capture '" + capture.authored.id
						+ "' capturing at full rate");
					continue;
				}
				auto const reason = !capture.lastBlockReason.empty()
					? capture.lastBlockReason : capture.lastLimiter;
				appendBoundedEvent(events, tick, capture.status == StationTransferStatus::Rejected
					? "CO2 capture '" + capture.authored.id + "' rejected: " + reason
					: "CO2 capture '" + capture.authored.id + "' partial: " + reason);
			}
			for (size_t index = 0; index < fuelCells.size(); ++index)
			{
				auto const& cell = fuelCells[index];
				if (cell.status == previousFuelCellStatus[index]) continue;
				if (cell.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Fuel cell '" + cell.authored.id
						+ "' delivering at commanded output");
					continue;
				}
				auto const reason = !cell.lastBlockReason.empty()
					? cell.lastBlockReason : cell.lastLimiter;
				appendBoundedEvent(events, tick, cell.status == StationTransferStatus::Rejected
					? "Fuel cell '" + cell.authored.id + "' rejected: " + reason
					: "Fuel cell '" + cell.authored.id + "' partial: " + reason);
			}
			for (size_t index = 0; index < sabatierReactors.size(); ++index)
			{
				auto const& reactor = sabatierReactors[index];
				if (reactor.status == previousSabatierStatus[index]) continue;
				if (reactor.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Sabatier reactor '" + reactor.authored.id
						+ "' converting at full rate");
					continue;
				}
				auto const reason = !reactor.lastBlockReason.empty()
					? reactor.lastBlockReason : reactor.lastLimiter;
				appendBoundedEvent(events, tick, reactor.status == StationTransferStatus::Rejected
					? "Sabatier reactor '" + reactor.authored.id + "' rejected: " + reason
					: "Sabatier reactor '" + reactor.authored.id + "' partial: " + reason);
			}
			for (size_t index = 0; index < methaneProcessors.size(); ++index)
			{
				auto const& processor = methaneProcessors[index];
				if (processor.status == previousMethaneProcessorStatus[index]) continue;
				if (processor.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Methane processor '" + processor.authored.id
						+ "' processing at full rate");
					continue;
				}
				auto const reason = !processor.lastBlockReason.empty()
					? processor.lastBlockReason : processor.lastLimiter;
				appendBoundedEvent(events, tick, processor.status == StationTransferStatus::Rejected
					? "Methane processor '" + processor.authored.id + "' rejected: " + reason
					: "Methane processor '" + processor.authored.id + "' partial: " + reason);
			}
			for (size_t index = 0; index < microbialProcessors.size(); ++index)
			{
				auto const& processor = microbialProcessors[index];
				if (processor.status == previousMicrobialProcessorStatus[index]) continue;
				if (processor.status == StationTransferStatus::Full)
				{
					appendBoundedEvent(events, tick, "Microbial processor '" + processor.authored.id
						+ "' digesting at full rate");
					continue;
				}
				auto const reason = !processor.lastBlockReason.empty()
					? processor.lastBlockReason : processor.lastLimiter;
				appendBoundedEvent(events, tick, processor.status == StationTransferStatus::Rejected
					? "Microbial processor '" + processor.authored.id + "' rejected: " + reason
					: "Microbial processor '" + processor.authored.id + "' partial: " + reason);
			}
			// Delivery arrivals are discrete scenario events rather than a
			// continuous per-tick service, so every arrival is announced with
			// its own quantities: the refused remainder is the account for
			// material that never entered the station (#392).
			for (auto const& delivery : deliveries)
			{
				if (delivery.lastFiredTick != tick) continue;
				auto const label = "Delivery '" + delivery.authored.id + "' at "
					+ std::to_string(delivery.lastOccurrenceSeconds) + " s: ";
				if (delivery.status == StationTransferStatus::Full)
					appendBoundedEvent(events, tick, label + "accepted "
						+ std::to_string(delivery.lastAccepted) + " " + delivery.authored.unit
						+ " into '" + delivery.authored.storeId + "'");
				else if (delivery.status == StationTransferStatus::Partial)
					appendBoundedEvent(events, tick, label + "partially accepted "
						+ std::to_string(delivery.lastAccepted) + " of "
						+ std::to_string(delivery.lastScheduled) + " " + delivery.authored.unit
						+ "; " + std::to_string(delivery.lastRejected)
						+ " refused, destination '" + delivery.authored.storeId + "' is at capacity");
				else
					appendBoundedEvent(events, tick, label + "refused "
						+ std::to_string(delivery.lastRejected) + " " + delivery.authored.unit
						+ ": destination '" + delivery.authored.storeId + "' is at capacity");
			}
			// Waste ejection occurrences are discrete scenario events too:
			// every ejection is announced with what actually left the
			// station, and the unmet remainder names why less went than was
			// scheduled — nothing eligible was available (#403).
			for (auto const& ejection : wasteEjections)
			{
				if (ejection.lastFiredTick != tick) continue;
				auto const label = "Waste ejection '" + ejection.authored.id + "' at "
					+ std::to_string(ejection.lastOccurrenceSeconds) + " s: ";
				if (ejection.status == StationTransferStatus::Full)
					appendBoundedEvent(events, tick, label + "ejected "
						+ std::to_string(ejection.lastEjected) + " " + ejection.authored.unit
						+ " from '" + ejection.authored.storeId + "' as external loss");
				else if (ejection.status == StationTransferStatus::Partial)
					appendBoundedEvent(events, tick, label + "ejected "
						+ std::to_string(ejection.lastEjected) + " of "
						+ std::to_string(ejection.lastScheduled) + " " + ejection.authored.unit
						+ " from '" + ejection.authored.storeId + "' as external loss; "
						+ std::to_string(ejection.lastUnmet)
						+ " not ejectable: no more disposal-ready material held");
				else
					appendBoundedEvent(events, tick, label + "nothing ejected from '"
						+ ejection.authored.storeId + "': no disposal-ready material held");
			}
			return true;
		}
	};
	StationSystemsRuntime::StationSystemsRuntime(StationSystemsSettings settings) : mImpl(std::make_unique<Impl>())
	{
		std::string diagnostic;
		if (!validateStationSystemsSettings(settings, diagnostic)) throw SerializationException(diagnostic);
		mImpl->settings = settings; mImpl->snapshot.enabled = settings.enabled;
		for (auto const& store : settings.stores)
		{
			mImpl->stores.push_back({store, store.startingQualified, store.startingUnqualified, 0.0, 0.0});
			mImpl->startingTotals[store.unit] += store.startingQualified + store.startingUnqualified;
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
		for (auto const& chiller : settings.chillers)
		{
			Impl::ChillerState state;
			state.authored = chiller;
			state.loopIndex = loopIndex[chiller.coldLoopId];
			state.sinkIndex = reservoirIndex[chiller.condenserReservoirId];
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
			state.declarationIndex = mImpl->microbialProcessors.size();
			mImpl->microbialProcessors.push_back(std::move(state));
		}
		mImpl->microbialProcessorOrder.resize(mImpl->microbialProcessors.size());
		std::iota(mImpl->microbialProcessorOrder.begin(), mImpl->microbialProcessorOrder.end(), size_t{0});
		std::stable_sort(mImpl->microbialProcessorOrder.begin(), mImpl->microbialProcessorOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->microbialProcessors[left].authored.priority
				> mImpl->microbialProcessors[right].authored.priority; });
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
				&settings.sabatierReactors, &settings.methaneProcessors,
				&settings.microbialProcessors};
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
		appendBoundedEvent(mImpl->events, 0, "Station systems initialized with "
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
	bool StationSystemsRuntime::advance(uint64_t tick, float seconds)
	{
		if (!mImpl->settings.enabled) return true;
		if (!mImpl->snapshot.healthy) return false;
		auto next = mImpl->snapshot;
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto& object = *mImpl->objects[i];
			Step step{&object, tick, seconds};
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
				if (step.completed != next.completedTicks[i] + 1)
				{
					step.invalidOutput = true;
					throw std::runtime_error("Station lifecycle tick accounting mismatch");
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
			|| !mImpl->reservoirs.empty()
			|| !mImpl->coolantLoops.empty() || !mImpl->chillers.empty()
			|| !mImpl->generators.empty() || !mImpl->atmospheres.empty()
			|| !mImpl->airlockCycles.empty() || !mImpl->crews.empty()
			|| !mImpl->waterCollectors.empty() || !mImpl->waterTreatments.empty()
			|| !mImpl->electrolyzers.empty() || !mImpl->co2Captures.empty()
			|| !mImpl->fuelCells.empty() || !mImpl->thermalBuffers.empty()
			|| !mImpl->sabatierReactors.empty() || !mImpl->methaneProcessors.empty())
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
		next.elapsedSeconds = static_cast<double>(tick) * World::getFixedTimestep();
		mImpl->snapshot = next;
		auto interval = static_cast<uint64_t>(std::ceil(mImpl->settings.observationIntervalSeconds / World::getFixedTimestep()));
		if (tick % interval == 0)
		{
			appendBoundedEvent(mImpl->events, tick, "All four Station systems completed the World tick");
		}
		return true;
	}
	StationSystemsSnapshot StationSystemsRuntime::snapshot() const
	{
		auto result = mImpl->snapshot;
		std::map<std::string, double> currentTotals;
		ConstituentLedger currentConstituents;
		for (auto const& store : mImpl->stores)
		{
			result.stores.push_back({store.authored.id, store.authored.unit,
				store.authored.capacity, store.authored.startingQualified,
				store.authored.startingUnqualified, store.authored.protectedReserve,
				store.currentQualified, store.currentUnqualified,
				store.lastTickIn, store.lastTickOut, store.authored.composition});
			currentTotals[store.authored.unit] += store.currentQualified + store.currentUnqualified;
			if (store.authored.composition)
				addComposition(currentConstituents, *store.authored.composition,
					massToKg(store.authored.unit,
						store.currentQualified + store.currentUnqualified), 1.0);
		}
		for (auto const index : mImpl->transferOrder)
		{
			auto const& transfer = mImpl->transfers[index];
			result.transfers.push_back({transfer.authored.id, transfer.authored.sourceId,
				transfer.authored.destinationId, transfer.authored.unit,
				transfer.authored.ratePerSecond, transfer.authored.priority,
				transfer.lastRequested, transfer.lastCompleted, transfer.lastUnmet, transfer.status});
		}
		for (auto const index : mImpl->conversionOrder)
		{
			auto const& conversion = mImpl->conversions[index];
			StationConversionSnapshot conversionResult;
			conversionResult.id = conversion.authored.id;
			conversionResult.priority = conversion.authored.priority;
			conversionResult.lastScale = conversion.lastScale;
			conversionResult.status = conversion.status;
			for (auto const& flow : conversion.inputs)
				conversionResult.inputs.push_back({mImpl->stores[flow.storeIndex].authored.id,
					mImpl->stores[flow.storeIndex].authored.unit,
					flow.lastRequested, flow.lastCompleted});
			for (auto const& flow : conversion.outputs)
				conversionResult.outputs.push_back({mImpl->stores[flow.storeIndex].authored.id,
					mImpl->stores[flow.storeIndex].authored.unit,
					flow.lastRequested, flow.lastCompleted});
			result.conversions.push_back(std::move(conversionResult));
		}
		for (auto const& [unit, starting] : mImpl->startingTotals)
		{
			auto const lost = mImpl->unitLoss.count(unit) ? mImpl->unitLoss.at(unit) : 0.0;
			auto const delivered = mImpl->unitDelivered.count(unit)
				? mImpl->unitDelivered.at(unit) : 0.0;
			auto const collected = mImpl->unitCollected.count(unit)
				? mImpl->unitCollected.at(unit) : 0.0;
			result.balances.push_back({unit, starting, currentTotals[unit], lost, delivered,
				collected, currentTotals[unit] + lost - delivered - collected - starting});
		}
		for (auto const& [constituent, starting] : mImpl->startingConstituents)
		{
			auto const current = currentConstituents.count(constituent)
				? currentConstituents.at(constituent) : 0.0;
			auto const lost = mImpl->constituentLoss.count(constituent)
				? mImpl->constituentLoss.at(constituent) : 0.0;
			auto const delivered = mImpl->constituentDelivered.count(constituent)
				? mImpl->constituentDelivered.at(constituent) : 0.0;
			result.constituentBalances.push_back({constituent,
				std::string(constituent == StationEnergyConstituent ? "kJ" : "kg"),
				starting, current, lost, delivered, current + lost - delivered - starting});
		}
		result.balanceTolerance = StationBalanceTolerance;
		// The power snapshot reports the most recent tick's solar supply
		// and external equipment allocation in the declared order.
		for (auto const& array : mImpl->solarArrays)
			result.power.sources.push_back({array.authored.id, array.authored.capacityKW,
				array.lastIllumination, array.lastAvailableKW, array.lastDisabled});
		double totalRequested = 0.0, totalSupplied = 0.0, totalUnmetCritical = 0.0, totalHeat = 0.0;
		double totalAvailable = 0.0;
		for (auto const index : mImpl->loadOrder)
		{
			auto const& load = mImpl->loads[index];
			StationEquipmentLoadSnapshot loadResult;
			loadResult.id = load.authored.id;
			loadResult.priority = load.authored.priority;
			loadResult.thermalClass = load.authored.thermalClass;
			loadResult.coolingDisposition = load.authored.coolingDisposition;
			loadResult.profileLevel = load.lastProfileLevel;
			loadResult.requestedKW = load.lastRequestedKW;
			loadResult.suppliedKW = load.lastSuppliedKW;
			loadResult.unmetKW = load.lastUnmetKW;
			loadResult.criticalFloorKW = load.authored.criticalFloorKW;
			loadResult.unmetCriticalKW = load.lastUnmetCriticalKW;
			loadResult.deliveredFraction = load.lastDeliveredFraction;
			loadResult.materialFeasibleFraction = load.lastMaterialFraction;
			loadResult.heatExportedKW = load.lastHeatExportedKW;
			loadResult.heatRoutedKW = load.lastHeatRoutedKW;
			loadResult.unmetCoolingKW = load.lastUnmetCoolingKW;
			loadResult.admitted = load.lastAdmitted;
			loadResult.blockReason = load.lastBlockReason;
			loadResult.status = load.status;
			for (auto const& exchange : load.exchanges)
				loadResult.materialExchanges.push_back({mImpl->stores[exchange.storeIndex].authored.id,
					mImpl->stores[exchange.storeIndex].authored.unit,
					exchange.lastRequested, exchange.lastCompleted, exchange.lastUnmet});
			totalRequested += load.lastRequestedKW;
			totalSupplied += load.lastSuppliedKW;
			totalUnmetCritical += load.lastUnmetCriticalKW;
			totalHeat += load.lastHeatExportedKW;
			result.power.loads.push_back(std::move(loadResult));
		}
		for (auto const& array : mImpl->solarArrays) totalAvailable += array.lastAvailableKW;
		result.power.availableGenerationKW = totalAvailable;
		result.power.totalRequestedKW = totalRequested;
		result.power.totalSuppliedKW = totalSupplied;
		result.power.totalUnmetKW = totalRequested - totalSupplied;
		result.power.unmetCriticalKW = totalUnmetCritical;
		result.power.surplusKW = mImpl->lastAvailableGenerationKW;
		result.power.exportedHeatKW = totalHeat;
		// The battery snapshot reports the authored nameplate/usable/reserve
		// policy beside the runtime stored energy, the last tick's charge and
		// discharge power, and the cumulative loss ledger (#394).
		double totalBatteryDischarge = 0.0, totalBatteryCharge = 0.0;
		for (auto const& battery : mImpl->batteries)
		{
			StationBatterySnapshot batteryResult;
			batteryResult.id = battery.authored.id;
			batteryResult.nameplateKWh = battery.authored.nameplateKWh;
			batteryResult.usableKWh = battery.authored.usableKWh;
			batteryResult.reserveKWh = battery.authored.nameplateKWh - battery.authored.usableKWh;
			batteryResult.startingStoredKWh = battery.authored.startingStoredKWh;
			batteryResult.storedKWh = battery.stored;
			batteryResult.stateOfCharge = battery.authored.nameplateKWh > 0.0
				? battery.stored / battery.authored.nameplateKWh : 0.0;
			batteryResult.maxChargeKW = battery.authored.maxChargeKW;
			batteryResult.maxDischargeKW = battery.authored.maxDischargeKW;
			batteryResult.chargeEfficiency = battery.authored.chargeEfficiency;
			batteryResult.dischargeEfficiency = battery.authored.dischargeEfficiency;
			batteryResult.lastChargeKW = battery.lastChargeKW;
			batteryResult.lastDischargeKW = battery.lastDischargeKW;
			batteryResult.lastDischargeRequestedKW = battery.lastDischargeRequestedKW;
			batteryResult.lastDischargeShortfallKW = battery.lastDischargeShortfallKW;
			batteryResult.chargedKWh = battery.chargedKWh;
			batteryResult.dischargedKWh = battery.dischargedKWh;
			batteryResult.deliveredKWh = battery.deliveredKWh;
			batteryResult.chargeInputKWh = battery.chargeInputKWh;
			batteryResult.lossesKWh = (battery.dischargedKWh - battery.deliveredKWh)
				+ (battery.chargeInputKWh - battery.chargedKWh);
			batteryResult.residualKWh = battery.authored.startingStoredKWh - battery.dischargedKWh
				+ battery.chargedKWh - battery.stored;
			batteryResult.disabled = battery.authored.usableKWh <= 0.0;
			batteryResult.atReserve = battery.lastAtReserve;
			batteryResult.saturated = battery.lastSaturated;
			batteryResult.status = battery.status;
			totalBatteryDischarge += battery.lastDischargeKW;
			totalBatteryCharge += battery.lastChargeKW;
			result.power.batteries.push_back(std::move(batteryResult));
		}
		result.power.batteryDischargeKW = totalBatteryDischarge;
		result.power.batteryChargeKW = totalBatteryCharge;
		result.power.batteryShortfallKW = mImpl->lastBatteryShortfallKW;
		// Coolant loop electrical service rode the same pool as external
		// loads; publish the totals so the window can show the loop draw
		// beside the load draw (#396).
		double totalCoolantPump = 0.0, totalCoolantHeater = 0.0;
		for (auto const& loop : mImpl->coolantLoops)
		{
			totalCoolantPump += loop.lastPumpSuppliedKW;
			totalCoolantHeater += loop.lastHeaterSuppliedKW;
		}
		result.power.coolantPumpKW = totalCoolantPump;
		result.power.coolantHeaterKW = totalCoolantHeater;
		// Chiller electrical service rode the same pool as external loads
		// and coolant loops; publish the total so the window can show the
		// chiller draw beside the others (#397).
		double totalChillerPower = 0.0;
		for (auto const& chiller : mImpl->chillers) totalChillerPower += chiller.lastPowerSuppliedKW;
		result.power.chillerPowerKW = totalChillerPower;
		// The generator snapshot reports the authored contract beside the
		// phase, the electrical output actually delivered, the fuel
		// actually drawn, the heat actually routed, the startup progress
		// and cooling qualification, and the closing fuel ledger (#399).
		// Generator output rode the same pool as solar; the totals make
		// the fuelled share explicit beside the solar share.
		double totalGeneratorOutput = 0.0, totalGeneratorStartup = 0.0;
		double totalGeneratorWaste = 0.0, totalGeneratorDecay = 0.0;
		for (auto const& generator : mImpl->generators)
		{
			StationGeneratorSnapshot generatorResult;
			generatorResult.id = generator.authored.id;
			generatorResult.fuelStoreId = generator.authored.fuelStoreId;
			generatorResult.thermalClass = generator.authored.thermalClass;
			generatorResult.heatSinkId = generator.authored.heatSinkId;
			generatorResult.ratedOutputKW = generator.authored.ratedOutputKW;
			generatorResult.efficiency = generator.authored.efficiency;
			generatorResult.maxRampKWPerSecond = generator.authored.maxRampKWPerSecond;
			generatorResult.commandedOutputKW = generator.authored.commandedOutputKW;
			generatorResult.startupPowerKW = generator.authored.startupPowerKW;
			generatorResult.startupSeconds = generator.authored.startupSeconds;
			generatorResult.shutdownHeatKW = generator.authored.shutdownHeatKW;
			generatorResult.shutdownSeconds = generator.authored.shutdownSeconds;
			generatorResult.maxSinkTemperatureC = generator.authored.maxSinkTemperatureC;
			generatorResult.phase = generator.phase;
			generatorResult.outputKW = generator.outputKW;
			generatorResult.fuelDrawKWh = generator.lastFuelDrawKWh;
			generatorResult.wasteHeatKW = generator.lastWasteHeatKW;
			generatorResult.startupDrawKW = generator.lastStartupDrawKW;
			generatorResult.startupUnmetKW = generator.lastStartupUnmetKW;
			generatorResult.startupReceivedKJ = generator.startupReceivedKJ;
			generatorResult.startupRequiredKJ = generator.authored.startupPowerKW
				* generator.authored.startupSeconds;
			generatorResult.decayHeatKW = generator.lastDecayHeatKW;
			generatorResult.sinkTemperatureC = mImpl->reservoirs[generator.sinkIndex].temperatureC;
			generatorResult.coolingAvailable = generator.lastCoolingAvailable;
			generatorResult.coolingRequired = generator.lastCoolingRequired;
			generatorResult.disabled = generator.authored.ratedOutputKW <= 0.0;
			generatorResult.blockReason = generator.lastBlockReason;
			generatorResult.fuelEnergyKJ = generator.fuelEnergyKJ;
			generatorResult.electricalKJ = generator.electricalKJ;
			generatorResult.wasteHeatKJ = generator.wasteHeatKJ;
			generatorResult.startupHeatKJ = generator.startupHeatKJ;
			generatorResult.decayHeatKJ = generator.decayHeatKJ;
			generatorResult.residualKJ = generator.fuelEnergyKJ
				- generator.electricalKJ - generator.wasteHeatKJ;
			generatorResult.status = generator.status;
			totalGeneratorOutput += generator.outputKW;
			totalGeneratorStartup += generator.lastStartupDrawKW;
			totalGeneratorWaste += generator.lastWasteHeatKW;
			totalGeneratorDecay += generator.lastDecayHeatKW;
			result.power.generators.push_back(std::move(generatorResult));
		}
		result.power.generatorOutputKW = totalGeneratorOutput;
		result.power.generatorStartupDrawKW = totalGeneratorStartup;
		result.power.generatorWasteHeatKW = totalGeneratorWaste;
		result.power.generatorDecayHeatKW = totalGeneratorDecay;
		// The thermal snapshot reports the authored capacity and declared
		// limits beside the integrated temperature, the last tick's heat
		// flows and the cumulative energy ledger (#395).
		for (auto const& reservoir : mImpl->reservoirs)
		{
			StationThermalReservoirSnapshot reservoirResult;
			reservoirResult.id = reservoir.authored.id;
			reservoirResult.thermalClass = reservoir.authored.thermalClass;
			reservoirResult.heatCapacityKJPerK = reservoir.authored.heatCapacityKJPerK;
			reservoirResult.startingTemperatureC = reservoir.authored.startingTemperatureC;
			reservoirResult.temperatureC = reservoir.temperatureC;
			reservoirResult.minTemperatureC = reservoir.authored.minTemperatureC;
			reservoirResult.maxTemperatureC = reservoir.authored.maxTemperatureC;
			reservoirResult.absorbedHeatKW = reservoir.authored.absorbedHeatKW;
			reservoirResult.lastHeatRoutedKW = reservoir.lastHeatRoutedKW;
			reservoirResult.lastLoopHeatKW = reservoir.lastLoopHeatKW;
			reservoirResult.lastCondenserHeatKW = reservoir.lastCondenserHeatKW;
			reservoirResult.lastGeneratorHeatKW = reservoir.lastGeneratorHeatKW;
			reservoirResult.lastElectrolysisHeatKW = reservoir.lastElectrolysisHeatKW;
			reservoirResult.electrolysisEnergyKJ = reservoir.electrolysisKJ;
			reservoirResult.lastFuelCellHeatKW = reservoir.lastFuelCellHeatKW;
			reservoirResult.fuelCellEnergyKJ = reservoir.fuelCellKJ;
			reservoirResult.lastSabatierHeatKW = reservoir.lastSabatierHeatKW;
			reservoirResult.sabatierEnergyKJ = reservoir.sabatierKJ;
			reservoirResult.lastMethaneProcessingHeatKW = reservoir.lastMethaneProcessingHeatKW;
			reservoirResult.methaneProcessingEnergyKJ = reservoir.methaneProcessingKJ;
			reservoirResult.lastMicrobialHeatKW = reservoir.lastMicrobialHeatKW;
			reservoirResult.microbialEnergyKJ = reservoir.microbialKJ;
			reservoirResult.lastAtmosphereHeatKW = reservoir.lastAtmosphereHeatKW;
			reservoirResult.lastBufferChargeKW = reservoir.lastBufferChargeKW;
			reservoirResult.lastBufferDischargeKW = reservoir.lastBufferDischargeKW;
			reservoirResult.lastHeatInKW = reservoir.lastHeatInKW;
			reservoirResult.lastRejectedKW = reservoir.lastRejectedKW;
			reservoirResult.lastNetKW = reservoir.lastNetKW;
			reservoirResult.aboveLimit = reservoir.lastAboveLimit;
			reservoirResult.belowLimit = reservoir.lastBelowLimit;
			reservoirResult.startingEnergyKJ = reservoir.authored.heatCapacityKJPerK
				* reservoir.authored.startingTemperatureC;
			reservoirResult.currentEnergyKJ = reservoir.authored.heatCapacityKJPerK
				* reservoir.temperatureC;
			reservoirResult.absorbedEnergyKJ = reservoir.absorbedKJ;
			reservoirResult.routedEnergyKJ = reservoir.routedKJ;
			reservoirResult.loopEnergyKJ = reservoir.loopKJ;
			reservoirResult.condenserEnergyKJ = reservoir.condenserKJ;
			reservoirResult.generatorEnergyKJ = reservoir.generatorKJ;
			reservoirResult.atmosphereEnergyKJ = reservoir.atmosphereKJ;
			reservoirResult.bufferChargeEnergyKJ = reservoir.bufferChargeKJ;
			reservoirResult.bufferDischargeEnergyKJ = reservoir.bufferDischargeKJ;
			reservoirResult.rejectedEnergyKJ = reservoir.rejectedKJ;
			reservoirResult.residualKJ = reservoirResult.startingEnergyKJ
				+ reservoir.absorbedKJ + reservoir.routedKJ + reservoir.loopKJ
				+ reservoir.condenserKJ + reservoir.generatorKJ + reservoir.electrolysisKJ
				+ reservoir.fuelCellKJ + reservoir.sabatierKJ + reservoir.methaneProcessingKJ
				+ reservoir.microbialKJ
				+ reservoir.atmosphereKJ
				+ reservoir.bufferDischargeKJ - reservoir.bufferChargeKJ
				- reservoir.rejectedKJ - reservoirResult.currentEnergyKJ;
			result.thermal.totalAbsorbedKW += reservoir.authored.absorbedHeatKW;
			result.thermal.totalRoutedKW += reservoir.lastHeatRoutedKW;
			result.thermal.totalRejectedKW += reservoir.lastRejectedKW;
			result.thermal.reservoirs.push_back(std::move(reservoirResult));
		}
		for (auto const& radiator : mImpl->radiators)
			result.thermal.radiators.push_back({radiator.authored.id,
				radiator.authored.reservoirId, radiator.authored.areaM2,
					radiator.authored.emissivity, radiator.authored.viewFactor,
					radiator.authored.sinkTemperatureC, radiator.lastRejectedKW});
		// The coolant loop snapshot reports the authored contract beside the
		// runtime's finite inventory, the pump and freeze-heater service
		// results, the exposed freeze condition and the closing loop energy
		// ledger (#396).
		for (auto const& loop : mImpl->coolantLoops)
		{
			StationCoolantLoopSnapshot loopResult;
			loopResult.id = loop.authored.id;
			loopResult.hazardClass = loop.authored.hazardClass;
			loopResult.reservoirId = loop.authored.reservoirId;
			loopResult.coolantCapacityKg = loop.authored.coolantCapacityKg;
			loopResult.coolantKg = loop.coolantKg;
			loopResult.fillFraction = loop.authored.coolantCapacityKg > 0.0
				? loop.coolantKg / loop.authored.coolantCapacityKg : 0.0;
			loopResult.pumpPowerKW = loop.authored.pumpPowerKW;
			loopResult.ratedFlowKgPerS = loop.authored.ratedFlowKgPerS;
			loopResult.pumpDemandKW = loop.lastPumpDemandKW;
			loopResult.pumpSuppliedKW = loop.lastPumpSuppliedKW;
			loopResult.pumpUnmetKW = loop.lastPumpDemandKW - loop.lastPumpSuppliedKW;
			loopResult.circulationFraction = loop.lastCirculationFraction;
			loopResult.flowKgPerS = loop.lastFlowKgPerS;
			loopResult.unmetFlowKgPerS = loop.authored.ratedFlowKgPerS - loop.lastFlowKgPerS;
			loopResult.heaterCapacityKW = loop.authored.heaterCapacityKW;
			loopResult.heaterDemandKW = loop.lastHeaterDemandKW;
			loopResult.heaterSuppliedKW = loop.lastHeaterSuppliedKW;
			loopResult.heaterUnmetKW = loop.lastHeaterDemandKW - loop.lastHeaterSuppliedKW;
			loopResult.loopTemperatureC = loop.temperatureC;
			loopResult.startingTemperatureC = loop.authored.startingTemperatureC;
			loopResult.freezeProtectionSetpointC = loop.authored.freezeProtectionSetpointC;
			loopResult.exchangerConductanceKWPerK = loop.authored.exchangerConductanceKWPerK;
			loopResult.exchangeIntoLoopKW = loop.lastExchangeIntoLoopKW;
			loopResult.chillerExtractionKW = loop.lastChillerExtractionKW;
			loopResult.belowFreezeSetpoint = loop.lastBelowFreeze;
			loopResult.disabled = loop.coolantKg <= 0.0;
			loopResult.startingEnergyKJ = loop.authored.startingCoolantKg
				* loop.authored.specificHeatKJPerKgK * loop.authored.startingTemperatureC;
			loopResult.currentEnergyKJ = loop.coolantKg
				* loop.authored.specificHeatKJPerKgK * loop.temperatureC;
			loopResult.pumpWorkEnergyKJ = loop.pumpWorkKJ;
			loopResult.heaterEnergyKJ = loop.heaterKJ;
			loopResult.exchangedEnergyKJ = loop.exchangedKJ;
			loopResult.extractedEnergyKJ = loop.extractedKJ;
			loopResult.collectorHeatKW = loop.lastCollectorHeatKW;
			loopResult.collectorHeatEnergyKJ = loop.collectorHeatKJ;
			loopResult.residualKJ = loopResult.startingEnergyKJ + loop.pumpWorkKJ
				+ loop.heaterKJ + loop.exchangedKJ + loop.collectorHeatKJ
				- loop.extractedKJ - loopResult.currentEnergyKJ;
			loopResult.status = loop.status;
			result.thermal.totalCirculationKgPerS += loop.lastFlowKgPerS;
			result.thermal.totalFreezeHeatingKW += loop.lastHeaterSuppliedKW;
			result.coolantLoops.push_back(std::move(loopResult));
		}
		// The chiller snapshot reports the authored contract beside the
		// cold-side duty actually extracted, the work and auxiliaries
		// actually supplied, the condenser heat actually rejected and the
		// unmet remainder, plus the closing chiller energy ledger
		// (#397). The per-class rollup keeps the cold-service account
		// segregated by thermal service class: spare capacity in one
		// class is never usable by another, so each class carries its own
		// provided/unmet/work/condenser totals.
		for (auto const& chiller : mImpl->chillers)
		{
			StationChillerSnapshot chillerResult;
			chillerResult.id = chiller.authored.id;
			chillerResult.thermalClass = chiller.authored.thermalClass;
			chillerResult.coldLoopId = chiller.authored.coldLoopId;
			chillerResult.condenserReservoirId = chiller.authored.condenserReservoirId;
			chillerResult.ratedCoolingCapacityKW = chiller.authored.ratedCoolingCapacityKW;
			chillerResult.cop = chiller.authored.cop;
			chillerResult.auxiliaryPowerKW = chiller.authored.auxiliaryPowerKW;
			chillerResult.chilledSetpointC = chiller.authored.chilledSetpointC;
			chillerResult.maxCondensingTemperatureC = chiller.authored.maxCondensingTemperatureC;
			chillerResult.coolingDemandKW = chiller.lastCoolingDemandKW;
			chillerResult.coolingKW = chiller.lastCoolingKW;
			chillerResult.coolingUnmetKW = chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
			chillerResult.deliverableKW = chiller.lastDeliverableKW;
			chillerResult.powerDemandKW = chiller.lastPowerDemandKW;
			chillerResult.powerSuppliedKW = chiller.lastPowerSuppliedKW;
			chillerResult.powerUnmetKW = chiller.lastPowerDemandKW - chiller.lastPowerSuppliedKW;
			chillerResult.deliveredFraction = chiller.lastDeliveredFraction;
			chillerResult.condenserHeatKW = chiller.lastCondenserKW;
			chillerResult.coldLoopTemperatureC = mImpl->coolantLoops[chiller.loopIndex].temperatureC;
			chillerResult.coldLoopCirculationFraction =
				mImpl->coolantLoops[chiller.loopIndex].lastCirculationFraction;
			chillerResult.sinkTemperatureC = mImpl->reservoirs[chiller.sinkIndex].temperatureC;
			chillerResult.admitted = chiller.lastAdmitted;
			chillerResult.blockReason = chiller.lastBlockReason;
			chillerResult.atChilledSetpoint = chiller.lastAtSetpoint;
			chillerResult.sinkOverMaxCondensing = chiller.lastSinkOver;
			chillerResult.disabled = chiller.authored.ratedCoolingCapacityKW <= 0.0;
			chillerResult.extractedEnergyKJ = chiller.extractedKJ;
			chillerResult.workEnergyKJ = chiller.workKJ;
			chillerResult.condenserEnergyKJ = chiller.condenserKJ;
			chillerResult.residualKJ = chiller.extractedKJ + chiller.workKJ - chiller.condenserKJ;
			chillerResult.status = chiller.status;
			result.thermal.totalCoolingProvidedKW += chiller.lastCoolingKW;
			result.thermal.totalCoolingUnmetKW += chillerResult.coolingUnmetKW;
			result.thermal.totalCondenserHeatKW += chiller.lastCondenserKW;
			result.chillers.push_back(std::move(chillerResult));
		}
		for (auto const& buffer : mImpl->thermalBuffers)
		{
			result.thermal.totalBufferStoredKJ += buffer.storedKJ;
			result.thermal.totalBufferAvailableCapacityKJ +=
				buffer.authored.capacityKJ - buffer.storedKJ;
			result.thermal.totalBufferChargeKW += buffer.lastChargeKW;
			result.thermal.totalBufferDischargeKW += buffer.lastDischargeKW;
		}
		// The thermal buffer snapshot reports the authored contract beside
		// the finite stored energy, the derived buffer temperature across
		// the usable band, the access gate state, the charge and discharge
		// actually moved this tick, the exhaustion flags and the closing
		// storage ledger (#398). Stored energy is distinct from any
		// instantaneous temperature reading: the store's own derived
		// temperature says how full it is, not how hot the host is.
		for (auto const& buffer : mImpl->thermalBuffers)
		{
			StationThermalBufferSnapshot bufferResult;
			auto const& authored = buffer.authored;
			bufferResult.id = authored.id;
			bufferResult.reservoirId = authored.reservoirId;
			bufferResult.accessMode = authored.accessMode;
			bufferResult.circulationLoopId = authored.circulationLoopId;
			bufferResult.capacityKJ = authored.capacityKJ;
			bufferResult.storedEnergyKJ = buffer.storedKJ;
			bufferResult.availableCapacityKJ = authored.capacityKJ - buffer.storedKJ;
			auto const band = authored.maxUsableTemperatureC - authored.minUsableTemperatureC;
			bufferResult.bufferTemperatureC = authored.minUsableTemperatureC
				+ buffer.storedKJ * band / authored.capacityKJ;
			bufferResult.fillFraction = buffer.storedKJ / authored.capacityKJ;
			bufferResult.minUsableTemperatureC = authored.minUsableTemperatureC;
			bufferResult.maxUsableTemperatureC = authored.maxUsableTemperatureC;
			bufferResult.maxChargeKW = authored.maxChargeKW;
			bufferResult.maxDischargeKW = authored.maxDischargeKW;
			bufferResult.transferConductanceKWPerK = authored.transferConductanceKWPerK;
			bufferResult.accessible = buffer.lastAccessible;
			bufferResult.lastChargeKW = buffer.lastChargeKW;
			bufferResult.lastDischargeKW = buffer.lastDischargeKW;
			bufferResult.full = buffer.lastFull;
			bufferResult.depleted = buffer.lastDepleted;
			bufferResult.lastLimiter = buffer.lastLimiter;
			bufferResult.startingEnergyKJ = authored.startingEnergyKJ;
			bufferResult.chargedEnergyKJ = buffer.chargedKJ;
			bufferResult.dischargedEnergyKJ = buffer.dischargedKJ;
			bufferResult.residualKJ = authored.startingEnergyKJ + buffer.chargedKJ
				- buffer.dischargedKJ - buffer.storedKJ;
			bufferResult.status = buffer.status;
			result.thermalBuffers.push_back(std::move(bufferResult));
		}
		for (auto const* thermalClass : StationThermalServiceClasses)
		{
			StationCoolingClassSnapshot classResult;
			classResult.thermalClass = thermalClass;
			for (auto const& chiller : mImpl->chillers)
				if (chiller.authored.thermalClass == classResult.thermalClass)
				{
					classResult.capacityKW += chiller.authored.ratedCoolingCapacityKW;
					classResult.providedKW += chiller.lastCoolingKW;
					classResult.unmetKW += chiller.lastCoolingDemandKW - chiller.lastCoolingKW;
					classResult.workKW += chiller.lastPowerSuppliedKW;
					classResult.condenserKW += chiller.lastCondenserKW;
				}
			result.thermal.coolingByClass.push_back(std::move(classResult));
		}
		// The atmosphere snapshot reports the authored contract beside the
		// initial versus current gas inventory, the derived absolute and
		// partial pressures at the current state, the humidity, the last
		// tick's pathway flows and the closing per-component and thermal
		// ledgers (#400). Containment is proven by the closing ledger and
		// the exposed crossings, never by a pressure setpoint.
		for (auto const& compartment : mImpl->atmospheres)
		{
			StationAtmosphereSnapshot atmosphereResult;
			auto const& authored = compartment.authored;
			atmosphereResult.id = authored.id;
			atmosphereResult.hazardClass = authored.hazardClass;
			atmosphereResult.reservoirId = authored.reservoirId;
			atmosphereResult.volumeM3 = authored.volumeM3;
			atmosphereResult.thermalCapacityKJPerK = authored.thermalCapacityKJPerK;
			atmosphereResult.exchangerConductanceKWPerK = authored.exchangerConductanceKWPerK;
			atmosphereResult.occupants = authored.occupants;
			atmosphereResult.oxygenUsePerOccupantKgPerS = authored.oxygenUsePerOccupantKgPerS;
			atmosphereResult.co2ProductionPerOccupantKgPerS = authored.co2ProductionPerOccupantKgPerS;
			atmosphereResult.sensibleHeatPerOccupantKW = authored.sensibleHeatPerOccupantKW;
			atmosphereResult.moistureAdditionPerOccupantKgPerS = authored.moistureAdditionPerOccupantKgPerS;
			atmosphereResult.leakRateKgPerS = authored.leakRateKgPerS;
			atmosphereResult.minPressureKPa = authored.minPressureKPa;
			atmosphereResult.maxPressureKPa = authored.maxPressureKPa;
			atmosphereResult.minOxygenPartialPressureKPa = authored.minOxygenPartialPressureKPa;
			atmosphereResult.maxCarbonDioxidePartialPressureKPa = authored.maxCarbonDioxidePartialPressureKPa;
			atmosphereResult.startingOxygenKg = authored.startingOxygenKg;
			atmosphereResult.startingNitrogenKg = authored.startingNitrogenKg;
			atmosphereResult.startingCarbonDioxideKg = authored.startingCarbonDioxideKg;
			atmosphereResult.startingWaterVapourKg = authored.startingWaterVapourKg;
			atmosphereResult.startingTemperatureC = authored.startingTemperatureC;
			atmosphereResult.startingPressureKPa = compartment.startingPressureKPa;
			atmosphereResult.oxygenKg = compartment.oxygenKg;
			atmosphereResult.nitrogenKg = compartment.nitrogenKg;
			atmosphereResult.carbonDioxideKg = compartment.carbonDioxideKg;
			atmosphereResult.waterVapourKg = compartment.waterVapourKg;
			atmosphereResult.temperatureC = compartment.temperatureC;
			atmosphereResult.oxygenPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.oxygenKg, StationMolarMassOxygenKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.nitrogenPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.nitrogenKg, StationMolarMassNitrogenKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.carbonDioxidePartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.carbonDioxideKg, StationMolarMassCarbonDioxideKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.waterVapourPartialPressureKPa = stationAtmospherePartialPressureKPa(
				compartment.waterVapourKg, StationMolarMassWaterKgPerMol,
				authored.volumeM3, compartment.temperatureC);
			atmosphereResult.pressureKPa = atmosphereResult.oxygenPartialPressureKPa
				+ atmosphereResult.nitrogenPartialPressureKPa
				+ atmosphereResult.carbonDioxidePartialPressureKPa
				+ atmosphereResult.waterVapourPartialPressureKPa;
			atmosphereResult.saturationVapourPressureKPa = stationSaturationVapourPressureKPa(
				compartment.temperatureC);
			atmosphereResult.relativeHumidity = atmosphereResult.saturationVapourPressureKPa > 0.0
				? std::min(1.0, atmosphereResult.waterVapourPartialPressureKPa
					/ atmosphereResult.saturationVapourPressureKPa) : 0.0;
			atmosphereResult.lastOxygenDemandKg = compartment.lastOxygenDemandKg;
			atmosphereResult.lastOxygenConsumedKg = compartment.lastOxygenConsumedKg;
			atmosphereResult.lastOxygenUnmetKg = compartment.lastOxygenDemandKg
				- compartment.lastOxygenConsumedKg;
			atmosphereResult.lastCo2ProducedKg = compartment.lastCo2ProducedKg;
			atmosphereResult.lastVapourAddedKg = compartment.lastVapourAddedKg;
			atmosphereResult.lastVapourCondensedKg = compartment.lastVapourCondensedKg;
			atmosphereResult.lastLeakedKg = compartment.lastLeakedKg;
			atmosphereResult.lastCollectedKg = compartment.lastCollectedKg;
			atmosphereResult.collectedKg = compartment.collectedKg;
			atmosphereResult.lastElectrolysisOxygenKg = compartment.lastElectrolysisOxygenKg;
			atmosphereResult.electrolysisOxygenKg = compartment.electrolysisOxygenKg;
			atmosphereResult.lastCo2CapturedKg = compartment.lastCo2CapturedKg;
			atmosphereResult.co2CapturedKg = compartment.capturedCarbonDioxideKg;
			atmosphereResult.lastCycleRemovedKg = compartment.lastCycleRemovedKg;
			atmosphereResult.lastCycleRecoveredKg = compartment.lastCycleRecoveredKg;
			atmosphereResult.lastCycleLostKg = compartment.lastCycleLostKg;
			atmosphereResult.lastOccupantHeatKW = compartment.lastOccupantHeatKW;
			atmosphereResult.lastCrewHeatKW = compartment.lastCrewHeatKW;
			atmosphereResult.lastLatentHeatKW = compartment.lastLatentHeatKW;
			atmosphereResult.lastExchangeIntoCompartmentKW = compartment.lastExchangeIntoCompartmentKW;
			atmosphereResult.abovePressureLimit = compartment.lastAbovePressure;
			atmosphereResult.belowPressureLimit = compartment.lastBelowPressure;
			atmosphereResult.lowOxygen = compartment.lastLowOxygen;
			atmosphereResult.highCarbonDioxide = compartment.lastHighCo2;
			atmosphereResult.condensing = compartment.lastCondensing;
			atmosphereResult.thermallyCoupled = compartment.reservoirIndex != SIZE_MAX;
			atmosphereResult.oxygenConsumedKg = compartment.oxygenConsumedKg;
			atmosphereResult.co2ProducedKg = compartment.co2ProducedKg;
			atmosphereResult.vapourAddedKg = compartment.vapourAddedKg;
			atmosphereResult.condensedKg = compartment.condensedKg;
			atmosphereResult.leakedKg = compartment.leakedKg;
			atmosphereResult.recoveredKg = compartment.recoveredKg;
			atmosphereResult.lostKg = compartment.lostKg;
			atmosphereResult.startingEnergyKJ = authored.thermalCapacityKJPerK
				* authored.startingTemperatureC;
			atmosphereResult.currentEnergyKJ = authored.thermalCapacityKJPerK
				* compartment.temperatureC;
			atmosphereResult.occupantHeatEnergyKJ = compartment.occupantHeatKJ;
			atmosphereResult.crewHeatEnergyKJ = compartment.crewHeatKJ;
			atmosphereResult.latentHeatEnergyKJ = compartment.latentHeatKJ;
			atmosphereResult.exchangedEnergyKJ = compartment.exchangedKJ;
			atmosphereResult.residualKJ = atmosphereResult.startingEnergyKJ
				+ compartment.occupantHeatKJ + compartment.crewHeatKJ
				+ compartment.latentHeatKJ + compartment.exchangedKJ
				- atmosphereResult.currentEnergyKJ;
			atmosphereResult.status = compartment.status;
			result.atmospheres.push_back(std::move(atmosphereResult));
		}
		// The airlock cycle snapshot reports the authored schedule beside
		// the runtime's cycle history: what each occurrence removed and
		// how the recovered-versus-lost split landed (#400).
		for (auto const& cycle : mImpl->airlockCycles)
		{
			StationAirlockCycleSnapshot cycleResult;
			cycleResult.id = cycle.authored.id;
			cycleResult.atmosphereId = cycle.authored.atmosphereId;
			cycleResult.gasMassKg = cycle.authored.gasMassKg;
			cycleResult.firstOccurrenceSeconds = cycle.authored.firstOccurrenceSeconds;
			cycleResult.recurrenceSeconds = cycle.authored.recurrenceSeconds;
			cycleResult.recoveredFraction = cycle.authored.recoveredFraction;
			cycleResult.priority = cycle.authored.priority;
			cycleResult.occurrencesCompleted = cycle.occurrencesFired;
			cycleResult.lastOccurrenceSeconds = cycle.lastOccurrenceSeconds;
			cycleResult.lastRemovedKg = cycle.lastRemovedKg;
			cycleResult.lastRecoveredKg = cycle.lastRecoveredKg;
			cycleResult.lastLostKg = cycle.lastLostKg;
			cycleResult.removedTotal = cycle.removedTotal;
			cycleResult.recoveredTotal = cycle.recoveredTotal;
			cycleResult.lostTotal = cycle.lostTotal;
			cycleResult.exhausted = cycle.exhausted;
			cycleResult.status = cycle.status;
			if (!cycle.exhausted)
			{
				cycleResult.hasNextOccurrence = true;
				cycleResult.nextOccurrenceSeconds = cycle.authored.recurrenceSeconds > 0.0
					? cycle.authored.firstOccurrenceSeconds
						+ static_cast<double>(cycle.occurrencesFired) * cycle.authored.recurrenceSeconds
					: cycle.authored.firstOccurrenceSeconds;
			}
			result.airlockCycles.push_back(std::move(cycleResult));
		}
		// The crew snapshot reports the authored contract beside the last
		// tick's requested-versus-actual exchange on every metabolic
		// pathway, the uniform scale and its limiter, and the closing
		// consumption/return/retained ledger (#401).
		for (auto const& crew : mImpl->crews)
		{
			StationCrewSnapshot crewResult;
			crewResult.id = crew.authored.id;
			crewResult.atmosphereId = crew.authored.atmosphereId;
			crewResult.crewCount = crew.authored.crewCount;
			crewResult.foodStoreId = crew.authored.foodStoreId;
			crewResult.waterStoreId = crew.authored.waterStoreId;
			crewResult.urineStoreId = crew.authored.urineStoreId;
			crewResult.organicWasteStoreId = crew.authored.organicWasteStoreId;
			crewResult.activityLevel = crew.lastActivityLevel;
			crewResult.metabolicScale = crew.lastScale;
			crewResult.limiter = crew.lastLimiter;
			crewResult.lastFoodRequested = crew.lastFoodRequested;
			crewResult.lastFoodActual = crew.lastFoodActual;
			crewResult.lastFoodUnmet = crew.lastFoodRequested - crew.lastFoodActual;
			crewResult.lastWaterRequested = crew.lastWaterRequested;
			crewResult.lastWaterActual = crew.lastWaterActual;
			crewResult.lastWaterUnmet = crew.lastWaterRequested - crew.lastWaterActual;
			crewResult.lastOxygenRequestedKg = crew.lastOxygenRequestedKg;
			crewResult.lastOxygenActualKg = crew.lastOxygenActualKg;
			crewResult.lastOxygenUnmetKg = crew.lastOxygenRequestedKg - crew.lastOxygenActualKg;
			crewResult.lastCo2ProducedKg = crew.lastCo2ProducedKg;
			crewResult.lastMoistureProducedKg = crew.lastMoistureProducedKg;
			crewResult.lastUrineRequested = crew.lastUrineRequested;
			crewResult.lastUrineActual = crew.lastUrineActual;
			crewResult.lastUrineUnmet = crew.lastUrineRequested - crew.lastUrineActual;
			crewResult.lastOrganicWasteRequested = crew.lastOrganicWasteRequested;
			crewResult.lastOrganicWasteActual = crew.lastOrganicWasteActual;
			crewResult.lastOrganicWasteUnmet = crew.lastOrganicWasteRequested - crew.lastOrganicWasteActual;
			crewResult.lastHeatKW = crew.lastHeatKW;
			crewResult.foodConsumed = crew.foodConsumed;
			crewResult.waterConsumed = crew.waterConsumed;
			crewResult.oxygenConsumedKg = crew.oxygenConsumedKg;
			crewResult.co2ProducedKg = crew.co2ProducedKg;
			crewResult.moistureProducedKg = crew.moistureProducedKg;
			crewResult.urineProduced = crew.urineProduced;
			crewResult.organicWasteProduced = crew.organicWasteProduced;
			crewResult.heatKJ = crew.heatKJ;
			crewResult.retainedKg = crew.retainedKg;
			crewResult.disabled = crew.authored.crewCount <= 0;
			crewResult.status = crew.status;
			result.crews.push_back(std::move(crewResult));
		}
		// The water recovery snapshot reports the authored contract beside
		// the condensate actually collected, the cold-surface conditions
		// that gated it and the latent heat routed into the coolant loop, and
		// the treatment's processed/treated/concentrate split with its
		// quality hold state (#402).
		for (auto const& collector : mImpl->waterCollectors)
		{
			StationWaterCollectorSnapshot collectorResult;
			collectorResult.id = collector.authored.id;
			collectorResult.hazardClass = collector.authored.hazardClass;
			collectorResult.atmosphereId = collector.authored.atmosphereId;
			collectorResult.coolingLoopId = collector.authored.coolingLoopId;
			collectorResult.thermalClass = collector.authored.thermalClass;
			collectorResult.destStoreId = collector.authored.destStoreId;
			collectorResult.maxRateKgPerS = collector.authored.maxRateKgPerS;
			collectorResult.targetRelativeHumidity = collector.authored.targetRelativeHumidity;
			collectorResult.lastRequestedKg = collector.lastRequestedKg;
			collectorResult.lastCondensableKg = collector.lastCondensableKg;
			collectorResult.lastCollectedKg = collector.lastCollectedKg;
			collectorResult.lastLatentHeatKW = collector.lastLatentHeatKW;
			collectorResult.lastCirculationFraction = collector.lastCirculationFraction;
			collectorResult.coldSurfaceAvailable = collector.lastColdSurfaceAvailable;
			collectorResult.blockReason = collector.lastBlockReason;
			collectorResult.disabled = collector.authored.maxRateKgPerS <= 0.0;
			collectorResult.collectedKg = collector.collectedTotalKg;
			collectorResult.latentHeatEnergyKJ = collector.latentHeatRoutedKJ;
			collectorResult.status = collector.status;
			result.waterCollectors.push_back(std::move(collectorResult));
		}
		double totalWaterTreatmentPower = 0.0;
		for (auto const& treatment : mImpl->waterTreatments)
		{
			StationWaterTreatmentSnapshot treatmentResult;
			treatmentResult.id = treatment.authored.id;
			treatmentResult.hazardClass = treatment.authored.hazardClass;
			treatmentResult.sourceStoreId = treatment.authored.sourceStoreId;
			treatmentResult.treatedStoreId = treatment.authored.treatedStoreId;
			treatmentResult.concentrateStoreId = treatment.authored.concentrateStoreId;
			treatmentResult.consumableStoreId = treatment.authored.consumableStoreId;
			treatmentResult.throughputKgPerS = treatment.authored.throughputKgPerS;
			treatmentResult.recoveryRatio = treatment.authored.recoveryRatio;
			treatmentResult.specificEnergyKWhPerKg = treatment.authored.specificEnergyKWhPerKg;
			treatmentResult.consumablePerKg = treatment.authored.consumablePerKg;
			treatmentResult.priority = treatment.authored.priority;
			treatmentResult.scale = treatment.lastScale;
			treatmentResult.limiter = treatment.lastLimiter;
			treatmentResult.lastProcessedKg = treatment.lastProcessedKg;
			treatmentResult.lastTreatedKg = treatment.lastTreatedKg;
			treatmentResult.lastConcentrateKg = treatment.lastConcentrateKg;
			treatmentResult.lastPowerDemandKW = treatment.lastPowerDemandKW;
			treatmentResult.lastPowerSuppliedKW = treatment.lastPowerSuppliedKW;
			treatmentResult.lastPowerUnmetKW = treatment.lastPowerDemandKW
				- treatment.lastPowerSuppliedKW;
			treatmentResult.lastConsumableRequested = treatment.lastConsumableRequested;
			treatmentResult.lastConsumableUsed = treatment.lastConsumableUsed;
			treatmentResult.lastConsumableUnmet = treatment.lastConsumableRequested
				- treatment.lastConsumableUsed;
			treatmentResult.qualityAccepted = treatment.lastQualityAccepted;
			treatmentResult.holdReason = treatment.lastHoldReason;
			treatmentResult.disabled = treatment.authored.throughputKgPerS <= 0.0;
			treatmentResult.processedKg = treatment.processedTotalKg;
			treatmentResult.treatedKg = treatment.treatedTotalKg;
			treatmentResult.acceptedKg = treatment.acceptedTotalKg;
			treatmentResult.heldKg = treatment.heldTotalKg;
			treatmentResult.concentrateKg = treatment.concentrateTotalKg;
			treatmentResult.energySuppliedKJ = treatment.energySuppliedKJ;
			treatmentResult.energyOwedKJ = treatment.energyOwedKJ;
			treatmentResult.consumablesUsed = treatment.consumablesUsedTotal;
			treatmentResult.residualKg = treatment.processedTotalKg
				- treatment.treatedTotalKg - treatment.concentrateTotalKg;
			treatmentResult.status = treatment.status;
			totalWaterTreatmentPower += treatment.lastPowerSuppliedKW;
			result.waterTreatments.push_back(std::move(treatmentResult));
		}
		result.power.waterTreatmentPowerKW = totalWaterTreatmentPower;
		// The oxygen-production snapshot reports the authored cell contract
		// beside the water actually split, the products actually made, the
		// energy actually supplied and the closing mass and energy ledgers:
		// the split and the energy each close over their own terms, so a
		// reader can see where every kilogram and kilojoule went (#404).
		double totalElectrolyzerPower = 0.0;
		for (auto const& cell : mImpl->electrolyzers)
		{
			StationElectrolyzerSnapshot cellResult;
			cellResult.id = cell.authored.id;
			cellResult.hazardClass = cell.authored.hazardClass;
			cellResult.waterStoreId = cell.authored.waterStoreId;
			cellResult.oxygenStoreId = cell.authored.oxygenStoreId;
			cellResult.hydrogenStoreId = cell.authored.hydrogenStoreId;
			cellResult.atmosphereId = cell.authored.atmosphereId;
			cellResult.thermalClass = cell.authored.thermalClass;
			cellResult.heatSinkId = cell.authored.heatSinkId;
			cellResult.rateKgPerS = cell.authored.rateKgPerS;
			cellResult.specificEnergyKWhPerKgWater = cell.authored.specificEnergyKWhPerKgWater;
			cellResult.maxSinkTemperatureC = cell.authored.maxSinkTemperatureC;
			cellResult.priority = cell.authored.priority;
			cellResult.scale = cell.lastScale;
			cellResult.limiter = cell.lastLimiter;
			cellResult.lastRequestedKg = cell.lastRequestedKg;
			cellResult.lastSplitKg = cell.lastSplitKg;
			cellResult.lastOxygenKg = cell.lastOxygenKg;
			cellResult.lastHydrogenKg = cell.lastHydrogenKg;
			cellResult.lastPowerDemandKW = cell.lastPowerDemandKW;
			cellResult.lastPowerSuppliedKW = cell.lastPowerSuppliedKW;
			cellResult.lastPowerUnmetKW = cell.lastPowerDemandKW - cell.lastPowerSuppliedKW;
			cellResult.lastUnmetProductionKg = cell.lastUnmetProductionKg;
			cellResult.coolingAvailable = cell.lastCoolingAvailable;
			cellResult.blockReason = cell.lastBlockReason;
			cellResult.disabled = cell.authored.rateKgPerS <= 0.0;
			cellResult.waterSplitKg = cell.waterSplitKg;
			cellResult.oxygenProducedKg = cell.oxygenProducedKg;
			cellResult.hydrogenProducedKg = cell.hydrogenProducedKg;
			cellResult.energySuppliedKJ = cell.energySuppliedKJ;
			cellResult.chemicalEnergyKJ = cell.chemicalEnergyKJ;
			cellResult.heatRoutedKJ = cell.heatRoutedKJ;
			cellResult.residualKg = cell.waterSplitKg
				- cell.oxygenProducedKg - cell.hydrogenProducedKg;
			cellResult.residualKJ = cell.energySuppliedKJ
				- cell.chemicalEnergyKJ - cell.heatRoutedKJ;
			cellResult.status = cell.status;
			totalElectrolyzerPower += cell.lastPowerSuppliedKW;
			result.electrolyzers.push_back(std::move(cellResult));
		}
		result.power.electrolyzerPowerKW = totalElectrolyzerPower;
		// The CO2-capture snapshot reports the authored machine beside the
		// captured mass, the unmet atmospheric service and the electricity
		// and consumable actually drawn: the captured carbon is visible as
		// inventory, never as a vanishing act (#404).
		double totalCo2CapturePower = 0.0;
		for (auto const& capture : mImpl->co2Captures)
		{
			StationCo2CaptureSnapshot captureResult;
			captureResult.id = capture.authored.id;
			captureResult.hazardClass = capture.authored.hazardClass;
			captureResult.atmosphereId = capture.authored.atmosphereId;
			captureResult.captureStoreId = capture.authored.captureStoreId;
			captureResult.consumableStoreId = capture.authored.consumableStoreId;
			captureResult.maxRateKgPerS = capture.authored.maxRateKgPerS;
			captureResult.specificEnergyKWhPerKg = capture.authored.specificEnergyKWhPerKg;
			captureResult.consumablePerKg = capture.authored.consumablePerKg;
			captureResult.priority = capture.authored.priority;
			captureResult.scale = capture.lastScale;
			captureResult.limiter = capture.lastLimiter;
			captureResult.lastRequestedKg = capture.lastRequestedKg;
			captureResult.lastCapturedKg = capture.lastCapturedKg;
			captureResult.lastUnmetServiceKg = capture.lastRequestedKg - capture.lastCapturedKg;
			captureResult.lastPowerDemandKW = capture.lastPowerDemandKW;
			captureResult.lastPowerSuppliedKW = capture.lastPowerSuppliedKW;
			captureResult.lastPowerUnmetKW = capture.lastPowerDemandKW - capture.lastPowerSuppliedKW;
			captureResult.lastConsumableRequested = capture.lastConsumableRequested;
			captureResult.lastConsumableUsed = capture.lastConsumableUsed;
			captureResult.lastConsumableUnmet = capture.lastConsumableRequested
				- capture.lastConsumableUsed;
			captureResult.blockReason = capture.lastBlockReason;
			captureResult.disabled = capture.authored.maxRateKgPerS <= 0.0;
			captureResult.capturedKg = capture.capturedTotalKg;
			captureResult.energySuppliedKJ = capture.energySuppliedKJ;
			captureResult.consumablesUsed = capture.consumablesUsedTotal;
			captureResult.status = capture.status;
			totalCo2CapturePower += capture.lastPowerSuppliedKW;
			result.co2Captures.push_back(std::move(captureResult));
		}
		result.power.co2CapturePowerKW = totalCo2CapturePower;
		// The fuel-cell backup snapshot reports the authored contract beside
		// the output actually delivered, the reactants actually burned, the
		// product water actually delivered, the waste heat actually routed
		// and the endurance the remaining usable reactants still buy:
		// the backup is a finite service with a visible clock, never an
		// open-ended power setting (#406).
		double totalFuelCellOutput = 0.0, totalFuelCellWasteHeat = 0.0;
		for (auto const& cell : mImpl->fuelCells)
		{
			StationFuelCellSnapshot cellResult;
			cellResult.id = cell.authored.id;
			cellResult.hazardClass = cell.authored.hazardClass;
			cellResult.hydrogenStoreId = cell.authored.hydrogenStoreId;
			cellResult.oxygenStoreId = cell.authored.oxygenStoreId;
			cellResult.waterStoreId = cell.authored.waterStoreId;
			cellResult.thermalClass = cell.authored.thermalClass;
			cellResult.heatSinkId = cell.authored.heatSinkId;
			cellResult.ratedOutputKW = cell.authored.ratedOutputKW;
			cellResult.efficiency = cell.authored.efficiency;
			cellResult.commandedOutputKW = cell.authored.commandedOutputKW;
			cellResult.maxSinkTemperatureC = cell.authored.maxSinkTemperatureC;
			cellResult.lastOutputKW = cell.lastOutputKW;
			cellResult.lastHydrogenKg = cell.lastHydrogenKg;
			cellResult.lastOxygenKg = cell.lastOxygenKg;
			cellResult.lastWaterKg = cell.lastWaterKg;
			cellResult.lastWasteHeatKW = cell.lastWasteHeatKW;
			cellResult.lastUnmetOutputKW = cell.lastUnmetOutputKW;
			cellResult.sinkTemperatureC = mImpl->reservoirs[cell.sinkIndex].temperatureC;
			cellResult.coolingAvailable = cell.lastCoolingAvailable;
			cellResult.limiter = cell.lastLimiter;
			cellResult.blockReason = cell.lastBlockReason;
			cellResult.disabled = cell.authored.ratedOutputKW <= 0.0;
			cellResult.hydrogenConsumedKg = cell.hydrogenConsumedKg;
			cellResult.oxygenConsumedKg = cell.oxygenConsumedKg;
			cellResult.waterProducedKg = cell.waterProducedKg;
			cellResult.chemicalEnergyKJ = cell.chemicalEnergyKJ;
			cellResult.electricalKJ = cell.electricalKJ;
			cellResult.wasteHeatKJ = cell.wasteHeatKJ;
			cellResult.residualKg = cell.hydrogenConsumedKg + cell.oxygenConsumedKg
				- cell.waterProducedKg;
			cellResult.residualKJ = cell.chemicalEnergyKJ - cell.electricalKJ - cell.wasteHeatKJ;
			cellResult.enduranceSeconds = cell.enduranceSeconds;
			cellResult.status = cell.status;
			totalFuelCellOutput += cell.lastOutputKW;
			totalFuelCellWasteHeat += cell.lastWasteHeatKW;
			result.fuelCells.push_back(std::move(cellResult));
		}
		result.power.fuelCellOutputKW = totalFuelCellOutput;
		result.power.fuelCellWasteHeatKW = totalFuelCellWasteHeat;
		// The Sabatier snapshot reports the authored reactor contract
		// beside the CO2 and hydrogen actually consumed, the methane and
		// water actually produced, the energy actually supplied and the
		// heat actually routed: the conversion closes on mass and the
		// energy closes over supplied + released = routed, so a reader can
		// see where every kilogram and kilojoule went (#405).
		double totalSabatierPower = 0.0;
		for (auto const& reactor : mImpl->sabatierReactors)
		{
			StationSabatierReactorSnapshot reactorResult;
			reactorResult.id = reactor.authored.id;
			reactorResult.hazardClass = reactor.authored.hazardClass;
			reactorResult.co2StoreId = reactor.authored.co2StoreId;
			reactorResult.hydrogenStoreId = reactor.authored.hydrogenStoreId;
			reactorResult.methaneStoreId = reactor.authored.methaneStoreId;
			reactorResult.waterStoreId = reactor.authored.waterStoreId;
			reactorResult.thermalClass = reactor.authored.thermalClass;
			reactorResult.heatSinkId = reactor.authored.heatSinkId;
			reactorResult.rateKgPerS = reactor.authored.rateKgPerS;
			reactorResult.specificEnergyKWhPerKgCO2 = reactor.authored.specificEnergyKWhPerKgCO2;
			reactorResult.maxSinkTemperatureC = reactor.authored.maxSinkTemperatureC;
			reactorResult.priority = reactor.authored.priority;
			reactorResult.scale = reactor.lastScale;
			reactorResult.limiter = reactor.lastLimiter;
			reactorResult.lastRequestedKg = reactor.lastRequestedKg;
			reactorResult.lastCo2ConsumedKg = reactor.lastCo2ConsumedKg;
			reactorResult.lastHydrogenConsumedKg = reactor.lastHydrogenConsumedKg;
			reactorResult.lastMethaneProducedKg = reactor.lastMethaneProducedKg;
			reactorResult.lastWaterProducedKg = reactor.lastWaterProducedKg;
			reactorResult.lastPowerDemandKW = reactor.lastPowerDemandKW;
			reactorResult.lastPowerSuppliedKW = reactor.lastPowerSuppliedKW;
			reactorResult.lastPowerUnmetKW = reactor.lastPowerDemandKW - reactor.lastPowerSuppliedKW;
			reactorResult.lastUnmetConversionKg = reactor.lastUnmetConversionKg;
			reactorResult.lastReactionHeatKW = reactor.lastReactionHeatKW;
			reactorResult.coolingAvailable = reactor.lastCoolingAvailable;
			reactorResult.blockReason = reactor.lastBlockReason;
			reactorResult.disabled = reactor.authored.rateKgPerS <= 0.0;
			reactorResult.co2ConsumedKg = reactor.co2ConsumedKg;
			reactorResult.hydrogenConsumedKg = reactor.hydrogenConsumedKg;
			reactorResult.methaneProducedKg = reactor.methaneProducedKg;
			reactorResult.waterProducedKg = reactor.waterProducedKg;
			reactorResult.energySuppliedKJ = reactor.energySuppliedKJ;
			reactorResult.reactionHeatKJ = reactor.reactionHeatKJ;
			reactorResult.heatRoutedKJ = reactor.heatRoutedKJ;
			reactorResult.residualKg = reactor.co2ConsumedKg + reactor.hydrogenConsumedKg
				- reactor.methaneProducedKg - reactor.waterProducedKg;
			reactorResult.residualKJ = reactor.energySuppliedKJ + reactor.reactionHeatKJ
				- reactor.heatRoutedKJ;
			reactorResult.status = reactor.status;
			totalSabatierPower += reactor.lastPowerSuppliedKW;
			result.sabatierReactors.push_back(std::move(reactorResult));
		}
		result.power.sabatierPowerKW = totalSabatierPower;
		// The methane-processing snapshot reports the authored processor
		// contract beside the methane actually split, the hydrogen
		// actually returned and the solid carbon actually produced, with
		// the closing mass and energy ledgers: the recycled hydrogen is
		// visible as a booked return, never as free supply (#405).
		double totalMethaneProcessingPower = 0.0;
		for (auto const& processor : mImpl->methaneProcessors)
		{
			StationMethaneProcessorSnapshot processorResult;
			processorResult.id = processor.authored.id;
			processorResult.hazardClass = processor.authored.hazardClass;
			processorResult.methaneStoreId = processor.authored.methaneStoreId;
			processorResult.hydrogenStoreId = processor.authored.hydrogenStoreId;
			processorResult.carbonStoreId = processor.authored.carbonStoreId;
			processorResult.thermalClass = processor.authored.thermalClass;
			processorResult.heatSinkId = processor.authored.heatSinkId;
			processorResult.rateKgPerS = processor.authored.rateKgPerS;
			processorResult.specificEnergyKWhPerKgCH4 = processor.authored.specificEnergyKWhPerKgCH4;
			processorResult.maxSinkTemperatureC = processor.authored.maxSinkTemperatureC;
			processorResult.priority = processor.authored.priority;
			processorResult.scale = processor.lastScale;
			processorResult.limiter = processor.lastLimiter;
			processorResult.lastRequestedKg = processor.lastRequestedKg;
			processorResult.lastMethaneConsumedKg = processor.lastMethaneConsumedKg;
			processorResult.lastHydrogenReturnedKg = processor.lastHydrogenReturnedKg;
			processorResult.lastCarbonProducedKg = processor.lastCarbonProducedKg;
			processorResult.lastPowerDemandKW = processor.lastPowerDemandKW;
			processorResult.lastPowerSuppliedKW = processor.lastPowerSuppliedKW;
			processorResult.lastPowerUnmetKW = processor.lastPowerDemandKW - processor.lastPowerSuppliedKW;
			processorResult.lastUnmetProcessingKg = processor.lastUnmetProcessingKg;
			processorResult.lastReactionHeatKW = processor.lastReactionHeatKW;
			processorResult.coolingAvailable = processor.lastCoolingAvailable;
			processorResult.blockReason = processor.lastBlockReason;
			processorResult.disabled = processor.authored.rateKgPerS <= 0.0;
			processorResult.methaneConsumedKg = processor.methaneConsumedKg;
			processorResult.hydrogenReturnedKg = processor.hydrogenReturnedKg;
			processorResult.carbonProducedKg = processor.carbonProducedKg;
			processorResult.energySuppliedKJ = processor.energySuppliedKJ;
			processorResult.reactionHeatKJ = processor.reactionHeatKJ;
			processorResult.heatRoutedKJ = processor.heatRoutedKJ;
			processorResult.residualKg = processor.methaneConsumedKg
				- processor.hydrogenReturnedKg - processor.carbonProducedKg;
			processorResult.residualKJ = processor.energySuppliedKJ
				- processor.reactionHeatKJ - processor.heatRoutedKJ;
			processorResult.status = processor.status;
			totalMethaneProcessingPower += processor.lastPowerSuppliedKW;
			result.methaneProcessors.push_back(std::move(processorResult));
		}
		result.power.methaneProcessingPowerKW = totalMethaneProcessingPower;
		// The nutrient-recovery snapshot reports the authored processor
		// contract beside the feed, oxygen, process water and consumable
		// actually consumed, the recovered nutrients, recovered water, CO2
		// and residue actually produced, the energy actually supplied and
		// the heat actually routed: the digestion closes on mass with the
		// residue as the balance term, and the energy closes over supplied
		// + released = routed, so a reader can see where every kilogram
		// and kilojoule went. The per-constituent recovery totals show
		// the recovered nitrogen, phosphorus, potassium and grouped
		// minerals separately, funded only by the feed's bound nutrient
		// constituents (#407).
		double totalMicrobialProcessingPower = 0.0;
		for (auto const& processor : mImpl->microbialProcessors)
		{
			StationMicrobialProcessorSnapshot processorResult;
			processorResult.id = processor.authored.id;
			processorResult.hazardClass = processor.authored.hazardClass;
			processorResult.feedStoreId = processor.authored.feedStoreId;
			processorResult.oxygenStoreId = processor.authored.oxygenStoreId;
			processorResult.waterStoreId = processor.authored.waterStoreId;
			processorResult.consumableStoreId = processor.authored.consumableStoreId;
			processorResult.nutrientStoreId = processor.authored.nutrientStoreId;
			processorResult.recoveredWaterStoreId = processor.authored.recoveredWaterStoreId;
			processorResult.co2StoreId = processor.authored.co2StoreId;
			processorResult.residueStoreId = processor.authored.residueStoreId;
			processorResult.thermalClass = processor.authored.thermalClass;
			processorResult.heatSinkId = processor.authored.heatSinkId;
			processorResult.rateKgPerS = processor.authored.rateKgPerS;
			processorResult.specificEnergyKWhPerKgFeed = processor.authored.specificEnergyKWhPerKgFeed;
			processorResult.oxygenPerKgFeedKg = processor.authored.oxygenPerKgFeedKg;
			processorResult.processWaterPerKgFeedKg = processor.authored.processWaterPerKgFeedKg;
			processorResult.consumablePerKgFeed = processor.authored.consumablePerKgFeed;
			processorResult.co2PerKgFeedKg = processor.authored.co2PerKgFeedKg;
			processorResult.nitrogenRecovery = processor.authored.nitrogenRecovery;
			processorResult.phosphorusRecovery = processor.authored.phosphorusRecovery;
			processorResult.potassiumRecovery = processor.authored.potassiumRecovery;
			processorResult.mineralsRecovery = processor.authored.mineralsRecovery;
			processorResult.waterRecovery = processor.authored.waterRecovery;
			processorResult.reactionHeatKJPerKgFeed = processor.authored.reactionHeatKJPerKgFeed;
			processorResult.minOperatingTemperatureC = processor.authored.minOperatingTemperatureC;
			processorResult.maxSinkTemperatureC = processor.authored.maxSinkTemperatureC;
			processorResult.priority = processor.authored.priority;
			processorResult.scale = processor.lastScale;
			processorResult.limiter = processor.lastLimiter;
			processorResult.lastRequestedKg = processor.lastRequestedKg;
			processorResult.lastFeedConsumedKg = processor.lastFeedConsumedKg;
			processorResult.lastOxygenConsumedKg = processor.lastOxygenConsumedKg;
			processorResult.lastWaterConsumedKg = processor.lastWaterConsumedKg;
			processorResult.lastConsumableRequested = processor.lastConsumableRequested;
			processorResult.lastConsumableUsed = processor.lastConsumableUsed;
			processorResult.lastConsumableUnmet = processor.lastConsumableUnmet;
			processorResult.lastNutrientProducedKg = processor.lastNutrientProducedKg;
			processorResult.lastRecoveredWaterKg = processor.lastRecoveredWaterKg;
			processorResult.lastCo2ProducedKg = processor.lastCo2ProducedKg;
			processorResult.lastResidueProducedKg = processor.lastResidueProducedKg;
			processorResult.lastPowerDemandKW = processor.lastPowerDemandKW;
			processorResult.lastPowerSuppliedKW = processor.lastPowerSuppliedKW;
			processorResult.lastPowerUnmetKW = processor.lastPowerDemandKW - processor.lastPowerSuppliedKW;
			processorResult.lastUnmetProcessingKg = processor.lastUnmetProcessingKg;
			processorResult.lastReactionHeatKW = processor.lastReactionHeatKW;
			processorResult.temperatureEligible = processor.lastTemperatureEligible;
			processorResult.blockReason = processor.lastBlockReason;
			processorResult.disabled = processor.authored.rateKgPerS <= 0.0;
			processorResult.feedConsumedKg = processor.feedConsumedKg;
			processorResult.oxygenConsumedKg = processor.oxygenConsumedKg;
			processorResult.waterConsumedKg = processor.waterConsumedKg;
			processorResult.consumablesUsed = processor.consumablesUsed;
			processorResult.nutrientProducedKg = processor.nutrientProducedKg;
			processorResult.recoveredWaterKg = processor.recoveredWaterKg;
			processorResult.co2ProducedKg = processor.co2ProducedKg;
			processorResult.residueProducedKg = processor.residueProducedKg;
			processorResult.nitrogenRecoveredKg = processor.nitrogenRecoveredKg;
			processorResult.phosphorusRecoveredKg = processor.phosphorusRecoveredKg;
			processorResult.potassiumRecoveredKg = processor.potassiumRecoveredKg;
			processorResult.mineralsRecoveredKg = processor.mineralsRecoveredKg;
			processorResult.energySuppliedKJ = processor.energySuppliedKJ;
			processorResult.reactionHeatKJ = processor.reactionHeatKJ;
			processorResult.heatRoutedKJ = processor.heatRoutedKJ;
			processorResult.residualKg = processor.feedConsumedKg + processor.oxygenConsumedKg
				+ processor.waterConsumedKg - processor.nutrientProducedKg
				- processor.recoveredWaterKg - processor.co2ProducedKg
				- processor.residueProducedKg;
			processorResult.residualKJ = processor.energySuppliedKJ + processor.reactionHeatKJ
				- processor.heatRoutedKJ;
			processorResult.status = processor.status;
			totalMicrobialProcessingPower += processor.lastPowerSuppliedKW;
			result.microbialProcessors.push_back(std::move(processorResult));
		}
		result.power.microbialProcessingPowerKW = totalMicrobialProcessingPower;
		// The delivery snapshot reports the authored schedule beside the
		// runtime's arrival history: what each occurrence brought in, what
		// the destination refused, and when the next arrival is due (#392).
		// The station-wide next-delivery readout is the soonest pending
		// occurrence across every schedule.
		for (auto const& delivery : mImpl->deliveries)
		{
			StationDeliverySnapshot deliveryResult;
			deliveryResult.id = delivery.authored.id;
			deliveryResult.storeId = delivery.authored.storeId;
			deliveryResult.unit = delivery.authored.unit;
			deliveryResult.quantity = delivery.authored.quantity;
			deliveryResult.firstOccurrenceSeconds = delivery.authored.firstOccurrenceSeconds;
			deliveryResult.recurrenceSeconds = delivery.authored.recurrenceSeconds;
			deliveryResult.arrivesQualified = delivery.authored.arrivesQualified;
			deliveryResult.priority = delivery.authored.priority;
			deliveryResult.occurrencesCompleted = delivery.occurrencesFired;
			deliveryResult.lastOccurrenceSeconds = delivery.lastOccurrenceSeconds;
			deliveryResult.lastScheduled = delivery.lastScheduled;
			deliveryResult.lastAccepted = delivery.lastAccepted;
			deliveryResult.lastRejected = delivery.lastRejected;
			deliveryResult.acceptedTotal = delivery.acceptedTotal;
			deliveryResult.rejectedTotal = delivery.rejectedTotal;
			deliveryResult.exhausted = delivery.exhausted;
			deliveryResult.status = delivery.status;
			if (!delivery.exhausted)
			{
				deliveryResult.hasNextOccurrence = true;
				deliveryResult.nextOccurrenceSeconds = delivery.authored.recurrenceSeconds > 0.0
					? delivery.authored.firstOccurrenceSeconds
						+ static_cast<double>(delivery.occurrencesFired) * delivery.authored.recurrenceSeconds
					: delivery.authored.firstOccurrenceSeconds;
				if (!result.hasNextDelivery || deliveryResult.nextOccurrenceSeconds < result.nextDeliverySeconds)
				{
					result.hasNextDelivery = true;
					result.nextDeliverySeconds = deliveryResult.nextOccurrenceSeconds;
				}
			}
			result.deliveries.push_back(std::move(deliveryResult));
		}
		// The waste ejection snapshot reports the authored schedule beside
		// the runtime's departure history: what each occurrence actually
		// ejected as external loss, what stayed behind held or absent, and
		// when the next ejection is due (#403). The station-wide
		// next-ejection readout is the soonest pending occurrence across
		// every ejection schedule.
		for (auto const& ejection : mImpl->wasteEjections)
		{
			StationWasteEjectionSnapshot ejectionResult;
			ejectionResult.id = ejection.authored.id;
			ejectionResult.storeId = ejection.authored.storeId;
			ejectionResult.unit = ejection.authored.unit;
			ejectionResult.quantity = ejection.authored.quantity;
			ejectionResult.firstOccurrenceSeconds = ejection.authored.firstOccurrenceSeconds;
			ejectionResult.recurrenceSeconds = ejection.authored.recurrenceSeconds;
			ejectionResult.priority = ejection.authored.priority;
			ejectionResult.occurrencesCompleted = ejection.occurrencesFired;
			ejectionResult.lastOccurrenceSeconds = ejection.lastOccurrenceSeconds;
			ejectionResult.lastScheduled = ejection.lastScheduled;
			ejectionResult.lastEjected = ejection.lastEjected;
			ejectionResult.lastUnmet = ejection.lastUnmet;
			ejectionResult.ejectedTotal = ejection.ejectedTotal;
			ejectionResult.unmetTotal = ejection.unmetTotal;
			ejectionResult.exhausted = ejection.exhausted;
			ejectionResult.status = ejection.status;
			if (!ejection.exhausted)
			{
				ejectionResult.hasNextOccurrence = true;
				ejectionResult.nextOccurrenceSeconds = ejection.authored.recurrenceSeconds > 0.0
					? ejection.authored.firstOccurrenceSeconds
						+ static_cast<double>(ejection.occurrencesFired) * ejection.authored.recurrenceSeconds
					: ejection.authored.firstOccurrenceSeconds;
				if (!result.hasNextWasteEjection
					|| ejectionResult.nextOccurrenceSeconds < result.nextWasteEjectionSeconds)
				{
					result.hasNextWasteEjection = true;
					result.nextWasteEjectionSeconds = ejectionResult.nextOccurrenceSeconds;
				}
			}
			result.wasteEjections.push_back(std::move(ejectionResult));
		}
		return result;
	}
	std::vector<StationSystemsEvent> const& StationSystemsRuntime::events() const { return mImpl->events; }
	std::vector<StationSystemsDiagnostic> const& StationSystemsRuntime::diagnostics() const { return mImpl->diagnostics; }
}
