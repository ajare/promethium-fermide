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
		std::string furnitureName, definitionKey, usablePointKey, pose;
		uint64_t furnitureId{};
	};

	enum class ActionEffectType { Pose, Claim, Release, Device };
	struct ActionEffect
	{
		ActionEffectType type;
		// Pose: 0 Standing, 1 Sitting, 2 Lying. Device: typed command kind.
		int value{};
		uint64_t point{};
	};
	struct ActionLog { std::string message; };

	struct ActionExecutionResult
	{
		bool succeeded{ false };
		ScriptExecutionFailure failure{ ScriptExecutionFailure::None };
		std::string diagnostic;
		std::vector<ActionEffect> effects;
		std::vector<ActionLog> logs;
		bool logsSuppressed{ false };
	};

	// Immutable authored metadata/source. Each invocation creates a fresh sandbox;
	// neither closures nor a live VM belong to a World document.
	class ActionRegistry
	{
		std::string mUuid, mSource;
		std::vector<ActionDefinition> mActions;
		friend class FurnitureCatalogue;
		static ActionExecutionResult executeFurniture(std::string const& source, std::string_view key,
			bool finishing, ActionViews const& views);
	public:
		static std::shared_ptr<const ActionRegistry> load(std::filesystem::path const& path);
		std::string const& uuid() const { return mUuid; }
		std::vector<ActionDefinition> const& actions() const { return mActions; }
		std::string identity(ActionDefinition const& action) const { return mUuid + ":" + action.key; }
		ActionDefinition const* find(std::string_view identity) const;
		ActionExecutionResult execute(std::string_view identity, ActionViews const& views) const;
	};
}
