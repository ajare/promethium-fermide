#include <cassert>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/ShuttleMountEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	ShuttleMountEdge
	----------------

	This Edge connects a regular SectorObjectVertex to a ShuttleVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Shuttle.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	ShuttleMountEdge::ShuttleMountEdge(shared_ptr<Shuttle> Shuttle)
		: Edge(EdgeType::ShuttleMount)
		, mShuttle(Shuttle)
	{
	}

	ShuttleMountEdge::ShuttleMountEdge(uint32_t id, shared_ptr<Shuttle> Shuttle)
		: Edge(id, EdgeType::ShuttleMount)
		, mShuttle(Shuttle)
	{
	}

	shared_ptr<Shuttle> ShuttleMountEdge::getShuttle() const
	{
		return mShuttle;
	}

	shared_ptr<Edge> ShuttleMountEdge::copyWithoutVertices()
	{
		return make_shared<ShuttleMountEdge>(getId(), getShuttle());
	}

	string ShuttleMountEdge::getDescription() const
	{
		return format("ShuttleMount edge for {}", mShuttle->getDescription());
	}

	bool ShuttleMountEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Shuttle)) return false;
		// TODO: see if any Agents are on the Shuttle

		return true;
	}

	EdgeTraversalRequestResult ShuttleMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float ShuttleMountEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool /* edgeVisible */) const
	{
		if (agentForbidsEdge(agent, *this, TraversalKind::Shuttle)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		auto resource = getTraversalResourceId();
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			+ (agent && resource ? agent->estimateTraversalDelay(resource,
				SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 }) : 0.0f);
	}

	DirectedTraversalFacts ShuttleMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		if (routeRejectsEdge(context, *this, TraversalKind::Shuttle)) return {};
		// Topology connector only: admission is charged at the landing Door.
		DirectedTraversalFacts facts;
		facts.feasible = true;
		facts.objectiveDurationSeconds = 0.0f;
		return facts;
	}

	bool ShuttleMountEdge::requiresButton() const
	{
		return true;
	}

	TraversalResourceId ShuttleMountEdge::getTraversalResourceId() const
	{
		return mShuttle ? mShuttle->getTraversalResourceId() : TraversalResourceId{};
	}

} // core