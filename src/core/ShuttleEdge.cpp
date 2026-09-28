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

	float ShuttleEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool /* edgeVisible */) const
	{
		if (agentForbidsEdge(agent, *this, TraversalKind::Shuttle)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		auto resource = getTraversalResourceId();
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			+ (agent && resource ? agent->estimateTraversalDelay(resource,
				SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 }) : 0.0f);
	}

	DirectedTraversalFacts ShuttleEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		if (agentForbidsEdge(context.legacyAgent, *this, TraversalKind::Shuttle)) return {};
		auto source = getOtherVertex(target);
		auto observe = [&](Vector2 const& endpoint)
		{
			return context.world ? context.world->observeShuttleAccess(getTraversalResourceId(), endpoint, false)
				: context.legacyAgent ? context.legacyAgent->observeShuttleAccess(getTraversalResourceId(), endpoint, false)
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