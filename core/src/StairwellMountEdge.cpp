#include <cassert>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/StairwellMountEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	StairwellMountEdge
	------------------

	This Edge connects a StairwellLocationVertex to a StairwellVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Stairwell.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	StairwellMountEdge::StairwellMountEdge(shared_ptr<Stairwell> stairwell)
		: Edge(EdgeType::StairwellMount)
		, mStairwell(stairwell)
	{
	}

	StairwellMountEdge::StairwellMountEdge(uint32_t id, shared_ptr<Stairwell> stairwell)
		: Edge(id, EdgeType::StairwellMount)
		, mStairwell(stairwell)
	{
	}

	shared_ptr<Stairwell> StairwellMountEdge::getStairwell() const
	{
		return mStairwell;
	}

	shared_ptr<Edge> StairwellMountEdge::copyWithoutVertices()
	{
		return make_shared<StairwellMountEdge>(getId(), getStairwell());
	}

	string StairwellMountEdge::getDescription() const
	{
		return format("StairwellMount edge for {}", mStairwell->getDescription());
	}

	bool StairwellMountEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Stairwell)) return false;
		// TODO: see if any Agents are on the Stairwell

		return true;
	}

	EdgeTraversalRequestResult StairwellMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts StairwellMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		facts.feasible = !routeRejectsEdge(context, *this, TraversalKind::Stairwell);
		if (facts.feasible) facts.objectiveDurationSeconds = 0.0f;
		else facts.exclusionReason = RouteExclusionReason::Mobility;
		return facts;
	}

} // core