#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include "core/LadderSectorObject.h"
#include "core/Button.h"

namespace
{

	bool roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints()
	{
		core::World world("Room Ladder editing", 8, 6);
		auto room = world.addRoom("Ladder room", 0, 0, 0, 5, 5);
		world.addSectorWalkway(room, 2, 1);
		world.addSectorWalkway(room, 4, 1);
		world.addSectorWalkway(room, 3, 3);
		world.addSectorWalkway(room, 3, 4);
		uint32_t height = 0; std::string diagnostic;
		if (!world.canAddRoomLadder(room, 0, 1, &height, &diagnostic) || height != 3) return false;
		auto lower = world.addRoomLadder(room, 0, 1);
		auto upper = world.addRoomLadder(room, 2, 1);
		core::World::CreateLadderOptions defaultOptions{};
		if (!world.getRoomLadderOptions(room, lower.ladder.index, defaultOptions))
			return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			lower.ladder.sector->getObject(lower.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (std::static_pointer_cast<const core::LadderSectorObject>(
			upper.ladder.sector->getObject(upper.ladder.index))->getLadder()->getLevelsHigh() != 3) return false;
		if (world.canAddRoomLadder(room, 0, 2, &height, &diagnostic)
			|| diagnostic.find("No Walkway") == std::string::npos) return false;
		auto corridor = world.addCorridor(5, 0, 3);
		if (world.canAddRoomLadder(corridor, 0, 0, &height, &diagnostic)) return false;

		world.finishBuild();
		world.pauseSimulation();
		auto move = world.planMoveSectorObject(room, lower.ladder.index, 3, 0);
		if (!move.valid || move.previewHeight != 4) return false;
		auto moved = world.applyObjectMove(move);
		if (!moved || std::static_pointer_cast<const core::LadderSectorObject>(moved)
			->getLadder()->getLevelsHigh() != 4) return false;

		auto nearer = world.addSectorWalkway(room, 1, 3);
		auto rebuiltRoom = world.getSector(room);
		std::shared_ptr<const core::LadderSectorObject> recalculated;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) recalculated = ladder;
		}
		if (!recalculated || recalculated->getLadder()->getLevelsHigh() != 2) return false;
		if (!world.removeSectorWalkway(room, nearer.index)) return false;
		rebuiltRoom = world.getSector(room);
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0)
			{
				if (ladder->getLadder()->getLevelsHigh() != 4) return false;
				movedIndex = i;
			}
		}
		if (movedIndex == ~0u) return false;
		auto edited = world.applyRoomLadderOptions(room, movedIndex, { 0, true, false, 2 });
		if (!edited) return false;
		core::World::CreateLadderOptions options{};
		rebuiltRoom = world.getSector(room);
		movedIndex = ~0u;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
			if (rebuiltRoom->getObject(i) == edited) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.getRoomLadderOptions(room, movedIndex, options)
			|| !options.extensible || options.startExtended
			|| options.directionalBatchLimit != 2) return false;
		uint32_t insetControls = 0;
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto control = rebuiltRoom->getObject(i);
			if (!control || control->getObjectType() != core::SectorObjectType::InteractionPoint
				|| control->getCellX() != 3 || (control->getCellY() != 0 && control->getCellY() != 3)) continue;
			auto button = control->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 3.8f) < 0.0001f) ++insetControls;
		}
		if (insetControls != 2) return false;
		if (!world.removeRoomLadder(room, movedIndex)) return false;
		rebuiltRoom = world.getSector(room);
		for (uint32_t i = 0; i < rebuiltRoom->getNumObjects(); ++i)
		{
			auto ladder = std::dynamic_pointer_cast<const core::LadderSectorObject>(rebuiltRoom->getObject(i));
			if (ladder && ladder->getCellX() == 3 && ladder->getCellY() == 0) return false;
		}
		auto edge = world.addRoomLadder(room, 0, 4, { 0, true, true });
		for (auto const& control : edge.controls)
		{
			auto button = control.sector->getObject(control.index)->_getObject();
			float centerX = button->getPosition().x + button->getSize().x * 0.5f;
			if (std::abs(centerX - 4.2f) >= 0.0001f) return false;
		}
		return true;
	}

	bool finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed()
	{
		core::World world("Finite ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(1, 0, 3);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();
		if (!created.traversalResource) return false;

		auto const& graph = world.getGraph();
		auto target = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 1.0f });
		if (!target) return false;
		std::vector<core::AgentId> ids = {
			world.createAgent("First climber", lower, 0, 1.5f),
			world.createAgent("Second climber", lower, 0, 1.5f),
			world.createAgent("Cancelled climber", lower, 0, 1.5f)
		};
		for (auto id : ids)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFull = false;
		bool observedQueuePosition = false;
		bool cancelledWaiter = false;
		uint64_t climbStarted = 0;
		uint64_t climbFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isLadder
				|| std::abs(resource->agentSpacing
					- CORE_LADDER_AGENT_SPACING / CORE_CELL_YX_RENDER_RATIO) > 0.001f
				|| resource->capacity != 1 || resource->queueLanes.size() != 2
				|| resource->occupantCount + resource->admissionReservationCount > resource->capacity
				|| resource->capacityPositions.size() != resource->capacity)
				return false;
			for (auto const& lane : resource->queueLanes)
				if (any_of(lane.positions.begin(), lane.positions.end(), [&](auto const& position)
					{ return position.position.distanceTo(lane.origin) <= 0.001f; })) return false;
			observedQueuePosition = observedQueuePosition
				|| any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request) { return request.resource == created.traversalResource
						&& request.hasQueuePosition && !request.hasCapacityPosition; });
			if (resource->occupantCount == 1)
			{
				observedFull = true;
				if (!climbStarted && world.lookupAgent(ids[0]).entity->getState()
					== core::Agent::State::TraversingEdge)
					climbStarted = world.getSimulationTick();
				if (!cancelledWaiter)
				{
					world.lookupAgent(ids[2]).entity->clearPath();
					cancelledWaiter = true;
				}
			}
			if (climbStarted && !climbFinished
				&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get())
				climbFinished = world.getSimulationTick();
			if (world.lookupAgent(ids[0]).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(ids[1]).entity->getState() == core::Agent::State::Idle)
				break;
		}

		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		// One vertical unit at 0.25 units/second requires about 240 fixed ticks;
		// this also detects accidentally using walking speed.
		return observedFull && observedQueuePosition && cancelledWaiter && climbStarted && climbFinished
			&& climbFinished - climbStarted >= 230
			&& world.lookupAgent(ids[0]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[1]).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(ids[2]).entity->getSector() == world.getSector(lower).get()
			&& resource != snapshot.traversalResources.end()
			&& resource->occupantCount == 0 && resource->admissionReservationCount == 0
			&& resource->admissionQueue.empty();
	}

	bool ladderQueuePositionsPreferAgentApproachSide()
	{
		core::World world("Ladder queue approach", 8, 4);
		auto lower = world.addCorridor(0, 0, 7);
		auto upper = world.addCorridor(1, 0, 7);
		core::World::CreateLadderOptions options{ 2, false, true };
		auto created = world.addLadder(1, 0, 3, options);
		uint32_t lowerApproachId, upperApproachId;
		world.addSectorMarker(lower, 0, 1.0f, &lowerApproachId);
		world.addSectorMarker(upper, 0, 6.0f, &upperApproachId);
		world.finishBuild();

		auto lowerApproach = world.getGraph()->getVertexByIdentifier(lowerApproachId);
		auto upperApproach = world.getGraph()->getVertexByIdentifier(upperApproachId);
		auto upperTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 3.5f, 1.0f });
		auto lowerTarget = world.getGraph()->getClosestVertexInSector(
			world.getSector(lower).get(), { 3.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;

		// Occupy the sole Ladder position so later Agents must claim queue spots
		// before entering the queue footprint.
		auto blocker = world.createAgent("Current climber", lower, 0, 3.5f);
		auto blockerEntity = world.lookupAgent(blocker).entity;
		auto blockerPath = world.getGraph()->calculatePath(blockerEntity, upperTarget);
		if (!blockerPath) return false;
		blockerEntity->setPath(std::move(blockerPath), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks
			&& blockerEntity->getSector() != created.ladder.sector.get(); ++tick)
			world.advanceTick();
		if (blockerEntity->getSector() != created.ladder.sector.get()) return false;

		auto lowerAgent = world.createAgent("Lower left approach", lower, 0, 1.0f);
		auto upperAgent = world.createAgent("Upper right approach", upper, 0, 6.0f);
		auto assignPath = [&](core::AgentId id, auto const& source, auto const& target)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, source, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
			return true;
		};
		if (!assignPath(lowerAgent, lowerApproach, upperTarget)
			|| !assignPath(upperAgent, upperApproach, lowerTarget)) return false;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalRequests.size() < 2) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end()) return false;
			auto lowerRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == lowerAgent; });
			auto upperRequest = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == upperAgent; });
			if (lowerRequest == snapshot.traversalRequests.end()
				|| upperRequest == snapshot.traversalRequests.end()
				|| !lowerRequest->hasQueuePosition || !upperRequest->hasQueuePosition) continue;
			auto const& lowerLane = resource->queueLanes[lowerRequest->queueApproach];
			auto const& upperLane = resource->queueLanes[upperRequest->queueApproach];
			auto const lowerSpot = lowerLane.positions[lowerRequest->queuePosition].position;
			auto const upperSpot = upperLane.positions[upperRequest->queuePosition].position;
			auto lowerSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == lowerAgent; });
			auto upperSnapshot = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& agent) { return agent.id == upperAgent; });
			return lowerSnapshot != snapshot.agents.end() && upperSnapshot != snapshot.agents.end()
				&& lowerSnapshot->globalPosition.x < lowerLane.origin.x
				&& upperSnapshot->globalPosition.x > upperLane.origin.x
				&& lowerSpot.x >= lowerSnapshot->globalPosition.x - 0.001f
				&& upperSpot.x <= upperSnapshot->globalPosition.x + 0.001f
				&& lowerSpot.x < lowerLane.origin.x && upperSpot.x > upperLane.origin.x;
		}
		return false;
	}

	bool ladderAdmissionsMaintainPhysicalSpacing()
	{
		// Mirrors resources/test-worlds/sector-ladder-test-1.world.yaml: twelve Agents cross
		// a four-level Ladder in both directions. Admission must stagger entry so
		// equal-speed climbers never overlap on the span.
		core::World world("Ladder spacing", 16, 6);
		auto lower = world.addCorridor(1, 0, 16);
		auto upper = world.addCorridor(4, 0, 16);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 1, 8, options);
		world.finishBuild();
		if (!created.traversalResource || !created.ladder.sector) return false;

		auto graph = world.getGraph();
		auto upperRight = graph->getClosestVertexInSector(world.getSector(upper).get(), { 15.5f, 4.0f });
		auto upperLeft = graph->getClosestVertexInSector(world.getSector(upper).get(), { 0.5f, 4.0f });
		auto lowerRight = graph->getClosestVertexInSector(world.getSector(lower).get(), { 15.5f, 1.0f });
		auto lowerLeft = graph->getClosestVertexInSector(world.getSector(lower).get(), { 0.5f, 1.0f });
		if (!upperRight || !upperLeft || !lowerRight || !lowerLeft) return false;

		std::vector<core::AgentId> ids;
		auto addAgent = [&](char const* name, uint32_t sector, float x,
			std::shared_ptr<const core::Vertex> const& target)
		{
			auto id = world.createAgent(name, sector, 0, x);
			auto agent = world.lookupAgent(id).entity;
			if (!agent) return false;
			auto path = graph->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			ids.push_back(id);
			return true;
		};
		if (!addAgent("Lower Left 1", lower, 1.25f, upperRight)
			|| !addAgent("Lower Left 2", lower, 2.0f, upperRight)
			|| !addAgent("Lower Left 3", lower, 2.75f, upperRight)
			|| !addAgent("Lower Right 1", lower, 14.75f, upperLeft)
			|| !addAgent("Lower Right 2", lower, 14.0f, upperLeft)
			|| !addAgent("Lower Right 3", lower, 13.25f, upperLeft)
			|| !addAgent("Upper Left 1", upper, 1.25f, lowerRight)
			|| !addAgent("Upper Left 2", upper, 2.0f, lowerRight)
			|| !addAgent("Upper Left 3", upper, 2.75f, lowerRight)
			|| !addAgent("Upper Right 1", upper, 14.75f, lowerLeft)
			|| !addAgent("Upper Right 2", upper, 14.0f, lowerLeft)
			|| !addAgent("Upper Right 3", upper, 13.25f, lowerLeft)) return false;

		auto ladderSector = created.ladder.sector;
		float minimumSeparation = std::numeric_limits<float>::max();
		bool observedConcurrentClimbers = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 8; ++i)
		{
			world.advanceTick();
			std::vector<core::Vector2> climbers;
			for (auto id : ids)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() == ladderSector.get())
					climbers.push_back(agent->getGlobalPosition());
			}
			observedConcurrentClimbers = observedConcurrentClimbers || climbers.size() > 1;
			for (uint32_t a = 0; a < climbers.size(); ++a)
				for (uint32_t b = a + 1; b < climbers.size(); ++b)
					minimumSeparation = std::min(minimumSeparation,
						climbers[a].distanceTo(climbers[b]));
			if (std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; }))
				break;
		}
		return observedConcurrentClimbers
			&& minimumSeparation >= CORE_AGENT_MAX_HEIGHT - 0.001f
			&& std::all_of(ids.begin(), ids.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getState() == core::Agent::State::Idle; });
	}

	bool extensibleLadderUsesDesiredStateAndLeases()
	{
		core::World world("Extensible ladder", 4, 4);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(2, 0, 3);
		core::World::CreateLadderOptions options{ 3, true, false };
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(upper).get(), { 1.5f, 2.0f });
		auto first = world.createAgent("Extension owner", lower, 0, 1.5f);
		auto second = world.createAgent("Shared extension owner", lower, 0, 1.5f);
		for (auto id : { first, second })
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool sawSharedOperation = false;
		bool sawLease = false;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 3; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || !resource->isExtensible) return false;
			sawLease = sawLease || resource->extensionRequestLeaseCount > 0
				|| resource->extensionOccupantLeaseCount > 0;
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SetExtendedState
					&& operation.command.desiredState && operation.requesters.size() == 2)
					sawSharedOperation = true;
			if (world.lookupAgent(first).entity->getState() == core::Agent::State::Idle
				&& world.lookupAgent(second).entity->getState() == core::Agent::State::Idle) break;
		}
		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == created.traversalResource; });
		return sawSharedOperation && sawLease && resource != snapshot.traversalResources.end()
			&& resource->extended && resource->extensionRequestLeaseCount == 0
			&& resource->extensionOccupantLeaseCount == 0
			&& world.lookupAgent(first).entity->getSector() == world.getSector(upper).get()
			&& world.lookupAgent(second).entity->getSector() == world.getSector(upper).get();
	}

	bool directionalLadderBoundsBatchesAndPreventsOpposingAdmission()
	{
		core::World world("Directional ladder", 4, 5);
		// Bounded-batch fairness applies to waiters who keep their queue tickets.
		auto waiting = world.getTraversalWaitingPolicy();
		waiting.minimumReplanWaitTicks = MaximumSimulationTicks * 4 + 1;
		world.setTraversalWaitingPolicy(waiting);
		auto lower = world.addCorridor(0, 0, 3);
		auto upper = world.addCorridor(3, 0, 3);
		core::World::CreateLadderOptions options{ 4, false, true };
		options.directionalBatchLimit = 4;
		auto created = world.addLadder(1, 0, 1, options);
		world.finishBuild();

		auto graph = world.getGraph();
		auto upperTarget = graph->getClosestVertexInSector(world.getSector(upper).get(), { 1.5f, 3.0f });
		auto lowerTarget = graph->getClosestVertexInSector(world.getSector(lower).get(), { 1.5f, 0.0f });
		if (!upperTarget || !lowerTarget) return false;
		std::vector<core::AgentId> ascending = {
			world.createAgent("Ascending one", lower, 0, 1.5f),
			world.createAgent("Ascending two", lower, 0, 1.5f),
			world.createAgent("Ascending three", lower, 0, 1.5f),
			world.createAgent("Ascending four", lower, 0, 1.5f),
			world.createAgent("Ascending next batch", lower, 0, 1.5f)
		};
		auto descending = world.createAgent("Descending waiter", upper, 0, 1.5f);
		for (auto id : ascending)
		{
			auto agent = world.lookupAgent(id).entity;
			auto path = graph->calculatePath(agent, upperTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}
		{
			auto agent = world.lookupAgent(descending).entity;
			auto path = graph->calculatePath(agent, lowerTarget);
			if (!path) return false;
			agent->setPath(path, true);
		}

		bool observedFourConcurrent = false;
		bool observedFullBatch = false;
		uint64_t fourthAscendingFinished = 0;
		uint64_t descendingFinished = 0;
		uint64_t fifthAscendingFinished = 0;
		for (uint32_t i = 0; i < MaximumSimulationTicks * 4; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			if (resource == snapshot.traversalResources.end() || resource->capacity != 5
				|| resource->occupantCount + resource->admissionReservationCount > 5
				|| resource->directionalBatchLimit != 4) return false;

			core::TraversalDirection admittedDirection = core::TraversalDirection::None;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.resource != created.traversalResource || !request.hasCapacityPosition) continue;
				if (admittedDirection != core::TraversalDirection::None
					&& admittedDirection != request.direction) return false;
				admittedDirection = request.direction;
			}
			if (resource->activeDirection != core::TraversalDirection::None
				&& admittedDirection != core::TraversalDirection::None
				&& resource->activeDirection != admittedDirection) return false;
			observedFourConcurrent = observedFourConcurrent
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->occupantCount + resource->admissionReservationCount == 4);
			observedFullBatch = observedFullBatch
				|| (resource->activeDirection == core::TraversalDirection::Ascending
					&& resource->descendingWaitingCount == 1
					&& resource->directionalBatchCount == 4);
			if (!fourthAscendingFinished && world.lookupAgent(ascending[3]).entity->getState() == core::Agent::State::Idle)
				fourthAscendingFinished = world.getSimulationTick();
			if (!descendingFinished && world.lookupAgent(descending).entity->getState() == core::Agent::State::Idle)
				descendingFinished = world.getSimulationTick();
			if (!fifthAscendingFinished && world.lookupAgent(ascending[4]).entity->getState() == core::Agent::State::Idle)
				fifthAscendingFinished = world.getSimulationTick();
			if (fourthAscendingFinished && descendingFinished && fifthAscendingFinished) break;
		}
		return observedFourConcurrent && observedFullBatch
			&& fourthAscendingFinished && descendingFinished && fifthAscendingFinished
			&& fourthAscendingFinished < descendingFinished
			&& descendingFinished < fifthAscendingFinished;
	}
}

void registerLadders(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints", [](smoke::Context const&)
		{
			smoke::require(roomLadderEditingCalculatesAndMaintainsWalkwayEndpoints(), "Room Ladder placement, stacking, movement, or Walkway recalculation failed");
		} });
	checks.push_back({ "finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed", [](smoke::Context const&)
		{
			smoke::require(finiteCapacityLadderSerializesAdmissionAndClimbsAtConfiguredSpeed(), "finite ladder capacity, reservations, cancellation, or climb speed failed");
		} });
	checks.push_back({ "ladderQueuePositionsPreferAgentApproachSide", [](smoke::Context const&)
		{
			smoke::require(ladderQueuePositionsPreferAgentApproachSide(), "Ladder queue spots ignored the Agents' approach sides");
		} });
	checks.push_back({ "ladderAdmissionsMaintainPhysicalSpacing", [](smoke::Context const&)
		{
			smoke::require(ladderAdmissionsMaintainPhysicalSpacing(), "Ladder admissions overlapped climbers on the span");
		} });
	checks.push_back({ "extensibleLadderUsesDesiredStateAndLeases", [](smoke::Context const&)
		{
			smoke::require(extensibleLadderUsesDesiredStateAndLeases(), "extensible ladder preparation or leases failed");
		} });
	checks.push_back({ "directionalLadderBoundsBatchesAndPreventsOpposingAdmission", [](smoke::Context const&)
		{
			smoke::require(directionalLadderBoundsBatchesAndPreventsOpposingAdmission(), "directional ladder admission or bounded-batch fairness failed");
		} });
}
