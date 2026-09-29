#include <cassert>
#include <limits>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/LadderMountEdge.h"
#include "core/Agent.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"


namespace core
{

	using namespace std;

	/*
	LadderMountEdge
	---------------

	This Edge connects a regular SectorObjectVertex to a LadderVertex.  It is traversed immediately, assuming
	that an Agent is able to use the Ladder.  Its vertices may or may not be in the same Sector, but will
	be in the same Layer.
	*/

	LadderMountEdge::LadderMountEdge(shared_ptr<Ladder> ladder)
		: Edge(EdgeType::LadderMount)
		, mLadder(ladder)
	{
	}

	LadderMountEdge::LadderMountEdge(uint32_t id, shared_ptr<Ladder> ladder)
		: Edge(id, EdgeType::LadderMount)
		, mLadder(ladder)
	{
	}

	shared_ptr<Ladder> LadderMountEdge::getLadder() const
	{
		return mLadder;
	}

	shared_ptr<Edge> LadderMountEdge::copyWithoutVertices()
	{
		return make_shared<LadderMountEdge>(getId(), getLadder());
	}

	string LadderMountEdge::getDescription() const
	{
		return format("LadderMount edge for {}", mLadder->getDescription());
	}

	bool LadderMountEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Ladder)) return false;
		// TODO: see if any Agents are on the Ladder

		return true;
	}

	EdgeTraversalRequestResult LadderMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	float LadderMountEdge::getWeight(shared_ptr<const Vertex> targetVertex, Agent const* agent, bool /* edgeVisible */) const
	{
		if (agentForbidsEdge(agent, *this, TraversalKind::Ladder)) return CORE_GRAPH_EDGE_UNTRAVERSABLE;
		if (!mLadder->isExtended())
		{
			auto source = getOtherVertex(targetVertex);
			auto sourceSector = source && source->getSector()
				? SectorId{ (uint64_t)source->getSector()->getIndex() + 1 } : SectorId{};
			if (source && source->getType() != VertexType::Ladder
				&& !mLadder->canPrepareFrom(sourceSector))
				return numeric_limits<float>::infinity();
		}
		return CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
	}

	DirectedTraversalFacts LadderMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (agentRejectsEdge(context.legacyAgent, *this, TraversalKind::Ladder, context.allowFallbackMobility)) return facts;
		auto const source = getOtherVertex(targetVertex);
		if (mLadder->isExtensible() && source && source->getType() != VertexType::Ladder)
		{
			auto const sourceSector = source->getSector()
				? SectorId{ static_cast<uint64_t>(source->getSector()->getIndex()) + 1 }
				: SectorId{};
			// Preparation capability is authored, not inferred from the Ladder's
			// transient deployed state. An already-extended Ladder may retract before
			// a remote Agent reaches it.
			if (!mLadder->hasExtensionControlInSector(sourceSector)) return facts;
		}
		facts.feasible = true;
		facts.objectiveDurationSeconds = 0.0f;
		return facts;
	}

	bool LadderMountEdge::requiresButton() const
	{
		return mLadder->isExtensible();
	}

	TraversalResourceId LadderMountEdge::getTraversalResourceId() const
	{
		return mLadder->getTraversalResourceId();
	}

} // core