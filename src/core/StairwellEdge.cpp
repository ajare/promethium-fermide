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

	DirectedTraversalFacts StairwellEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsEdge(context, *this, TraversalKind::Stairwell))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto const rise = getDirectedRise(*targetVertex);
		auto const ascending = rise > 0.0f;
		// A route decision snapshots its modifier profile; the unmodified
		// directional baseline belongs to the concrete Agent type.
		auto const typeSpeed = context.agent
			? (ascending ? context.agent->getPhysicalBaseline().stairAscentSpeed
				: context.agent->getPhysicalBaseline().stairDescentSpeed)
			// Agent-less editor previews retain the Human-compatible baseline.
			: (ascending ? 0.35f : 0.45f);
		auto const speed = typeSpeed * context.profile.stairSpeedModifier;
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