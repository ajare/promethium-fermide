#include <cassert>
#include <cmath>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/LiftMountEdge.h"
#include "core/Agent.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LiftMountEdge
	-------------

	This Edge connects a regular SectorObjectVertex to a LiftVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Lift.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	LiftMountEdge::LiftMountEdge(shared_ptr<Lift> lift)
		: Edge(EdgeType::LiftMount)
		, mLift(lift)
	{
	}

	LiftMountEdge::LiftMountEdge(uint32_t id, shared_ptr<Lift> lift)
		: Edge(id, EdgeType::LiftMount)
		, mLift(lift)
	{
	}

	shared_ptr<Lift> LiftMountEdge::getLift() const
	{
		return mLift;
	}

	shared_ptr<Edge> LiftMountEdge::copyWithoutVertices()
	{
		return make_shared<LiftMountEdge>(getId(), getLift());
	}

	string LiftMountEdge::getDescription() const
	{
		return format("LiftMount edge for {}", mLift->getDescription());
	}

	bool LiftMountEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift)) return false;
		// TODO: see if any Agents are on the Lift

		return true;
	}

	EdgeTraversalRequestResult LiftMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts LiftMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsEdge(context, *this,
			mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		facts.feasible = true;
		// The authored PlatformLift object is the Location-side Vertex. Entering
		// therefore targets the topology-only Lift Vertex; leaving targets the object.
		bool const boarding = target && !target->getObject();
		auto& c = facts.components;
		if (boarding)
		{
			auto const capacity = max(1u, mLift->getRouteCapacity());
			bool const observed = context.agent && context.observationSector
				&& getOtherVertex(target)->getSector().get() == context.observationSector
				&& abs(context.agent->getGlobalPosition().y
					- getOtherVertex(target)->getPosition().y) <= 0.5f;
			auto observation = observed ? context.agent->observeLiftAccess(
				getTraversalResourceId(), getOtherVertex(target)->getPosition()) : nullopt;
			if (observation)
			{
				c.expectedWaitSeconds = context.policy.liftExpectedWaitSeconds;
				c.knownWaitSeconds = context.policy.liftQueueServiceSeconds
					* (float)observation->queuedAgents / max(1u, observation->capacity);
				c.crowdingUnits = (float)observation->queuedAgents
					/ max(1u, observation->capacity);
			}
			else
			{
				c.expectedWaitSeconds = context.policy.liftExpectedWaitSeconds
					+ context.policy.liftQueueServiceSeconds
					* context.policy.liftExpectedQueuePassengers / capacity;
				c.crowdingUnits = context.policy.liftExpectedCrowdingUnits;
			}
			c.expectedWaitSeconds += mLift->getRouteMinimumDwellSeconds()
				+ context.policy.platformLiftPreparationSeconds;
			c.motionSeconds = context.policy.liftBoardingSeconds;
			c.interactionUnits = context.policy.liftCallBoardingInteraction
				+ context.policy.platformLiftInconvenience;
		}
		else
		{
			c.motionSeconds = context.policy.liftAlightingSeconds;
			c.interactionUnits = context.policy.liftAlightingInteraction;
		}
		facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds
			+ c.expectedWaitSeconds;
		return facts;
	}

	bool LiftMountEdge::requiresButton() const
	{
		return true;
	}

	TraversalResourceId LiftMountEdge::getTraversalResourceId() const
	{
		return mLift->getTraversalResourceId();
	}

} // core