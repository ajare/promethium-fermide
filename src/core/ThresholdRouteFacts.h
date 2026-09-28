#pragma once

#include "core/Agent.h"
#include "core/Door.h"
#include "core/Edge.h"
#include "core/MobilityProfile.h"
#include "core/Vertex.h"
#include "core/Sector.h"

namespace core
{
	// Capture once per directed threshold arc, never on adjoining Location arcs.
	// Visibility is a sector relationship, not proximity across coincident Layers.
	inline DirectedTraversalFacts thresholdRouteFacts(Edge const& edge, Door const& door,
		std::shared_ptr<const Vertex> target, RouteDecisionContext const& context,
		float motionSeconds, float openingSeconds)
	{
		if (agentForbidsEdge(context.legacyAgent, edge, TraversalKind::Door)) return {};
		auto source = edge.getOtherVertex(target);
		auto sector = source ? source->getSector() : nullptr;
		bool const observed = sector && sector.get() == context.observationSector;
		auto const& policy = context.policy;
		DirectedTraversalFacts facts;
		facts.feasible = true;
		auto& c = facts.components;
		c.motionSeconds = motionSeconds;
		facts.optimisticLowerBoundSeconds = motionSeconds;
		// Style deliberately never enters routing, including tall OpenUp animation timing.
		float preparationProbability = observed ? (door.isOpen() ? 0.0f : 1.0f)
			: policy.unobservedDoorClosedProbability;
		if (observed)
		{
			c.knownWaitSeconds = preparationProbability * openingSeconds;
			if (context.legacyAgent)
				c.knownWaitSeconds += context.legacyAgent->estimateTraversalDelay(
					edge.getTraversalResourceId(), SectorId{ (uint64_t)sector->getIndex() + 1 });
		}
		else
			c.expectedWaitSeconds = preparationProbability * openingSeconds
				+ policy.unobservedDoorQueueSeconds;
		c.interactionUnits = policy.thresholdInteraction;
		if (door.getActivationMode() == DoorActivationMode::Manual)
			c.interactionUnits += preparationProbability * policy.manualDoorInteraction;
		else if (door.getActivationMode() == DoorActivationMode::RemoteControlled)
			c.interactionUnits += preparationProbability * policy.remoteDoorInteraction;
		facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds + c.expectedWaitSeconds;
		return facts;
	}
}
