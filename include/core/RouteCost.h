#pragma once

#include "core/AgentTag.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <initializer_list>
#include <stdexcept>

namespace core
{
	class Agent;
	class Sector;
	class World;

	struct RouteCostComponents
	{
		float motionSeconds = 0;
		float knownWaitSeconds = 0;
		float expectedWaitSeconds = 0;
		float physicalEffortUnits = 0;
		float interactionUnits = 0;
		float crowdingUnits = 0;
		float riskUnits = 0;
		float uncertaintyUnits = 0;
		float perceptionVariationUnits = 0;
	};

	struct EffectiveRoutingProfile
	{
		float walkSpeedModifier = 1;
		float stairSpeedModifier = 1;
		float ladderSpeedModifier = 1;
		float escalatorWalkingChance = 0;
		float waitingAversion = 1;
		float effortAversion = 1;
		float interactionAversion = 1;
		float crowdAversion = 1;
		float riskAversion = 1;
		float routeFamiliarity = 0.5f;
		float routePersistence = 0.15f;
	};

	struct LiftRouteAccessObservation
	{
		uint32_t queuedAgents = 0;
		uint32_t capacity = 1;
		float minimumDwellSeconds = 0;
	};

	struct ShuttleRouteAccessObservation
	{
		uint32_t queuedAgents = 0;
		uint32_t capacity = 1;
		float minimumDwellSeconds = 0;
		float stopPosition = 0;
	};

	enum class RouteExclusionReason
	{
		None,
		Mobility,
		Clearance,
		Direction,
		Control,
		PreparationSide,
		Permission,
		NoContinuation,
		AnalysisLimit,
		Unknown
	};

	struct DirectedTraversalFacts
	{
		bool feasible = false;
		RouteExclusionReason exclusionReason{ RouteExclusionReason::None };
		RouteCostComponents components;
		// Legacy weights mix duration and preference: do not report them as time.
		std::optional<float> objectiveDurationSeconds;
		float optimisticLowerBoundSeconds = 0;
	};

	// Weighted seconds-equivalent contributions. Unlike DirectedTraversalFacts,
	// these values add directly to the perceived total shown in diagnostics.
	struct PerceivedRouteCostComponents
	{
		float movement = 0;
		float knownWait = 0;
		float expectedWait = 0;
		float effort = 0;
		float interaction = 0;
		float crowding = 0;
		float risk = 0;
		float uncertainty = 0;
		float stableVariation = 0;

		[[nodiscard]] float total() const
		{
			return movement + knownWait + expectedWait + effort + interaction
				+ crowding + risk + uncertainty + stableVariation;
		}
	};

	struct EvaluatedRouteCost
	{
		float perceivedCost;
		std::optional<float> objectiveDurationSeconds;
		PerceivedRouteCostComponents components;
	};

	struct RouteChoicePolicy
	{
		EffectiveRoutingProfile baselineProfile;
		// Seconds-equivalent threshold inconvenience and unknown-state expectations.
		float thresholdInteraction = 0.05f;
		float manualDoorInteraction = 0.5f;
		float remoteDoorInteraction = 2.0f;
		float unobservedDoorClosedProbability = 0.5f;
		float unobservedDoorQueueSeconds = 0.0f;
		// Stationary stair motion is directional. Effort and interaction are
		// seconds-equivalent values per unit of positive/negative rise, so the
		// three topology edges in one Stairwell flight still incur one charge.
		float stairAscentSpeed = 0.35f;
		float stairDescentSpeed = 0.45f;
		float stairAscentEffortPerRise = 1.5f;
		float stairDescentEffortPerRise = 0.25f;
		float stairInteractionPerFlight = 0.15f;
		// Ladders remain usable but carry ordinary effort, mounting inconvenience,
		// and perceived exposure. A later Agent property scales ladder speed; this
		// baseline uses the Agent's current effective physical climb speed.
		float ladderAscentEffortPerUnit = 1.5f;
		float ladderDescentEffortPerUnit = 1.0f;
		float ladderMountDismountInteraction = 1.5f;
		float ladderRiskPerUnit = 8.0f;
		// Extensible resources use these expectations when their deployed state is
		// not a local Route observation. Interaction is expected in the same
		// proportion as deployment delay.
		float unobservedLadderRetractedProbability = 0.5f;
		float unobservedForceBridgeRetractedProbability = 0.5f;
		float unobservedExtensionUncertaintyFraction = 0.25f;
		float extensiblePreparationInteraction = 1.0f;
		float forceBridgeRiskPerUnit = 0.25f;
		// Escalator route estimates use the expected contribution of walking;
		// admission still makes the authoritative deterministic walk/stand draw.
		float escalatorAscentEffortPerRise = 0.1f;
		float escalatorDescentEffortPerRise = 0.05f;
		float escalatorWalkingEffortPerUnit = 0.1f;
		float escalatorMountDismountInteraction = 0.3f;
		// Standing occupants divided by the evaluating Agent's walking chance.
		// At or above this value, expected walking is suppressed.
		float escalatorCongestionThreshold = 1.0f;
		// A transport journey pays access and exit overhead at its thresholds;
		// body edges contain only ride and intermediate-service time.
		float liftExpectedWaitSeconds = 3.25f;
		float liftExpectedQueuePassengers = 1.0f;
		float liftQueueServiceSeconds = 2.0f;
		float liftBoardingSeconds = 1.0f;
		float liftAlightingSeconds = 1.0f;
		float liftCallBoardingInteraction = 0.75f;
		float liftAlightingInteraction = 0.25f;
		float liftExpectedIntermediateStopsPerLevel = 0.12f;
		float liftExpectedCrowdingUnits = 0.2f;
		// Expected service interval, independent of unobserved vehicle position.
		float shuttleHeadwaySeconds = 12.0f;
		float shuttleExpectedQueuePassengers = 1.0f;
		float shuttleBoardingSeconds = 1.0f;
		float shuttleAlightingSeconds = 1.0f;
		float shuttleBoardingInteraction = 0.75f;
		float shuttleAlightingInteraction = 0.25f;
		float shuttleExpectedCrowdingUnits = 0.2f;
		float platformLiftPreparationSeconds = 2.0f;
		float platformLiftInconvenience = 8.0f;
		// A still-valid Path is replaced only when an alternative clears this
		// absolute gain and the Agent's proportional persistence threshold.
		float minimumSwitchGainSeconds = 2.0f;
		[[nodiscard]] bool shouldReplacePath(float currentCost, float alternativeCost,
			float routePersistence) const
		{
			if (!std::isfinite(currentCost) || !std::isfinite(alternativeCost)
				|| !std::isfinite(routePersistence) || !std::isfinite(minimumSwitchGainSeconds)
				|| currentCost < 0 || alternativeCost < 0 || routePersistence < 0
				|| routePersistence > 1 || minimumSwitchGainSeconds < 0)
				throw std::invalid_argument("Invalid Route persistence comparison");
			return currentCost - alternativeCost
				> std::max(minimumSwitchGainSeconds, currentCost * routePersistence);
		}
		[[nodiscard]] std::optional<EvaluatedRouteCost> evaluate(
			DirectedTraversalFacts const& facts, EffectiveRoutingProfile const& profile) const
		{
			if (!facts.feasible) return std::nullopt;
			auto validate = [](float value)
			{
				if (!std::isfinite(value) || value < 0)
					throw std::invalid_argument("Route cost must be finite and non-negative");
			};
			auto const& c = facts.components;
			for (auto value : { c.motionSeconds, c.knownWaitSeconds, c.expectedWaitSeconds,
				c.physicalEffortUnits, c.interactionUnits, c.crowdingUnits, c.riskUnits,
				c.uncertaintyUnits, facts.optimisticLowerBoundSeconds,
				profile.walkSpeedModifier, profile.stairSpeedModifier,
				profile.ladderSpeedModifier, profile.escalatorWalkingChance, profile.waitingAversion,
				profile.effortAversion, profile.interactionAversion,
				profile.crowdAversion, profile.riskAversion, profile.routeFamiliarity,
				profile.routePersistence }) validate(value);
			if (profile.routeFamiliarity > 1 || profile.routePersistence > 1
				|| !std::isfinite(c.perceptionVariationUnits))
				throw std::invalid_argument("Invalid route familiarity or perception variation");
			if (facts.objectiveDurationSeconds) validate(*facts.objectiveDurationSeconds);
			PerceivedRouteCostComponents components{
				c.motionSeconds,
				profile.waitingAversion * c.knownWaitSeconds,
				profile.waitingAversion * c.expectedWaitSeconds,
				profile.effortAversion * c.physicalEffortUnits,
				profile.interactionAversion * c.interactionUnits,
				profile.crowdAversion * c.crowdingUnits,
				profile.riskAversion * c.riskUnits,
				(1.0f - profile.routeFamiliarity) * c.uncertaintyUnits,
				c.perceptionVariationUnits
			};
			auto cost = components.total();
			if (!std::isfinite(cost)) throw std::invalid_argument("Route cost is not finite");
			// Stable error may be negative, but perception can never undercut the
			// directed traversal's universal physical lower bound. Attribute any
			// clamping to variation so the diagnostic components still add exactly.
			cost = std::max(cost, facts.optimisticLowerBoundSeconds);
			components.stableVariation += cost - components.total();
			return EvaluatedRouteCost{ cost, facts.objectiveDurationSeconds, components };
		}
	};

	// Constructed once per synchronous query. Agent observations are confined to
	// input capture; demand evaluation consumes value-only traversal inputs and
	// this frozen profile/policy, never live Agent or resource state.
	[[nodiscard]] int routeAgentLocalDepth(Agent const* agent);

	struct RouteDecisionContext
	{
		Agent const* const agent;
		EffectiveRoutingProfile const profile;
		RouteChoicePolicy const policy;
		Sector const* const observationSector = nullptr;
		float const walkSpeed = 0.5f;
		World const* const world = nullptr;
		float const climbSpeed = 0.25f;
		// False for the first search pass. If no Path exists, pathfinding repeats
		// with fallback Mobility profile entries admitted.
		bool const allowFallbackMobility = false;
		uint64_t const perceptionKey = 0;
		uint64_t const observationEpoch = 0;
		// Populated once at snapshot capture; direct diagnostic callers may omit it.
		std::optional<MobilityProfile> const mobilityProfile = std::nullopt;
		// Snapshot the departure depth, including stationary incoming-edge history.
		// This is context only: it adds no route cost or continuity preference.
		int const localDepth = routeAgentLocalDepth(agent);
		// Ordinary movement clears temporary poses. A retained-Pose traversal
		// driver can explicitly evaluate an already positioned Agent instead.
		bool const beginningMovement = true;
	};
}
