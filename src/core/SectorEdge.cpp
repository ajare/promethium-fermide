#include <cassert>

#include "core/Defines.h"
#include "core/SectorEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"

namespace core
{

	using namespace std;

	/*
	SectorEdge
	----------

	Implementation of Edge for the Vertices in a Sector.  This is a default Edge which
	will typically be used for moving between Vertices in a Location when no more appropriate
	Edge implementation is available.
	*/

	SectorEdge::SectorEdge()
		: Edge(EdgeType::Location)
	{
	}

	SectorEdge::SectorEdge(uint32_t id)
		: Edge(id, EdgeType::Location)
	{
	}

	shared_ptr<Edge> SectorEdge::copyWithoutVertices()
	{
		return make_shared<SectorEdge>(getId());
	}

	string SectorEdge::getDescription() const
	{
		return "Sector edge";
	}

	bool SectorEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return true;
	}

	EdgeTraversalRequestResult SectorEdge::requestTraversal(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return EdgeTraversalRequestResult::OK;
	}

	DirectedTraversalFacts SectorEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		facts.feasible = true;
		auto const distance = getLength();
		facts.components.motionSeconds = distance == 0.0f
			? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : distance / context.walkSpeed;
		facts.objectiveDurationSeconds = facts.components.motionSeconds;
		facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		return facts;
	}

} // core