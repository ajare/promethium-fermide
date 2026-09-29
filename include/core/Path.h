#pragma once

#include <memory>
#include <vector>

#include "core/Edge.h"
#include "core/EntityId.h"
#include "core/Vertex.h"


namespace core
{
	enum class RoutingPropertySource
	{
		Default,
		Individual,
		AgentTagSample
	};

	struct RoutingPropertyProvenance
	{
		RoutingPropertySource source{ RoutingPropertySource::Default };
		AgentTagId sourceTag{};

		bool operator==(RoutingPropertyProvenance const&) const = default;
	};

	struct RoutingProfileProvenance
	{
		RoutingPropertyProvenance walkSpeedModifier;
		RoutingPropertyProvenance stairSpeedModifier;
		RoutingPropertyProvenance ladderSpeedModifier;
		RoutingPropertyProvenance escalatorWalkingChance;
		RoutingPropertyProvenance waitingAversion;
		RoutingPropertyProvenance effortAversion;
		RoutingPropertyProvenance interactionAversion;
		RoutingPropertyProvenance crowdAversion;
		RoutingPropertyProvenance riskAversion;
		RoutingPropertyProvenance routeFamiliarity;
		RoutingPropertyProvenance routePersistence;

		bool operator==(RoutingProfileProvenance const&) const = default;
	};

	struct RouteDiagnosticContext
	{
		EffectiveRoutingProfile profile;
		RoutingProfileProvenance provenance;
		uint64_t topologyGeneration{ 0 };
	};

	struct PathNode
	{
		std::shared_ptr<const Edge> edge;
		std::shared_ptr<const Vertex> targetVertex;
		// Legacy name retained for serialization/runtime compatibility; this is a
		// cumulative perceived score, never a movement duration. Inferred-source
		// Paths include the initial walk to their first vertex (whose edge is null).
		float edgeWeight;
		std::optional<float> objectiveDurationSeconds;
		// Captured from the immutable route-decision context. It is deliberately
		// not serialized: a restored Path must report historical diagnostics as
		// unavailable rather than recreating them from current observations.
		std::optional<EvaluatedRouteCost> diagnosticCost;

		float getCumulativePerceivedCost() const { return edgeWeight; }
	};

	struct Path
	{
		std::vector<PathNode> nodes;
		std::optional<RouteDiagnosticContext> diagnosticContext;
	};

	struct PathRouteDiagnostics
	{
		PerceivedRouteCostComponents components;
		float perceivedCost{ 0 };
		std::optional<float> objectiveEstimatedDurationSeconds;
		RouteDiagnosticContext context;
	};

} // core
