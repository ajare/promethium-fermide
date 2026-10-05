#include "core/ActionRegistry.h"

#include <fstream>
#include <set>
#include <utility>
#include "core/AgentAction.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/SerializationException.h"
#include "ScriptSandbox.h"

namespace core
{
	namespace
	{
		struct Invocation
		{
			std::string const& source;
			std::string* uuid;
			std::vector<ActionDefinition>* definitions;
			std::string_view identity;
			ActionViews const* views;
			script::LogStaging& logs;
			std::set<std::string> keys, names;
			std::string key, name;
		};

		int log(lua_State* state)
		{
			auto* staging = static_cast<script::LogStaging*>(lua_touserdata(state, lua_upvalueindex(1)));
			size_t length{};
			auto const* message = luaL_checklstring(state, 1, &length);
			// Host allocation exceptions must not escape a Lua C function.
			bool failed = false;
			try { staging->stageLog(LogLevel::Info, message, length); }
			catch (...) { failed = true; }
			if (failed) return luaL_error(state, "Action logging allocation failed");
			return 0;
		}

		void stringField(lua_State* state, char const* field, std::string const& value)
		{
			lua_pushlstring(state, value.data(), value.size());
			lua_setfield(state, -2, field);
		}
		void integerField(lua_State* state, char const* field, uint64_t value)
		{
			lua_pushinteger(state, static_cast<lua_Integer>(value));
			lua_setfield(state, -2, field);
		}

		// This trampoline only borrows owning host records. All Lua allocations,
		// module evaluation, contract conversion and callback calls are protected.
		int invoke(lua_State* state)
		{
			auto& call = *static_cast<Invocation*>(lua_touserdata(state, 1));
			lua_settop(state, 0);
			script::pushPrivateEnvironment(state);
			auto const environment = lua_gettop(state);
			if (luaL_loadbufferx(state, call.source.data(), call.source.size(), "@actions.lua", "t") != LUA_OK)
				return lua_error(state);
			lua_pushvalue(state, environment);
			lua_setupvalue(state, -2, 1);
			lua_call(state, 0, 1);
			if (!lua_istable(state, -1)) return luaL_error(state, "Action registry must return a table");
			auto const registry = lua_gettop(state);
			lua_getfield(state, registry, "api_version");
			if (!lua_isinteger(state, -1) || lua_tointeger(state, -1) != 1)
				return luaL_error(state, "Action registry requires api_version = 1");
			lua_pop(state, 1);
			lua_getfield(state, registry, "uuid");
			if (lua_type(state, -1) != LUA_TSTRING) return luaL_error(state, "Action registry UUID must be a string");
			size_t length{};
			auto text = lua_tolstring(state, -1, &length);
			call.uuid->assign(text, length);
			lua_pop(state, 1);
			if (!AgentBehaviourRegistry::uuidIsValid(*call.uuid)) return luaL_error(state, "Invalid Action registry UUID");
			lua_getfield(state, registry, "actions");
			if (!lua_istable(state, -1)) return luaL_error(state, "Action registry actions must be an ordered array");
			auto const actions = lua_gettop(state);
			auto const count = lua_rawlen(state, actions);
			if (count > 256) return luaL_error(state, "Too many Actions in registry");
			// Reject holes and non-array entries, rather than silently ignoring them.
			lua_pushnil(state);
			size_t entries = 0;
			while (lua_next(state, actions))
			{
				if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1
					|| static_cast<size_t>(lua_tointeger(state, -2)) > count)
					return luaL_error(state, "Action definitions must be an ordered array");
				++entries;
				lua_pop(state, 1);
			}
			if (entries != count) return luaL_error(state, "Action array contains holes");
			int selected = LUA_NOREF;
			for (size_t i = 1; i <= count; ++i)
			{
				lua_rawgeti(state, actions, static_cast<lua_Integer>(i));
				if (!lua_istable(state, -1)) return luaL_error(state, "Action definition must be a table");
				lua_getfield(state, -1, "key");
				if (lua_type(state, -1) != LUA_TSTRING) return luaL_error(state, "Action key must be a string");
				text = lua_tolstring(state, -1, &length);
				call.key.assign(text, length);
				lua_pop(state, 1);
				lua_getfield(state, -1, "name");
				if (lua_type(state, -1) != LUA_TSTRING) return luaL_error(state, "Action name must be a string");
				text = lua_tolstring(state, -1, &length);
				call.name.assign(text, length);
				lua_pop(state, 1);
				if (call.key.empty() || call.key.size() > 128 || call.key.find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
					return luaL_error(state, "Invalid stable Action key");
				if (call.name.empty() || call.name.size() > 128 || call.name.find('\0') != std::string::npos)
					return luaL_error(state, "Invalid Action display name");
				if (call.key == IdleAction || call.key == UseFurnitureAction
					|| call.name == "Idle" || call.name == "Use furniture")
					return luaL_error(state, "Built-in Actions cannot be replaced");
				if (!call.keys.insert(call.key).second || !call.names.insert(call.name).second)
					return luaL_error(state, "Duplicate Action key or display name");
				lua_getfield(state, -1, "run");
				if (!lua_isfunction(state, -1) || lua_iscfunction(state, -1))
					return luaL_error(state, "Action run must be a Lua function");
				for (int upvalue = 1; ; ++upvalue)
				{
					auto const* name = lua_getupvalue(state, -1, upvalue);
					if (!name) break;
					bool const allowed = std::string_view(name) == "_ENV";
					lua_pop(state, 1);
					if (!allowed) return luaL_error(state, "Action functions cannot capture module upvalues");
				}
				if (call.definitions) call.definitions->push_back({call.key, call.name});
				if (call.views && call.identity == *call.uuid + ":" + call.key)
				{
					lua_pushvalue(state, -1);
					selected = luaL_ref(state, LUA_REGISTRYINDEX);
				}
				lua_pop(state, 2);
			}
			if (!call.views) return 0;
			if (selected == LUA_NOREF) return luaL_error(state, "Unavailable Action identity");
			lua_rawgeti(state, LUA_REGISTRYINDEX, selected);
			lua_newtable(state);
			stringField(state, "name", call.views->agentName);
			integerField(state, "id", call.views->agentId);
			lua_pushnumber(state, call.views->x); lua_setfield(state, -2, "x");
			lua_pushnumber(state, call.views->y); lua_setfield(state, -2, "y");
			script::pushImmutableProxy(state);
			lua_newtable(state);
			stringField(state, "name", call.views->worldName);
			integerField(state, "tick", call.views->tick);
			integerField(state, "api_version", 1);
			lua_pushlightuserdata(state, &call.logs);
			lua_pushcclosure(state, log, 1); lua_setfield(state, -2, "log");
			script::pushImmutableProxy(state);
			lua_newtable(state);
			stringField(state, "name", call.views->markerName);
			integerField(state, "id", call.views->markerId);
			script::pushImmutableProxy(state);
			lua_call(state, 3, 0);
			return 0;
		}

		ActionExecutionResult evaluate(Invocation& invocation)
		{
			script::ScratchBudget budget(2 * 1024 * 1024, 100'000);
			std::unique_ptr<lua_State, script::StateCloser> owner(lua_newstate(script::budgetedAllocate, &budget));
			if (!owner) return { false, ScriptExecutionFailure::MemoryBudgetExceeded, "Cannot allocate Action sandbox" };
			auto* state = owner.get();
			lua_atpanic(state, script::luaPanic);
			script::ModuleLoader loader;
			loader.hostModuleVersions.emplace("prometheum.actions.v1", 1);
			std::string diagnostic;
			try
			{
				if (script::runScratchSetup(state, &loader, nullptr, diagnostic) != LUA_OK)
					return { false, script::failureKind(budget), diagnostic };
				lua_settop(state, 0);
				lua_pushcfunction(state, invoke);
				lua_pushlightuserdata(state, &invocation);
				auto result = script::protectedCall(state, budget, 1, 0);
				if (result.diagnostic.size() > 2048) result.diagnostic.resize(2048);
				return { result.succeeded, result.failure, std::move(result.diagnostic) };
			}
			catch (std::exception const& error)
			{
				return { false, script::failureKind(budget), error.what() };
			}
		}
	}

	std::shared_ptr<const ActionRegistry> ActionRegistry::load(std::filesystem::path const& path)
	{
		if (path.filename().string().size() <= 12 || !path.filename().string().ends_with(".actions.lua"))
			throw SerializationException("Action registry must end with .actions.lua");
		std::ifstream file(path, std::ios::binary);
		if (!file) throw SerializationException("Missing Action registry: " + path.string());
		auto registry = std::make_shared<ActionRegistry>();
		char buffer[4096];
		while (file.read(buffer, sizeof(buffer)) || file.gcount())
		{
			registry->mSource.append(buffer, static_cast<size_t>(file.gcount()));
			if (registry->mSource.size() > 256 * 1024) throw SerializationException("Action registry source exceeds budget");
		}
		if (file.bad()) throw SerializationException("Cannot read Action registry");
		script::LogStaging logs;
		Invocation invocation{registry->mSource, &registry->mUuid, &registry->mActions, {}, nullptr, logs, {}, {}, {}, {}};
		auto result = evaluate(invocation);
		if (!result.succeeded) throw SerializationException("Invalid Action registry: " + result.diagnostic);
		return registry;
	}

	ActionDefinition const* ActionRegistry::find(std::string_view id) const
	{
		for (auto const& action : mActions) if (id == identity(action)) return &action;
		return nullptr;
	}

	ActionExecutionResult ActionRegistry::execute(std::string_view id, ActionViews const& views) const
	{
		if (!find(id)) return { false, ScriptExecutionFailure::ConversionError, "Unavailable Action identity" };
		script::LogStaging logs;
		logs.logMessageCountLimit = 32;
		logs.logMessageByteLimit = 1024;
		logs.logStagingByteLimit = 8192;
		std::string uuid;
		Invocation invocation{mSource, &uuid, nullptr, id, &views, logs, {}, {}, {}, {}};
		auto result = evaluate(invocation);
		if (result.succeeded)
		{
			for (auto const& message : logs.logs) addLogMessage("Marker Action", 0, message.level, message.message);
			if (logs.logsSuppressed) addLogMessage("Marker Action", 0, LogLevel::Warning, "Action logging budget exhausted");
		}
		return result;
	}
}
