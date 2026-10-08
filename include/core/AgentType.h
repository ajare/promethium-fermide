#pragma once

#include <string>
#include <string_view>

#include "core/Agent.h"

namespace core
{
	// One resolved Agent-type definition: the immutable type identity plus the
	// shared `.agent.lua` source. The source is shared between every Agent of
	// the type; executed mutable instance state is not. The frozen physical
	// baseline is produced per instance by the World-owned runtime, never stored
	// here, so a definition is not a physical authority in itself.
	struct AgentTypeDefinition
	{
		std::string typeId;
		std::string displayName;
		// Application Resource name the source was resolved from. The embedded
		// bundled Human uses the canonical resource name so a scripted Human and
		// a legacy Human resolve to the same identity.
		std::string resourceName;
		std::string source;
	};

	// The bundled Human definition. Its source is embedded so headless World
	// creation and legacy document loading can resolve Human without an editor
	// startup or a filesystem dependency. It is the single authority for the
	// Human baseline in this slice.
	AgentTypeDefinition bundledHumanAgentType();

	// The frozen, validated Human baseline produced by running the bundled
	// definition's new() once. Computed lazily and shared by the compatibility
	// Human class and any caller that needs Human dimensions without a World.
	AgentPhysicalBaseline const& bundledHumanBaseline();

	// True when `typeId` is an acceptable stable type ID: a non-empty, bounded
	// identifier using only letters, digits, underscore and hyphen.
	bool agentTypeIdIsValid(std::string_view typeId);

	// True when `name` is an acceptable type display name (non-empty, bounded,
	// no control characters).
	bool agentTypeDisplayNameIsValid(std::string_view name);
}
