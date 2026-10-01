#include "core/RouteTraversalInputs.h"
#include "core/Agent.h"
#include "core/Defines.h"
#include "core/Vertex.h"
#include "core/World.h"
#include "core/DoorEdge.h"
#include "core/BulkheadDoorEdge.h"
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
		auto extension = [&](auto const& device)
		{
			result.extensible = device.isExtensible();
			result.preparationSeconds = device.getExtendRetractTime();
			if (result.observed && result.extensible)
			{
				result.extended = device.isExtended();
				result.extendedPercentage = device.getExtendedPercentage();
				result.needsActivation = device.isRetracted() || device.isRetracting();
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
			result.escalator = staircase->isEscalator();
			result.mobilityKind = result.escalator ? TraversalKind::Escalator : TraversalKind::Staircase;
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
				result.observed = visibleEntry(EdgeType::LadderMount);
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
			if (result.type == EdgeType::Ladder && ladder->isExtensible()
				&& !(result.observed && result.extended) && context.world && context.agent
				&& !context.world->canAgentOperateExtensibleControl(
					ladder->getTraversalResourceId(), approachSector,
					context.world->getAgentId(context.agent)))
				result.exclusion = RouteExclusionReason::Permission;
			if (result.type == EdgeType::LadderMount && ladder->isExtensible()
				&& source->getType() != VertexType::Ladder && !ladder->hasExtensionControlInSector(sourceSector))
				result.exclusion = RouteExclusionReason::PreparationSide;
			break;
		}
		case EdgeType::ForceBridge:
		{
			auto const& bridge = static_cast<ForceBridgeEdge const&>(edge).mForceBridge;
			extension(*bridge);
			if (bridge->isExtensible() && (!bridge->hasExtensionControlInSector(sourceSector)
				|| !bridge->canPrepareFromPosition(source->getPosition().x))) result.exclusion = RouteExclusionReason::PreparationSide;
			else if (bridge->isExtensible() && !(result.observed && result.extended)
				&& context.world && context.agent
				&& !context.world->canAgentOperateExtensibleControl(
					bridge->getTraversalResourceId(), sourceSector,
					context.world->getAgentId(context.agent)))
				result.exclusion = RouteExclusionReason::Permission;
			break;
		}
		case EdgeType::Lift: case EdgeType::LiftMount:
		{
			auto const& lift = result.type == EdgeType::Lift
				? static_cast<LiftEdge const&>(edge).mLift : static_cast<LiftMountEdge const&>(edge).mLift;
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
			if (result.type == EdgeType::LiftMount && lift->isOpenPlatformLift()
				&& result.boarding && routeAgent
				&& !(result.observed && context.world->isTransportLocallyBoardable(
					edge.getTraversalResourceId(), source->getPosition()))
				&& !context.world->canAgentOperateTransportLandingControl(
					edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			break;
		}
		case EdgeType::ShuttleMount:
			result.mobilityKind = TraversalKind::Shuttle;
			break;
		case EdgeType::Shuttle:
		{
			result.mobilityKind = TraversalKind::Shuttle;
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
			result.preparationSeconds = result.type == EdgeType::Door ? CORE_DOOR_OPEN_CLOSE_TIME : CORE_BULKHEAD_DOOR_OPEN_CLOSE_TIME;
			result.boarding = sector && isLocationLike(sector->getType());
			result.lift = context.agent ? context.agent->observeLiftAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed) : std::nullopt;
			result.shuttle = context.world ? context.world->observeShuttleAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed)
				: context.agent ? context.agent->observeShuttleAccess(edge.getTraversalResourceId(), source->getPosition(), result.observed) : std::nullopt;
			// Unavailable doors retain the existing hard permission rule; otherwise
			// remote openness is deliberately not even read.
			result.open = (result.observed || door.getActivationMode() == DoorActivationMode::Unavailable) && door.isOpen();
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
			auto const routeAgent = context.world && context.agent
				? context.world->getAgentId(context.agent) : AgentId{};
			// A Lift body is segmented at intermediate Stops. Test the actual
			// disembark destination, not each segment passed on the way there.
			if (!result.boarding && result.lift && routeAgent
				&& !context.world->canAgentUseLiftJourney(edge.getTraversalResourceId(),
					context.agent->getGlobalPosition(), source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			if (result.boarding && (result.lift || result.shuttle) && routeAgent
				&& !(result.observed && context.world->isTransportLocallyBoardable(
					edge.getTraversalResourceId(), source->getPosition()))
				&& !context.world->canAgentOperateTransportLandingControl(
					edge.getTraversalResourceId(), sourceSector, source->getPosition(), routeAgent))
				result.exclusion = RouteExclusionReason::Permission;
			if (door.getActivationMode() == DoorActivationMode::Manual) result.activation = 1;
			else if (door.getActivationMode() == DoorActivationMode::RemoteControlled) result.activation = 2;
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
