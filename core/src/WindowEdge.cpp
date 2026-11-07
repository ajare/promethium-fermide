#include "core/WindowEdge.h"

#include "core/Agent.h"
#include "core/Defines.h"

namespace core
{
	using namespace std;

	WindowEdge::WindowEdge(shared_ptr<Window> window)
		: Edge(EdgeType::Window), mWindow(std::move(window))
	{
	}

	WindowEdge::WindowEdge(uint32_t id, shared_ptr<Window> window)
		: Edge(id, EdgeType::Window), mWindow(std::move(window))
	{
	}

	shared_ptr<Edge> WindowEdge::copyWithoutVertices()
	{
		return make_shared<WindowEdge>(getId(), mWindow);
	}

	string WindowEdge::getDescription() const
	{
		return "Window threshold";
	}

	bool WindowEdge::isTraversable(shared_ptr<const Vertex>, shared_ptr<const Agent>) const
	{
		return mWindow && mWindow->isNormallyTraversable();
	}

	EdgeTraversalRequestResult WindowEdge::requestTraversal(shared_ptr<const Vertex>, shared_ptr<const Agent>) const
	{
		return isTraversable({}, {}) ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts WindowEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const&) const
	{
		DirectedTraversalFacts facts;
		facts.feasible = mWindow && mWindow->isNormallyTraversable();
		facts.exclusionReason = facts.feasible
			? RouteExclusionReason::None : RouteExclusionReason::Control;
		if (facts.feasible)
		{
			facts.components.motionSeconds = CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
			facts.objectiveDurationSeconds = facts.components.motionSeconds;
			facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		}
		return facts;
	}

	TraversalResourceId WindowEdge::getTraversalResourceId() const
	{
		return mWindow ? mWindow->getTraversalResourceId() : TraversalResourceId{};
	}
}
