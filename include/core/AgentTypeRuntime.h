#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/Agent.h"
#include "core/ScriptExecution.h"

namespace core
{
	// Application-owned limits for Agent-type construction. Type scripts cannot
	// inspect or alter them. One live allocator budget belongs to one World.
	struct AgentTypeRuntimeLimits
	{
		size_t memoryBytes{ 64u * 1024u * 1024u };
		uint32_t instructionsPerCall{ 100'000u };
	};

	// Validation-only result of loading one `.agent.lua` source in an isolated
	// scratch state. It never mutates the live runtime or constructs an Agent.
	struct AgentTypePreflight
	{
		bool loaded{ false };
		std::string typeId;
		std::string displayName;
		ScriptExecutionFailure failure{ ScriptExecutionFailure::None };
		std::string diagnostic;
	};

	// Ordinary C++ result of constructing one live Agent-type instance. The
	// opaque `instance` handle is owned by the returned shared_ptr; releasing it
	// destroys the Lua instance and its private state. The baseline is a
	// validated, frozen copy that later Lua mutation cannot change.
	struct AgentTypeConstructResult
	{
		bool succeeded{ false };
		AgentPhysicalBaseline baseline{};
		std::shared_ptr<void> instance;
		ScriptExecutionFailure failure{ ScriptExecutionFailure::None };
		std::string diagnostic;
	};

	// One live adapter is owned by each World. Its implementation owns that
	// World's Lua state; Lua and sol2 remain confined to private implementation
	// files. Source is shared between instances, executed mutable instance state
	// is not: every construct runs new() in a fresh private environment.
	class AgentTypeRuntimeAdapter
	{
		friend class World;

		struct Impl;
		std::unique_ptr<Impl> mImpl;

	public:
		static constexpr size_t DefaultMemoryBudgetBytes{ 64u * 1024u * 1024u };
		static constexpr uint32_t DefaultInstructionBudget{ 100'000u };

		explicit AgentTypeRuntimeAdapter(AgentTypeRuntimeLimits limits = {});
		~AgentTypeRuntimeAdapter();
		AgentTypeRuntimeAdapter(AgentTypeRuntimeAdapter const&) = delete;
		AgentTypeRuntimeAdapter& operator=(AgentTypeRuntimeAdapter const&) = delete;

		// Validates a source without touching the live state: executes it in an
		// isolated scratch sandbox and extracts the stable type ID, display name,
		// and new() constructor contract.
		static AgentTypePreflight preflightType(std::string resourceName,
			std::string_view source);

		// Constructs a live instance for the given (already preflighted) source.
		// Executes new() in a fresh private environment, validates and freezes the
		// eleven baseline fields, and returns the baseline plus an opaque handle
		// to the retained live instance.
		AgentTypeConstructResult construct(std::string_view typeId,
			std::string_view source, std::string_view name);

		AgentTypeRuntimeLimits getLimits() const;
	};
}
