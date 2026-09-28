#include <cassert>
#include "ThresholdRouteFacts.h"

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/BulkheadDoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	BulkheadDoorEdge
	----------------

	Implementation of Edge for the Vertices on either side of a BulkheadDoor.
	*/

	BulkheadDoorEdge::BulkheadDoorEdge(shared_ptr<BulkheadDoor> door)
		: Edge(EdgeType::BulkheadDoor)
		, mDoor(door)
	{
	}

	BulkheadDoorEdge::BulkheadDoorEdge(uint32_t id, shared_ptr<BulkheadDoor> door)
		: Edge(id, EdgeType::BulkheadDoor)
		, mDoor(door)
	{
	}

	shared_ptr<Edge> BulkheadDoorEdge::copyWithoutVertices()
	{
		return make_shared<BulkheadDoorEdge>(getId(), mDoor);
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
		return thresholdRouteFacts(*this, *mDoor, target, context,
			distance == 0.0f ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : distance / context.walkSpeed,
			CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME);
	}

	float BulkheadDoorEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		if (agentForbidsEdge(agent, *this, TraversalKind::Door)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		// Weight is time to cross the Edge, plus possibly the time waiting for the Door to open.
		auto distance = getLength();
		float traverseTime = !agent || distance == 0.0f ? 0.0f : distance / agent->getWalkSpeed();

		if (!edgeVisible || !mDoor->isOpen())
		{
			traverseTime += mDoor->getOpenCloseTime();
		}
		if (agent)
		{
			auto targetSector = targetVertex && targetVertex->getSector()
				? SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 } : SectorId{};
			auto sourceSector = getVertex(0) && SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 } != targetSector
				? SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 }
				: getVertex(1) ? SectorId{ (uint64_t)getVertex(1)->getSector()->getIndex() + 1 } : SectorId{};
			traverseTime += agent->estimateTraversalDelay(getTraversalResourceId(), sourceSector);
		}
		return max(traverseTime, CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME);
	}

	bool BulkheadDoorEdge::requiresButton() const
	{
		return mDoor->getActivationMode() == DoorActivationMode::RemoteControlled;
	}

	TraversalResourceId BulkheadDoorEdge::getTraversalResourceId() const
	{
		return mDoor->getTraversalResourceId();
	}

} // core