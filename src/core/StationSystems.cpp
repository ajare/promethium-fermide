#include "StationSystemsRuntime.h"
#include "core/World.h"
#include "core/SerializationException.h"
#include "BundledStationSystemsSource.h"
#include "ScriptSandbox.h"
#include <algorithm>
#include <cmath>
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
					lua_createtable(state, 0, 5);
					lua_pushstring(state, store.id.c_str()); lua_setfield(state, -2, "id");
					lua_pushstring(state, store.unit.c_str()); lua_setfield(state, -2, "unit");
					lua_pushnumber(state, store.capacity); lua_setfield(state, -2, "capacity");
					lua_pushnumber(state, store.startingQualified); lua_setfield(state, -2, "qualified");
					lua_pushnumber(state, store.startingUnqualified); lua_setfield(state, -2, "unqualified");
					lua_rawseti(state, -2, static_cast<int>(index) + 1);
				}
				lua_call(state, 2, 1);
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
		};
		StationSystemsSettings settings;
		std::vector<StoreState> stores;
		std::array<std::unique_ptr<ScriptObject>, 4> objects;
		StationSystemsSnapshot snapshot;
		std::vector<StationSystemsEvent> events;
		std::vector<StationSystemsDiagnostic> diagnostics;
	};
	StationSystemsRuntime::StationSystemsRuntime(StationSystemsSettings settings) : mImpl(std::make_unique<Impl>())
	{
		std::string diagnostic;
		if (!validateStationSystemsSettings(settings, diagnostic)) throw SerializationException(diagnostic);
		mImpl->settings = settings; mImpl->snapshot.enabled = settings.enabled;
		for (auto const& store : settings.stores)
			mImpl->stores.push_back({store, store.startingQualified, store.startingUnqualified});
		if (!settings.enabled) return;
		auto const source = sourceLoader ? sourceLoader() : bundledStationSystemsSource();
		if (source.empty() || source.size() > 64 * 1024) throw SerializationException("Invalid Station systems source size");
		for (size_t i = 0; i < mImpl->objects.size(); ++i)
		{
			auto object = std::make_unique<ScriptObject>();
			Construction construction{object.get(), &source, StationSystemNames[i], &settings.stores};
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
			+ std::to_string(settings.stores.size()) + " resource store(s)"});
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
		for (auto const& store : mImpl->stores)
			result.stores.push_back({store.authored.id, store.authored.unit,
				store.authored.capacity, store.authored.startingQualified,
				store.authored.startingUnqualified, store.currentQualified, store.currentUnqualified});
		return result;
	}
	std::vector<StationSystemsEvent> const& StationSystemsRuntime::events() const { return mImpl->events; }
	std::vector<StationSystemsDiagnostic> const& StationSystemsRuntime::diagnostics() const { return mImpl->diagnostics; }
}
