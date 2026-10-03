#include "core/RouteTraversalInputs.h"
#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/DoorEdge.h"
#include "core/BulkheadDoorEdge.h"
#include "core/AirlockTransit.h"
#include "core/SecurityScannerTransit.h"
#include "core/LadderEdge.h"
#include "core/LadderMountEdge.h"
#include "core/StaircaseEdge.h"
#include "core/StaircaseMountEdge.h"
#include "core/LiftEdge.h"
#include "core/LiftMountEdge.h"
#include "core/ShuttleEdge.h"
#include "core/ForceBridgeEdge.h"
#include "core/WindowEdge.h"

namespace core
{
	RouteTraversalInputs RouteTraversalInputs::capture(Edge const& edge,
		std::shared_ptr<const Vertex> const& target, RouteDecisionContext const& context)
	{
		RouteTraversalInputs result;
		result.type = edge.getType();
		result.length = edge.getLength();
		result.rise = edge.getDirectedRise(*target);
		result.buttons = edge.requiresButton();
		auto const source = edge.getOtherVertex(target);
		auto const sector = source->getSector();
		auto const sourceSector = sector ? SectorId{ static_cast<uint64_t>(sector->getIndex()) + 1 } : SectorId{};
		result.observed = sector && sector.get() == context.observationSector;
		auto visibleEntry = [&](EdgeType mount)
		{
			if (!context.observationSector) return false;
			for (auto const& adjacent : source->getEdges())
				if (adjacent->getType() == mount
					&& adjacent->getOtherVertex(source)->getSector().get() == context.observationSector) return true;
			return false;
		};
		bool frozenExtended = false;
		auto extension = [&](auto const& device)
		{
			auto known = device.knownCondition(context.agent, edge.getTraversalResourceId(), context.observationSector, result.observed);
			frozenExtended = known && known->broken && known->position >= 1.0f;
			if (known && !known->admitsPassage()) result.exclusion = RouteExclusionReason::Control;
			result.extensible = device.isExtensible();
			result.preparationSeconds = device.getExtendRetractTime();
			if (result.observed && result.extensible)
			{
				result.extended = device.isExtended();
				result.extendedPercentage = device.getExtendedPercentage();
				result.needsActivation = device.isRetracted() || device.isRetracting();
			}
			if (frozenExtended)
			{
				result.extended = true;
				result.extendedPercentage = 1.0f;
				result.needsActivation = false;
			}
		};
		switch (result.type)
		{
		case EdgeType::Location: break;
		case EdgeType::Gap:
			result.exclusion = RouteExclusionReason::NoContinuation;
			break;
		case EdgeType::Window:
		{
			auto const& window = static_cast<WindowEdge const&>(edge).mWindow;
			if (!window || !window->isNormallyTraversable()) result.exclusion = RouteExclusionReason::Control;
			break;
		}
		case EdgeType::Stairwell: case EdgeType::StairwellMount:
			result.mobilityKind = TraversalKind::Stairwell;
			break;
		case EdgeType::Staircase: case EdgeType::StaircaseMount:
		{
			auto const& staircase = result.type == EdgeType::Staircase
				? static_cast<StaircaseEdge const&>(edge).mStaircase
				: static_cast<StaircaseMountEdge const&>(edge).mStaircase;
			bool local = result.observed || target->getSector().get() == context.observationSector;
			if (result.type == EdgeType::Staircase)
			{
				local = local || visibleEntry(EdgeType::StaircaseMount);
				for (auto const& adjacent : target->getEdges())
					if (adjacent->getType() == EdgeType::StaircaseMount
						&& adjacent->getOtherVertex(target)->getSector().get() == context.observationSector) local = true;
			}
			result.escalator = staircase->routeIsMoving(context.agent, local);
			result.mobilityKind = result.escalator ? TraversalKind::Escalator : TraversalKind::Staircase;
			if (result.type == EdgeType::StaircaseMount && context.agent && context.agent->getSector()
				&& context.agent->getSector()->getIndex() == staircase->getSectorIndex()
				&& target->getSector().get() != context.agent->getSector()) result.mobilityKind.reset();
			result.speed = staircase->getSpeed();
			if (result.type == EdgeType::Staircase && result.escalator)
			{
				if ((result.rise > 0) != (result.speed > 0)) result.exclusion = RouteExclusionReason::Direction;
				result.observed = context.agent && visibleEntry(EdgeType::StaircaseMount);
				if (result.observed) result.standingAgents = context.agent->countObservedStandingEscalatorAgents(&edge);
			}
			break;
		}
		case EdgeType::Ladder: case EdgeType::LadderMount:
		{
			auto const& ladder = result.type == EdgeType::Ladder
				? static_cast<LadderEdge const&>(edge).mLadder
				: static_cast<LadderMountEdge const&>(edge).mLadder;
			result.mobilityKind = TraversalKind::Ladder;
			SectorId approachSector = sourceSector;
			if (result.type == EdgeType::Ladder)
			{
				result.observed = result.observed || visibleEntry(EdgeType::LadderMount);
				for (auto const& adjacent : source->getEdges())
					if (adjacent->getType() == EdgeType::LadderMount)
					{
						auto approach = adjacent->getOtherVertex(source)->getSector();
						if (approach) approachSector = SectorId{
							static_cast<uint64_t>(approach->getIndex()) + 1 };
						break;
					}
			}
			extension(*ladder);
			if (result.exclusion == RouteExclusionReason::Control) break;
			if (result.type == EdgeType::Ladder && ladder->isExtensible()
				&& context.world && context.agent
				&& ((result.extended
					&& !context.world->agentAdheresToExtensiblePermission(
						ladder->getTraversalResourceId(), approachSector, source->getPosition(),
						context.world->getAgentId(context.agent)))
					|| (!result.extended
						&& !context.world->canAgentOperateExtensibleControl(
							ladder->getTraversalResourceId(), approachSector, source->getPosition(),
							context.world->getAgentId(context.agent)))))
				result.exclusion = RouteExclusionReason::Permission;
			if (result.type == EdgeType::LadderMount && ladder->isExtensible() && !frozenExtended
				&& source->getType() != VertexType::Ladder && !ladder->hasExtensionControlInSector(sourceSector))
				result.exclusion = RouteExclusionReason::PreparationSide;
			break;
		}
		case EdgeType::ForceBridge:
		{
			auto const& bridge = static_cast<ForceBridgeEdge const&>(edge).mForceBridge;
			extension(*bridge);
			if (result.exclusion == RouteExclusionReason::Control) break;
			if (bridge->isExtensible() && !frozenExtended && (!bridge->hasExtensionControlInSector(sourceSector)
				|| !bridge->canPrepareFromPosition(source->getPosition().x))) result.exclusion = RouteExclusionReason::PreparationSide;
			else if (bridge->isExtensible() && context.world && context.agent
				&& ((result.extended
					&& !context.world->agentAdheresToExtensiblePermission(
						bridge->getTraversalResourceId(), sourceSector, source->getPosition(),
						context.world->getAgentId(context.agent)))
					|| (!result.extended
						&& !context.world->canAgentOperateExtensibleControl(
							bridge->getTraversalResourceId(), sourceSector, source->getPosition(),
							context.world->getAgentId(context.agent)))))
				result.exclusion = RouteExclusionReason::Permission;
			break;
		}
		case EdgeType::Lift: case EdgeType::LiftMount:
		{
			auto const& lift = result.type == EdgeType::Lift
				? static_cast<LiftEdge const&>(edge).mLift : static_cast<LiftMountEdge const&>(edge).mLift;
			auto known = context.world ? context.world->knownTransportCondition(
				edge.getTraversalResourceId(), context.agent, context.observationSector) : std::nullopt;
			if (known && known->broken)
			{
				result.exclusion = RouteExclusionReason::Control;
				return result;
			}
			result.mobilityKind = lift->isOpenPlatformLift() ? TraversalKind::PlatformLift : TraversalKind::Lift;
			result.speed = lift->getSpeed();
			result.dwell = lift->getRouteMinimumDwellSeconds();
			result.capacity = std::max(1u, lift->getRouteCapacity());
			result.boarding = !target->getObject();
			if (result.type == EdgeType::LiftMount && result.boarding && context.agent
				&& context.agent->getSector() && result.observed
				&& std::abs(context.agent->getGlobalPosition().y - source->getPosition().y) <= 0.5f)
				result.lift = context.agent->observeLiftAccess(edge.getTraversalResourceId(), source->getPosition());
			auto const routeAgent = context.world && context.agent
				? context.world->getAgentId(context.agent) : AgentId{};
			if (result.type == EdgeType::LiftMount && lift->isOpenPlatformLift()
				&& !result.boarding && routeAgent
				&& !context.world->canAgentUseLiftJourney(edge.getTraversalResourceId(),
					context.agent->getGlobalPosition(), source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			// Platform passengers share their Room's Sector. After a paused edit,
			// source inference may include the co-located mount adapter again;
			// continuing an onboard journey is not a new landing admission.
			bool const continuingPlatformJourney = routeAgent && lift->isOpenPlatformLift()
				&& std::abs(context.agent->getGlobalPosition().y - source->getPosition().y) <= 0.5f
				&& context.world->isAgentTransportOccupant(edge.getTraversalResourceId(), routeAgent);
			if (result.type == EdgeType::LiftMount && lift->isOpenPlatformLift()
				&& result.boarding && routeAgent && !continuingPlatformJourney
				&& !(result.observed
					&& std::abs(context.agent->getGlobalPosition().y - source->getPosition().y) <= 0.5f
					&& context.world->isTransportLocallyBoardable(
					edge.getTraversalResourceId(), source->getPosition())
					&& context.world->agentAdheresToTransportLandingPermission(
						edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				&& !context.world->canAgentOperateTransportLandingControl(
					edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			break;
		}
		case EdgeType::ShuttleMount:
		{
			result.mobilityKind = TraversalKind::Shuttle;
			auto known = context.world ? context.world->knownTransportCondition(
				edge.getTraversalResourceId(), context.agent, context.observationSector) : std::nullopt;
			if (known && known->broken) result.exclusion = RouteExclusionReason::Control;
			break;
		}
		case EdgeType::Shuttle:
		{
			result.mobilityKind = TraversalKind::Shuttle;
			auto known = context.world ? context.world->knownTransportCondition(
				edge.getTraversalResourceId(), context.agent, context.observationSector) : std::nullopt;
			if (known && known->broken)
			{
				result.exclusion = RouteExclusionReason::Control;
				return result;
			}
			result.speed = static_cast<ShuttleEdge const&>(edge).mShuttle->getSpeed();
			auto observe = [&](Vector2 endpoint)
			{
				return context.world ? context.world->observeShuttleAccess(edge.getTraversalResourceId(), endpoint, false)
					: context.agent ? context.agent->observeShuttleAccess(edge.getTraversalResourceId(), endpoint, false) : std::nullopt;
			};
			auto from = observe(source->getPosition()), to = observe(target->getPosition());
			result.shuttleStopsKnown = from && to;
			if (result.shuttleStopsKnown)
			{
				result.shuttleDistance = std::abs(to->stopPosition - from->stopPosition);
				result.dwell = from->minimumDwellSeconds;
			}
			break;
		}
		case EdgeType::Door: case EdgeType::BulkheadDoor:
		{
			Door const& door = result.type == EdgeType::Door
				? *static_cast<DoorEdge const&>(edge).mDoor : *static_cast<BulkheadDoorEdge const&>(edge).mDoor;
			result.mobilityKind = TraversalKind::Door;
			if (result.type == EdgeType::BulkheadDoor)
				if (auto chamber = static_cast<BulkheadDoorEdge const&>(edge).mSecurityScanner)
				{
					result.securityScanner = true;
					result.boarding = target->getSector().get() == chamber.get();
					auto expectedSource = result.boarding ? chamber->getStop(chamber->getEntrySide()).sector.get() : chamber.get();
					auto expectedTarget = result.boarding ? chamber.get() : chamber->getStop(chamber->getExitSide()).sector.get();
					if (sector.get() != expectedSource || target->getSector().get() != expectedTarget)
						result.exclusion = RouteExclusionReason::Control;
					if (result.boarding && context.world && context.agent
						&& !context.world->canAgentAccessLocation(*chamber->getStop(chamber->getExitSide()).sector, *context.agent))
						result.exclusion = RouteExclusionReason::Permission;
					result.preparationSeconds = CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
					if (!result.boarding)
						result.preparationSeconds += CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME
							+ chamber->getPreDelaySeconds() + chamber->getScanSeconds() + chamber->getPostPauseSeconds();
					break;
				}
			if (result.type == EdgeType::BulkheadDoor)
				if (auto chamber = static_cast<BulkheadDoorEdge const&>(edge).mAirlock)
				{
					result.airlock = true;
					result.boarding = target->getSector().get() == chamber.get();
					int side = sector.get() == chamber->getStop(0).sector.get() ? 0 : 1;
					result.open = result.boarding && result.observed && door.isOpen();
					result.needsActivation = !result.open;
					result.preparationSeconds = result.open ? 0 : CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
					if (!result.boarding)
					{
						result.preparationSeconds += CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
						result.airlockCycleSeconds = chamber->getCycleSeconds();
					}
					if (context.world && context.agent && result.boarding
						&& !context.world->canAgentEnterAirlock(edge.getTraversalResourceId(),
							sourceSector, context.world->getAgentId(context.agent), result.observed))
						result.exclusion = RouteExclusionReason::Permission;
					if (result.needsActivation)
					{
						auto buttonX = result.boarding ? source->getPosition().x
							: chamber->getCellX() + (chamber->getCellsWide() - 1) / 2 + 0.5f;
						result.interactionSeconds = World::getFixedTimestep();
						if (context.world)
							if (auto point = context.world->lookupInteractionPoint(chamber->getControl(result.boarding ? side : 2)); point)
							{
								buttonX = point.entity->getPosition().x;
								result.interactionSeconds = point.entity->getDurationTicks() * World::getFixedTimestep();
							}
						result.length += 2 * std::abs(source->getPosition().x - buttonX);
					}
					if (result.boarding && result.observed && context.agent)
					{
						result.queueSeconds = context.agent->estimateTraversalDelay(edge.getTraversalResourceId(), sourceSector);
						result.density = context.agent->observeAccessZoneDensity(edge.getTraversalResourceId(), sourceSector);
					}
					break;
				}
			if (door.isSecurityScannerOwned())
			{
				result.exclusion = RouteExclusionReason::Control;
				break; // An ordinary Bulkhead edge cannot stand in for the journey authority.
			}
			result.preparationSeconds = result.type == EdgeType::Door ? CORE_DOOR_OPEN_CLOSE_TIME : CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
			result.boarding = sector && isLocationLike(sector->getType());
			result.lift = context.agent ? context.agent->observeLiftAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed) : std::nullopt;
			result.shuttle = context.world ? context.world->observeShuttleAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed)
				: context.agent ? context.agent->observeShuttleAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed) : std::nullopt;
			auto liftCondition = context.world ? context.world->knownTransportCondition(
				edge.getTraversalResourceId(), context.agent, context.observationSector) : std::nullopt;
			if (liftCondition && liftCondition->broken && (result.boarding
				|| !liftCondition->atStop || !liftCondition->doorsOpen
				|| std::abs(liftCondition->position - (result.shuttle ? result.shuttle->stopPosition
					: source->getPosition().y)) > 0.001f))
			{
				result.exclusion = RouteExclusionReason::Control;
				return result;
			}
			// Unavailable doors retain the existing hard permission rule; otherwise
			// remote openness is deliberately not even read.
			auto known = door.knownCondition(context.agent, context.observationSector);
			bool const brokenOpen = known && known->broken && known->position >= 1.0f;
			result.open = brokenOpen || ((result.observed
				|| (!door.isBreakable() && door.getActivationMode() == DoorActivationMode::Unavailable)) && door.isOpen());
			if (known && !known->admitsPassage())
			{
				result.exclusion = RouteExclusionReason::Control;
				return result; // Operation permissions cannot make a frozen aperture usable.
			}
			// A frozen open threshold needs no preparation even when its last
			// observation is remote. Do not turn memory into live queue visibility.
			if (brokenOpen) result.preparationSeconds = 0.0f;
			if (door.getActivationMode() == DoorActivationMode::Unavailable && !result.open && !result.lift && !result.shuttle)
				result.exclusion = RouteExclusionReason::Control;
			// The Door's live state is usable only when this threshold is local. A
			// remote protected Door therefore remains infeasible for an unauthorized
			// Agent even if another Agent currently has it open.
			if (result.type == EdgeType::Door && door.getActivationMode() == DoorActivationMode::Manual
				&& !result.open && context.world && context.agent
				&& !context.world->canAgentOpenManualDoor(door.getTraversalResourceId(),
					context.world->getAgentId(context.agent)))
				result.exclusion = RouteExclusionReason::Permission;
			if (door.getActivationMode() == DoorActivationMode::RemoteControlled
				&& !result.open && context.world && context.agent
				&& !context.world->canAgentOperateDoorControl(door.getTraversalResourceId(),
					sourceSector, context.world->getAgentId(context.agent)))
				result.exclusion = RouteExclusionReason::Permission;
			if (result.type == EdgeType::Door && result.open && context.world && context.agent
				&& !context.world->agentAdheresToDoorPermission(door.getTraversalResourceId(),
					sourceSector, context.world->getAgentId(context.agent)))
				result.exclusion = RouteExclusionReason::Permission;
			auto const routeAgent = context.world && context.agent
				? context.world->getAgentId(context.agent) : AgentId{};
			// A Lift body is segmented at intermediate Stops. Test the actual
			// disembark destination, not each segment passed on the way there.
			if (!result.boarding && (result.lift || result.shuttle) && routeAgent
				&& !context.world->canAgentUseLiftJourney(edge.getTraversalResourceId(),
					context.agent->getGlobalPosition(), source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			if (result.boarding && (result.lift || result.shuttle) && routeAgent
				&& !(result.observed
					&& std::abs(context.agent->getGlobalPosition().y - source->getPosition().y) <= 0.5f
					&& context.world->isTransportLocallyBoardable(
					edge.getTraversalResourceId(), source->getPosition())
					&& context.world->agentAdheresToTransportLandingPermission(
						edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				&& !context.world->canAgentOperateTransportLandingControl(
					edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			if (!brokenOpen && door.getActivationMode() == DoorActivationMode::Manual) result.activation = 1;
			else if (!brokenOpen && door.getActivationMode() == DoorActivationMode::RemoteControlled) result.activation = 2;
			if (result.observed && context.agent && !result.lift && !result.shuttle)
			{
				result.queueSeconds = context.agent->estimateTraversalDelay(edge.getTraversalResourceId(), sourceSector);
				result.density = context.agent->observeAccessZoneDensity(edge.getTraversalResourceId(), sourceSector);
			}
			break;
		}
		default: throw std::logic_error("Missing traversal input capture");
		}
		return result;
	}
}
