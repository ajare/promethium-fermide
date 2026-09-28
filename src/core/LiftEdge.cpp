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

	float LiftEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool edgeVisible) const
	{
		if (agentForbidsEdge(agent, *this, mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		(void)edgeVisible;
		auto rideTime = getLength() / mLift->getSpeed();
		auto ride = rideTime > CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			? rideTime : CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
		if (!agent) return ride;
		auto targetSector = targetVertex && targetVertex->getSector()
			? SectorId{ (uint64_t)targetVertex->getSector()->getIndex() + 1 } : SectorId{};
		auto sourceSector = getVertex(0) && SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 } != targetSector
			? SectorId{ (uint64_t)getVertex(0)->getSector()->getIndex() + 1 }
			: getVertex(1) ? SectorId{ (uint64_t)getVertex(1)->getSector()->getIndex() + 1 } : SectorId{};
		return ride + agent->estimateTraversalDelay(getTraversalResourceId(), sourceSector);
	}

	DirectedTraversalFacts LiftEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		if (agentForbidsEdge(context.legacyAgent, *this,
			mLift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift)) return {};
		DirectedTraversalFacts facts;
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