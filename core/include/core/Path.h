#pragma once

#include <memory>
#include <vector>

#include "core/Edge.h"
#include "core/EntityId.h"
#include "core/AgentTag.h"
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
		MobilityProfile mobilityProfile;
		RoutingPropertyProvenance mobilityProvenance;
		uint64_t topologyGeneration{ 0 };
		bool allowFallbackMobility{ false };
	};

	struct PathNode
	{
		std::shared_ptr<const Edge> edge;
		std::shared_ptr<const Vertex> targetVertex;
		// Cumulative perceived score, never a movement duration. Inferred-source
		// Paths include the initial walk to their first vertex (whose edge is null).
		float cumulativePerceivedCost;
		std::optional<float> objectiveDurationSeconds;
		// Captured from the immutable route-decision context. It is deliberately
		// not serialized: a restored Path must report historical diagnostics as
		// unavailable rather than recreating them from current observations.
		std::optional<EvaluatedRouteCost> diagnosticCost;

		float getCumulativePerceivedCost() const { return cumulativePerceivedCost; }
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

	enum class RouteExplanationEvidence
	{
		DecisionTime,
		CurrentContext
	};

	struct RouteContinuationExplanation
	{
		std::shared_ptr<const Edge> edge;
		std::shared_ptr<const Vertex> nextVertex;
		bool selected{ false };
		bool feasible{ false };
		RouteExclusionReason exclusionReason{ RouteExclusionReason::None };
		float perceivedContinuationCost{ 0 };
		PerceivedRouteCostComponents components;
		std::optional<float> capturedPerceivedContinuationCost;
		std::optional<PerceivedRouteCostComponents> capturedComponents;
	};

	struct PathVertexExplanation
	{
		uint32_t pathNodeIndex{ 0 };
		std::shared_ptr<const Vertex> vertex;
		bool target{ false };
		bool meaningfulDecision{ false };
		std::vector<RouteContinuationExplanation> continuations;
	};

	struct PathRouteExplanation
	{
		RouteExplanationEvidence comparisonEvidence{ RouteExplanationEvidence::CurrentContext };
		bool capturedContextStale{ true };
		bool analysisTruncated{ false };
		std::vector<PathVertexExplanation> vertices;
	};

} // core
