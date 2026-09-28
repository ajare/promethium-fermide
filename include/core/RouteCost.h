#pragma once

#include <cmath>
#include <optional>
#include <initializer_list>
#include <stdexcept>

namespace core
{
	class Agent;

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
		float waitingAversion = 1;
		float effortAversion = 1;
		float interactionAversion = 1;
		float crowdAversion = 1;
		float riskAversion = 1;
	};

	struct DirectedTraversalFacts
	{
		bool feasible = false;
		RouteCostComponents components;
		// Legacy weights mix duration and preference: do not report them as time.
		std::optional<float> objectiveDurationSeconds;
		float optimisticLowerBoundSeconds = 0;
	};

	struct EvaluatedRouteCost
	{
		float perceivedCost;
		std::optional<float> objectiveDurationSeconds;
	};

	struct RouteChoicePolicy
	{
		EffectiveRoutingProfile baselineProfile;
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
				c.uncertaintyUnits, c.perceptionVariationUnits, facts.optimisticLowerBoundSeconds,
				profile.waitingAversion, profile.effortAversion, profile.interactionAversion,
				profile.crowdAversion, profile.riskAversion }) validate(value);
			if (facts.objectiveDurationSeconds) validate(*facts.objectiveDurationSeconds);
			auto cost = c.motionSeconds + profile.waitingAversion * (c.knownWaitSeconds + c.expectedWaitSeconds)
				+ profile.effortAversion * c.physicalEffortUnits + profile.interactionAversion * c.interactionUnits
				+ profile.crowdAversion * c.crowdingUnits + profile.riskAversion * c.riskUnits
				+ c.uncertaintyUnits + c.perceptionVariationUnits;
			validate(cost);
			if (cost < facts.optimisticLowerBoundSeconds)
				throw std::invalid_argument("Route cost is below its physical lower bound");
			return EvaluatedRouteCost{ cost, facts.objectiveDurationSeconds };
		}
	};

	// Constructed once per synchronous query. Legacy access is confined to snapshot
	// capture; the frontier consumes only captured directed costs, never live state.
	struct RouteDecisionContext
	{
		Agent const* const legacyAgent;
		EffectiveRoutingProfile const profile;
		RouteChoicePolicy const policy;
	};
}
