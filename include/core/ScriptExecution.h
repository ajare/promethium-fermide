#pragma once

#include <string>

namespace core
{
	// Value-only script boundary records. No VM or binding-library types enter
	// domain interfaces; each workflow supplies its own contracts and policy.
	enum class ScriptExecutionFailure
	{
		None,
		LuaError,
		MemoryBudgetExceeded,
		InstructionBudgetExceeded,
		ConversionError
	};

	// Logical imports are declared source records, never filesystem paths.
	struct ScriptModuleSource
	{
		std::string name;
		std::string sourceModulePath;
		std::string source;
	};
}
