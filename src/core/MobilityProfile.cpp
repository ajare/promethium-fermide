#include "core/MobilityProfile.h"

#include "core/Agent.h"
#include "core/Edge.h"

namespace core
{
	bool agentForbidsTraversal(Agent const* agent, TraversalKind kind)
	{
		return agent && (agent->getEffectiveMobilityProfile().forbiddenTraversals
			& traversalMask(kind)) != 0;
	}

	bool agentForbidsButtons(Agent const* agent)
	{
		return agentForbidsTraversal(agent, TraversalKind::Buttons);
	}

	bool agentForbidsEdge(Agent const* agent, Edge const& edge, TraversalKind kind)
	{
		return agentForbidsTraversal(agent, kind)
			|| (agentForbidsButtons(agent) && edge.requiresButton());
	}
}
