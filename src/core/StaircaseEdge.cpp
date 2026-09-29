#include <algorithm>
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

	namespace
	{
		bool entryIsVisibleFrom(shared_ptr<const Vertex> const& entry, Sector const* observationSector)
		{
			if (!observationSector) return false;
			for (auto const& edge : entry->getEdges())
				if (edge->getType() == EdgeType::StaircaseMount
					&& edge->getOtherVertex(entry)->getSector().get() == observationSector) return true;
			return false;
		}
	}

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

	DirectedTraversalFacts StaircaseEdge::getDirectedTraversalFacts(
		shared_ptr<const Vertex> targetVertex, RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		auto const sourceVertex = getOtherVertex(targetVertex);
		auto const rise = getDirectedRise(*targetVertex);
		auto const ascending = rise > 0.0f;
		if (mStaircase->isEscalator())
		{
			if (routeRejectsEdge(context, *this, TraversalKind::Escalator))
			{
				facts.exclusionReason = RouteExclusionReason::Mobility;
				return facts;
			}
			if (ascending != (mStaircase->getSpeed() > 0.0f))
			{
				facts.exclusionReason = RouteExclusionReason::Direction;
				return facts;
			}
			auto walkingChance = clamp(context.profile.escalatorWalkingChance, 0.0f, 1.0f);
			if (context.agent && walkingChance > 0.0f
				&& entryIsVisibleFrom(sourceVertex, context.observationSector))
			{
				// A zero walking chance already contributes no expected walking and is
				// deliberately handled before this division.
				auto const congestion = static_cast<float>(
					context.agent->countObservedStandingEscalatorAgents(this)) / walkingChance;
				if (congestion >= context.policy.escalatorCongestionThreshold) walkingChance = 0.0f;
			}
			auto const expectedSpeed = abs(mStaircase->getSpeed())
				+ walkingChance * context.walkSpeed;
			facts.feasible = true;
			facts.components.motionSeconds = getLength() / expectedSpeed;
			facts.components.physicalEffortUnits = abs(rise) * (ascending
				? context.policy.escalatorAscentEffortPerRise
				: context.policy.escalatorDescentEffortPerRise)
				+ walkingChance * getLength() * context.policy.escalatorWalkingEffortPerUnit;
			facts.components.interactionUnits = context.policy.escalatorMountDismountInteraction;
			facts.objectiveDurationSeconds = facts.components.motionSeconds;
			facts.optimisticLowerBoundSeconds = getLength()
				/ (abs(mStaircase->getSpeed()) + context.walkSpeed);
			return facts;
		}
		if (routeRejectsEdge(context, *this, TraversalKind::Staircase))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto const speed = (ascending ? context.policy.stairAscentSpeed : context.policy.stairDescentSpeed)
			* context.profile.stairSpeedModifier;
		facts.feasible = true;
		facts.components.motionSeconds = getLength() / speed;
		facts.components.physicalEffortUnits = abs(rise) * (ascending
			? context.policy.stairAscentEffortPerRise : context.policy.stairDescentEffortPerRise);
		facts.components.interactionUnits = abs(rise) * context.policy.stairInteractionPerFlight;
		facts.objectiveDurationSeconds = facts.components.motionSeconds;
		facts.optimisticLowerBoundSeconds = facts.components.motionSeconds;
		return facts;
	}

	float StaircaseEdge::getTraversalSpeed(Agent const* agent,
		shared_ptr<const Vertex> const& targetVertex) const
	{
		if (mStaircase->isEscalator())
			return abs(mStaircase->getSpeed())
				+ (agent && agent->isWalkingOnEscalator(this) ? agent->getWalkSpeed() : 0.0f);
		if (!agent || !targetVertex) return 0.0f;
		auto const ascending = targetVertex->getPosition().y
			> getOtherVertex(targetVertex)->getPosition().y;
		return agent->getStationaryStairSpeed(ascending);
	}
}
