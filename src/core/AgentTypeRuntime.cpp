#include "core/AgentTypeRuntime.h"
#include "core/AgentType.h"
#include "core/SerializationException.h"
#include "ScriptSandbox.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace core
{
	namespace
	{
		using namespace script;

		constexpr char ReservedBehaviourDiagnostic[] =
			"Agent type instance member 'behaviour' is reserved for the runtime-owned Installed behaviour";

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
			if (apiVersion != 2)
			{
				call.diagnostic = "Agent type requires api_version = 2; migrate API v1 flat pose ratios to poses and automatic_poses";
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

		[[noreturn]] void poseError(std::string const& field, std::string const& reason)
		{
			throw SerializationException("Agent type pose field '" + field + "' " + reason);
		}

		void requireTable(lua_State* state, int index, std::string const& field)
		{
			if (!lua_istable(state, index)) poseError(field, "must be a table");
		}

		void checkKeys(lua_State* state, int index, std::string const& field,
			std::initializer_list<std::string_view> allowed)
		{
			index = lua_absindex(state, index);
			lua_pushnil(state);
			while (lua_next(state, index))
			{
				if (lua_type(state, -2) != LUA_TSTRING) poseError(field, "contains a non-string field");
				size_t length;
				auto text = lua_tolstring(state, -2, &length);
				auto key = std::string_view(text, length);
				bool known = false;
				for (auto candidate : allowed) known = known || candidate == key;
				if (!known) poseError(field + "." + std::string(key), "is unknown");
				lua_pop(state, 1);
			}
		}

		float readRatio(lua_State* state, int index, char const* key, std::string const& field,
			bool bounded = true)
		{
			lua_pushstring(state, key);
			lua_rawget(state, index);
			auto const isNumber = lua_type(state, -1) == LUA_TNUMBER;
			auto const value = lua_tonumber(state, -1);
			lua_pop(state, 1);
			if (!isNumber || !std::isfinite(value) || value <= 0 || (bounded && value > 1))
				poseError(field, bounded ? "must be a finite number within (0, 1]" : "must be a finite positive number");
			auto const frozen = static_cast<float>(value);
			if (!std::isfinite(frozen) || frozen <= 0 || (bounded && frozen > 1))
				poseError(field, "must be representable as a finite positive simulation ratio");
			return frozen;
		}

		void readPoseContract(lua_State* state, int instance, AgentPhysicalBaseline& baseline)
		{
			// Private instance data is allowed; retired physical fields are not.
			for (auto key : { "sitting_height_ratio", "crouching_height_ratio", "crawling_height_ratio", "crawling_speed_ratio" })
			{
				lua_pushstring(state, key);
				lua_rawget(state, instance);
				auto present = !lua_isnil(state, -1);
				lua_pop(state, 1);
				if (present) poseError(key, "is retired; migrate to poses and automatic_poses (API v2)");
			}
			lua_pushliteral(state, "poses");
			lua_rawget(state, instance);
			requireTable(state, -1, "poses");
			auto const poses = lua_gettop(state);
			checkKeys(state, poses, "poses", { "standing", "sitting", "lying", "crouching", "crawling" });
			struct Entry { char const* name; Pose pose; bool lowered; };
			static constexpr Entry entries[] = {
				{ "standing", Pose::Standing, false }, { "sitting", Pose::Sitting, true },
				{ "lying", Pose::Lying, true }, { "crouching", Pose::Crouching, true },
				{ "crawling", Pose::Crawling, true }
			};
			for (auto const& entry : entries)
			{
				lua_pushstring(state, entry.name);
				lua_rawget(state, poses);
				if (!lua_isnil(state, -1))
				{
					auto const field = std::string("poses.") + entry.name;
					requireTable(state, -1, field);
					auto const definition = lua_gettop(state);
					checkKeys(state, definition, field, entry.lowered
						? std::initializer_list<std::string_view>{ "height_ratio", "width_ratio", "image_tile" }
						: std::initializer_list<std::string_view>{ "image_tile" });
					AgentPoseDefinition pose{entry.lowered
						? readRatio(state, definition, "height_ratio", field + ".height_ratio") : 1.f, {}};
					if (entry.lowered)
					{
						lua_pushliteral(state, "width_ratio");
						lua_rawget(state, definition);
						bool const hasWidth = !lua_isnil(state, -1);
						lua_pop(state, 1);
						if (hasWidth) pose.widthRatio = readRatio(state, definition, "width_ratio", field + ".width_ratio", false);
						if (!std::isfinite(baseline.width * pose.widthRatio) || baseline.width * pose.widthRatio <= 0.f)
							poseError(field + ".width_ratio", "must produce a finite positive bodily width");
					}
					lua_pushliteral(state, "image_tile");
					lua_rawget(state, definition);
					bool const validTile = lua_type(state, -1) == LUA_TSTRING
						&& readStringField(state, -1, 128, pose.imageTile)
						&& agentTypeDisplayNameIsValid(pose.imageTile);
					lua_pop(state, 1);
					if (!validTile) poseError(field + ".image_tile", "must be a nonempty tile name of at most 128 bytes without control characters");
					baseline.poses.emplace(entry.pose, std::move(pose));
				}
				lua_pop(state, 1);
			}
			lua_pop(state, 1);
			if (!baseline.supportsPose(Pose::Standing)) poseError("poses.standing", "is required");
			for (auto pose : { Pose::Crouching, Pose::Crawling })
				if (baseline.supportsPose(pose) && baseline.poses.at(pose).heightRatio >= 1.f)
					poseError(pose == Pose::Crouching ? "poses.crouching.height_ratio" : "poses.crawling.height_ratio",
						"must be strictly less than standing (1)");
			if (baseline.supportsPose(Pose::Crouching) && baseline.supportsPose(Pose::Crawling)
				&& baseline.poses.at(Pose::Crawling).heightRatio >= baseline.poses.at(Pose::Crouching).heightRatio)
				poseError("poses.crawling.height_ratio", "must be strictly less than poses.crouching.height_ratio");

			lua_pushliteral(state, "automatic_poses");
			lua_rawget(state, instance);
			requireTable(state, -1, "automatic_poses");
			auto const automatic = lua_gettop(state);
			checkKeys(state, automatic, "automatic_poses", { "room_movement", "door_crossing" });
			for (auto context : { AutomaticPoseContext::RoomMovement, AutomaticPoseContext::DoorCrossing })
			{
				auto key = context == AutomaticPoseContext::RoomMovement ? "room_movement" : "door_crossing";
				auto const field = std::string("automatic_poses.") + key;
				lua_pushstring(state, key);
				lua_rawget(state, automatic);
				requireTable(state, -1, field);
				auto const choices = lua_gettop(state);
				// At most three locomotion poses; count keys, not Lua's ambiguous
				// length operator for sparse arrays.
				size_t count = 0;
				lua_pushnil(state);
				while (lua_next(state, choices))
				{
					if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 || lua_tointeger(state, -2) > 3)
						poseError(field, "must be a dense ordered choice array (at most three poses)");
					++count;
					lua_pop(state, 1);
				}
				if (!count) poseError(field, "must be nonempty");
				auto& result = context == AutomaticPoseContext::RoomMovement ? baseline.roomMovement : baseline.doorCrossing;
				for (size_t i = 1; i <= count; ++i)
				{
					auto const choiceField = field + "[" + std::to_string(i) + "]";
					lua_rawgeti(state, choices, i);
					requireTable(state, -1, choiceField);
					auto const choice = lua_gettop(state);
					checkKeys(state, choice, choiceField, { "pose", "speed_ratio" });
					lua_pushliteral(state, "pose");
					lua_rawget(state, choice);
					std::string name;
					if (lua_type(state, -1) != LUA_TSTRING || !readStringField(state, -1, 16, name))
						poseError(choiceField + ".pose", "must name a supported locomotion pose");
					lua_pop(state, 1);
					std::optional<Pose> pose;
					for (auto const& entry : entries)
						if (name == entry.name && (entry.pose == Pose::Standing || entry.pose == Pose::Crouching || entry.pose == Pose::Crawling)) pose = entry.pose;
					if (!pose || !baseline.supportsPose(*pose)) poseError(choiceField + ".pose", "is not a declared supported locomotion pose");
					if (i == 1 && *pose != Pose::Standing) poseError(choiceField + ".pose", "must start with standing");
					for (auto const& previous : result)
						if (previous.pose == *pose) poseError(choiceField + ".pose", "duplicates a pose");
					result.push_back({ *pose, readRatio(state, choice, "speed_ratio", choiceField + ".speed_ratio") });
					lua_pop(state, 1);
				}
				lua_pop(state, 1);
			}
			lua_pop(state, 1);
		}

		// Shared, validated baseline extraction from the instance table returned
		// by new(). Throws SerializationException with the offending field named.
		AgentPhysicalBaseline readBaseline(lua_State* state, int instance)
		{
			instance = lua_absindex(state, instance);
			AgentPhysicalBaseline baseline{};
			lua_pushliteral(state, "object_usage");
			lua_rawget(state, instance);
			std::string usage;
			if (!lua_isnil(state, -1)
				&& (lua_type(state, -1) != LUA_TSTRING || !readStringField(state, -1, 14, usage) || !parseObjectUsage(usage, baseline.objectUsage)))
				throw SerializationException("Agent type baseline field 'object_usage' must be 'arms', 'none' or 'remote_control'");
			lua_pop(state, 1);
			// Legacy reach is an input alias only, never a second frozen authority.
			// Reject dual declarations even when equal, so migration is unambiguous.
			lua_pushliteral(state, "object_usage_distance");
			lua_rawget(state, instance);
			bool const hasDistance = !lua_isnil(state, -1);
			lua_pop(state, 1);
			lua_pushliteral(state, "reach");
			lua_rawget(state, instance);
			bool const hasReach = !lua_isnil(state, -1);
			lua_pop(state, 1);
			if (baseline.objectUsage == ObjectUsage::RemoteControl && hasReach)
				throw SerializationException("Remote control requires 'object_usage_distance', not legacy 'reach'");
			if (hasDistance && hasReach)
				throw SerializationException("Agent type baseline fields 'reach' and 'object_usage_distance' are mutually exclusive");
			struct Field
			{
				char const* key;
				float AgentPhysicalBaseline::* destination;
				bool ratio;
			};
			Field const fields[] = {
				{ "width", &AgentPhysicalBaseline::width, false },
				{ "standing_height", &AgentPhysicalBaseline::standingHeight, false },
				{ hasReach ? "reach" : "object_usage_distance", &AgentPhysicalBaseline::objectUsageDistance, false },
				{ "walk_speed", &AgentPhysicalBaseline::walkSpeed, false },
				{ "climb_speed", &AgentPhysicalBaseline::climbSpeed, false },
				{ "stair_ascent_speed", &AgentPhysicalBaseline::stairAscentSpeed, false },
				{ "stair_descent_speed", &AgentPhysicalBaseline::stairDescentSpeed, false },
			};
			for (auto const& field : fields)
			{
				// None has no usable range. Ignore the distance input and freeze a
				// canonical zero; all other physical fields remain required.
				if (field.destination == &AgentPhysicalBaseline::objectUsageDistance)
				{
					if (baseline.objectUsage == ObjectUsage::None) continue;
					if (baseline.objectUsage == ObjectUsage::RemoteControl && !hasDistance)
					{
						baseline.objectUsageDistance = 1.f;
						continue;
					}
				}
				lua_pushstring(state, field.key);
				lua_rawget(state, instance);
				auto const missing = lua_isnil(state, -1);
				auto const value = lua_tonumber(state, -1);
				auto const isNumber = lua_type(state, -1) == LUA_TNUMBER;
				// Simulation consumes floats, not Lua doubles. Reject finite Lua
				// values that overflow or underflow the frozen physical baseline.
				auto const representable = std::isfinite(value) && value > 0
					&& value <= std::numeric_limits<float>::max();
				auto const frozen = representable ? static_cast<float>(value) : 0.f;
				lua_pop(state, 1);
				char const* reason = nullptr;
				if (missing) reason = "is missing";
				else if (!isNumber) reason = "must be a finite number";
				else if (!std::isfinite(value)) reason = "must be finite";
				else if (value <= 0) reason = "must be positive";
				else if (field.ratio && value > 1) reason = "must be within (0, 1]";
				else if (!std::isfinite(frozen) || frozen <= 0)
					reason = "must be representable as a finite positive float";
				if (reason)
					throw SerializationException(std::string("Agent type baseline field '")
						+ field.key + "' " + reason);
				baseline.*field.destination = frozen;
			}
			readPoseContract(state, instance, baseline);
			return baseline;
		}

		MobilityProfile readDefaultMobilityProfile(lua_State* state, int instance)
		{
			instance = lua_absindex(state, instance);
			lua_pushliteral(state, "mobility_profile");
			lua_rawget(state, instance);
			if (!lua_istable(state, -1))
			{
				lua_pop(state, 1);
				throw SerializationException("Agent type Mobility profile field 'mobility_profile' must be a table");
			}
			auto const profile = lua_gettop(state);
			struct Entry { char const* key; TraversalKind kind; };
			static constexpr Entry entries[] = {
				{ "staircase", TraversalKind::Staircase }, { "escalator", TraversalKind::Escalator },
				{ "stairwell", TraversalKind::Stairwell }, { "ladder", TraversalKind::Ladder },
				{ "lift", TraversalKind::Lift }, { "platform_lift", TraversalKind::PlatformLift },
				{ "shuttle", TraversalKind::Shuttle }, { "door", TraversalKind::Door },
				{ "buttons", TraversalKind::Buttons },
			};
			MobilityProfile result;
			for (auto const& entry : entries)
			{
				lua_pushstring(state, entry.key);
				lua_rawget(state, profile);
				auto const value = lua_tostring(state, -1);
				auto const isString = lua_type(state, -1) == LUA_TSTRING;
				lua_pop(state, 1);
				if (!isString)
					throw SerializationException(std::string("Agent type Mobility profile entry '")
						+ entry.key + "' is missing or must be a string");
				if (std::string_view(value) == "can_use") result.set(entry.kind, MobilityUse::CanUse);
				else if (std::string_view(value) == "cannot_use") result.set(entry.kind, MobilityUse::CannotUse);
				else if (std::string_view(value) == "only_if_no_other_option")
					result.set(entry.kind, MobilityUse::OnlyIfNoOtherOption);
				else throw SerializationException(std::string("Agent type Mobility profile entry '")
					+ entry.key + "' has invalid use '" + value + "'");
			}
			lua_pushnil(state);
			while (lua_next(state, profile))
			{
				if (lua_type(state, -2) != LUA_TSTRING)
				{
					lua_pop(state, 2);
					throw SerializationException("Agent type Mobility profile contains an unknown non-string entry");
				}
				auto const key = std::string_view(lua_tostring(state, -2));
				bool known = false;
				for (auto const& entry : entries) known = known || key == entry.key;
				lua_pop(state, 1);
				if (!known)
				{
					lua_pop(state, 1);
					throw SerializationException("Agent type Mobility profile contains unknown entry '" + std::string(key) + "'");
				}
			}
			lua_pop(state, 1);
			return result;
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
				lua_pushliteral(lua, "api_version");
				lua_rawget(lua, typeTable);
				auto const versionOk = lua_isinteger(lua, -1) && lua_tointeger(lua, -1) == 2;
				lua_pop(lua, 1);
				if (!versionOk)
					throw SerializationException("Agent type requires api_version = 2; migrate API v1 flat pose ratios to poses and automatic_poses");

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

				lua_pushliteral(lua, "behaviour");
				lua_rawget(lua, -2);
				auto const definesBehaviour = !lua_isnil(lua, -1);
				lua_pop(lua, 1);
				if (definesBehaviour)
				{
					result.failure = ScriptExecutionFailure::ConversionError;
					result.diagnostic = ReservedBehaviourDiagnostic;
					lua_settop(lua, base);
					return result;
				}

				// Freeze the baseline before publishing: copy it out of Lua so
				// later mutations of the live instance cannot change simulation.
				try
				{
					result.baseline = readBaseline(lua, -1);
					result.defaultMobilityProfile = readDefaultMobilityProfile(lua, -1);
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
			// The reserved name belongs to the returned instance, not the type
			// object. Probe new() in a separate bounded scratch runtime so neither
			// private state nor budget exhaustion can affect the live runtime.
			// Other constructor/baseline failures remain construction-time errors,
			// as before; only the reserved-member contract changes preflight.
			AgentTypeRuntimeAdapter probe({ 2u * 1024u * 1024u, 100'000u });
			auto const instance = probe.construct(call.type.typeId, source, call.type.displayName);
			if (instance.failure == ScriptExecutionFailure::ConversionError
				&& instance.diagnostic == ReservedBehaviourDiagnostic)
			{
				result.failure = instance.failure;
				result.diagnostic = instance.diagnostic;
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
