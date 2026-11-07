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
		if (mLadder->isBroken() && !mLadder->admitsNewTraversals()) return false;
		// TODO: see if any Agents are on the Ladder

		return true;
	}

	EdgeTraversalRequestResult LadderMountEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts LadderMountEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsEdge(context, *this, TraversalKind::Ladder))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto const source = getOtherVertex(targetVertex);
		auto known = mLadder->knownCondition(context.agent, getTraversalResourceId(), context.observationSector,
			source && source->getSector().get() == context.observationSector);
		if (known && !known->admitsPassage())
		{
			facts.exclusionReason = RouteExclusionReason::Control;
			return facts;
		}
		bool const frozenExtended = known && known->broken && known->position >= 1.0f;
		if (mLadder->isExtensible() && !frozenExtended && source && source->getType() != VertexType::Ladder)
		{
			auto const sourceSector = source->getSector()
				? SectorId{ static_cast<uint64_t>(source->getSector()->getIndex()) + 1 }
				: SectorId{};
			// Preparation capability is authored, not inferred from the Ladder's
			// transient deployed state. An already-extended Ladder may retract before
			// a remote Agent reaches it.
			if (!mLadder->hasExtensionControlInSector(sourceSector))
			{
				facts.exclusionReason = RouteExclusionReason::PreparationSide;
				return facts;
			}
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