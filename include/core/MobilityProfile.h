#pragma once

#include "core/AgentTag.h"

namespace core
{
	class Agent;
	class Edge;

	[[nodiscard]] bool agentForbidsTraversal(Agent const* agent, TraversalKind kind);
	[[nodiscard]] bool agentForbidsButtons(Agent const* agent);
	[[nodiscard]] bool agentForbidsEdge(Agent const* agent, Edge const& edge,
		TraversalKind kind);
}
