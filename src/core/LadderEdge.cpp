#include <algorithm>
#include <cassert>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/LadderEdge.h"
#include "core/Vertex.h"
#include "core/Agent.h"
#include "core/Exceptions.h"
#include "core/World.h"


namespace core
{

	/*
	LadderEdge
	----------

	Implementation of Edge for the Vertices at either end of a Ladder.
	*/

	using namespace std;

	LadderEdge::LadderEdge(shared_ptr<Ladder> ladder)
		: Edge(EdgeType::Ladder)
		, mLadder(ladder)
	{
	}

	LadderEdge::LadderEdge(uint32_t id, shared_ptr<Ladder> ladder)
		: Edge(id, EdgeType::Ladder)
		, mLadder(ladder)
	{
	}

	shared_ptr<Edge> LadderEdge::copyWithoutVertices()
	{
		return make_shared<LadderEdge>(getId(), mLadder);
	}

	string LadderEdge::getDescription() const
	{
		return format("Ladder edge for {}", mLadder->getDescription());
	}

	bool LadderEdge::isTraversable(shared_ptr<const Vertex> /* targetVertex */, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsEdge(agent.get(), *this, TraversalKind::Ladder)) return false;
		// TODO: this will depend on whether there are any Agents in the way.
		return true;
	}

	EdgeTraversalRequestResult LadderEdge::requestTraversal(shared_ptr<const Vertex> targetVertex, shared_ptr<const Agent> agent) const
	{
		return isTraversable(std::move(targetVertex), std::move(agent))
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts LadderEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsEdge(context, *this, TraversalKind::Ladder))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto const source = getOtherVertex(targetVertex);
		SectorId sourceSector;
		bool locallyObserved = false;
		if (source)
		{
			sourceSector = SectorId{ static_cast<uint64_t>(source->getSector()->getIndex()) + 1 };
			for (auto const& edge : source->getEdges())
				if (edge->getType() == EdgeType::LadderMount)
				{
					auto approach = edge->getOtherVertex(source)->getSector();
					if (approach)
					{
						sourceSector = SectorId{ static_cast<uint64_t>(approach->getIndex()) + 1 };
						if (approach.get() == context.observationSector) locallyObserved = true;
					}
					break;
				}
		}
		if (mLadder->isExtensible() && context.world && context.agent
			&& ((locallyObserved && mLadder->isExtended()
				&& !context.world->agentAdheresToExtensiblePermission(
					mLadder->getTraversalResourceId(), sourceSector,
					source->getPosition(), context.world->getAgentId(context.agent)))
				|| (!(locallyObserved && mLadder->isExtended())
					&& !context.world->canAgentOperateExtensibleControl(
						mLadder->getTraversalResourceId(), sourceSector,
						source->getPosition(), context.world->getAgentId(context.agent)))))
		{
			facts.exclusionReason = RouteExclusionReason::Permission;
			return facts;
		}
		auto const rise = getDirectedRise(*targetVertex);
		auto const distance = getLength();
		auto const speed = context.climbSpeed > 0.0f
			? context.climbSpeed : static_cast<float>(CORE_AGENT_BASE_CLIMB_SPEED);
		facts.feasible = true;
		auto& c = facts.components;
		c.motionSeconds = distance == 0.0f
			? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : distance / speed;
		c.physicalEffortUnits = distance * (rise > 0.0f
			? context.policy.ladderAscentEffortPerUnit
			: context.policy.ladderDescentEffortPerUnit);
		c.interactionUnits = context.policy.ladderMountDismountInteraction;
		c.riskUnits = distance * context.policy.ladderRiskPerUnit;

		if (mLadder->isExtensible())
		{
			auto const preparation = mLadder->getExtendRetractTime();
			if (locallyObserved)
			{
				if (!mLadder->isExtended())
					c.knownWaitSeconds = preparation
						* (1.0f - std::clamp(mLadder->getExtendedPercentage(), 0.0f, 1.0f));
				if (mLadder->isRetracted() || mLadder->isRetracting())
					c.interactionUnits += context.policy.extensiblePreparationInteraction;
			}
			else
			{
				auto const probability = std::clamp(
					context.policy.unobservedLadderRetractedProbability, 0.0f, 1.0f);
				c.expectedWaitSeconds = probability * preparation;
				c.interactionUnits += probability
					* context.policy.extensiblePreparationInteraction;
				c.uncertaintyUnits = probability * (1.0f - probability) * preparation
					* context.policy.unobservedExtensionUncertaintyFraction;
			}
		}
		facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds
			+ c.expectedWaitSeconds;
		facts.optimisticLowerBoundSeconds = c.motionSeconds;
		return facts;
	}

	bool LadderEdge::requiresButton() const
	{
		return mLadder->isExtensible();
	}

	TraversalResourceId LadderEdge::getTraversalResourceId() const
	{
		return mLadder->getTraversalResourceId();
	}

} // core