#include "Checks.h"
#include "core/World.h"
#include "core/Button.h"
#include "core/DoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LadderTransit.h"
#include "core/MobilityProfile.h"
#include <algorithm>
#include <cmath>

namespace
{
	using smoke::require;
	core::AgentId player(core::World& world, uint32_t sector, uint32_t level, float x)
	{
		auto id = world.createAgent("Player", sector, level, x - world.getSector(sector)->getPosition().x);
		require(world.setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}), "Cannot assign Player");
		return id;
	}
	void press(core::World& world)
	{
		if (world.isTraversalTopologyDirty()) require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
		require(world.resumeSimulation(), "Cannot resume Button fixture");
		world.setPlayerStairSelection(true, false);
		world.requestPlayerDoorAction(); world.update(.1f);
	}
	std::vector<core::InteractionPointId> requests(core::World& world)
	{
		std::vector<core::InteractionPointId> points;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::InteractionRequestAdded)
				points.push_back(event.interactionRequest.point);
		return points;
	}
	void buttons()
	{
		for (int scenario = 0; scenario < 13; ++scenario)
		{
			core::World world("Player physical Button", 12, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 2);
			auto other = world.addRoom("Other Layer", 1, 0, 0, 12, 1);
			world.addSectorWalkway(room, 1, 4);
			auto control = world.addSectorLightSwitch(room, 4);
			auto point = world.lookupInteractionPoint(control.interactionPoint).entity;
			float const start = point->getPosition().x - (scenario == 12 ? .1875f : .125f);
			world.finishBuild(); world.pauseSimulation();
			auto id = player(world, scenario == 8 ? other : room, scenario == 7 ? 1 : 0, start);
			if (scenario == 1) world.setAgentIndividualObjectUsageDistance(id, .0625f);
			if (scenario == 2) world.setAgentIndividualObjectUsageDistance(id, .125f);
			if (scenario == 3) world.setAgentIndividualObjectUsage(id, core::ObjectUsage::None);
			if (scenario == 4) world.setAgentIndividualObjectUsage(id, core::ObjectUsage::RemoteControl);
			if (scenario == 12) world.setAgentIndividualObjectUsageDistance(id, 1.f);
			if (scenario == 5 || scenario == 6)
			{
				auto profile = world.lookupAgent(id).entity->getEffectiveMobilityProfile().value;
				profile.set(core::TraversalKind::Buttons, scenario == 5 ? core::MobilityUse::CannotUse : core::MobilityUse::OnlyIfNoOtherOption);
				world.setAgentIndividualMobilityProfile(id, profile);
			}
			if (scenario == 9) world.setAgentActive(id, false);
			core::AccessPermissionId permission;
			if (scenario == 10 || scenario == 11)
			{
				permission = world.addAccessPermission("Operate switch");
				require(world.setInteractionPointPermissionRequirement(control.interactionPoint, {permission}), "Cannot protect Button");
				world.setAgentIndividualPermissionAdherence(id, false);
				if (scenario == 11) world.grantAgentAccessPermission(id, permission);
			}
			auto const initial = world.getSector(room)->areLightsOn();
			world.consumeSimulationEvents(); press(world);
			bool const accepted = scenario == 0 || scenario == 2 || scenario == 6 || scenario == 11;
			require((world.getSector(room)->areLightsOn() != initial) == accepted,
				"Up must use effective arm length, geometry, Layer/Level, activation, capability, Buttons Mobility and permissions");
			require(requests(world).size() == size_t(accepted), "A press must select exactly one eligible physical Button");
			require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().x - start) < .0001f, "Button interaction must not auto-approach or align");
			world.update(1.f);
			require(requests(world).empty(), "Held Up must not repeat a Button action after cooldown");
			world.pauseSimulation(); world.requestPlayerDoorAction(); world.resumeSimulation(); world.update(.1f);
			require(requests(world).empty(), "Paused Up must not queue a later Button press");
		}
		// An accepted press must not chase the control if Player movement leaves
		// reach before its physical press phase, even within the same tick.
		core::World world("Player leaves Button reach", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
		auto control = world.addSectorLightSwitch(room, 4);
		auto start = world.lookupInteractionPoint(control.interactionPoint).entity->getPosition().x - .125f;
		world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 0, start);
		world.setAgentIndividualObjectUsageDistance(id, .125f);
		auto initial = world.getSector(room)->areLightsOn();
		world.resumeSimulation(); world.setPlayerMovementDirection(-1); world.requestPlayerDoorAction();
		world.update(core::World::getFixedTimestep());
		require(world.getSector(room)->areLightsOn() == initial, "Leaving reach must cancel unpressed work");
		require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().x - start
			+ world.lookupAgent(id).entity->getWalkSpeed() * core::World::getFixedTimestep()) < .0001f,
			"Cancelled interaction must not pull the Player back toward the Button");
		world.setPlayerMovementDirection(0); world.update(1.f);
		require(world.getSector(room)->areLightsOn() == initial, "Cancelled press must not execute later");
	}
	void queuedPress()
	{
		core::World world("Queued Player Button", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
		auto control = world.addSectorLightSwitch(room, 4);
		auto start = world.lookupInteractionPoint(control.interactionPoint).entity->getPosition().x - .125f;
		world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 0, start);
		auto other = world.createAgent("Other operator", room, 0, 1.f);
		world.resumeSimulation(); require(bool(world.requestInteraction(control.interactionPoint, other)), "Cannot queue other operator");
		world.consumeSimulationEvents(); press(world);
		require(requests(world).size() == 1, "Player must queue only one reachable press behind an existing operator");
		world.setPlayerMovementDirection(-1);
		for (int second = 0; second < 2; ++second) world.update(1.f);
		world.setPlayerMovementDirection(0);
		auto before = world.lookupAgent(id).entity->getGlobalPosition();
		for (int second = 0; second < 15; ++second) world.update(1.f);
		require(world.lookupAgent(id).entity->getGlobalPosition().distanceTo(before) < .0001f, "Queued work must not auto-approach after the Player walks away");
		bool cancelled = false;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::InteractionRequestChanged && event.interactionRequest.actor == id
				&& event.interactionRequest.result == core::InteractionResult::Cancelled) cancelled = true;
		require(cancelled, "A queued Player press must revalidate reach before allocation");
	}
	void priority()
	{
		for (bool broken : {false, true})
		{
			core::World world("Door before Button", 12, 2);
			auto front = world.addRoom("Front", 0, 0, 0, 12, 1);
			auto back = world.addRoom("Back", 1, 0, 0, 12, 1);
			core::World::CreateDoorOptions options; options.width = 2;
			options.activationMode = core::DoorActivationMode::Manual; options.initiallyBroken = broken;
			auto doorResult = world.addSectorDoor(front, 0, 4, options);
			auto control = world.addSectorLightSwitch(front, 4);
			auto position = world.lookupInteractionPoint(control.interactionPoint).entity->getPosition();
			auto door = std::dynamic_pointer_cast<core::DoorSectorObject>(doorResult.door.sector->getObject(doorResult.door.index))->getDoor();
			world.finishBuild(); world.pauseSimulation(); auto id = player(world, front, 0, position.x);
			auto initial = world.getSector(front)->areLightsOn(); world.consumeSimulationEvents(); press(world);
			require((world.getSector(front)->areLightsOn() != initial) == broken, "Manual Door opening must precede an eligible Button; an inoperable Door must not consume Up");
			require(requests(world).size() == size_t(broken), "Door opening and Button pressing must never share one Up press");
			if (!broken)
			{
				for (int second = 0; second < 3; ++second) world.update(1.f);
				require(door->isOpen(), "Priority Door must open");
				press(world); world.update(1.f);
				require(world.lookupAgent(id).entity->getSector()->getIndex() == back, "Open Door crossing must precede an eligible Button");
				require(world.getSector(front)->areLightsOn() == initial && requests(world).empty(), "Crossing must not press the overlapping Button");
			}
		}
		core::World world("Climbing before Button", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 4, 8, 2);
		world.addSectorWalkway(room, 1, 0); world.addSectorLadder(room, 0, 0, {2, false, true});
		auto control = world.addSectorLightSwitch(room, 0);
		auto buttonX = world.lookupInteractionPoint(control.interactionPoint).entity->getPosition().x;
		float const start = buttonX + (buttonX < 4.5f ? 1.f : -1.f) * .125f;
		int const direction = start < 4.5f ? 1 : -1;
		world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 0, start);
		require(std::abs(start - 4.5f) <= .25f, "Fixture must offer both climbing and Button pressing");
		auto initial = world.getSector(room)->areLightsOn(); world.consumeSimulationEvents(); world.resumeSimulation();
		world.setPlayerMovementDirection(direction); press(world);
		require(world.getSector(room)->areLightsOn() == initial && requests(world).empty(), "Eligible climb selection must take precedence over a reachable Button: buttonX=" + std::to_string(buttonX)
			+ " actorX=" + std::to_string(world.lookupAgent(id).entity->getGlobalPosition().x));
	}
	void stackedButtons()
	{
		for (bool protectFirst : {false, true})
		{
			core::World world("Player Button stack", 6, 1); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 6, 1);
			auto middle = world.addRoom("Middle", 1, 0, 1, 2, 1);
			world.addRoom("Back", 2, 0, 0, 6, 1);
			auto first = world.addSectorDoor(0, 0, 2, core::World::RemoteControlledDoor1Options);
			auto second = world.addSectorDoor(1, 0, 2, core::World::RemoteControlledDoor1Options);
			auto a = first.controls[1].interactionPoint, b = second.controls[0].interactionPoint;
			world.finishBuild(); world.pauseSimulation(); auto id = player(world, middle, 0, 2.f);
			if (protectFirst)
			{
				auto permission = world.addAccessPermission("First control");
				world.setInteractionPointPermissionRequirement(a, {permission});
			}
			require(world.lookupInteractionPoint(a).entity->getPosition() == world.lookupInteractionPoint(b).entity->getPosition(), "Fixture must have a real shared approach stack");
			require(world.lookupAgent(id).entity->getGlobalPosition() == world.lookupInteractionPoint(a).entity->getPosition(), "Stack Player must stand at the actual approach");
			require(world.lookupInteractionPoint(a).entity->getSector().value == uint64_t(middle) + 1, "Stack control must belong to the Player's Location");
			require(std::dynamic_pointer_cast<core::Button>(first.controls[1].sector->getObject(first.controls[1].index)->_getObject())->isEnabled(), "Stack control must be enabled");
			world.consumeSimulationEvents(); press(world);
			auto pressed = requests(world);
			require(pressed == std::vector<core::InteractionPointId>{protectFirst ? b : std::min(a, b)},
				"Equidistant Buttons must choose one permission-eligible control by stable identity: protected="
				+ std::to_string(protectFirst) + " first=" + std::to_string(a.value) + " second=" + std::to_string(b.value)
				+ " count=" + std::to_string(pressed.size()) + " actual=" + std::to_string(pressed.empty() ? 0 : pressed[0].value));
		}
	}
	void bridgeButton()
	{
		for (bool broken : {false, true})
		{
			core::World world("Player Force Bridge Button", 12, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 2);
			world.addSectorWalkway(room, 1, 3); world.addSectorWalkway(room, 1, 6);
			core::World::CreateForceBridgeOptions options; options.width = 2; options.startExtended = false;
			options.controlCount = 2; options.initiallyBroken = broken;
			auto result = world.addSectorForceBridge(room, 1, 4, options);
			auto bridge = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(result.forceBridge.sector->getObject(result.forceBridge.index))->getForceBridge();
			auto point = world.lookupInteractionPoint(result.controls[0].interactionPoint).entity;
			world.finishBuild(); world.pauseSimulation(); auto id = player(world, room, 1, point->getPosition().x);
			auto before = world.lookupAgent(id).entity->getGlobalPosition();
			world.consumeSimulationEvents(); press(world);
			require(requests(world).size() == size_t(!broken), "A reachable Broken-device Button must be refused");
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(bridge->isExtended() == !broken, "Button must operate the Force Bridge through its existing device operation");
			require(world.lookupAgent(id).entity->getGlobalPosition().distanceTo(before) < .0001f, "Bridge operation must not move its operator");
			if (!broken)
			{
				world.consumeSimulationEvents(); press(world);
				require(requests(world).size() == 1, "Second Bridge press must enqueue a new activation");
				for (int second = 0; second < 8; ++second) world.update(1.f);
				require(bridge->isRetracted(), "A second Force Bridge Button press must retract");
				press(world);
				for (int second = 0; second < 8; ++second) world.update(1.f);
				require(bridge->isExtended(), "A third Force Bridge Button press must extend again");
				world.setPlayerStairSelection(false, false); world.setPlayerMovementDirection(1);
				world.update(3.f);
				require(std::abs(world.lookupAgent(id).entity->getGlobalPosition().x - before.x
					- world.lookupAgent(id).entity->getWalkSpeed() * 3.f) < .0001f, "An extended Force Bridge must admit existing Player Floor movement");
			}
		}
	}
	void bridgeSafeRetraction()
	{
		core::World world("Manual Force Bridge safe retraction", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 2);
		world.addSectorWalkway(room, 1, 3); world.addSectorWalkway(room, 1, 6);
		core::World::CreateForceBridgeOptions options; options.width = 2;
		options.startExtended = true; options.controlCount = 2;
		auto result = world.addSectorForceBridge(room, 1, 4, options);
		auto bridge = std::dynamic_pointer_cast<core::ForceBridgeSectorObject>(result.forceBridge.sector->getObject(result.forceBridge.index))->getForceBridge();
		auto control = result.controls[0].interactionPoint;
		world.finishBuild(); world.pauseSimulation();
		auto id = player(world, room, 1, 5.f);
		auto operatorId = world.createAgent("Retractor", room, 1, world.lookupInteractionPoint(control).entity->getPosition().x);
		world.resumeSimulation();
		require(bool(world.requestInteraction(control, operatorId)), "Cannot press Bridge toggle");
		for (int second = 0; second < 8; ++second) world.update(1.f);
		require(bridge->isExtended(), "Retraction must retain support under a stopped manual Floor occupant");
		auto snapshot = world.getSimulationSnapshot();
		auto resource = std::find_if(snapshot.traversalResources.begin(), snapshot.traversalResources.end(),
			[&](auto const& value) { return value.id == result.traversalResource; });
		require(resource != snapshot.traversalResources.end() && !resource->enabled, "Pending retract must close Bridge admission");
		world.setPlayerMovementDirection(1);
		for (int second = 0; second < 3; ++second) world.update(1.f);
		world.setPlayerMovementDirection(0);
		require(world.lookupAgent(id).entity->getGlobalPosition().x > 6.f, "Existing occupant must be able to leave during pending retraction");
		for (int second = 0; second < 8; ++second) world.update(1.f);
		require(bridge->isRetracted(), "Bridge must retract only after its manual occupant clears the span");
	}
	void safeRetraction()
	{
		for (bool embedded : {false, true})
		{
			core::World world("Player Ladder safe retraction", 12, 2);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, embedded ? 2 : 1);
			if (embedded) world.addSectorWalkway(bottom, 1, 4);
			else world.addCorridor(1, 0, 12);
			auto result = embedded ? world.addSectorLadder(bottom, 0, 4, {2, true, true}) : world.addLadder(1, 0, 4, {2, true, true});
			auto ladder = embedded ? std::dynamic_pointer_cast<core::LadderSectorObject>(result.ladder.sector->getObject(result.ladder.index))->getLadder()
				: std::dynamic_pointer_cast<core::LadderTransit>(result.ladder.sector)->getLadder();
			world.finishBuild(); world.pauseSimulation(); auto id = player(world, bottom, 0, 4.35f);
			auto control = result.controls[0].interactionPoint;
			auto operatorId = world.createAgent("Retractor", bottom, 0,
				world.lookupInteractionPoint(control).entity->getPosition().x);
			world.resumeSimulation(); world.setPlayerMovementDirection(1); world.setPlayerStairSelection(true, false);
			for (int step = 0; step < 60 && world.lookupAgent(id).entity->getGlobalPosition().y < .4f; ++step) world.update(.1f);
			require(world.lookupAgent(id).entity->getGlobalPosition().y >= .4f, "Player must mount an already extended Ladder");
			world.setPlayerMovementDirection(0); world.setPlayerStairSelection(false, false);
			require(bool(world.requestInteraction(control, operatorId)), "Cannot request safe retraction"); world.update(.2f);
			require(ladder->isExtended(), "A stopped admitted Player must retain extension leases during safe retraction");
			world.setPlayerStairSelection(false, true);
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(world.lookupAgent(id).entity->getSector()->getIndex() == bottom
				&& world.lookupAgent(id).entity->getGlobalPosition().y == 0.f && ladder->isRetracted(),
				"Safe retracing must dismount and release leases before Ladder retraction");
		}
	}
	void extensibleLadders()
	{
		for (bool embedded : {false, true})
		for (bool descending : {false, true})
		{
			core::World world("Player extends Ladder", 12, 3);
			auto bottom = world.addRoom("Bottom", 0, 0, 0, 12, embedded ? 3 : 1);
			auto top = embedded ? bottom : world.addCorridor(2, 0, 12);
			if (embedded) world.addSectorWalkway(bottom, 2, 4);
			auto result = embedded ? world.addSectorLadder(bottom, 0, 4, {3, true, false}) : world.addLadder(1, 0, 4, {3, true, false});
			auto ladder = embedded ? std::dynamic_pointer_cast<core::LadderSectorObject>(result.ladder.sector->getObject(result.ladder.index))->getLadder()
				: std::dynamic_pointer_cast<core::LadderTransit>(result.ladder.sector)->getLadder();
			auto point = world.lookupInteractionPoint(result.controls[descending ? 1 : 0].interactionPoint).entity;
			world.finishBuild(); world.pauseSimulation();
			auto id = player(world, descending ? top : bottom, embedded && descending ? 2 : 0, point->getPosition().x);
			auto initialPosition = world.lookupAgent(id).entity->getGlobalPosition();
			world.consumeSimulationEvents(); press(world);
			require(requests(world).size() == 1 && !ladder->isRetracted(), "Up must operate the endpoint's Ladder extension Button");
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(ladder->isExtended(), "The existing device operation must complete Ladder extension");
			world.consumeSimulationEvents(); press(world);
			require(requests(world).size() == 1, "Second Ladder press must enqueue a new activation");
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(ladder->isRetracted(), "Either endpoint Button must retract an unused Ladder");
			press(world);
			for (int second = 0; second < 8; ++second) world.update(1.f);
			require(ladder->isExtended(), "Either endpoint Button must extend the Ladder again");
			require(world.lookupAgent(id).entity->getGlobalPosition().distanceTo(initialPosition) < .0001f, "Extending must not approach or mount the Ladder automatically");
			world.setPlayerMovementDirection(initialPosition.x < 4.5f ? 1 : -1);
			world.setPlayerStairSelection(!descending, descending);
			for (int second = 0; second < 16; ++second) world.update(1.f);
			auto actor = world.lookupAgent(id).entity;
			require(actor->getSector()->getIndex() == (descending ? bottom : top)
				&& std::abs(actor->getGlobalPosition().y - (descending ? 0.f : 2.f)) < .0001f,
				"A fully extended Ladder must use existing Player mounting, climbing and endpoint dismount");
		}
	}
}

namespace agent_smoke
{
	void registerPlayerButtons(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"playerControlButtons", [](smoke::Context const&) { buttons(); queuedPress(); bridgeButton(); bridgeSafeRetraction(); }});
		checks.push_back({"playerControlButtonPriority", [](smoke::Context const&) { priority(); stackedButtons(); }});
		checks.push_back({"playerControlExtensibleLadders", [](smoke::Context const&) { extensibleLadders(); safeRetraction(); }});
	}
}
