#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/DoorSectorObject.h"
#include <cmath>

namespace
{
	using smoke::require;
	void assign(core::World& world, core::AgentId id)
	{
		std::string diagnostic;
		require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour,
			core::PlayerControlledBehaviourRevision, {}, &diagnostic), diagnostic);
	}
	float x(core::World const& world, core::AgentId id)
	{ return world.lookupAgent(id).entity->getGlobalPosition().x; }
	void walk(core::World& world, int direction, float seconds)
	{
		if (world.isTraversalTopologyDirty())
			require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
		require(world.resumeSimulation(), "Could not resume Player World: " + world.getTopologyDiagnostic());
		world.setPlayerMovementDirection(direction);
		world.update(seconds);
	}
	void near(float actual, float expected, char const* message)
	{ require(std::abs(actual - expected) < 0.0001f, message); }

	void basics()
	{
		core::World world("Player control", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
		world.finishBuild(); world.pauseSimulation();
		require(!world.getPlayerControlledAgent(), "No assignment must disable controls");
		auto first = world.createAgent("First", room, 0, 1.f);
		auto second = world.createAgent("Second", room, 0, 3.f);
		assign(world, second); assign(world, first);
		require(world.getPlayerControlledAgent() == first, "Selection must use lowest ID, not assignment order");
		require(world.agentBehaviourOwnsMovement(first), "Player behaviour must own movement in editor");
		std::string diagnostic;
		require(!world.validateAgentBehaviourAssignment(core::PlayerControlledBehaviour, 2, {}, nullptr, &diagnostic)
			&& !world.validateAgentBehaviourAssignment(core::PlayerControlledBehaviour, 1, {{"bad", true}}, nullptr, &diagnostic),
			"Built-in revision/configuration must be immutable");
		world.setAgentIndividualWalkSpeedModifier(first, 1.4f);
		auto speed = world.lookupAgent(first).entity->getWalkSpeed();
		walk(world, 0, 1.f);
		near(x(world, first), 1.f, "Editor simulation must idle");
		walk(world, 1, 1.f);
		near(x(world, first), 1.f + speed, "Held movement must use effective speed immediately");
		near(x(world, second), 3.f, "Additional eligible Agents must idle");
		walk(world, 0, 1.f);
		near(x(world, first), 1.f + speed, "Release must stop immediately");
		walk(world, -1, 1.f);
		near(x(world, first), 1.f, "Left must reverse movement");
		require(world.lookupAgent(first).entity->getPose() == core::Pose::Standing, "Player movement must remain Standing");
		world.pauseSimulation(); world.setAgentActive(first, false);
		walk(world, 1, 1.f);
		near(x(world, first), 1.f, "Inactive Player must not move");
		near(x(world, second), 3.f, "Inactive first must not silently select second");
		world.pauseSimulation(); world.setAgentActive(first, true);
		walk(world, 1, 1.f);
		world.pauseSimulation();
		world.resumeSimulation(); world.update(1.f);
		near(x(world, first), 1.f + speed, "Pause must clear stale input");
		world.pauseSimulation(); world.clearAgentBehaviourAssignment(first);
		require(world.getPlayerControlledAgent() == second, "Clearing first must select next eligible Agent");
		world.clearAgentBehaviourAssignment(second);
		walk(world, 1, 1.f);
		near(x(world, second), 3.f, "No eligible Agent must disable controls");
	}

	void boundaries()
	{
		core::World world("Player boundaries", 12, 3);
		auto left = world.addRoom("Left", 0, 0, 0, 3, 1);
		auto middle = world.addRoom("Middle", 0, 0, 3, 3, 1);
		auto low = world.addRoom("Low", 0, 0, 6, 3, 1);
		world.finishBuild(); world.pauseSimulation();
		world.setRoomHeightScale(low, 0.4f);
		auto player = world.createAgent("Player", left, 0, 1.f); assign(world, player);
		walk(world, 1, 10.f);
		near(x(world, player), 2.8f, "Solid wall must stop the leading body edge");
		world.pauseSimulation(); world.removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		walk(world, 1, 10.f);
		require(world.lookupAgent(player).entity->getSector()->getIndex() == middle,
			"Open boundary must transfer Sector membership");
		near(x(world, player), 5.8f, "Next wall must stop movement");
		world.pauseSimulation(); world.removeLocationWall(middle, 0, CORE_SIDE_RIGHT);
		walk(world, 1, 10.f);
		near(x(world, player), 5.8f, "Open low Room must refuse Standing entry");
		require(world.lookupAgent(player).entity->getPose() == core::Pose::Standing, "Low entry must not auto-crouch");
		world.pauseSimulation();
		auto trapped = world.createAgent("Low start", low, 0, 1.f); assign(world, trapped);
		world.clearAgentBehaviourAssignment(player);
		auto pose = world.lookupAgent(trapped).entity->getPose();
		walk(world, -1, 3.f);
		near(x(world, trapped), 7.f, "Low starting Room must refuse movement");
		require(world.lookupAgent(trapped).entity->getPose() == pose, "Low start must not force an impossible Standing pose");

		core::World gaps("Player gaps", 8, 3);
		auto tall = gaps.addRoom("Tall", 0, 0, 0, 8, 2);
		gaps.addSectorWalkway(tall, 1, 0); gaps.addSectorWalkway(tall, 1, 1);
		gaps.addSectorWalkway(tall, 1, 3);
		gaps.finishBuild(); gaps.pauseSimulation();
		auto walker = gaps.createAgent("Walkway player", tall, 1, 0.5f); assign(gaps, walker);
		walk(gaps, 1, 10.f);
		near(x(gaps, walker), 1.8f, "Floor gap must stop Player before its leading edge loses support");
	}

	void bulkheads()
	{
		core::World world("Player Bulkhead", 10, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
		world.addSectorMarker(right, 0, 1.f, "Right marker");
		core::World::CreateBulkheadDoorOptions options;
		options.activationMode = core::DoorActivationMode::Manual;
		options.controls[0] = options.controls[1] = false;
		world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_RIGHT, options);
		world.finishBuild(); world.pauseSimulation();
		auto id = world.createAgent("Player", left, 0, 2.f); assign(world, id);
		std::shared_ptr<core::BulkheadDoor> door;
		for (uint32_t i = 0; i < world.getSector(left)->getNumObjects(); ++i)
			if (auto object = std::dynamic_pointer_cast<core::BulkheadDoorSectorObject>(world.getSector(left)->getObject(i)))
				door = object->getDoor();
		require(bool(door), "Missing Bulkhead fixture");
		walk(world, 1, 5.f);
		near(x(world, id), 3.7f, "Closed Bulkhead must block the leading body edge at its physical surface");
		require(!door->isOpen() && !door->isOpening(), "Player must not operate a manual Door");
		door->acquireOpenLease();
		require(door->open(), "Cannot externally open Bulkhead");
		walk(world, 0, 6.1f);
		require(door->isOpen(), "Bulkhead must finish opening before Player crosses");
		walk(world, 1, 2.f);
		require(x(world, id) > 4.f && world.lookupAgent(id).entity->getSector()->getIndex() != left,
			"Fully open Bulkhead must allow same-Layer Player passage");
		walk(world, -1, 2.f);
		require(x(world, id) < 4.f && world.lookupAgent(id).entity->getSector()->getIndex() == left,
			"Open Bulkhead must allow reverse passage");
		door->releaseOpenLease();
	}

	void doorways()
	{
		core::World world("Player Door", 8, 2);
		auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
		auto back = world.addRoom("Back", 1, 0, 0, 8, 1);
		core::World::CreateDoorOptions options;
		options.holdOpenSeconds = 30.f;
		auto result = world.addSectorDoor(0, 0, 2, options);
		auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(result.door.sector->getObject(result.door.index))->getDoor();
		world.finishBuild(); world.pauseSimulation();
		auto id = world.createAgent("Player", front, 0, 2.5f); assign(world, id);
		world.setAgentIndividualObjectUsageDistance(id, 0.f);
		walk(world, 0, 0.f);
		world.requestPlayerDoorAction(); world.update(1.f / 60.f);
		require(door->isOpening(), "Up must open manual Door despite zero interaction range");
		world.update(3.f);
		require(door->isOpen() && world.lookupAgent(id).entity->getSector()->getIndex() == front,
			"Opening must not queue or automatically perform crossing");
		world.requestPlayerDoorAction(); world.update(1.f / 60.f);
		require(world.lookupAgent(id).entity->hasActiveLocomotionTask()
			&& world.lookupAgent(id).entity->getState() == core::Agent::State::TraversingEdge,
			"Open doorway must start an authoritative timed crossing");
		world.setPlayerMovementDirection(1); world.update(1.f / 60.f);
		near(x(world, id), 2.5f, "Walking must be ignored during crossing");
		world.requestPlayerDoorAction(); // Underway presses are discarded, not queued for return.
		world.pauseSimulation(); world.update(1.f);
		require(world.setDoorBroken(result.traversalResource, true), "Cannot break admitted Door");
		require(world.lookupAgent(id).entity->getSector()->getIndex() == front,
			"Pause must freeze admitted crossing");
		require(world.resumeSimulation(), "Cannot resume admitted crossing"); world.update(.2f);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == back
			&& !world.lookupAgent(id).entity->hasActiveLocomotionTask(), "Admitted crossing must survive Broken edits, commit and release its lane");
		near(x(world, id), 2.5f, "Layer transition must preserve X");
		world.requestPlayerDoorAction(); world.update(.2f);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == front, "Up must cross in reverse too");
		world.requestPlayerDoorAction(); world.resetSimulation(); world.resumeSimulation(); world.update(.2f);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == front && !door->isOpening(),
			"Reset must discard pending Up and restore authored position");
	}

	void doorwayRestrictions()
	{
		auto fixture = [](float playerX, core::World::CreateDoorOptions options, auto check) {
			core::World world("Door restrictions", 8, 3);
			auto front = world.addRoom("Front", 0, 0, 0, 8, 2);
			auto back = world.addRoom("Back", 1, 0, 0, 8, 2);
			options.holdOpenSeconds = 30.f;
			auto result = world.addSectorDoor(0, 0, 2, options);
			auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(result.door.sector->getObject(result.door.index))->getDoor();
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", front, 0, playerX); assign(world, id);
			check(world, id, door, result.traversalResource, back);
		};
		auto press = [](core::World& world) { walk(world, 0, 0.f); world.requestPlayerDoorAction(); world.update(.2f); };
		fixture(1.9f, {}, [&](auto& world, auto, auto door, auto, auto) {
			press(world); require(door->isClosed(), "Outside doorway X-bounds must not open");
		});
		fixture(2.15f, {}, [&](auto& world, auto id, auto door, auto, auto) {
			press(world); world.update(3.f); require(door->isOpen(), "Centre-in-bounds must permit opening");
			press(world); require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0,
				"Body outside aperture must prevent crossing without auto-aligning");
		});
		fixture(2.5f, {}, [&](auto& world, auto id, auto door, auto resource, auto back) {
			auto permission = world.addAccessPermission("Door access");
			world.setManualDoorPermissionRequirement(resource, {permission});
			press(world); require(door->isClosed(), "Unauthorized player cannot open");
			world.pauseSimulation(); world.grantAgentAccessPermission(id, permission);
			world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
			press(world); require(door->isClosed(), "None capability cannot open");
			world.pauseSimulation(); world.setAgentIndividualObjectUsage(id, core::ObjectUsage::RemoteControl);
			world.setAgentIndividualObjectUsageDistance(id, 0.f);
			press(world); world.update(3.f); require(door->isOpen(), "Remote player also ignores distance");
			world.pauseSimulation(); world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
			world.setLocationPermissionRequirement(back, {world.addAccessPermission("Room access")});
			press(world); require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0,
				"Location authorization must still gate entry");
			world.pauseSimulation(); world.setLocationPermissionRequirement(back, {});
			press(world); require(world.lookupAgent(id).entity->getSector()->getIndex() == back,
				"None may cross an already open authorized doorway");
		});
		fixture(2.5f, {}, [&](auto& world, auto id, auto door, auto, auto) {
			auto profile = world.lookupAgent(id).entity->getEffectiveMobilityProfile().value;
			profile.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
			world.setAgentIndividualMobilityProfile(id, profile);
			press(world); require(door->isClosed(), "Forbidden Door Mobility must prevent opening");
		});
		fixture(2.5f, {}, [&](auto& world, auto id, auto door, auto resource, auto back) {
			press(world); world.update(3.f);
			world.pauseSimulation();
			auto permission = world.addAccessPermission("Protected open Door");
			world.setManualDoorPermissionRequirement(resource, {permission});
			press(world); require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0,
				"Permission-adhering player must decline an unauthorized open Door");
			world.pauseSimulation(); world.setAgentIndividualPermissionAdherence(id, false);
			press(world); require(world.lookupAgent(id).entity->getSector()->getIndex() == back,
				"Non-adhering player may use an already open Door");
			require(door->admitsNewCrossings(), "Passage must not close the Door");
		});
		core::World::CreateDoorOptions broken;
		broken.initiallyBroken = true;
		fixture(2.5f, broken, [&](auto& world, auto, auto door, auto, auto) {
			press(world); require(door->isClosed(), "Broken closed Door must not open");
		});
		broken.brokenOpenPercentage = 1.f;
		fixture(2.5f, broken, [&](auto& world, auto id, auto, auto, auto back) {
			press(world); require(world.lookupAgent(id).entity->getSector()->getIndex() == back,
				"Fully open Broken Door remains passable");
		});
		core::World::CreateDoorOptions low; low.heightScale = .4f;
		fixture(2.5f, low, [&](auto& world, auto id, auto door, auto, auto) {
			press(world); world.update(3.f); require(door->isOpen(), "Low Door can be opened");
			press(world); require(world.lookupAgent(id).entity->getSector()->getLayerIndex() == 0
				&& world.lookupAgent(id).entity->getPose() == core::Pose::Standing,
				"Low opening must refuse crossing, not auto-crouch");
		});
		core::World::CreateDoorOptions buttons; buttons.activationMode = core::DoorActivationMode::RemoteControlled;
		buttons.controls[0] = buttons.controls[1] = true;
		fixture(2.5f, buttons, [&](auto& world, auto, auto door, auto, auto) {
			press(world); require(door->isClosed(), "Up must not operate a Button-required Door");
		});
	}

	void doorwaySelection()
	{
		auto run = [](bool bothOpen) {
			core::World world("Overlapping Player Doors", 8, 2);
			world.addLayer();
			auto front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto middle = world.addRoom("Middle", 1, 0, 0, 8, 1);
			auto back = world.addRoom("Back", 2, 0, 0, 8, 1);
			core::World::CreateDoorOptions options;
			options.holdOpenSeconds = 30.f;
			auto a = world.addSectorDoor(0, 0, 2, options);
			if (!bothOpen) options.width = 2;
			auto b = world.addSectorDoor(1, 0, 2, options);
			auto first = std::dynamic_pointer_cast<core::DoorSectorObject>(a.door.sector->getObject(a.door.index))->getDoor();
			auto second = std::dynamic_pointer_cast<core::DoorSectorObject>(b.door.sector->getObject(b.door.index))->getDoor();
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", middle, 0, 2.5f); assign(world, id);
			second->requestOpen(); if (bothOpen) first->requestOpen();
			walk(world, 0, 3.f);
			world.requestPlayerDoorAction(); world.update(.2f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == (bothOpen ? front : back),
				bothOpen ? "Coincident passable Doors must use stable identity tie-break"
				: "An open farther doorway must take priority over a nearer closed Door");
			if (!bothOpen) require(first->isClosed(), "Crossing priority must not open another Door");
		};
		run(false); run(true);
	}

	void persistenceAndPermissions()
	{
		core::World world("Portable Player", 10, 2);
		auto left = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto right = world.addRoom("Right", 0, 0, 4, 4, 1);
		world.finishBuild(); world.pauseSimulation();
		world.removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		auto permission = world.addAccessPermission("Entry");
		world.setLocationPermissionRequirement(right, {permission});
		auto id = world.createAgent("Player", left, 0, 3.f); assign(world, id);
		walk(world, 1, 4.f);
		near(x(world, id), 3.8f, "Player cannot bypass Location permission requirements");
		world.pauseSimulation(); world.grantAgentAccessPermission(id, permission);
		walk(world, 1, 2.f);
		require(world.lookupAgent(id).entity->getSector()->getIndex() == right, "Authorized open passage must work");
		world.pauseSimulation();
		auto registry = core::AgentBehaviourRegistry::create();
		world.attachAgentBehaviourRegistry("behaviours.yaml", registry);
		world.detachAgentBehaviourRegistryAndClearAssignments();
		require(world.lookupAgent(id).entity->isPlayerControlled(), "Detaching registry must not clear a built-in");
		auto roundTrip = [&](auto writer, auto makeReader)
		{
			core::SerializationWorkData data; data.markSerializedUnmodified = false;
			world.serialize(*writer, data); writer->serialize();
			auto text = writer->getSerializedString();
			auto reader = makeReader(text);
			reader->deserialize();
			core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, data), "Player World must round trip without registry");
			require(loaded.getPlayerControlledAgent() == id && loaded.lookupAgent(id).entity->isPlayerControlled(),
				"Round trip must retain built-in assignment and ID");
		};
		roundTrip(core::YamlSerializer::toString(), [](auto const& text) { return core::YamlSerializer::fromString(text); });
		roundTrip(core::BinarySerializer::toString(), [](auto const& text) { return core::BinarySerializer::fromString(text); });
		walk(world, 1, 1.f); world.resetSimulation();
		require(world.getPlayerControlledAgent() == id, "Reset must preserve built-in assignment");
		world.resumeSimulation(); world.update(1.f);
		near(x(world, id), 3.f, "Reset must restore authored position and discard input");
	}
}

void agent_smoke::registerPlayerControl(std::vector<smoke::Check>& checks)
{
	checks.push_back({"playerControlBasics", [](smoke::Context const&) { basics(); }});
	checks.push_back({"playerControlBoundaries", [](smoke::Context const&) { boundaries(); }});
	checks.push_back({"playerControlBulkheads", [](smoke::Context const&) { bulkheads(); }});
	checks.push_back({"playerControlDoorways", [](smoke::Context const&) { doorways(); }});
	checks.push_back({"playerControlDoorwayRestrictions", [](smoke::Context const&) { doorwayRestrictions(); doorwaySelection(); }});
	checks.push_back({"playerControlPersistence", [](smoke::Context const&) { persistenceAndPermissions(); }});
}
