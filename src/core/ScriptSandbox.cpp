#include "ScriptSandbox.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace core::script
{
	int luaPanic(lua_State* state)
	{
		size_t length = 0;
		auto const* message = lua_tolstring(state, -1, &length);
		lua_settop(state, 0);
		throw LuaPanicError(message ? std::string(message, length)
			: "Lua sandbox allocation failed");
	}

	void* budgetedAllocate(void* userData, void* pointer, size_t oldSize,
		size_t newSize) noexcept
	{
		auto& budget = *static_cast<ScratchBudget*>(userData);
		// For a new block Lua passes a type tag rather than an allocation size
		// in oldSize; only an existing pointer makes that value chargeable.
		if (!pointer) oldSize = 0;
		if (newSize == 0)
		{
			std::free(pointer);
			budget.bytesUsed -= std::min(budget.bytesUsed, oldSize);
			return nullptr;
		}
		auto const growth = newSize > oldSize ? newSize - oldSize : 0;
		if (growth > budget.byteLimit - std::min(budget.byteLimit, budget.bytesUsed))
		{
			// A refused growth leaves both the original block and accounting intact,
			// exactly as Lua's allocator contract requires.
			budget.memoryLimitExceeded = true;
			return nullptr;
		}
		void* replacement = std::realloc(pointer, newSize);
		if (!replacement) return nullptr;
		if (newSize >= oldSize) budget.bytesUsed += newSize - oldSize;
		else budget.bytesUsed -= std::min(budget.bytesUsed, oldSize - newSize);
		return replacement;
	}

	void instructionHook(lua_State* state, lua_Debug*)
	{
		void* userData = nullptr;
		(void)lua_getallocf(state, &userData);
		auto& budget = *static_cast<ScratchBudget*>(userData);
		if (budget.instructionsRemaining <= budget.hookInterval)
		{
			budget.instructionsRemaining = 0;
			budget.instructionLimitExceeded = true;
			luaL_error(state, "Agent behaviour instruction budget exceeded");
			return;
		}
		budget.instructionsRemaining -= budget.hookInterval;
	}

	void beginInstructionBudget(lua_State* state, ScratchBudget& budget)
	{
		budget.instructionsRemaining = budget.instructionLimit;
		budget.hookInterval = std::min<uint32_t>(1'000, budget.instructionLimit);
		budget.memoryLimitExceeded = false;
		budget.instructionLimitExceeded = false;
		lua_sethook(state, instructionHook, LUA_MASKCOUNT,
			static_cast<int>(budget.hookInterval));
	}

	void endInstructionBudget(lua_State* state)
	{
		lua_sethook(state, nullptr, 0, 0);
	}

	ScratchBudget& scratchBudget(lua_State* state)
	{
		void* userData = nullptr;
		(void)lua_getallocf(state, &userData);
		return *static_cast<ScratchBudget*>(userData);
	}

	bool budgetExhausted(ScratchBudget const& budget)
	{
		return budget.instructionLimitExceeded || budget.memoryLimitExceeded;
	}

	char const* budgetDiagnostic(ScratchBudget const& budget)
	{
		return budget.memoryLimitExceeded
			? "Agent behaviour memory budget exceeded"
			: "Agent behaviour instruction budget exceeded";
	}

	int raiseBudgetDiagnostic(lua_State* state, ScratchBudget const& budget)
	{
		return luaL_error(state, "%s", budgetDiagnostic(budget));
	}

	// Lua's own pcall and xpcall can catch the instruction-hook and allocator
	// budget errors, so a hostile module could retry a caught exhaustion
	// forever or return a valid contract after exceeding a host budget. The
	// sandbox replaces them with a wrapper that raises as soon as a sticky
	// budget flag is set; an outer wrapper re-raises on the way out, so the
	// failure always reaches the host boundary regardless of nesting.
	int budgetedProtectedCall(lua_State* state)
	{
		auto& budget = scratchBudget(state);
		if (budgetExhausted(budget)) return raiseBudgetDiagnostic(state, budget);
		auto const argumentCount = lua_gettop(state);
		lua_pushvalue(state, lua_upvalueindex(1));
		lua_insert(state, 1);
		lua_call(state, argumentCount, LUA_MULTRET);
		if (budgetExhausted(budget)) return raiseBudgetDiagnostic(state, budget);
		return lua_gettop(state);
	}

	void installBudgetedProtectedCall(lua_State* state, char const* name)
	{
		lua_getglobal(state, name);
		if (!lua_isfunction(state, -1) || !lua_iscfunction(state, -1))
		{
			lua_pop(state, 1);
			return;
		}
		lua_pushcclosure(state, budgetedProtectedCall, 1);
		lua_setglobal(state, name);
	}

	int tracebackHandler(lua_State* state)
	{
		auto const* message = lua_tostring(state, 1);
		if (message) luaL_traceback(state, state, message, 1);
		else
		{
			lua_pushliteral(state, "Lua error with no string diagnostic");
		}
		return 1;
	}

	int immutableNewIndex(lua_State* state)
	{
		return luaL_error(state, "Agent behaviour exported value is immutable");
	}

	void pushImmutableValue(lua_State* state, int value,
		std::map<void const*, int>& visited)
	{
		value = lua_absindex(state, value);
		if (!lua_istable(state, value))
		{
			lua_pushvalue(state, value);
			return;
		}

		auto const identity = lua_topointer(state, value);
		if (auto found = visited.find(identity); found != visited.end())
		{
			lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
			return;
		}

		lua_newtable(state);
		auto const backing = lua_gettop(state);
		(void)lua_newuserdatauv(state, 1, 0);
		auto const proxy = lua_gettop(state);
		lua_newtable(state);
		lua_pushvalue(state, backing);
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, immutableNewIndex);
		lua_setfield(state, -2, "__newindex");
		lua_pushliteral(state, "immutable");
		lua_setfield(state, -2, "__metatable");
		lua_setmetatable(state, proxy);
		lua_pushvalue(state, proxy);
		visited.emplace(identity, luaL_ref(state, LUA_REGISTRYINDEX));

		lua_pushnil(state);
		while (lua_next(state, value) != 0)
		{
			pushImmutableValue(state, -2, visited);
			pushImmutableValue(state, -2, visited);
			lua_rawset(state, backing);
			lua_pop(state, 1);
		}
		lua_pushvalue(state, proxy);
		lua_remove(state, backing);
		lua_remove(state, backing);
	}

	// Leaves either the copied string or Lua's allocation-error object on
	// the stack. The owner can then go out of scope before lua_error.
	void pushBorrowedDiagnostic(lua_State* state, std::string const& diagnostic)
	{
		auto copy = [&diagnostic](lua_State* inner)
		{
			lua_pushlstring(inner, diagnostic.data(), diagnostic.size());
			return 1;
		};
		(void)protectedLuaOperation(state, copy);
	}

	void makeTopImmutable(lua_State* state)
	{
		int status;
		{
			std::map<void const*, int> visited;
			auto convert = [&visited](lua_State* inner)
			{
				pushImmutableValue(inner, 1, visited);
				return 1;
			};
			status = protectedLuaOperation(state, convert, 1);
			for (auto const& [identity, reference] : visited)
			{
				(void)identity;
				luaL_unref(state, LUA_REGISTRYINDEX, reference);
			}
		}
		if (status != LUA_OK) lua_error(state);
	}

	int loadDeclaredModule(lua_State* state, ModuleLoader& loader,
		std::string const& requested, std::string& chunkName, std::string& chain)
	{
		auto const* requestedText = requested.c_str();
		if (auto host = loader.hostModules.find(requested);
			host != loader.hostModules.end())
		{
			lua_rawgeti(state, LUA_REGISTRYINDEX, host->second);
			return 1;
		}
		if (auto loaded = loader.loadedModules.find(requested);
			loaded != loader.loadedModules.end())
		{
			lua_rawgeti(state, LUA_REGISTRYINDEX, loaded->second);
			return 1;
		}
		auto module = loader.modules.find(requested);
		if (module == loader.modules.end())
		{
			return luaL_error(state,
				"module '%s' is not available: it is not declared in Agent behaviour package '%s'",
				requestedText, loader.packageName.c_str());
		}
		auto cycle = std::find(loader.dependencyChain.begin(),
			loader.dependencyChain.end(), requested);
		if (cycle != loader.dependencyChain.end())
		{
			for (auto current = loader.dependencyChain.begin();
				current != loader.dependencyChain.end(); ++current)
			{
				if (!chain.empty()) chain += " -> ";
				chain += *current;
			}
			chain += " -> " + requested;
			return luaL_error(state, "Agent behaviour import cycle: %s", chain.c_str());
		}

		auto const& source = module->second.source;
		chunkName = "@" + loader.packageName + "/"
			+ module->second.sourceModulePath;
		if (luaL_loadbufferx(state, source.data(), source.size(),
			chunkName.c_str(), "t") != LUA_OK) return lua_error(state);
		if (loader.environmentReference == LUA_NOREF)
			return luaL_error(state, "Agent behaviour module environment is unavailable");
		lua_rawgeti(state, LUA_REGISTRYINDEX, loader.environmentReference);
		if (!lua_setupvalue(state, -2, 1))
			return luaL_error(state, "Agent behaviour helper module has no environment");

		loader.dependencyChain.push_back(requested);
		auto const status = lua_pcall(state, 0, 1, 0);
		loader.dependencyChain.pop_back();
		if (status != LUA_OK) return lua_error(state);
		if (lua_isnil(state, -1))
		{
			lua_pop(state, 1);
			lua_pushboolean(state, 1);
		}
		makeTopImmutable(state);
		lua_pushvalue(state, -1);
		loader.loadedModules.emplace(requested,
			luaL_ref(state, LUA_REGISTRYINDEX));
		return 1;
	}

	int requireDeclaredModule(lua_State* state)
	{
		auto& loader = *static_cast<ModuleLoader*>(
			lua_touserdata(state, lua_upvalueindex(1)));
		auto const* requestedText = luaL_checkstring(state, 1);
		int status;
		{
			std::string const requested(requestedText);
			std::string chunkName;
			std::string chain;
			auto load = [&](lua_State* inner)
			{
				return loadDeclaredModule(inner, loader, requested, chunkName, chain);
			};
			status = protectedLuaOperation(state, load);
		}
		if (status != LUA_OK) return lua_error(state);
		return 1;
	}

	int createImmutableProxy(lua_State* state, int apiVersion)
	{
		lua_newtable(state);
		auto const backing = lua_gettop(state);
		if (apiVersion)
		{
			lua_pushinteger(state, apiVersion);
			lua_setfield(state, backing, "api_version");
		}

		(void)lua_newuserdatauv(state, 1, 0);
		auto const proxy = lua_gettop(state);
		lua_newtable(state);
		lua_pushvalue(state, backing);
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, immutableNewIndex);
		lua_setfield(state, -2, "__newindex");
		lua_pushliteral(state, "locked");
		lua_setfield(state, -2, "__metatable");
		lua_setmetatable(state, proxy);
		lua_remove(state, backing);
		return luaL_ref(state, LUA_REGISTRYINDEX);
	}

	void removeGlobal(lua_State* state, char const* name)
	{
		lua_pushnil(state);
		lua_setglobal(state, name);
	}

	void openScratchLibraries(lua_State* state, ModuleLoader& loader)
	{
		// This runs inside a C trampoline: do not create sol2 reference
		// owners whose destructors an allocator longjmp could bypass.
		for (auto const& library : {
			std::pair{ LUA_GNAME, luaopen_base },
			std::pair{ LUA_TABLIBNAME, luaopen_table },
			std::pair{ LUA_STRLIBNAME, luaopen_string },
			std::pair{ LUA_MATHLIBNAME, luaopen_math },
			std::pair{ LUA_UTF8LIBNAME, luaopen_utf8 } })
		{
			luaL_requiref(state, library.first, library.second, 1);
			lua_pop(state, 1);
		}
		installBudgetedProtectedCall(state, "pcall");
		installBudgetedProtectedCall(state, "xpcall");

		// The package library is never opened. Replace its require function
		// with the one reserved host-module loader and remove base functions
		// that could load another chunk or bypass the immutable proxy.
		removeGlobal(state, "package");
		removeGlobal(state, "dofile");
		removeGlobal(state, "loadfile");
		removeGlobal(state, "load");
		removeGlobal(state, "rawset");
		removeGlobal(state, "setmetatable");
		lua_getglobal(state, "string");
		if (lua_istable(state, -1))
		{
			lua_pushnil(state);
			lua_setfield(state, -2, "dump");
		}
		lua_pop(state, 1);
		lua_getglobal(state, "math");
		if (lua_istable(state, -1))
		{
			lua_pushnil(state);
			lua_setfield(state, -2, "random");
			lua_pushnil(state);
			lua_setfield(state, -2, "randomseed");
		}
		lua_pop(state, 1);

		installDeterministicSandbox(state);

		for (auto const& [name, version] : loader.hostModuleVersions)
			loader.hostModules.emplace(name, createImmutableProxy(state, version));
		lua_pushlightuserdata(state, &loader);
		lua_pushcclosure(state, requireDeclaredModule, 1);
		lua_setglobal(state, "require");
		lua_pushcfunction(state, tracebackHandler);
		lua_setglobal(state, "__prometheum_traceback");
	}

	// Sandbox setup is the one phase that must run before any protected call
	// can exist, so it is invoked as a C trampoline inside lua_pcall: a Lua
	// allocation failure during library opening, environment construction, or
	// metatable setup becomes a recoverable LUA_ERRMEM status instead of
	// reaching the (host-installed, throw-based) panic handler. The scratch
	// variant leaves the private environment on top of the stack.
	struct ScratchSetup
	{
		ModuleLoader* loader;
		void (*initializeMetatables)(lua_State*);
	};

	int initializeScratchState(lua_State* state)
	{
		auto* setup = static_cast<ScratchSetup*>(
			lua_touserdata(state, lua_upvalueindex(1)));
		openScratchLibraries(state, *setup->loader);
		if (setup->initializeMetatables)
		{
			setup->initializeMetatables(state);
			return 0;
		}
		pushPrivateEnvironment(state);
		return 1;
	}

	int runScratchSetup(lua_State* state, ModuleLoader* loader,
		void (*initializeMetatables)(lua_State*), std::string& message)
	{
		ScratchSetup setup{ loader, initializeMetatables };
		lua_pushlightuserdata(state, &setup);
		lua_pushcclosure(state, initializeScratchState, 1);
		auto const status = lua_pcall(state, 0, initializeMetatables ? 0 : 1, 0);
		if (status == LUA_OK) return status;
		auto const* text = lua_tostring(state, -1);
		message = text ? text : "Lua sandbox initialization failed";
		lua_pop(state, 1);
		return status;
	}

	size_t diagnosticLine(std::string_view traceback,
		std::string_view chunkName)
	{
		auto marker = std::string(chunkName) + ":";
		auto position = traceback.find(marker);
		if (position == std::string_view::npos) return 1;
		position += marker.size();
		size_t line = 0;
		bool foundDigit = false;
		while (position < traceback.size() && traceback[position] >= '0'
			&& traceback[position] <= '9')
		{
			foundDigit = true;
			line = line * 10 + static_cast<size_t>(traceback[position] - '0');
			++position;
		}
		return foundDigit && line != 0 ? line : 1;
	}

	ScriptExecutionFailure failureKind(ScratchBudget const& budget,
		ScriptExecutionFailure fallback)
	{
		if (budget.instructionLimitExceeded)
			return ScriptExecutionFailure::InstructionBudgetExceeded;
		if (budget.memoryLimitExceeded)
			return ScriptExecutionFailure::MemoryBudgetExceeded;
		return fallback;
	}

	// Lua strings are byte sequences, but a log line is text. A message over
	// the byte cap is truncated without splitting a multi-byte sequence, so
	// the copy is always bounded and the result stays valid UTF-8.
	std::string boundedLogMessage(char const* text, size_t length, size_t byteLimit)
	{
		if (length <= byteLimit) return std::string(text, length);
		constexpr std::string_view marker = "...(truncated)";
		if (byteLimit < marker.size()) return {};
		size_t prefix = byteLimit - marker.size();
		while (prefix > 0
			&& (static_cast<unsigned char>(text[prefix]) & 0xC0u) == 0x80u)
			--prefix;
		std::string message(text, prefix);
		message.append(marker);
		return message;
	}

	void pushImmutableProxy(lua_State* state)
	{
		// The caller leaves a backing table on top. A userdata proxy cannot be
		// altered through raw table operations; reads resolve against the hidden
		// backing table and every assignment reaches __newindex.
		auto const backing = lua_gettop(state);
		(void)lua_newuserdatauv(state, 1, 0);
		auto const proxy = lua_gettop(state);
		lua_newtable(state);
		lua_pushvalue(state, backing);
		lua_setfield(state, -2, "__index");
		lua_pushcfunction(state, immutableNewIndex);
		lua_setfield(state, -2, "__newindex");
		lua_pushliteral(state, "immutable");
		lua_setfield(state, -2, "__metatable");
		lua_setmetatable(state, proxy);
		lua_remove(state, backing);
	}

	ProtectedCallResult protectedCall(lua_State* state, ScratchBudget& budget,
		int argumentCount, int resultCount)
	{
		auto const functionIndex = lua_gettop(state) - argumentCount;
		lua_getglobal(state, "__prometheum_traceback");
		lua_insert(state, functionIndex);
		beginInstructionBudget(state, budget);
		auto const status = lua_pcall(state, argumentCount, resultCount,
			functionIndex);
		endInstructionBudget(state);
		if (status != LUA_OK)
		{
			auto const* message = lua_tostring(state, -1);
			ProtectedCallResult result;
			result.failure = status == LUA_ERRMEM
				? ScriptExecutionFailure::MemoryBudgetExceeded
				: failureKind(budget);
			result.diagnostic = message ? message : "Lua execution failed";
			result.traceback = result.diagnostic;
			lua_pop(state, 1);
			lua_remove(state, functionIndex);
			return result;
		}
		lua_remove(state, functionIndex);
		if (budgetExhausted(budget))
		{
			// A protected call inside the sandbox may have caught the budget
			// error, so a successful host call is not proof the budget held.
			ProtectedCallResult result;
			result.failure = failureKind(budget);
			result.diagnostic = budgetDiagnostic(budget);
			result.traceback = result.diagnostic;
			return result;
		}
		ProtectedCallResult result;
		result.succeeded = true;
		return result;
	}

	void copyGlobal(lua_State* state, int environment, char const* name)
	{
		lua_getglobal(state, name);
		lua_setfield(state, environment, name);
	}

	void copyLibrary(lua_State* state, int environment, char const* name)
	{
		lua_getglobal(state, name);
		if (!lua_istable(state, -1))
		{
			lua_pop(state, 1);
			return;
		}
		lua_newtable(state);
		auto const copy = lua_gettop(state);
		lua_pushnil(state);
		while (lua_next(state, -3) != 0)
		{
			lua_pushvalue(state, -2);
			lua_pushvalue(state, -2);
			lua_settable(state, copy);
			lua_pop(state, 1);
		}
		lua_remove(state, copy - 1);
		makeTopImmutable(state);
		lua_setfield(state, environment, name);
	}

	// Table keys are ordered by a defined total order so that Lua's native
	// per-state string hash seed cannot leak into behaviour execution: booleans
	// (false first), then numbers (ascending), then strings (byte order).
	// Identity-bearing keys have no cross-process stable order, so iteration
	// refuses them rather than silently replaying differently.
	int deterministicKeyRank(int type)
	{
		if (type == LUA_TBOOLEAN) return 0;
		if (type == LUA_TNUMBER) return 1;
		if (type == LUA_TSTRING) return 2;
		return 3;
	}

	// Lua table keys normalize integral floats to integers, so a numeric key is
	// either an exact lua_Integer or a non-integral float. Comparing integer and
	// float keys through double could make two distinct keys look equal above
	// 2^53, which would let the native traversal order decide the successor;
	// compare them exactly instead.
	bool numberIsInteger(lua_State* state, int index, lua_Integer& value)
	{
		if (lua_isinteger(state, index))
		{
			value = lua_tointeger(state, index);
			return true;
		}
		auto const number = lua_tonumber(state, index);
		if (number < -9223372036854775808.0
			|| number >= 9223372036854775808.0)
			return false;
		auto const converted = static_cast<lua_Integer>(number);
		if (static_cast<lua_Number>(converted) != number) return false;
		value = converted;
		return true;
	}

	int compareIntegerAndNumber(lua_Integer integer, lua_Number number)
	{
		if (number >= 9223372036854775808.0) return -1;
		if (number < -9223372036854775808.0) return 1;
		auto const truncated = static_cast<lua_Integer>(number);
		if (integer != truncated) return integer < truncated ? -1 : 1;
		auto const fraction = number - static_cast<lua_Number>(truncated);
		if (fraction > 0) return -1;
		if (fraction < 0) return 1;
		return 0;
	}

	int deterministicKeyCompare(lua_State* state, int leftIndex,
		int rightIndex)
	{
		leftIndex = lua_absindex(state, leftIndex);
		rightIndex = lua_absindex(state, rightIndex);
		auto const leftType = lua_type(state, leftIndex);
		auto const rightType = lua_type(state, rightIndex);
		if (leftType != rightType)
		{
			auto const leftRank = deterministicKeyRank(leftType);
			auto const rightRank = deterministicKeyRank(rightType);
			return leftRank < rightRank ? -1 : (leftRank > rightRank ? 1 : 0);
		}
		if (leftType == LUA_TBOOLEAN)
		{
			auto const left = lua_toboolean(state, leftIndex);
			auto const right = lua_toboolean(state, rightIndex);
			if (left == right) return 0;
			return left ? 1 : -1;
		}
		if (leftType == LUA_TNUMBER)
		{
			lua_Integer leftInteger = 0;
			lua_Integer rightInteger = 0;
			auto const leftIsInteger = numberIsInteger(state, leftIndex,
				leftInteger);
			auto const rightIsInteger = numberIsInteger(state, rightIndex,
				rightInteger);
			if (leftIsInteger && rightIsInteger)
				return leftInteger < rightInteger
					? -1 : (leftInteger > rightInteger ? 1 : 0);
			if (!leftIsInteger && !rightIsInteger)
			{
				auto const left = lua_tonumber(state, leftIndex);
				auto const right = lua_tonumber(state, rightIndex);
				return left < right ? -1 : (left > right ? 1 : 0);
			}
			if (leftIsInteger)
				return compareIntegerAndNumber(leftInteger,
					lua_tonumber(state, rightIndex));
			return -compareIntegerAndNumber(rightInteger,
				lua_tonumber(state, leftIndex));
		}
		if (leftType == LUA_TSTRING)
		{
			size_t leftLength = 0;
			size_t rightLength = 0;
			auto const* left = lua_tolstring(state, leftIndex, &leftLength);
			auto const* right = lua_tolstring(state, rightIndex, &rightLength);
			auto const common = leftLength < rightLength
				? leftLength : rightLength;
			auto const compared = std::memcmp(left, right, common);
			if (compared != 0) return compared < 0 ? -1 : 1;
			return leftLength < rightLength
				? -1 : (leftLength > rightLength ? 1 : 0);
		}
		return 0;
	}

	// next is stateless, so it scans the current keys for the successor of the
	// control key. The scan allocates nothing on the Lua heap, so a large table
	// cannot turn one Lua step into unbounded host allocation.
	int deterministicNext(lua_State* state)
	{
		luaL_checktype(state, 1, LUA_TTABLE);
		auto const argumentCount = lua_gettop(state);
		bool const hasControl = argumentCount >= 2
			&& !lua_isnoneornil(state, 2);

		lua_pushnil(state);
		auto const best = lua_gettop(state);
		bool found = !hasControl;
		char const* unsupportedType = nullptr;
		lua_pushnil(state);
		while (lua_next(state, 1) != 0)
		{
			auto const key = lua_absindex(state, -2);
			auto const type = lua_type(state, key);
			if (type != LUA_TBOOLEAN && type != LUA_TNUMBER
				&& type != LUA_TSTRING)
			{
				unsupportedType = luaL_typename(state, key);
				lua_pop(state, 2);
				break;
			}
			bool candidate = true;
			if (hasControl)
			{
				auto const compared = deterministicKeyCompare(state, key, 2);
				if (compared == 0) found = true;
				candidate = compared > 0;
			}
			if (candidate)
			{
				lua_pushvalue(state, best);
				auto const currentBest = lua_gettop(state);
				if (lua_isnil(state, currentBest)
					|| deterministicKeyCompare(state, key, currentBest) < 0)
				{
					lua_pop(state, 1);
					lua_pushvalue(state, key);
					lua_replace(state, best);
				}
				else
				{
					lua_pop(state, 1);
				}
			}
			lua_pop(state, 1);
		}
		if (unsupportedType)
			return luaL_error(state,
				"Agent behaviour table iteration requires boolean, number, or string keys; got %s",
				unsupportedType);
		if (!found) return luaL_error(state, "invalid key to 'next'");
		lua_pushvalue(state, best);
		if (lua_isnil(state, -1)) return 0;
		lua_pushvalue(state, -1);
		lua_rawget(state, 1);
		return 2;
	}

	int deterministicPairs(lua_State* state)
	{
		luaL_checktype(state, 1, LUA_TTABLE);
		lua_pushcfunction(state, deterministicNext);
		lua_pushvalue(state, 1);
		lua_pushnil(state);
		return 3;
	}

	bool isIdentityBearing(lua_State* state, int index)
	{
		auto const type = lua_type(state, index);
		return type == LUA_TTABLE || type == LUA_TFUNCTION
			|| type == LUA_TUSERDATA || type == LUA_TLIGHTUSERDATA
			|| type == LUA_TTHREAD;
	}

	// Behaviour-visible identity must not be a process address, and a
	// first-observation serial would still depend on replay-wide ordering. A
	// type label is unconditionally stable; values that already define
	// __tostring keep their semantic representation.
	void pushDeterministicString(lua_State* state, int index)
	{
		index = lua_absindex(state, index);
		if (!isIdentityBearing(state, index))
		{
			luaL_tolstring(state, index, nullptr);
			return;
		}
		if (luaL_getmetafield(state, index, "__tostring") != LUA_TNIL)
		{
			lua_pop(state, 1);
			luaL_tolstring(state, index, nullptr);
			return;
		}
		lua_pushstring(state, luaL_typename(state, index));
	}

	int deterministicToString(lua_State* state)
	{
		pushDeterministicString(state, 1);
		return 1;
	}

	bool formatContainsPointerConversion(std::string_view format)
	{
		for (size_t index = 0; index < format.size(); ++index)
		{
			if (format[index] != '%') continue;
			size_t cursor = index + 1;
			while (cursor < format.size()
				&& std::strchr("-+ #0", format[cursor]) != nullptr) ++cursor;
			while (cursor < format.size()
				&& std::isdigit(static_cast<unsigned char>(format[cursor]))) ++cursor;
			if (cursor < format.size() && format[cursor] == '.')
			{
				++cursor;
				while (cursor < format.size()
					&& std::isdigit(static_cast<unsigned char>(format[cursor]))) ++cursor;
			}
			if (cursor < format.size() && format[cursor] == 'p') return true;
			index = cursor;
		}
		return false;
	}

	// string.format reaches identity-bearing values through %s and %p. The %p
	// conversion is refused outright, and %s arguments are routed through the
	// deterministic representation before the native formatter runs.
	int deterministicFormat(lua_State* state)
	{
		auto const argumentCount = lua_gettop(state);
		size_t formatLength = 0;
		auto const* format = luaL_checklstring(state, 1, &formatLength);
		if (formatContainsPointerConversion(
			std::string_view(format, formatLength)))
			return luaL_error(state,
				"string.format does not support the '%%p' conversion in Agent "
				"behaviours because object addresses are not deterministic");
		for (int index = 2; index <= argumentCount; ++index)
		{
			if (!isIdentityBearing(state, index)) continue;
			pushDeterministicString(state, index);
			lua_replace(state, index);
		}
		lua_pushvalue(state, lua_upvalueindex(1));
		lua_insert(state, 1);
		lua_call(state, argumentCount, LUA_MULTRET);
		return lua_gettop(state);
	}

	// Installed on the raw globals so that both the private environment and the
	// string metatable's __index (used by s:format(...)) reach it. The native
	// formatter is captured as an upvalue before the global is replaced.
	void installDeterministicSandbox(lua_State* state)
	{
		lua_pushcfunction(state, deterministicPairs);
		lua_setglobal(state, "pairs");
		lua_pushcfunction(state, deterministicNext);
		lua_setglobal(state, "next");
		lua_pushcfunction(state, deterministicToString);
		lua_setglobal(state, "tostring");
		lua_getglobal(state, "string");
		if (lua_istable(state, -1))
		{
			lua_getfield(state, -1, "format");
			lua_pushcclosure(state, deterministicFormat, 1);
			lua_setfield(state, -2, "format");
		}
		lua_pop(state, 1);
	}

	void pushPrivateEnvironment(lua_State* state)
	{
		lua_newtable(state);
		auto const environment = lua_gettop(state);
		for (auto const* name : { "assert", "error", "ipairs", "next", "pairs",
			"pcall", "rawequal", "rawget", "select", "tonumber", "tostring",
			"type", "xpcall", "_VERSION", "require" })
			copyGlobal(state, environment, name);
		for (auto const* name : { "table", "string", "math", "utf8" })
			copyLibrary(state, environment, name);
		lua_pushvalue(state, environment);
		lua_setfield(state, environment, "_G");
	}

	int marshallPrivateEnvironment(lua_State* state)
	{
		pushPrivateEnvironment(state);
		return 1;
	}

	int registerTrampoline(lua_State* state, lua_CFunction trampoline)
	{
		lua_pushcfunction(state, trampoline);
		return luaL_ref(state, LUA_REGISTRYINDEX);
	}

	bool protectedMarshall(lua_State* state, int trampolineReference,
		void const* payload, int resultCount, ProtectedCallResult& result)
	{
		lua_rawgeti(state, LUA_REGISTRYINDEX, trampolineReference);
		if (payload) lua_pushlightuserdata(state, const_cast<void*>(payload));
		auto const status = lua_pcall(state, payload ? 1 : 0, resultCount, 0);
		if (status == LUA_OK)
		{
			result.succeeded = true;
			return true;
		}
		auto const* message = lua_tostring(state, -1);
		result.failure = (status == LUA_ERRMEM
			|| scratchBudget(state).memoryLimitExceeded)
			? ScriptExecutionFailure::MemoryBudgetExceeded
			: ScriptExecutionFailure::LuaError;
		result.diagnostic = message ? message : "Lua marshalling failed";
		result.traceback = result.diagnostic;
		lua_pop(state, 1);
		return false;
	}

	void ModuleLoader::release(lua_State* state)
	{
		for (auto const& [name, reference] : loadedModules)
		{
			(void)name;
			luaL_unref(state, LUA_REGISTRYINDEX, reference);
		}
		loadedModules.clear();
		dependencyChain.clear();
	}

	void LogStaging::stageLog(LogLevel level, char const* text, size_t length)
	{
		if (logs.size() >= logMessageCountLimit
			|| logStagedBytes >= logStagingByteLimit)
		{
			logsSuppressed = true;
			return;
		}
		auto const remaining = logStagingByteLimit - logStagedBytes;
		auto const byteLimit = std::min(logMessageByteLimit, remaining);
		auto message = boundedLogMessage(text ? text : "", length, byteLimit);
		if (message.empty() && length != 0)
		{
			// Not even the truncation marker fits the remaining window bytes.
			logsSuppressed = true;
			return;
		}
		logStagedBytes += message.size();
		logs.push_back({ level, std::move(message) });
	}
}
