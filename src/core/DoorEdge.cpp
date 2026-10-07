#include <cassert>
#include "ThresholdRouteFacts.h"

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/DoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"
#include "core/World.h"


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
		if ((agent && !mDoor->admitsVerticalExtent(agent->getTraversalDoorClearanceExtent(), agent->getGlobalPosition().y))
			|| agentForbidsEdge(agent.get(), *this, TraversalKind::Door)) return false;
		return mDoor->admitsNewCrossings();
	}

	EdgeTraversalRequestResult DoorEdge::requestTraversal(shared_ptr<const Vertex>,
		shared_ptr<const Agent> agent) const
	{
		if ((agent && !mDoor->admitsVerticalExtent(agent->getTraversalDoorClearanceExtent(), agent->getGlobalPosition().y))
			|| agentForbidsEdge(agent.get(), *this, TraversalKind::Door))
			return EdgeTraversalRequestResult::Failed;
		return (mDoor->admitsNewCrossings() || mDoor->requestOpen())
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts DoorEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		// Runtime keeps an ordinary Door crossing in place for six 1/60-second ticks.
		auto facts = thresholdRouteFacts(*this, *mDoor, target, context, 6.0f / 60.0f,
			CORE_DOOR_OPEN_CLOSE_TIME);
		auto source = getOtherVertex(target);
		if (context.agent && source && !mDoor->admitsVerticalExtent(
			context.agent->getTraversalDoorClearanceExtent(context.beginningMovement), source->getPosition().y))
		{
			facts.feasible = false;
			facts.exclusionReason = RouteExclusionReason::Clearance;
			return facts;
		}
		auto known = mDoor->knownCondition(context.agent, context.observationSector);
		auto const locallyOpen = (source && source->getSector().get() == context.observationSector
			&& mDoor->isOpen()) || (known && known->broken && known->position >= 1.0f);
		if (facts.feasible && mDoor->getActivationMode() == DoorActivationMode::Manual
			&& !locallyOpen && context.world && context.agent
			&& !context.world->canAgentOpenManualDoor(mDoor->getTraversalResourceId(),
				context.world->getAgentId(context.agent)))
		{
			facts.feasible = false;
			facts.exclusionReason = RouteExclusionReason::Permission;
		}
		// An open Door needs no operation, but an adhering Agent still declines
		// permission-based passage when every applicable approach-side control is
		// protected by requirements it does not satisfy.
		if (facts.feasible && locallyOpen && context.world && context.agent && source
			&& !context.world->agentAdheresToDoorPermission(
				mDoor->getTraversalResourceId(),
				SectorId{ static_cast<uint64_t>(source->getSector()->getIndex()) + 1 },
				context.world->getAgentId(context.agent)))
		{
			facts.feasible = false;
			facts.exclusionReason = RouteExclusionReason::Permission;
		}
		return facts;
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