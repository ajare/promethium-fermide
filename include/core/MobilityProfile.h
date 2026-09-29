#pragma once

#include "core/AgentTag.h"

namespace core
{
	class Agent;
	class Edge;
	class Serializer;

	void serializeMobilityProfile(Serializer& serializer, MobilityProfile const& profile);
	[[nodiscard]] MobilityProfile deserializeMobilityProfile(Serializer& serializer);

	[[nodiscard]] MobilityUse agentTraversalUse(Agent const* agent, TraversalKind kind);
	[[nodiscard]] MobilityUse agentEdgeUse(Agent const* agent, Edge const& edge,
		TraversalKind kind);
	[[nodiscard]] bool agentForbidsTraversal(Agent const* agent, TraversalKind kind);
	[[nodiscard]] bool agentForbidsButtons(Agent const* agent);
	[[nodiscard]] bool agentForbidsEdge(Agent const* agent, Edge const& edge,
		TraversalKind kind);
	[[nodiscard]] bool agentRejectsTraversal(Agent const* agent, TraversalKind kind,
		bool allowFallback);
	[[nodiscard]] bool agentRejectsButtons(Agent const* agent, bool allowFallback);
	[[nodiscard]] bool agentRejectsEdge(Agent const* agent, Edge const& edge,
		TraversalKind kind, bool allowFallback);
}
