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
		return mForceBridge->isExtended();
	}

	EdgeTraversalRequestResult ForceBridgeEdge::requestTraversal(shared_ptr<const Vertex>,
		shared_ptr<const Agent> agent) const
	{
		if (agentForbidsButtons(agent.get()) && requiresButton())
			return EdgeTraversalRequestResult::Failed;
		return mForceBridge->extend() ? EdgeTraversalRequestResult::OK : EdgeTraversalRequestResult::Failed;
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
		if (mForceBridge->isExtensible()
			&& (!mForceBridge->hasExtensionControlInSector(sourceSector)
				|| !source || !mForceBridge->canPrepareFromPosition(source->getPosition().x)))
		{
			facts.exclusionReason = RouteExclusionReason::PreparationSide;
			return facts;
		}
		auto const locallyObserved = source && source->getSector().get()
			== context.observationSector;
		if (mForceBridge->isExtensible() && !(locallyObserved && mForceBridge->isExtended())
			&& context.world && context.agent
			&& !context.world->canAgentOperateExtensibleControl(
				mForceBridge->getTraversalResourceId(), sourceSector,
				context.world->getAgentId(context.agent)))
		{
			facts.exclusionReason = RouteExclusionReason::Permission;
			return facts;
		}

		facts.feasible = true;
		auto& c = facts.components;
		auto const distance = getLength();
		c.motionSeconds = distance == 0.0f ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
			: distance / context.walkSpeed;
		c.riskUnits = distance * context.policy.forceBridgeRiskPerUnit;
		if (mForceBridge->isExtensible())
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