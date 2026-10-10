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
				lua_call(state, 6, 1);
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
		struct LoadExchangeState
		{
			size_t storeIndex{0};
			double ratePerSecond{0.0};
			bool consumes{true};
			double lastRequested{0.0};
			double lastCompleted{0.0};
		};
		struct LoadState
		{
			EquipmentLoadSettings authored;
			std::vector<LoadExchangeState> exchanges;
			double lastProfileLevel{0.0};
			double lastRequestedKW{0.0};
			double lastSuppliedKW{0.0};
			double lastUnmetKW{0.0};
			double lastUnmetCriticalKW{0.0};
			double lastDeliveredFraction{0.0};
			double lastHeatExportedKW{0.0};
			double lastUnmetCoolingKW{0.0};
			bool lastAdmitted{false};
			std::string lastBlockReason;
			std::string lastShortfallReason;
			bool lastCriticalUnmet{false};
			StationTransferStatus status{StationTransferStatus::Idle};
		};
		StationSystemsSettings settings;
		std::vector<StoreState> stores;
		std::vector<TransferState> transfers;
		std::vector<ConversionState> conversions;
		std::vector<SolarState> solarArrays;
		std::vector<LoadState> loads;
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
		// Per-constituent totals at run start and the cumulative declared
		// conversion losses, the compound conservation boundary (#391):
		// current + lost must equal starting within tolerance.
		ConstituentLedger startingConstituents;
		ConstituentLedger constituentLoss;
		// Generation left untaken after this tick's allocation, for the
		// power snapshot's surplus reporting.
		double lastAvailableGenerationKW{0.0};
		std::array<std::unique_ptr<ScriptObject>, 4> objects;
		StationSystemsSnapshot snapshot;
		std::vector<StationSystemsEvent> events;
		std::vector<StationSystemsDiagnostic> diagnostics;

		// Apply every authored transfer and conversion to a staged copy of
		// the inventory in the declared deterministic order â transfers
		// first, then conversions, each by priority descending and
		// declaration order â audit the staged result against finite
		// limits, the per-unit conservation boundary and the per-constituent
		// compound boundary, and only then commit. A violation never clamps
		// material into existence or out of it: it fails the whole tick
		// through the integrated error path.
		bool applyLedger(uint64_t tick, float seconds, std::string& violation)
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
			// Solar supply and external equipment allocation run before the
			// resource ledger: the electrical service a load receives bounds
			// its heat and material exchanges, and allocated power can never
			// exceed the generation actually available this tick.
			auto const elapsed = static_cast<double>(tick) * World::getFixedTimestep();
			double availableGeneration = 0.0;
			for (auto& array : solarArrays)
			{
				array.lastIllumination = evaluateStationProfile(array.authored.illumination, elapsed);
				array.lastDisabled = array.authored.capacityKW <= 0.0;
				array.lastAvailableKW = array.lastDisabled
					? 0.0 : array.authored.capacityKW * array.lastIllumination;
				array.lastEclipse = !array.lastDisabled && array.lastAvailableKW <= 0.0;
				availableGeneration += array.lastAvailableKW;
			}
			std::vector<double> supplied(loads.size(), 0.0);
			// Pass 1: protected critical floors allocate ahead of every
			// discretionary request, in the declared priority order. A
			// floor that generation cannot meet remains explicit unmet
			// critical demand, never a silently permitted curtailment.
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
				if (!load.lastAdmitted || load.lastRequestedKW <= 0.0) continue;
				auto const floor = std::min(load.authored.criticalFloorKW, load.lastRequestedKW);
				auto const give = std::min(floor, availableGeneration);
				supplied[index] += give;
				availableGeneration -= give;
				load.lastUnmetCriticalKW = floor - give;
			}
			// Pass 2: discretionary demand in the same declared order. A
			// load is never supplied below its minimum operable level,
			// demand * (1 - allowed curtailment): if the remaining pool
			// cannot reach that level the load takes nothing this tick.
			for (auto const index : loadOrder)
			{
				auto& load = loads[index];
				if (!load.lastAdmitted || load.lastRequestedKW <= 0.0) continue;
				auto const remaining = load.lastRequestedKW - supplied[index];
				if (remaining <= 0.0) continue;
				auto const candidate = std::min(remaining, availableGeneration);
				auto const minOperating = load.lastRequestedKW * (1.0 - load.authored.maxCurtailment);
				if (supplied[index] + candidate + 1e-12 >= minOperating)
				{
					supplied[index] += candidate;
					availableGeneration -= candidate;
				}
				else
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
				// the delivered fraction. A cooling-infeasible load never
				// runs, so it produces no station heat; its cooling need is
				// reported as unmet cooling instead of being absorbed.
				load.lastHeatExportedKW = load.lastAdmitted
					? load.authored.heatOutputKW * load.lastDeliveredFraction : 0.0;
				load.lastUnmetCoolingKW = load.lastAdmitted
					? 0.0 : load.authored.heatOutputKW * load.lastProfileLevel;
				load.lastCriticalUnmet = load.lastUnmetCriticalKW > 1e-12;
				if (load.lastRequestedKW <= 0.0) load.status = StationTransferStatus::Idle;
				else if (!load.lastAdmitted || supplied[index] <= 1e-12)
					load.status = StationTransferStatus::Rejected;
				else if (supplied[index] >= load.lastRequestedKW - 1e-12)
					load.status = StationTransferStatus::Full;
				else load.status = StationTransferStatus::Partial;
				// Declared material exchanges scale with the delivered power
				// fraction: curtailed equipment consumes and delivers less,
				// never draws into a protected reserve and never overfills a
				// destination. Completed amounts book at the external
				// boundary — draws are losses, deliveries are negative
				// losses — so the station ledgers stay closed.
				for (auto& exchange : load.exchanges)
				{
					auto const& exchangeStore = stores[exchange.storeIndex].authored;
					auto const requested = exchange.ratePerSecond * static_cast<double>(seconds)
						* load.lastDeliveredFraction;
					double completed = 0.0;
					if (exchange.consumes)
					{
						auto const drawable = std::max(0.0,
							qualified[exchange.storeIndex] - exchangeStore.protectedReserve);
						completed = std::min(requested, drawable);
						qualified[exchange.storeIndex] -= completed;
						flowOut[exchange.storeIndex] += completed;
					}
					else
					{
						auto const headroom = exchangeStore.capacity
							- (qualified[exchange.storeIndex] + unqualified[exchange.storeIndex]);
						completed = std::min(requested, headroom);
						qualified[exchange.storeIndex] += completed;
						flowIn[exchange.storeIndex] += completed;
					}
					exchange.lastRequested = requested;
					exchange.lastCompleted = completed;
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
			lastAvailableGenerationKW = availableGeneration;
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
				ConstituentLedger netChange;
				for (auto& flow : conversion.inputs)
				{
					auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
					auto const completed = requested * scale;
					qualified[flow.storeIndex] -= completed;
					flow.lastRequested = requested;
					flow.lastCompleted = completed;
					addComposition(netChange, *stores[flow.storeIndex].authored.composition,
						massToKg(stores[flow.storeIndex].authored.unit, completed), -1.0);
				}
				for (auto& flow : conversion.outputs)
				{
					auto const requested = flow.ratePerSecond * static_cast<double>(seconds);
					auto const completed = requested * scale;
					qualified[flow.storeIndex] += completed;
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
				if (qualified[index] < 0.0 || unqualified[index] < 0.0)
					return violation = "transfer drove inventory negative in store '" + stores[index].authored.id + "'", false;
				if (qualified[index] + unqualified[index] > stores[index].authored.capacity)
					return violation = "transfer exceeded capacity of store '" + stores[index].authored.id + "'", false;
				currentTotals[stores[index].authored.unit] += qualified[index] + unqualified[index];
			}
			for (auto const& [unit, starting] : startingTotals)
			{
				auto const residual = currentTotals[unit] + stagedUnitLoss[unit] - starting;
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
					+ stagedLoss[constituent] - starting;
				if (!std::isfinite(residual)
					|| std::abs(residual) > StationBalanceTolerance * std::max(1.0, std::abs(starting)))
					return violation = "constituent conservation residual " + std::to_string(residual)
						+ " for '" + constituent + "' exceeds tolerance "
						+ std::to_string(StationBalanceTolerance), false;
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
			// Rejections and partial completions are visible: announce each
			// status transition (after the commit) so the event log records
			// where material stalled without flooding every tick.
			for (auto const index : transferOrder)
			{
				auto const& transfer = transfers[index];
				if (transfer.status == previousStatus[index]) continue;
				if (transfer.status == StationTransferStatus::Full)
				{
					events.push_back({tick, "Transfer '" + transfer.authored.id + "' supplying fully"});
					continue;
				}
				auto const& reason = stallReason[index];
				auto const message = transfer.status == StationTransferStatus::Rejected
					? std::string("Transfer '") + transfer.authored.id + "' rejected: " + reason
					: std::string("Transfer '") + transfer.authored.id + "' partial: " + reason;
				events.push_back({tick, message});
			}
			for (auto const index : conversionOrder)
			{
				auto const& conversion = conversions[index];
				if (conversion.status == previousConversionStatus[index]) continue;
				if (conversion.status == StationTransferStatus::Full)
				{
					events.push_back({tick, "Conversion '" + conversion.authored.id
						+ "' running at full throughput"});
					continue;
				}
				auto const message = conversion.status == StationTransferStatus::Rejected
					? std::string("Conversion '") + conversion.authored.id + "' rejected: " + conversion.stallReason
					: std::string("Conversion '") + conversion.authored.id + "' partial: " + conversion.stallReason;
				events.push_back({tick, message});
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
						events.push_back({tick, "Load '" + load.authored.id + "' supplying fully"});
					else if (load.status == StationTransferStatus::Rejected)
						events.push_back({tick, "Load '" + load.authored.id + "' rejected: "
							+ (!load.lastAdmitted ? load.lastBlockReason
								: (load.lastShortfallReason.empty()
									? std::string("electrical supply is unavailable")
									: load.lastShortfallReason))});
					else
						events.push_back({tick, "Load '" + load.authored.id + "' curtailed: supplied "
							+ std::to_string(load.lastSuppliedKW) + " of "
							+ std::to_string(load.lastRequestedKW) + " kW"});
				}
				if (load.lastCriticalUnmet != previousCriticalUnmet[index])
					events.push_back({tick, load.lastCriticalUnmet
						? "Load '" + load.authored.id + "' critical floor unmet: short "
							+ std::to_string(load.lastUnmetCriticalKW) + " kW"
						: std::string("Load '") + load.authored.id + "' critical floor restored"});
			}
			for (size_t index = 0; index < solarArrays.size(); ++index)
			{
				auto const& array = solarArrays[index];
				if (array.lastEclipse == previousEclipse[index] || array.lastDisabled) continue;
				events.push_back({tick, array.lastEclipse
					? "Solar array '" + array.authored.id + "' entered eclipse"
					: "Solar array '" + array.authored.id + "' returned to sunlight"});
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
		for (auto const& load : settings.equipmentLoads)
		{
			Impl::LoadState state;
			state.authored = load;
			for (auto const& exchange : load.materialExchanges)
				state.exchanges.push_back({storeIndex[exchange.storeId], exchange.ratePerSecond,
					exchange.consumes});
			mImpl->loads.push_back(std::move(state));
		}
		mImpl->loadOrder.resize(mImpl->loads.size());
		std::iota(mImpl->loadOrder.begin(), mImpl->loadOrder.end(), size_t{0});
		std::stable_sort(mImpl->loadOrder.begin(), mImpl->loadOrder.end(),
			[&](size_t left, size_t right)
			{ return mImpl->loads[left].authored.priority > mImpl->loads[right].authored.priority; });
		// The compound conservation boundary is fixed at run start from the
		// authored starting inventory of every compound store (#391).
		for (auto const& store : mImpl->stores)
			if (store.authored.composition)
				addComposition(mImpl->startingConstituents, *store.authored.composition,
					massToKg(store.authored.unit,
						store.authored.startingQualified + store.authored.startingUnqualified), 1.0);
		if (!settings.enabled) return;
		auto const source = sourceLoader ? sourceLoader() : bundledStationSystemsSource();
		if (source.empty() || source.size() > 64 * 1024) throw SerializationException("Invalid Station systems source size");
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto object = std::make_unique<ScriptObject>();
			Construction construction{object.get(), &source, StationSystemNames[i],
				&settings.stores, &settings.transfers, &settings.conversions,
				&settings.solarArrays, &settings.equipmentLoads};
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
		mImpl->events.push_back({0, "Station systems initialized with "
			+ std::to_string(settings.stores.size()) + " resource store(s), "
			+ std::to_string(settings.transfers.size()) + " transfer(s), "
			+ std::to_string(settings.conversions.size()) + " conversion(s), "
			+ std::to_string(settings.solarArrays.size()) + " solar array(s) and "
			+ std::to_string(settings.equipmentLoads.size()) + " external equipment load(s)"});
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
			|| !mImpl->loads.empty() || !mImpl->solarArrays.empty())
		{
			std::string violation;
			if (!mImpl->applyLedger(tick, seconds, violation))
			{
				mImpl->snapshot.healthy = false;
				mImpl->diagnostics.push_back({tick, "Resource transfer",
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
			if (mImpl->events.size() == EventLimit) mImpl->events.erase(mImpl->events.begin());
			mImpl->events.push_back({tick, "All four Station systems completed the World tick"});
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
			result.balances.push_back({unit, starting, currentTotals[unit], lost,
				currentTotals[unit] + lost - starting});
		}
		for (auto const& [constituent, starting] : mImpl->startingConstituents)
		{
			auto const current = currentConstituents.count(constituent)
				? currentConstituents.at(constituent) : 0.0;
			auto const lost = mImpl->constituentLoss.count(constituent)
				? mImpl->constituentLoss.at(constituent) : 0.0;
			result.constituentBalances.push_back({constituent,
				std::string(constituent == StationEnergyConstituent ? "kJ" : "kg"),
				starting, current, lost, current + lost - starting});
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
			loadResult.heatExportedKW = load.lastHeatExportedKW;
			loadResult.unmetCoolingKW = load.lastUnmetCoolingKW;
			loadResult.admitted = load.lastAdmitted;
			loadResult.blockReason = load.lastBlockReason;
			loadResult.status = load.status;
			for (auto const& exchange : load.exchanges)
				loadResult.materialExchanges.push_back({mImpl->stores[exchange.storeIndex].authored.id,
					mImpl->stores[exchange.storeIndex].authored.unit,
					exchange.lastRequested, exchange.lastCompleted});
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
		return result;
	}
	std::vector<StationSystemsEvent> const& StationSystemsRuntime::events() const { return mImpl->events; }
	std::vector<StationSystemsDiagnostic> const& StationSystemsRuntime::diagnostics() const { return mImpl->diagnostics; }
}
