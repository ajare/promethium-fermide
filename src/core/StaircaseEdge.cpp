#include <cmath>
#include <format>
#include <limits>

#include "core/Agent.h"
#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/StaircaseEdge.h"
#include "core/Vertex.h"

namespace core
{
	using namespace std;

	StaircaseEdge::StaircaseEdge(shared_ptr<Staircase> staircase)
		: Edge(EdgeType::Staircase), mStaircase(std::move(staircase)) {}

	StaircaseEdge::StaircaseEdge(uint32_t id, shared_ptr<Staircase> staircase)
		: Edge(id, EdgeType::Staircase), mStaircase(std::move(staircase)) {}

	string StaircaseEdge::getDescription() const
	{
		return format("Staircase edge for {}", mStaircase->getDescription());
	}

	shared_ptr<Edge> StaircaseEdge::copyWithoutVertices()
	{
		return make_shared<StaircaseEdge>(getId(), mStaircase);
	}

	bool StaircaseEdge::isTraversable(shared_ptr<const Vertex> targetVertex,
		shared_ptr<const Agent> agent) const
	{
		auto const kind = mStaircase->isEscalator()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		if (agentForbidsEdge(agent.get(), *this, kind)) return false;
		if (!mStaircase->isEscalator()) return true;
		auto sourceVertex = getOtherVertex(targetVertex);
		bool const movingUp = targetVertex->getPosition().y > sourceVertex->getPosition().y;
		return movingUp == (mStaircase->getSpeed() > 0.0f);
	}

	EdgeTraversalRequestResult StaircaseEdge::requestTraversal(
		shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float StaircaseEdge::getWeight(shared_ptr<const Vertex> targetVertex,
		Agent const* agent, bool) const
	{
		auto const kind = mStaircase->isEscalator()
			? TraversalKind::Escalator : TraversalKind::Staircase;
		if (agentForbidsEdge(agent, *this, kind)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		if (!mStaircase->isEscalator()) return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
		auto sourceVertex = getOtherVertex(targetVertex);
		bool const movingUp = targetVertex->getPosition().y > sourceVertex->getPosition().y;
		if (movingUp != (mStaircase->getSpeed() > 0.0f))
			return numeric_limits<float>::infinity();
		return getLength() / abs(mStaircase->getSpeed());
	}

	float StaircaseEdge::getTraversalSpeed(Agent const* agent) const
	{
		return abs(mStaircase->getSpeed())
			+ (mStaircase->isEscalator() && agent && agent->isWalkingOnEscalator(this)
				? agent->getWalkSpeed() : 0.0f);
	}
}
