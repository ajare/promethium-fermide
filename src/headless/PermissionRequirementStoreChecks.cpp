#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"
#include "core/PermissionRequirementStore.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("PermissionRequirementStore check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("PermissionRequirementStore check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		// The store is constructed directly against a World it does not own:
		// the ADR 0022 proof-of-pattern is that the `*PermissionRequirement`
		// family is unit-testable without driving the World facade. World
		// keeps the state it edits (ADR 0001); the collaborator holds only
		// a reference.
		//
		// World A: a Room with a light switch (interaction point) and a
		// manual ordinary Door to the room above.
		World world("pr-checks", 8, 3);
		PermissionRequirementStore store(world);

		auto const room = world.addRoom("Room", 0, 0, 0, 8, 2);
		world.addRoom("Back", 1, 0, 0, 8, 2);
		auto const lightSwitch = world.addSectorLightSwitch(room, 4);
		auto const door = world.addSectorDoor(0, 0, 2);
		world.finishBuild();

		std::string diagnostic;

		// Access permission authoring is itself paused-only (#273).
		world.pauseSimulation();
		auto const blue = world.addAccessPermission("Blue key");
		auto const amber = world.addAccessPermission("Amber key");
		world.resumeSimulation();

		// ---------------------------------------------------------------
		// The simulation runs by default: every authored edit is refused.
		// ---------------------------------------------------------------
		require(!store.setLocationRequirement(room, { blue }, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Location edit accepted while running");
		require(!store.setInteractionPointRequirement(lightSwitch.interactionPoint, { blue }, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Interaction-point edit accepted while running");
		require(!store.setManualDoorRequirement(door.traversalResource, { blue }, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"Manual-door edit accepted while running");

		world.pauseSimulation();

		// ---------------------------------------------------------------
		// Static Location requirements (#273).
		// ---------------------------------------------------------------
		require(store.isLocationEligible(room), "Room refused as Location-eligible");
		require(!store.isLocationEligible(9999), "out-of-range sector deemed eligible");
		require(store.locationRequirement(room).empty(), "fresh Location is restricted");

		require(store.setLocationRequirement(room, { blue }, &diagnostic),
			"Location edit refused: " + diagnostic);
		require(store.locationRequirement(room) == std::vector<AccessPermissionId>{ blue },
			"Location requirement did not round-trip");

		// Requirements are stored by permission slot, not insertion order.
		require(store.setLocationRequirement(room, { amber, blue }, &diagnostic),
			"two-permission Location edit refused: " + diagnostic);
		require(store.locationRequirement(room)
			== std::vector<AccessPermissionId>{ blue, amber },
			"Location requirement not ordered by permission slot");

		// A no-op edit succeeds without complaint.
		require(store.setLocationRequirement(room, { blue, amber }, &diagnostic),
			"idempotent Location edit refused: " + diagnostic);

		// Duplicates and unknown permissions are refused having changed nothing.
		require(!store.setLocationRequirement(room, { blue, blue }, &diagnostic)
			&& !diagnostic.empty()
			&& store.locationRequirement(room) == std::vector<AccessPermissionId>{ blue, amber },
			"duplicate Location permission accepted");
		require(!store.setLocationRequirement(room, { AccessPermissionId{ 9999 } }, &diagnostic)
			&& store.locationRequirement(room) == std::vector<AccessPermissionId>{ blue, amber },
			"unknown Location permission changed the requirement");

		require(store.setLocationRequirement(room, {}, &diagnostic),
			"clearing the Location requirement refused: " + diagnostic);
		require(store.locationRequirement(room).empty(), "cleared Location still restricted");
		requireThrows([&]{ store.locationRequirement(9999); },
			"requirement of unknown sector did not throw");

		// ---------------------------------------------------------------
		// Interaction-point requirements.
		// ---------------------------------------------------------------
		require(store.isInteractionPointEligible(lightSwitch.interactionPoint),
			"light-switch control refused as eligible");
		require(store.interactionPointRequirement(lightSwitch.interactionPoint).empty(),
			"fresh interaction point is restricted");

		require(store.setInteractionPointRequirement(lightSwitch.interactionPoint, { blue }, &diagnostic),
			"interaction-point edit refused: " + diagnostic);
		require(store.interactionPointRequirement(lightSwitch.interactionPoint)
			== std::vector<AccessPermissionId>{ blue },
			"interaction-point requirement did not round-trip");
		require(!store.setInteractionPointRequirement(lightSwitch.interactionPoint, { blue, blue }, &diagnostic)
			&& store.interactionPointRequirement(lightSwitch.interactionPoint)
				== std::vector<AccessPermissionId>{ blue },
			"duplicate interaction-point permission accepted");
		require(!store.setInteractionPointRequirement(InteractionPointId{ 9999 }, { blue }, &diagnostic),
			"unknown interaction point accepted");
		requireThrows([&]{ store.interactionPointRequirement(InteractionPointId{ 9999 }); },
			"requirement of unknown interaction point did not throw");

		require(store.setInteractionPointRequirement(lightSwitch.interactionPoint, {}, &diagnostic),
			"clearing the interaction-point requirement refused: " + diagnostic);
		require(store.interactionPointRequirement(lightSwitch.interactionPoint).empty(),
			"cleared interaction point still restricted");

		// ---------------------------------------------------------------
		// Manual ordinary Door requirements.
		// ---------------------------------------------------------------
		require(store.isManualDoorEligible(door.traversalResource),
			"manual ordinary Door refused as eligible");
		require(store.manualDoorRequirement(door.traversalResource).empty(),
			"fresh manual Door is restricted");

		require(store.setManualDoorRequirement(door.traversalResource, { blue }, &diagnostic),
			"manual-door edit refused: " + diagnostic);
		require(store.manualDoorRequirement(door.traversalResource)
			== std::vector<AccessPermissionId>{ blue },
			"manual-door requirement did not round-trip");
		require(!store.setManualDoorRequirement(door.traversalResource, { amber, amber }, &diagnostic)
			&& store.manualDoorRequirement(door.traversalResource)
				== std::vector<AccessPermissionId>{ blue },
			"duplicate manual-door permission accepted");
		require(!store.setManualDoorRequirement(TraversalResourceId{ 9999 }, { blue }, &diagnostic),
			"unknown Door accepted");
		requireThrows([&]{ store.manualDoorRequirement(TraversalResourceId{ 9999 }); },
			"requirement of unknown Door did not throw");

		require(store.setManualDoorRequirement(door.traversalResource, {}, &diagnostic),
			"clearing the manual-door requirement refused: " + diagnostic);
		require(store.manualDoorRequirement(door.traversalResource).empty(),
			"cleared manual Door still restricted");

		// ---------------------------------------------------------------
		// Transport destination requirements. World B: a Lift whose stops
		// span three corridor Levels.
		// ---------------------------------------------------------------
		World liftWorld("pr-lift-checks", 10, 3);
		for (uint32_t level = 0; level < 3; ++level) liftWorld.addCorridor(level, 0, 10);
		World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 2;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = liftWorld.addLift(1, 0, 8, liftOptions);
		liftWorld.finishBuild();
		PermissionRequirementStore liftStore(liftWorld);
		liftWorld.pauseSimulation();
		auto const red = liftWorld.addAccessPermission("Red key");
		auto const green = liftWorld.addAccessPermission("Green key");
		liftWorld.resumeSimulation();

		auto const sector = lift.lift.sector->getIndex();
		require(liftStore.destinationRequirement(sector, 0).empty(),
			"fresh destination Stop is restricted");

		require(!liftStore.setDestinationRequirement(sector, 1, { red }, &diagnostic)
			&& diagnostic.find("paused") != std::string::npos,
			"destination edit accepted while running");

		liftWorld.pauseSimulation();
		require(liftStore.setDestinationRequirement(sector, 1, { green, red }, &diagnostic),
			"destination edit refused: " + diagnostic);
		require(liftStore.destinationRequirement(sector, 1)
			== std::vector<AccessPermissionId>{ red, green },
			"destination requirement not sorted by permission slot");
		require(liftStore.destinationRequirement(sector, 0).empty(),
			"sibling Stop changed by an unrelated edit");
		require(!liftStore.setDestinationRequirement(sector, 1, { green, green }, &diagnostic)
			&& liftStore.destinationRequirement(sector, 1)
				== std::vector<AccessPermissionId>{ red, green },
			"duplicate destination permission accepted");
		require(!liftStore.setDestinationRequirement(sector, 9, { green }, &diagnostic),
			"unknown destination Stop accepted");
		requireThrows([&]{ liftStore.destinationRequirement(sector, 9); },
			"requirement of unknown destination Stop did not throw");

		require(liftStore.setDestinationRequirement(sector, 1, {}, &diagnostic),
			"clearing the destination requirement refused: " + diagnostic);
		require(liftStore.destinationRequirement(sector, 1).empty(),
			"cleared destination Stop still restricted");

		// ---------------------------------------------------------------
		// The World facade forwards to the same state the store edits.
		// ---------------------------------------------------------------
		require(world.setLocationPermissionRequirement(room, { amber }, &diagnostic),
			"facade Location edit refused: " + diagnostic);
		require(store.locationRequirement(room) == std::vector<AccessPermissionId>{ amber },
			"facade Location edit did not reach the store");
		require(store.setInteractionPointRequirement(lightSwitch.interactionPoint, { amber }, &diagnostic)
			&& world.getInteractionPointPermissionRequirement(lightSwitch.interactionPoint)
				== std::vector<AccessPermissionId>{ amber },
			"store interaction-point edit did not reach the facade");
		require(world.setManualDoorPermissionRequirement(door.traversalResource, { blue }, &diagnostic)
			&& store.manualDoorRequirement(door.traversalResource)
				== std::vector<AccessPermissionId>{ blue },
			"facade manual-door edit did not reach the store");
		require(liftWorld.setLiftDestinationPermissionRequirement(sector, 2, { red }, &diagnostic)
			&& liftStore.destinationRequirement(sector, 2)
				== std::vector<AccessPermissionId>{ red },
			"facade destination edit did not reach the store");
		require(world.isLocationPermissionEligible(room)
			== store.isLocationEligible(room)
			&& world.isInteractionPointPermissionEligible(lightSwitch.interactionPoint)
				== store.isInteractionPointEligible(lightSwitch.interactionPoint)
			&& world.isManualDoorPermissionEligible(door.traversalResource)
				== store.isManualDoorEligible(door.traversalResource),
			"facade eligibility disagrees with the store");

		std::cout << "PermissionRequirementStore checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "PermissionRequirementStore checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
