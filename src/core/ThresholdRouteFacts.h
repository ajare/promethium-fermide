#pragma once

#include <algorithm>

#include "core/Agent.h"
#include "core/Door.h"
#include "core/Edge.h"
#include "core/MobilityProfile.h"
#include "core/Vertex.h"
#include "core/Sector.h"
#include "core/World.h"

namespace core
{
	// Capture once per directed threshold arc, never on adjoining Location arcs.
	// Visibility is a sector relationship, not proximity across coincident Layers.
	inline DirectedTraversalFacts thresholdRouteFacts(Edge const& edge, Door const& door,
		std::shared_ptr<const Vertex> target, RouteDecisionContext const& context,
		float motionSeconds, float openingSeconds)
	{
		DirectedTraversalFacts facts;
		if (agentRejectsEdge(context.legacyAgent, edge, TraversalKind::Door, context.allowFallbackMobility))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		auto source = edge.getOtherVertex(target);
		auto sector = source ? source->getSector() : nullptr;
		bool const observed = sector && sector.get() == context.observationSector;
		auto const& policy = context.policy;
		facts.feasible = true;
		auto& c = facts.components;
		c.motionSeconds = motionSeconds;
		facts.optimisticLowerBoundSeconds = motionSeconds;
		auto const sourceEndpoint = source ? source->getPosition() : Vector2{};
		// Querying an unobserved landing returns static capacity only. The World does
		// not inspect its queue, car position, calls, or scheduler state.
		auto liftAccess = context.legacyAgent ? context.legacyAgent->observeLiftAccess(
			edge.getTraversalResourceId(), sourceEndpoint, observed) : std::nullopt;
		auto shuttleAccess = context.world ? context.world->observeShuttleAccess(
			edge.getTraversalResourceId(), sourceEndpoint, observed)
			: context.legacyAgent ? context.legacyAgent->observeShuttleAccess(
				edge.getTraversalResourceId(), sourceEndpoint, observed) : std::nullopt;
		if (door.getActivationMode() == DoorActivationMode::Unavailable
			&& !door.isOpen() && !liftAccess && !shuttleAccess)
		{
			facts.feasible = false;
			facts.exclusionReason = RouteExclusionReason::Control;
			return facts;
		}
		if (shuttleAccess)
		{
			bool const boarding = sector && isLocationLike(sector->getType());
			c.motionSeconds += boarding ? policy.shuttleBoardingSeconds : policy.shuttleAlightingSeconds;
			c.interactionUnits = policy.thresholdInteraction + (boarding
				? policy.shuttleBoardingInteraction : policy.shuttleAlightingInteraction);
			if (boarding)
			{
				// Half a headway to the next service, plus expected missed services.
				// No vehicle position, manifest, or remote queue is consulted.
				auto passengers = observed ? (float)shuttleAccess->queuedAgents
					: policy.shuttleExpectedQueuePassengers;
				c.expectedWaitSeconds = policy.shuttleHeadwaySeconds
					* (0.5f + passengers / shuttleAccess->capacity);
				c.crowdingUnits = observed ? passengers / shuttleAccess->capacity
					: policy.shuttleExpectedCrowdingUnits;
				if (observed) c.knownWaitSeconds = door.isOpen() ? 0.0f : openingSeconds;
				else c.expectedWaitSeconds += policy.unobservedDoorClosedProbability * openingSeconds;
			}
			facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds + c.expectedWaitSeconds;
			return facts;
		}
		bool const liftBoarding = liftAccess && sector && isLocationLike(sector->getType());
		// Style deliberately never enters routing, including tall OpenUp animation timing.
		float preparationProbability = observed ? (door.isOpen() ? 0.0f : 1.0f)
			: policy.unobservedDoorClosedProbability;
		if (observed)
		{
			c.knownWaitSeconds = preparationProbability * openingSeconds;
			if (liftBoarding)
			{
				// The access-zone queue is observable; remote car allocation is not.
				c.expectedWaitSeconds = policy.liftExpectedWaitSeconds;
				c.knownWaitSeconds += policy.liftQueueServiceSeconds
					* (float)liftAccess->queuedAgents / std::max(1u, liftAccess->capacity);
				c.crowdingUnits = (float)liftAccess->queuedAgents
					/ std::max(1u, liftAccess->capacity);
			}
			else if (!liftAccess && context.legacyAgent)
			{
				auto const sourceSector = SectorId{ (uint64_t)sector->getIndex() + 1 };
				// Admission delay and visible density are deliberately independent:
				// Waiting aversion weights the former and Crowd aversion the latter.
				c.knownWaitSeconds += context.legacyAgent->estimateTraversalDelay(
					edge.getTraversalResourceId(), sourceSector);
				c.crowdingUnits = context.legacyAgent->observeAccessZoneDensity(
					edge.getTraversalResourceId(), sourceSector);
			}
		}
		else
		{
			c.expectedWaitSeconds = preparationProbability * openingSeconds
				+ policy.unobservedDoorQueueSeconds;
			if (liftBoarding)
			{
				c.expectedWaitSeconds += policy.liftExpectedWaitSeconds
					+ policy.liftQueueServiceSeconds * policy.liftExpectedQueuePassengers
					/ std::max(1u, liftAccess->capacity);
				c.crowdingUnits = policy.liftExpectedCrowdingUnits;
			}
		}
		c.interactionUnits = policy.thresholdInteraction;
		if (liftBoarding)
		{
			c.motionSeconds += policy.liftBoardingSeconds;
			c.expectedWaitSeconds += liftAccess->minimumDwellSeconds;
			c.interactionUnits += policy.liftCallBoardingInteraction;
		}
		else if (liftAccess)
		{
			c.motionSeconds += policy.liftAlightingSeconds;
			c.interactionUnits += policy.liftAlightingInteraction;
		}
		else if (door.getActivationMode() == DoorActivationMode::Manual)
			c.interactionUnits += preparationProbability * policy.manualDoorInteraction;
		else if (door.getActivationMode() == DoorActivationMode::RemoteControlled)
			c.interactionUnits += preparationProbability * policy.remoteDoorInteraction;
		facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds + c.expectedWaitSeconds;
		return facts;
	}
}
