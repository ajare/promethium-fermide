#include <format>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/StaircaseMountEdge.h"

namespace core
{
	using namespace std;
	StaircaseMountEdge::StaircaseMountEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	StaircaseMountEdge::StaircaseMountEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::StaircaseMount), mStaircase(std::move(staircase)) {}
	string StaircaseMountEdge::getDescription() const { return format("Staircase mount edge for {}", mStaircase->getDescription()); }
	shared_ptr<Edge> StaircaseMountEdge::copyWithoutVertices() { return make_shared<StaircaseMountEdge>(getId(), mStaircase); }
	bool StaircaseMountEdge::isTraversable(shared_ptr<const Vertex>, shared_ptr<const Agent> agent) const
	{
		auto const kind = mStaircase->isEscalator()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		return !agentForbidsEdge(agent.get(), *this, kind);
	}
	EdgeTraversalRequestResult StaircaseMountEdge::requestTraversal(
		shared_ptr<const Vertex> target, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(target), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}
	float StaircaseMountEdge::getWeight(shared_ptr<const Vertex>, Agent const* agent, bool) const
	{
		auto const kind = mStaircase->isEscalator()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		return agentForbidsEdge(agent, *this, kind) ? CORE_GRAPH_EDGE_UNTRAVERSABLE : 0.0f;
	}

	DirectedTraversalFacts StaircaseMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		auto const kind = mStaircase->isEscalator()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		DirectedTraversalFacts facts;
		facts.feasible = !agentForbidsEdge(context.legacyAgent, *this, kind);
		if (facts.feasible) facts.objectiveDurationSeconds = 0.0f;
		return facts;
	}
}
