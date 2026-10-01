#include "Checks.h"
#include "PathFixture.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <map>
#include <vector>
#include "core/SectorEdge.h"

namespace
{
	using smoke::twoNodePath;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	bool fairDoorQueuesServeBothSidesInStableOrder()
	{
		core::World world("Fair two-sided door", 8, 2);
		auto fore = world.addRoom("Fore queue", 0, 0, 0, 7, 1);
		auto back = world.addRoom("Back queue", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		auto created = world.addSectorDoor(0, 0, 3, options);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto foreVertex = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto backVertex = edge->getOtherVertex(foreVertex);

		std::vector<core::AgentId> ids = {
			world.createAgent("Fore first", fore, 0, 3.5f),
			world.createAgent("Back first", back, 0, 3.5f),
			world.createAgent("Fore second", fore, 0, 3.5f),
			world.createAgent("Back second", back, 0, 3.5f)
		};
		for (size_t i = 0; i < ids.size(); ++i)
		{
			auto source = i % 2 == 0 ? foreVertex : backVertex;
			auto destination = i % 2 == 0 ? backVertex : foreVertex;
			world.lookupAgent(ids[i]).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		bool observedSeparatedPositions = false;
		bool observedQueueDiagnostics = false;
		std::vector<core::AgentId> completionOrder;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 2 && completionOrder.size() < ids.size(); ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalPermits.size() > 1)
			{
				return false;
			}
			auto const& resource = snapshot.traversalResources.front();
			observedQueueDiagnostics = observedQueueDiagnostics
				|| (resource.queueLanes.size() == 2 && resource.crossingOwner);
			for (auto const& lane : resource.queueLanes)
			{
				std::vector<core::Vector2> occupied;
				for (auto const& position : lane.positions)
				{
					if (!position.owner)
					{
						continue;
					}
					for (auto const& other : occupied)
					{
						if (position.position.distanceTo(other) < CORE_DOOR_QUEUE_STOP_WIDTH - 0.001f)
						{
							return false;
						}
					}
					occupied.push_back(position.position);
				}
				observedSeparatedPositions = observedSeparatedPositions || occupied.size() >= 2;
			}
			for (auto id : ids)
			{
				if (std::find(completionOrder.begin(), completionOrder.end(), id) == completionOrder.end()
					&& world.lookupAgent(id).entity->getState() == core::Agent::State::Idle)
				{
					completionOrder.push_back(id);
				}
			}
		}
		return completionOrder == ids && observedSeparatedPositions && observedQueueDiagnostics
			&& world.getSimulationSnapshot().traversalResources.front().crossingOwner == core::TraversalRequestId{};
	}

	// #172: start with separated Agents (arbitrary overlapping spawn positions
	// cannot be repaired instantaneously without teleporting). Check every tick,
	// not just the settled reservation geometry, through the complete service.
	bool queueChainsFollowWithoutCompressing(float separation, int direction,
		std::vector<uint32_t>& trace)
	{
		core::World world("Following queue", 16, 2);
		auto fore = world.addRoom("Fore", 0, 0, 0, 15, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 15, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		world.addSectorDoor(0, 0, 7, options);
		auto policy = world.getTraversalGeometryPolicy();
		policy.minimumQueueSeparation = separation;
		policy.advanceStepThreshold = 0.2f;
		world.setTraversalGeometryPolicy(policy);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids;
		for (auto sector : { fore, back })
			for (int i = 0; i < 4; ++i)
			{
				auto id = world.createAgent("Waiter", sector, 0, 7.5f + direction * i * (separation + 0.1f));
				ids.push_back(id);
				world.lookupAgent(id).entity->setPath(sector == fore
					? twoNodePath(source, destination, edge) : twoNodePath(destination, source, edge), true);
			}
		bool sawChain = false;
		bool sawDelayedAdvance = false;
		uint32_t calmQueueTicks = 0;
		uint32_t longestCalmQueueRun = 0;
		std::map<core::AgentId, core::Vector2> previousPositions;
		for (auto id : ids) previousPositions[id] = world.lookupAgent(id).entity->getGlobalPosition();
		std::map<core::AgentId, core::Vector2> previousTargets;
		std::map<core::AgentId, uint64_t> previousAssignmentTicks;
		std::map<core::AgentId, uint32_t> previousQueuePositions;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 3; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			std::map<core::AgentId, core::Vector2> targets;
			std::map<core::AgentId, uint64_t> assignmentTicks;
			std::map<core::AgentId, uint32_t> queuePositions;
			bool calmQueueThisTick = false;
			for (auto const& lane : snapshot.traversalResources.front().queueLanes)
			{
				std::vector<core::TraversalRequestSnapshot const*> waiters;
				for (auto const& request : snapshot.traversalRequests)
					if (request.hasQueuePosition && request.sourceSector == lane.sector)
						waiters.push_back(&request);
				std::sort(waiters.begin(), waiters.end(), [](auto a, auto b) { return a->queueTicket < b->queueTicket; });
				sawChain = sawChain || waiters.size() >= 3;
				bool laneIsCalm = waiters.size() >= 3;
				for (size_t i = 0; i < waiters.size(); ++i)
				{
					auto const& request = *waiters[i];
					targets[request.owner] = request.queueStandingTarget;
					assignmentTicks[request.owner] = request.positionAssignedAtTick;
					queuePositions[request.owner] = request.queuePosition;
					auto previousTarget = previousTargets.find(request.owner);
					auto previousAssignment = previousAssignmentTicks.find(request.owner);
					auto previousPosition = previousQueuePositions.find(request.owner);
					if (previousTarget == previousTargets.end()
						|| previousAssignment == previousAssignmentTicks.end()
						|| previousPosition == previousQueuePositions.end()
						|| previousTarget->second.distanceTo(request.queueStandingTarget) > 0.001f
						|| previousPosition->second != request.queuePosition)
						laneIsCalm = false;
					else if (previousAssignment->second != request.positionAssignedAtTick)
						return false; // an unchanged standing position was reassigned
					if (previousTarget != previousTargets.end()
						&& previousTarget->second.distanceTo(request.queueStandingTarget) > 0.001f
						&& request.positionAssignedAtTick != snapshot.tick)
						return false;
					if (i == 0) continue;
					auto const& ahead = *waiters[i - 1];
					if (direction * (request.queueStandingTarget.x - ahead.queueStandingTarget.x)
						< separation - 0.001f) return false;
					if (previousTarget != previousTargets.end())
					{
						auto const advance = direction
							* (previousTarget->second.x - request.queueStandingTarget.x);
						if (advance > 0.001f
							&& advance <= policy.advanceStepThreshold + 0.001f) return false;
					}
					auto agent = world.lookupAgent(request.owner).entity;
					auto leader = world.lookupAgent(ahead.owner).entity;
					if (direction * (agent->getGlobalPosition().x - leader->getGlobalPosition().x)
						< separation - 0.001f)
					{
						std::cerr << "Queue gap at tick " << tick << ", separation " << separation
							<< ", direction " << direction << ", Agent " << request.owner.value
							<< " x=" << agent->getGlobalPosition().x << ", leader x="
							<< leader->getGlobalPosition().x << '\n';
						return false;
					}
					if (i == 1 && ahead.owner != ids[0] && ahead.owner != ids[4]
						&& previousTargets.contains(ahead.owner) && previousTargets.contains(request.owner)
						&& ahead.queueStandingTarget.distanceTo(previousTargets[ahead.owner]) > 0.001f
						&& request.queueStandingTarget.distanceTo(previousTargets[request.owner]) <= 0.001f)
						sawDelayedAdvance = true;
				}
				calmQueueThisTick = calmQueueThisTick || laneIsCalm;
			}
			calmQueueTicks = calmQueueThisTick ? calmQueueTicks + 1 : 0;
			longestCalmQueueRun = std::max(longestCalmQueueRun, calmQueueTicks);
			previousTargets = std::move(targets);
			previousAssignmentTicks = std::move(assignmentTicks);
			previousQueuePositions = std::move(queuePositions);
			bool finished = true;
			for (auto id : ids)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getGlobalPosition().distanceTo(previousPositions[id])
					> agent->getWalkSpeed() * core::World::getFixedTimestep() + 0.001f) return false;
				previousPositions[id] = agent->getGlobalPosition();
				trace.push_back(std::bit_cast<uint32_t>(agent->getGlobalPosition().x));
				trace.push_back((uint32_t)agent->getState());
				finished = finished && agent->getState() == core::Agent::State::Idle;
			}
			if (finished)
			{
				if (!sawChain || !sawDelayedAdvance || longestCalmQueueRun < 2)
					std::cerr << "Queue observations: chain=" << sawChain << ", delayed="
						<< sawDelayedAdvance << ", calm ticks=" << longestCalmQueueRun << '\n';
				return sawChain && sawDelayedAdvance && longestCalmQueueRun >= 2;
			}
		}
		return false;
	}

	bool overflowingQueueAlwaysHasWalkableTailTargets(std::vector<uint32_t>& trace)
	{
		core::World world("Overflow tail", 8, 2);
		auto fore = world.addRoom("Approach", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		auto policy = world.getTraversalGeometryPolicy();
		policy.overflowTailSeparation = 0.6f;
		world.setTraversalGeometryPolicy(policy);
		world.finishBuild();

		auto initialEdge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);
		world.pauseSimulation();
		if (!world.configureDoorQueueLane(created.traversalResource,
			core::SectorId{ (uint64_t)fore + 1 }, initialSource->getPosition(),
			core::Vector2::NEGATIVE_UNIT_X, 0.0f)
			|| !world.rebuildTraversalTopology() || !world.resumeSimulation()) return false;

		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids;
		for (uint32_t i = 0; i < 8; ++i)
		{
			auto id = world.createAgent("Overflow waiter", fore, 0, source->getSectorOffset().x);
			ids.push_back(id);
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}

		world.advanceTicks(2);
		auto const floorBoundary = CORE_AGENT_MAX_WIDTH * 0.5f;
		bool sawClampedTail = false;
		for (uint32_t tick = 0; tick < 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			if (snapshot.traversalRequests.size() != ids.size()) return false;
			std::vector<core::TraversalRequestSnapshot const*> requests;
			for (auto const& request : snapshot.traversalRequests)
				if (request.queueTicket) requests.push_back(&request);
			if (requests.size() != ids.size()) return false;
			std::sort(requests.begin(), requests.end(), [](auto lhs, auto rhs)
				{ return lhs->queueTicket < rhs->queueTicket; });
			if (std::count_if(requests.begin(), requests.end(), [](auto request)
				{ return request->hasQueuePosition; }) != 1) return false;
			float previousTarget = source->getPosition().x + 0.001f;
			for (auto request : requests)
			{
				if (!request->hasQueueStandingTarget
					|| request->queueStandingTarget.x > previousTarget + 0.001f
					|| request->queueStandingTarget.x < floorBoundary - 0.001f) return false;
				if (!request->hasQueuePosition && (request->positionRetryCount != 0
					|| request->state != core::TraversalRequestState::Pending)) return false;
				previousTarget = request->queueStandingTarget.x;
				sawClampedTail = sawClampedTail
					|| std::abs(previousTarget - floorBoundary) <= 0.001f;
				trace.push_back(std::bit_cast<uint32_t>(request->queueStandingTarget.x));
				trace.push_back(request->hasQueuePosition ? 1u : 0u);
			}
		}
		return sawClampedTail;
	}

	bool queuePositionsPreferObjectProximityThenAgentProximity()
	{
		core::World world("Nearest queue position", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Second waiter", fore, 0, 3.5f),
			world.createAgent("Third waiter", fore, 0, 3.5f)
		};
		for (auto id : ids)
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);

		world.advanceTicks(3);
		auto initial = world.getSimulationSnapshot();
		if (initial.traversalRequests.size() != 3) return false;
		auto initialThird = std::find_if(initial.traversalRequests.begin(), initial.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[2]; });
		if (initialThird == initial.traversalRequests.end() || !initialThird->hasQueuePosition) return false;
		auto const& initialLane = initial.traversalResources.front().queueLanes[initialThird->queueApproach];
		auto initialThirdTarget = initialLane.positions[initialThird->queuePosition].position;

		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto first = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[0]; });
			auto second = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[1]; });
			auto third = std::find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& request) { return request.owner == ids[2]; });
			if (second == snapshot.traversalRequests.end() || third == snapshot.traversalRequests.end()) continue;
			if ((first == snapshot.traversalRequests.end() || !first->hasQueuePosition)
				&& second->hasQueuePosition && third->hasQueuePosition)
			{
				auto const& lane = snapshot.traversalResources.front().queueLanes[third->queueApproach];
				auto secondTarget = lane.positions[second->queuePosition].position;
				auto thirdTarget = lane.positions[third->queuePosition].position;
				return secondTarget.distanceTo(source->getPosition()) <= 0.001f
					&& thirdTarget.distanceTo(initialThirdTarget) <= 0.001f;
			}
		}
		return false;
	}

	bool doorQueueRequestsBeforeOccupiedTail()
	{
		core::World world("Early Door queue", 8, 2);
		auto fore = world.addRoom("Approach", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.activationMode = core::DoorActivationMode::Automatic;
		auto created = world.addSectorDoor(0, 0, 3, options);
		uint32_t approachId;
		world.addSectorMarker(fore, 0, 2.75f, &approachId);
		world.finishBuild();

		auto edge = *find_if(world.getGraph()->getEdges().begin(),
			world.getGraph()->getEdges().end(), [&](auto const& candidate)
				{ return candidate->getTraversalResourceId() == created.traversalResource; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore
			? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto blocker = world.createAgent("Door queue head", fore, 0, source->getSectorOffset().x);
		world.lookupAgent(blocker).entity->setPath(twoNodePath(source, destination, edge), true);
		for (uint32_t tick = 0; tick < 20; ++tick) world.advanceTick();

		auto approach = world.getGraph()->getVertexByIdentifier(approachId);
		auto waiter = world.createAgent("Door waiter", fore, 0, 2.75f);
		auto waiterEntity = world.lookupAgent(waiter).entity;
		auto path = world.getGraph()->calculatePath(waiterEntity, approach, destination);
		if (!path) return false;
		waiterEntity->setPath(std::move(path), true);
		for (uint32_t tick = 0; tick < MaximumSimulationTicks; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto request = find_if(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
				[&](auto const& value) { return value.owner == waiter; });
			if (request == snapshot.traversalRequests.end() || !request->hasQueuePosition) continue;
			auto resource = find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == created.traversalResource; });
			auto agent = find_if(snapshot.agents.begin(), snapshot.agents.end(),
				[&](auto const& value) { return value.id == waiter; });
			if (resource == snapshot.traversalResources.end() || agent == snapshot.agents.end()) return false;
			auto const& lane = resource->queueLanes[request->queueApproach];
			auto const target = lane.positions[request->queuePosition].position;
			return agent->globalPosition.x < lane.origin.x
				&& target.x >= agent->globalPosition.x - 0.001f
				&& target.x <= lane.origin.x + 0.001f;
		}
		return false;
	}

	bool queuedCancellationReleasesAndAdvancesPositions()
	{
		core::World world("Queue cancellation", 8, 2);
		auto fore = world.addRoom("Queue room", 0, 0, 0, 7, 1);
		world.addRoom("Destination", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		auto firstId = world.createAgent("First", fore, 0, 3.5f);
		auto cancelledId = world.createAgent("Cancelled", fore, 0, 3.5f);
		auto lastId = world.createAgent("Last", fore, 0, 3.5f);
		for (auto id : { firstId, cancelledId, lastId })
		{
			world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		}
		world.advanceTicks(3);
		auto before = world.getSimulationSnapshot();
		if (before.traversalRequests.size() != 3
			|| std::count_if(before.traversalRequests.begin(), before.traversalRequests.end(),
				[](auto const& request) { return request.queueTicket && request.hasQueuePosition; }) != 3)
		{
			return false;
		}
		world.lookupAgent(cancelledId).entity->clearPath();
		auto after = world.getSimulationSnapshot();
		if (after.traversalRequests.size() != 2
			|| after.traversalResources.front().queueLanes.front().queue.size() != 2)
		{
			return false;
		}
		for (auto const& position : after.traversalResources.front().queueLanes.front().positions)
		{
			if (position.owner && position.owner == before.traversalRequests[1].id)
			{
				return false;
			}
		}
		world.advanceTicks(MaximumSimulationTicks);
		return world.lookupAgent(firstId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(lastId).entity->getState() == core::Agent::State::Idle
			&& world.lookupAgent(cancelledId).entity->getSector() == world.getSector(fore).get();
	}

	bool resilientWaitingRetainsPriorityAndExpiresPermits()
	{
		core::World world("Resilient door waiting", 8, 2);
		auto fore = world.addRoom("Waiting side", 0, 0, 0, 7, 1);
		world.addRoom("Destination side", 1, 0, 0, 7, 1);
		auto created = world.addSectorDoor(0, 0, 3);
		world.finishBuild();
		auto initialEdge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto initialSource = initialEdge->getVertex(0)->getSector()->getIndex() == fore
			? initialEdge->getVertex(0) : initialEdge->getVertex(1);

		// One physical position deliberately forces logical overflow. Runtime queue
		// geometry is a structural edit and therefore uses the paused rebuild seam.
		world.pauseSimulation();
		auto sourceSectorId = core::SectorId{ (uint64_t)fore + 1 };
		if (!world.configureDoorQueueLane(created.traversalResource, sourceSectorId,
			initialSource->getPosition(), { -1.0f, 0.0f }, 0.0f)
			|| !world.rebuildTraversalTopology() || !world.resumeSimulation()) return false;
		auto edge = *std::find_if(world.getGraph()->getEdges().begin(), world.getGraph()->getEdges().end(),
			[](auto const& candidate) { return candidate->getType() == core::EdgeType::Door; });
		auto source = edge->getVertex(0)->getSector()->getIndex() == fore ? edge->getVertex(0) : edge->getVertex(1);
		auto destination = edge->getOtherVertex(source);
		std::vector<core::AgentId> ids = {
			world.createAgent("Queue head", fore, 0, 3.5f),
			world.createAgent("Overflow one", fore, 0, 3.5f),
			world.createAgent("Overflow two", fore, 0, 3.5f)
		};
		for (auto id : ids) world.lookupAgent(id).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto queued = world.getSimulationSnapshot();
		if (queued.traversalRequests.size() != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return (bool)request.queueTicket; }) != 3
			|| std::count_if(queued.traversalRequests.begin(), queued.traversalRequests.end(),
				[](auto const& request) { return request.hasQueuePosition; }) != 1)
		{
			return false;
		}

		auto firstRequest = queued.traversalRequests.front();
		world.lookupAgent(ids.front()).entity->setPath(twoNodePath(source, destination, edge), true);
		auto compatible = world.getSimulationSnapshot();
		auto retained = std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == compatible.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		// Changing the immediate authority is incompatible and must release the old
		// logical ticket before a later compatible route can queue afresh.
		auto oldOverflow = *std::find_if(compatible.traversalRequests.begin(), compatible.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination,
			std::make_shared<core::SectorEdge>()), true);
		auto incompatible = world.getSimulationSnapshot();
		if (std::any_of(incompatible.traversalRequests.begin(), incompatible.traversalRequests.end(),
			[&](auto const& request) { return request.id == oldOverflow.id; })) return false;
		world.lookupAgent(ids[1]).entity->setPath(twoNodePath(source, destination, edge), true);
		world.advanceTicks(2);
		auto fresh = world.getSimulationSnapshot();
		auto freshOverflow = std::find_if(fresh.traversalRequests.begin(), fresh.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids[1]; });
		if (freshOverflow == fresh.traversalRequests.end() || freshOverflow->queueTicket == oldOverflow.queueTicket)
			return false;

		// Route estimation observes queue demand but creates no coordination state.
		auto requestCount = fresh.traversalRequests.size();
		auto permitCount = fresh.traversalPermits.size();
		auto routeAgent = world.lookupAgent(ids.front()).entity;
		auto const& routePolicy = world.getRouteChoicePolicy();
		core::RouteDecisionContext const routeContext{ routeAgent,
			routePolicy.baselineProfile, routePolicy, routeAgent->getSector(),
			routeAgent->getWalkSpeed(), &world, routeAgent->getClimbSpeed(), false,
			0, 0, routeAgent->getEffectiveMobilityProfile().value };
		auto const routeCost = routePolicy.evaluate(
			edge->getDirectedTraversalFacts(destination, routeContext), routeContext.profile);
		if (!routeCost || routeCost->perceivedCost <= 0.0f
			|| world.getSimulationSnapshot().traversalRequests.size() != requestCount
			|| world.getSimulationSnapshot().traversalPermits.size() != permitCount) return false;

		// A deliberately short no-progress deadline expires the coincident threshold
		// crossing. The request and ticket survive and a fresh permit is assigned.
		auto policy = world.getTraversalWaitingPolicy();
		policy.permitProgressTimeoutTicks = 1;
		world.setTraversalWaitingPolicy(policy);
		core::TraversalPermitId firstPermit;
		core::TraversalPermitId replacementPermit;
		for (uint32_t i = 0; i < MaximumSimulationTicks && !replacementPermit; ++i)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			for (auto const& permit : snapshot.traversalPermits)
			{
				if (permit.owner != ids.front()) continue;
				if (!firstPermit) firstPermit = permit.id;
				else if (permit.id != firstPermit) replacementPermit = permit.id;
			}
		}
		if (!firstPermit || !replacementPermit) return false;
		auto afterExpiry = world.getSimulationSnapshot();
		retained = std::find_if(afterExpiry.traversalRequests.begin(), afterExpiry.traversalRequests.end(),
			[&](auto const& request) { return request.owner == ids.front(); });
		if (retained == afterExpiry.traversalRequests.end() || retained->id != firstRequest.id
			|| retained->queueTicket != firstRequest.queueTicket) return false;

		policy.permitProgressTimeoutTicks = 120;
		world.setTraversalWaitingPolicy(policy);
		world.advanceTicks(MaximumSimulationTicks * 2);
		return std::all_of(ids.begin(), ids.end(), [&](auto id)
		{
			auto agent = world.lookupAgent(id).entity;
			return agent->getState() == core::Agent::State::Idle
				&& agent->getSector() == destination->getSector().get();
		}) && world.getSimulationSnapshot().traversalRequests.empty()
			&& world.getSimulationSnapshot().traversalPermits.empty();
	}
}

void registerDoorQueues(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "fairDoorQueuesServeBothSidesInStableOrder", [](smoke::Context const&)
		{
			smoke::require(fairDoorQueuesServeBothSidesInStableOrder(), "two-sided door queues were not separated, FIFO, or fair");
		} });
	checks.push_back({ "queueChainsFollowWithoutCompressingDefaultLeft", [](smoke::Context const&)
		{
			std::vector<uint32_t> first, repeated;
			smoke::require(queueChainsFollowWithoutCompressing((float)CORE_DOOR_QUEUE_STOP_WIDTH, -1, first)
				&& queueChainsFollowWithoutCompressing((float)CORE_DOOR_QUEUE_STOP_WIDTH, -1, repeated)
				&& first == repeated, "queue chain spacing, staggered advancement or determinism");
		} });
	checks.push_back({ "queueChainsFollowWithoutCompressingDefaultRight", [](smoke::Context const&)
		{
			std::vector<uint32_t> first, repeated;
			smoke::require(queueChainsFollowWithoutCompressing((float)CORE_DOOR_QUEUE_STOP_WIDTH, 1, first)
				&& queueChainsFollowWithoutCompressing((float)CORE_DOOR_QUEUE_STOP_WIDTH, 1, repeated)
				&& first == repeated, "queue chain spacing, staggered advancement or determinism");
		} });
	checks.push_back({ "queueChainsFollowWithoutCompressingWideLeft", [](smoke::Context const&)
		{
			std::vector<uint32_t> first, repeated;
			smoke::require(queueChainsFollowWithoutCompressing(0.8f, -1, first)
				&& queueChainsFollowWithoutCompressing(0.8f, -1, repeated)
				&& first == repeated, "queue chain spacing, staggered advancement or determinism");
		} });
	checks.push_back({ "queueChainsFollowWithoutCompressingWideRight", [](smoke::Context const&)
		{
			std::vector<uint32_t> first, repeated;
			smoke::require(queueChainsFollowWithoutCompressing(0.8f, 1, first)
				&& queueChainsFollowWithoutCompressing(0.8f, 1, repeated)
				&& first == repeated, "queue chain spacing, staggered advancement or determinism");
		} });
	checks.push_back({ "overflowingQueueAlwaysHasWalkableTailTargets", [](smoke::Context const&)
		{
			std::vector<uint32_t> first, repeated;
			smoke::require(overflowingQueueAlwaysHasWalkableTailTargets(first)
				&& overflowingQueueAlwaysHasWalkableTailTargets(repeated)
				&& first == repeated, "overflowing queue lacked deterministic walkable tail targets");
		} });
	checks.push_back({ "queuePositionsPreferObjectProximityThenAgentProximity", [](smoke::Context const&)
		{
			smoke::require(queuePositionsPreferObjectProximityThenAgentProximity(), "queue positions were not selected by object then agent proximity");
		} });
	checks.push_back({ "doorQueueRequestsBeforeOccupiedTail", [](smoke::Context const&)
		{
			smoke::require(doorQueueRequestsBeforeOccupiedTail(), "Door queue request was not made before its occupied tail");
		} });
	checks.push_back({ "queuedCancellationReleasesAndAdvancesPositions", [](smoke::Context const&)
		{
			smoke::require(queuedCancellationReleasesAndAdvancesPositions(), "queued cancellation leaked a ticket or physical position");
		} });
	checks.push_back({ "resilientWaitingRetainsPriorityAndExpiresPermits", [](smoke::Context const&)
		{
			smoke::require(resilientWaitingRetainsPriorityAndExpiresPermits(), "resilient waiting lost priority, leaked reservations, or failed permit expiry");
		} });
}
