#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Button.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Location.h"
#include "core/DoorEdge.h"
#include "core/BulkheadDoorEdge.h"
#include "core/AirlockTransit.h"
#include "core/ChamberTransit.h"
#include "core/Window.h"
#include "core/AccessPanel.h"
#include "core/Graph.h"
#include "core/MobilityProfile.h"
#include "core/StairwellMountEdge.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/LadderMountEdge.h"
#include "core/LadderEdge.h"
#include "core/LadderVertex.h"
#include "core/SectorEdge.h"
#include "core/LiftMountEdge.h"
#include "core/Lift.h"
#include "core/Shuttle.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <limits>

namespace core
{
	namespace
	{
		// Signed speed along the flight: positive is ascent, regardless of its
		// horizontal orientation. Moving steps carry even a non-walking Player.
		float playerStaircaseSpeed(Staircase const& stairs, Agent const& agent, int horizontal)
		{
			auto const path = stairs.getPath();
			int const direction = horizontal * (path[1].x > path[0].x ? 1 : -1);
			auto const ratio = agent.getPhysicalBaseline().automaticSpeedRatio(
				AutomaticPoseContext::RoomMovement, agent.getPose()).value_or(0.f);
			if (stairs.isMoving()) return stairs.getSpeed() + direction * agent.getWalkSpeed() * ratio;
			return direction * agent.getStationaryStairSpeed(direction > 0) * ratio;
		}
	}

	AgentId World::getPlayerControlledAgent() const
	{
		for (auto const& [id, agent] : mAgents.entries())
			if (agent && agent->isPlayerControlled()) return id;
		return {};
	}

	void World::setPlayerMovementDirection(int direction)
	{
		mPlayerMovementDirection = isSimulationPaused() ? 0 : std::clamp(direction, -1, 1);
	}

	float World::maximumInstalledStandingWidth() const
	{
		float width = bundledHumanBaseline().width;
		for (auto const& [id, definition] : mAgentTypes)
			if (auto baseline = registeredAgentTypeBaseline(id, definition->resourceName))
				if (baseline->supportsPose(Pose::Standing)) width = std::max(width, baseline->width);
		return width;
	}

	void World::setPlayerStairSelection(bool up, bool down)
	{
		mPlayerStairUp = !isSimulationPaused() && up;
		mPlayerStairDown = !isSimulationPaused() && down;
	}

	void World::setPlayerCrouch(bool held)
	{
		mPlayerCrouch = !isSimulationPaused() && held;
	}

	void World::clearPlayerInput()
	{
		mPlayerMovementDirection = 0;
		mPlayerDoorAction = {};
		mPlayerStairUp = mPlayerStairDown = mPlayerCrouch = false;
	}

	void World::requestPlayerDoorAction()
	{
		if (!isSimulationPaused()) mPlayerDoorAction = getPlayerControlledAgent();
	}

	void World::performPlayerDoorAction()
	{
		auto const id = std::exchange(mPlayerDoorAction, AgentId{});
		auto* agent = mAgents.find(id);
		if (!agent || id != getPlayerControlledAgent() || !agent->isActive() || !agent->getSector()) return;
		auto const* stairSector = dynamic_cast<StairwellTransit const*>(agent->getSector());
		bool const flatTask = stairSector && agent->mTraversalTask
			&& agent->mTraversalTask->edge->getType() == EdgeType::Location;
		if (agent->mTraversalTask && !flatTask) return;
		if (isLocationLike(agent->getSector()->getType()))
			updatePlayerPose(*agent, agent->getSector()->getPosition().y + agent->getSector()->getLevelsHigh() - 1
				+ agent->getSector()->getEffectiveTopLevelHeight() - agent->getGlobalPosition().y,
				-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
		else if (stairSector) updatePlayerClimbingPose(*agent);
		// Held climb selection is contextual: approaching a stair or Ladder
		// endpoint must not simultaneously activate a neighbouring Door.
		if (agent->mPlayerLadderMount || playerLadderApproach(*agent) || playerStaircaseApproach(*agent)) return;
		auto const position = agent->getGlobalPosition();
		// Holding Up while approaching a junction selects at departure; it must
		// not accidentally exit before arrival. Walking away (or stopping) retains
		// fresh Up's doorway meaning, even midway along a flat platform edge.
		if (stairSector && agent->mPlayerStairPlatform && mPlayerMovementDirection && mPlayerStairUp)
		{
			auto stairs = stairSector->getStairwell();
			auto const level = static_cast<uint32_t>(std::max(0.f, std::floor(position.y - stairs->getPosition().y)));
			for (auto const& platform : stairs->getLandingPaths(level))
			{
				auto junction = stairs->getPosition() + platform[0], endpoint = stairs->getPosition() + platform[1];
				if (std::abs(position.y - junction.y) <= PoseFitTolerance
					&& position.x >= std::min(junction.x, endpoint.x) - PoseFitTolerance
					&& position.x <= std::max(junction.x, endpoint.x) + PoseFitTolerance
					&& (std::abs(position.x - junction.x) <= PoseFitTolerance
						|| mPlayerMovementDirection * (junction.x - position.x) > 0.f)) return;
			}
		}
		struct Candidate
		{
			std::shared_ptr<const Edge> edge;
			std::shared_ptr<const Vertex> source, destination;
			std::optional<PoseSelection> pose;
			float distance;
			bool passable, openable;
		};
		std::vector<Candidate> candidates;
		for (auto const& edge : getGraph()->getEdges())
		{
			auto ordinary = std::dynamic_pointer_cast<DoorEdge const>(edge);
			if (!ordinary)
			{
				auto mount = std::dynamic_pointer_cast<StairwellMountEdge const>(edge);
				if (!mount) continue;
				auto source = edge->getVertex(0), destination = edge->getVertex(1);
				if (source->getSector().get() != agent->getSector()) std::swap(source, destination);
				if (source->getSector().get() != agent->getSector()
					|| std::abs(position.y - source->getPosition().y) > PoseFitTolerance
					|| agentForbidsEdge(agent, *edge, TraversalKind::Stairwell)
					|| !canAgentAccessLocation(*destination->getSector(), *agent)) continue;
				auto const envelope = agent->getPoseEnvelope(agent->getPose());
				auto const stairs = mount->getStairwell();
				if (!envelope || !agent->poseFits(agent->getPose(), CORE_DOOR_HEIGHT)
					|| position.x - envelope->x * .5f < stairs->getPosition().x + CORE_DOOR_X_INSET - PoseFitTolerance
					|| position.x + envelope->x * .5f > stairs->getPosition().x + stairs->getSize().x - CORE_DOOR_X_INSET + PoseFitTolerance)
					continue;
				bool fit = true;
				for (auto const& sector : {source->getSector(), destination->getSector()})
					fit = fit && agent->poseFits(agent->getPose(), sector->getPosition().y + sector->getLevelsHigh() - 1
						+ sector->getEffectiveTopLevelHeight() - position.y);
				if (fit) candidates.push_back({edge, source, destination, PoseSelection{agent->getPose(), agent->getPhysicalBaseline().automaticSpeedRatio(
					AutomaticPoseContext::RoomMovement, agent->getPose()).value_or(0.f)},
					std::abs(position.x - source->getPosition().x), true, false});
				continue;
			}
			auto door = ordinary->getDoor();
			bool const transport = door->isLiftOwned() || door->isShuttleOwned();
			if (typeid(*door) != typeid(Door) || !door->getFrontSector() || !door->getBackSector()
				|| (!transport && (!isLocationLike(door->getFrontSector()->getType())
					|| !isLocationLike(door->getBackSector()->getType())))) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (source->getSector().get() != agent->getSector()) std::swap(source, destination);
			if (source->getSector().get() != agent->getSector()
				|| std::abs(position.y - door->getPosition().y) > PoseFitTolerance
				|| position.x < door->getPosition().x
				|| position.x > door->getPosition().x + door->getSize().x
				|| agentForbidsEdge(agent, *edge, transport
					? (door->isLiftOwned() ? TraversalKind::Lift : TraversalKind::Shuttle)
					: TraversalKind::Door)) continue;
			auto resource = mTraversalResources.find(door->getTraversalResourceId());
			if (!resource || !resource->mEnabled) continue;
			auto const halfWidth = agent->getWidth() * .5f;
			auto const pose = door->selectAgentCrossing(*agent, position.y, true,
				transport ? std::nullopt : std::optional<float>{door->getOpenPercentage()});
			auto const currentPoseRoom = [&](Sector const& sector) {
				return agent->poseFits(agent->getPose(), sector.getPosition().y + sector.getLevelsHigh() - 1
					+ sector.getEffectiveTopLevelHeight() - position.y);
			};
			bool const passable = (transport ? door->admitsNewCrossings() : door->admitsPlayerCrossing(*agent, position.y)) && pose
				&& currentPoseRoom(*source->getSector()) && currentPoseRoom(*destination->getSector())
				&& position.x - halfWidth >= door->getPosition().x - PoseFitTolerance
				&& position.x + halfWidth <= door->getPosition().x + door->getSize().x + PoseFitTolerance
				&& canAgentAccessLocation(*destination->getSector(), *agent)
				&& (!transport || !isLocationLike(destination->getSector()->getType())
					|| playerTransportLandingSupported(*destination->getSector(), position, halfWidth))
				&& agentAdheresToDoorPermission(door->getTraversalResourceId(),
					SectorId{uint64_t(agent->getSector()->getIndex()) + 1}, id);
			// Player geometry replaces distance, not capability or authorization.
			bool const openable = !transport && door->getActivationMode() == DoorActivationMode::Manual
				&& !door->isBroken() && !door->isOpen() && !door->isOpening()
				&& agentCanOperateObjects(*agent) && agentSatisfiesDoorPermission(*door, *agent);
			if (passable || openable) candidates.push_back({ordinary, source, destination, pose,
				std::abs(position.x - (door->getPosition().x + door->getSize().x * .5f)), passable, openable});
		}
		std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) {
			if (a.passable != b.passable) return a.passable > b.passable;
			if (a.distance != b.distance) return a.distance < b.distance;
			if (a.edge->getTraversalResourceId() != b.edge->getTraversalResourceId())
				return a.edge->getTraversalResourceId() < b.edge->getTraversalResourceId();
			return a.edge->getId() < b.edge->getId();
		});
		for (auto const& candidate : candidates)
		{
			if (!candidate.passable) continue;
			agent->mPathStartPosition = position;
			auto request = createTraversalRequest(*agent, candidate.edge, candidate.source, candidate.destination);
			allocateTraversalRequest(request, candidate.edge, candidate.destination);
			auto lookup = lookupTraversalRequest(request);
			if (!lookup || lookup.entity->getState() != TraversalRequestState::Granted)
			{
				cancelTraversal(request, {});
				releaseTraversal(request, {});
				continue; // Never auto-align, queue movement, or bypass another Agent's lane.
			}
			Agent::TraversalTask task;
			task.edge = candidate.edge;
			task.sourceVertex = candidate.source;
			task.destinationVertex = candidate.destination;
			task.request = request;
			task.permit = lookup.entity->getPermit();
			task.crossingPose = candidate.pose;
			task.traversalTicksRemaining = Door::crossingDurationTicks(task.crossingPose->speedRatio);
			if (flatTask) agent->cancelTraversal();
			finishFurnitureUse(id);
			agent->mOccupiedUsablePoint = {};
			agent->mRetainedActionPose = false;
			agent->mPlayerPose = candidate.pose->pose;
			agent->mPose = candidate.pose->pose;
			if (candidate.edge->getType() == EdgeType::StairwellMount)
			{
				agent->mPlayerStairPlatform = true;
				agent->mPlayerStairApproach = 0;
			}
			agent->mTraversalTask = std::move(task);
			agent->mState = Agent::State::TraversingEdge;
			return;
		}
		for (auto const& candidate : candidates)
			if (candidate.openable)
			{
				static_cast<DoorEdge const&>(*candidate.edge).getDoor()->requestOpen();
				return;
			}
		// Physical Buttons and owned BoothWindow/Access panel controls are the last
		// meaning of Up. Use physical reach, never remote range or auto-approach.
		if (agent->getObjectUsage() != ObjectUsage::Arms || agentForbidsButtons(agent)) return;
		InteractionPointId selected;
		float nearest = 0.f;
		for (auto const& [pointId, point] : mInteractionPoints.entries())
		{
			auto placement = physicalControlPlacement(pointId);
			auto booth = lookupBoothWindow(point->mBoothWindowOwner);
			bool boothPanel = booth && !booth->getDumbwaiterOwner() && booth->getPanel() == pointId
				&& booth->getBackSector().get() == agent->getSector();
			bool accessPanelControl = false;
			if (auto panel = lookupAccessPanel(point->mAccessPanelOwner))
				for (auto action : panel->getActions())
					if (panel->getControl(action) == pointId && canRequestAccessPanel(panel->getId(), action, id))
						accessPanelControl = true;
			if ((!placement && !boothPanel && !accessPanelControl) || std::abs(position.y - point->getPosition().y) > PoseFitTolerance
				|| !agentCanOperateInteraction(pointId, *agent, true)
				|| !missingInteractionPermissions(*point, *agent).empty()
				|| isControlTargetBroken(pointId)) continue;
			if (!boothPanel && !accessPanelControl)
			{
				auto button = std::dynamic_pointer_cast<Button>(mSectors[placement->sectorIndex]
					->getObject(placement->objectIndex)->_getObject());
				if (!button || !button->isEnabled()) continue;
			}
			auto const distance = position.distanceTo(point->getPosition());
			if (!selected || distance < nearest || (distance == nearest && pointId < selected))
			{ selected = pointId; nearest = distance; }
		}
		if (selected) requestInteraction(selected, id);
	}

	std::shared_ptr<const Edge> World::playerStaircaseApproach(Agent const& agent) const
	{
		constexpr float approachDistance = .25f;
		int const choice = int(mPlayerStairUp) - int(mPlayerStairDown);
		if (!choice || !mPlayerMovementDirection || agent.mTraversalTask) return {};
		auto const position = agent.getGlobalPosition();
		std::shared_ptr<const Edge> selected;
		float nearest = approachDistance + PoseFitTolerance;
		for (auto const& edge : getGraph()->getEdges())
		{
			if (edge->getType() != EdgeType::StaircaseMount) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (source->getSector().get() != agent.getSector()) std::swap(source, destination);
			if (source->getSector().get() != agent.getSector()) continue;
			auto const* transit = dynamic_cast<StaircaseTransit const*>(destination->getSector().get());
			if (!transit) continue;
			auto const kind = transit->getStaircase()->isMoving()
				? TraversalKind::Escalator : TraversalKind::Staircase;
			auto const path = transit->getStaircase()->getPath();
			bool const lower = std::abs(source->getPosition().y - transit->getPosition().y) <= PoseFitTolerance;
			if ((choice > 0) != lower) continue;
			// Entry must make positive progress onto the flight. An opposing belt
			// may still carry an already-admitted Player backward, but must not
			// admit one whose walking speed cannot overcome it (including equality).
			if (transit->getStaircase()->isMoving()
				&& choice * playerStaircaseSpeed(*transit->getStaircase(), agent, mPlayerMovementDirection) <= 0.f) continue;
			auto const delta = path[lower ? 1 : 0] - path[lower ? 0 : 1];
			auto const gap = mPlayerMovementDirection * (source->getPosition().x - position.x);
			if (mPlayerMovementDirection * delta.x <= 0.f || gap < -PoseFitTolerance
				|| gap > nearest || std::abs(position.y - source->getPosition().y) > PoseFitTolerance
				|| agentForbidsEdge(&agent, *edge, kind)
				|| !canAgentAccessLocation(*destination->getSector(), agent)
				|| !agent.poseFits(agent.getPose(), source->getSector()->getPosition().y
					+ source->getSector()->getLevelsHigh() - 1 + source->getSector()->getEffectiveTopLevelHeight() - position.y)
				|| !agent.poseFits(agent.getPose(), CORE_CORRIDOR_HEIGHT, 0.f, transit->getSize().x)) continue;
			if (!selected || gap < nearest || (gap == nearest && edge->getId() < selected->getId()))
			{ selected = edge; nearest = gap; }
		}
		return selected;
	}

	std::shared_ptr<const Edge> World::playerLadderApproach(Agent const& agent) const
	{
		int const choice = int(mPlayerStairUp) - int(mPlayerStairDown);
		if (!choice || !mPlayerMovementDirection || agent.mTraversalTask || agent.mPlayerLadderMount) return {};
		auto const position = agent.getGlobalPosition();
		std::shared_ptr<const Edge> selected;
		float nearest = .25f + PoseFitTolerance;
		for (auto const& edge : getGraph()->getEdges())
		{
			auto mount = std::dynamic_pointer_cast<LadderMountEdge const>(edge);
			if (!mount || !mount->getLadder()->admitsNewTraversals()) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (!std::dynamic_pointer_cast<LadderVertex const>(destination)) std::swap(source, destination);
			if (source->getSector().get() != agent.getSector()) continue;
			auto const gap = mPlayerMovementDirection * (source->getPosition().x - position.x);
			if (gap < -PoseFitTolerance || gap > nearest
				|| std::abs(position.y - source->getPosition().y) > PoseFitTolerance
				|| agentForbidsEdge(&agent, *edge, TraversalKind::Ladder)
				|| !canAgentAccessLocation(*destination->getSector(), agent)) continue;
			bool fits = true, flight = false;
			for (auto const& sector : {source->getSector(), destination->getSector()})
				fits = fits && agent.poseFits(agent.getPose(), sector->getPosition().y + sector->getLevelsHigh() - 1
					+ sector->getEffectiveTopLevelHeight() - position.y);
			for (auto const& climb : destination->getEdges())
				if (climb->getType() == EdgeType::Ladder
					&& choice * (climb->getOtherVertex(destination)->getPosition().y - position.y) > PoseFitTolerance)
				{
					auto target = climb->getOtherVertex(destination);
					flight = agent.poseFits(agent.getPose(), target->getSector()->getPosition().y
						+ target->getSector()->getLevelsHigh() - 1 + target->getSector()->getEffectiveTopLevelHeight()
						- target->getPosition().y);
				}
			if (!fits || !flight || agent.getWidth() > destination->getSector()->getSize().x) continue;
			if (!selected || gap < nearest || (gap == nearest && edge->getId() < selected->getId()))
			{ selected = edge; nearest = gap; }
		}
		return selected;
	}

	void World::advancePlayerLadderAgent(Agent& agent, float timestep)
	{
		if (agent.mState == Agent::State::AwaitingTraversalCommit) return;
		if (agent.mTraversalTask)
		{
			if (agent.mTraversalTask->edge->getType() == EdgeType::Ladder)
				advancePlayerStairSegment(agent, timestep);
			else agent.update(timestep);
			return;
		}
		auto const ladder = static_cast<LadderMountEdge const&>(*agent.mPlayerLadderMount).getLadder();
		auto const position = agent.getGlobalPosition();
		int const choice = int(mPlayerStairUp) - int(mPlayerStairDown);
		for (auto const& edge : getGraph()->getEdges())
		{
			auto mount = std::dynamic_pointer_cast<LadderMountEdge const>(edge);
			if (!mount || mount->getLadder() != ladder) continue;
			auto source = edge->getVertex(1), destination = edge->getVertex(0);
			if (!std::dynamic_pointer_cast<LadderVertex const>(source)) std::swap(source, destination);
			if (position.distanceTo(source->getPosition()) > PoseFitTolerance) continue;
			bool const lower = std::abs(source->getPosition().y - (ladder->getPosition().y - CORE_LADDER_HEIGHT_OFF_GROUND)) <= PoseFitTolerance;
			if (!agent.mPlayerLadderEntry || choice * (lower ? 1 : -1) <= 0)
			{
				auto const sector = destination->getSector();
				if (canAgentAccessLocation(*sector, agent) && agent.poseFits(agent.getPose(),
					sector->getPosition().y + sector->getLevelsHigh() - 1
					+ sector->getEffectiveTopLevelHeight() - position.y)
					&& beginPlayerTraversal(agent, edge, source, destination))
				{
					agent.mPlayerLadderMount.reset();
					return;
				}
				if (choice * (lower ? 1 : -1) <= 0) return;
			}
		}
		if (!choice) return;
		for (auto const& edge : getGraph()->getEdges())
		{
			auto climb = std::dynamic_pointer_cast<LadderEdge const>(edge);
			if (!climb || climb->getTraversalResourceId() != ladder->getTraversalResourceId()) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (position.distanceTo(source->getPosition()) > PoseFitTolerance) std::swap(source, destination);
			if (position.distanceTo(source->getPosition()) > PoseFitTolerance
				|| choice * (destination->getPosition().y - position.y) <= 0.f) continue;
			if (beginPlayerTraversal(agent, edge, source, destination))
			{
				agent.mPlayerLadderEntry = false;
				advancePlayerStairSegment(agent, timestep);
			}
			return;
		}
	}

	bool World::beginPlayerTraversal(Agent& agent, std::shared_ptr<const Edge> const& edge,
		std::shared_ptr<const Vertex> const& source, std::shared_ptr<const Vertex> const& destination)
	{
		auto threshold = std::dynamic_pointer_cast<const BulkheadDoorEdge>(edge);
		auto crossingPose = threshold ? threshold->getDoor()->selectPlayerCrossing(agent, agent.getGlobalPosition().y)
			: std::optional<PoseSelection>{};
		if (threshold && !crossingPose) return false;
		agent.mPathStartPosition = agent.getGlobalPosition();
		auto request = createTraversalRequest(agent, edge, source, destination);
		allocateTraversalRequest(request, edge, destination);
		auto lookup = lookupTraversalRequest(request);
		if (!lookup || lookup.entity->getState() != TraversalRequestState::Granted)
		{
			cancelTraversal(request, {}); releaseTraversal(request, {});
			return false;
		}
		Agent::TraversalTask task;
		task.edge = edge; task.sourceVertex = source; task.destinationVertex = destination;
		task.request = request; task.permit = lookup.entity->getPermit();
		finishFurnitureUse(getAgentId(&agent));
		agent.mOccupiedUsablePoint = {};
		task.crossingPose = crossingPose;
		// Retain a lowered threshold pose until a safe post-crossing expansion,
		// including an environmental starting pose without held C input.
		agent.mPlayerPose = crossingPose ? crossingPose->pose : agent.getPose();
		agent.mRetainedActionPose = false;
		agent.mTraversalTask = std::move(task);
		agent.mState = Agent::State::TraversingEdge;
		agent.mPose = *agent.mPlayerPose;
		return true;
	}

	void World::advancePlayerStaircaseAgent(Agent& agent, float timestep)
	{
		auto const* transit = dynamic_cast<StaircaseTransit const*>(agent.getSector());
		if (!transit) return;
		if (getPlayerControlledAgent() != getAgentId(&agent)) return;
		if (agent.mState == Agent::State::AwaitingTraversalCommit) return;
		if (agent.mTraversalTask)
		{
			if (agent.mTraversalTask->edge->getType() == EdgeType::Staircase)
				advancePlayerStairSegment(agent, timestep);
			else agent.update(timestep);
			return;
		}
		auto const position = agent.getGlobalPosition();
		auto const path = transit->getStaircase()->getPath();
		auto const speed = playerStaircaseSpeed(*transit->getStaircase(), agent, mPlayerMovementDirection);
		int const motion = (speed > 0.f ? 1 : speed < 0.f ? -1 : 0) * (path[1].x > path[0].x ? 1 : -1);
		// Endpoints are mounts, not doorways or platforms. Dismount even with
		// no input, including after retracing a flight to its original endpoint.
		for (auto const& edge : getGraph()->getEdges())
		{
			if (edge->getType() != EdgeType::StaircaseMount) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (source->getSector().get() != transit) std::swap(source, destination);
			if (source->getSector().get() != transit
				|| position.distanceTo(source->getPosition()) > PoseFitTolerance) continue;
			// Continue a fresh entry only while net motion points into the flight.
			// Belt motion may overcome input and immediately carry a Player out.
			bool const lower = std::abs(position.y - transit->getPosition().y) <= PoseFitTolerance;
			auto const into = path[lower ? 1 : 0].x - path[lower ? 0 : 1].x;
			if (!agent.mPlayerStaircaseEntry || motion * into <= 0.f)
			{
				if (canAgentAccessLocation(*destination->getSector(), agent)
					&& agent.poseFits(agent.getPose(), destination->getSector()->getPosition().y
						+ destination->getSector()->getLevelsHigh() - 1
						+ destination->getSector()->getEffectiveTopLevelHeight() - position.y))
				{
					beginPlayerTraversal(agent, edge, source, destination);
					return;
				}
				// A refused exit must not prevent retracing the admitted stairs.
				if (motion * into <= 0.f) return;
			}
		}
		if (!motion) return;
		for (auto const& edge : getGraph()->getEdges())
		{
			if (edge->getType() != EdgeType::Staircase || edge->getVertex(0)->getSector().get() != transit) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (motion * (destination->getPosition().x - source->getPosition().x) < 0.f)
				std::swap(source, destination);
			if (beginPlayerTraversal(agent, edge, source, destination))
			{
				agent.mPlayerStaircaseEntry = false;
				advancePlayerStairSegment(agent, timestep);
			}
			return;
		}
	}

	void World::advancePlayerStairwellAgent(Agent& agent, float timestep)
	{
		auto const* sector = dynamic_cast<StairwellTransit const*>(agent.getSector());
		if (!sector) return;
		if (getPlayerControlledAgent() != getAgentId(&agent))
		{
			if (agent.mTraversalTask) agent.update(timestep);
			return;
		}
		if (!mPlayerMovementDirection) return;
		if (agent.mState == Agent::State::AwaitingTraversalCommit) return;
		auto const position = agent.getGlobalPosition();
		if (!agent.mTraversalTask)
		{
			if (agentForbidsTraversal(&agent, TraversalKind::Stairwell)
				|| !agent.poseFits(agent.getPose(), sector->getPosition().y + sector->getLevelsHigh() - 1
					+ sector->getEffectiveTopLevelHeight() - position.y)) return;
			// Up preserves X. On an integer landing, walking reaches the central
			// stair mount explicitly; it never aligns the Agent as a side effect of Up.
			auto const centreX = sector->getPosition().x + 1.f;
			auto const platform = sector->getStairwell()->getLandingPaths(0).front();
			auto const platformLength = std::abs(platform[1].x - platform[0].x);
			if (std::abs(position.y - std::round(position.y)) <= PoseFitTolerance
				&& std::abs(position.x - centreX) > platformLength + PoseFitTolerance)
			{
				auto envelope = agent.getPoseEnvelope(agent.getPose());
				auto const halfWidth = envelope->x * .5f;
				if (envelope->x > sector->getSize().x - 2.f * CORE_DOOR_X_INSET) return;
				auto target = position;
				target.x = std::clamp(position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * timestep,
					sector->getPosition().x + CORE_DOOR_X_INSET + halfWidth,
					sector->getPosition().x + sector->getSize().x - CORE_DOOR_X_INSET - halfWidth);
				if ((position.x - centreX) * (target.x - centreX) <= 0.f) target.x = centreX;
				agent.setPosition({const_cast<StairwellTransit*>(sector), target - sector->getPosition()}, false);
				if (target.x == centreX) agent.mPlayerStairApproach = mPlayerMovementDirection;
				return;
			}
			std::shared_ptr<const Edge> selected;
			std::shared_ptr<const Vertex> source, destination;
			int const choice = int(mPlayerStairUp) - int(mPlayerStairDown);
			bool junction = false;
			for (auto const& edge : getGraph()->getEdges())
				if (edge->getType() == EdgeType::Stairwell && edge->getVertex(0)->getSector().get() == sector)
					for (uint32_t side = 0; side < 2; ++side)
						junction = junction || position.distanceTo(edge->getVertex(side)->getPosition()) <= PoseFitTolerance;
			bool const departure = junction && agent.mPlayerStairPlatform
				&& (choice || agent.mPlayerStairApproach == mPlayerMovementDirection);
			for (auto const& edge : getGraph()->getEdges())
			{
				if ((edge->getType() != EdgeType::Stairwell && edge->getType() != EdgeType::Location)
					|| edge->getVertex(0)->getSector().get() != sector) continue;
				for (uint32_t side = 0; side < 2; ++side)
				{
					auto from = edge->getVertex(side), to = edge->getVertex(1 - side);
					auto a = from->getPosition(), b = to->getPosition();
					auto delta = b - a;
					auto const t = ((position.x - a.x) * delta.x + (position.y - a.y) * delta.y)
						/ (delta.x * delta.x + delta.y * delta.y);
					if (t < -PoseFitTolerance || t > 1.f + PoseFitTolerance
						|| position.distanceTo(a + delta * std::clamp(t, 0.f, 1.f)) > PoseFitTolerance
						|| mPlayerMovementDirection * (b.x - position.x) <= PoseFitTolerance) continue;
					bool const flight = edge->getType() == EdgeType::Stairwell;
					// A topology rebuild may retire a task on a continuous stair
					// surface. Resume that same segment, not a new flight choice.
					bool const resuming = t > PoseFitTolerance && t < 1.f - PoseFitTolerance;
					if (flight && !resuming && (!departure || !choice || delta.y * choice <= 0.f)) continue;
					if (!flight && departure) continue;
					if (!selected || edge->getId() < selected->getId())
					{
						selected = edge; source = from; destination = to;
					}
				}
			}
			if (!selected) return;
			if (!beginPlayerTraversal(agent, selected, source, destination)) return;
			agent.mPlayerStairPlatform = selected->getType() == EdgeType::Location;
			agent.mPlayerStairApproach = 0;
		}
		advancePlayerStairSegment(agent, timestep);
	}

	void World::advancePlayerStairSegment(Agent& agent, float timestep)
	{
		if (agent.mState == Agent::State::AwaitingTraversalCommit) return;
		auto const position = agent.getGlobalPosition();
		auto const& task = *agent.mTraversalTask;
		bool const ladder = task.edge->getType() == EdgeType::Ladder;
		int motion = ladder ? int(mPlayerStairUp) - int(mPlayerStairDown) : mPlayerMovementDirection;
		float stairSpeed = 0.f;
		if (task.edge->getType() == EdgeType::Staircase)
		{
			auto const& stairs = *static_cast<StaircaseTransit const*>(agent.getSector())->getStaircase();
			auto const path = stairs.getPath();
			auto const speed = playerStaircaseSpeed(stairs, agent, motion);
			motion = (speed > 0.f ? 1 : speed < 0.f ? -1 : 0) * (path[1].x > path[0].x ? 1 : -1);
			stairSpeed = std::abs(speed);
		}
		if (!motion) return;
		auto const delta = task.destinationVertex->getPosition() - task.sourceVertex->getPosition();
		bool const forward = motion * (ladder ? delta.y : delta.x) > 0.f;
		auto const target = forward ? task.destinationVertex : task.sourceVertex;
		auto const rise = target->getPosition().y - position.y;
		// Reversal on a direction-constrained climbing span is safe only while alone.
		if (auto resource = mTraversalResources.find(task.edge->getTraversalResourceId()); resource && (resource->mStairwell || resource->mLadder))
		{
			auto const direction = rise > 0.f ? TraversalDirection::Ascending : TraversalDirection::Descending;
			if (resource->mActiveDirection != direction)
			{
				if (std::any_of(resource->mAdmissionReservations.begin(), resource->mAdmissionReservations.end(),
					[&](auto owner) { return owner && owner != task.request; })
					|| std::any_of(resource->mOccupants.begin(), resource->mOccupants.end(),
						[&](auto owner) { return owner && owner != getAgentId(&agent); })) return;
				resource->mActiveDirection = direction;
			}
		}
		auto const ratio = agent.getPhysicalBaseline().automaticSpeedRatio(
			AutomaticPoseContext::RoomMovement, agent.getPose()).value_or(0.f);
		auto const speed = ladder ? agent.getClimbSpeed() * ratio
			: task.edge->getType() == EdgeType::Location ? agent.getRoomMovementSpeed()
			: task.edge->getType() == EdgeType::Staircase ? stairSpeed : agent.getStationaryStairSpeed(rise > 0.f) * ratio;
		if (!agent.poseFits(agent.getPose(), target->getSector()->getPosition().y
			+ target->getSector()->getLevelsHigh() - 1 + target->getSector()->getEffectiveTopLevelHeight()
			- target->getPosition().y)) return;
		auto boundedTarget = target->getPosition();
		if (auto sector = agent.getSector(); sector->getType() == SectorType::Stairwell
			|| sector->getType() == SectorType::Ladder || agent.mPlayerLadderMount)
		{
			float left = sector->getPosition().x, right = left + sector->getSize().x;
			if (sector->getType() == SectorType::Stairwell)
			{ left += CORE_DOOR_X_INSET; right -= CORE_DOOR_X_INSET; }
			auto half = agent.getWidth() * .5f;
			if (right - left < 2.f * half) return;
			auto x = std::clamp(boundedTarget.x, left + half, right - half);
			if (boundedTarget.x != position.x)
				boundedTarget.y = position.y + (boundedTarget.y - position.y)
					* (x - position.x) / (boundedTarget.x - position.x);
			boundedTarget.x = x;
		}
		if (agent.moveToPosition(boundedTarget, timestep, speed)
			&& agent.getGlobalPosition().distanceTo(target->getPosition()) <= PoseFitTolerance)
		{
			if (agent.getSector()->getType() == SectorType::Stairwell && agent.mPlayerStairPlatform)
				for (auto const& edge : getGraph()->getEdges())
					if (edge->getType() == EdgeType::Stairwell
						&& (edge->getVertex(0) == target || edge->getVertex(1) == target))
						agent.mPlayerStairApproach = mPlayerMovementDirection;
			if (forward) agent.mState = Agent::State::AwaitingTraversalCommit;
			else { agent.cancelTraversal(); agent.mState = Agent::State::Idle; }
		}
	}

	bool World::advancePlayerChamberAgent(Agent& agent, float timestep)
	{
		auto half = agent.getWidth() * .5f;
		auto position = agent.getGlobalPosition();
		auto airlock = dynamic_cast<AirlockTransit const*>(agent.getSector());
		auto scanner = dynamic_cast<ChamberTransit const*>(agent.getSector());
		auto chamber = airlock ? static_cast<Transit const*>(airlock) : static_cast<Transit const*>(scanner);
		auto chamberDoor = [&](int side) { return airlock ? airlock->getDoor(side) : scanner->getDoor(side); };
		bool crossing = false;
		if (agent.mTraversalTask)
		{
			auto resource = mTraversalResources.find(agent.mTraversalTask->edge->getTraversalResourceId());
			crossing = resource && (resource->mAirlock || resource->mSecurityScanner);
			if (!crossing) return false;
		}
		if (crossing)
		{
			auto const& task = *agent.mTraversalTask;
			float height = std::numeric_limits<float>::infinity();
			for (auto sector : {task.sourceVertex->getSector(), task.destinationVertex->getSector()})
				height = std::min(height, sector->getPosition().y + sector->getLevelsHigh() - 1
					+ sector->getEffectiveTopLevelHeight() - position.y);
			updatePlayerPose(agent, height, -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
		}
		else if (chamber) updatePlayerPose(agent, chamber->getEffectiveTopLevelHeight(),
			chamberDoor(0)->getPosition().x + chamberDoor(0)->getSize().x, chamberDoor(1)->getPosition().x);
		half = agent.getWidth() * .5f;
		if (!crossing && !chamber)
		{
			auto source = agent.getSector();
			if (agent.mPlayerLadderMount || !source || !isLocationLike(source->getType())
				|| !agent.poseFits(agent.getPose(), source->getPosition().y + source->getLevelsHigh() - 1
					+ source->getEffectiveTopLevelHeight() - position.y)) return false;
		}
		if (!crossing && mPlayerMovementDirection)
		{
			float next = position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * timestep;
			for (auto const& edge : getGraph()->getEdges())
			{
				auto threshold = std::dynamic_pointer_cast<const BulkheadDoorEdge>(edge);
				if (!threshold || !threshold->getDoor()->isChamberOwned()) continue;
				auto source = edge->getVertex(0), destination = edge->getVertex(1);
				if (source->getSector().get() != agent.getSector()) std::swap(source, destination);
				if (source->getSector().get() != agent.getSector()) continue;
				auto door = threshold->getDoor();
				int direction = destination->getPosition().x > source->getPosition().x ? 1 : -1;
				float boundary = direction > 0 ? door->getPosition().x : door->getPosition().x + door->getSize().x;
				if (direction != mPlayerMovementDirection || std::abs(position.y - door->getPosition().y) > PoseFitTolerance
					|| direction * (next + direction * half - boundary) < -PoseFitTolerance) continue;
				// Entry must be feasible through both thresholds; the coordinator
				// owns capacity, deadline, permissions and committed exit checks.
				auto pose = door->selectPlayerCrossing(agent, position.y);
				bool fit = door->admitsPlayerPassage(agent, position.y) && pose.has_value();
				for (auto const& sector : {source->getSector(), destination->getSector()})
					fit = fit && agent.poseFits(agent.getPose(), sector->getPosition().y + sector->getLevelsHigh() - 1
						+ sector->getEffectiveTopLevelHeight() - position.y);
				if (isLocationLike(destination->getSector()->getType()))
					fit = fit && (chamber || canAgentAccessLocation(*destination->getSector(), agent))
						&& playerTransportLandingSupported(*destination->getSector(),
							{boundary + direction * (door->getSize().x + half), position.y}, half);
				if (fit && beginPlayerTraversal(agent, edge, source, destination))
				{
					agent.mTraversalTask->crossingPose = pose;
					crossing = true;
				}
				if (!crossing && !chamber)
				{
					// Chamber cells are not ordinary Locations: stop at the physical
					// threshold, not the farther cell boundary, while admission is blocked.
					float limit = boundary - direction * half;
					position.x = direction > 0 ? std::min(next, std::max(position.x, limit))
						: std::max(next, std::min(position.x, limit));
					agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
					return true;
				}
				break;
			}
		}
		if (crossing)
		{
			auto& task = *agent.mTraversalTask;
			auto door = std::static_pointer_cast<const BulkheadDoorEdge>(task.edge)->getDoor();
			int direction = task.destinationVertex->getPosition().x > task.sourceVertex->getPosition().x ? 1 : -1;
			float delta = mPlayerMovementDirection * agent.getWalkSpeed() * timestep
				* (task.crossingPose ? task.crossingPose->speedRatio : 1.f);
			float next = position.x + delta;
			if (!door->admitsPlayerPassage(agent, position.y))
			{
				Vector2 min, max;
				door->getCurrentShape(min, max);
				if (delta > 0 && position.x - half < max.x)
					next = std::min(next, std::max(position.x, min.x - half));
				else if (delta < 0 && position.x + half > min.x)
					next = std::max(next, std::min(position.x, max.x + half));
			}
			position.x = next;
			agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
			float left = door->getPosition().x, right = left + door->getSize().x;
			bool complete = direction > 0 ? position.x - half >= right : position.x + half <= left;
			bool retraced = direction > 0 ? position.x + half <= left : position.x - half >= right;
			if (complete)
			{
				if (commitTraversal(agent, task.request, task.permit, task.destinationVertex))
				{
					releaseTraversal(task.request, task.permit);
					agent.mTraversalTask.reset(); agent.mTraversalLocalGoal.reset();
					agent.mState = Agent::State::Idle;
				}
			}
			else if (retraced && mPlayerMovementDirection == -direction)
			{
				agent.cancelTraversal();
				agent.mState = Agent::State::Idle;
			}
			return true;
		}
		if (!chamber) return false;
		// A stopped Player owns its slot and may walk during closure/cycling, but
		// never crosses a closed entrance or an uncommitted exit.
		float left = chamberDoor(0)->getPosition().x + chamberDoor(0)->getSize().x;
		float right = chamberDoor(1)->getPosition().x;
		updatePlayerPose(agent, chamber->getEffectiveTopLevelHeight(), left, right);
		half = agent.getWidth() * .5f;
		if (left + half <= right - half && agent.poseFits(agent.getPose(), chamber->getEffectiveTopLevelHeight()))
		{
			position.x = std::clamp(position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * timestep, left + half, right - half);
			agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
		}
		return true;
	}

	void World::updatePlayerPose(Agent& agent, float height, float left, float right, bool transitSupport)
	{
		auto threshold = agent.mTraversalTask ? std::dynamic_pointer_cast<const BulkheadDoorEdge>(agent.mTraversalTask->edge)
			: std::shared_ptr<const BulkheadDoorEdge>{};
		bool const climbing = agent.mPlayerLadderMount || (agent.mTraversalTask && (
			agent.mTraversalTask->edge->getType() == EdgeType::Staircase
			|| agent.mTraversalTask->edge->getType() == EdgeType::StaircaseMount
			|| agent.mTraversalTask->edge->getType() == EdgeType::Stairwell
			|| agent.mTraversalTask->edge->getType() == EdgeType::Location
			|| agent.mTraversalTask->edge->getType() == EdgeType::Ladder
			|| agent.mTraversalTask->edge->getType() == EdgeType::LadderMount));
		if ((agent.mTraversalTask && !threshold && !climbing) || agent.mFurnitureUse) return;
		if (!mPlayerCrouch && !agent.mPlayerPose) return;
		Pose desired = mPlayerCrouch ? (mPlayerMovementDirection ? Pose::Crawling : Pose::Crouching)
			: Pose::Standing;
		auto envelope = agent.getPoseEnvelope(desired);
		auto position = agent.getGlobalPosition();
		if (!envelope || !agent.poseFits(desired, height)
			|| position.x - envelope->x * .5f < left - PoseFitTolerance
			|| position.x + envelope->x * .5f > right + PoseFitTolerance) return;
		// Transit support replaces Floor lookup, never collision with a live leaf
		// in the owning Room (e.g. a Platform lift or Room Ladder at a wall).
		if (climbing || transitSupport)
			for (uint32_t i = 0; i < agent.getSector()->getNumObjects(); ++i)
				if (auto object = std::dynamic_pointer_cast<const BulkheadDoorSectorObject>(agent.getSector()->getObject(i)))
				{
					Vector2 min, max; object->getDoor()->getCurrentShape(min, max);
					if (min.x < max.x && min.y < max.y
						&& position.x - envelope->x * .5f < max.x - PoseFitTolerance
						&& position.x + envelope->x * .5f > min.x + PoseFitTolerance
						&& position.y < max.y - PoseFitTolerance
						&& position.y + envelope->y > min.y + PoseFitTolerance) return;
				}
		std::optional<float> crossingSpeed;
		if (threshold)
		{
			crossingSpeed = agent.getPhysicalBaseline().automaticSpeedRatio(AutomaticPoseContext::DoorCrossing, desired);
			if (!crossingSpeed || !threshold->getDoor()->admitsVerticalExtent(envelope->y, position.y)) return;
			Vector2 min, max; threshold->getDoor()->getCurrentShape(min, max);
			if (min.x < max.x && min.y < max.y
				&& position.x - envelope->x * .5f < max.x - PoseFitTolerance
				&& position.x + envelope->x * .5f > min.x + PoseFitTolerance
				&& position.y < max.y - PoseFitTolerance
				&& position.y + envelope->y > min.y + PoseFitTolerance) return;
		}
		if (mPlayerPlatformCrossing && mPlayerPlatformCrossing->owner == getAgentId(&agent))
			if (auto vehicle = mTraversalResources.find(mPlayerPlatformCrossing->resource);
				!vehicle || envelope->x > vehicle->mLift->getSize().x + PoseFitTolerance) return;
		if (threshold || (!climbing && !transitSupport && isLocationLike(agent.getSector()->getType())))
		{
			int y = (int)std::round(position.y);
			if (std::abs(position.y - y) > PoseFitTolerance || y < 0 || y >= (int)getLevelsHigh()) return;
			auto layer = getLayer(agent.getSector()->getLayerIndex());
			int first = (int)std::floor(position.x - envelope->x * .5f + PoseFitTolerance);
			int last = (int)std::floor(position.x + envelope->x * .5f - PoseFitTolerance);
			if (first < 0 || last >= (int)getCellsWide()) return;
			for (int x = first; x <= last; ++x)
			{
				auto const& cell = layer->getCellDefinition(x, y);
				if (cell.sectorIndex >= mSectors.size()) return;
				auto const& sector = *mSectors[cell.sectorIndex];
				bool ownedSide = threshold && (&sector == agent.mTraversalTask->sourceVertex->getSector().get()
					|| &sector == agent.mTraversalTask->destinationVertex->getSector().get());
				auto airlock = ownedSide ? dynamic_cast<AirlockTransit const*>(&sector) : nullptr;
				auto scanner = ownedSide ? dynamic_cast<ChamberTransit const*>(&sector) : nullptr;
				bool chamberFloor = (airlock || scanner) && y == (int)sector.getCellY();
				if ((!cell.isTraversableOnFoot() && !chamberFloor)
					|| (!isLocationLike(sector.getType()) && !chamberFloor) || !agent.poseFits(desired, sector.getPosition().y
					+ sector.getLevelsHigh() - 1 + sector.getEffectiveTopLevelHeight() - position.y)) return;
				if (&sector != agent.getSector() && !ownedSide && sector.getType() == SectorType::Location
					&& (static_cast<Location const&>(sector).getPermissionRequirement() & ~effectiveAccessGrants(agent)).any()) return;
				if (airlock || scanner)
				{
					auto entry = airlock ? airlock->getDoor(0) : scanner->getDoor(0);
					auto exit = airlock ? airlock->getDoor(1) : scanner->getDoor(1);
					if (envelope->x > exit->getPosition().x - entry->getPosition().x - entry->getSize().x + PoseFitTolerance) return;
				}
				for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
					if (isLocationLike(sector.getType()) && sector.getEndType(y - sector.getCellY(), side) == SectorEndType::Wall
						&& (side == CORE_SIDE_LEFT ? position.x - envelope->x * .5f < sector.getPosition().x - PoseFitTolerance
							: position.x + envelope->x * .5f > sector.getPosition().x + sector.getSize().x + PoseFitTolerance)) return;
				for (uint32_t i = 0; i < sector.getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<const BulkheadDoorSectorObject>(sector.getObject(i)))
					{
						Vector2 min, max; object->getDoor()->getCurrentShape(min, max);
						if (min.x < max.x && min.y < max.y
							&& position.x - envelope->x * .5f < max.x - PoseFitTolerance
							&& position.x + envelope->x * .5f > min.x + PoseFitTolerance
							&& position.y < max.y - PoseFitTolerance
							&& position.y + envelope->y > min.y + PoseFitTolerance) return;
					}
			}
		}
		if (threshold) agent.mTraversalTask->crossingPose = PoseSelection{desired, *crossingSpeed};
		agent.mRetainedActionPose = false;
		agent.mPose = desired;
		if (mPlayerCrouch) agent.mPlayerPose = desired;
		else agent.mPlayerPose.reset();
	}

	void World::updatePlayerClimbingPose(Agent& agent)
	{
		auto sector = agent.getSector();
		if (!sector) return;
		auto const position = agent.getGlobalPosition();
		float height = std::min(CORE_CORRIDOR_HEIGHT, sector->getPosition().y
			+ sector->getLevelsHigh() - 1 + sector->getEffectiveTopLevelHeight() - position.y);
		float left = sector->getPosition().x, right = left + sector->getSize().x;
		if (auto stairs = dynamic_cast<StairwellTransit const*>(sector))
		{ left += CORE_DOOR_X_INSET; right -= CORE_DOOR_X_INSET; }
		else if (sector->getType() == SectorType::Staircase)
		{
			// At an endpoint the adjoining landing supports the body's overhang.
			// The flight still refuses envelopes wider than its authored span.
			left = position.x - sector->getSize().x * .5f;
			right = position.x + sector->getSize().x * .5f;
		}
		updatePlayerPose(agent, height, left, right);
	}

	void World::advancePlayerControlledAgent(AgentId id, float timestep)
	{
		auto* agent = mAgents.find(id);
		if (!agent || !agent->isActive() || !agent->isPlayerControlled()) return;
		// A moving Platform lift, not the Room's integer Floor, supplies support.
		// Check its bounded body before the generic Location posture update.
		for (auto const& [resourceId, resource] : mTraversalResources.entries())
			if (!agent->mTraversalTask && resource->mOpenPlatformLift
				&& std::find(resource->mOccupants.begin(), resource->mOccupants.end(), id) != resource->mOccupants.end())
			{ advancePlayerTransportAgent(*agent, timestep); return; }
		if (auto source = agent->getSector(); source && isLocationLike(source->getType()) && !agent->mTraversalTask)
			updatePlayerPose(*agent, source->getPosition().y + source->getLevelsHigh() - 1
				+ source->getEffectiveTopLevelHeight() - agent->getGlobalPosition().y,
				-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
		if (agent->getSector() && (agent->getSector()->getType() == SectorType::Staircase
			|| agent->getSector()->getType() == SectorType::Stairwell || agent->mPlayerLadderMount))
			updatePlayerClimbingPose(*agent);
		if (advancePlayerChamberAgent(*agent, timestep)) return;
		if (agent->mPlayerLadderMount)
		{
			advancePlayerLadderAgent(*agent, timestep);
			return;
		}
		if (agent->getSector() && agent->getSector()->getType() == SectorType::Staircase)
		{
			advancePlayerStaircaseAgent(*agent, timestep);
			return;
		}
		if (agent->getSector() && agent->getSector()->getType() == SectorType::Stairwell
			&& (!agent->mTraversalTask || agent->mTraversalTask->edge->getType() == EdgeType::Stairwell
				|| agent->mTraversalTask->edge->getType() == EdgeType::Location))
		{
			advancePlayerStairwellAgent(*agent, timestep);
			return;
		}
		if (agent->mTraversalTask)
		{
			agent->update(timestep);
			return;
		}
		agent->mState = Agent::State::Idle;
		if (advancePlayerTransportAgent(*agent, timestep)) return;
		auto* source = agent->getSector();
		if (!source || !isLocationLike(source->getType())) return;
		auto position = agent->getGlobalPosition();
		if (!mPlayerMovementDirection) return;
		auto const level = static_cast<int>(std::round(position.y));
		if (level < 0 || level >= static_cast<int>(getLevelsHigh())
			|| std::abs(position.y - level) > PoseFitTolerance) return;
		auto const layer = getLayer(source->getLayerIndex());
		auto const grants = effectiveAccessGrants(*agent);
		auto fits = [&](Sector const& sector)
		{
			auto const ceiling = sector.getPosition().y + sector.getLevelsHigh() - 1
				+ sector.getEffectiveTopLevelHeight();
			return agent->poseFits(agent->mPlayerPose.value_or(Pose::Standing), ceiling - position.y);
		};
		// Movement uses the selected supported pose, never invented headroom.
		if (!fits(*source)) return;
		auto cellSector = [&](int x) -> Sector*
		{
			if (x < 0 || x >= static_cast<int>(getCellsWide())) return nullptr;
			auto const& cell = layer->getCellDefinition(x, level);
			if (!cell.isTraversableOnFoot() || cell.sectorIndex >= mSectors.size()) return nullptr;
			auto* sector = mSectors[cell.sectorIndex].get();
			if (!isLocationLike(sector->getType()) || !fits(*sector)) return nullptr;
			if (sector != source && sector->getType() == SectorType::Location
				&& (static_cast<Location const&>(*sector).getPermissionRequirement() & ~grants).any()) return nullptr;
			if (cell.floorType == CellFloorType::ForceBridge)
			{
				auto bridge = std::dynamic_pointer_cast<ForceBridgeSectorObject>(sector->getObject(cell.floorIndex));
				if (!bridge || !bridge->getForceBridge()->admitsNewTraversals()) return nullptr;
				auto const device = bridge->getForceBridge();
				auto const resource = mTraversalResources.find(device->getTraversalResourceId());
				if (resource && !resource->mEnabled)
				{
					// A pending safe retract refuses new entry but lets the existing
					// manual occupant walk off the still-supported span.
					auto const left = device->getPosition().x;
					auto const right = left + device->getSize().x;
					if (source != sector || position.x + agent->getWidth() * .5f <= left
						|| position.x - agent->getWidth() * .5f >= right) return nullptr;
				}
			}
			return sector;
		};
		auto passage = [&](Sector const& from, Sector const& to, int fromX, int toX, float& barrier)
		{
			if (&from == &to) return true;
			int const side = toX > fromX ? CORE_SIDE_RIGHT : CORE_SIDE_LEFT;
			for (auto const& entry : {std::pair{&from, fromX}, std::pair{&to, toX}})
			{
				auto const* sector = entry.first;
				int const endSide = sector == &from ? side : 1 - side;
				auto const end = sector->getEndType(level - sector->getCellY(), endSide);
				if (end == SectorEndType::Wall) return false;
				if (end == SectorEndType::BulkheadDoor)
				{
					// Both cells store the index in the left Sector's object array;
					// its shared registration on the right may have another index.
					auto const& cell = layer->getCellDefinition(std::min(fromX, toX), level);
					auto object = std::dynamic_pointer_cast<BulkheadDoorSectorObject>(
						mSectors[cell.sectorIndex]->getObject(cell.bulkheadIndices[CORE_SIDE_RIGHT]));
					auto door = object ? object->getDoor() : nullptr;
					if (!door || door->isChamberOwned() || !door->admitsPlayerPassage(*agent, position.y)
						|| agentForbidsTraversal(agent, TraversalKind::Door))
					{
						if (door) barrier = side == CORE_SIDE_RIGHT ? door->getPosition().x
							: door->getPosition().x + door->getSize().x;
						return false;
					}
				}
			}
			return true;
		};

		int const direction = mPlayerMovementDirection;
		float const halfWidth = agent->getWidth() * 0.5f;
		float targetX = position.x + direction * agent->getRoomMovementSpeed() * timestep;
		auto stairMount = playerStaircaseApproach(*agent);
		auto const ladderMount = playerLadderApproach(*agent);
		if (ladderMount && (!stairMount
			|| std::abs(ladderMount->getVertex(0)->getPosition().x - position.x)
				< std::abs(stairMount->getVertex(0)->getPosition().x - position.x)
			|| (std::abs(ladderMount->getVertex(0)->getPosition().x - position.x)
				== std::abs(stairMount->getVertex(0)->getPosition().x - position.x)
				&& ladderMount->getId() < stairMount->getId()))) stairMount = ladderMount;
		std::shared_ptr<const Vertex> stairSource, stairDestination;
		if (stairMount)
		{
			stairSource = stairMount->getVertex(0); stairDestination = stairMount->getVertex(1);
			if (stairSource->getSector().get() != source) std::swap(stairSource, stairDestination);
			if (direction * (targetX - stairSource->getPosition().x) >= 0.f)
				targetX = stairSource->getPosition().x;
		}
		int x = static_cast<int>(std::floor(position.x));
		auto* current = cellSector(x);
		if (!current) return;
		// Sweep the leading edge across every crossed cell, not just the endpoint:
		// fast Agents cannot tunnel through walls, low Rooms or Floor gaps.
		while (true)
		{
			float const boundary = direction > 0 ? float(x + 1) : float(x);
			auto* next = cellSector(x + direction);
			// At a selected open Room-wall mount the flight supplies support
			// beyond the landing Floor. Let the body overhang only that endpoint;
			// its centre still walks exactly to the mount before changing Sector.
			if (!next && stairMount && stairMount->getType() == EdgeType::StaircaseMount && current == source
				&& std::abs(boundary - stairSource->getPosition().x) <= PoseFitTolerance
				&& source->getEndType(level - source->getCellY(), direction > 0 ? CORE_SIDE_RIGHT : CORE_SIDE_LEFT)
					== SectorEndType::None) break;
			float barrier = boundary;
			if (!next || !passage(*current, *next, x, x + direction, barrier))
			{
				float const limit = barrier - direction * halfWidth;
				if (direction * (targetX - limit) > 0.f)
					targetX = direction > 0 ? std::max(position.x, limit) : std::min(position.x, limit);
				break;
			}
			if (direction * (targetX + direction * halfWidth - boundary) <= 0.f) break;
			x += direction;
			current = next;
		}
		if (stairMount && std::abs(targetX - stairSource->getPosition().x) <= PoseFitTolerance)
		{
			// The normal Floor sweep above remains authoritative while approaching.
			agent->setPosition({source, Vector2{targetX, position.y} - source->getPosition()}, false);
			if (beginPlayerTraversal(*agent, stairMount, stairSource, stairDestination))
			{
				if (stairMount->getType() == EdgeType::LadderMount)
				{ agent->mPlayerLadderMount = stairMount; agent->mPlayerLadderEntry = true; }
				else agent->mPlayerStaircaseEntry = true;
			}
			return;
		}
		if (targetX == position.x) return;
		position.x = targetX;
		auto* destination = cellSector(static_cast<int>(std::floor(targetX)));
		if (!destination) return;
		if (destination != source)
		{
			const_cast<Sector*>(source)->exitAgent(agent);
			destination->enterAgent(agent, {destination, position - destination->getPosition()}, false);
		}
		else agent->setPosition({source, position - source->getPosition()}, false);
		if (!agent->mPlayerPose) agent->mPose = Pose::Standing;
		updatePlayerPose(*agent, destination->getPosition().y + destination->getLevelsHigh() - 1
			+ destination->getEffectiveTopLevelHeight() - position.y,
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
	}

	bool World::playerTransportLandingSupported(Sector const& sector, Vector2 position, float halfWidth) const
	{
		int y = (int)std::round(position.y);
		int first = (int)std::floor(position.x - halfWidth + PoseFitTolerance);
		int last = (int)std::floor(position.x + halfWidth - PoseFitTolerance);
		if (y < 0 || y >= (int)getLevelsHigh() || first < 0 || last >= (int)getCellsWide()) return false;
		auto layer = getLayer(sector.getLayerIndex());
		for (int x = first; x <= last; ++x)
		{
			auto const& cell = layer->getCellDefinition(x, y);
			if (cell.sectorIndex != sector.getIndex() || !cell.isTraversableOnFoot()) return false;
			if (cell.floorType == CellFloorType::ForceBridge)
			{
				auto bridge = std::dynamic_pointer_cast<ForceBridgeSectorObject>(sector.getObject(cell.floorIndex));
				if (!bridge || !bridge->getForceBridge()->admitsNewTraversals()) return false;
				auto resource = mTraversalResources.find(bridge->getForceBridge()->getTraversalResourceId());
				if (resource && !resource->mEnabled) return false;
			}
		}
		return true;
	}

	std::optional<World::PlayerTransportSelector> World::getPlayerTransportSelector() const
	{
		if (isSimulationPaused()) return {};
		auto id = getPlayerControlledAgent();
		auto actor = mAgents.find(id);
		if (!actor || !actor->isActive() || actor->mTraversalTask || mPlayerPlatformCrossing) return {};
		for (auto const& [resourceId, ptr] : mTraversalResources.entries())
		{
			auto const& vehicle = *ptr;
			auto occupant = std::find(vehicle.mOccupants.begin(), vehicle.mOccupants.end(), id);
			if ((!vehicle.mLift && !vehicle.mShuttle) || occupant == vehicle.mOccupants.end()
				|| !vehicle.mEnabled || vehicle.mLiftMoving
				|| (vehicle.mLift && vehicle.mLift->isBroken()) || (vehicle.mShuttle && vehicle.mShuttle->isBroken())
				|| vehicle.mLiftStopPhase != LiftStopPhase::Boarding
				|| mSimulationTick >= vehicle.mLiftBoardingCutoffTick) continue;
			auto carriage = vehicle.mShuttle ? (uint32_t)std::distance(vehicle.mOccupants.begin(), occupant)
				/ vehicle.mShuttleCapacityPerCarriage : 0u;
			auto landingAt = [&](uint32_t stop) -> TraversalResource const*
			{
				if (!vehicle.mShuttle) return mTraversalResources.find(vehicle.mLiftStops[stop].landingResource);
				for (auto const& door : vehicle.mShuttleDoors)
					if (door.stopIndex == stop && door.carriageIndex == carriage)
						if (auto landing = mTraversalResources.find(door.landingResource)) return landing;
				return nullptr;
			};
			auto current = vehicle.mLiftCurrentStop;
			auto landing = landingAt(current);
			if (!vehicle.mOpenPlatformLift && (!landing || !landing->mDoor || !landing->mDoor->isOpen()
				|| (!vehicle.mShuttle && !vehicle.mLiftCarDoorOpen))) continue;
			PlayerTransportSelector result;
			result.resource = resourceId; result.horizontal = (bool)vehicle.mShuttle;
			result.anchor = actor->getGlobalPosition();
			if (vehicle.mShuttle)
			{
				result.anchor.x = vehicle.mLiftPosition + carriage * (vehicle.mShuttle->getCarWidth() + 1.f)
					+ vehicle.mShuttle->getCarWidth() * .5f;
				result.anchor.y += vehicle.mShuttle->getSize().y + .2f;
			}
			else
			{
				auto control = mInteractionPoints.find(vehicle.mLiftStops[current].callControl);
				if (!control && landing && !landing->mControls.empty()) control = mInteractionPoints.find(landing->mControls.front());
				result.anchor = control ? control->mPosition : vehicle.mLift->getPosition();
				result.anchor.y += .3f;
			}
			for (uint32_t stop = 0; stop < vehicle.mLiftStops.size(); ++stop)
			{
				bool pending = vehicle.mPlayerStopRequests.contains(stop) || !vehicle.mLiftStopRequestOwners[stop].empty();
				DeviceCommand command;
				command.type = vehicle.mShuttle ? DeviceCommandType::SelectShuttleDestination : DeviceCommandType::SelectLiftDestination;
				command.traversalResource = resourceId; command.stopIndex = stop;
				bool eligible = missingLiftDestinationPermissions(command, id).empty()
					&& agentAdheresToLiftDestinationPermission(resourceId, stop, id)
					&& ((!agentForbidsButtons(actor) && actor->getObjectUsage() == ObjectUsage::Arms) || pending
						|| agentCanRemotelySelectLiftDestination(resourceId, *actor, false));
				auto stopLanding = landingAt(stop);
				auto locationId = stopLanding && vehicle.mShuttle
					? SectorId{(uint64_t)stopLanding->mDoor->getFrontSector()->getIndex() + 1}
					: vehicle.mLiftStops[stop].locationSector;
				if (!locationId || locationId.value > mSectors.size()) eligible = false;
				else
				{
					auto location = mSectors[(size_t)locationId.value - 1].get();
					float y = vehicle.mShuttle ? actor->getGlobalPosition().y : vehicle.mLiftStops[stop].globalPosition;
					float x = stopLanding && stopLanding->mDoor ? stopLanding->mDoor->getPosition().x + stopLanding->mDoor->getSize().x * .5f
						: (stop < vehicle.mQueueLanes.size() ? vehicle.mQueueLanes[stop].origin.x : vehicle.mLift->getPosition().x - .5f);
					eligible = eligible && playerTransportLandingSupported(*location, {x, y}, actor->getWidth() * .5f)
						&& canAgentAccessLocation(*location, *actor)
						&& actor->poseFits(actor->getPose(), location->getPosition().y + location->getLevelsHigh() - 1
							+ location->getEffectiveTopLevelHeight() - y);
				}
				if (vehicle.mShuttle && !stopLanding) eligible = false;
				if (stop != current && !eligible) continue;
				result.stops.push_back({stop, getTransportStopShortName(resourceId, stop), stop == current, pending});
			}
			result.focus = vehicle.mPlayerStopFocus;
			if (std::none_of(result.stops.begin(), result.stops.end(), [&](auto const& stop) { return !stop.current && stop.index == result.focus; }))
			{
				result.focus = ~0u;
				float best = std::numeric_limits<float>::max();
				for (auto const& stop : result.stops) if (!stop.current)
				{
					auto distance = std::abs(vehicle.mLiftStops[stop.index].globalPosition - vehicle.mLiftPosition);
					if (distance < best) { best = distance; result.focus = stop.index; }
				}
			}
			return result;
		}
		return {};
	}

	void World::browsePlayerTransportStop(int direction, bool horizontal)
	{
		auto selector = getPlayerTransportSelector();
		if (!selector || selector->horizontal != horizontal || !direction) return;
		std::vector<uint32_t> eligible;
		for (auto const& stop : selector->stops) if (!stop.current) eligible.push_back(stop.index);
		auto focus = std::find(eligible.begin(), eligible.end(), selector->focus);
		if (focus == eligible.end()) return;
		auto index = std::clamp((int)std::distance(eligible.begin(), focus) + (direction > 0 ? 1 : -1), 0, (int)eligible.size() - 1);
		mTraversalResources.find(selector->resource)->mPlayerStopFocus = eligible[index];
		invalidateSimulationSnapshot();
	}

	void World::confirmPlayerTransportStop()
	{
		auto selector = getPlayerTransportSelector();
		if (!selector || selector->focus == ~0u) return;
		auto resource = mTraversalResources.find(selector->resource);
		resource->mPlayerStopFocus = selector->focus;
		if (resource->mLiftStopRequestOwners[selector->focus].empty())
			resource->mPlayerStopRequests.try_emplace(selector->focus, mSimulationTick);
		invalidateSimulationSnapshot();
	}

	bool World::tryPlayerPlatformMount(Agent& agent)
	{
		auto id = getAgentId(&agent);
		if (mPlayerPlatformCrossing && (mPlayerPlatformCrossing->owner != id
			|| !mTraversalRequests.find(mPlayerPlatformCrossing->request)
			|| mTraversalRequests.find(mPlayerPlatformCrossing->request)->mState != TraversalRequestState::Granted)) mPlayerPlatformCrossing.reset();
		for (auto const& [resourceId, ptr] : mTraversalResources.entries())
		{
			auto& vehicle = *ptr;
			if (!vehicle.mOpenPlatformLift || vehicle.mLiftSector.value != agent.getSector()->getIndex() + 1) continue;
			auto occupant = std::find(vehicle.mOccupants.begin(), vehicle.mOccupants.end(), id);
			bool aboard = occupant != vehicle.mOccupants.end();
			bool crossing = mPlayerPlatformCrossing && mPlayerPlatformCrossing->resource == resourceId;
			if (!aboard && !crossing && (!mPlayerMovementDirection || vehicle.mLiftMoving
				|| std::abs(agent.getGlobalPosition().y - vehicle.mLiftPosition) > PoseFitTolerance)) continue;
			auto position = agent.getGlobalPosition();
			float left = vehicle.mLift->getPosition().x, right = left + vehicle.mLift->getSize().x;
			if (aboard && !crossing) updatePlayerPose(agent, agent.getSector()->getPosition().y
				+ agent.getSector()->getLevelsHigh() - 1 + agent.getSector()->getEffectiveTopLevelHeight() - position.y, left, right, true);
			else if (crossing) updatePlayerPose(agent, agent.getSector()->getPosition().y
				+ agent.getSector()->getLevelsHigh() - 1 + agent.getSector()->getEffectiveTopLevelHeight() - position.y,
				-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity());
			float half = agent.getWidth() * .5f;
			float next = position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * getFixedTimestep();
			bool inside = next - half >= left && next + half <= right;
			if (!aboard && !crossing && (next + half <= left || next - half >= right)) continue;
			if (half * 2.f > right - left || !agent.poseFits(agent.getPose(),
				agent.getSector()->getPosition().y + agent.getSector()->getLevelsHigh() - 1
					+ agent.getSector()->getEffectiveTopLevelHeight() - position.y)) return true;
			bool boundary = crossing || (!aboard || !inside);
			if (boundary && !crossing)
			{
				// A real supported landing and a free virtual boundary are required;
				// neither a closed moving platform nor a missing Floor can be crossed.
				if (vehicle.mLiftMoving || (vehicle.mLiftStopPhase != LiftStopPhase::Boarding
					&& vehicle.mLiftStopPhase != LiftStopPhase::Opening
					&& vehicle.mLiftStopPhase != LiftStopPhase::Disembarking)
					|| agentForbidsTraversal(&agent, TraversalKind::PlatformLift)) return true;
				int x = position.x < (left + right) * .5f ? (int)std::floor(left - .001f) : (int)std::ceil(right);
				int y = (int)std::round(vehicle.mLiftPosition);
				if (x < 0 || x >= (int)getCellsWide() || y < 0 || y >= (int)getLevelsHigh()) return true;
				if (!playerTransportLandingSupported(*agent.getSector(), {x + .5f, (float)y}, half)) return true;
				std::shared_ptr<const Edge> edge;
				std::shared_ptr<const Vertex> source, destination;
				for (auto const& candidate : getGraph()->getEdges())
					if (candidate->getType() == EdgeType::LiftMount && candidate->getTraversalResourceId() == resourceId
						&& std::abs(candidate->getVertex(0)->getPosition().y - position.y) <= PoseFitTolerance)
					{
						edge = candidate; source = edge->getVertex(0); destination = edge->getVertex(1);
						if ((bool)source->getObject() != !aboard) std::swap(source, destination);
						break;
					}
				if (!edge || !agent.poseFits(agent.getPose(), agent.getSector()->getPosition().y
					+ agent.getSector()->getLevelsHigh() - 1 + agent.getSector()->getEffectiveTopLevelHeight() - position.y)) return true;
				agent.mPathStartPosition = position;
				auto requestId = createTraversalRequest(agent, edge, source, destination);
				auto request = mTraversalRequests.find(requestId);
				request->mSourceEndpoint = request->mDestinationEndpoint = position;
				allocateTraversalRequest(requestId, edge, destination);
				if (request->mState != TraversalRequestState::Granted)
				{
					cancelTraversal(requestId, {}); releaseTraversal(requestId, {}); return true;
				}
				mPlayerPlatformCrossing = PlayerPlatformCrossing{resourceId, requestId, request->mPermit,
					destination, id, request->mCapacityPosition, !aboard};
				agent.mPlayerPose = agent.getPose();
				half = agent.getWidth() * .5f;
				next = position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * getFixedTimestep();
				crossing = true;
			}
			if (!crossing)
			{
				position.x = std::clamp(next, left + half, right - half);
				agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
				return true;
			}
			position.x = next;
			agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
			bool outside = position.x + half <= left || position.x - half >= right;
			inside = position.x - half >= left && position.x + half <= right;
			if (inside || outside)
			{
				auto transfer = *mPlayerPlatformCrossing;
				if (inside == transfer.entering)
				{
					if (commitTraversal(agent, transfer.request, transfer.permit, transfer.destination))
					{
						if (transfer.entering)
						{
							vehicle.mAdmissionReservations[transfer.capacity] = {};
							vehicle.mOccupants[transfer.capacity] = id;
						}
						else mSimulationCoordinator.releaseAgentFromResource(vehicle, id);
					}
				}
				else cancelTraversal(transfer.request, transfer.permit); // Manually retraced the boundary.
				releaseTraversal(transfer.request, transfer.permit);
				mPlayerPlatformCrossing.reset();
			}
			return true;
		}
		return false;
	}

	bool World::advancePlayerTransportAgent(Agent& agent, float timestep)
	{
		auto id = getAgentId(&agent);
		for (auto const& [resourceId, ptr] : mTraversalResources.entries())
		{
			auto& vehicle = *ptr;
			auto occupant = std::find(vehicle.mOccupants.begin(), vehicle.mOccupants.end(), id);
			if ((!vehicle.mLift && !vehicle.mShuttle) || occupant == vehicle.mOccupants.end()) continue;
			if (vehicle.mOpenPlatformLift) break; // The open boundary is handled with ordinary horizontal input below.
			float left = vehicle.mLift ? vehicle.mLift->getPosition().x : vehicle.mLiftPosition;
			float width = vehicle.mLift ? vehicle.mLift->getSize().x : (float)vehicle.mShuttle->getCarWidth();
			if (vehicle.mShuttle) left += ((uint32_t)std::distance(vehicle.mOccupants.begin(), occupant)
				/ vehicle.mShuttleCapacityPerCarriage) * (width + 1.f);
			float height = vehicle.mLift ? vehicle.mLift->getSize().y : vehicle.mShuttle->getSize().y;
			updatePlayerPose(agent, height, left, left + width);
			auto envelope = agent.getPoseEnvelope(agent.getPose());
			auto half = envelope.value_or(Vector2{}).x * .5f;
			if (!envelope || width < 2.f * half || !agent.poseFits(agent.getPose(), height)) return true;
			auto position = agent.getGlobalPosition();
			position.x = std::clamp(position.x + mPlayerMovementDirection * agent.getRoomMovementSpeed() * timestep,
				left + half, left + width - half);
			agent.setPosition({const_cast<Sector*>(agent.getSector()), position - agent.getSector()->getPosition()}, false);
			return true;
		}
		return tryPlayerPlatformMount(agent);
	}
}
