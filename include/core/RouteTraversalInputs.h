#pragma once

#include <memory>

#include "core/RouteCost.h"
#include "core/EdgeType.h"

namespace core
{
	class Edge;
	class Vertex;
	// Value-only inputs captured before a synchronous decision. No Agent, World,
	// resource, registry or endpoint reads are permitted by evaluate(). Ordinary
	// walking arcs use the Graph's immutable length directly, without a snapshot.
	struct RouteTraversalInputs
	{
		EdgeType type{};
		std::optional<TraversalKind> mobilityKind;
		bool buttons = false;
		bool airlock = false;
		float airlockCycleSeconds = 0;
		float interactionSeconds = 0;
		RouteExclusionReason exclusion = RouteExclusionReason::None;
		float length = 0;
		float rise = 0;
		float speed = 0;
		float dwell = 0;
		bool escalator = false;
		bool observed = false;
		uint32_t standingAgents = 0;
		bool extensible = false;
		float preparationSeconds = 0;
		bool extended = false;
		float extendedPercentage = 0;
		bool needsActivation = false;
		bool boarding = false;
		bool open = false;
		// 0 automatic, 1 manual, 2 remote; unavailable is captured as exclusion.
		uint8_t activation = 0;
		float queueSeconds = 0;
		float density = 0;
		uint32_t capacity = 1;
		std::optional<LiftRouteAccessObservation> lift;
		std::optional<ShuttleRouteAccessObservation> shuttle;
		bool shuttleStopsKnown = false;
		float shuttleDistance = 0;

		static RouteTraversalInputs capture(Edge const& edge,
			std::shared_ptr<const Vertex> const& target, RouteDecisionContext const& context);
		[[nodiscard]] DirectedTraversalFacts evaluate(RouteDecisionContext const& context) const;
	};
}
