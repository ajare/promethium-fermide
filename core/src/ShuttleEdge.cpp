#include <cassert>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/ShuttleEdge.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	ShuttleEdge
	-----------

	Implementation of Edge for the Vertices at either end of a Shuttle.
	*/

	using namespace std;

	ShuttleEdge::ShuttleEdge(shared_ptr<Shuttle> shuttle)
		: Edge(EdgeType::Shuttle)
		, mShuttle(shuttle)
	{
	}

	ShuttleEdge::ShuttleEdge(uint32_t id, shared_ptr<Shuttle> shuttle)
		: Edge(id, EdgeType::Shuttle)
		, mShuttle(shuttle)
	{
	}

	shared_ptr<Edge> ShuttleEdge::copyWithoutVertices()
	{
		return make_shared<ShuttleEdge>(getId(), mShuttle);
	}

	string ShuttleEdge::getDescription() const
	{
		return format("Shuttle edge for {}", mShuttle->getDescription());
	}

	bool ShuttleEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Shuttle)) return false;
		return true;
	}

	EdgeTraversalRequestResult ShuttleEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts ShuttleEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		if (routeRejectsEdge(context, *this, TraversalKind::Shuttle))
		{
			DirectedTraversalFacts facts;
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto known = context.world ? context.world->knownTransportCondition(
			getTraversalResourceId(), context.agent, context.observationSector) : std::nullopt;
		if (known && known->broken)
		{
			DirectedTraversalFacts facts;
			facts.exclusionReason = RouteExclusionReason::Control;
			return facts;
		}
		auto source = getOtherVertex(target);
		auto observe = [&](Vector2 const& endpoint)
		{
			return context.world ? context.world->observeShuttleAccess(getTraversalResourceId(), endpoint, false)
				: context.agent ? context.agent->observeShuttleAccess(getTraversalResourceId(), endpoint, false)
				: std::nullopt;
		};
		auto from = observe(source->getPosition());
		auto to = observe(target->getPosition());
		DirectedTraversalFacts facts;
		facts.feasible = true;
		auto& c = facts.components;
		if (from && to)
		{
			auto distance = std::abs(to->stopPosition - from->stopPosition);
			c.motionSeconds = distance > 0 ? distance / mShuttle->getSpeed() : getLength() / context.walkSpeed;
			// Departure dwell on each leg includes intermediate stop service, without
			// repeating the initial headway, queue, or boarding interaction.
			if (distance > 0) c.expectedWaitSeconds = from->minimumDwellSeconds;
		}
		else c.motionSeconds = getLength() / mShuttle->getSpeed();
		facts.optimisticLowerBoundSeconds = c.motionSeconds;
		facts.objectiveDurationSeconds = c.motionSeconds + c.expectedWaitSeconds;
		return facts;
	}

	bool ShuttleEdge::requiresButton() const
	{
		return true;
	}

	TraversalResourceId ShuttleEdge::getTraversalResourceId() const
	{
		return mShuttle ? mShuttle->getTraversalResourceId() : TraversalResourceId{};
	}

} // core