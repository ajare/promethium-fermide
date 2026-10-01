#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>

namespace
{

	bool sharedLocationWallsCanBeOpenedAndRestored()
	{
		core::World world("Shared Location walls", 8, 4);
		auto left = world.addRoom("Left", 0, 1, 0, 3, 2);
		auto right = world.addRoom("Right", 0, 0, 3, 3, 3);
		world.finishBuild();

		std::string diagnostic;
		if (!world.canRemoveLocationWall(left, 0, CORE_SIDE_RIGHT, &diagnostic)
			|| world.canRemoveLocationWall(left, 0, CORE_SIDE_LEFT, &diagnostic)) return false;
		bool activeEditRejected = false;
		try { world.removeLocationWall(left, 0, CORE_SIDE_RIGHT); }
		catch (std::exception const&) { activeEditRejected = true; }
		if (!activeEditRejected) return false;

		world.pauseSimulation();
		world.removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::None
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::None
			|| !world.canAddLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic)) return false;
		world.finishBuild();
		if (!world.isTraversalTopologyValid()) return false;

		world.addLocationWall(right, 1, CORE_SIDE_LEFT);
		if (world.getSector(left)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::Wall
			|| world.getSector(right)->getEndType(1, CORE_SIDE_LEFT) != core::SectorEndType::Wall)
			return false;
		world.finishBuild();
		return world.isTraversalTopologyValid()
			&& world.canRemoveLocationWall(right, 1, CORE_SIDE_LEFT, &diagnostic);
	}

	bool walkwayEditingEnforcesPlacementMovementAndOccupancyRules()
	{
		core::World world("Walkway editing", 10, 4);
		auto room = world.addRoom("Walkway room", 0, 0, 0, 4, 3);
		auto otherRoom = world.addRoom("Other room", 0, 0, 6, 3, 3);
		std::string diagnostic;
		if (world.canAddSectorWalkway(room, 0, 1, &diagnostic)
			|| !world.canAddSectorWalkway(room, 1, 1, &diagnostic)) return false;
		auto created = world.addSectorWalkway(room, 1, 1);
		world.finishBuild();
		world.pauseSimulation();

		auto acrossRooms = world.planMoveSectorObject(room, created.index, 6, 1);
		auto withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (acrossRooms.valid || !withinRoom.valid) return false;

		auto agentId = world.createAgent("Walkway occupant", room, 1, 1.5f);
		if (world.planMoveSectorObject(room, created.index, 2, 1).valid) return false;
		bool occupiedDeleteRejected = false;
		try { world.removeSectorWalkway(room, created.index); }
		catch (std::exception const&) { occupiedDeleteRejected = true; }
		if (!occupiedDeleteRejected) return false;
		auto cropped = world.planResizeLocation(room, 0, 0, 1, 3);
		if (cropped.valid) return false;

		if (!world.removeAgent(agentId)) return false;
		withinRoom = world.planMoveSectorObject(room, created.index, 2, 1);
		if (!withinRoom.valid) return false;
		auto moved = world.applyObjectMove(withinRoom);
		if (!moved || moved->getCellX() != 2 || moved->getCellY() != 1
			|| moved->getSector()->getIndex() != room) return false;
		uint32_t movedIndex = ~0u;
		for (uint32_t i = 0; i < moved->getSector()->getNumObjects(); ++i)
			if (moved->getSector()->getObject(i) == moved) { movedIndex = i; break; }
		if (movedIndex == ~0u || !world.removeSectorWalkway(room, movedIndex)) return false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;

		world.addSectorWalkway(room, 1, 3);
		world.finishBuild();
		auto cropUnoccupied = world.planResizeLocation(room, 0, 0, 3, 3);
		if (!cropUnoccupied.valid) return false;
		auto resizedRoom = world.applyLocationEdit(cropUnoccupied);
		for (uint32_t i = 0; i < world.getSector(resizedRoom)->getNumObjects(); ++i)
			if (auto object = world.getSector(resizedRoom)->getObject(i))
				if (object->getObjectType() == core::SectorObjectType::Walkway) return false;
		return world.getSector(otherRoom) != nullptr && world.isTraversalTopologyValid();
	}

	bool deletingWalkwayPreservesUnrelatedRoomDoor()
	{
		core::World world("Walkway deletion isolation", 16, 6);
		world.addCorridor(4, 9, 4);
		auto room = world.addRoom("Walkway room", 1, 3, 9, 4, 2);
		core::World::CreateObjectResult walkways[4];
		for (uint32_t x = 0; x < 4; ++x)
			walkways[x] = world.addSectorWalkway(room, 1, x);
		world.addSectorDoor(0, 4, 12);
		world.finishBuild();
		world.pauseSimulation();

		try
		{
			// The Walkway at 11,4 is not beneath the Door at 12,4. Removing it
			// must shrink the physical queue rather than invalidate the Door.
			if (!world.removeSectorWalkway(room, walkways[2].index)) return false;
		}
		catch (std::exception const&)
		{
			return false;
		}
		uint32_t doorsInRoom = 0, remainingWalkways = 0;
		bool walkwayAt11 = false, walkwayAt12 = false;
		for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
			if (auto object = world.getSector(room)->getObject(i))
			{
				doorsInRoom += object->getObjectType() == core::SectorObjectType::Door;
				if (object->getObjectType() != core::SectorObjectType::Walkway) continue;
				++remainingWalkways;
				walkwayAt11 = walkwayAt11 || object->getCellX() == 11;
				walkwayAt12 = walkwayAt12 || object->getCellX() == 12;
			}
		return doorsInRoom == 1 && remainingWalkways == 3
			&& !walkwayAt11 && walkwayAt12 && world.isTraversalTopologyValid()
			&& world.getSimulationSnapshot().traversalResources.size() == 1;
	}
}

void registerFloorsAndWalls(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "sharedLocationWallsCanBeOpenedAndRestored", [](smoke::Context const&)
		{
			smoke::require(sharedLocationWallsCanBeOpenedAndRestored(), "shared Location walls could not be opened and restored");
		} });
	checks.push_back({ "walkwayEditingEnforcesPlacementMovementAndOccupancyRules", [](smoke::Context const&)
		{
			smoke::require(walkwayEditingEnforcesPlacementMovementAndOccupancyRules(), "Walkway editing violated placement, movement, deletion, or occupancy rules");
		} });
	checks.push_back({ "deletingWalkwayPreservesUnrelatedRoomDoor", [](smoke::Context const&)
		{
			smoke::require(deletingWalkwayPreservesUnrelatedRoomDoor(), "deleting a Walkway removed an unrelated Room Door");
		} });
}
