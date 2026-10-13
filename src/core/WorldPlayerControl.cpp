#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/Location.h"
#include "core/DoorEdge.h"
#include "core/Graph.h"
#include "core/MobilityProfile.h"

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

	void World::clearPlayerInput()
	{
		mPlayerMovementDirection = 0;
		mPlayerDoorAction = {};
	}

	void World::requestPlayerDoorAction()
	{
		if (!isSimulationPaused()) mPlayerDoorAction = getPlayerControlledAgent();
	}

	void World::performPlayerDoorAction()
	{
		auto const id = std::exchange(mPlayerDoorAction, AgentId{});
		auto* agent = mAgents.find(id);
		if (!agent || id != getPlayerControlledAgent() || !agent->isActive()
			|| agent->mTraversalTask || !agent->getSector()) return;
		auto const position = agent->getGlobalPosition();
		struct Candidate
		{
			std::shared_ptr<const DoorEdge> edge;
			std::shared_ptr<const Vertex> source, destination;
			float distance;
			bool passable, openable;
		};
		std::vector<Candidate> candidates;
		for (auto const& edge : getGraph()->getEdges())
		{
			auto ordinary = std::dynamic_pointer_cast<DoorEdge const>(edge);
			if (!ordinary) continue;
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
			if (passable || openable) candidates.push_back({ordinary, source, destination,
				std::abs(position.x - (door->getPosition().x + door->getSize().x * .5f)), passable, openable});
		}
		std::sort(candidates.begin(), candidates.end(), [](auto const& a, auto const& b) {
			if (a.passable != b.passable) return a.passable > b.passable;
			if (a.distance != b.distance) return a.distance < b.distance;
			return a.edge->getTraversalResourceId() < b.edge->getTraversalResourceId();
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
			task.crossingPose = candidate.edge->getDoor()->selectAgentCrossing(*agent, position.y, true);
			task.traversalTicksRemaining = Door::crossingDurationTicks(task.crossingPose->speedRatio);
			finishFurnitureUse(id);
			agent->mOccupiedUsablePoint = {};
			agent->mRetainedActionPose = false;
			agent->mPose = Pose::Standing;
			agent->mTraversalTask = std::move(task);
			agent->mState = Agent::State::TraversingEdge;
			return;
		}
		for (auto const& candidate : candidates)
			if (candidate.openable) { candidate.edge->getDoor()->requestOpen(); return; }
	}

	void World::advancePlayerControlledAgent(AgentId id, float timestep)
	{
		auto* agent = mAgents.find(id);
		if (!agent || !agent->isActive() || !agent->isPlayerControlled()) return;
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
		int x = static_cast<int>(std::floor(position.x));
		auto* current = cellSector(x);
		if (!current) return;
		// Sweep the leading edge across every crossed cell, not just the endpoint:
		// fast Agents cannot tunnel through walls, low Rooms or Floor gaps.
		while (true)
		{
			float const boundary = direction > 0 ? float(x + 1) : float(x);
			auto* next = cellSector(x + direction);
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
