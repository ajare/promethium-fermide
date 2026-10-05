#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "core/ScriptExecution.h"

namespace core
{
	struct ActionDefinition
	{
		std::string key;
		std::string name;
	};

	struct ActionViews
	{
		std::string agentName, worldName, markerName;
		uint64_t agentId{}, markerId{}, tick{};
		float x{}, y{};
	};

	struct ActionExecutionResult
	{
		bool succeeded{ false };
		ScriptExecutionFailure failure{ ScriptExecutionFailure::None };
		std::string diagnostic;
	};

	// Immutable authored metadata/source. Each invocation creates a fresh sandbox;
	// neither closures nor a live VM belong to a World document.
	class ActionRegistry
	{
		std::string mUuid, mSource;
		std::vector<ActionDefinition> mActions;
	public:
		static std::shared_ptr<const ActionRegistry> load(std::filesystem::path const& path);
		std::string const& uuid() const { return mUuid; }
		std::vector<ActionDefinition> const& actions() const { return mActions; }
		std::string identity(ActionDefinition const& action) const { return mUuid + ":" + action.key; }
		ActionDefinition const* find(std::string_view identity) const;
		ActionExecutionResult execute(std::string_view identity, ActionViews const& views) const;
	};
}
