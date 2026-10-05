#pragma once

// Private execution infrastructure, shared by script adapters, not a domain API.
// State ownership stays with the World adapter. Operations borrow host payloads
// across protected Lua boundaries; workflow contracts and scheduling stay outside.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <lua.hpp>

#include "core/Log.h"
#include "core/ScriptExecution.h"

namespace core::script
{
	struct ScratchBudget
	{
		size_t bytesUsed{ 0 };
		size_t byteLimit;
		uint32_t instructionLimit;
		uint32_t instructionsRemaining;
		uint32_t hookInterval{ 1'000 };
		bool memoryLimitExceeded{ false };
		bool instructionLimitExceeded{ false };

		explicit ScratchBudget(size_t memoryBytes, uint32_t instructionsPerCall)
			: byteLimit(memoryBytes)
			, instructionLimit(instructionsPerCall)
			, instructionsRemaining(instructionsPerCall)
		{
		}
	};

	struct StateCloser
	{
		void operator()(lua_State* state) const noexcept
		{
			if (state) lua_close(state);
		}
	};

	// Thrown by the panic handler when an unprotected Lua allocation fails.
	// Lua's default panic aborts the process; a host-installed panic must not
	// return, so it throws and unwinds to the nearest C++ handler instead.
	// A panic resets the Lua thread but does not leave it safe to reuse, so
	// the live runtime treats this exception as fatal to its Lua state.
	struct LuaPanicError : std::runtime_error
	{
		using std::runtime_error::runtime_error;
	};

	struct ModuleLoader
	{
		std::string packageName;
		// Only adapter-declared, versioned capabilities are admitted. Setup
		// creates immutable proxies; instance loaders borrow their references.
		std::map<std::string, int> hostModuleVersions;
		std::map<std::string, int> hostModules;
		int environmentReference{ LUA_NOREF };
		std::map<std::string, ScriptModuleSource> modules;
		std::map<std::string, int> loadedModules;
		std::vector<std::string> dependencyChain;

		void release(lua_State* state);
	};

	// Keep owning C++ temporaries in the caller, outside Lua's longjmp
	// boundary. The operation must only borrow them: even allocation APIs
	// such as lua_pushlstring can raise on memory-budget exhaustion.
	template<typename Operation>
	int protectedLuaOperation(lua_State* state, Operation& operation,
		int argumentCount = 0, int resultCount = 1)
	{
		static_assert(std::is_trivially_destructible_v<Operation>);
		auto const functionIndex = lua_gettop(state) - argumentCount + 1;
		lua_pushcfunction(state, [](lua_State* inner) -> int
		{
			auto* borrowed = static_cast<Operation*>(lua_touserdata(inner, 1));
			lua_remove(inner, 1);
			return (*borrowed)(inner);
		});
		lua_insert(state, functionIndex);
		lua_pushlightuserdata(state, &operation);
		lua_insert(state, functionIndex + 1);
		return lua_pcall(state, argumentCount + 1, resultCount, 0);
	}

	struct PendingLogMessage
	{
		LogLevel level{ LogLevel::Info };
		std::string message;
	};

	// Invocation-local staging. Adapters publish only after successful execution
	// and discard on failure; admission happens before copying any host string.
	struct LogStaging
	{
		std::vector<PendingLogMessage> logs;
		uint32_t logMessageCountLimit{ 0 };
		size_t logMessageByteLimit{ 0 };
		size_t logStagingByteLimit{ 0 };
		size_t logStagedBytes{ 0 };
		bool logsSuppressed{ false };

		void stageLog(LogLevel level, char const* text, size_t length);
	};

	struct ProtectedCallResult
	{
		bool succeeded{ false };
		ScriptExecutionFailure failure{ ScriptExecutionFailure::None };
		std::string diagnostic;
		std::string traceback;
	};

	int luaPanic(lua_State* state);
	void* budgetedAllocate(void* userData, void* pointer, size_t oldSize,
		size_t newSize) noexcept;
	void beginInstructionBudget(lua_State* state, ScratchBudget& budget);
	void endInstructionBudget(lua_State* state);
	ScratchBudget& scratchBudget(lua_State* state);
	bool budgetExhausted(ScratchBudget const& budget);
	char const* budgetDiagnostic(ScratchBudget const& budget);
	ScriptExecutionFailure failureKind(ScratchBudget const& budget,
		ScriptExecutionFailure fallback = ScriptExecutionFailure::LuaError);
	void pushBorrowedDiagnostic(lua_State* state, std::string const& diagnostic);
	int immutableNewIndex(lua_State* state);
	void pushImmutableProxy(lua_State* state);
	int createImmutableProxy(lua_State* state, int apiVersion);
	int requireDeclaredModule(lua_State* state);
	void pushPrivateEnvironment(lua_State* state);
	void installDeterministicSandbox(lua_State* state);
	int runScratchSetup(lua_State* state, ModuleLoader* loader,
		void (*initializeMetatables)(lua_State*), std::string& message);
	size_t diagnosticLine(std::string_view traceback, std::string_view chunkName);
	std::string boundedLogMessage(char const* text, size_t length, size_t byteLimit);
	ProtectedCallResult protectedCall(lua_State* state, ScratchBudget& budget,
		int argumentCount, int resultCount);
	int marshallPrivateEnvironment(lua_State* state);
	int registerTrampoline(lua_State* state, lua_CFunction trampoline);
	bool protectedMarshall(lua_State* state, int trampolineReference,
		void const* payload, int resultCount, ProtectedCallResult& result);
}
