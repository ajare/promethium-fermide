#include <cassert>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/LiftEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	LiftEdge
	--------

	Implementation of Edge for the Vertices at either end of a Lift.
	*/

	using namespace std;

	LiftEdge::LiftEdge(shared_ptr<Lift> lift)
		: Edge(EdgeType::Lift)
		, mLift(lift)
	{
	}

	LiftEdge::LiftEdge(uint32_t id, shared_ptr<Lift> lift)
		: Edge(id, EdgeType::Lift)
		, mLift(lift)
	{
	}

	shared_ptr<Edge> LiftEdge::copyWithoutVertices()
	{
		return make_shared<LiftEdge>(getId(), mLift);
	}

	string LiftEdge::getDescription() const
	{
		return format("Lift edge for {}", mLift->getDescription());
	}

	bool LiftEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift)) return false;
		return true;
	}

	EdgeTraversalRequestResult LiftEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts LiftEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsEdge(context, *this,
			mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		facts.feasible = true;
		auto const distance = getLength();
		facts.components.motionSeconds = distance / mLift->getSpeed();
		// Each body segment represents disjoint physical travel. This allowance can
		// therefore accumulate for a long journey without repeating admission or exit.
		facts.components.expectedWaitSeconds = distance
			* context.policy.liftExpectedIntermediateStopsPerLevel
			* mLift->getRouteMinimumDwellSeconds();
		facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		facts.objectiveDurationSeconds = facts.components.motionSeconds
			+ facts.components.expectedWaitSeconds;
		return facts;
	}

	bool LiftEdge::requiresButton() const
	{
		return true;
	}

	TraversalResourceId LiftEdge::getTraversalResourceId() const
	{
		return mLift->getTraversalResourceId();
	}

} // core