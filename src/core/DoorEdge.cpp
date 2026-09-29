#include <cassert>
#include "ThresholdRouteFacts.h"

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/DoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	/*
	DoorEdge
	--------

	Implementation of Edge for the Vertices on either side of a Door.  These Vertices
	will be on different Layers - Fore and Back respectively.
	*/

	using namespace std;

	DoorEdge::DoorEdge(shared_ptr<Door> door)
		: Edge(EdgeType::Door)
		, mDoor(door)
	{
	}

	DoorEdge::DoorEdge(uint32_t id, shared_ptr<Door> door)
		: Edge(id, EdgeType::Door)
		, mDoor(door)
	{
	}

	shared_ptr<Edge> DoorEdge::copyWithoutVertices()
	{
		return make_shared<DoorEdge>(getId(), mDoor);
	}

	string DoorEdge::getDescription() const
	{
		return format("Door edge for {}", mDoor->getDescription());
	}

	bool DoorEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door)) return false;
		return mDoor->isOpen();
	}

	EdgeTraversalRequestResult DoorEdge::requestTraversal(shared_ptr<const Vertex>,
		shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door))
			return EdgeTraversalRequestResult::Failed;
		return mDoor->open() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts DoorEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		// Runtime keeps an ordinary Door crossing in place for six 1/60-second ticks.
		return thresholdRouteFacts(*this, *mDoor, target, context, 6.0f / 60.0f,
			CORE_DOOR_OPEN_CLOSE_TIME);
	}

	bool DoorEdge::requiresButton() const
	{
		return mDoor->getActivationMode() == DoorActivationMode::RemoteControlled;
	}

	TraversalResourceId DoorEdge::getTraversalResourceId() const
	{
		return mDoor->getTraversalResourceId();
	}

} // core