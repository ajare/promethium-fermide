#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/Staircase.h"
#include "core/Transit.h"

namespace
{
	bool platformLiftAuthoringReconcilesWalkwayStops()
	{
		{
			core::World offset("PlatformLift initial level", 8, 6);
			auto offsetRoom = offset.addRoom("Offset room", 0, 1, 0, 7, 4);
			offset.addSectorWalkway(offsetRoom, 2, 2);
			offset.addSectorWalkway(offsetRoom, 2, 3);
			core::World::CreateLiftOptions offsetOptions;
			offsetOptions.stopOffsets = { 0, 2 };
			auto placed = offset.addSectorPlatformLift(offsetRoom, 0, 2, offsetOptions);
			auto object = std::dynamic_pointer_cast<const core::LiftSectorObject>(
				placed.lift.sector->getObject(placed.lift.index));
			offset.finishBuild();
			auto snapshot = offset.getSimulationSnapshot();
			auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& value) { return value.id == placed.traversalResource; });
			std::shared_ptr<const core::SectorObject> hitObject;
			auto hit = offset.getObjectAtPosition(0, 2.5f, 0.975f, &hitObject);
			if (!object || std::abs(object->getLift()->getPosition().y - 1.0f) > 0.001f
				|| resource == snapshot.traversalResources.end()
				|| std::abs(resource->liftPosition - 1.0f) > 0.001f
				|| hit.get() != object->getLift().get() || hitObject != object) return false;
		}
		core::World world("PlatformLift authoring", 8, 5);
		auto room = world.addRoom("Lift room", 0, 0, 0, 7, 4);
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		world.addSectorWalkway(room, 3, 2);
		world.addSectorWalkway(room, 3, 3);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1; options.stopOffsets = { 0, 1 };
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		world.finishBuild(); world.pauseSimulation();
		options.stopOffsets = { 0, 1, 3 };
		auto edit = world.planPlatformLiftEdit(room, created.lift.index, options);
		if (!edit.valid) return false;
		auto liftObject = world.applyPlatformLiftEdit(edit);
		if (!liftObject) return false;

		auto findObject = [&](core::SectorObjectType type, uint32_t x, uint32_t y)
		{
			auto sector = world.getSector(room);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object && object->getObjectType() == type
					&& object->getCellX() == x && object->getCellY() == y) return i;
			}
			return ~0u;
		};
		auto lower = findObject(core::SectorObjectType::Walkway, 2, 1);
		auto removal = world.planRemoveSectorWalkway(room, lower);
		if (!removal.valid || !removal.consequences.empty() || !world.applyWalkwayEdit(removal)) return false;
		auto liftIndex = findObject(core::SectorObjectType::Lift, 2, 0);
		core::World::CreateLiftOptions retained;
		if (liftIndex == ~0u || !world.getPlatformLiftOptions(room, liftIndex, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 3 })) return false;
		auto upper = findObject(core::SectorObjectType::Walkway, 2, 3);
		removal = world.planRemoveSectorWalkway(room, upper);
		if (!removal.valid || removal.consequences.empty() || !world.applyWalkwayEdit(removal)) return false;
		if (findObject(core::SectorObjectType::Lift, 2, 0) != ~0u) return false;

		core::World resized("PlatformLift resize", 8, 5);
		auto resizedRoom = resized.addRoom("Lift room", 0, 0, 0, 7, 4);
		resized.addSectorWalkway(resizedRoom, 1, 2); resized.addSectorWalkway(resizedRoom, 1, 3);
		resized.addSectorWalkway(resizedRoom, 3, 2); resized.addSectorWalkway(resizedRoom, 3, 3);
		options.stopOffsets = { 0, 1, 3 };
		resized.addSectorPlatformLift(resizedRoom, 0, 2, options);
		resized.finishBuild(); resized.pauseSimulation();
		auto resize = resized.planResizeLocation(resizedRoom, 0, 0, 7, 2);
		if (!resize.valid || resize.consequences.empty()) return false;
		resizedRoom = resized.applyLocationEdit(resize);
		uint32_t resizedLift = ~0u;
		for (uint32_t i = 0; i < resized.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = resized.getSector(resizedRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) resizedLift = i;
		if (resizedLift == ~0u || !resized.getPlatformLiftOptions(resizedRoom, resizedLift, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 1 })) return false;
		resize = resized.planResizeLocation(resizedRoom, 0, 0, 7, 1);
		if (!resize.valid || std::none_of(resize.consequences.begin(), resize.consequences.end(),
			[](auto const& value) { return value.find("Platform Lift") != std::string::npos; })) return false;
		resizedRoom = resized.applyLocationEdit(resize);
		for (uint32_t i = 0; i < resized.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = resized.getSector(resizedRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) return false;

		core::World moving("PlatformLift movement", 9, 5);
		auto movingRoom = moving.addRoom("Lift room", 0, 0, 0, 8, 4);
		moving.addSectorWalkway(movingRoom, 1, 1); moving.addSectorWalkway(movingRoom, 1, 2);
		moving.addSectorWalkway(movingRoom, 2, 3); moving.addSectorWalkway(movingRoom, 2, 4);
		options.stopOffsets = { 0, 1 };
		auto movingLift = moving.addSectorPlatformLift(movingRoom, 0, 1, options);
		moving.finishBuild(); moving.pauseSimulation();
		auto move = moving.planMoveSectorObject(movingRoom, movingLift.lift.index, 3, 0);
		if (!move.valid || !move.requiresConfirmation()) return false;
		auto movedLift = moving.applyObjectMove(move);
		uint32_t movedLiftIndex = ~0u;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
			if (moving.getSector(movingRoom)->getObject(i) == movedLift) movedLiftIndex = i;
		if (movedLiftIndex == ~0u || !moving.getPlatformLiftOptions(movingRoom, movedLiftIndex, retained)
			|| retained.stopOffsets != std::vector<uint32_t>({ 0, 2 })) return false;
		uint32_t connectedWalkway = ~0u;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
		{
			auto object = moving.getSector(movingRoom)->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Walkway
				&& object->getCellX() == 3 && object->getCellY() == 2) connectedWalkway = i;
		}
		move = moving.planMoveSectorObject(movingRoom, connectedWalkway, 6, 2);
		if (!move.valid || !move.requiresConfirmation() || !moving.applyObjectMove(move)) return false;
		for (uint32_t i = 0; i < moving.getSector(movingRoom)->getNumObjects(); ++i)
			if (auto object = moving.getSector(movingRoom)->getObject(i);
				object && object->getObjectType() == core::SectorObjectType::Lift) return false;
		return true;
	}

	bool openPlatformLiftUsesVirtualBoundaryAndTransportPolicy()
	{
		core::World world("Open platform lift", 7, 5);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 4);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.platformStopDurationSeconds = 2.0f;
		world.addSectorWalkway(room, 2, 0);
		world.addSectorWalkway(room, 2, 1);
		world.addSectorWalkway(room, 2, 2);
		world.addSectorWalkway(room, 2, 3);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 2, 0.5f, &destinationVertexId);
		world.finishBuild();
		if (!created.traversalResource || !created.interiorSelector || created.buttons.size() != 2)
			return false;

		auto target = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto passengerId = world.createAgent("Platform passenger", room, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, target);
		if (!path || std::count_if(path->nodes.begin(), path->nodes.end(), [](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift; }) != 1) return false;
		passenger->setPath(path, true);

		bool sawPhysicalQueuePosition = false;
		bool sawOnboard = false;
		bool sawBoardingCrossing = false;
		bool boardingStayedAtWalkSpeed = true;
		bool fullyInsideWhenRegistered = false;
		bool registeredAtAssignedPosition = false;
		bool reachedAssignedPosition = false;
		bool exitedTowardNextVertex = false;
		bool checkedExitDirection = false;
		bool wasOnboard = false;
		bool sawAttachedMotion = false;
		bool sawDestinationConfirmation = false;
		bool sawConfiguredStopDuration = false;
		auto previousPosition = passenger->getGlobalPosition();
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 6
			&& passenger->getState() != core::Agent::State::Idle; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
				[&](auto const& resource) { return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end() || !platform->isOpenPlatformLift
				|| platform->liftCarDoorOpen
				|| platform->occupantCount + platform->admissionReservationCount > options.capacity)
				return false;
			if (platform->queueLanes.size() != options.stopOffsets.size()
				|| std::any_of(platform->queueLanes.begin(), platform->queueLanes.end(),
					[](auto const& lane) { return lane.positions.empty(); })) return false;
			sawPhysicalQueuePosition = sawPhysicalQueuePosition
				|| std::any_of(snapshot.traversalRequests.begin(), snapshot.traversalRequests.end(),
					[&](auto const& request)
					{ return request.owner == passengerId && request.hasQueuePosition; });
			auto expectedStopTicks = (uint64_t)ceil(options.platformStopDurationSeconds
				/ world.getFixedTimestep());
			if (platform->liftBoardingCutoffTick >= platform->liftServiceStartedTick
				&& platform->liftBoardingCutoffTick - platform->liftServiceStartedTick
					== expectedStopTicks) sawConfiguredStopDuration = true;
			auto onboard = platform->occupantCount == 1;
			sawOnboard = sawOnboard || onboard;
			sawBoardingCrossing = sawBoardingCrossing
				|| (!onboard && platform->virtualBoundaryCrossingCount == 1);
			if (platform->virtualBoundaryCrossingCount > 1) return false;
			auto const assignedX = world.getSector(room)->getPosition().x
				+ platform->capacityPositions.front().position.x;
			if (onboard && !wasOnboard)
			{
				boardingStayedAtWalkSpeed = passenger->getGlobalPosition().distanceTo(previousPosition)
					<= passenger->getWalkSpeed() * world.getFixedTimestep() + 0.001f;
				auto const centerX = passenger->getGlobalPosition().x;
				fullyInsideWhenRegistered = centerX - CORE_RESOURCE_SLOT_WIDTH * 0.5f >= 2.0f - 0.001f
					&& centerX + CORE_RESOURCE_SLOT_WIDTH * 0.5f <= 3.0f + 0.001f;
				registeredAtAssignedPosition = std::abs(centerX - assignedX) < 0.01f;
			}
			if (onboard)
			{
				reachedAssignedPosition = reachedAssignedPosition
					|| std::abs(passenger->getGlobalPosition().x - assignedX) < 0.01f;
			}
			for (auto const& operation : snapshot.deviceOperations)
				if (operation.command.type == core::DeviceCommandType::SelectLiftDestination
					&& operation.state == core::DeviceOperationState::Succeeded)
					sawDestinationConfirmation = true;
			if (platform->liftMoving)
			{
				if (platform->virtualBoundaryCrossingCount != 0) return false;
				if (std::abs(passenger->getGlobalPosition().y - platform->liftPosition) < 0.001f)
					sawAttachedMotion = true;
			}
			auto horizontalStep = passenger->getGlobalPosition().x - previousPosition.x;
			if (!checkedExitDirection && platform->liftCurrentStop == 1 && !platform->liftMoving
				&& passenger->getState() == core::Agent::State::TraversingEdge
				&& std::abs(horizontalStep) > 0.0001f)
			{
				checkedExitDirection = true;
				exitedTowardNextVertex = horizontalStep < 0.0f;
			}
			wasOnboard = onboard;
			previousPosition = passenger->getGlobalPosition();
		}
		auto final = world.getSimulationSnapshot();
		auto platform = std::find_if(final.traversalResources.begin(), final.traversalResources.end(),
			[&](auto const& resource) { return resource.id == created.traversalResource; });
		return sawPhysicalQueuePosition && sawOnboard
			&& sawConfiguredStopDuration
			&& sawBoardingCrossing && boardingStayedAtWalkSpeed
			&& fullyInsideWhenRegistered && registeredAtAssignedPosition
			&& reachedAssignedPosition
			&& exitedTowardNextVertex && sawAttachedMotion && sawDestinationConfirmation
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getSector() == world.getSector(room).get()
			&& passenger->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f
			&& platform != final.traversalResources.end() && platform->occupantCount == 0
			&& platform->virtualBoundaryCrossingCount == 0;
	}

	bool openPlatformLiftCrossingLaneSpreadsPassengers()
	{
		core::World world("Platform lift crossing lane", 7, 4);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 3);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 2 };
		options.capacity = 2;
		options.platformStopDurationSeconds = 10.0f;
		for (uint32_t x = 0; x < 6; ++x) world.addSectorWalkway(room, 2, x);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 2, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		std::vector<core::Agent*> passengers;
		for (float x : { 0.5f, 1.0f })
		{
			auto id = world.createAgent("Platform passenger", room, 0, x);
			auto passenger = world.lookupAgent(id).entity;
			auto path = world.getGraph()->calculatePath(passenger, destination);
			if (!path) return false;
			passenger->setPath(path, true);
			passengers.push_back(passenger);
		}

		if (std::abs(CORE_PLATFORM_LIFT_CROSSING_HALF_WIDTH(options.cellsWide) * 2.0f
			- ((float)options.cellsWide - CORE_RESOURCE_SLOT_WIDTH)) > 0.001f) return false;
		bool sawTwoPassengersSpread = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 12; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end()
				|| platform->virtualBoundaryCrossingCount > 1) return false;
			if (platform->occupantCount == 2)
			{
				auto const roomX = world.getSector(room)->getPosition().x;
				bool bothAtSeparatedPositions = true;
				for (auto const& slot : platform->capacityPositions)
				{
					if (!slot.occupant) { bothAtSeparatedPositions = false; break; }
					auto passenger = world.lookupAgent(slot.occupant).entity;
					if (!passenger || std::abs(passenger->getGlobalPosition().x
						- (roomX + slot.position.x)) > 0.01f)
					{ bothAtSeparatedPositions = false; break; }
				}
				sawTwoPassengersSpread = sawTwoPassengersSpread || bothAtSeparatedPositions;
			}
			if (std::all_of(passengers.begin(), passengers.end(), [](auto passenger)
				{ return passenger->getState() == core::Agent::State::Idle; })) break;
		}
		return sawTwoPassengersSpread
			&& std::all_of(passengers.begin(), passengers.end(), [&](auto passenger)
			{
				return passenger->getState() == core::Agent::State::Idle
					&& passenger->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f;
			});
	}

	bool openPlatformLiftUsesOneJourneyAcrossIntermediateStops()
	{
		core::World world("Multi-stop open platform lift", 7, 5);
		auto room = world.addRoom("Platform room", 0, 0, 0, 6, 4);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 1, 2 };
		options.capacity = 1;
		options.minimumDwellSeconds = 0.1f;
		options.maximumBoardingSeconds = 0.5f;
		world.addSectorWalkway(room, 1, 2);
		world.addSectorWalkway(room, 1, 3);
		for (uint32_t x = 0; x < 4; ++x) world.addSectorWalkway(room, 2, x);
		auto created = world.addSectorPlatformLift(room, 0, 2, options);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(room, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(room, 2, 0.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto passengerId = world.createAgent("Multi-stop platform passenger", room, 0, 0.5f);
		auto passenger = world.lookupAgent(passengerId).entity;
		auto path = world.getGraph()->calculatePath(passenger, source, destination);
		if (!path || std::count_if(path->nodes.begin(), path->nodes.end(), [](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift; }) != 2) return false;
		passenger->setPath(path, true);

		bool passedIntermediateLevelWhileMoving = false;
		for (uint32_t tick = 0; tick < MaximumSimulationTicks * 8
			&& passenger->getState() != core::Agent::State::Idle; ++tick)
		{
			world.advanceTick();
			auto snapshot = world.getSimulationSnapshot();
			auto platform = std::find_if(snapshot.traversalResources.begin(),
				snapshot.traversalResources.end(), [&](auto const& resource)
				{ return resource.id == created.traversalResource; });
			if (platform == snapshot.traversalResources.end()) return false;
			if (platform->liftCurrentStop == 1 && !platform->liftMoving) return false;
			passedIntermediateLevelWhileMoving = passedIntermediateLevelWhileMoving
				|| (platform->liftMoving && std::abs(platform->liftPosition - 1.0f) < 0.01f);
		}
		return passedIntermediateLevelWhileMoving
			&& passenger->getState() == core::Agent::State::Idle
			&& passenger->getGlobalPosition().distanceTo(destination->getPosition()) < 0.001f;
	}
}

void registerPlatformLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "platformLiftAuthoringReconcilesWalkwayStops", [](smoke::Context const&) { smoke::require(platformLiftAuthoringReconcilesWalkwayStops(), "platformLiftAuthoringReconcilesWalkwayStops"); } });
	checks.push_back({ "openPlatformLiftUsesVirtualBoundaryAndTransportPolicy", [](smoke::Context const&) { smoke::require(openPlatformLiftUsesVirtualBoundaryAndTransportPolicy(), "openPlatformLiftUsesVirtualBoundaryAndTransportPolicy"); } });
	checks.push_back({ "openPlatformLiftCrossingLaneSpreadsPassengers", [](smoke::Context const&) { smoke::require(openPlatformLiftCrossingLaneSpreadsPassengers(), "openPlatformLiftCrossingLaneSpreadsPassengers"); } });
	checks.push_back({ "openPlatformLiftUsesOneJourneyAcrossIntermediateStops", [](smoke::Context const&) { smoke::require(openPlatformLiftUsesOneJourneyAcrossIntermediateStops(), "openPlatformLiftUsesOneJourneyAcrossIntermediateStops"); } });
}
