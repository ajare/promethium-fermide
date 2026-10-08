#pragma once

#include "core/Agent.h"

namespace core
{
	class AgentTypeDefinition;

	// Compatibility adapter for callers that still name a concrete Human type.
	// It routes to the bundled Human Agent-type definition (the single physical
	// authority) rather than carrying its own compiled baseline, so ordinary
	// placement and route queries observe the same values as script-backed
	// Humans. Owned World Agents are constructed through the World's Agent type
	// runtime instead; this class only serves temporary, non-owned callers.
	class Human final : public Agent
	{
	public:
		explicit Human(std::string const& name);
	};
}
