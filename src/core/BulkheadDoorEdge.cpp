#include <cassert>
#include "ThresholdRouteFacts.h"

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/BulkheadDoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/AirlockTransit.h"
#include "core/RouteTraversalInputs.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	BulkheadDoorEdge
	----------------

	Implementation of Edge for the Vertices on either side of a BulkheadDoor.
	*/

	BulkheadDoorEdge::BulkheadDoorEdge(shared_ptr<BulkheadDoor> door, shared_ptr<AirlockTransit> airlock)
		: Edge(EdgeType::BulkheadDoor)
		, mDoor(door), mAirlock(std::move(airlock))
	{
	}

	BulkheadDoorEdge::BulkheadDoorEdge(uint32_t id, shared_ptr<BulkheadDoor> door)
		: Edge(id, EdgeType::BulkheadDoor)
		, mDoor(door)
	{
	}

	shared_ptr<Edge> BulkheadDoorEdge::copyWithoutVertices()
	{
		auto copy = make_shared<BulkheadDoorEdge>(getId(), mDoor);
		copy->mAirlock = mAirlock;
		return copy;
	}

	string BulkheadDoorEdge::getDescription() const
	{
		return format("BulkheadDoor edge for {}", mDoor->getDescription());
	}

	bool BulkheadDoorEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door)) return false;
		return mDoor->isOpen();
	}

	EdgeTraversalRequestResult BulkheadDoorEdge::requestTraversal(shared_ptr<const Vertex>,
		shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door))
			return EdgeTraversalRequestResult::Failed;
		return mDoor->open() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts BulkheadDoorEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		auto const distance = getLength();
		if (mAirlock) return RouteTraversalInputs::capture(*this, target, context).evaluate(context);
		return thresholdRouteFacts(*this, *mDoor, target, context,
			distance == 0.0f ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : distance / context.walkSpeed,
			CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME);
	}

	bool BulkheadDoorEdge::requiresButton() const
	{
		return mAirlock || mDoor->getActivationMode() == DoorActivationMode::RemoteControlled;
	}

	TraversalResourceId BulkheadDoorEdge::getTraversalResourceId() const
	{
		return mAirlock ? mAirlock->getTraversalResourceId() : mDoor->getTraversalResourceId();
	}

} // core