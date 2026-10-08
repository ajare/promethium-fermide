#include <algorithm>
#include <cassert>
#include <limits>

#include "core/Defines.h"
#include "core/MobilityProfile.h"
#include "core/ForceBridgeEdge.h"
#include "core/Agent.h"
#include "core/Vertex.h"
#include "core/Exceptions.h"
#include "core/World.h"


namespace core
{

	using namespace std;

	/*
	ForceBridgeEdge
	---------------

	Implementation of Edge for the Vertices on either side of a ForceBridge.
	*/

	ForceBridgeEdge::ForceBridgeEdge(shared_ptr<ForceBridge> forceBridge)
		: Edge(EdgeType::ForceBridge)
		, mForceBridge(forceBridge)
	{
	}

	ForceBridgeEdge::ForceBridgeEdge(uint32_t id, shared_ptr<ForceBridge> forceBridge)
		: Edge(id, EdgeType::ForceBridge)
		, mForceBridge(forceBridge)
	{
	}

	shared_ptr<Edge> ForceBridgeEdge::copyWithoutVertices()
	{
		return make_shared<ForceBridgeEdge>(getId(), mForceBridge);
	}

	string ForceBridgeEdge::getDescription() const
	{
		return format("ForceBridge edge for {}", mForceBridge->getDescription());
	}

	bool ForceBridgeEdge::isTraversable(shared_ptr<const Vertex>, shared_ptr<const Agent> agent) const
	{
		if (agentForbidsButtons(agent.get()) && requiresButton()) return false;
		return mForceBridge->admitsNewTraversals();
	}

	EdgeTraversalRequestResult ForceBridgeEdge::requestTraversal(shared_ptr<const Vertex>,
		shared_ptr<const Agent> agent) const
	{
		if (agentForbidsButtons(agent.get()) && requiresButton())
			return EdgeTraversalRequestResult::Failed;
		return (mForceBridge->admitsNewTraversals() || mForceBridge->extend())
			? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
	}

	DirectedTraversalFacts ForceBridgeEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		if (routeRejectsButtons(context) && requiresButton())
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto const source = getOtherVertex(targetVertex);
		auto const sourceSector = source && source->getSector()
			? SectorId{ static_cast<uint64_t>(source->getSector()->getIndex()) + 1 }
			: SectorId{};
		auto const locallyObserved = source && source->getSector().get() == context.observationSector;
		auto known = mForceBridge->knownCondition(context.agent, getTraversalResourceId(), context.observationSector, locallyObserved);
		if (known && !known->admitsPassage())
		{
			facts.exclusionReason = RouteExclusionReason::Control;
			return facts;
		}
		bool const frozenExtended = known && known->broken && known->position >= 1.0f;
		bool const usable = frozenExtended || (locallyObserved && mForceBridge->isExtended());
		if (mForceBridge->isExtensible() && !frozenExtended
			&& (!mForceBridge->hasExtensionControlInSector(sourceSector)
				|| !source || !mForceBridge->canPrepareFromPosition(source->getPosition().x)))
		{
			facts.exclusionReason = RouteExclusionReason::PreparationSide;
			return facts;
		}
		if (mForceBridge->isExtensible() && context.world && context.agent
			&& ((usable
				&& !context.world->agentAdheresToExtensiblePermission(
					mForceBridge->getTraversalResourceId(), sourceSector,
					source->getPosition(), context.world->getAgentId(context.agent)))
				|| (!usable
					&& !context.world->canAgentOperateExtensibleControl(
						mForceBridge->getTraversalResourceId(), sourceSector,
						source->getPosition(), context.world->getAgentId(context.agent)))))
		{
			facts.exclusionReason = RouteExclusionReason::Permission;
			return facts;
		}

		auto const distance = getLength();
		auto const motion = context.agent
			? context.agent->roomMovementSeconds(*source, *targetVertex, context.walkSpeed)
			: std::optional<float>{distance == 0.0f ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
				: distance / context.walkSpeed};
		if (!motion)
		{
			facts.exclusionReason = RouteExclusionReason::Clearance;
			return facts;
		}
		facts.feasible = true;
		auto& c = facts.components;
		c.motionSeconds = *motion;
		c.riskUnits = distance * context.policy.forceBridgeRiskPerUnit;
		if (mForceBridge->isExtensible() && !frozenExtended)
		{
			auto const preparation = mForceBridge->getExtendRetractTime();
			if (locallyObserved)
			{
				if (!mForceBridge->isExtended())
					c.knownWaitSeconds = preparation * (1.0f - std::clamp(
						mForceBridge->getExtendedPercentage(), 0.0f, 1.0f));
				if (mForceBridge->isRetracted() || mForceBridge->isRetracting())
					c.interactionUnits = context.policy.extensiblePreparationInteraction;
			}
			else
			{
				auto const probability = std::clamp(
					context.policy.unobservedForceBridgeRetractedProbability, 0.0f, 1.0f);
				c.expectedWaitSeconds = probability * preparation;
				c.interactionUnits = probability
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

	bool ForceBridgeEdge::requiresButton() const
	{
		return mForceBridge->isExtensible();
	}

	TraversalResourceId ForceBridgeEdge::getTraversalResourceId() const
	{
		return mForceBridge->getTraversalResourceId();
	}

} // core