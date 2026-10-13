#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Location.h"
#include "core/DoorEdge.h"
#include "core/Graph.h"
#include "core/MobilityProfile.h"
#include "core/StairwellMountEdge.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/SectorEdge.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace core
{
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

	void World::clearPlayerInput()
	{
		mPlayerMovementDirection = 0;
		mPlayerDoorAction = {};
		mPlayerStairUp = mPlayerStairDown = false;
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
		// Held stair selection is contextual: near an endpoint and walking into
		// its flight, Up must not simultaneously activate a neighbouring Door.
		if (playerStaircaseApproach(*agent)) return;
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
				auto const envelope = agent->getPoseEnvelope(Pose::Standing);
				auto const stairs = mount->getStairwell();
				if (!envelope || !agent->poseFits(Pose::Standing, CORE_DOOR_HEIGHT)
					|| position.x - envelope->x * .5f < stairs->getPosition().x + CORE_DOOR_X_INSET - PoseFitTolerance
					|| position.x + envelope->x * .5f > stairs->getPosition().x + stairs->getSize().x - CORE_DOOR_X_INSET + PoseFitTolerance)
					continue;
				bool fit = true;
				for (auto const& sector : {source->getSector(), destination->getSector()})
					fit = fit && agent->poseFits(Pose::Standing, sector->getPosition().y + sector->getLevelsHigh() - 1
						+ sector->getEffectiveTopLevelHeight() - position.y);
				if (fit) candidates.push_back({edge, source, destination, PoseSelection{Pose::Standing, 1.f},
					std::abs(position.x - source->getPosition().x), true, false});
				continue;
			}
			auto door = ordinary->getDoor();
			if (typeid(*door) != typeid(Door) || door->isLiftOwned() || door->isShuttleOwned()
				|| !door->getFrontSector() || !door->getBackSector()
				|| !isLocationLike(door->getFrontSector()->getType())
				|| !isLocationLike(door->getBackSector()->getType())) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (source->getSector().get() != agent->getSector()) std::swap(source, destination);
			if (source->getSector().get() != agent->getSector()
				|| std::abs(position.y - door->getPosition().y) > PoseFitTolerance
				|| position.x < door->getPosition().x
				|| position.x > door->getPosition().x + door->getSize().x
				|| agentForbidsEdge(agent, *edge, TraversalKind::Door)) continue;
			auto resource = mTraversalResources.find(door->getTraversalResourceId());
			if (!resource || !resource->mEnabled) continue;
			auto const halfWidth = agent->getPoseEnvelope(Pose::Standing).value_or(Vector2{}).x * .5f;
			auto const pose = door->selectAgentCrossing(*agent, position.y, true);
			auto const standingRoom = [&](Sector const& sector) {
				return agent->poseFits(Pose::Standing, sector.getPosition().y + sector.getLevelsHigh() - 1
					+ sector.getEffectiveTopLevelHeight() - position.y);
			};
			bool const passable = door->admitsNewCrossings() && pose && pose->pose == Pose::Standing
				&& standingRoom(*source->getSector()) && standingRoom(*destination->getSector())
				&& position.x - halfWidth >= door->getPosition().x - PoseFitTolerance
				&& position.x + halfWidth <= door->getPosition().x + door->getSize().x + PoseFitTolerance
				&& canAgentAccessLocation(*destination->getSector(), *agent)
				&& agentAdheresToDoorPermission(door->getTraversalResourceId(),
					SectorId{uint64_t(agent->getSector()->getIndex()) + 1}, id);
			// Player geometry replaces distance, not capability or authorization.
			bool const openable = door->getActivationMode() == DoorActivationMode::Manual
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
			agent->mPose = Pose::Standing;
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
			if (!transit || transit->getStaircase()->isEscalator()) continue;
			auto const path = transit->getStaircase()->getPath();
			bool const lower = std::abs(source->getPosition().y - transit->getPosition().y) <= PoseFitTolerance;
			if ((choice > 0) != lower) continue;
			auto const delta = path[lower ? 1 : 0] - path[lower ? 0 : 1];
			auto const gap = mPlayerMovementDirection * (source->getPosition().x - position.x);
			if (mPlayerMovementDirection * delta.x <= 0.f || gap < -PoseFitTolerance
				|| gap > nearest || std::abs(position.y - source->getPosition().y) > PoseFitTolerance
				|| agentForbidsEdge(&agent, *edge, TraversalKind::Staircase)
				|| !canAgentAccessLocation(*destination->getSector(), agent)
				|| !agent.poseFits(Pose::Standing, source->getSector()->getPosition().y
					+ source->getSector()->getLevelsHigh() - 1 + source->getSector()->getEffectiveTopLevelHeight() - position.y)
				|| !agent.poseFits(Pose::Standing, CORE_CORRIDOR_HEIGHT)) continue;
			if (!selected || gap < nearest || (gap == nearest && edge->getId() < selected->getId()))
			{ selected = edge; nearest = gap; }
		}
		return selected;
	}

	bool World::beginPlayerTraversal(Agent& agent, std::shared_ptr<const Edge> const& edge,
		std::shared_ptr<const Vertex> const& source, std::shared_ptr<const Vertex> const& destination)
	{
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
		agent.mRetainedActionPose = false;
		agent.mTraversalTask = std::move(task);
		agent.mState = Agent::State::TraversingEdge;
		agent.mPose = Pose::Standing;
		return true;
	}

	void World::advancePlayerStaircaseAgent(Agent& agent, float timestep)
	{
		auto const* transit = dynamic_cast<StaircaseTransit const*>(agent.getSector());
		if (!transit || transit->getStaircase()->isEscalator()) return;
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
		// Endpoints are mounts, not doorways or platforms. Dismount even with
		// no input, including after retracing a flight to its original endpoint.
		for (auto const& edge : getGraph()->getEdges())
		{
			if (edge->getType() != EdgeType::StaircaseMount) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (source->getSector().get() != transit) std::swap(source, destination);
			if (source->getSector().get() != transit
				|| position.distanceTo(source->getPosition()) > PoseFitTolerance) continue;
			// Immediately after mounting, input points into the flight. At its
			// other end (or after reversal), it points out and triggers dismount.
			auto const path = transit->getStaircase()->getPath();
			bool const lower = std::abs(position.y - transit->getPosition().y) <= PoseFitTolerance;
			auto const into = path[lower ? 1 : 0].x - path[lower ? 0 : 1].x;
			if (!agent.mPlayerStaircaseEntry || mPlayerMovementDirection * into <= 0.f)
			{
				if (canAgentAccessLocation(*destination->getSector(), agent)
					&& agent.poseFits(Pose::Standing, destination->getSector()->getPosition().y
						+ destination->getSector()->getLevelsHigh() - 1
						+ destination->getSector()->getEffectiveTopLevelHeight() - position.y))
				{
					beginPlayerTraversal(agent, edge, source, destination);
					return;
				}
				// A refused exit must not prevent retracing the admitted stairs.
				if (mPlayerMovementDirection * into <= 0.f) return;
			}
		}
		if (!mPlayerMovementDirection) return;
		for (auto const& edge : getGraph()->getEdges())
		{
			if (edge->getType() != EdgeType::Staircase || edge->getVertex(0)->getSector().get() != transit) continue;
			auto source = edge->getVertex(0), destination = edge->getVertex(1);
			if (mPlayerMovementDirection * (destination->getPosition().x - source->getPosition().x) < 0.f)
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
				|| !agent.poseFits(Pose::Standing, sector->getPosition().y + sector->getLevelsHigh() - 1
					+ sector->getEffectiveTopLevelHeight() - position.y)) return;
			// Up preserves X. On an integer landing, walking reaches the central
			// stair mount explicitly; it never aligns the Agent as a side effect of Up.
			auto const centreX = sector->getPosition().x + 1.f;
			auto const platform = sector->getStairwell()->getLandingPaths(0).front();
			auto const platformLength = std::abs(platform[1].x - platform[0].x);
			if (std::abs(position.y - std::round(position.y)) <= PoseFitTolerance
				&& std::abs(position.x - centreX) > platformLength + PoseFitTolerance)
			{
				auto envelope = agent.getPoseEnvelope(Pose::Standing);
				auto const halfWidth = envelope->x * .5f;
				if (envelope->x > sector->getSize().x - 2.f * CORE_DOOR_X_INSET) return;
				auto target = position;
				target.x = std::clamp(position.x + mPlayerMovementDirection * agent.getWalkSpeed() * timestep,
					sector->getPosition().x + CORE_DOOR_X_INSET + halfWidth,
					sector->getPosition().x + sector->getSize().x - CORE_DOOR_X_INSET - halfWidth);
				if ((position.x - centreX) * (target.x - centreX) <= 0.f) target.x = centreX;
				agent.setPosition({const_cast<StairwellTransit*>(sector), target - sector->getPosition()}, false);
				if (target.x == centreX) agent.mPlayerStairApproach = mPlayerMovementDirection;
				agent.mPose = Pose::Standing;
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
		if (!mPlayerMovementDirection || agent.mState == Agent::State::AwaitingTraversalCommit) return;
		auto const position = agent.getGlobalPosition();
		auto const& task = *agent.mTraversalTask;
		auto const forwardX = task.destinationVertex->getPosition().x - task.sourceVertex->getPosition().x;
		bool const forward = mPlayerMovementDirection * forwardX > 0.f;
		auto const target = forward ? task.destinationVertex : task.sourceVertex;
		auto const rise = target->getPosition().y - position.y;
		// A reversal in a narrow stairwell is safe only while alone on the span.
		if (auto resource = mTraversalResources.find(task.edge->getTraversalResourceId()); resource && resource->mStairwell)
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
		auto const speed = task.edge->getType() == EdgeType::Location
			? agent.getWalkSpeed() : agent.getStationaryStairSpeed(rise > 0.f);
		if (agent.moveToPosition(target->getPosition(), timestep, speed))
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

	void World::advancePlayerControlledAgent(AgentId id, float timestep)
	{
		auto* agent = mAgents.find(id);
		if (!agent || !agent->isActive() || !agent->isPlayerControlled()) return;
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
		if (!mPlayerMovementDirection) return;
		auto* source = agent->getSector();
		if (!source || !isLocationLike(source->getType())) return;
		auto position = agent->getGlobalPosition();
		auto const level = static_cast<int>(std::round(position.y));
		if (level < 0 || level >= static_cast<int>(getLevelsHigh())
			|| std::abs(position.y - level) > PoseFitTolerance) return;
		auto const layer = getLayer(source->getLayerIndex());
		auto const grants = effectiveAccessGrants(*agent);
		auto fits = [&](Sector const& sector)
		{
			auto const ceiling = sector.getPosition().y + sector.getLevelsHigh() - 1
				+ sector.getEffectiveTopLevelHeight();
			return agent->poseFits(Pose::Standing, ceiling - position.y);
		};
		// A lowered environmental starting pose may be retained, but never used
		// to turn manual input into crouching/crawling locomotion.
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
					if (!door || door->isChamberOwned() || !door->admitsNewCrossings()
						|| !door->admitsStandingHeight(agent->getStandingHeight(), position.y)
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
		float targetX = position.x + direction * agent->getWalkSpeed() * timestep;
		auto const stairMount = playerStaircaseApproach(*agent);
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
			if (!next && stairMount && current == source
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
				agent->mPlayerStaircaseEntry = true;
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
		agent->mPose = Pose::Standing;
	}
}
