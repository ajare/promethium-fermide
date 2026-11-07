#include <cassert>

#include "core/Defines.h"
#include "core/GapEdge.h"
#include "core/Agent.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	GapEdge
	-------

	Implementation of Edge for the Vertices on either side of a space in the air.  This can
	happen when there a Walkways on either side of a Cell, but nothing between them, or when
	a Location leads to another Location at a higher level but there is no Walkway for it to
	connect to.

	Gaps are not crossable. They remain in the topology to represent absent physical
	continuation, but directed route facts exclude them from every Path.
	*/

	GapEdge::GapEdge()
		: Edge(EdgeType::Gap)
	{
	}

	GapEdge::GapEdge(uint32_t id)
		: Edge(id, EdgeType::Gap)
	{
	}

	shared_ptr<Edge> GapEdge::copyWithoutVertices()
	{
		return make_shared<GapEdge>(getId());
	}

	string GapEdge::getDescription() const
	{
		return "Gap edge";
	}

	bool GapEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return false;
	}

	EdgeTraversalRequestResult GapEdge::requestTraversal(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> /* agent */) const
	{
		return EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts GapEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex>, RouteDecisionContext const&) const
	{
		DirectedTraversalFacts facts;
		facts.exclusionReason = RouteExclusionReason::NoContinuation;
		return facts;
	}

} // core