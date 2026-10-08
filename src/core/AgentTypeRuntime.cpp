#include "core/AgentTypeRuntime.h"
#include "core/AgentType.h"
#include "core/SerializationException.h"
#include "ScriptSandbox.h"

#include <cmath>
#include <cstring>
#include <utility>

namespace core
{
	namespace
	{
		using namespace script;

		// A C++-visible extraction of the type object returned by one `.agent.lua`
		// source. Preflight fills this without constructing an instance.
		struct TypeObjectView
		{
			std::string typeId;
			std::string displayName;
		};

		// Reads a bounded, NUL-free Lua string field into `out`. Returns false and
		// leaves the value on the stack untouched when it is not a valid string.
		bool readStringField(lua_State* state, int index, size_t maxLength,
			std::string& out)
		{
			size_t length = 0;
			auto const* text = lua_tolstring(state, index, &length);
			if (!text || length == 0 || length > maxLength
				|| std::memchr(text, '\0', length) != nullptr)
				return false;
			out.assign(text, length);
			return true;
		}

		void pushConversionError(lua_State* state, std::string const& diagnostic)
		{
			pushBorrowedDiagnostic(state, diagnostic);
		}

		// Scratch preflight body. Runs inside lua_pcall so an allocation failure
		// becomes a recoverable error status instead of reaching the panic handler.
		struct PreflightConversion
		{
			std::string source;
			TypeObjectView type;
			std::string diagnostic;
		};

		int preflightEvaluate(lua_State* state)
		{
			auto& call = *static_cast<PreflightConversion*>(
				lua_touserdata(state, 1));
			lua_settop(state, 0);
			pushPrivateEnvironment(state);
			if (luaL_loadbufferx(state, call.source.data(), call.source.size(),
				"@agent.lua", "t") != LUA_OK)
				return lua_error(state);
			lua_pushvalue(state, 1);
			lua_setupvalue(state, -2, 1);
			lua_call(state, 0, 1);

			if (!lua_istable(state, -1))
			{
				call.diagnostic = "Agent type must return a table";
				pushConversionError(state, call.diagnostic);
				return lua_error(state);
			}
			auto const typeTable = lua_absindex(state, -1);

			lua_pushliteral(state, "api_version");
			lua_rawget(state, typeTable);
			auto const apiVersion = lua_isinteger(state, -1)
				? lua_tointeger(state, -1) : 0;
			lua_pop(state, 1);
			if (apiVersion != 1)
			{
				call.diagnostic = "Agent type requires api_version = 1";
				pushConversionError(state, call.diagnostic);
				return lua_error(state);
			}

			lua_pushliteral(state, "type_id");
			lua_rawget(state, typeTable);
			auto const idOk = readStringField(state, -1, 128, call.type.typeId)
				&& agentTypeIdIsValid(call.type.typeId);
			lua_pop(state, 1);
			if (!idOk)
			{
				call.diagnostic = "Agent type has an invalid type_id";
				pushConversionError(state, call.diagnostic);
				return lua_error(state);
			}

			lua_pushliteral(state, "display_name");
			lua_rawget(state, typeTable);
			auto const nameOk = readStringField(state, -1, 128, call.type.displayName)
				&& agentTypeDisplayNameIsValid(call.type.displayName);
			lua_pop(state, 1);
			if (!nameOk)
			{
				call.diagnostic = "Agent type has an invalid display_name";
				pushConversionError(state, call.diagnostic);
				return lua_error(state);
			}

			lua_pushliteral(state, "new");
			lua_rawget(state, typeTable);
			auto const newOk = lua_isfunction(state, -1)
				&& !lua_iscfunction(state, -1);
			lua_pop(state, 1);
			if (!newOk)
			{
				call.diagnostic = "Agent type requires a Lua new() constructor";
				pushConversionError(state, call.diagnostic);
				return lua_error(state);
			}
			return 0;
		}

		// Shared, validated baseline extraction from the instance table returned
		// by new(). Throws SerializationException with the offending field named.
		AgentPhysicalBaseline readBaseline(lua_State* state, int instance)
		{
			instance = lua_absindex(state, instance);
			AgentPhysicalBaseline baseline{};
			struct Field
			{
				char const* key;
				float AgentPhysicalBaseline::* destination;
				bool ratio;
			};
			static constexpr Field fields[] = {
				{ "width", &AgentPhysicalBaseline::width, false },
				{ "standing_height", &AgentPhysicalBaseline::standingHeight, false },
				{ "reach", &AgentPhysicalBaseline::reach, false },
				{ "walk_speed", &AgentPhysicalBaseline::walkSpeed, false },
				{ "climb_speed", &AgentPhysicalBaseline::climbSpeed, false },
				{ "stair_ascent_speed", &AgentPhysicalBaseline::stairAscentSpeed, false },
				{ "stair_descent_speed", &AgentPhysicalBaseline::stairDescentSpeed, false },
				{ "sitting_height_ratio", &AgentPhysicalBaseline::sittingHeightRatio, true },
				{ "crouching_height_ratio", &AgentPhysicalBaseline::crouchingHeightRatio, true },
				{ "crawling_height_ratio", &AgentPhysicalBaseline::crawlingHeightRatio, true },
				{ "crawling_speed_ratio", &AgentPhysicalBaseline::crawlingSpeedRatio, true },
			};
			for (auto const& field : fields)
			{
				lua_pushstring(state, field.key);
				lua_rawget(state, instance);
				auto const missing = lua_isnil(state, -1);
				auto const value = lua_tonumber(state, -1);
				auto const isNumber = !missing && lua_isnumber(state, -1);
				auto const valid = isNumber && std::isfinite(value)
					&& value > 0.0f
					&& (!field.ratio || value <= 1.0f);
				lua_pop(state, 1);
				if (!valid)
				{
					auto const reason = missing
						? "is missing"
						: (!isNumber
							? "must be a finite number"
							: (!std::isfinite(value)
								? "must be finite"
								: (value <= 0.0f
									? "must be positive"
									: "must be within (0, 1]")));
					throw SerializationException(std::string("Agent type baseline field '")
						+ field.key + "' " + reason);
				}
				baseline.*field.destination = static_cast<float>(value);
			}
			return baseline;
		}
	}

	struct AgentTypeRuntimeAdapter::Impl : std::enable_shared_from_this<Impl>
	{
		ScratchBudget budget;
		// Package loader retained as a member because the sandbox's require
		// closure borrows its address. Agent types expose no host modules.
		ModuleLoader hostLoader;
		bool stateFailed{ false };
		std::unique_ptr<lua_State, StateCloser> state;

		explicit Impl(AgentTypeRuntimeLimits limits)
			: budget(limits.memoryBytes, limits.instructionsPerCall)
			, state(lua_newstate(budgetedAllocate, &budget))
		{
			if (!state) throw std::runtime_error("Could not create Agent type Lua runtime");
			lua_atpanic(state.get(), luaPanic);
			hostLoader.packageName = "World Agent types";
			std::string message;
			auto const setupStatus = runScratchSetup(state.get(), &hostLoader,
				[](lua_State*) {}, message);
			if (setupStatus != LUA_OK)
				throw std::runtime_error(
					"Could not initialize Agent type Lua runtime: " + message);
		}

		// Executes the type source in a fresh private environment, calls new(),
		// validates and freezes the baseline, and retains the live instance as a
		// registry reference owned by the returned handle.
		AgentTypeConstructResult construct(std::string_view typeId,
			std::string_view source, std::string_view name)
		{
			AgentTypeConstructResult result;
			if (stateFailed)
			{
				result.failure = ScriptExecutionFailure::LuaError;
				result.diagnostic = "Agent type Lua runtime is unavailable";
				return result;
			}
			auto* lua = state.get();
			auto const base = lua_gettop(lua);
			try
			{
				budget.memoryLimitExceeded = false;
				budget.instructionLimitExceeded = false;

				auto const loadStatus = luaL_loadbufferx(lua, source.data(),
					source.size(), "@agent.lua", "t");
				if (loadStatus != LUA_OK)
				{
					auto const* message = lua_tostring(lua, -1);
					result.failure = loadStatus == LUA_ERRMEM
						|| budget.memoryLimitExceeded
							? ScriptExecutionFailure::MemoryBudgetExceeded
							: ScriptExecutionFailure::LuaError;
					result.diagnostic = message ? message : "Agent type module load failed";
					lua_settop(lua, base);
					return result;
				}
				auto const chunk = lua_gettop(lua);
				pushPrivateEnvironment(lua);
				auto const environment = lua_gettop(lua);
				lua_pushvalue(lua, environment);
				if (!lua_setupvalue(lua, chunk, 1))
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = "Agent type module has no isolated environment";
					lua_settop(lua, base);
					return result;
				}
				lua_remove(lua, environment);

				auto const moduleLoaded = protectedCall(lua, budget, 0, 1);
				if (!moduleLoaded.succeeded)
				{
					result.failure = moduleLoaded.failure;
					result.diagnostic = moduleLoaded.diagnostic;
					lua_settop(lua, base);
					return result;
				}
				if (!lua_istable(lua, -1))
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = "Agent type must return a table";
					lua_settop(lua, base);
					return result;
				}
				auto const typeTable = lua_gettop(lua);

				// Verify the declared type ID matches the resolved definition. A
				// mismatched source must never silently publish another type.
				lua_pushliteral(lua, "type_id");
				lua_rawget(lua, typeTable);
				std::string declaredId;
				auto const idOk = readStringField(lua, -1, 128, declaredId);
				lua_pop(lua, 1);
				if (!idOk || declaredId != typeId)
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = idOk
						? "Agent type source declares type_id '" + declaredId
							+ "' but the resolved definition expects '" + std::string(typeId) + "'"
						: "Agent type source has an invalid type_id";
					lua_settop(lua, base);
					return result;
				}

				lua_pushliteral(lua, "new");
				lua_rawget(lua, typeTable);
				if (!lua_isfunction(lua, -1) || lua_iscfunction(lua, -1))
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = "Agent type requires a Lua new() constructor";
					lua_settop(lua, base);
					return result;
				}
				auto const constructorCalled = protectedCall(lua, budget, 0, 1);
				if (!constructorCalled.succeeded)
				{
					result.failure = constructorCalled.failure;
					result.diagnostic = constructorCalled.diagnostic;
					lua_settop(lua, base);
					return result;
				}
				if (!lua_istable(lua, -1))
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = "Agent type new() must return an instance table";
					lua_settop(lua, base);
					return result;
				}

				// Freeze the baseline before publishing: copy it out of Lua so
				// later mutations of the live instance cannot change simulation.
				try
				{
					result.baseline = readBaseline(lua, -1);
				}
				catch (SerializationException const& error)
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = error.what();
					lua_settop(lua, base);
					return result;
				}

				auto const instanceReference = luaL_ref(lua, LUA_REGISTRYINDEX);
				lua_settop(lua, base);
				if (instanceReference == LUA_NOREF || instanceReference == LUA_REFNIL)
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = "Agent type instance could not be retained";
					return result;
				}
				// Document history may replace the World while a surviving instance
				// still belongs to this state. Keep its allocator and loader alive
				// until the last handle releases its registry reference.
				result.instance = std::shared_ptr<void>(nullptr,
					[owner = shared_from_this(), instanceReference](void*)
				{
					luaL_unref(owner->state.get(), LUA_REGISTRYINDEX, instanceReference);
				});
				result.succeeded = true;
				(void)name;
				return result;
			}
			catch (LuaPanicError const& error)
			{
				// A panic has reset the Lua thread; the state is no longer safe.
				stateFailed = true;
				result.failure = ScriptExecutionFailure::MemoryBudgetExceeded;
				result.diagnostic = error.what();
				lua_settop(lua, base);
				return result;
			}
			catch (std::exception const& error)
			{
				result.failure = ScriptExecutionFailure::LuaError;
				result.diagnostic = error.what();
				lua_settop(lua, base);
				return result;
			}
		}
	};

	AgentTypeRuntimeAdapter::AgentTypeRuntimeAdapter(AgentTypeRuntimeLimits limits)
		: mImpl(std::make_shared<Impl>(limits))
	{
	}

	AgentTypeRuntimeAdapter::~AgentTypeRuntimeAdapter() = default;

	AgentTypePreflight AgentTypeRuntimeAdapter::preflightType(
		std::string resourceName, std::string_view source)
	{
		(void)resourceName;
		AgentTypePreflight result;
		try
		{
			ScratchBudget budget(2u * 1024u * 1024u, 100'000u);
			std::unique_ptr<lua_State, StateCloser> state(
				lua_newstate(budgetedAllocate, &budget));
			if (!state)
			{
				result.failure = ScriptExecutionFailure::MemoryBudgetExceeded;
				result.diagnostic = "Cannot allocate Agent type sandbox";
				return result;
			}
			lua_atpanic(state.get(), luaPanic);
			ModuleLoader loader;
			loader.packageName = "Agent type preflight";
			std::string message;
			if (runScratchSetup(state.get(), &loader, nullptr, message) != LUA_OK)
			{
				result.failure = ScriptExecutionFailure::LuaError;
				result.diagnostic = message;
				return result;
			}
			PreflightConversion call{ std::string(source), {}, {} };
			lua_settop(state.get(), 0);
			lua_pushcfunction(state.get(), preflightEvaluate);
			lua_pushlightuserdata(state.get(), &call);
			auto const callResult = protectedCall(state.get(), budget, 1, 0);
			if (!callResult.succeeded)
			{
				result.failure = callResult.failure;
				result.diagnostic = callResult.diagnostic.substr(0, 2048);
				return result;
			}
			result.loaded = true;
			result.typeId = std::move(call.type.typeId);
			result.displayName = std::move(call.type.displayName);
		}
		catch (std::exception const& error)
		{
			result.failure = ScriptExecutionFailure::LuaError;
			result.diagnostic = error.what();
		}
		return result;
	}

	AgentTypeConstructResult AgentTypeRuntimeAdapter::construct(
		std::string_view typeId, std::string_view source, std::string_view name)
	{
		return mImpl->construct(typeId, source, name);
	}

	AgentTypeRuntimeLimits AgentTypeRuntimeAdapter::getLimits() const
	{
		return { mImpl->budget.byteLimit, mImpl->budget.instructionLimit };
	}
}
