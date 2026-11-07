#pragma once

#include "core/Platform.h"
#include "core/AgentTag.h"

namespace core
{
	class Agent;
	class Edge;
	class Serializer;
	struct RouteDecisionContext;

	PF_CORE_API [[nodiscard]] bool routeRejectsEdge(RouteDecisionContext const& context,
		Edge const& edge, TraversalKind kind);
	PF_CORE_API [[nodiscard]] bool routeRejectsButtons(RouteDecisionContext const& context);

	PF_CORE_API void serializeMobilityProfile(Serializer& serializer, MobilityProfile const& profile);
	PF_CORE_API [[nodiscard]] MobilityProfile deserializeMobilityProfile(Serializer& serializer);

	PF_CORE_API [[nodiscard]] MobilityUse agentTraversalUse(Agent const* agent, TraversalKind kind);
	PF_CORE_API [[nodiscard]] MobilityUse agentEdgeUse(Agent const* agent, Edge const& edge,
		TraversalKind kind);
	PF_CORE_API [[nodiscard]] bool agentForbidsTraversal(Agent const* agent, TraversalKind kind);
	PF_CORE_API [[nodiscard]] bool agentForbidsButtons(Agent const* agent);
	PF_CORE_API [[nodiscard]] bool agentForbidsEdge(Agent const* agent, Edge const& edge,
		TraversalKind kind);
	PF_CORE_API [[nodiscard]] bool agentRejectsTraversal(Agent const* agent, TraversalKind kind,
		bool allowFallback);
	PF_CORE_API [[nodiscard]] bool agentRejectsButtons(Agent const* agent, bool allowFallback);
	PF_CORE_API [[nodiscard]] bool agentRejectsEdge(Agent const* agent, Edge const& edge,
		TraversalKind kind, bool allowFallback);
}
