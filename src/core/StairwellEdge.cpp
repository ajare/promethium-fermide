#include <cassert>
#include <cmath>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/StairwellEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	StairwellEdge
	-------------

	Implementation of Edge for the Vertices at either end of a Stairwell.
	*/

	using namespace std;

	StairwellEdge::StairwellEdge(shared_ptr<Stairwell> stairwell)
		: Edge(EdgeType::Stairwell)
		, mStairwell(stairwell)
	{
	}

	StairwellEdge::StairwellEdge(uint32_t id, shared_ptr<Stairwell> stairwell)
		: Edge(id, EdgeType::Stairwell)
		, mStairwell(stairwell)
	{
	}

	shared_ptr<Edge> StairwellEdge::copyWithoutVertices()
	{
		return make_shared<StairwellEdge>(getId(), mStairwell);
	}

	string StairwellEdge::getDescription() const
	{
		return format("Stairwell edge for {}", mStairwell->getDescription());
	}

	bool StairwellEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Stairwell)) return false;
		return true;
	}

	EdgeTraversalRequestResult StairwellEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float StairwellEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool /* edgeVisible */) const
	{
		if (agentForbidsEdge(agent, *this, TraversalKind::Stairwell)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		auto const sourceVertex = getOtherVertex(targetVertex);
		auto const rise = targetVertex->getPosition().y - sourceVertex->getPosition().y;
		auto const ascending = rise > 0.0f;
		auto const policy = RouteChoicePolicy{};
		auto const speed = agent ? agent->getStationaryStairSpeed(ascending)
			: (ascending ? policy.stairAscentSpeed : policy.stairDescentSpeed);
		return getLength() / speed;
	}

	DirectedTraversalFacts StairwellEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (agentForbidsEdge(context.legacyAgent, *this, TraversalKind::Stairwell)) return facts;
		auto const sourceVertex = getOtherVertex(targetVertex);
		auto const rise = targetVertex->getPosition().y - sourceVertex->getPosition().y;
		auto const ascending = rise > 0.0f;
		auto const speed = ascending ? context.policy.stairAscentSpeed : context.policy.stairDescentSpeed;
		facts.feasible = true;
		facts.components.motionSeconds = getLength() / speed;
		facts.components.physicalEffortUnits = abs(rise) * (ascending
			? context.policy.stairAscentEffortPerRise : context.policy.stairDescentEffortPerRise);
		// A complete flight rises one World unit. Pro-rating by rise charges once
		// per flight, not once for each of its three graph segments.
		facts.components.interactionUnits = abs(rise) * context.policy.stairInteractionPerFlight;
		facts.objectiveDurationSeconds = facts.components.motionSeconds;
		facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		return facts;
	}

	float StairwellEdge::getTraversalSpeed(Agent const* agent,
		shared_ptr<const Vertex> const& targetVertex) const
	{
		if (!agent || !targetVertex) return 0.0f;
		auto const ascending = targetVertex->getPosition().y
			> getOtherVertex(targetVertex)->getPosition().y;
		return agent->getStationaryStairSpeed(ascending);
	}

	TraversalResourceId StairwellEdge::getTraversalResourceId() const
	{
		return mStairwell->getTraversalResourceId();
	}

} // core