#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/LadderMountEdge.h"
#include "core/MobilityProfile.h"
#include "core/Stairwell.h"
#include "core/AgentType.h"
#include "core/Graph.h"
#include <algorithm>
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
	{ require(std::abs(actual - expected) < 0.0001f,
		std::string(message) + ": actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected)); }

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
			press(world); require(door->isClosed(), "Up at a Door centre must not auto-approach its Buttons");
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

	void stairwells()
	{
		for (int mountSide : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
		{
			core::World world("Player Stairwell", 8, 3);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 8, 1);
			auto middle = world.addRoom("Middle", 0, 1, 0, 8, 1);
			auto top = world.addRoom("Top", 0, 2, 0, 8, 1);
			auto stairs = world.addStairwell(1, 0, 2, core::World::CreateStairwellOptions{3, mountSide,
				mountSide == CORE_SIDE_RIGHT ? 1u : 0u});
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, 3.f); assign(world, id);
			auto y = [&] { return world.lookupAgent(id).entity->getGlobalPosition().y; };
			auto sector = [&] { return world.lookupAgent(id).entity->getSector()->getIndex(); };
			auto up = [&] { world.setPlayerStairSelection(false, false); walk(world, 0, 0.f); world.requestPlayerDoorAction(); world.update(.2f); };
			auto drive = [&](int horizontal, int vertical, float seconds = 5.f) {
				world.resumeSimulation(); world.setPlayerStairSelection(vertical > 0, vertical < 0);
				walk(world, horizontal, seconds);
			};
			up(); require(sector() == stairs.sectorIndex, "Up must enter Stairwell sector");
			near(x(world, id), 3.f, "Stairwell entry must preserve X");
			if (stairs.traversalResource)
			{
				world.setTraversalResourceEnabled(stairs.traversalResource, false);
				drive(-1, 1, .2f);
				near(y(), 0.f, "Disabled narrow-stair resource must refuse new stair admission");
				world.setTraversalResourceEnabled(stairs.traversalResource, true);
			}
			int const first = mountSide == CORE_SIDE_LEFT ? 1 : -1;
			world.pauseSimulation();
			require(world.setAgentIndividualStairSpeedModifier(id, 1.4f), "Stair speed modifier must apply");
			drive(first, 1, .1f);
			auto position = world.lookupAgent(id).entity->getGlobalPosition();
			auto speed = world.lookupAgent(id).entity->getStationaryStairSpeed(true);
			near(position.distanceTo(core::Vector2{3.f, 0.f}), speed * .1f,
				"Stairs must use effective type-driven ascent speed along the slope");
			require(y() > 0.f && y() < .25f, "Arrow must move diagonally on first flight");
			walk(world, 0, 30.f);
			near(y(), position.y, "Release must stop immediately without losing stair capacity");
			require(world.lookupAgent(id).entity->hasActiveLocomotionTask(), "Stopped admitted stairs must not time out");
			world.requestPlayerDoorAction(); world.update(.2f);
			require(sector() == stairs.sectorIndex, "Up away from landing must not exit");
			world.pauseSimulation(); world.update(1.f); world.resumeSimulation(); world.update(.1f);
			near(y(), position.y, "Pause/resume must preserve exact in-flight position and clear input");
			walk(world, -first, 5.f);
			near(y(), 0.f, "Reversing an arrow mid-flight must return to the lower landing");
			require(!world.lookupAgent(id).entity->hasActiveLocomotionTask(), "Returning to source must release capacity");
			drive(first, 1); near(y(), .25f, "Ascent must reach the first bend platform");
			drive(-first, 0); near(y(), .25f, "Approach without a selection must stop at the junction");
			world.setPlayerStairSelection(true, true); walk(world, -first, .2f);
			near(y(), .25f, "Both selection keys must refuse flight entry");
			drive(-first, 1); near(y(), .75f, "Up must select ascent from the bend platform");
			drive(first, 1); near(y(), 1.f, "Arrival must walk onto the Level platform, not skip the landing");
			up(); require(sector() == middle, "Up at middle doorway must exit to its adjacent Location");
			up(); require(sector() == stairs.sectorIndex, "Up must re-enter from a middle Level");
			drive(-first, 0); // Return from the doorway side platform to its junction.
			drive(-first, 1, .2f);
			world.requestPlayerDoorAction(); world.update(.1f);
			near(y(), 1.f, "A selection opposite horizontal input must stop, never automatically turn");
			require(sector() == stairs.sectorIndex, "Departure Up must select instead of exiting the doorway");
			drive(first, 1); near(y(), 1.25f, "Second flight must be manually navigable");
			drive(-first, 1); drive(first, 1); near(y(), 2.f, "Top landing must stop movement");
			up(); require(sector() == top, "Top exit must reach its actual landing Location");
			up(); require(sector() == stairs.sectorIndex, "Top entry must be supported");
			drive(-first, 0);
			drive(-first, -1, .1f);
			position = world.lookupAgent(id).entity->getGlobalPosition();
			speed = world.lookupAgent(id).entity->getStationaryStairSpeed(false);
			near(position.distanceTo(core::Vector2{3.f, 2.f}), speed * .1f,
				"Stairs must use effective type-driven descent speed along the slope");
			for (int level = 2; level > 0; --level)
			{
				drive(-first, -1); near(y(), level - .25f, "Descending upper segment must follow stair geometry");
				drive(first, -1); near(y(), level - .75f, "Down must choose descent at the bend");
				drive(-first, -1); near(y(), float(level - 1), "Descent must reach each Level platform");
				drive(first, 0); // Approach the next departure junction without choosing yet.
			}
			up(); require(sector() == bottom, "Bottom exit must complete the round trip");
			world.resetSimulation(); world.resumeSimulation(); world.update(.2f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom, "Reset must restore authored Location");
		}
	}

	void stairwellPlatforms()
	{
		core::World world("Stairwell platforms", 8, 3);
		for (uint32_t y = 0; y < 3; ++y) world.addRoom("Landing", 0, y, 0, 8, 1);
		auto stairs = world.addStairwell(1, 0, 2, 3, CORE_SIDE_LEFT);
		world.finishBuild(); world.pauseSimulation();
		// Installed types count even when no Agent of that type has been placed.
		auto definition = core::bundledHumanAgentType();
		definition.source.replace(definition.source.find("type_id = \"Human\""), 17, "type_id = \"Wide\"");
		definition.source.replace(definition.source.find("width = 0.4"), 11, "width = 1.2");
		std::string diagnostic;
		require(world.attachAgentType("wide.agent.lua", definition.source, &diagnostic), diagnostic);
		near(world.maximumInstalledStandingWidth(), 1.2f, "Widest installed Standing type must determine wall clearance");
		auto geometry = std::dynamic_pointer_cast<core::StairwellTransit const>(world.getSector(stairs))->getStairwell();
		unsigned count = 0;
		std::shared_ptr<const core::Vertex> destination;
		for (uint32_t y = 0; y < 3; ++y)
			for (auto const& platform : geometry->getLandingPaths(y))
			{
				++count;
				near(platform[0].y, platform[1].y, "Platform must be horizontal");
				near(platform[0].distanceTo(platform[1]), .25f, "Platform must have target quarter-unit length where it fits");
				for (auto point : platform)
					require(point.x >= .6f - core::PoseFitTolerance && point.x <= 1.4f + core::PoseFitTolerance,
						"Platform vertices must fit the widest body between both walls");
				bool found = false;
				for (auto const& edge : world.getGraph()->getEdges())
					if (edge->getType() == core::EdgeType::Location
						&& edge->getVertex(0)->getSector()->getIndex() == stairs)
						for (uint32_t side = 0; side < 2; ++side)
							if (edge->getVertex(side)->getPosition().distanceTo(geometry->getPosition() + platform[0]) < core::PoseFitTolerance
								&& edge->getVertex(1 - side)->getPosition().distanceTo(geometry->getPosition() + platform[1]) < core::PoseFitTolerance)
							{
								found = true;
								if (platform[0].y == .25f) destination = edge->getVertex(1 - side);
								require(edge->getVertex(1 - side)->getEdges().size() == 1,
									"Platform endpoint must be a leaf, not inserted into autonomous through-routes");
								require(!edge->getTraversalResourceId(), "Flat platform must not consume narrow-stair admission");
							}
				require(found, "Rendered platform must have the same graph edge endpoints");
			}
		require(count == 10, "Three-Level stairwell must have six doorway branches and four bend branches");
		auto id = world.createAgent("Autonomous platform visitor", 0, 0, 3.f);
		auto* agent = world.lookupAgent(id).entity;
		auto path = world.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "Autonomous Agent must be able to route onto a side platform");
		agent->setPath(path, true); world.resumeSimulation(); world.update(10.f);
		require(agent->getSector()->getIndex() == stairs, "Autonomous platform route must enter the Stairwell");
		near(agent->getGlobalPosition().distanceTo(destination->getPosition()), 0.f,
			"Autonomous Agent must physically reach the platform endpoint");
		world.pauseSimulation();
		definition.source.replace(definition.source.find("type_id = \"Wide\""), 16, "type_id = \"Wider\"");
		definition.source.replace(definition.source.find("width = 1.2"), 11, "width = 1.4");
		require(!world.attachAgentType("wider.agent.lua", definition.source, &diagnostic)
			&& !world.hasAgentType("Wider"), "Occupied Stairwell geometry changes must be refused atomically");
	}

	void stairwellRestrictions()
	{
		core::World world("Restricted Player Stairwell", 8, 2);
		world.addLayer();
		auto bottom = world.addRoom("Bottom", 1, 0, 0, 8, 1);
		auto top = world.addRoom("Top", 1, 1, 0, 8, 1);
		auto stairs = world.addStairwell(2, 0, 2, 2, CORE_SIDE_LEFT);
		world.finishBuild(); world.pauseSimulation();
		auto id = world.createAgent("Player", bottom, 0, 2.15f); assign(world, id);
		auto up = [&] { world.setPlayerStairSelection(false, false); walk(world, 0, 0.f); world.requestPlayerDoorAction(); world.update(.2f); };
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom,
			"A body outside the Stairwell doorway must not be auto-aligned or admitted");
		walk(world, 1, .5f);
		world.pauseSimulation(); world.setRoomHeightScale(bottom, .4f);
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom,
			"Low starting clearance must refuse Stairwell entry instead of automatically crawling");
		world.pauseSimulation(); world.setRoomHeightScale(bottom, 1.f);
		auto profile = world.lookupAgent(id).entity->getEffectiveMobilityProfile().value;
		profile.set(core::TraversalKind::Stairwell, core::MobilityUse::CannotUse);
		world.setAgentIndividualMobilityProfile(id, profile);
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom,
			"Forbidden Stairwell Mobility must refuse entry");
		world.pauseSimulation(); profile.set(core::TraversalKind::Stairwell, core::MobilityUse::CanUse);
		world.setAgentIndividualMobilityProfile(id, profile);
		world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
		auto permission = world.addAccessPermission("Upper landing");
		world.setLocationPermissionRequirement(top, {permission});
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs,
			"None capability may enter an open deep-Layer Stairwell doorway");
		world.setPlayerStairSelection(true, false);
		walk(world, 1, 5.f); walk(world, -1, 5.f); walk(world, 1, 5.f);
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs,
			"Stairwell exit must respect destination Location permissions");
		world.pauseSimulation(); world.grantAgentAccessPermission(id, permission);
		up(); require(world.lookupAgent(id).entity->getSector()->getIndex() == top,
			"Authorized deep-Layer exit must reach the Location immediately in front");
	}

	void staircases()
	{
		for (int riseSide : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
		{
			core::World world("Player Staircase", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			auto top = world.addCorridor(1, 0, 12);
			auto stairs = world.addStaircase(1, 0, 4, {3, riseSide, 0.f});
			int const direction = riseSide == CORE_SIDE_RIGHT ? 1 : -1;
			float const lowerX = direction > 0 ? 4.f : 7.f;
			float const upperX = direction > 0 ? 7.f : 4.f;
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, lowerX - direction * .15f); assign(world, id);
			require(world.setAgentIndividualStairSpeedModifier(id, 1.2f), "Staircase speed modifier must apply");
			auto position = [&] { return world.lookupAgent(id).entity->getGlobalPosition(); };
			auto sector = [&] { return world.lookupAgent(id).entity->getSector()->getIndex(); };
			auto drive = [&](int horizontal, int vertical, float seconds) {
				world.resumeSimulation(); world.setPlayerStairSelection(vertical > 0, vertical < 0);
				world.setPlayerMovementDirection(horizontal);
				while (seconds > 0.f)
				{
					auto const step = std::min(seconds, 1.f);
					world.update(step); seconds -= step;
				}
			};
			drive(direction, 1, .01f);
			require(sector() == bottom && x(world, id) != lowerX, "Near approach must walk, never snap to the endpoint");
			drive(direction, 1, .6f);
			require(sector() == stairs && position().y > 0.f && position().y < 1.f, "Up and approach must mount and ascend");
			auto stopped = position();
			drive(0, 0, 1.f);
			for (int second = 0; second < 30; ++second) world.update(1.f);
			near(position().distanceTo(stopped), 0.f, "Release must stop exactly on a Staircase");
			require(world.lookupAgent(id).entity->hasActiveLocomotionTask(), "Stopped flight must retain its admitted task");
			world.pauseSimulation(); world.resumeSimulation(); world.update(.1f);
			near(position().distanceTo(stopped), 0.f, "Pause must retain flight position and clear keys");
			drive(-direction, 0, .1f);
			near(position().distanceTo(stopped), world.lookupAgent(id).entity->getStationaryStairSpeed(false) * .1f,
				"Reversal must use effective descent speed");
			drive(-direction, 0, 1.f);
			require(sector() == bottom, "Returning to the source must automatically dismount");
			// Return to the endpoint without changing Layer or automatically mounting.
			drive(direction, 1, 12.f);
			require(sector() == top, "Completing ascent must automatically enter the upper Location: sector="
				+ std::to_string(sector()) + " x=" + std::to_string(x(world, id)) + " y=" + std::to_string(position().y));
			world.setPlayerMovementDirection(0); world.update(.1f);
			// Walk back into the upper endpoint's outside approach zone.
			float const approachX = upperX + direction * .15f;
			drive(approachX > x(world, id) ? 1 : -1, 0, std::abs(x(world, id) - approachX)
				/ world.lookupAgent(id).entity->getWalkSpeed());
			require(direction * (x(world, id) - upperX) > 0.f && std::abs(x(world, id) - upperX) <= .25f,
				"Return walk must reach outside upper approach: x=" + std::to_string(x(world, id)));
			drive(-direction, -1, .6f);
			require(sector() == stairs && position().y < 1.f && position().y > 0.f, "Down and approach must mount from above: sector="
				+ std::to_string(sector()) + " x=" + std::to_string(x(world, id)) + " y=" + std::to_string(position().y));
			stopped = position();
			drive(direction, 0, .1f);
			near(position().distanceTo(stopped), world.lookupAgent(id).entity->getStationaryStairSpeed(true) * .1f,
				"Reversing descent must use ascent speed");
			drive(-direction, 0, 12.f);
			require(sector() == bottom, "Descending must automatically dismount below");
			world.resetSimulation(); world.resumeSimulation(); world.update(.1f);
			near(position().y, 0.f, "Reset must restore authored Level");
			near(x(world, id), lowerX - direction * .15f, "Reset must discard held stair input");
		}
	}

	void ladders()
	{
		for (bool embedded : {false, true})
		for (bool descending : {false, true})
		for (int approach : {-1, 1})
		{
			core::World world("Player fixed Ladder", 12, 3);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, embedded ? 3 : 1);
			auto top = embedded ? bottom : world.addCorridor(2, 0, 12);
			if (embedded) world.addSectorWalkway(bottom, 2, 4);
			if (embedded) world.addSectorLadder(bottom, 0, 4, {3, false, true});
			else world.addLadder(1, 0, 4, {3, false, true});
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", descending ? top : bottom,
				embedded && descending ? 2 : 0, 4.5f - approach * .15f); assign(world, id);
			world.setAgentIndividualLadderSpeedModifier(id, 1.4f);
			world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
			auto agent = world.lookupAgent(id).entity;
			auto position = [&] { return world.lookupAgent(id).entity->getGlobalPosition(); };
			auto drive = [&](int horizontal, int vertical, float seconds) {
				world.setPlayerMovementDirection(horizontal); world.setPlayerStairSelection(vertical > 0, vertical < 0);
				while (seconds > 0.f) { auto step = std::min(seconds, 1.f); world.update(step); seconds -= step; }
			};
			int const vertical = descending ? -1 : 1;
			world.resumeSimulation(); drive(approach, vertical, .01f);
			require(position().x != 4.5f, "Ladder approach must walk to the mount, not snap");
			near(position().y, descending ? 2.f : 0.f, "Approach must not climb before reaching the mount");
			drive(approach, vertical, .6f);
			require(position().y > 0.f && position().y < 2.f && agent->hasActiveLocomotionTask(),
				"Either fixed Ladder endpoint must mount and climb, including Room Ladders");
			near(position().x, 4.5f, "Mounted Ladder must climb on its actual vertical centreline");
			auto before = position(); drive(0, vertical, .1f);
			near(position().distanceTo(before), agent->getClimbSpeed() * .1f, "Up/Down must use effective climb speed");
			before = position(); drive(1, 0, 1.f);
			for (int second = 0; second < 35; ++second) world.update(1.f);
			near(position().distanceTo(before), 0.f, "Release must stop even with horizontal input and retain Ladder capacity");
			require(agent->hasActiveLocomotionTask(), "Stopped Player Ladder task must not time out");
			world.setPlayerStairSelection(true, true); world.update(.2f);
			near(position().distanceTo(before), 0.f, "Both vertical keys must stop climbing");
			world.pauseSimulation(); world.resumeSimulation(); world.update(.1f);
			near(position().distanceTo(before), 0.f, "Pause/resume must preserve exact Ladder position and clear keys");
			drive(0, -vertical, .1f);
			near(position().distanceTo(before), agent->getClimbSpeed() * .1f, "Opposite vertical input must reverse the climb");
			drive(0, -vertical, 3.f);
			near(position().y, descending ? 2.f : 0.f, "Returning to the source must dismount safely");
			require(agent->getSector()->getIndex() == (descending ? top : bottom) && !agent->hasActiveLocomotionTask(),
				"Source return must release Ladder admission and restore the landing");
			drive(approach, vertical, 12.f);
			near(position().y, descending ? 0.f : 2.f, "A complete climb must reach the other endpoint");
			require(agent->getSector()->getIndex() == (descending ? bottom : top), "Arrival must automatically dismount into the landing");
			world.resetSimulation(); world.resumeSimulation(); world.update(.1f);
			near(position().y, descending ? 2.f : 0.f, "Reset must restore the authored landing");
			near(position().x, 4.5f - approach * .15f, "Reset must discard mounted state and held keys");
		}
	}

	void ladderRestrictions()
	{
		for (int scenario = 0; scenario < 9; ++scenario)
		{
			core::World world("Ladder restrictions", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			world.addCorridor(1, 0, 12);
			auto result = world.addLadder(1, 0, 4, {2, scenario == 5, scenario != 5});
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, scenario == 0 ? 3.5f : 4.35f); assign(world, id);
			if (scenario == 6)
			{
				auto mobility = world.lookupAgent(id).entity->getEffectiveMobilityProfile().value;
				mobility.set(core::TraversalKind::Ladder, core::MobilityUse::CannotUse);
				world.setAgentIndividualMobilityProfile(id, mobility);
			}
			if (scenario == 7) world.setRoomHeightScale(bottom, .4f);
			if (scenario == 8) world.setTraversalResourceEnabled(result.traversalResource, false);
			world.resumeSimulation(); world.setPlayerMovementDirection(scenario == 1 ? 0 : scenario == 2 ? -1 : 1);
			world.setPlayerStairSelection(scenario != 3, scenario == 3 || scenario == 4);
			world.update(scenario == 0 ? .1f : .6f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom,
				"Far, stopped, away, wrong/ambiguous, retracted, forbidden, low and disabled mounts must refuse entry");
		}
		for (bool low : {false, true})
		{
			core::World world("Ladder exit authorization", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			auto top = world.addRoom("Top", 0, 1, 0, 12, 1);
			world.addLadder(1, 0, 4, {2, false, true});
			world.finishBuild(); world.pauseSimulation();
			if (low) world.setRoomHeightScale(top, .4f);
			else
			{
				auto permission = world.addAccessPermission("Upper landing");
				world.setLocationPermissionRequirement(top, {permission});
			}
			auto id = world.createAgent("Player", bottom, 0, 4.35f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			for (int second = 0; second < 8; ++second) world.update(1.f);
			auto agent = world.lookupAgent(id).entity;
			require(agent->getSector()->getType() == core::SectorType::Ladder, "Refused Ladder exit must stay on the Ladder");
			near(agent->getGlobalPosition().y, 1.f, "Refused exit must remain at the supported endpoint");
			world.setPlayerMovementDirection(0); world.setPlayerStairSelection(false, true); world.update(.1f);
			require(agent->getGlobalPosition().y < 1.f, "Refused exit must permit retracing the Ladder");
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(agent->getSector()->getIndex() == bottom, "Unauthorized destination must not trap the Player");
		}
		for (bool nearMount : {false, true})
		{
			core::World world("Ladder versus Door", 12, 2); world.addLayer();
			auto fore = world.addRoom("Fore", 0, 0, 0, 12, 1);
			auto bottom = world.addRoom("Bottom", 1, 0, 0, 12, 1);
			world.addCorridor(1, 1, 0, 12, 1);
			world.addLadder(2, 0, 4, {2, false, true});
			core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
			auto result = world.addSectorDoor(fore, 0, 4, options);
			auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(result.door.sector->getObject(result.door.index))->getDoor();
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, nearMount ? 4.35f : 4.15f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			world.requestPlayerDoorAction(); world.update(.1f);
			require(door->isClosed() == nearMount, "Only an eligible nearby Ladder approach may suppress Up's Door action");
		}
	}

	void ladderSharedCapacity()
	{
		core::World world("Player Ladder sharing", 12, 3);
		auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
		auto top = world.addCorridor(2, 0, 12);
		world.addSectorMarker(top, 0, 4.5f);
		world.addLadder(1, 0, 4, {3, false, true});
		world.finishBuild(); world.pauseSimulation();
		auto id = world.createAgent("Player", bottom, 0, 4.35f); assign(world, id);
		auto otherId = world.createAgent("Other climber", bottom, 0, 4.5f);
		world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
		for (int second = 0; second < 4; ++second) world.update(1.f);
		auto player = world.lookupAgent(id).entity;
		require(player->getGlobalPosition().y > .8f && player->getGlobalPosition().y < 2.f,
			"Player must clear entry spacing before admitting another climber");
		world.setPlayerStairSelection(false, false);
		auto other = world.lookupAgent(otherId).entity;
		require(world.moveAgentToMarker(otherId, world.getMarkerIds().front()).accepted(), "Cannot route other climber");
		for (int step = 0; step < 80 && other->getSector()->getType() != core::SectorType::Ladder; ++step)
			world.update(.1f);
		require(other->getSector()->getType() == core::SectorType::Ladder,
			"A same-direction climber must be admitted after entry spacing clears");
		auto before = player->getGlobalPosition();
		world.setPlayerStairSelection(false, true); world.update(.2f);
		near(player->getGlobalPosition().distanceTo(before), 0.f, "Reversal must be refused while another climber shares the Ladder");
		world.setPlayerStairSelection(false, false);
		for (int second = 0; second < 15; ++second) world.update(1.f);
		require(other->getSector()->getIndex() == top, "Other climber must finish through ordinary traversal admission");
		world.setPlayerStairSelection(false, true); world.update(.2f);
		require(player->getGlobalPosition().y < before.y, "Reversal must resume once the Player is alone");
		for (int second = 0; second < 8; ++second) world.update(1.f);
		require(player->getSector()->getIndex() == bottom, "Sole climber reversal must safely dismount");
	}

	void escalators()
	{
		for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
		for (float belt : {-.1f, .1f})
		for (bool descending : {false, true})
		{
			core::World world("Player Escalator", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			auto top = world.addCorridor(1, 0, 12);
			auto stairs = world.addStaircase(1, 0, 4, {3, side, belt});
			int const ascent = side == CORE_SIDE_RIGHT ? 1 : -1;
			int const horizontal = descending ? -ascent : ascent;
			float const mountX = descending ? (ascent > 0 ? 7.f : 4.f) : (ascent > 0 ? 4.f : 7.f);
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", descending ? top : bottom, 0, mountX - horizontal * .15f);
			assign(world, id);
			world.setAgentIndividualStairSpeedModifier(id, 1.2f);
			auto normalId = world.createAgent("Autonomous", bottom, 0, 1.f);
			auto normal = world.lookupAgent(normalId).entity;
			auto player = world.lookupAgent(id).entity;
			for (auto const& edge : world.getGraph()->getEdges())
				if (edge->getType() == core::EdgeType::Staircase && edge->getVertex(0)->getSector()->getIndex() == stairs)
				{
					auto wrong = edge->getVertex(0);
					if ((wrong->getPosition().y > edge->getVertex(1)->getPosition().y) == (belt > 0.f))
						wrong = edge->getVertex(1);
					require(!edge->isTraversable(wrong, std::shared_ptr<const core::Agent>(normal, [](auto*) {})),
						"Autonomous traversal must remain one-way on a moving Escalator");
					require(edge->isTraversable(wrong, std::shared_ptr<const core::Agent>(player, [](auto*) {})),
						"Player traversal must admit the wrong direction");
					core::RouteDecisionContext context{player, {}, {}, player->getSector()};
					require(!edge->getDirectedTraversalFacts(wrong, context).feasible,
						"Direct Player input must not change autonomous pathfinding direction facts");
				}
			auto position = [&] { return world.lookupAgent(id).entity->getGlobalPosition(); };
			auto sector = [&] { return world.lookupAgent(id).entity->getSector()->getIndex(); };
			auto drive = [&](int direction, float seconds) {
				world.setPlayerMovementDirection(direction);
				while (seconds > 0.f) { auto step = std::min(seconds, 1.f); world.update(step); seconds -= step; }
			};
			world.resumeSimulation(); world.setPlayerStairSelection(!descending, descending);
			drive(horizontal, .6f);
			require(sector() == stairs && position().y > 0.f && position().y < 1.f,
				"Players must board either Escalator endpoint, including against its belt");
			world.setPlayerStairSelection(false, false);
			auto before = position();
			drive(horizontal, .1f);
			near(position().distanceTo(before), std::abs(belt + horizontal * ascent
				* world.lookupAgent(id).entity->getWalkSpeed()) * .1f,
				"Moving Escalator speed must combine signed walking and belt speeds");
			before = position(); drive(0, .2f);
			near(position().distanceTo(before), std::abs(belt) * .2f, "Release must ride the belt, not stop");
			require((position().y > before.y) == (belt > 0.f), "Belt must carry in its authored direction");
			before = position(); world.pauseSimulation(); world.update(.2f);
			near(position().distanceTo(before), 0.f, "Pause must freeze Escalator passengers");
			world.resumeSimulation(); world.update(.1f);
			near(position().distanceTo(before), std::abs(belt) * .1f, "Resume with cleared keys must ride");
			require(world.setEscalatorBroken(stairs, true), "Cannot break Player Escalator");
			before = position(); drive(0, .2f);
			near(position().distanceTo(before), 0.f, "Broken Escalator must stop on release");
			before = position(); drive(horizontal, .1f);
			near(position().distanceTo(before), world.lookupAgent(id).entity->getStationaryStairSpeed(!descending) * .1f,
				"Broken Escalator must use effective directional stationary stair speed");
			require(world.setEscalatorBroken(stairs, false), "Cannot restore Player Escalator");
			before = position(); drive(-horizontal, .1f);
			near(position().distanceTo(before), std::abs(belt - horizontal * ascent
				* world.lookupAgent(id).entity->getWalkSpeed()) * .1f,
				"Reversal after restore must retain position and use signed belt motion");
			drive(horizontal, 12.f);
			require(sector() == (descending ? bottom : top), "Players must finish either Escalator direction and dismount");
		}
		// Against a sufficiently fast belt, walking can hold position or lose
		// ground. Exercise both outcomes from an interior admitted flight.
		for (float factor : {1.f, 2.f})
		{
			core::World world("Player loses to belt", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			auto top = world.addCorridor(1, 0, 12);
			float const belt = core::bundledHumanBaseline().walkSpeed * factor;
			auto stairs = world.addStaircase(1, 0, 4, {3, CORE_SIDE_RIGHT, belt});
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, 3.85f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			world.update(.8f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs, "Cannot board slow belt fixture");
			auto agent = world.lookupAgent(id).entity;
			auto const targetSpeed = agent->getWalkSpeed();
			auto before = agent->getGlobalPosition();
			world.setPlayerStairSelection(false, false); world.setPlayerMovementDirection(-1); world.update(.2f);
			near(agent->getGlobalPosition().distanceTo(before), (belt - targetSpeed) * .2f,
				"Opposing input must stall or be carried backwards according to net speed");
			world.setPlayerMovementDirection(0);
			for (int second = 0; second < 40; ++second) world.update(1.f);
			require(agent->getSector()->getIndex() == top, "Released Player must ride to an automatic endpoint exit");
		}
	}

	void staircaseRestrictions()
	{
		for (int scenario = 0; scenario < 12; ++scenario)
		{
			core::World world("Staircase approach restrictions", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			world.addCorridor(1, 0, 12);
			auto stairs = world.addStaircase(1, 0, 4, {3, CORE_SIDE_RIGHT, scenario == 6 || scenario >= 9 ? .1f : 0.f});
			world.finishBuild(); world.pauseSimulation();
			if (scenario >= 10) world.setEscalatorInitiallyBroken(stairs, true);
			auto id = world.createAgent("Player", bottom, 0, scenario == 0 ? 3.f : 3.85f); assign(world, id);
			if (scenario == 5 || scenario == 6 || scenario >= 9)
			{
				auto profile = world.lookupAgent(id).entity->getEffectiveMobilityProfile().value;
				profile.set(scenario == 6 || scenario == 10 ? core::TraversalKind::Escalator : core::TraversalKind::Staircase,
					core::MobilityUse::CannotUse);
				world.setAgentIndividualMobilityProfile(id, profile);
			}
			if (scenario == 7) world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
			if (scenario == 8) world.setRoomHeightScale(bottom, .4f);
			world.resumeSimulation();
			world.setPlayerStairSelection(scenario != 3, scenario == 3 || scenario == 4);
			world.setPlayerMovementDirection(scenario == 1 ? 0 : scenario == 2 ? -1 : 1);
			world.update(scenario == 0 ? .1f : .6f);
			require((world.lookupAgent(id).entity->getSector()->getIndex() != bottom) == (scenario == 7 || scenario == 9 || scenario == 10),
				"Only near, toward-flight, correct exclusive selection may mount; None must still permit stairs");
		}
		for (float belt : {0.f, .1f})
		{
			core::World world("Staircase exit authorization", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, 1);
			auto top = world.addCorridor(1, 0, 12);
			auto stairs = world.addStaircase(1, 0, 4, {3, CORE_SIDE_RIGHT, belt});
			world.finishBuild(); world.pauseSimulation();
			auto permission = world.addAccessPermission("Upper access");
			world.setLocationPermissionRequirement(top, {permission});
			auto id = world.createAgent("Player", bottom, 0, 3.85f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			for (int second = 0; second < 12; ++second) world.update(1.f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs,
				"Automatic dismount must respect destination Location permissions");
			near(world.lookupAgent(id).entity->getGlobalPosition().y, 1.f, "Refused exit must remain on the endpoint");
			world.setPlayerMovementDirection(-1); world.update(.2f);
			require(world.lookupAgent(id).entity->getGlobalPosition().y < 1.f,
				"A refused exit must allow reversing back down the stairs");
			world.pauseSimulation(); world.grantAgentAccessPermission(id, permission);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.update(1.f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == top,
				"Granted permission must allow automatic upper dismount: y="
				+ std::to_string(world.lookupAgent(id).entity->getGlobalPosition().y));
		}
		for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
		{
			bool const right = side == CORE_SIDE_RIGHT;
			core::World world("Room wall Staircase", 12, 2);
			auto room = world.addRoom("Stair Room", 0, 0, right ? 0 : 4, right ? 7 : 8, 2);
			auto upper = world.addRoom("Upper Floor", 0, 1, right ? 7 : 0, right ? 5 : 4, 1);
			world.removeLocationWall(room, 1, side);
			auto stairs = world.addStaircase(1, 0, 4, {3, side, 0.f});
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", upper, 0, right ? .15f : 3.85f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(right ? -1 : 1);
			world.setPlayerStairSelection(false, true); world.update(.6f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == stairs,
				"Open Room-wall endpoint must mount from its adjacent upper Floor");
			for (int second = 0; second < 12; ++second) world.update(1.f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == room,
				"Room-wall descent must automatically dismount onto the lower Floor");
			world.setPlayerMovementDirection(right ? 1 : -1); world.setPlayerStairSelection(true, false);
			for (int second = 0; second < 25; ++second) world.update(1.f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == upper,
				"Room-wall ascent must dismount directly onto the supported adjacent upper Floor: x="
				+ std::to_string(x(world, id)) + " y=" + std::to_string(world.lookupAgent(id).entity->getGlobalPosition().y)
				+ " sector=" + std::to_string(world.lookupAgent(id).entity->getSector()->getIndex()));
		}
		for (bool nearAndToward : {false, true})
		{
			core::World world("Staircase versus Door", 12, 2); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 12, 1);
			auto bottom = world.addRoom("Bottom", 1, 0, 0, 12, 1);
			world.addCorridor(1, 1, 0, 12, 1);
			world.addStaircase(2, 0, 4, {3, CORE_SIDE_RIGHT, 0.f});
			core::World::CreateDoorOptions options; options.activationMode = core::DoorActivationMode::Manual;
			auto result = world.addSectorDoor(0, 0, 3, options);
			auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(result.door.sector->getObject(result.door.index))->getDoor();
			world.finishBuild(); world.pauseSimulation();
			auto id = world.createAgent("Player", bottom, 0, nearAndToward ? 3.85f : 3.5f); assign(world, id);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			world.requestPlayerDoorAction(); world.update(.1f);
			require(door->isClosed() == nearAndToward, "Only a near toward-flight stair selection may suppress Up's Door action");
		}
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
	registerPlayerButtons(checks);
	checks.push_back({"playerControlBasics", [](smoke::Context const&) { basics(); }});
	checks.push_back({"playerControlBoundaries", [](smoke::Context const&) { boundaries(); }});
	checks.push_back({"playerControlBulkheads", [](smoke::Context const&) { bulkheads(); }});
	checks.push_back({"playerControlDoorways", [](smoke::Context const&) { doorways(); }});
	checks.push_back({"playerControlDoorwayRestrictions", [](smoke::Context const&) { doorwayRestrictions(); doorwaySelection(); }});
	checks.push_back({"playerControlStairwellPlatforms", [](smoke::Context const&) { stairwellPlatforms(); }});
	checks.push_back({"playerControlLadders", [](smoke::Context const&) { ladders(); }});
	checks.push_back({"playerControlLadderSharedCapacity", [](smoke::Context const&) { ladderSharedCapacity(); }});
	checks.push_back({"playerControlLadderRestrictions", [](smoke::Context const&) { ladderRestrictions(); }});
	checks.push_back({"playerControlEscalators", [](smoke::Context const&) { escalators(); }});
	checks.push_back({"playerControlStaircases", [](smoke::Context const&) { staircases(); }});
	checks.push_back({"playerControlStaircaseRestrictions", [](smoke::Context const&) { staircaseRestrictions(); }});
	checks.push_back({"playerControlStairwells", [](smoke::Context const&) { stairwells(); }});
	checks.push_back({"playerControlStairwellRestrictions", [](smoke::Context const&) { stairwellRestrictions(); }});
	checks.push_back({"playerControlPersistence", [](smoke::Context const&) { persistenceAndPermissions(); }});
}
