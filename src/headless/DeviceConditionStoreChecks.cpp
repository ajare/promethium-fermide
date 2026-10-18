#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/DeviceConditionStore.h"
#include "core/Exceptions.h"
#include "core/World.h"

#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/StaircaseTransit.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("DeviceConditionStore check failed: " + what);
		++checksRun;
	}

	core::Door& doorOf(core::World const& world, core::Sector const& sector, uint32_t index)
	{
		auto object = std::static_pointer_cast<core::DoorSectorObject>(sector.getObject(index));
		if (!object || !object->getDoor()) throw std::runtime_error("no Door at index");
		return *object->getDoor();
	}
}

int main()
{
	using namespace core;

	try
	{
		// The store is constructed directly against a World it does not own:
		// the ADR 0022 proof-of-pattern is that the authored-vs-runtime
		// Broken family is unit-testable without driving the World facade.
		// World keeps the state it edits (ADR 0001); the collaborator holds
		// only a reference, and device operations stay with the coordinator.

		// ---------------------------------------------------------------
		// Ordinary Door: authored mirror, live-only edits, speed override,
		// sensor observation, and the control-target query.
		// ---------------------------------------------------------------
		{
			World world("dc-door", 12, 3);
			DeviceConditionStore store(world);
			auto front = world.addRoom("Front", 0, 0, 0, 11, 1);
			world.addRoom("Back", 1, 0, 0, 11, 1);
			World::CreateDoorOptions doorOptions;
			doorOptions.activationMode = DoorActivationMode::RemoteControlled;
			doorOptions.controls[0] = true;
			auto made = world.addSectorDoor(0, 0, 2, doorOptions);
			world.finishBuild();
			world.markSaved();
			auto const id = made.traversalResource;
			auto& door = doorOf(world, *made.door.sector, made.door.index);
			auto const button = made.controls[0].interactionPoint;

			// Running: every authored edit is refused, nothing is dirtied.
			world.resumeSimulation();
			require(!store.setDoorInitiallyBroken(id, true) && !world.isModified(),
				"authored Door break accepted while running");
			require(!store.setDoorSpeedOverride(id, 0.5f) && !world.isModified(),
				"speed override accepted while running");

			// Live edits are allowed while running and never dirty the document.
			require(store.setDoorBroken(id, true) && door.isBroken() && !door.isInitiallyBroken()
				&& !world.isModified(), "live Door break changed authored/dirty state");
			require(store.setDoorBroken(id, false) && !door.isBroken(),
				"live Door restore failed");

			// Paused authored edit: record mirror and live Door move together,
			// and the Door freezes at its authored broken-open position.
			world.pauseSimulation();
			require(store.setDoorInitiallyBroken(id, true) && door.isBroken()
				&& door.isInitiallyBroken() && world.isModified(),
				"paused authored Door break failed");
			require(door.getOpenPercentage() == door.getBrokenOpenPercentage(),
				"authored break did not freeze Door at broken-open position");
			require(store.setDoorInitiallyBroken(id, true), "repeat authored break not idempotent");

			// The control-target query follows the live condition.
			require(store.isControlTargetBroken(button), "Button target not seen Broken");
			store.setDoorBroken(id, false);
			require(!store.isControlTargetBroken(button), "Button target still Broken");
			require(!store.isControlTargetBroken(InteractionPointId{}),
				"null interaction point reported Broken target");

			// Speed override: paused-only, validated, mirrored on the Door.
			require(!store.setDoorSpeedOverride(id, -1.0f), "negative speed accepted");
			require(store.setDoorSpeedOverride(id, 0.5f) && door.getSpeedOverride() == 0.5f,
				"speed override not applied");
			require(store.setDoorSpeedOverride(id, std::nullopt) && !door.getSpeedOverride().has_value(),
				"speed override not cleared");

			// Sensor observations live on the World-owned resource map.
			DoorSensorId sensor;
			require(!store.setDoorSensorObservation(id, sensor, DoorSensorObservation::Obstruction),
				"null sensor accepted");
			sensor = DoorSensorId{ 1 };
			require(store.setDoorSensorObservation(id, sensor, DoorSensorObservation::Obstruction),
				"sensor observation refused");
			require(store.setDoorSensorObservation(id, sensor, DoorSensorObservation::Clear),
				"sensor clear refused");

			// Facade (design pattern) and store agree on refusals too.
			require(world.setDoorBroken(TraversalResourceId{}, true)
					== store.setDoorBroken(TraversalResourceId{}, true),
				"facade and store disagree on unknown resource");
		}

		// ---------------------------------------------------------------
		// Escalator: escalator-only authored edits, live break anywhere.
		// ---------------------------------------------------------------
		{
			World world("dc-escalator", 12, 3);
			DeviceConditionStore store(world);
			world.addCorridor(0, 0, 11);
			world.addCorridor(1, 0, 11);
			auto const escalator = world.addStaircase(1, 0, 0, 4, CORE_SIDE_RIGHT, 1.0f);
			auto const stationary = world.addStaircase(1, 0, 6, 4, CORE_SIDE_RIGHT, 0.0f);
			world.finishBuild();
			world.markSaved();

			world.resumeSimulation();
			require(!store.setEscalatorInitiallyBroken(escalator, true) && !world.isModified(),
				"authored Escalator break accepted while running");

			world.pauseSimulation();
			require(!store.setEscalatorInitiallyBroken(stationary, true) && !world.isModified(),
				"authored break accepted for a stationary Staircase");
			require(!store.setEscalatorInitiallyBroken(0, true),
				"authored break accepted for a non-Staircase sector");
			require(store.setEscalatorInitiallyBroken(escalator, true) && world.isModified(),
				"paused authored Escalator break failed");
			auto transit = std::static_pointer_cast<const StaircaseTransit>(world.getSector(escalator));
			require(transit->getStaircase()->isBroken() && transit->getStaircase()->isInitiallyBroken(),
				"authored Escalator break did not reach the live Staircase");

			// Live break works while running and never touches authored state.
			world.resumeSimulation();
			require(store.setEscalatorBroken(escalator, false)
				&& !transit->getStaircase()->isBroken()
				&& transit->getStaircase()->isInitiallyBroken(),
				"live Escalator restore changed authored state");
			require(!store.setEscalatorBroken(100000u, true),
				"live break accepted for an out-of-range sector");
		}

		// ---------------------------------------------------------------
		// Lift: authored mirror, vehicle-wide live propagation, and the
		// condition query's visibility rule.
		// ---------------------------------------------------------------
		{
			World world("dc-lift", 8, 4);
			DeviceConditionStore store(world);
			auto lower = world.addCorridor(0, 0, 7);
			world.addCorridor(2, 0, 7);
			World::CreateLiftOptions liftOptions;
			liftOptions.cellsWide = 1;
			liftOptions.stopOffsets = { 0, 2 };
			auto created = world.addLift(1, 0, 3, liftOptions);
			world.finishBuild();
			world.markSaved();
			auto const id = created.traversalResource;
			auto const lowerSector = world.getSector(lower).get();

			world.resumeSimulation();
			require(!store.setLiftInitiallyBroken(id, true) && !world.isModified(),
				"authored Lift break accepted while running");

			world.pauseSimulation();
			require(store.setLiftInitiallyBroken(id, true) && world.isModified(),
				"paused authored Lift break failed");
			require(store.knownTransportCondition(id, nullptr, lowerSector)->broken,
				"visible Lift condition not reported Broken");
			// A landing Door resource resolves through to the vehicle.
			require(store.knownTransportCondition(created.doors[0].traversalResource,
					nullptr, lowerSector)->broken,
				"landing Door did not resolve to the broken vehicle");
			// Out of sight with no Agent: nothing is knowable.
			require(!store.knownTransportCondition(id, nullptr, nullptr).has_value(),
				"invisible Lift reported a condition with no observer");
			// A non-transport resource has no transport condition.
			{
				World plain("dc-plain", 12, 3);
				DeviceConditionStore plainStore(plain);
				plain.addRoom("Front", 0, 0, 0, 11, 1);
				plain.addRoom("Back", 1, 0, 0, 11, 1);
				auto plainDoor = plain.addSectorDoor(0, 0, 2);
				plain.finishBuild();
				require(!plainStore.knownTransportCondition(plainDoor.traversalResource,
						nullptr, plainDoor.door.sector.get()).has_value(),
					"ordinary Door reported a transport condition");
			}

			// Live restoration is vehicle-wide: landing Doors recover with it.
			world.resumeSimulation();
			require(store.setLiftBroken(id, false), "live Lift restore failed");
			require(!store.knownTransportCondition(id, nullptr, lowerSector)->broken,
				"restored Lift still reported Broken");
			require(!store.setLiftBroken(created.doors[0].traversalResource, true),
				"a landing Door was broken independently of the vehicle");
		}

		// ---------------------------------------------------------------
		// Shuttle: one authored condition for the complete coupled vehicle.
		// ---------------------------------------------------------------
		{
			World world("dc-shuttle", 16, 2);
			DeviceConditionStore store(world);
			world.addRoom("Left platform", 0, 0, 0, 4, 1);
			world.addRoom("Right platform", 0, 0, 10, 4, 1);
			World::CreateShuttleOptions shuttleOptions{ 1, 4, { 0, 10 }, 0 };
			auto created = world.addShuttle(1, 0, 0, 15, shuttleOptions);
			world.finishBuild();
			world.markSaved();
			auto const id = created.traversalResource;
			auto const leftSector = world.getSector(0).get();

			world.resumeSimulation();
			require(!store.setShuttleInitiallyBroken(id, true) && !world.isModified(),
				"authored Shuttle break accepted while running");

			world.pauseSimulation();
			require(store.setShuttleInitiallyBroken(id, true) && world.isModified(),
				"paused authored Shuttle break failed");
			require(store.knownTransportCondition(id, nullptr, leftSector)->broken,
				"visible Shuttle condition not reported Broken");
			world.resumeSimulation();
			require(store.setShuttleBroken(id, false)
				&& !store.knownTransportCondition(id, nullptr, leftSector)->broken,
				"live Shuttle restore failed");
			require(!store.setShuttleBroken(TraversalResourceId{ 1u << 20 }, true),
				"unknown resource accepted as a Shuttle");
		}

		// ---------------------------------------------------------------
		// Extensible Ladder: authored mirror and live break; a plain
		// (non-extensible) Ladder is refused.
		// ---------------------------------------------------------------
		{
			World world("dc-extensible", 12, 3);
			DeviceConditionStore store(world);
			world.addCorridor(0, 0, 11);
			world.addCorridor(1, 0, 11);
			World::CreateLadderOptions ladderOptions{ 2, true, false };
			auto extensible = world.addLadder(1, 0, 2, ladderOptions);
			World::CreateLadderOptions plainOptions{ 2, false, false };
			auto plain = world.addLadder(1, 0, 6, plainOptions);
			world.finishBuild();
			world.markSaved();

			world.resumeSimulation();
			require(!store.setExtensibleInitiallyBroken(extensible.traversalResource, true)
				&& !world.isModified(),
				"authored extensible break accepted while running");

			world.pauseSimulation();
			require(store.setExtensibleInitiallyBroken(extensible.traversalResource, true)
				&& world.isModified(),
				"paused authored extensible break failed");
			require(store.setExtensibleBroken(extensible.traversalResource, false)
				&& !store.setExtensibleBroken(plain.traversalResource, true),
				"non-extensible Ladder accepted by the live setter");
			require(!store.setExtensibleInitiallyBroken(plain.traversalResource, false),
				"authored edit accepted for a Ladder with no matching record state");
		}

		std::cout << "DeviceConditionStore checks passed: " << checksRun << "\n";
		return 0;
	}
	catch (std::exception const& e)
	{
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
