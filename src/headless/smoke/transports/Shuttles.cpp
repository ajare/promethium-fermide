#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/Staircase.h"
#include "core/Transit.h"

namespace
{
	bool shuttlePassengerWalksToForwardInteriorSpot()
	{
		core::World world("Shuttle interior walking", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b0001;
		options.minimumDwellSeconds = 0.0f;
		options.maximumBoardingSeconds = 0.1f;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		if (!target) return false;
		auto passengerId = world.createAgent("Walking shuttle passenger", left, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		passenger->setPath(path, true);

		bool boardedWithoutTeleport = false;
		bool selectedForwardmostSpot = false;
		bool reachedInteriorSpot = false;
		bool walkedWhileShuttleMoving = false;
		bool wasOnboard = false;
		float previousX = passenger->getGlobalPosition().x;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()
				|| shuttle->shuttleCarriages.size() != 1
				|| shuttle->shuttleCarriages.front().positions.size() != options.capacity) return false;

			auto onboard = passenger->getSector()
				== world.getSector(created.shuttle.sector->getIndex()).get();
			if (onboard && !wasOnboard)
			{
				boardedWithoutTeleport = std::abs(passenger->getGlobalPosition().x - previousX)
					<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
				auto const& positions = shuttle->shuttleCarriages.front().positions;
				selectedForwardmostSpot = positions.back().occupant == passengerId;
			}
			if (onboard)
			{
				auto const& forward = shuttle->shuttleCarriages.front().positions.back().position;
				auto passengerCarriageX = passenger->getGlobalPosition().x - shuttle->liftPosition;
				reachedInteriorSpot = reachedInteriorSpot
					|| std::abs(passengerCarriageX - forward.x) < 0.01f;
				walkedWhileShuttleMoving = walkedWhileShuttleMoving
					|| (shuttle->liftMoving && passengerCarriageX < forward.x - 0.01f);
			}
			wasOnboard = onboard;
			previousX = passenger->getGlobalPosition().x;
			if (passenger->getState() == core::Agent::State::Idle
				&& passenger->getSector() == world.getSector(right).get()) break;
		}
		return boardedWithoutTeleport && selectedForwardmostSpot && reachedInteriorSpot
			&& walkedWhileShuttleMoving && passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(right).get();
	}

	bool shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed()
	{
		core::World world("Shuttle passenger spacing", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b0001;
		options.minimumDwellSeconds = 20.0f;
		options.maximumBoardingSeconds = 30.0f;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		if (!target) return false;
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Spacing passenger", left, 0, 0.4f + i * 0.7f);
			auto agent = world.lookupAgent(id).entity;
			if (i < 2)
			{
				auto path = world.getGraph()->calculatePath(agent, target);
				if (!path) return false;
				agent->setPath(path, true);
			}
			passengers.push_back(id);
		}

		std::map<core::AgentId, float> previousCarriageX;
		std::map<core::AgentId, float> twoPassengerTargets;
		bool sawTwoPassengers = false;
		bool retargetedExistingPassenger = false;
		bool reachedSpacedPositions = false;
		bool teleportedInside = false;
		bool violatedPassengerBuffer = false;
		bool thirdJourneyStarted = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
					{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()
				|| shuttle->shuttleCarriages.size() != 1) return false;
			auto const& carriage = shuttle->shuttleCarriages.front();

			std::map<core::AgentId, float> currentCarriageX;
			for (auto id : passengers)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() != world.getSector(created.shuttle.sector->getIndex()).get())
					continue;
				auto relativeX = agent->getGlobalPosition().x - shuttle->liftPosition;
				currentCarriageX[id] = relativeX;
				if (auto previous = previousCarriageX.find(id); previous != previousCarriageX.end())
					teleportedInside = teleportedInside
						|| std::abs(relativeX - previous->second)
							> agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
			}
			previousCarriageX = currentCarriageX;
			for (auto left = currentCarriageX.begin(); left != currentCarriageX.end(); ++left)
				for (auto right = std::next(left); right != currentCarriageX.end(); ++right)
					violatedPassengerBuffer = violatedPassengerBuffer
						|| std::abs(left->second - right->second) + 0.001f
							< CORE_AGENT_MAX_WIDTH + CORE_SHUTTLE_AGENT_BUFFER;

			if (carriage.occupantCount == 2 && !thirdJourneyStarted
				&& currentCarriageX.size() == 2)
			{
				std::vector<float> positions;
				for (auto const& [id, x] : currentCarriageX) positions.push_back(x);
				std::sort(positions.begin(), positions.end());
				auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
				auto const first = CORE_SHUTTLE_AGENT_BUFFER + halfWidth;
				auto const last = options.carWidth - CORE_SHUTTLE_AGENT_BUFFER - halfWidth;
				if (std::abs(positions.front() - first) < 0.02f
					&& std::abs(positions.back() - last) < 0.02f)
				{
					sawTwoPassengers = true;
					for (auto const& position : carriage.positions)
						if (position.occupant)
							twoPassengerTargets[position.occupant] = position.position.x;
					auto third = world.lookupAgent(passengers.back()).entity;
					auto path = world.getGraph()->calculatePath(third, target);
					if (!path) return false;
					third->setPath(path, true);
					thirdJourneyStarted = true;
				}
			}
			if (carriage.occupantCount == 3)
			{
				for (auto const& position : carriage.positions)
					if (auto previous = twoPassengerTargets.find(position.occupant);
						previous != twoPassengerTargets.end()
						&& std::abs(previous->second - position.position.x) > 0.1f)
						retargetedExistingPassenger = true;

				std::vector<float> positions;
				for (auto const& [id, x] : currentCarriageX) positions.push_back(x);
				std::sort(positions.begin(), positions.end());
				if (positions.size() == 3)
				{
					auto const halfWidth = CORE_AGENT_MAX_WIDTH * 0.5f;
					auto const first = CORE_SHUTTLE_AGENT_BUFFER + halfWidth;
					auto const last = options.carWidth - CORE_SHUTTLE_AGENT_BUFFER - halfWidth;
					reachedSpacedPositions = reachedSpacedPositions
						|| (std::abs(positions.front() - first) < 0.02f
							&& std::abs(positions[1] - options.carWidth * 0.5f) < 0.02f
							&& std::abs(positions.back() - last) < 0.02f);
				}
			}
			if (reachedSpacedPositions && retargetedExistingPassenger) break;
		}
		return sawTwoPassengers && retargetedExistingPassenger
			&& reachedSpacedPositions && !teleportedInside && !violatedPassengerBuffer;
	}

	bool shuttleBoardingRequiresDoorAlignment()
	{
		core::World world("Shuttle boarding alignment", 16, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
		auto right = world.addRoom("Right", 0, 0, 10, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 10 }, 0 };
		options.capacity = 3;
		options.doorMask = 0b101;
		auto created = world.addShuttle(1, 0, 0, 13, options);
		world.finishBuild();
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 11.5f, 0.0f });
		std::vector<core::AgentId> passengers;
		for (float x : { 0.4f, 1.5f, 2.6f })
		{
			auto id = world.createAgent("Passenger", left, 0, x);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}
		bool sawGrant = false;
		for (unsigned tick = 0; tick < 12000; ++tick)
		{
			std::map<core::AgentId, float> approaching;
			for (auto id : passengers)
			{
				auto agent = world.lookupAgent(id).entity;
				if (agent->getSector() == world.getSector(left).get())
					approaching[id] = agent->getGlobalPosition().x;
			}
			world.advanceTick();
			for (auto const& [id, previousX] : approaching)
			{
				auto agent = world.lookupAgent(id).entity;
				if (std::abs(agent->getGlobalPosition().x - previousX)
					> agent->getWalkSpeed() * world.getFixedTimestep() + 0.001f) return false;
			}
			auto const snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
					{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()) return false;
			for (auto const& request : snapshot.traversalRequests)
			{
				if (request.edgeType != core::EdgeType::Door
					|| request.sourceSector != core::SectorId{ (uint64_t)left + 1 }
					|| request.state != core::TraversalRequestState::Granted) continue;
				sawGrant = true;
				auto agent = world.lookupAgent(request.owner).entity;
				if (!core::isWithinDoorCrossingBand(agent->getGlobalPosition(),
					request.sourceEndpoint, CORE_DOOR_CROSSING_HALF_WIDTH(1))) return false;
			}
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{ return world.lookupAgent(id).entity->getSector() == world.getSector(right).get(); }))
				return sawGrant;
		}
		return false;
	}

	bool shuttlePassengerUsesBoardingSelectedAlightingDoor()
	{
		core::World world("Nearest Shuttle exit", 16, 2);
		auto left = world.addRoom("Left platform", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right platform", 0, 0, 10, 4, 1);
		core::World::CreateShuttleOptions options{ 1, 4, { 0, 10 }, 0 };
		options.capacity = 1;
		options.doorMask = 0b1001;
		auto created = world.addShuttle(1, 0, 0, 15, options);
		world.finishBuild();
		if (created.doors.size() != 4) return false;

		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 10.5f, 0.0f });
		if (!target) return false;
		auto passengerId = world.createAgent("Nearest-exit passenger", left, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path) return false;
		auto const plannedDoor = created.doors[2].traversalResource;
		auto const nearestDoor = created.doors[3].traversalResource;
		bool plannedLeftDoor = std::any_of(path->nodes.begin(), path->nodes.end(),
			[&](auto const& node)
				{ return node.edge && node.edge->getTraversalResourceId() == plannedDoor; });
		if (!plannedLeftDoor) return false;
		passenger->setPath(path, true);

		auto const shuttleSector = core::SectorId{
			(uint64_t)created.shuttle.sector->getIndex() + 1 };
		bool selectedWhileOnboard = false;
		bool stoodInSelectedDoorRange = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
					{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()) return false;
			for (auto const& rider : shuttle->liftAgents)
				if (rider.agent == passengerId
					&& rider.state == core::LiftAgentState::InLift)
					selectedWhileOnboard = selectedWhileOnboard
						|| rider.shuttleAlightingDoor == nearestDoor;
			if (!shuttle->shuttleCarriages.empty())
				for (auto const& position : shuttle->shuttleCarriages.front().positions)
					if (position.occupant == passengerId)
						stoodInSelectedDoorRange = stoodInSelectedDoorRange
							|| position.position.x > options.carWidth * 0.5f;
			for (auto const& request : snapshot.traversalRequests)
				if (request.owner == passengerId && request.edgeType == core::EdgeType::Door
					&& request.sourceSector == shuttleSector && request.shuttleDoor)
				{
					auto const x = passenger->getGlobalPosition().x;
					auto const selectedDoorIsPhysicallyNearest = std::abs(x - 13.5f)
						< std::abs(x - 10.5f);
					return selectedWhileOnboard && stoodInSelectedDoorRange
						&& selectedDoorIsPhysicallyNearest && request.shuttleDoor == nearestDoor;
				}
		}
		return false;
	}

	bool singleCarriageShuttleUsesTransportJourneyProtocol()
	{
		core::World world("Single carriage shuttle", 12, 2);
		// This scenario observes continuous queue ownership across a vehicle cycle.
		// Voluntary planning deliberately forfeits that ownership (#256).
		auto waiting = world.getTraversalWaitingPolicy();
		waiting.minimumReplanWaitTicks = MaximumSimulationTicks * 24 + 1;
		world.setTraversalWaitingPolicy(waiting);
		auto left = world.addRoom("Left platform", 0, 0, 0, 3, 1);
		auto right = world.addRoom("Right platform", 0, 0, 7, 3, 1);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 7 }, 0 };
		options.capacity = 2;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		auto created = world.addShuttle(1, 0, 0, 11, options);
		world.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.doors.size() != 2)
			return false;
		auto initial = world.getSimulationSnapshot();
		for (auto const& door : created.doors)
		{
			auto landing = std::find_if(initial.traversalResources.begin(),
				initial.traversalResources.end(),
				[&](auto const& resource) { return resource.id == door.traversalResource; });
			if (landing == initial.traversalResources.end()) return false;
			auto carLane = std::find_if(landing->queueLanes.begin(), landing->queueLanes.end(),
				[&](auto const& lane)
				{ return lane.sector.value == created.shuttle.sector->getIndex() + 1; });
			if (carLane == landing->queueLanes.end()
				|| carLane->positions.size() != options.capacity) return false;
		}
		auto target = world.getGraph()->getClosestVertexInSector(
			world.getSector(right).get(), { 8.5f, 0.0f });
		if (!target) return false;
		std::vector<core::AgentId> passengers;
		for (uint32_t i = 0; i < 3; ++i)
		{
			auto id = world.createAgent("Shuttle passenger", left, 0, 1.0f + i * 0.15f);
			auto agent = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			uint32_t rides = 0, doors = 0;
			for (auto const& node : path->nodes) if (node.edge)
			{
				rides += node.edge->getType() == core::EdgeType::Shuttle;
				doors += node.edge->getType() == core::EdgeType::Door;
			}
			if (rides != 1 || doors != 2) return false;
			agent->setPath(path, true);
			passengers.push_back(id);
		}

		bool sawPhysicalCall = false;
		bool sawFullWithWaiter = false;
		bool sawPlatformQueuePosition = false;
		bool sawAttachedMotion = false;
		bool sawDisembarkBeforeBoard = false;
		bool sawHeldReturnBoarder = false;
		core::AgentId returnBoarderId;
		std::map<core::AgentId, float> previousDisembarkTargets;
		std::map<core::AgentId, float> previousCarriageX;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 24; ++tick)
		{
			world.advanceTick();
			// Calls stay at authored doorways while the occupied coupled vehicle
			// and its invisible destination selector move independently.
			for (uint32_t stop = 0; stop < created.doors.size(); ++stop)
			{
				auto const& control = created.doors[stop].controls[0];
				auto button = std::dynamic_pointer_cast<const core::Button>(control.sector->getObject(control.index)->_getObject());
				auto point = world.lookupInteractionPoint(control.interactionPoint);
				float expected = stop == 0 ? 2.0f : 9.0f;
				if (!button || !point || button->getPosition().x + button->getSize().x * 0.5f != expected
					|| button->getPosition().y != CORE_BUTTON_Y_OFFSET || point.entity->getPosition() != core::Vector2{expected, 0}) return false;
			}
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end() || !shuttle->isShuttle
				|| shuttle->occupantCount + shuttle->admissionReservationCount > options.capacity)
				return false;
			if (!returnBoarderId && shuttle->liftMoving
				&& shuttle->liftDirection == core::TraversalDirection::Ascending)
			{
				auto returnTarget = world.getGraph()->getClosestVertexInSector(
					world.getSector(left).get(), { 1.5f, 0.0f });
				if (!returnTarget) return false;
				returnBoarderId = world.createAgent(
					"Waiting return passenger", right, 0, 0.5f);
				auto returnBoarder = world.lookupAgent(returnBoarderId).entity;
				auto returnPath = world.getGraph()->calculatePath(returnBoarder, returnTarget);
				if (!returnPath) return false;
				returnBoarder->setPath(std::move(returnPath), true);
			}
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::CallShuttle
					&& operation.state == core::DeviceOperationState::Succeeded) sawPhysicalCall = true;
			std::vector<core::Vector2> platformQueueTargets;
			for (auto const& request : snapshot.traversalRequests)
				if (request.state == core::TraversalRequestState::Pending
					&& request.sourceSector.value == left + 1 && request.hasQueuePosition)
					platformQueueTargets.push_back(request.queuePositionTarget);
			sawPlatformQueuePosition = sawPlatformQueuePosition || !platformQueueTargets.empty();
			for (auto passengerId : passengers)
			{
				auto passenger = world.lookupAgent(passengerId).entity;
				if (passenger->getSector() != world.getSector(created.shuttle.sector->getIndex()).get())
				{
					previousCarriageX.erase(passengerId);
					continue;
				}
				auto carriageX = passenger->getGlobalPosition().x - shuttle->liftPosition;
				if (auto previous = previousCarriageX.find(passengerId); previous != previousCarriageX.end())
				{
					if (std::abs(carriageX - previous->second) > passenger->getWalkSpeed()
						* world.getFixedTimestep() + 0.001f) return false;
				}
				previousCarriageX[passengerId] = carriageX;
			}
			sawFullWithWaiter = sawFullWithWaiter
				|| (shuttle->occupantCount == options.capacity && !shuttle->admissionQueue.empty());
			if (shuttle->liftStopPhase == core::LiftStopPhase::Disembarking)
			{
				if (shuttle->admissionReservationCount != 0) return false;
				sawDisembarkBeforeBoard = true;
				for (auto const& request : snapshot.traversalRequests)
				{
					if (request.owner != returnBoarderId || !request.queueTicket
						|| request.state != core::TraversalRequestState::Pending
						|| !request.hasQueueStandingTarget) continue;
					auto const distance = std::abs(
						request.queueStandingTarget.x - request.sourceEndpoint.x);
					if (auto previous = previousDisembarkTargets.find(request.owner);
						previous != previousDisembarkTargets.end()
						&& distance + 0.001f < previous->second) return false;
					previousDisembarkTargets[request.owner] = distance;
					if (request.permit || request.hasCapacityPosition) return false;
					sawHeldReturnBoarder = true;
				}
			}
			for (auto const& passenger : passengers)
			{
				auto agent = world.lookupAgent(passenger).entity;
				if (agent->getSector() == world.getSector(left).get()
					&& std::abs(agent->getGlobalPosition().y) > 0.001f) return false;
			}
			if (shuttle->liftMoving)
				for (auto const& passenger : passengers)
				{
					auto agent = world.lookupAgent(passenger).entity;
					if (agent->getSector() == world.getSector(created.shuttle.sector->getIndex()).get()
						&& agent->getGlobalPosition().x >= shuttle->liftPosition)
						sawAttachedMotion = true;
				}
			if (std::all_of(passengers.begin(), passengers.end(), [&](auto id)
				{ auto agent = world.lookupAgent(id).entity; return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(right).get(); }) && returnBoarderId)
			{
				auto returnBoarder = world.lookupAgent(returnBoarderId).entity;
				if (returnBoarder->getState() == core::Agent::State::Idle
					&& returnBoarder->getSector() == world.getSector(left).get()) break;
			}
		}
		auto final = world.getSimulationSnapshot();
		auto shuttle = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawPhysicalCall && sawFullWithWaiter && sawPlatformQueuePosition
			&& sawAttachedMotion && sawDisembarkBeforeBoard && sawHeldReturnBoarder
			&& shuttle != final.traversalResources.end() && shuttle->occupantCount == 0
			&& shuttle->admissionReservationCount == 0;
	}

	bool shuttleArrivalFollowsFinalPathNodeWithoutBacktracking()
	{
		core::World world("Multi-door Shuttle arrival", 48, 6);
		auto left = world.addCorridor(1, 1, 6);
		auto right = world.addCorridor(1, 15, 6);
		core::World::CreateShuttleOptions options{ 1, 3, { 0, 13 }, 0 };
		options.capacity = 5;
		options.doorMask = 0b101;
		options.minimumDwellSeconds = 0.75f;
		options.maximumBoardingSeconds = 5.0f;
		auto created = world.addShuttle(1, 1, 3, 16, options);
		world.finishBuild();

		// Pin opposite carriage doors so equal-cost platform walking cannot turn
		// this backtracking regression into a single-door journey.
		std::shared_ptr<const core::Vertex> source;
		std::shared_ptr<const core::Vertex> target;
		for (auto const& edge : world.getGraph()->getEdges())
		{
			for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
			{
				auto vertex = edge->getVertex(endpoint);
				if (edge->getTraversalResourceId() == created.doors[0].traversalResource
					&& vertex->getSector().get() == world.getSector(left).get()) source = vertex;
				if (edge->getTraversalResourceId() == created.doors[3].traversalResource
					&& vertex->getSector().get() == world.getSector(right).get()) target = vertex;
			}
		}
		if (!source || !target) return false;
		auto passengerId = world.createAgent("Multi-door passenger", left, 0, 0.4f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, source, target);
		if (!path) return false;
		auto const finalShuttleX = target->getPosition().x;
		passenger->setPath(std::move(path), true);

		std::optional<float> previousStoppedX;
		std::optional<uint64_t> lastOccupiedAtDestination;
		bool reachedFinalDoorBand = false;
		bool sawAllDestinationDoorsOpen = false;
		bool sawBoardingWindowAfterDisembark = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 14; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end()) return false;
			passenger = world.lookupAgent(passengerId).entity;
			if (!shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				if (passenger->getSector()
					== world.getSector(created.shuttle.sector->getIndex()).get())
					lastOccupiedAtDestination = snapshot.tick;
				if (shuttle->liftStopPhase == core::LiftStopPhase::Disembarking)
				{
					bool allOpen = true;
					for (uint32_t doorIndex = 2; doorIndex < 4; ++doorIndex)
					{
						auto door = std::find_if(snapshot.traversalResources.begin(),
							snapshot.traversalResources.end(), [&](auto const& resource)
							{ return resource.id == created.doors[doorIndex].traversalResource; });
						allOpen = allOpen && door != snapshot.traversalResources.end()
							&& (door->doorState == core::DoorSnapshotState::Opening
								|| door->doorState == core::DoorSnapshotState::Open);
					}
					if (!allOpen) return false;
					sawAllDestinationDoorsOpen = true;
				}
				if (shuttle->liftStopPhase == core::LiftStopPhase::Boarding
					&& lastOccupiedAtDestination)
				{
					auto boardingTicks = (uint64_t)std::ceil(options.maximumBoardingSeconds
						/ world.getFixedTimestep());
					if (shuttle->liftServiceStartedTick <= *lastOccupiedAtDestination
						|| shuttle->liftBoardingCutoffTick
							!= shuttle->liftServiceStartedTick + boardingTicks) return false;
					sawBoardingWindowAfterDisembark = true;
				}
			}
			if (passenger->getSector() == world.getSector(created.shuttle.sector->getIndex()).get()
				&& !shuttle->liftMoving && shuttle->liftCurrentStop == 1)
			{
				auto x = passenger->getGlobalPosition().x;
				if (previousStoppedX && x < *previousStoppedX - 0.001f) return false;
				reachedFinalDoorBand = reachedFinalDoorBand
					|| std::abs(x - finalShuttleX) <= CORE_DOOR_CROSSING_HALF_WIDTH(1) + 0.001f;
				previousStoppedX = x;
			}
			if (sawBoardingWindowAfterDisembark && passenger->getState() == core::Agent::State::Idle
				&& passenger->getSector() == world.getSector(right).get()) break;
		}
		return reachedFinalDoorBand && sawAllDestinationDoorsOpen && sawBoardingWindowAfterDisembark
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(right).get();
	}

	bool multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones()
	{
		core::World world("Coupled shuttle", 20, 2);
		auto leftA = world.addRoom("Left A", 0, 0, 0, 3, 1);
		auto leftB = world.addRoom("Left B", 0, 0, 4, 3, 1);
		auto rightA = world.addRoom("Right A", 0, 0, 12, 3, 1);
		auto rightB = world.addRoom("Right B", 0, 0, 16, 3, 1);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 12 }, 0 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 2.0f;
		auto created = world.addShuttle(1, 0, 0, 19, options);
		world.finishBuild();
		if (!created.traversalResource || created.doors.size() != 4) return false;

		struct Journey { core::AgentId agent; uint32_t targetSector; float targetX; };
		std::vector<Journey> journeys = {
			{ world.createAgent("A first", leftA, 0, 1.35f), rightA, 13.5f },
			{ world.createAgent("B", leftB, 0, 1.5f), rightB, 17.5f },
			{ world.createAgent("A overflow", leftA, 0, 1.65f), rightA, 13.5f }
		};
		for (auto const& journey : journeys)
		{
			auto agent = world.lookupAgent(journey.agent).entity;
			auto target = world.getGraph()->getClosestVertexInSector(
				world.getSector(journey.targetSector).get(), { journey.targetX, 0.0f });
			if (!target) return false;
			auto path = world.getGraph()->calculatePath(agent, target);
			if (!path) return false;
			agent->setPath(std::move(path), true);
		}

		bool sawIndependentFullCarriages = false;
		std::array<bool, 2> sawCarriageOccupied{};
		bool sawSeparatedAccessZones = false;
		bool sawBoundAssignment = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 24; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto shuttle = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (shuttle == snapshot.traversalResources.end() || shuttle->shuttleCarriages.size() != 2
				|| shuttle->capacity != 2 || shuttle->shuttleCapacityPerCarriage != 1) return false;
			for (auto const& carriage : shuttle->shuttleCarriages)
			{
				if (carriage.occupantCount + carriage.admissionReservationCount > carriage.capacity) return false;
				if (carriage.index < sawCarriageOccupied.size() && carriage.occupantCount)
					sawCarriageOccupied[carriage.index] = true;
			}
			sawIndependentFullCarriages = sawIndependentFullCarriages
				|| (shuttle->shuttleCarriages[0].occupantCount == 1
					&& shuttle->shuttleCarriages[1].occupantCount == 1);
			uint32_t leftZones = 0;
			for (auto const& zone : shuttle->shuttleAccessZones)
				if (zone.stopIndex == 0 && zone.direction == core::TraversalDirection::Ascending) ++leftZones;
			sawSeparatedAccessZones = sawSeparatedAccessZones || leftZones >= 2;
			for (auto const& request : snapshot.traversalRequests)
				if (request.shuttleCarriage != ~0u && request.shuttleDoor)
					sawBoundAssignment = true;
			if (std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
				{ auto agent = world.lookupAgent(journey.agent).entity;
					return agent->getState() == core::Agent::State::Idle
						&& agent->getSector() == world.getSector(journey.targetSector).get(); })) break;
		}
		auto complete = std::all_of(journeys.begin(), journeys.end(), [&](auto const& journey)
			{ auto agent = world.lookupAgent(journey.agent).entity;
				return agent->getState() == core::Agent::State::Idle
					&& agent->getSector() == world.getSector(journey.targetSector).get(); });
		return (sawIndependentFullCarriages || std::all_of(sawCarriageOccupied.begin(), sawCarriageOccupied.end(),
			[](bool occupied) { return occupied; }))
			&& sawSeparatedAccessZones && sawBoundAssignment && complete;
	}

	bool editorShuttleAuthoringReconcilesOwnedLandings()
	{
		core::World world("Editor shuttle authoring", 32, 3);
		world.addCorridor(0, 0, 31);
		world.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.doorMask = 0b101;
		auto candidates = world.getValidShuttleStopOffsets(1, 0, 0, 27, 2, 3, false, 2);
		if (find(candidates.begin(), candidates.end(), 0) == candidates.end()
			|| find(candidates.begin(), candidates.end(), 18) == candidates.end()) return false;
		auto created = world.addShuttle(1, 0, 0, 27, options);
		world.finishBuild();
		auto shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(created.shuttle.sector);
		if (!shuttle || shuttle->getNumStops() != 2 || created.doors.size() != 8) return false;
		for (auto const& door : created.doors)
		{
			uint32_t owner, stop, carriage;
			if (!world.isShuttleOwnedDoor(door.door.sector->getObject(door.door.index),
				&owner, &stop, &carriage) || owner != shuttle->getIndex()
				|| stop >= 2 || carriage >= 2) return false;
		}
		auto doorCandidates = world.getShuttleStopCandidatesForDoor(1, 0, 9);
		if (none_of(doorCandidates.begin(), doorCandidates.end(), [&](auto const& candidate)
			{ return candidate.sectorIndex == shuttle->getIndex() && candidate.stopOffset == 9; })) return false;

		world.pauseSimulation();
		auto move = world.planResizeShuttle(shuttle->getIndex(), 1, 1, 27);
		if (!move.valid || !move.move || move.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		auto movedIndex = world.applyShuttleEdit(move);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getCellX() != 1 || shuttle->getCellY() != 1) return false;

		auto resize = world.planResizeShuttle(movedIndex, 1, 1, 26);
		if (!resize.valid || resize.move || resize.stopOffsets != std::vector<uint32_t>({ 0, 18 })) return false;
		movedIndex = world.applyShuttleEdit(resize);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getCellsWide() != 26) return false;

		auto add = world.planAddShuttleStop(movedIndex, 9);
		if (!add.valid || !add.requiresConfirmation()) return false;
		movedIndex = world.applyShuttleEdit(add);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 3) return false;
		auto removeStop = world.planRemoveShuttleStop(movedIndex, 1);
		if (!removeStop.valid || !removeStop.requiresConfirmation()) return false;
		movedIndex = world.applyShuttleEdit(removeStop);
		shuttle = dynamic_pointer_cast<const core::ShuttleTransit>(world.getSector(movedIndex));
		if (!shuttle || shuttle->getNumStops() != 2) return false;
		world.addSectorWindow(0, 1, 10, 1, 1);
		auto remove = world.planRemoveShuttle(movedIndex);
		if (!remove.valid) return false;
		world.applyShuttleEdit(remove);
		if (world.getSectorAtPosition(1, 1.0f, 1.0f)) return false;

		core::World manyDoors("Schematic Shuttle doors", 24, 2);
		manyDoors.addCorridor(0, 0, 23);
		core::World::CreateShuttleOptions manyDoorOptions{ 1, 4, { 0, 10 }, 0 };
		manyDoorOptions.doorMask = 0b1111;
		auto manyDoorResult = manyDoors.addShuttle(1, 0, 0, 20, manyDoorOptions);
		manyDoors.finishBuild();
		if (manyDoorResult.doors.size() != 8
			|| !manyDoors.getValidShuttleStopOffsets(1, 0, 0, 20, 1, 4, false, 1u << 4).empty()) return false;
		for (uint32_t door = 0; door < 4; ++door)
			if (manyDoorResult.doors[door].door.sector->getObject(
				manyDoorResult.doors[door].door.index)->getCellX() != door) return false;

		core::World partial("Partial Shuttle authoring", 24, 2);
		partial.addCorridor(0, 0, 4);
		partial.addCorridor(0, 10, 4);
		core::World::CreateShuttleOptions partialOptions{ 2, 3, { 0, 10 }, 0 };
		partialOptions.allowPartialLandings = true;
		partialOptions.doorMask = 0b101;
		auto partialCreated = partial.addShuttle(1, 0, 0, 20, partialOptions);
		partial.finishBuild();
		return partialCreated.doors.size() == 8
			&& partialCreated.doors[0].traversalResource
			&& partialCreated.doors[1].traversalResource
			&& !partialCreated.doors[2].traversalResource
			&& !partialCreated.doors[3].traversalResource
			&& partialCreated.doors[4].traversalResource
			&& partialCreated.doors[5].traversalResource
			&& !partialCreated.doors[6].traversalResource
			&& !partialCreated.doors[7].traversalResource;
	}
}

void registerShuttles(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "shuttlePassengerWalksToForwardInteriorSpot", [](smoke::Context const&) { smoke::require(shuttlePassengerWalksToForwardInteriorSpot(), "shuttlePassengerWalksToForwardInteriorSpot"); } });
	checks.push_back({ "shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed", [](smoke::Context const&) { smoke::require(shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed(), "shuttlePassengersSpreadAcrossCarriageAtWalkingSpeed"); } });
	checks.push_back({ "shuttleBoardingRequiresDoorAlignment", [](smoke::Context const&) { smoke::require(shuttleBoardingRequiresDoorAlignment(), "shuttleBoardingRequiresDoorAlignment"); } });
	checks.push_back({ "shuttlePassengerUsesBoardingSelectedAlightingDoor", [](smoke::Context const&) { smoke::require(shuttlePassengerUsesBoardingSelectedAlightingDoor(), "shuttlePassengerUsesBoardingSelectedAlightingDoor"); } });
	checks.push_back({ "singleCarriageShuttleUsesTransportJourneyProtocol", [](smoke::Context const&) { smoke::require(singleCarriageShuttleUsesTransportJourneyProtocol(), "singleCarriageShuttleUsesTransportJourneyProtocol"); } });
	checks.push_back({ "shuttleArrivalFollowsFinalPathNodeWithoutBacktracking", [](smoke::Context const&) { smoke::require(shuttleArrivalFollowsFinalPathNodeWithoutBacktracking(), "shuttleArrivalFollowsFinalPathNodeWithoutBacktracking"); } });
	checks.push_back({ "multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones", [](smoke::Context const&) { smoke::require(multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones(), "multiCarriageShuttleCoordinatesIndependentCarriagesAndAccessZones"); } });
	checks.push_back({ "editorShuttleAuthoringReconcilesOwnedLandings", [](smoke::Context const&) { smoke::require(editorShuttleAuthoringReconcilesOwnedLandings(), "editorShuttleAuthoringReconcilesOwnedLandings"); } });
}
