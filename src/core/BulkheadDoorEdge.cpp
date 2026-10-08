#include <cassert>
#include "ThresholdRouteFacts.h"

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/BulkheadDoorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/AirlockTransit.h"
#include "core/ChamberTransit.h"
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

	BulkheadDoorEdge::BulkheadDoorEdge(shared_ptr<BulkheadDoor> door, shared_ptr<ChamberTransit> scanner)
		: Edge(EdgeType::BulkheadDoor), mDoor(door), mSecurityScanner(std::move(scanner)) {}

	BulkheadDoorEdge::BulkheadDoorEdge(uint32_t id, shared_ptr<BulkheadDoor> door)
		: Edge(id, EdgeType::BulkheadDoor)
		, mDoor(door)
	{
	}

	shared_ptr<Edge> BulkheadDoorEdge::copyWithoutVertices()
	{
		auto copy = make_shared<BulkheadDoorEdge>(getId(), mDoor);
		copy->mAirlock = mAirlock;
		copy->mSecurityScanner = mSecurityScanner;
		return copy;
	}

	string BulkheadDoorEdge::getDescription() const
	{
		return format("BulkheadDoor edge for {}", mDoor->getDescription());
	}

	bool BulkheadDoorEdge::isTraversable(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door)) return false;
		// Scanner thresholds require a coordinator permit, never opportunistic use.
		if (mDoor->isSecurityScannerOwned()) return false;
		if (agent && !mDoor->admitsAgentTraversal(*agent,
			getOtherVertex(targetVertex)->getPosition().y)) return false;
		if (mDoor->isIndependentlyBroken())
			return agent ? mDoor->admitsBrokenPassage(*agent,
				getOtherVertex(targetVertex)->getPosition().y) : mDoor->admitsNewCrossings();
		return mDoor->isOpen();
	}

	EdgeTraversalRequestResult BulkheadDoorEdge::requestTraversal(shared_ptr<const Vertex> target,
		shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Door))
			return EdgeTraversalRequestResult::Failed;
		if (agent && !mDoor->admitsAgentTraversal(*agent, getOtherVertex(target)->getPosition().y))
			return EdgeTraversalRequestResult::Failed;
		if (mDoor->isIndependentlyBroken())
			return (agent && mDoor->admitsBrokenPassage(*agent, getOtherVertex(target)->getPosition().y))
				? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
		return mDoor->open() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts BulkheadDoorEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		auto const distance = getLength();
		if (mAirlock || mSecurityScanner) return RouteTraversalInputs::capture(*this, target, context).evaluate(context);
		if (mDoor->isSecurityScannerOwned())
		{
			DirectedTraversalFacts facts;
			facts.exclusionReason = RouteExclusionReason::Control;
			return facts;
		}
		auto mode = Door::DoorCrossingMode::Standing;
		if (context.agent)
		{
			mode = mDoor->classifyAgentCrossing(*context.agent,
				getOtherVertex(target)->getPosition().y, context.beginningMovement);
			if (mode == Door::DoorCrossingMode::None)
			{
				DirectedTraversalFacts facts;
				facts.exclusionReason = RouteExclusionReason::Clearance;
				return facts;
			}
		}
		// A Crawling crossing runs at the Agent type's crawling speed ratio, so
		// its duration divides the ordinary walking duration by that ratio.
		return thresholdRouteFacts(*this, *mDoor, target, context,
			(distance == 0.0f ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : distance / context.walkSpeed)
				/ (context.agent && mode == Door::DoorCrossingMode::Crawling
					? context.agent->getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, Pose::Crawling).value() : 1.0f),
			CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME);
	}

	bool BulkheadDoorEdge::requiresButton() const
	{
		return mAirlock || mDoor->getActivationMode() == DoorActivationMode::RemoteControlled;
	}

	TraversalResourceId BulkheadDoorEdge::getTraversalResourceId() const
	{
		if (mSecurityScanner) return mSecurityScanner->getTraversalResourceId();
		return mAirlock ? mAirlock->getTraversalResourceId() : mDoor->getTraversalResourceId();
	}

} // core