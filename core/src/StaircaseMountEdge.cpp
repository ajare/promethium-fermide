#include <format>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/StaircaseMountEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Sector.h"

namespace core
{
	using namespace std;
	StaircaseMountEdge::StaircaseMountEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	StaircaseMountEdge::StaircaseMountEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	string StaircaseMountEdge::getDescription() const { return format("Staircase mount edge for {}", mStaircase->getDescription()); }
	shared_ptr<Edge> StaircaseMountEdge::copyWithoutVertices() { return make_shared<StaircaseMountEdge>(getId(), mStaircase); }
	bool StaircaseMountEdge::isTraversable(shared_ptr<const Vertex> target, shared_ptr<const Agent> agent) const
	{
		// Restoration cannot strand an admitted stair user at the landing.
		if (agent && agent->getSector()
			&& agent->getSector()->getIndex() == mStaircase->getSectorIndex()
			&& target->getSector().get() != agent->getSector()) return true;
		auto const kind = mStaircase->isMoving()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		return !agentForbidsEdge(agent.get(), *this, kind);
	}
	EdgeTraversalRequestResult StaircaseMountEdge::requestTraversal(
		shared_ptr<const Vertex> target, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(target), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}
	DirectedTraversalFacts StaircaseMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> target, RouteDecisionContext const& context) const
	{
		bool const local = target->getSector().get() == context.observationSector
			|| getOtherVertex(target)->getSector().get() == context.observationSector;
		auto const kind = mStaircase->routeIsMoving(context.agent, local)
			? TraversalKind::Escalator : TraversalKind::Staircase;
		DirectedTraversalFacts facts;
		bool const dismounting = context.agent && context.agent->getSector()
			&& context.agent->getSector()->getIndex() == mStaircase->getSectorIndex()
			&& target->getSector().get() != context.agent->getSector();
		facts.feasible = dismounting || !routeRejectsEdge(context, *this, kind);
		if (facts.feasible) facts.objectiveDurationSeconds = 0.0f;
		else facts.exclusionReason = RouteExclusionReason::Mobility;
		return facts;
	}
}
