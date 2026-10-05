#include "core/AgentBehaviourRuntime.h"

#include <algorithm>
#include <bit>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <sol/sol.hpp>

#include "ScriptSandbox.h"

#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/Marker.h"
#include "core/Log.h"
#include "core/Simulation.h"
#include "core/Sector.h"

namespace core
{
	namespace
	{
		using namespace script;

		bool limitsAreValid(AgentBehaviourRuntimeLimits limits)
		{
			return limits.memoryBytes != 0 && limits.instructionsPerCall != 0
				&& limits.timersPerInstance != 0 && limits.callbacksPerBoundary != 0
				&& limits.commandsPerCallback != 0
				&& limits.logMessagesPerWindow != 0 && limits.logWindowTicks != 0
				&& limits.logBytesPerMessage != 0 && limits.logBytesPerWindow != 0;
		}

		void ensureOpaqueMetatables(lua_State* state);

		AgentBehaviourModulePreflight failure(std::string_view packageName,
			std::string_view moduleName, std::string traceback,
			std::string_view summary = {},
			AgentBehaviourRuntimeFailure kind = AgentBehaviourRuntimeFailure::LuaError)
		{
			auto const chunkName = std::format("{}/{}", packageName, moduleName);
			auto const line = diagnosticLine(traceback, chunkName);
			AgentBehaviourModulePreflight result;
			result.failure = kind;
			result.diagnostic = std::format("Agent behaviour package '{}', module '{}', line {}: {}",
				packageName, moduleName, line,
				summary.empty() ? std::string_view(traceback) : summary);
			result.traceback = std::move(traceback);
			return result;
		}

		bool isCallback(sol::object const& object)
		{
			return object.get_type() == sol::type::nil
				|| object.get_type() == sol::type::function;
		}
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightModule(
		std::string_view packageName, std::string_view moduleName,
		std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits)
	{
		return preflightModule(packageName, moduleName, source, helpers, limits, true);
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightModuleContract(
		std::string_view packageName, std::string_view moduleName,
		std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits)
	{
		return preflightModule(packageName, moduleName, source, helpers, limits, false);
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightModule(
		std::string_view packageName, std::string_view moduleName,
		std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits, bool invokeFactory)
	{
		auto const normalizedPackage = packageName.empty()
			? std::string("<unknown package>") : std::string(packageName);
		auto const normalizedModule = moduleName.empty()
			? std::string("<unknown module>") : std::string(moduleName);
		auto const chunkName = normalizedPackage + "/" + normalizedModule;
		if (!limitsAreValid(limits))
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: Lua runtime limits must be nonzero", {},
				AgentBehaviourRuntimeFailure::ConversionError);
		}
		if (limits.memoryBytes
			< AgentBehaviourRuntimeAdapter::MinimumMemoryBudgetBytes)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: Lua memory budget is below the minimum required to initialize the sandbox",
				std::format("Agent behaviour memory budget of {} bytes is below the {} byte minimum required to initialize the Lua sandbox",
					limits.memoryBytes,
					AgentBehaviourRuntimeAdapter::MinimumMemoryBudgetBytes),
				AgentBehaviourRuntimeFailure::ConversionError);
		}
		if (!source.empty() && static_cast<unsigned char>(source.front()) == 0x1b)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: precompiled Lua bytecode is not accepted",
				"precompiled Lua bytecode is not accepted; use text source");
		}

		ScratchBudget budget(limits.memoryBytes, limits.instructionsPerCall);
		std::unique_ptr<lua_State, StateCloser> ownedState(
			lua_newstate(budgetedAllocate, &budget));
		if (!ownedState)
		{
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: could not create the budgeted Lua scratch state");
		}
		// Never let an unprotected allocation terminate the process: the panic
		// handler throws a catchable error instead of Lua's default abort.
		lua_atpanic(ownedState.get(), luaPanic);

		try
		{
			auto* state = ownedState.get();
			sol::state_view lua(state);
			ModuleLoader loader;
			loader.hostModuleVersions = {
				{ "prometheum.v1", 1 }, { "prometheum.v2", 2 } };
			loader.packageName = normalizedPackage;
			for (auto const& helper : helpers)
				loader.modules.emplace(helper.name, helper);
			std::string setupMessage;
			auto const setupStatus = runScratchSetup(state, &loader, nullptr,
				setupMessage);
			if (setupStatus != LUA_OK)
			{
				auto const kind = (setupStatus == LUA_ERRMEM
					|| budget.memoryLimitExceeded)
					? AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
					: AgentBehaviourRuntimeFailure::LuaError;
				auto const summary = kind
						== AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
					? std::string("Agent behaviour memory budget is insufficient to initialize the Lua sandbox")
					: std::string{};
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: "
						+ (summary.empty() ? setupMessage : summary),
					summary.empty() ? setupMessage : summary, kind);
			}
			auto const environment = lua_gettop(state);
			lua_pushlightuserdata(state, &loader);
			lua_pushcclosure(state, requireDeclaredModule, 1);
			lua_setfield(state, environment, "require");
			lua_pushvalue(state, environment);
			loader.environmentReference = luaL_ref(state, LUA_REGISTRYINDEX);
			lua_pop(state, 1);
			loader.dependencyChain.push_back(normalizedModule);
			auto errorHandler = lua["__prometheum_traceback"];

			auto loaded = lua.load_buffer(source.data(), source.size(), "@" + chunkName,
				sol::load_mode::text);
			if (!loaded.valid())
			{
				sol::error error = loaded;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}

			sol::protected_function moduleChunk = loaded;
			moduleChunk.push();
			lua_rawgeti(state, LUA_REGISTRYINDEX, loader.environmentReference);
			if (!lua_setupvalue(state, -2, 1))
			{
				lua_pop(state, 1);
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module has no isolated environment");
			}
			lua_pop(state, 1);
			moduleChunk.set_error_handler(errorHandler);
			beginInstructionBudget(state, budget);
			auto moduleResult = moduleChunk();
			endInstructionBudget(state);
			loader.dependencyChain.clear();
			if (!moduleResult.valid())
			{
				sol::error error = moduleResult;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}
			// A protected call inside the module may have swallowed the budget
			// error, so success alone does not clear the sticky budget flags.
			if (budgetExhausted(budget))
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: " + budgetDiagnostic(budget),
					budgetDiagnostic(budget), failureKind(budget));
			sol::object exports = moduleResult.get<sol::object>();
			if (exports.get_type() != sol::type::table)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module must return a contract table",
					"module must return a contract table");
			}

			auto contract = exports.as<sol::table>();
			auto apiVersion = contract.raw_get<sol::object>("api_version");
			if (!apiVersion.is<lua_Integer>()
				|| (apiVersion.as<lua_Integer>() != 1
					&& apiVersion.as<lua_Integer>() != HostApiVersion))
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module must declare api_version = 1 or 2",
					"module must declare API version 1 or 2 as api_version");
			}
			auto factoryObject = contract.raw_get<sol::object>("factory");
			if (factoryObject.get_type() != sol::type::function)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: module contract is missing its factory",
					"module contract must provide a factory function");
			}
			// Registry package preflight validates the module/API contract only.
			// Hot reload invokes this factory with every real authored
			// configuration in fresh per-World candidate runtimes.
			if (!invokeFactory)
			{
				AgentBehaviourModulePreflight result;
				result.loaded = true;
				return result;
			}

			auto const configurationReference = createImmutableProxy(state, false);
			lua_rawgeti(state, LUA_REGISTRYINDEX, configurationReference);
			sol::object configuration = sol::stack::get<sol::object>(state, -1);
			lua_pop(state, 1);
			sol::protected_function factory = factoryObject.as<sol::protected_function>();
			factory.set_error_handler(errorHandler);
			beginInstructionBudget(state, budget);
			auto factoryResult = factory(configuration);
			endInstructionBudget(state);
			luaL_unref(state, LUA_REGISTRYINDEX, configurationReference);
			if (!factoryResult.valid())
			{
				sol::error error = factoryResult;
				return failure(normalizedPackage, normalizedModule, error.what(), {},
					failureKind(budget));
			}
			if (budgetExhausted(budget))
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: " + budgetDiagnostic(budget),
					budgetDiagnostic(budget), failureKind(budget));
			sol::object instanceObject = factoryResult.get<sol::object>();
			if (instanceObject.get_type() != sol::type::table)
			{
				return failure(normalizedPackage, normalizedModule,
					chunkName + ":1: factory must return an instance table",
					"factory must return an instance table");
			}

			auto instance = instanceObject.as<sol::table>();
			for (auto const* callback : { "on_start", "on_event", "on_timer",
				"on_route_lost", "on_stop" })
			{
				auto value = instance.raw_get<sol::object>(callback);
				if (!isCallback(value))
				{
					return failure(normalizedPackage, normalizedModule,
						chunkName + ":1: invalid instance callback",
						std::format("instance field '{}' must be a function when present",
							callback));
				}
			}

			AgentBehaviourModulePreflight result;
			result.loaded = true;
			return result;
		}
		catch (std::exception const& error)
		{
			endInstructionBudget(ownedState.get());
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: " + error.what(), error.what(),
				failureKind(budget, AgentBehaviourRuntimeFailure::ConversionError));
		}
		catch (...)
		{
			endInstructionBudget(ownedState.get());
			return failure(normalizedPackage, normalizedModule,
				chunkName + ":1: unknown sol2 conversion failure",
				"unknown sol2 conversion failure",
				failureKind(budget, AgentBehaviourRuntimeFailure::ConversionError));
		}
	}

	AgentBehaviourModulePreflight AgentBehaviourRuntimeAdapter::preflightHelperModule(
		std::string_view packageName, std::string_view helperName,
		std::string_view moduleName, std::string_view source,
		std::vector<AgentBehaviourHelperSource> const& helpers,
		AgentBehaviourRuntimeLimits limits)
	{
		std::vector<AgentBehaviourHelperSource> graph = helpers;
		auto found = std::find_if(graph.begin(), graph.end(),
			[helperName](AgentBehaviourHelperSource const& candidate)
			{ return candidate.name == helperName; });
		if (found == graph.end())
			graph.push_back({ std::string(helperName), std::string(moduleName),
				std::string(source) });
		else
		{
			found->sourceModulePath = moduleName;
			found->source = source;
		}
		// Helper roots have no behaviour contract. A tiny text root imports the
		// declared module through exactly the same loader and budget used by a
		// behaviour, so syntax, nested imports, cycles, and module execution are
		// validated without inventing a second loading path.
		auto wrapper = std::format(
			"local helper = require(\"{}\")\n"
			"return {{ api_version = 1, factory = function() return {{}} end }}\n",
			helperName);
		return preflightModule(packageName, moduleName, wrapper, graph, limits);
	}

	namespace
	{
		constexpr char MarkerMetatable[] = "prometheum.v1.marker";
		constexpr char AgentMetatable[] = "prometheum.v1.agent";
		constexpr char SectorMetatable[] = "prometheum.v1.sector";
		constexpr char InteractionMetatable[] = "prometheum.v1.interaction";

		struct MarkerHandle
		{
			MarkerId marker;
		};

		struct AgentHandle
		{
			AgentId agent;
		};

		struct SectorHandle
		{
			SectorId sector;
		};

		struct InteractionHandle
		{
			InteractionRequestId interaction;
		};

		enum class PendingMovementCommandType { MoveTo, Cancel };

		struct PendingMovementCommand
		{
			PendingMovementCommandType type{ PendingMovementCommandType::MoveTo };
			AgentId agent;
			MarkerId marker;
			std::string action{ IdleAction };
		};

		enum class PendingAuthorizationCommandType
		{
			GrantAccessPermission,
			RevokeAccessPermission,
			AssignPermissionSet,
			UnassignPermissionSet
		};

		struct PendingAuthorizationCommand
		{
			PendingAuthorizationCommandType type;
			AgentId agent;
			AccessPermissionId permission;
			PermissionSetId permissionSet;
		};

		struct CallbackScope : LogStaging
		{
			bool active{ false };
			bool movementCommandIssued{ false };
			AgentId agent;
			uint32_t commandCount{ 0 };
			uint32_t commandLimit{ AgentBehaviourRuntimeAdapter::DefaultCommandsPerCallback };
			uint32_t apiVersion{ 1 };
			std::function<MarkerId(std::string_view)> resolveMarker;
			std::function<MovementCommandResult(MarkerId, std::string_view)> inspectMove;
			std::function<MovementCommandResult()> inspectCancel;
			std::function<bool(std::string, uint64_t, std::string&)> setTimer;
			std::function<bool(std::string const&)> cancelTimer;
			std::function<uint64_t()> nextRandom;
			std::function<bool(PendingAuthorizationCommandType,
				std::string_view, std::string&)> changeAuthorization;
			std::map<std::string, uint64_t> stagedTimers;
			uint64_t stagedRandomState{ 0 };
			std::vector<PendingMovementCommand> commands;
			std::vector<PendingAuthorizationCommand> authorizationCommands;
		};

		void countCommand(lua_State* state, CallbackScope& scope)
		{
			if (scope.commandCount >= scope.commandLimit)
				luaL_error(state, "Agent behaviour command limit of %d per callback exceeded",
					static_cast<int>(scope.commandLimit));
			++scope.commandCount;
		}

		void pushCommandResult(lua_State* state, bool accepted, std::string_view status)
		{
			lua_newtable(state);
			lua_pushboolean(state, accepted);
			lua_setfield(state, -2, "accepted");
			lua_pushlstring(state, status.data(), status.size());
			lua_setfield(state, -2, "status");
			pushImmutableProxy(state);
		}

		std::string_view movementStatusName(MovementCommandStatus status)
		{
			switch (status)
			{
			case MovementCommandStatus::Accepted: return "accepted";
			case MovementCommandStatus::Superseded: return "superseded";
			case MovementCommandStatus::UnavailableAction: return "unavailable_action";
			case MovementCommandStatus::NoOp: return "no_op";
			case MovementCommandStatus::UnknownAgent: return "unknown_agent";
			case MovementCommandStatus::InactiveAgent: return "inactive_agent";
			case MovementCommandStatus::UnknownMarker: return "unknown_marker";
			case MovementCommandStatus::AgentBusy: return "agent_busy";
			case MovementCommandStatus::TopologyUnavailable: return "topology_unavailable";
			case MovementCommandStatus::BehaviourOwned: return "behaviour_owned";
			case MovementCommandStatus::NoOccupiableSector: return "no_occupiable_sector";
			}
			return "unknown";
		}

		std::string_view routeLossReasonName(RouteLossReason reason)
		{
			switch (reason)
			{
			case RouteLossReason::Unreachable: return "unreachable";
			case RouteLossReason::TopologyChanged: return "topology_changed";
			case RouteLossReason::DestinationRemoved: return "destination_removed";
			case RouteLossReason::None: break;
			}
			return "unknown";
		}

		std::string_view cancellationReasonName(MovementCancellationReason reason)
		{
			return reason == MovementCancellationReason::Explicit ? "explicit"
				: reason == MovementCancellationReason::Superseded ? "superseded"
				: reason == MovementCancellationReason::TargetDeleted ? "target_deleted"
				: reason == MovementCancellationReason::ActionUnavailable ? "action_unavailable" : "unknown";
		}

		std::string_view interactionResultName(InteractionResult result)
		{
			switch (result)
			{
			case InteractionResult::Succeeded: return "succeeded";
			case InteractionResult::SucceededWithBestEffortFailure:
				return "succeeded_with_best_effort_failure";
			case InteractionResult::Failed: return "failed";
			case InteractionResult::Rejected: return "rejected";
			case InteractionResult::Cancelled: return "cancelled";
			case InteractionResult::Pending: break;
			}
			return "unknown";
		}

		std::string_view teardownReasonName(AgentBehaviourTeardownReason reason)
		{
			switch (reason)
			{
			case AgentBehaviourTeardownReason::Unassignment: return "unassignment";
			case AgentBehaviourTeardownReason::Reset: return "reset";
			case AgentBehaviourTeardownReason::Reload: return "reload";
			case AgentBehaviourTeardownReason::WorldClose: return "world_close";
			case AgentBehaviourTeardownReason::InstanceFailure: return "instance_failure";
			case AgentBehaviourTeardownReason::BehaviourDeletion:
				return "behaviour_deletion";
			}
			return "unknown";
		}

		int markerToString(lua_State* state)
		{
			(void)luaL_checkudata(state, 1, MarkerMetatable);
			lua_pushliteral(state, "Marker");
			return 1;
		}

		int agentToString(lua_State* state)
		{
			(void)luaL_checkudata(state, 1, AgentMetatable);
			lua_pushliteral(state, "Agent");
			return 1;
		}

		int sectorToString(lua_State* state)
		{
			(void)luaL_checkudata(state, 1, SectorMetatable);
			lua_pushliteral(state, "Sector");
			return 1;
		}

		int interactionToString(lua_State* state)
		{
			(void)luaL_checkudata(state, 1, InteractionMetatable);
			lua_pushliteral(state, "Interaction");
			return 1;
		}

		template<typename Handle, typename Id, Id Handle::* member>
		int opaqueHandleEqual(lua_State* state, char const* metatable)
		{
			auto* lhs = static_cast<Handle*>(luaL_testudata(state, 1, metatable));
			auto* rhs = static_cast<Handle*>(luaL_testudata(state, 2, metatable));
			lua_pushboolean(state, lhs && rhs && lhs->*member == rhs->*member);
			return 1;
		}

		int markerEqual(lua_State* state)
		{
			return opaqueHandleEqual<MarkerHandle, MarkerId, &MarkerHandle::marker>(
				state, MarkerMetatable);
		}

		int agentEqual(lua_State* state)
		{
			return opaqueHandleEqual<AgentHandle, AgentId, &AgentHandle::agent>(
				state, AgentMetatable);
		}

		int sectorEqual(lua_State* state)
		{
			return opaqueHandleEqual<SectorHandle, SectorId, &SectorHandle::sector>(
				state, SectorMetatable);
		}

		int interactionEqual(lua_State* state)
		{
			return opaqueHandleEqual<InteractionHandle, InteractionRequestId,
				&InteractionHandle::interaction>(state, InteractionMetatable);
		}

		void ensureOpaqueMetatable(lua_State* state, char const* metatable,
			char const* description, lua_CFunction toString, lua_CFunction equal)
		{
			if (luaL_newmetatable(state, metatable))
			{
				lua_pushstring(state, description);
				lua_setfield(state, -2, "__metatable");
				lua_pushcfunction(state, toString);
				lua_setfield(state, -2, "__tostring");
				lua_pushcfunction(state, equal);
				lua_setfield(state, -2, "__eq");
			}
			lua_pop(state, 1);
		}

		void ensureOpaqueMetatables(lua_State* state)
		{
			ensureOpaqueMetatable(state, MarkerMetatable, "opaque Marker handle",
				markerToString, markerEqual);
			ensureOpaqueMetatable(state, AgentMetatable, "opaque Agent handle",
				agentToString, agentEqual);
			ensureOpaqueMetatable(state, SectorMetatable, "opaque Sector handle",
				sectorToString, sectorEqual);
			ensureOpaqueMetatable(state, InteractionMetatable,
				"opaque interaction handle", interactionToString, interactionEqual);
		}

		template<typename Handle>
		void pushOpaqueHandle(lua_State* state, Handle handle, char const* metatable)
		{
			auto* value = static_cast<Handle*>(
				lua_newuserdatauv(state, sizeof(Handle), 0));
			*value = handle;
			luaL_setmetatable(state, metatable);
		}

		void pushMarkerHandle(lua_State* state, MarkerId marker)
		{
			pushOpaqueHandle(state, MarkerHandle{ marker }, MarkerMetatable);
		}

		CallbackScope* activeScope(lua_State* state)
		{
			auto* scope = static_cast<CallbackScope*>(
				lua_touserdata(state, lua_upvalueindex(1)));
			if (!scope || !scope->active)
			{
				luaL_error(state, "Agent behaviour callback context is no longer active");
				return nullptr;
			}
			if (scope->movementCommandIssued)
			{
				luaL_error(state,
					"multiple movement commands in one Agent behaviour callback are a programming error");
				return nullptr;
			}
			scope->movementCommandIssued = true;
			return scope;
		}

		int queueMoveTo(lua_State* state)
		{
			auto* scope = activeScope(state);
			if (!scope) return 0;
			countCommand(state, *scope);

			MarkerHandle* handle = nullptr;
			int markerIndex = 0;
			for (int index = 1; index <= lua_gettop(state) && !handle; ++index)
			{
				handle = static_cast<MarkerHandle*>(
					luaL_testudata(state, index, MarkerMetatable));
				if (handle) markerIndex = index;
			}
			MarkerId marker = handle ? handle->marker : MarkerId{};
			if (!handle && scope->apiVersion >= 2 && lua_type(state, 1) == LUA_TSTRING)
			{
				size_t length = 0;
				auto const* name = lua_tolstring(state, 1, &length);
				marker = scope->resolveMarker(std::string_view(name, length));
				markerIndex = 1;
			}
			if (!marker)
			{
				pushCommandResult(state, false, "invalid_marker");
				return 1;
			}

			std::string_view action = IdleAction;
			if (!lua_isnoneornil(state, markerIndex + 1))
			{
				if (scope->apiVersion < 2 || lua_type(state, markerIndex + 1) != LUA_TSTRING)
				{
					pushCommandResult(state, false, "unavailable_action");
					return 1;
				}
				size_t length = 0;
				auto const* text = lua_tolstring(state, markerIndex + 1, &length);
				if (length > 165 || length == 0)
				{
					pushCommandResult(state, false, "unavailable_action");
					return 1;
				}
				action = std::string_view(text, length);
			}
			auto const result = scope->inspectMove(marker, action);
			if (result.status == MovementCommandStatus::Accepted
				|| result.status == MovementCommandStatus::Superseded)
				scope->commands.push_back({ PendingMovementCommandType::MoveTo,
					scope->agent, marker, std::string(action) });
			pushCommandResult(state, result.accepted(), movementStatusName(result.status));
			return 1;
		}

		int queueCancelMovement(lua_State* state)
		{
			auto* scope = activeScope(state);
			if (!scope) return 0;
			countCommand(state, *scope);
			auto const result = scope->inspectCancel();
			if (result.status == MovementCommandStatus::Accepted)
				scope->commands.push_back({ PendingMovementCommandType::Cancel,
					scope->agent, {} });
			pushCommandResult(state, result.accepted(), movementStatusName(result.status));
			return 1;
		}

		CallbackScope* activeTimerScope(lua_State* state)
		{
			auto* scope = static_cast<CallbackScope*>(
				lua_touserdata(state, lua_upvalueindex(1)));
			if (!scope || !scope->active)
			{
				luaL_error(state, "Agent behaviour callback context is no longer active");
				return nullptr;
			}
			return scope;
		}

		int changeAuthorization(lua_State* state,
			PendingAuthorizationCommandType type, char const* operation)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			countCommand(state, *scope);
			int nameIndex = 0;
			for (int index = 1; index <= lua_gettop(state); ++index)
				if (lua_type(state, index) == LUA_TSTRING)
				{
					if (nameIndex != 0)
						return luaL_error(state, "%s requires exactly one name", operation);
					nameIndex = index;
				}
			if (nameIndex == 0)
				return luaL_error(state, "%s requires a name", operation);
			size_t length = 0;
			auto const* text = lua_tolstring(state, nameIndex, &length);
			bool changed;
			bool failed;
			{
				std::string diagnostic;
				changed = scope->changeAuthorization(type,
					std::string_view(text ? text : "", length), diagnostic);
				failed = !diagnostic.empty();
				if (failed) pushBorrowedDiagnostic(state, diagnostic);
			}
			if (failed) return lua_error(state);
			pushCommandResult(state, true, changed ? "accepted" : "no_op");
			return 1;
		}

		int grantAccessPermission(lua_State* state)
		{
			return changeAuthorization(state,
				PendingAuthorizationCommandType::GrantAccessPermission,
				"grant_access_permission");
		}

		int revokeAccessPermission(lua_State* state)
		{
			return changeAuthorization(state,
				PendingAuthorizationCommandType::RevokeAccessPermission,
				"revoke_access_permission");
		}

		int assignPermissionSet(lua_State* state)
		{
			return changeAuthorization(state,
				PendingAuthorizationCommandType::AssignPermissionSet,
				"assign_permission_set");
		}

		int unassignPermissionSet(lua_State* state)
		{
			return changeAuthorization(state,
				PendingAuthorizationCommandType::UnassignPermissionSet,
				"unassign_permission_set");
		}

		int randomNumber(lua_State* state)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			// The high 53 bits map exactly onto the binary64 mantissa, yielding
			// a deterministic value in [0, 1) without platform distributions.
			auto const bits = scope->nextRandom() >> 11;
			lua_pushnumber(state, static_cast<lua_Number>(bits)
				* (1.0 / 9007199254740992.0));
			return 1;
		}

		int randomInteger(lua_State* state)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			lua_Integer bounds[2]{};
			int count = 0;
			for (int index = 1; index <= lua_gettop(state); ++index)
			{
				if (!lua_isinteger(state, index)) continue;
				if (count == 2)
					return luaL_error(state,
						"random_integer requires exactly minimum and maximum integers");
				bounds[count++] = lua_tointeger(state, index);
			}
			if (count != 2)
				return luaL_error(state,
					"random_integer requires exactly minimum and maximum integers");
			if (bounds[0] > bounds[1])
				return luaL_error(state,
					"random_integer minimum cannot exceed maximum");
			auto const minimum = static_cast<int64_t>(bounds[0]);
			auto const maximum = static_cast<int64_t>(bounds[1]);
			auto const span = static_cast<uint64_t>(maximum)
				- static_cast<uint64_t>(minimum) + 1u;
			uint64_t offset = scope->nextRandom();
			if (span != 0)
			{
				auto const threshold = static_cast<uint64_t>(-span) % span;
				while (offset < threshold) offset = scope->nextRandom();
				offset %= span;
			}
			auto const resultBits = static_cast<uint64_t>(minimum) + offset;
			lua_pushinteger(state, static_cast<lua_Integer>(
				std::bit_cast<int64_t>(resultBits)));
			return 1;
		}

		int setTimer(lua_State* state)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			countCommand(state, *scope);
			int nameIndex = 0;
			for (int index = 1; index <= lua_gettop(state); ++index)
				if (lua_type(state, index) == LUA_TSTRING) { nameIndex = index; break; }
			if (nameIndex == 0 || nameIndex == lua_gettop(state)
				|| !lua_isinteger(state, nameIndex + 1))
				return luaL_error(state, "set_timer requires a name and whole-tick duration");
			size_t nameLength = 0;
			auto const* nameText = lua_tolstring(state, nameIndex, &nameLength);
			auto const duration = lua_tointeger(state, nameIndex + 1);
			if (nameLength == 0)
				return luaL_error(state, "timer name must not be empty");
			if (duration < 1)
				return luaL_error(state, "timer duration must be at least one simulation tick");
			bool accepted;
			{
				std::string diagnostic;
				accepted = scope->setTimer(std::string(nameText, nameLength),
					static_cast<uint64_t>(duration), diagnostic);
				if (!accepted) pushBorrowedDiagnostic(state, diagnostic);
			}
			if (!accepted) return lua_error(state);
			pushCommandResult(state, true, "accepted");
			return 1;
		}

		int cancelTimer(lua_State* state)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			countCommand(state, *scope);
			for (int index = 1; index <= lua_gettop(state); ++index)
			{
				if (lua_type(state, index) != LUA_TSTRING) continue;
				size_t length = 0;
				auto const* text = lua_tolstring(state, index, &length);
				if (length == 0) return luaL_error(state, "timer name must not be empty");
				auto const removed = scope->cancelTimer(std::string(text, length));
				pushCommandResult(state, true, removed ? "accepted" : "no_op");
				return 1;
			}
			return luaL_error(state, "cancel_timer requires a name");
		}

		int logMessage(lua_State* state)
		{
			auto* scope = activeTimerScope(state);
			if (!scope) return 0;
			// Only the message and an optional level are meaningful, so no string
			// argument is copied before the staging budgets admit the message.
			int indices[2] = { 0, 0 };
			int stringCount = 0;
			for (int index = 1; index <= lua_gettop(state); ++index)
				if (lua_type(state, index) == LUA_TSTRING)
				{
					if (stringCount == 2)
						return luaL_error(state,
							"log requires a message and optional level");
					indices[stringCount++] = index;
				}
			if (stringCount == 0)
				return luaL_error(state, "log requires a message and optional level");
			auto stringEquals = [&](int index, char const* value)
			{
				size_t length = 0;
				auto const* text = lua_tolstring(state, index, &length);
				return text && std::string_view(text, length) == value;
			};
			auto parseLevel = [&](int index, LogLevel& level)
			{
				if (stringEquals(index, "debug")) { level = LogLevel::Debug; return true; }
				if (stringEquals(index, "info")) { level = LogLevel::Info; return true; }
				if (stringEquals(index, "warning")) { level = LogLevel::Warning; return true; }
				if (stringEquals(index, "error")) { level = LogLevel::Error; return true; }
				return false;
			};
			LogLevel level = LogLevel::Info;
			int messageIndex = indices[0];
			if (stringCount == 2)
			{
				if (parseLevel(indices[0], level)) messageIndex = indices[1];
				else if (parseLevel(indices[1], level)) messageIndex = indices[0];
				else return luaL_error(state,
					"log level must be debug, info, warning, or error");
			}
			// The transactional window budget is shared across Agents, so messages
			// beyond what the current window can still publish are counted but not
			// staged. Truncation is reported on the line itself; a refusal is folded
			// into the single bounded suppression summary.
			size_t length = 0;
			auto const* text = lua_tolstring(state, messageIndex, &length);
			scope->stageLog(level, text, length);
			return 0;
		}

		void pushConfigurationValue(lua_State* state,
			AgentBehaviourConfigurationValue const& value)
		{
			std::visit([&](auto const& typed)
			{
				using T = std::decay_t<decltype(typed)>;
				if constexpr (std::is_same_v<T, bool>)
					lua_pushboolean(state, typed);
				else if constexpr (std::is_same_v<T, int64_t>)
					lua_pushinteger(state, static_cast<lua_Integer>(typed));
				else if constexpr (std::is_same_v<T, double>)
					lua_pushnumber(state, typed);
				else if constexpr (std::is_same_v<T, std::string>)
					lua_pushlstring(state, typed.data(), typed.size());
				else if constexpr (std::is_same_v<T, AgentBehaviourAction>)
					lua_pushlstring(state, typed.reference.data(), typed.reference.size());
				else if constexpr (std::is_same_v<T, AgentBehaviourDuration>)
					lua_pushinteger(state, static_cast<lua_Integer>(typed.ticks));
				else if constexpr (std::is_same_v<T, MarkerId>)
					pushMarkerHandle(state, typed);
				else if constexpr (std::is_same_v<T, AgentBehaviourConfigurationList>)
				{
					lua_newtable(state);
					auto const backing = lua_gettop(state);
					for (size_t index = 0; index < typed.size(); ++index)
					{
						pushConfigurationValue(state, typed[index]);
						lua_rawseti(state, backing, static_cast<lua_Integer>(index + 1));
					}
					pushImmutableProxy(state);
				}
				else
				{
					lua_newtable(state);
					auto const backing = lua_gettop(state);
					for (auto const& [name, nested] : typed)
					{
						pushConfigurationValue(state, nested);
						lua_setfield(state, backing, name.c_str());
					}
					pushImmutableProxy(state);
				}
			}, value.value);
		}

		void pushConfiguration(lua_State* state,
			AgentBehaviourConfiguration const& configuration)
		{
			// Borrow the authored record directly. A temporary variant here
			// would copy its strings/containers inside the marshalling trampoline
			// and leak them if a Lua allocation failed before destruction.
			lua_newtable(state);
			auto const backing = lua_gettop(state);
			for (auto const& [name, value] : configuration)
			{
				pushConfigurationValue(state, value);
				lua_setfield(state, backing, name.c_str());
			}
			pushImmutableProxy(state);
		}

		// Per-instance marshalling trampolines are cached in the registry once
		// per live runtime so the protected marshalling call never pushes a
		// fresh C closure at the point of use (that push would itself allocate
		// outside a protected boundary). Each trampoline reads its payload from
		// the stack rather than from an upvalue.
		int marshallConfiguration(lua_State* state)
		{
			auto const* configuration = static_cast<AgentBehaviourConfiguration const*>(
				lua_touserdata(state, 1));
			pushConfiguration(state, *configuration);
			return 1;
		}

		uint64_t mixRandomSeed(uint64_t value)
		{
			value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
			value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
			return value ^ (value >> 31);
		}

		uint64_t deriveRandomSeed(uint64_t worldSeed, AgentId agent,
			AgentBehaviourId behaviour)
		{
			auto state = mixRandomSeed(worldSeed + 0x9e3779b97f4a7c15ull);
			state ^= mixRandomSeed(agent.value + 0x243f6a8885a308d3ull);
			state ^= mixRandomSeed(behaviour.value + 0x13198a2e03707344ull);
			return mixRandomSeed(state);
		}
	}

	struct AgentBehaviourRuntimeAdapter::Impl
	{
		enum class OutcomeType
		{
			DestinationReached,
			ActionFailed,
			MovementCancelled,
			RouteLost,
			InteractionCompleted,
			InteractionFailed,
			Activated,
			Deactivated
		};

		struct PendingOutcome
		{
			uint64_t sequence{ 0 };
			uint64_t tick{ 0 };
			OutcomeType type{ OutcomeType::DestinationReached };
			MarkerId destination;
			std::string action{ IdleAction };
			std::string diagnostic;
			ScriptExecutionFailure scriptFailure{ ScriptExecutionFailure::None };
			RouteLossReason routeLossReason{ RouteLossReason::None };
			MovementCancellationReason cancellationReason{
				MovementCancellationReason::None };
			InteractionRequestId interaction;
			std::string interactionName;
			InteractionResult interactionResult{ InteractionResult::Pending };
		};

		// Registry-owned immutable text. One pack is shared by every Agent in one
		// boundary and its helper sources are materialized only when at least one
		// instance must be constructed, so an unchanged tick copies no source.
		struct SourcePack
		{
			std::string packageName;
			std::vector<AgentBehaviourHelperSource> helpers;
			bool materialized{ false };

			void materialize(AgentBehaviourRegistry const& registry)
			{
				if (materialized) return;
				materialized = true;
				helpers.reserve(registry.mHelperModules.size());
				for (auto const& [name, helper] : registry.mHelperModules)
				{
					auto source = registry.mSourceCache.find(
						helper->getSourceModulePath());
					if (source == registry.mSourceCache.end()) continue;
					helpers.push_back({ name, helper->getSourceModulePath(),
						source->second });
				}
			}
		};

		// A transient, non-owning view of one desired instance. Every referenced
		// value is owned by the World, its registry, or a boundary-owned
		// SourcePack and outlives the boundary that builds the view.
		struct Definition
		{
			AgentId agent;
			std::string_view agentName;
			std::string_view behaviourName;
			AgentBehaviourAssignment const* assignment{ nullptr };
			bool active{ true };
			uint64_t randomSeed{ 0 };
			std::string_view registryUuid;
			uint64_t packageRevision{ 0 };
			std::string_view packageName;
			std::string_view moduleName;
			std::string_view source;
			std::vector<AgentBehaviourHelperSource> const* helpers{ nullptr };
		};

		struct Instance
		{
			uint32_t apiVersion{ 1 };
			AgentBehaviourAssignment assignment;
			std::string agentName;
			std::string behaviourName;
			std::string registryUuid;
			uint64_t packageRevision{ 0 };
			std::string packageName;
			std::string moduleName;
			int environmentReference{ LUA_NOREF };
			int configurationReference{ LUA_NOREF };
			int instanceReference{ LUA_NOREF };
			std::unique_ptr<ModuleLoader> moduleLoader;
			bool started{ false };
			bool disabled{ false };
			bool suspended{ false };
			uint64_t randomState{ 0 };
			CallbackScope scope;
			// Active instances store absolute due ticks. Suspended instances store
			// remaining durations in the same map, frozen at deactivation.
			std::map<std::string, uint64_t> timers;
			std::vector<PendingOutcome> outcomes;
			std::vector<PendingOutcome> lifecycleOutcomes;
		};

		ScratchBudget budget;
		uint32_t timerLimit{ AgentBehaviourRuntimeAdapter::DefaultTimersPerInstance };
		uint32_t callbackLimit{ AgentBehaviourRuntimeAdapter::DefaultCallbacksPerBoundary };
		uint32_t commandLimit{ AgentBehaviourRuntimeAdapter::DefaultCommandsPerCallback };
		uint32_t logLimit{ AgentBehaviourRuntimeAdapter::DefaultLogMessagesPerWindow };
		uint64_t logWindowTicks{ AgentBehaviourRuntimeAdapter::DefaultLogWindowTicks };
		size_t logMessageByteLimit{ AgentBehaviourRuntimeAdapter::DefaultLogBytesPerMessage };
		size_t logWindowByteLimit{ AgentBehaviourRuntimeAdapter::DefaultLogBytesPerWindow };
		uint32_t callbackCount{ 0 };
		uint64_t currentTick{ 0 };
		uint64_t logWindow{ 0 };
		uint32_t logCount{ 0 };
		size_t logWindowBytes{ 0 };
		bool logSuppressionEmitted{ false };
		int privateEnvironmentTrampolineReference{ LUA_NOREF };
		int configurationTrampolineReference{ LUA_NOREF };
		// Set when the panic handler fires on an unprotected allocation: the Lua
		// thread has been reset but is no longer safe to reuse, so every instance
		// is abandoned and no further Lua work is attempted.
		bool stateFailed{ false };
		std::unique_ptr<lua_State, StateCloser> state;
		ModuleLoader hostLoader;
		std::map<AgentId, Instance> instances;
		// Module-load and factory failures have behaviour scope. Callback failures
		// remain confined to one Agent instance.
		std::set<AgentBehaviourId> disabledBehaviours;
		// Activation can be authored before the first simulation boundary has
		// constructed the assigned instance.
		std::map<AgentId, std::vector<PendingOutcome>> pendingLifecycleOutcomes;
		std::vector<AgentBehaviourRuntimeDiagnostic> diagnostics;
		uint64_t observedOutcomeCount{ 0 };
		uint64_t lastObservedSequence{ 0 };

		explicit Impl(AgentBehaviourRuntimeLimits limits)
			: budget(limits.memoryBytes, limits.instructionsPerCall)
			, timerLimit(limits.timersPerInstance)
			, callbackLimit(limits.callbacksPerBoundary)
			, commandLimit(limits.commandsPerCallback)
			, logLimit(limits.logMessagesPerWindow)
			, logWindowTicks(limits.logWindowTicks)
			, logMessageByteLimit(limits.logBytesPerMessage)
			, logWindowByteLimit(limits.logBytesPerWindow)
			, state(lua_newstate(budgetedAllocate, &budget))
		{
			if (!state) throw std::runtime_error("Could not create World Lua runtime");
			// Never let an unprotected allocation terminate the process.
			lua_atpanic(state.get(), luaPanic);
			hostLoader.packageName = "World Agent behaviours";
			hostLoader.hostModuleVersions = {
				{ "prometheum.v1", 1 }, { "prometheum.v2", 2 } };
			std::string setupMessage;
			auto const setupStatus = runScratchSetup(state.get(), &hostLoader,
				ensureOpaqueMetatables, setupMessage);
			if (setupStatus != LUA_OK)
				throw std::runtime_error(
					"Could not initialize World Lua runtime: " + setupMessage);
			privateEnvironmentTrampolineReference = registerTrampoline(state.get(),
				marshallPrivateEnvironment);
			configurationTrampolineReference = registerTrampoline(state.get(),
				marshallConfiguration);
		}

		// A panic has already reset the Lua thread; the state is unusable, so
		// every instance is abandoned without touching it. The registry
		// references are reclaimed by lua_close on destruction.
		void poison()
		{
			stateFailed = true;
			for (auto& [agent, instance] : instances)
			{
				(void)agent;
				instance.scope.active = false;
				instance.scope.commands.clear();
				instance.moduleLoader.reset();
			}
			instances.clear();
			disabledBehaviours.clear();
			pendingLifecycleOutcomes.clear();
			callbackCount = 0;
		}

		void release(Instance& instance)
		{
			if (instance.moduleLoader) instance.moduleLoader->release(state.get());
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.instanceReference);
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.configurationReference);
			luaL_unref(state.get(), LUA_REGISTRYINDEX, instance.environmentReference);
			instance.instanceReference = LUA_NOREF;
			instance.configurationReference = LUA_NOREF;
			instance.environmentReference = LUA_NOREF;
			instance.moduleLoader.reset();
		}

		void record(Definition const& definition, AgentBehaviourRuntimeStage stage,
			std::string_view callback, ProtectedCallResult const& result)
		{
			diagnostics.push_back({ result.failure, stage, definition.agent,
				definition.assignment->behaviour, currentTick,
				std::string(definition.agentName),
				std::string(definition.behaviourName),
				std::string(definition.packageName),
				std::string(definition.moduleName), std::string(callback),
				result.diagnostic, result.traceback });
		}

		void record(Instance const& instance, AgentBehaviourRuntimeStage stage,
			std::string_view callback, ProtectedCallResult const& result)
		{
			diagnostics.push_back({ result.failure, stage, instance.scope.agent,
				instance.assignment.behaviour, currentTick, instance.agentName,
				instance.behaviourName, instance.packageName, instance.moduleName,
				std::string(callback), result.diagnostic, result.traceback });
		}

		void clear()
		{
			if (stateFailed)
			{
				// poison() already abandoned every instance; the dead Lua state
				// must not be touched again. Registry references are reclaimed by
				// lua_close on destruction.
				instances.clear();
				disabledBehaviours.clear();
				pendingLifecycleOutcomes.clear();
				diagnostics.clear();
				observedOutcomeCount = 0;
				lastObservedSequence = 0;
				callbackCount = 0;
				currentTick = 0;
				logWindow = 0;
				logCount = 0;
				logWindowBytes = 0;
				logSuppressionEmitted = false;
				return;
			}
			for (auto& [agent, instance] : instances)
			{
				(void)agent;
				release(instance);
			}
			instances.clear();
			disabledBehaviours.clear();
			pendingLifecycleOutcomes.clear();
			diagnostics.clear();
			observedOutcomeCount = 0;
			lastObservedSequence = 0;
			callbackCount = 0;
			currentTick = 0;
			logWindow = 0;
			logCount = 0;
			logWindowBytes = 0;
			logSuppressionEmitted = false;
			(void)lua_gc(state.get(), LUA_GCCOLLECT);
		}

		bool construct(Definition const& definition, Instance& instance)
		{
			auto* lua = state.get();
			auto const base = lua_gettop(lua);
			auto conversionFailure = [&](AgentBehaviourRuntimeStage stage,
				std::string message)
			{
				ProtectedCallResult result;
				result.failure = AgentBehaviourRuntimeFailure::ConversionError;
				result.diagnostic = std::move(message);
				result.traceback = result.diagnostic;
				record(definition, stage, {}, result);
				lua_settop(lua, base);
				return false;
			};
			std::string const chunkName = std::string("@")
				+ std::string(definition.packageName) + "/"
				+ std::string(definition.moduleName);
			budget.memoryLimitExceeded = false;
			auto const loadStatus = luaL_loadbufferx(lua, definition.source.data(),
				definition.source.size(), chunkName.c_str(), "t");
			if (loadStatus != LUA_OK)
			{
				ProtectedCallResult result;
				result.failure = loadStatus == LUA_ERRMEM || budget.memoryLimitExceeded
					? AgentBehaviourRuntimeFailure::MemoryBudgetExceeded
					: AgentBehaviourRuntimeFailure::LuaError;
				auto const* message = lua_tostring(lua, -1);
				result.diagnostic = message ? message : "Lua module load failed";
				result.traceback = result.diagnostic;
				record(definition, AgentBehaviourRuntimeStage::ModuleLoad, {}, result);
				lua_settop(lua, base);
				return false;
			}
			auto const chunk = lua_gettop(lua);

			ProtectedCallResult environmentResult;
			if (!protectedMarshall(lua, privateEnvironmentTrampolineReference,
				nullptr, 1, environmentResult))
			{
				record(definition, AgentBehaviourRuntimeStage::ModuleLoad, {},
					environmentResult);
				lua_settop(lua, base);
				return false;
			}
			auto const environment = lua_gettop(lua);
			lua_pushvalue(lua, environment);
			instance.environmentReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			instance.moduleLoader = std::make_unique<ModuleLoader>();
			instance.moduleLoader->packageName = definition.packageName;
			instance.moduleLoader->hostModules = hostLoader.hostModules;
			instance.moduleLoader->environmentReference = instance.environmentReference;
			for (auto const& helper : *definition.helpers)
				instance.moduleLoader->modules.emplace(helper.name, helper);
			lua_pushlightuserdata(lua, instance.moduleLoader.get());
			lua_pushcclosure(lua, requireDeclaredModule, 1);
			lua_setfield(lua, environment, "require");
			lua_pushvalue(lua, environment);
			if (!lua_setupvalue(lua, chunk, 1))
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module has no isolated environment");
			lua_remove(lua, environment);

			instance.moduleLoader->dependencyChain.emplace_back(definition.moduleName);
			auto const moduleLoaded = protectedCall(lua, budget, 0, 1);
			instance.moduleLoader->dependencyChain.clear();
			if (!moduleLoaded.succeeded)
			{
				record(definition, AgentBehaviourRuntimeStage::ModuleLoad, {}, moduleLoaded);
				lua_settop(lua, base);
				return false;
			}
			if (!lua_istable(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module must return a contract table");
			auto const contract = lua_gettop(lua);
			lua_pushliteral(lua, "api_version");
			lua_rawget(lua, contract);
			auto const apiVersion = lua_isinteger(lua, -1) ? lua_tointeger(lua, -1) : 0;
			lua_pop(lua, 1);
			if (apiVersion != 1 && apiVersion != HostApiVersion)
				return conversionFailure(AgentBehaviourRuntimeStage::ModuleLoad,
					"Agent behaviour module API version is invalid");
			instance.apiVersion = static_cast<uint32_t>(apiVersion);
			lua_pushliteral(lua, "factory");
			lua_rawget(lua, contract);
			if (!lua_isfunction(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::Factory,
					"Agent behaviour module factory is invalid");

			ProtectedCallResult configurationResult;
			if (!protectedMarshall(lua, configurationTrampolineReference,
				&definition.assignment->configuration, 1, configurationResult))
			{
				record(definition, AgentBehaviourRuntimeStage::Factory, {},
					configurationResult);
				lua_settop(lua, base);
				return false;
			}
			lua_pushvalue(lua, -1);
			instance.configurationReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			auto const factoryCalled = protectedCall(lua, budget, 1, 1);
			if (!factoryCalled.succeeded)
			{
				record(definition, AgentBehaviourRuntimeStage::Factory, {}, factoryCalled);
				lua_settop(lua, base);
				return false;
			}
			if (!lua_istable(lua, -1))
				return conversionFailure(AgentBehaviourRuntimeStage::Factory,
					"Agent behaviour factory must return an instance table");
			auto const instanceTable = lua_gettop(lua);
			for (auto const* callback : { "on_start", "on_event", "on_timer",
				"on_route_lost", "on_stop" })
			{
				lua_pushstring(lua, callback);
				lua_rawget(lua, instanceTable);
				auto const valid = lua_isnil(lua, -1) || lua_isfunction(lua, -1);
				lua_pop(lua, 1);
				if (!valid)
					return conversionFailure(AgentBehaviourRuntimeStage::Factory,
						std::format("Agent behaviour instance field '{}' must be a function when present",
							callback));
			}
			instance.instanceReference = luaL_ref(lua, LUA_REGISTRYINDEX);
			lua_settop(lua, base);
			return true;
		}

		void synchronize(World& world, AgentBehaviourRegistry const* registry,
			SourcePack& sources, std::vector<Definition> const& definitions)
		{
			std::map<AgentId, Definition const*> desired;
			for (auto const& definition : definitions)
				desired.emplace(definition.agent, &definition);

			for (auto iterator = instances.begin(); iterator != instances.end();)
			{
				auto found = desired.find(iterator->first);
				if (found != desired.end()
					&& iterator->second.assignment == *found->second->assignment
					&& iterator->second.registryUuid == found->second->registryUuid
					&& iterator->second.packageRevision == found->second->packageRevision)
				{
					++iterator;
					continue;
				}
				release(iterator->second);
				iterator = instances.erase(iterator);
			}

			auto& failedBehaviours = disabledBehaviours;
			for (auto const& definition : definitions)
			{
				if (instances.contains(definition.agent)) continue;
				// An unchanged instance needs no source at all; only a new or
				// changed one materializes the shared registry sources.
				sources.materialize(*registry);
				Instance instance;
				instance.assignment = *definition.assignment;
				instance.agentName = definition.agentName;
				instance.behaviourName = definition.behaviourName;
				instance.suspended = !definition.active;
				instance.randomState = definition.randomSeed;
				instance.registryUuid = definition.registryUuid;
				instance.packageRevision = definition.packageRevision;
				instance.packageName = definition.packageName;
				instance.moduleName = definition.moduleName;
				instance.scope.agent = definition.agent;
				if (failedBehaviours.contains(definition.assignment->behaviour))
					instance.disabled = true;
				else if (!construct(definition, instance))
				{
					instance.disabled = true;
					failedBehaviours.insert(definition.assignment->behaviour);
					for (auto& [otherAgent, other] : instances)
					{
						(void)otherAgent;
						if (other.assignment.behaviour != definition.assignment->behaviour
							|| other.disabled) continue;
						teardownInstance(world, other,
							AgentBehaviourTeardownReason::InstanceFailure, true);
						(void)world.cancelBehaviourAgentMovement(otherAgent);
					}
				}
				if (instance.disabled)
				{
					release(instance);
					(void)world.cancelBehaviourAgentMovement(definition.agent);
					(void)lua_gc(state.get(), LUA_GCCOLLECT);
				}
				if (auto pending = pendingLifecycleOutcomes.find(definition.agent);
					pending != pendingLifecycleOutcomes.end())
				{
					instance.lifecycleOutcomes = std::move(pending->second);
					pendingLifecycleOutcomes.erase(pending);
				}
				instances.emplace(definition.agent, std::move(instance));
			}
		}

		std::string_view semanticMovementState(World const& world,
			AgentId agentId, Agent const& agent) const
		{
			if (!agent.isActive()) return "suspended";
			auto const goal = world.mMovementGoals.find(agentId);
			if (goal == world.mMovementGoals.end()) return "idle";
			if (goal->second.cancelling) return "cancelling";
			switch (agent.getState())
			{
			case Agent::State::WaitingForTraversal: return "waiting";
			case Agent::State::TraversingEdge:
			case Agent::State::AwaitingTraversalCommit: return "traversing";
			case Agent::State::RoutePlanning:
				return instances.at(agentId).apiVersion == 1 ? "idle" : "route_planning";
			case Agent::State::Idle:
			case Agent::State::MovingToVertex: return "moving";
			}
			return "idle";
		}

		void pushAgentState(World const& world, AgentId agentId)
		{
			auto* lua = state.get();
			auto const* agent = world.mAgents.find(agentId);
			lua_newtable(lua);
			auto const backing = lua_gettop(lua);
			pushOpaqueHandle(lua, AgentHandle{ agentId }, AgentMetatable);
			lua_setfield(lua, backing, "identity");
			lua_pushlstring(lua, agent->getName().data(), agent->getName().size());
			lua_setfield(lua, backing, "name");
			lua_pushboolean(lua, agent->isActive());
			lua_setfield(lua, backing, "active");
			lua_pushboolean(lua, !agent->isActive());
			lua_setfield(lua, backing, "suspended");
			auto const status = agent->isActive()
				? std::string_view("active") : std::string_view("suspended");
			lua_pushlstring(lua, status.data(), status.size());
			lua_setfield(lua, backing, "status");
			auto const movement = semanticMovementState(world, agentId, *agent);
			lua_pushlstring(lua, movement.data(), movement.size());
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "movement");
			lua_setfield(lua, backing, "movement_state");
			auto const goal = world.mMovementGoals.find(agentId);
			if (goal == world.mMovementGoals.end()) lua_pushnil(lua);
			else pushMarkerHandle(lua, goal->second.marker);
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "destination_marker");
			lua_setfield(lua, backing, "destination");
			auto const action = goal == world.mMovementGoals.end() ? IdleAction
				: std::string_view(goal->second.selectedAction);
			lua_pushlstring(lua, action.data(), action.size());
			lua_setfield(lua, backing, "selected_action");
			auto const* sector = agent->getSector();
			if (sector)
				pushOpaqueHandle(lua, SectorHandle{ SectorId{
					static_cast<uint64_t>(sector->getIndex()) + 1 } }, SectorMetatable);
			else lua_pushnil(lua);
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "sector_identity");
			lua_setfield(lua, backing, "sector");
			if (sector)
				lua_pushlstring(lua, sector->getName().data(), sector->getName().size());
			else lua_pushliteral(lua, "");
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "sector_display_name");
			lua_setfield(lua, backing, "sector_name");
			auto const position = agent->getGlobalPosition();
			lua_newtable(lua);
			lua_pushnumber(lua, position.x);
			lua_setfield(lua, -2, "x");
			lua_pushnumber(lua, position.y);
			lua_setfield(lua, -2, "y");
			pushImmutableProxy(lua);
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "position");
			lua_setfield(lua, backing, "global_position");
			lua_pushinteger(lua, static_cast<lua_Integer>(world.mSimulationTick));
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "simulation_tick");
			lua_setfield(lua, backing, "tick");
			pushImmutableProxy(lua);
		}

		void pushReadOnlyContext(World const& world, Instance& instance)
		{
			auto* lua = state.get();
			lua_newtable(lua);
			auto const backing = lua_gettop(lua);
			lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.configurationReference);
			lua_setfield(lua, backing, "configuration");
			lua_pushinteger(lua, static_cast<lua_Integer>(world.mSimulationTick));
			lua_setfield(lua, backing, "tick");
			pushAgentState(world, instance.scope.agent);
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "agent");
			lua_setfield(lua, backing, "state");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, logMessage, 1);
			lua_setfield(lua, backing, "log");
			pushImmutableProxy(lua);
		}

		void pushContext(World const& world, Instance& instance)
		{
			auto* lua = state.get();
			lua_newtable(lua);
			auto const backing = lua_gettop(lua);
			lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.configurationReference);
			lua_setfield(lua, backing, "configuration");
			lua_pushinteger(lua, static_cast<lua_Integer>(world.mSimulationTick));
			lua_setfield(lua, backing, "tick");
			pushAgentState(world, instance.scope.agent);
			lua_pushvalue(lua, -1);
			lua_setfield(lua, backing, "agent");
			lua_setfield(lua, backing, "state");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, queueMoveTo, 1);
			lua_setfield(lua, backing, "move_to");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, queueCancelMovement, 1);
			lua_setfield(lua, backing, "cancel_movement");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, setTimer, 1);
			lua_setfield(lua, backing, "set_timer");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, cancelTimer, 1);
			lua_setfield(lua, backing, "cancel_timer");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, randomNumber, 1);
			lua_setfield(lua, backing, "random_number");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, randomInteger, 1);
			lua_setfield(lua, backing, "random_integer");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, grantAccessPermission, 1);
			lua_setfield(lua, backing, "grant_access_permission");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, revokeAccessPermission, 1);
			lua_setfield(lua, backing, "revoke_access_permission");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, assignPermissionSet, 1);
			lua_setfield(lua, backing, "assign_permission_set");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, unassignPermissionSet, 1);
			lua_setfield(lua, backing, "unassign_permission_set");
			lua_pushlightuserdata(lua, &instance.scope);
			lua_pushcclosure(lua, logMessage, 1);
			lua_setfield(lua, backing, "log");
			pushImmutableProxy(lua);
		}

		// A window is a property of simulation time, not of a successful callback,
		// so it is rolled over before staging as well as before publishing. This is
		// what lets staging see the same remaining allowance publishing will.
		void beginLogWindow()
		{
			auto const window = currentTick / logWindowTicks;
			if (window != logWindow)
			{
				logWindow = window;
				logCount = 0;
				logWindowBytes = 0;
				logSuppressionEmitted = false;
			}
		}

		void prepareScope(World& world, AgentId agentId, Instance& instance,
			std::vector<PendingMovementCommand> const& pendingCommands,
			std::vector<PendingAuthorizationCommand> const& pendingAuthorizationCommands)
		{
			instance.scope.active = true;
			instance.scope.apiVersion = instance.apiVersion;
			instance.scope.movementCommandIssued = false;
			instance.scope.commandCount = 0;
			instance.scope.commandLimit = commandLimit;
			instance.scope.commands.clear();
			instance.scope.authorizationCommands.clear();
			instance.scope.logs.clear();
			beginLogWindow();
			instance.scope.logsSuppressed = false;
			instance.scope.logStagedBytes = 0;
			instance.scope.logMessageCountLimit = logCount < logLimit
				? logLimit - logCount : 0;
			instance.scope.logStagingByteLimit = logWindowBytes < logWindowByteLimit
				? logWindowByteLimit - logWindowBytes : 0;
			instance.scope.logMessageByteLimit = logMessageByteLimit;
			instance.scope.stagedTimers = instance.timers;
			instance.scope.stagedRandomState = instance.randomState;
			instance.scope.resolveMarker = [&world](std::string_view name)
			{
				for (auto const marker : world.getMarkerIds())
					if (auto candidate = world.lookupMarker(marker); candidate && candidate->getName() == name)
						return marker;
				return MarkerId{};
			};
			instance.scope.inspectMove = [&world, agentId, &pendingCommands, &instance](MarkerId marker, std::string_view action)
			{
				auto pending = std::find_if(pendingCommands.rbegin(), pendingCommands.rend(),
					[agentId](PendingMovementCommand const& command)
					{ return command.agent == agentId; });
				if (pending != pendingCommands.rend())
					return MovementCommandResult{ pending->type == PendingMovementCommandType::MoveTo
						&& pending->marker == marker && pending->action == action ? MovementCommandStatus::NoOp
						: MovementCommandStatus::AgentBusy };
				auto result = world.inspectBehaviourMoveToMarker(agentId, marker, action);
				if (instance.apiVersion == 1 && result.status == MovementCommandStatus::Superseded)
					return MovementCommandResult{ MovementCommandStatus::AgentBusy };
				return result;
			};
			instance.scope.inspectCancel = [&world, agentId, &pendingCommands]
			{
				auto pending = std::find_if(pendingCommands.rbegin(), pendingCommands.rend(),
					[agentId](PendingMovementCommand const& command)
					{ return command.agent == agentId; });
				if (pending != pendingCommands.rend())
					return MovementCommandResult{ pending->type == PendingMovementCommandType::Cancel
						? MovementCommandStatus::NoOp : MovementCommandStatus::AgentBusy };
				return world.inspectBehaviourMovementCancellation(agentId);
			};
			instance.scope.setTimer = [&world, &instance, this](std::string name,
				uint64_t duration, std::string& diagnostic)
			{
				auto& timers = instance.scope.stagedTimers;
				if (!instance.suspended
					&& duration > std::numeric_limits<uint64_t>::max()
						- world.mSimulationTick)
				{
					diagnostic = "timer due tick exceeds the simulation tick range";
					return false;
				}
				if (!timers.contains(name)
					&& timers.size() >= timerLimit)
				{
					diagnostic = std::format(
						"Agent behaviour timer limit of {} per instance exceeded", timerLimit);
					return false;
				}
				timers[std::move(name)] = instance.suspended
					? duration : world.mSimulationTick + duration;
				return true;
			};
			instance.scope.cancelTimer = [&instance](std::string const& name)
			{
				return instance.scope.stagedTimers.erase(name) != 0;
			};
			instance.scope.nextRandom = [&instance]
			{
				// SplitMix64 has a completely specified integer transition and no
				// process-global state. Each Agent instance owns this state.
				instance.scope.stagedRandomState += 0x9e3779b97f4a7c15ull;
				auto value = instance.scope.stagedRandomState;
				value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
				value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
				return value ^ (value >> 31);
			};
			instance.scope.changeAuthorization = [&world, &instance, agentId,
				&pendingAuthorizationCommands](
				PendingAuthorizationCommandType type, std::string_view name,
				std::string& diagnostic)
			{
				PendingAuthorizationCommand command{ type, agentId, {}, {} };
				if (type == PendingAuthorizationCommandType::GrantAccessPermission
					|| type == PendingAuthorizationCommandType::RevokeAccessPermission)
				{
					command.permission = world.accessPermissionNamed(name);
					if (!command.permission)
					{
						diagnostic = std::format(
							"Unknown Access permission '{}' (names are case-sensitive)", name);
						return false;
					}
				}
				else
				{
					command.permissionSet = world.permissionSetNamed(name);
					if (!command.permissionSet)
					{
						diagnostic = std::format(
							"Unknown Permission set '{}' (names are case-sensitive)", name);
						return false;
					}
				}

				bool current = command.permission
					? world.currentDirectAccessGrants(*world.mAgents.find(agentId))
						.test(command.permission.value - 1)
					: world.currentPermissionSets(*world.mAgents.find(agentId))
						.contains(command.permissionSet);
				auto foldPrior = [&](PendingAuthorizationCommand const& prior)
				{
					if (prior.agent != agentId) return;
					if (command.permission && prior.permission == command.permission)
						current = prior.type
							== PendingAuthorizationCommandType::GrantAccessPermission;
					else if (command.permissionSet
						&& prior.permissionSet == command.permissionSet)
						current = prior.type
							== PendingAuthorizationCommandType::AssignPermissionSet;
				};
				for (auto const& prior : pendingAuthorizationCommands) foldPrior(prior);
				for (auto const& prior : instance.scope.authorizationCommands) foldPrior(prior);
				auto const desired = type
					== PendingAuthorizationCommandType::GrantAccessPermission
					|| type == PendingAuthorizationCommandType::AssignPermissionSet;
				if (current == desired) return false;
				instance.scope.authorizationCommands.push_back(command);
				return true;
			};
		}

		void publishLogs(Instance const& instance)
		{
			beginLogWindow();
			auto const& messages = instance.scope.logs;
			auto const source = std::format("Agent behaviour '{}' / Agent '{}'",
				instance.behaviourName, instance.agentName);
			auto const sourceId = instance.scope.agent.value
				<= std::numeric_limits<uint32_t>::max()
				? static_cast<uint32_t>(instance.scope.agent.value) : ~0u;
			bool dropped = false;
			for (auto const& message : messages)
			{
				// Staging already respected the window allowance; this remains a
				// defensive bound so publishing can never exceed it either.
				if (logCount >= logLimit
					|| message.message.size() > logWindowByteLimit - logWindowBytes)
				{
					dropped = true;
					break;
				}
				addLogMessage(source, sourceId, message.level, message.message);
				++logCount;
				logWindowBytes += message.message.size();
			}
			if (!logSuppressionEmitted && (instance.scope.logsSuppressed || dropped))
			{
				// One fixed-width line per window keeps suppression reporting itself
				// inside the same host-memory allowance as the logs it reports on.
				addLogMessage("Agent behaviours", 0, LogLevel::Warning,
					std::format("Agent behaviour log limit of {} messages / {} bytes per World per {} ticks exceeded; further messages suppressed",
						logLimit, logWindowByteLimit, logWindowTicks));
				logSuppressionEmitted = true;
			}
		}

		ProtectedCallResult admitCallback()
		{
			if (callbackCount < callbackLimit)
			{
				++callbackCount;
				ProtectedCallResult result;
				result.succeeded = true;
				return result;
			}
			ProtectedCallResult result;
			result.failure = AgentBehaviourRuntimeFailure::ConversionError;
			result.diagnostic = std::format(
				"Agent behaviour callback limit of {} per World boundary exceeded",
				callbackLimit);
			result.traceback = result.diagnostic;
			return result;
		}

		bool finishCallback(Instance& instance, std::string_view callback,
			ProtectedCallResult const& result,
			std::vector<PendingMovementCommand>& commands,
			std::vector<PendingAuthorizationCommand>& authorizationCommands)
		{
			instance.scope.active = false;
			if (result.succeeded)
			{
				instance.timers = std::move(instance.scope.stagedTimers);
				instance.randomState = instance.scope.stagedRandomState;
				commands.insert(commands.end(), instance.scope.commands.begin(),
					instance.scope.commands.end());
				authorizationCommands.insert(authorizationCommands.end(),
					instance.scope.authorizationCommands.begin(),
					instance.scope.authorizationCommands.end());
				publishLogs(instance);
			}
			else
			{
				instance.disabled = true;
				record(instance, AgentBehaviourRuntimeStage::Callback, callback, result);
			}
			instance.scope.commands.clear();
			instance.scope.authorizationCommands.clear();
			instance.scope.logs.clear();
			instance.scope.stagedTimers.clear();
			return result.succeeded;
		}

		bool pushCallback(Instance& instance, char const* callback)
		{
			auto* lua = state.get();
			lua_rawgeti(lua, LUA_REGISTRYINDEX, instance.instanceReference);
			lua_pushstring(lua, callback);
			lua_rawget(lua, -2);
			lua_remove(lua, -2);
			if (lua_isnil(lua, -1))
			{
				lua_pop(lua, 1);
				return false;
			}
			return lua_isfunction(lua, -1);
		}

		void pushSemanticEvent(PendingOutcome const& outcome)
		{
			auto* lua = state.get();
			lua_newtable(lua);
			auto const backing = lua_gettop(lua);
			std::string_view type;
			switch (outcome.type)
			{
			case OutcomeType::DestinationReached: type = "destination_reached"; break;
			case OutcomeType::ActionFailed: type = "action_failed"; break;
			case OutcomeType::MovementCancelled: type = "movement_cancelled"; break;
			case OutcomeType::InteractionCompleted: type = "interaction_completed"; break;
			case OutcomeType::InteractionFailed: type = "interaction_failed"; break;
			case OutcomeType::Activated: type = "activated"; break;
			case OutcomeType::Deactivated: type = "deactivated"; break;
			case OutcomeType::RouteLost: type = "route_lost"; break;
			}
			lua_pushlstring(lua, type.data(), type.size());
			lua_setfield(lua, backing, "type");
			lua_pushinteger(lua, static_cast<lua_Integer>(outcome.tick));
			lua_setfield(lua, backing, "tick");
			lua_pushinteger(lua, static_cast<lua_Integer>(outcome.sequence));
			lua_setfield(lua, backing, "sequence");
			if (outcome.type == OutcomeType::DestinationReached
				|| outcome.type == OutcomeType::ActionFailed
				|| outcome.type == OutcomeType::RouteLost
				|| outcome.type == OutcomeType::MovementCancelled)
			{
				pushMarkerHandle(lua, outcome.destination);
				lua_setfield(lua, backing, "destination");
				lua_pushlstring(lua, outcome.action.data(), outcome.action.size());
				lua_setfield(lua, backing, "action");
				lua_pushstring(lua, outcome.type == OutcomeType::DestinationReached ? "succeeded"
					: outcome.type == OutcomeType::MovementCancelled ? "cancelled" : "failed");
				lua_setfield(lua, backing, "result");
				if (outcome.type == OutcomeType::ActionFailed)
				{
					lua_pushstring(lua, outcome.scriptFailure == ScriptExecutionFailure::None
						? "refused" : "script_error");
					lua_setfield(lua, backing, "reason");
					char const* failure = "none";
					switch (outcome.scriptFailure)
					{
					case ScriptExecutionFailure::None: break;
					case ScriptExecutionFailure::LuaError: failure = "lua_error"; break;
					case ScriptExecutionFailure::MemoryBudgetExceeded: failure = "memory_budget_exceeded"; break;
					case ScriptExecutionFailure::InstructionBudgetExceeded: failure = "instruction_budget_exceeded"; break;
					case ScriptExecutionFailure::ConversionError: failure = "conversion_error"; break;
					}
					lua_pushstring(lua, failure);
					lua_setfield(lua, backing, "script_failure");
					lua_pushlstring(lua, outcome.diagnostic.data(), outcome.diagnostic.size());
					lua_setfield(lua, backing, "diagnostic");
				}
				if (outcome.type == OutcomeType::RouteLost)
				{
					auto const reason = routeLossReasonName(outcome.routeLossReason);
					lua_pushlstring(lua, reason.data(), reason.size());
					lua_setfield(lua, backing, "reason");
				}
				if (outcome.type == OutcomeType::MovementCancelled)
				{
					auto const reason = cancellationReasonName(outcome.cancellationReason);
					lua_pushlstring(lua, reason.data(), reason.size());
					lua_setfield(lua, backing, "reason");
				}
			}
			else if (outcome.type == OutcomeType::InteractionCompleted
				|| outcome.type == OutcomeType::InteractionFailed)
			{
				pushOpaqueHandle(lua, InteractionHandle{ outcome.interaction },
					InteractionMetatable);
				lua_setfield(lua, backing, "interaction");
				lua_pushlstring(lua, outcome.interactionName.data(),
					outcome.interactionName.size());
				lua_setfield(lua, backing, "name");
				auto const value = interactionResultName(outcome.interactionResult);
				lua_pushlstring(lua, value.data(), value.size());
				lua_setfield(lua, backing,
					outcome.type == OutcomeType::InteractionCompleted
						? "result" : "reason");
			}
			pushImmutableProxy(lua);
		}

		void teardownInstance(World& world, Instance& instance,
			AgentBehaviourTeardownReason reason, bool keepDisabled)
		{
			auto* lua = state.get();
			instance.scope.active = false;
			instance.scope.commands.clear();
			if (instance.instanceReference != LUA_NOREF)
			{
				auto const base = lua_gettop(lua);
				if (pushCallback(instance, "on_stop"))
				{
					std::vector<PendingMovementCommand> noCommands;
					std::vector<PendingAuthorizationCommand> noAuthorizationCommands;
					prepareScope(world, instance.scope.agent, instance, noCommands,
						noAuthorizationCommands);
					auto const reasonName = teardownReasonName(reason);
					lua_pushlstring(lua, reasonName.data(), reasonName.size());
					pushReadOnlyContext(world, instance);
					auto const result = protectedCall(lua, budget, 2, 0);
					instance.scope.active = false;
					if (result.succeeded) publishLogs(instance);
					else record(instance, AgentBehaviourRuntimeStage::Callback,
						"on_stop", result);
					instance.scope.logs.clear();
					instance.scope.commands.clear();
					instance.scope.stagedTimers.clear();
				}
				lua_settop(lua, base);
			}
			instance.outcomes.clear();
			instance.lifecycleOutcomes.clear();
			instance.timers.clear();
			release(instance);
			instance.disabled = keepDisabled;
			(void)lua_gc(lua, LUA_GCCOLLECT);
		}

		void applyActivation(AgentId agent, bool active, uint64_t tick,
			PendingOutcome outcome)
		{
			auto found = instances.find(agent);
			if (found == instances.end())
			{
				pendingLifecycleOutcomes[agent].push_back(std::move(outcome));
				return;
			}
			auto& instance = found->second;
			if (active)
			{
				if (instance.suspended)
					for (auto& [name, remaining] : instance.timers)
					{
						(void)name;
						remaining = remaining > std::numeric_limits<uint64_t>::max() - tick
							? std::numeric_limits<uint64_t>::max() : tick + remaining;
					}
				instance.suspended = false;
			}
			else
			{
				if (!instance.suspended)
					for (auto& [name, dueTick] : instance.timers)
					{
						(void)name;
						dueTick = dueTick > tick ? dueTick - tick : 0;
					}
				instance.suspended = true;
			}
			instance.lifecycleOutcomes.push_back(std::move(outcome));
		}

		void dispatchOutcome(World& world, AgentId agentId, Instance& instance,
			PendingOutcome const& outcome,
			std::vector<PendingMovementCommand>& commands,
			std::vector<PendingAuthorizationCommand>& authorizationCommands)
		{
			auto* lua = state.get();
			auto const base = lua_gettop(lua);
			if (outcome.type == OutcomeType::RouteLost)
			{
				if (pushCallback(instance, "on_route_lost"))
				{
					prepareScope(world, agentId, instance, commands,
						authorizationCommands);
					pushMarkerHandle(lua, outcome.destination);
					auto const reason = routeLossReasonName(outcome.routeLossReason);
					lua_pushlstring(lua, reason.data(), reason.size());
					pushContext(world, instance);
					pushSemanticEvent(outcome);
					auto const admission = admitCallback();
					auto const result = admission.succeeded
						? protectedCall(lua, budget, 4, 0) : admission;
					finishCallback(instance, "on_route_lost", result, commands,
						authorizationCommands);
				}
			}
			else if (pushCallback(instance, "on_event"))
			{
				prepareScope(world, agentId, instance, commands,
					authorizationCommands);
				pushSemanticEvent(outcome);
				pushContext(world, instance);
				auto const admission = admitCallback();
				auto const result = admission.succeeded
					? protectedCall(lua, budget, 2, 0) : admission;
				finishCallback(instance, "on_event", result, commands,
					authorizationCommands);
			}
			lua_settop(lua, base);
		}

		void runBoundaryCallbacks(World& world)
		{
			callbackCount = 0;
			currentTick = world.mSimulationTick;
			std::vector<PendingMovementCommand> commands;
			std::vector<PendingAuthorizationCommand> authorizationCommands;
			std::vector<AgentId> disabledAgents;
			for (auto& [agentId, instance] : instances)
			{
				if (instance.disabled) continue;
				auto agent = world.mAgents.find(agentId);
				if (!agent) continue;
				auto* lua = state.get();

				std::sort(instance.lifecycleOutcomes.begin(),
					instance.lifecycleOutcomes.end(),
					[](PendingOutcome const& lhs, PendingOutcome const& rhs)
					{ return lhs.sequence < rhs.sequence; });
				for (auto const& outcome : instance.lifecycleOutcomes)
				{
					dispatchOutcome(world, agentId, instance, outcome, commands,
						authorizationCommands);
					if (instance.disabled) break;
				}
				instance.lifecycleOutcomes.clear();

				if (!instance.disabled && !instance.suspended && agent->isActive()
					&& !instance.started)
				{
					instance.started = true;
					auto const base = lua_gettop(lua);
					if (pushCallback(instance, "on_start"))
					{
						prepareScope(world, agentId, instance, commands,
							authorizationCommands);
						pushContext(world, instance);
						lua_rawgeti(lua, LUA_REGISTRYINDEX,
							instance.configurationReference);
						auto const admission = admitCallback();
						auto const result = admission.succeeded
							? protectedCall(lua, budget, 2, 0) : admission;
						finishCallback(instance, "on_start", result, commands,
							authorizationCommands);
					}
					lua_settop(lua, base);
				}

				if (!instance.disabled && !instance.suspended && agent->isActive())
				{
					std::sort(instance.outcomes.begin(), instance.outcomes.end(),
						[](PendingOutcome const& lhs, PendingOutcome const& rhs)
						{ return lhs.sequence < rhs.sequence; });
					for (auto const& outcome : instance.outcomes)
					{
						dispatchOutcome(world, agentId, instance, outcome, commands,
							authorizationCommands);
						if (instance.disabled) break;
					}
					instance.outcomes.clear();
				}

				if (!instance.disabled && !instance.suspended && agent->isActive())
				{
					std::vector<std::string> dueTimers;
					for (auto const& [name, dueTick] : instance.timers)
						if (dueTick <= world.mSimulationTick) dueTimers.push_back(name);
					// Due timers form this boundary's immutable callback batch. Erasing all
					// before the first callback preserves one-shot semantics.
					for (auto const& name : dueTimers) instance.timers.erase(name);
					for (auto const& name : dueTimers)
					{
						auto const base = lua_gettop(lua);
						if (pushCallback(instance, "on_timer"))
						{
							prepareScope(world, agentId, instance, commands,
								authorizationCommands);
							lua_pushlstring(lua, name.data(), name.size());
							pushContext(world, instance);
							auto const admission = admitCallback();
							auto const result = admission.succeeded
								? protectedCall(lua, budget, 2, 0) : admission;
							finishCallback(instance, "on_timer", result, commands,
								authorizationCommands);
						}
						lua_settop(lua, base);
						if (instance.disabled) break;
					}
				}
				if (instance.disabled)
				{
					teardownInstance(world, instance,
						AgentBehaviourTeardownReason::InstanceFailure, true);
					disabledAgents.push_back(agentId);
				}
			}

			// Every callback above has returned and the phase marker is still None.
			// Apply only complete successful callback batches, in stable Agent/event
			// order. Authorization is applied before movement so a move issued in the
			// same callback sees the newly current routing constraints.
			for (auto const& command : authorizationCommands)
			{
				if (std::find(disabledAgents.begin(), disabledAgents.end(), command.agent)
					!= disabledAgents.end()) continue;
				switch (command.type)
				{
				case PendingAuthorizationCommandType::GrantAccessPermission:
					(void)world.setAgentRuntimeAccessPermissionGrant(
						command.agent, command.permission, true);
					break;
				case PendingAuthorizationCommandType::RevokeAccessPermission:
					(void)world.setAgentRuntimeAccessPermissionGrant(
						command.agent, command.permission, false);
					break;
				case PendingAuthorizationCommandType::AssignPermissionSet:
					(void)world.setAgentRuntimePermissionSetAssignment(
						command.agent, command.permissionSet, true);
					break;
				case PendingAuthorizationCommandType::UnassignPermissionSet:
					(void)world.setAgentRuntimePermissionSetAssignment(
						command.agent, command.permissionSet, false);
					break;
				}
			}
			for (auto const& command : commands)
			{
				if (std::find(disabledAgents.begin(), disabledAgents.end(), command.agent)
					!= disabledAgents.end()) continue;
				if (command.type == PendingMovementCommandType::MoveTo)
					(void)world.moveBehaviourAgentToMarker(
						command.agent, command.marker, command.action);
				else
					(void)world.cancelBehaviourAgentMovement(command.agent);
			}
			for (auto agentId : disabledAgents)
				(void)world.cancelBehaviourAgentMovement(agentId);
		}
	};

	AgentBehaviourRuntimeAdapter::AgentBehaviourRuntimeAdapter(
		AgentBehaviourRuntimeLimits limits)
	{
		if (!limitsAreValid(limits))
			throw std::invalid_argument("Agent behaviour runtime limits must be nonzero");
		if (limits.memoryBytes < MinimumMemoryBudgetBytes)
			throw std::invalid_argument(
				"Agent behaviour memory budget is below the minimum required to initialize the Lua sandbox");
		mImpl = std::make_unique<Impl>(limits);
	}

	AgentBehaviourRuntimeAdapter::~AgentBehaviourRuntimeAdapter() = default;

	bool AgentBehaviourRuntimeAdapter::prepareReload(World& world,
		AgentBehaviourRegistry const& registry,
		std::unique_ptr<AgentBehaviourRuntimeAdapter>& candidate,
		std::vector<AgentBehaviourRuntimeDiagnostic>& diagnostics,
		std::map<AgentId, AgentBehaviourAssignment> const* assignments,
		AgentBehaviourId excludedBehaviour)
	{
		candidate.reset();
		diagnostics.clear();
		try
		{
			auto prepared = std::make_unique<AgentBehaviourRuntimeAdapter>(
				world.mAgentBehaviourRuntime->getLimits());
			prepared->mImpl->currentTick = world.mSimulationTick;

			Impl::SourcePack sources;

			std::vector<Impl::Definition> definitions;
			bool packageNameResolved = false;
			for (auto const& [agentId, agent] : world.mAgents.entries())
			{
				if (!agent || !agent->getBehaviourAssignment()) continue;
				auto const overrideAssignment = assignments
					? assignments->find(agentId) : std::map<AgentId,
						AgentBehaviourAssignment>::const_iterator{};
				auto const& assignment = assignments
					&& overrideAssignment != assignments->end()
					? overrideAssignment->second : *agent->getBehaviourAssignment();
				if (excludedBehaviour && assignment.behaviour == excludedBehaviour)
					continue;
				auto const* behaviour = registry.lookupAgentBehaviour(
					assignment.behaviour);
				if (!behaviour) continue;
				auto source = registry.mSourceCache.find(
					behaviour->getSourceModulePath());
				if (source == registry.mSourceCache.end()) continue;
				if (!packageNameResolved)
				{
					packageNameResolved = true;
					sources.packageName = registry.mPackageDirectory
						? registry.mPackageDirectory->filename().string()
						: world.getAgentBehaviourRegistryResourceName();
				}
				definitions.push_back({ agentId, agent->getName(), behaviour->getName(),
					&assignment, agent->isActive(),
					deriveRandomSeed(world.mRandomSeed, agentId, assignment.behaviour),
					registry.getUuid(), registry.getPackageRevision(),
					sources.packageName, behaviour->getSourceModulePath(),
					source->second, &sources.helpers });
			}

			// EntityRegistry iteration is stable Agent-ID order. Unlike live
			// synchronization, every factory is attempted so the editor can report
			// all configuration-specific failures in one reload attempt.
			for (auto const& definition : definitions)
			{
				// Reload always constructs every instance, so its shared sources are
				// materialized once rather than copied into each definition.
				sources.materialize(registry);
				Impl::Instance instance;
				instance.assignment = *definition.assignment;
				instance.agentName = definition.agentName;
				instance.behaviourName = definition.behaviourName;
				instance.suspended = !definition.active;
				instance.randomState = definition.randomSeed;
				instance.registryUuid = definition.registryUuid;
				instance.packageRevision = definition.packageRevision;
				instance.packageName = definition.packageName;
				instance.moduleName = definition.moduleName;
				instance.scope.agent = definition.agent;
				if (!prepared->mImpl->construct(definition, instance))
				{
					prepared->mImpl->release(instance);
					(void)lua_gc(prepared->mImpl->state.get(), LUA_GCCOLLECT);
					continue;
				}
				prepared->mImpl->instances.emplace(definition.agent,
					std::move(instance));
			}

			diagnostics = prepared->mImpl->diagnostics;
			if (!diagnostics.empty()) return false;
			candidate = std::move(prepared);
			return true;
		}
		catch (std::exception const& error)
		{
			diagnostics.push_back({ AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Factory, {}, {}, world.mSimulationTick,
				{}, {}, world.hasAgentBehaviourRegistryReference()
					? world.getAgentBehaviourRegistryResourceName() : std::string{},
				{}, {}, error.what(), error.what() });
			return false;
		}
		catch (...)
		{
			diagnostics.push_back({ AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Factory, {}, {}, world.mSimulationTick,
				{}, {}, world.hasAgentBehaviourRegistryReference()
					? world.getAgentBehaviourRegistryResourceName() : std::string{},
				{}, {}, "Unknown Lua reload preflight failure",
				"Unknown Lua reload preflight failure" });
			return false;
		}
	}

	void AgentBehaviourRuntimeAdapter::appendDiagnostics(
		std::vector<AgentBehaviourRuntimeDiagnostic> diagnostics)
	{
		mImpl->diagnostics.insert(mImpl->diagnostics.end(),
			std::make_move_iterator(diagnostics.begin()),
			std::make_move_iterator(diagnostics.end()));
	}

	bool AgentBehaviourRuntimeAdapter::runBoundary(World& world)
	{
		if (world.mCurrentPhase != SimulationPhase::None) return true;
		if (mImpl->stateFailed) return true;
		auto const diagnosticsBefore = mImpl->diagnostics.size();
		std::vector<Impl::Definition> definitions;
		Impl::SourcePack sources;
		auto const registry = world.mAgentBehaviourRegistry;
		if (registry && registry->mPackageDirectory)
		{
			sources.packageName = registry->mPackageDirectory->filename().string();
			for (auto const& [agentId, agent] : world.mAgents.entries())
			{
				if (!agent || !agent->getBehaviourAssignment()) continue;
				auto const& assignment = *agent->getBehaviourAssignment();
				auto const* behaviour = registry->lookupAgentBehaviour(
					assignment.behaviour);
				if (!behaviour || behaviour->getModuleStatus()
					!= AgentBehaviourModuleStatus::Loaded) continue;
				auto source = registry->mSourceCache.find(
					behaviour->getSourceModulePath());
				if (source == registry->mSourceCache.end()) continue;
				definitions.push_back({ agentId, agent->getName(), behaviour->getName(),
					&assignment, agent->isActive(),
					deriveRandomSeed(world.mRandomSeed, agentId, assignment.behaviour),
					registry->getUuid(), registry->getPackageRevision(),
					sources.packageName, behaviour->getSourceModulePath(),
					source->second, &sources.helpers });
			}
		}
		try
		{
			mImpl->currentTick = world.mSimulationTick;
			mImpl->synchronize(world, registry.get(), sources, definitions);
			mImpl->runBoundaryCallbacks(world);
		}
		catch (LuaPanicError const& error)
		{
			// The panic handler fired on an unprotected allocation. Lua has reset
			// the thread but it is not safe to reuse, so the runtime is abandoned
			// and every behaviour instance is disabled for the rest of the run.
			mImpl->diagnostics.push_back({
				AgentBehaviourRuntimeFailure::MemoryBudgetExceeded,
				AgentBehaviourRuntimeStage::Callback, {}, {}, world.mSimulationTick,
				{}, {}, {}, {}, {}, error.what(), error.what() });
			mImpl->poison();
		}
		catch (std::exception const& error)
		{
			// sol2 conversions and adapter-side Lua value marshaling are contained at
			// the same boundary as protected Lua errors. The complete failure policy
			// (pause/headless stop and module scope) is layered by ticket #159.
			mImpl->diagnostics.push_back({
				AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Callback, {}, {}, world.mSimulationTick,
				{}, {}, {}, {}, {}, error.what(), error.what() });
			for (auto& [agent, instance] : mImpl->instances)
			{
				(void)agent;
				if (!instance.scope.active) continue;
				instance.scope.active = false;
				instance.scope.commands.clear();
				instance.disabled = true;
			}
		}
		catch (...)
		{
			mImpl->diagnostics.push_back({
				AgentBehaviourRuntimeFailure::ConversionError,
				AgentBehaviourRuntimeStage::Callback, {}, {}, world.mSimulationTick,
				{}, {}, {}, {}, {}, "Unknown Lua adapter conversion failure",
				"Unknown Lua adapter conversion failure" });
			for (auto& [agent, instance] : mImpl->instances)
			{
				(void)agent;
				if (!instance.scope.active) continue;
				instance.scope.active = false;
				instance.scope.commands.clear();
				instance.disabled = true;
			}
		}
		auto const succeeded = mImpl->diagnostics.size() == diagnosticsBefore;
		if (!succeeded && !world.mSimulationPaused) world.pauseSimulation();
		return succeeded;
	}

	void AgentBehaviourRuntimeAdapter::observeOutcome(SimulationEvent const& event)
	{
		if (mImpl->stateFailed) return;
		Impl::PendingOutcome outcome;
		AgentId agent;
		outcome.sequence = event.sequence;
		outcome.tick = event.tick;
		if (event.type == SimulationEventType::DestinationReached
			|| event.type == SimulationEventType::MovementCancelled
			|| event.type == SimulationEventType::RouteLost
			|| event.type == SimulationEventType::ActionFailed)
		{
			agent = event.agent.id;
			outcome.type = event.type == SimulationEventType::DestinationReached
				? Impl::OutcomeType::DestinationReached
				: event.type == SimulationEventType::MovementCancelled
					? Impl::OutcomeType::MovementCancelled
					: Impl::OutcomeType::RouteLost;
			if (event.type == SimulationEventType::ActionFailed)
				outcome.type = Impl::OutcomeType::ActionFailed;
			outcome.destination = event.destinationMarker;
			outcome.action = event.selectedAction;
			outcome.diagnostic = event.diagnostic;
			outcome.scriptFailure = event.scriptFailure;
			outcome.routeLossReason = event.routeLossReason;
			outcome.cancellationReason = event.movementCancellationReason;
		}
		else if (event.type == SimulationEventType::InteractionRequestChanged
			&& event.interactionRequest.result != InteractionResult::Pending)
		{
			agent = event.interactionRequest.actor;
			outcome.type = event.interactionRequest.result == InteractionResult::Succeeded
				|| event.interactionRequest.result
					== InteractionResult::SucceededWithBestEffortFailure
				? Impl::OutcomeType::InteractionCompleted
				: Impl::OutcomeType::InteractionFailed;
			outcome.interaction = event.interactionRequest.id;
			outcome.interactionName = event.interactionName;
			outcome.interactionResult = event.interactionRequest.result;
		}
		else return;

		auto found = mImpl->instances.find(agent);
		if (found == mImpl->instances.end() || found->second.disabled) return;
		if (found->second.apiVersion == 1
			&& outcome.type == Impl::OutcomeType::MovementCancelled
			&& outcome.cancellationReason == MovementCancellationReason::Superseded) return;
		found->second.outcomes.push_back(std::move(outcome));
		++mImpl->observedOutcomeCount;
		mImpl->lastObservedSequence = event.sequence;
	}

	void AgentBehaviourRuntimeAdapter::observeActivation(
		SimulationEvent const& event)
	{
		if (mImpl->stateFailed) return;
		if (event.type != SimulationEventType::AgentActivated
			&& event.type != SimulationEventType::AgentDeactivated) return;
		Impl::PendingOutcome outcome;
		outcome.sequence = event.sequence;
		outcome.tick = event.tick;
		outcome.type = event.type == SimulationEventType::AgentActivated
			? Impl::OutcomeType::Activated : Impl::OutcomeType::Deactivated;
		mImpl->applyActivation(event.agent.id,
			event.type == SimulationEventType::AgentActivated, event.tick,
			std::move(outcome));
	}

	void AgentBehaviourRuntimeAdapter::removeInstance(World& world,
		AgentId agent, AgentBehaviourTeardownReason reason)
	{
		if (mImpl->stateFailed) return;
		mImpl->pendingLifecycleOutcomes.erase(agent);
		auto found = mImpl->instances.find(agent);
		if (found == mImpl->instances.end()) return;
		try
		{
			mImpl->teardownInstance(world, found->second, reason, false);
		}
		catch (...)
		{
			// Adapter-side conversion/allocation failures are no more entitled to
			// veto teardown than a protected Lua error.
			found->second.scope.active = false;
			found->second.scope.commands.clear();
			mImpl->release(found->second);
		}
		mImpl->instances.erase(found);
	}

	void AgentBehaviourRuntimeAdapter::teardownAll(World& world,
		AgentBehaviourTeardownReason reason)
	{
		if (mImpl->stateFailed) return;
		for (auto& [agent, instance] : mImpl->instances)
		{
			(void)agent;
			try
			{
				mImpl->teardownInstance(world, instance, reason, false);
			}
			catch (...)
			{
				instance.scope.active = false;
				instance.scope.commands.clear();
				mImpl->release(instance);
			}
		}
		mImpl->instances.clear();
		mImpl->disabledBehaviours.clear();
		mImpl->pendingLifecycleOutcomes.clear();
		mImpl->observedOutcomeCount = 0;
		mImpl->lastObservedSequence = 0;
	}

	bool AgentBehaviourRuntimeAdapter::isInstanceDisabled(AgentId agent) const
	{
		auto found = mImpl->instances.find(agent);
		return found != mImpl->instances.end() && found->second.disabled;
	}

	std::vector<AgentBehaviourRuntimeDiagnostic>
	AgentBehaviourRuntimeAdapter::getDiagnostics() const
	{
		return mImpl->diagnostics;
	}

	std::vector<AgentBehaviourRuntimeDiagnostic>
	AgentBehaviourRuntimeAdapter::consumeDiagnostics()
	{
		auto result = std::move(mImpl->diagnostics);
		mImpl->diagnostics.clear();
		return result;
	}

	AgentBehaviourRuntimeLimits AgentBehaviourRuntimeAdapter::getLimits() const
	{
		return { mImpl->budget.byteLimit, mImpl->budget.instructionLimit,
			mImpl->timerLimit, mImpl->callbackLimit, mImpl->commandLimit,
			mImpl->logLimit, mImpl->logWindowTicks, mImpl->logMessageByteLimit,
			mImpl->logWindowByteLimit };
	}

	void AgentBehaviourRuntimeAdapter::reset()
	{
		mImpl->clear();
	}
}
