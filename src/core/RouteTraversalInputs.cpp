#include "core/AgentType.h"
#include "core/RouteTraversalInputs.h"
#include "core/Defines.h"
#include "core/Agent.h"
#include "core/MobilityProfile.h"
#include "core/World.h"

namespace core
{
	DirectedTraversalFacts RouteTraversalInputs::evaluate(RouteDecisionContext const& context) const
	{
		DirectedTraversalFacts facts;
		auto const& p = context.policy;
		auto const& profile = context.profile;
		auto const mobility = context.mobilityProfile.value_or(MobilityProfile{});
		auto rejects = [&](MobilityUse use)
		{
			return use == MobilityUse::CannotUse
				|| (use == MobilityUse::OnlyIfNoOtherOption && !context.allowFallbackMobility);
		};
		if ((mobilityKind && rejects(mobility.get(*mobilityKind)))
			|| (buttons && rejects(mobility.get(TraversalKind::Buttons))))
		{
			facts.exclusionReason = RouteExclusionReason::Mobility;
			return facts;
		}
		if (exclusion != RouteExclusionReason::None)
		{
			facts.exclusionReason = exclusion;
			return facts;
		}
		facts.feasible = true;
		auto& c = facts.components;
		auto walking = [&] { return length == 0 ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME : length / context.walkSpeed; };
		auto extension = [&](float probability)
		{
			if (!extensible || extended) return;
			if (observed)
			{
				if (!extended) c.knownWaitSeconds = preparationSeconds
					* (1.0f - std::clamp(extendedPercentage, 0.0f, 1.0f));
				if (needsActivation) c.interactionUnits += p.extensiblePreparationInteraction;
			}
			else
			{
				probability = std::clamp(probability, 0.0f, 1.0f);
				c.expectedWaitSeconds = probability * preparationSeconds;
				c.interactionUnits += probability * p.extensiblePreparationInteraction;
				c.uncertaintyUnits = probability * (1.0f - probability) * preparationSeconds
					* p.unobservedExtensionUncertaintyFraction;
			}
		};
		switch (type)
		{
		case EdgeType::Location:
			c.motionSeconds = walking();
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::Window:
			c.motionSeconds = CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME;
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::LadderMount: case EdgeType::StaircaseMount:
		case EdgeType::StairwellMount: case EdgeType::ShuttleMount:
			break;
		case EdgeType::Staircase: case EdgeType::Stairwell:
			if (escalator)
			{
				auto chance = std::clamp(profile.escalatorWalkingChance, 0.0f, 1.0f);
				if (observed && chance > 0 && static_cast<float>(standingAgents) / chance >= p.escalatorCongestionThreshold)
					chance = 0;
				c.motionSeconds = length / (std::abs(speed) + chance * context.walkSpeed);
				c.physicalEffortUnits = std::abs(rise) * (rise > 0 ? p.escalatorAscentEffortPerRise : p.escalatorDescentEffortPerRise)
					+ chance * length * p.escalatorWalkingEffortPerUnit;
				c.interactionUnits = p.escalatorMountDismountInteraction;
				facts.optimisticLowerBoundSeconds = length / (std::abs(speed) + context.walkSpeed);
			}
			else
			{
				auto const typeSpeed = context.agent
					? (rise > 0 ? context.agent->getPhysicalBaseline().stairAscentSpeed
						: context.agent->getPhysicalBaseline().stairDescentSpeed)
					// Agent-less editor previews retain the Human-compatible baseline.
					: (rise > 0 ? bundledHumanBaseline().stairAscentSpeed
						: bundledHumanBaseline().stairDescentSpeed);
				c.motionSeconds = length / (typeSpeed * profile.stairSpeedModifier);
				c.physicalEffortUnits = std::abs(rise) * (rise > 0 ? p.stairAscentEffortPerRise : p.stairDescentEffortPerRise);
				c.interactionUnits = std::abs(rise) * p.stairInteractionPerFlight;
				facts.optimisticLowerBoundSeconds = c.motionSeconds;
			}
			break;
		case EdgeType::Ladder:
			c.motionSeconds = length == 0 ? CORE_GRAPH_EDGE_MIN_TRAVERSAL_TIME
				: length / (context.climbSpeed > 0 ? context.climbSpeed
					: bundledHumanBaseline().climbSpeed);
			c.physicalEffortUnits = length * (rise > 0 ? p.ladderAscentEffortPerUnit : p.ladderDescentEffortPerUnit);
			c.interactionUnits = p.ladderMountDismountInteraction;
			c.riskUnits = length * p.ladderRiskPerUnit;
			extension(p.unobservedLadderRetractedProbability);
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::ForceBridge:
			c.motionSeconds = walking();
			c.riskUnits = length * p.forceBridgeRiskPerUnit;
			extension(p.unobservedForceBridgeRetractedProbability);
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::Lift:
			c.motionSeconds = length / speed;
			c.expectedWaitSeconds = length * p.liftExpectedIntermediateStopsPerLevel * dwell;
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::Shuttle:
			c.motionSeconds = shuttleStopsKnown
				? (shuttleDistance > 0 ? shuttleDistance / speed : length / context.walkSpeed) : length / speed;
			if (shuttleStopsKnown && shuttleDistance > 0) c.expectedWaitSeconds = dwell;
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			break;
		case EdgeType::LiftMount:
			if (boarding)
			{
				if (lift)
				{
					c.expectedWaitSeconds = p.liftExpectedWaitSeconds;
					c.knownWaitSeconds = p.liftQueueServiceSeconds * static_cast<float>(lift->queuedAgents) / std::max(1u, lift->capacity);
					c.crowdingUnits = static_cast<float>(lift->queuedAgents) / std::max(1u, lift->capacity);
				}
				else
				{
					c.expectedWaitSeconds = p.liftExpectedWaitSeconds + p.liftQueueServiceSeconds * p.liftExpectedQueuePassengers / capacity;
					c.crowdingUnits = p.liftExpectedCrowdingUnits;
				}
				c.expectedWaitSeconds += dwell + p.platformLiftPreparationSeconds;
				c.motionSeconds = p.liftBoardingSeconds;
				c.interactionUnits = p.liftCallBoardingInteraction + p.platformLiftInconvenience;
			}
			else
			{
				c.motionSeconds = p.liftAlightingSeconds;
				c.interactionUnits = p.liftAlightingInteraction;
			}
			break;
		case EdgeType::Door: case EdgeType::BulkheadDoor:
		{
			c.motionSeconds = type == EdgeType::Door
				? (crawling ? (6.0f / 60.0f) / context.agent->getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, Pose::Crawling).value()
					: 6.0f / 60.0f)
				: walking() / (crawling ? context.agent->getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, Pose::Crawling).value() : 1.0f);
			facts.optimisticLowerBoundSeconds = c.motionSeconds;
			if (securityScanner)
			{
				c.expectedWaitSeconds = preparationSeconds;
				if (boarding && observed)
				{
					c.knownWaitSeconds = queueSeconds;
					c.crowdingUnits = density;
				}
				break; // Automatic: no button motion or interaction premiums.
			}
			if (airlock)
			{
				// Only the threshold slows for Crawling, not the Standing walk
				// to the outside control or its interaction duration.
				c.motionSeconds += controlApproachLength / context.walkSpeed + interactionSeconds;
				c.expectedWaitSeconds = airlockCycleSeconds;
				if (observed && boarding)
				{
					c.knownWaitSeconds = preparationSeconds + queueSeconds;
					c.crowdingUnits = density;
				}
				else c.expectedWaitSeconds += preparationSeconds
					+ (boarding ? p.unobservedDoorQueueSeconds : 0);
				c.interactionUnits = p.thresholdInteraction
					+ (needsActivation ? p.remoteDoorInteraction : 0);
				break;
			}
			if (shuttle)
			{
				c.motionSeconds += boarding ? p.shuttleBoardingSeconds : p.shuttleAlightingSeconds;
				c.interactionUnits = p.thresholdInteraction + (boarding ? p.shuttleBoardingInteraction : p.shuttleAlightingInteraction);
				if (boarding)
				{
					auto passengers = observed ? static_cast<float>(shuttle->queuedAgents) : p.shuttleExpectedQueuePassengers;
					c.expectedWaitSeconds = p.shuttleHeadwaySeconds * (0.5f + passengers / shuttle->capacity);
					c.crowdingUnits = observed ? passengers / shuttle->capacity : p.shuttleExpectedCrowdingUnits;
					if (observed) c.knownWaitSeconds = open ? 0.0f : preparationSeconds;
					else c.expectedWaitSeconds += p.unobservedDoorClosedProbability * preparationSeconds;
				}
				break;
			}
			auto const probability = observed ? (open ? 0.0f : 1.0f) : p.unobservedDoorClosedProbability;
			if (observed)
			{
				c.knownWaitSeconds = probability * preparationSeconds;
				if (lift && boarding)
				{
					c.expectedWaitSeconds = p.liftExpectedWaitSeconds;
					c.knownWaitSeconds += p.liftQueueServiceSeconds * static_cast<float>(lift->queuedAgents) / std::max(1u, lift->capacity);
					c.crowdingUnits = static_cast<float>(lift->queuedAgents) / std::max(1u, lift->capacity);
				}
				else if (!lift) { c.knownWaitSeconds += queueSeconds; c.crowdingUnits = density; }
			}
			else
			{
				c.expectedWaitSeconds = probability * preparationSeconds + p.unobservedDoorQueueSeconds;
				if (lift && boarding)
				{
					c.expectedWaitSeconds += p.liftExpectedWaitSeconds + p.liftQueueServiceSeconds * p.liftExpectedQueuePassengers / std::max(1u, lift->capacity);
					c.crowdingUnits = p.liftExpectedCrowdingUnits;
				}
			}
			c.interactionUnits = p.thresholdInteraction;
			if (lift && boarding)
			{
				c.motionSeconds += p.liftBoardingSeconds;
				c.expectedWaitSeconds += lift->minimumDwellSeconds;
				c.interactionUnits += p.liftCallBoardingInteraction;
			}
			else if (lift) { c.motionSeconds += p.liftAlightingSeconds; c.interactionUnits += p.liftAlightingInteraction; }
			else if (activation == 1) c.interactionUnits += probability * p.manualDoorInteraction;
			else if (activation == 2) c.interactionUnits += probability * p.remoteDoorInteraction;
			break;
		}
		default: throw std::logic_error("Missing captured traversal evaluator");
		}
		facts.objectiveDurationSeconds = c.motionSeconds + c.knownWaitSeconds + c.expectedWaitSeconds;
		return facts;
	}
}
