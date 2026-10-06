// Agent behaviour callbacks runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/YamlSerializer.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	void programmingErrorDisablesMovementOwnership(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "programming-error.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "duplicate.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return { on_start = function(context)
      context.move_to(configuration.destination)
      context.cancel_movement()
    end }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Duplicate", "duplicate.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		writeRuntimeText(package / "moving.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return { on_start = function(context)
      local result = context.move_to(configuration.destination)
      if not result.accepted then error(result.status) end
    end }
  end
}
)lua");
		auto const movingBehaviour = registry->addAgentBehaviour("Moving", "moving.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		core::World world("Programming error", 8, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.addSectorMarker(room, 0, 6.5f, "Destination");
		world.addSectorMarker(room, 0, 0.5f, "Return");
		world.finishBuild();
		auto const markers = world.getMarkerIds();
		auto const marker = markers[0];
		auto const id = world.createAgent("Walker", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("programming-error.behaviours", registry);
		require(world.setAgentBehaviourAssignment(id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", marker } }), "Could not assign duplicate-command fixture");
		require(world.resumeSimulation(), "Could not start duplicate-command fixture");
		world.consumeSimulationEvents();
		world.advanceTick();
		require(!world.agentBehaviourOwnsMovement(id)
			&& !world.lookupAgent(id).entity->getPath(),
			"A multiple-movement-command callback partially applied or retained ownership");
		require(world.isSimulationPaused(),
			"A callback failure did not pause before the next tick");
		require(world.moveAgentToMarker(id, marker).accepted(),
			"Disabling the failed instance did not restore manual movement controls");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging the callback failure");
		world.advanceTicks(1000);
		unsigned reached = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::DestinationReached) ++reached;
		require(reached == 1, "Manual movement did not work after instance disablement");

		world.pauseSimulation();
		require(world.clearAgentBehaviourAssignment(id),
			"Could not unassign the disabled behaviour");
		require(!world.agentBehaviourOwnsMovement(id),
			"Unassignment left runtime movement ownership behind");

		require(world.setAgentBehaviourAssignment(id, movingBehaviour,
			registry->lookupAgentBehaviour(movingBehaviour)->getRevision(),
			{ { "destination", markers[1] } }),
			"Could not assign the active-unassignment fixture");
		require(world.resumeSimulation(), "Could not start active-unassignment fixture");
		world.advanceTick();
		world.advanceTicks(world.lookupAgent(id).entity->getRoutePlanningRemainingTicks());
		require(world.lookupAgent(id).entity->getPath()
			&& world.agentBehaviourOwnsMovement(id),
			"The active-unassignment fixture did not acquire a route");
		world.pauseSimulation();
		require(world.clearAgentBehaviourAssignment(id),
			"Could not unassign an actively moving behaviour");
		require(!world.agentBehaviourOwnsMovement(id)
			&& !world.lookupAgent(id).entity->getPath(),
			"Active unassignment retained runtime movement ownership");
		require(world.resumeSimulation(), "Could not resume after active unassignment");
		require(world.moveAgentToMarker(id, markers[1]).accepted(),
			"Active unassignment did not restore manual movement commands");
		world.advanceTicks(1000);
		world.pauseSimulation();
		require(world.setAgentBehaviourAssignment(id, movingBehaviour,
			registry->lookupAgentBehaviour(movingBehaviour)->getRevision(),
			{ { "destination", markers[1] } }), "Idle-arrival behaviour assignment refused");
		require(world.resumeSimulation(), "Idle-arrival resume refused");
		world.consumeSimulationEvents();
		world.advanceTicks(1000);
		reached = 0;
		for (auto const& event : world.consumeSimulationEvents())
			if (event.type == core::SimulationEventType::DestinationReached)
			{
				++reached;
				require(event.selectedAction == core::IdleAction, "Behaviour omission did not select Idle");
			}
		require(reached == 1 && world.agentBehaviourOwnsMovement(id)
			&& world.lookupAgent(id).entity->getState() == core::Agent::State::Idle
			&& !world.lookupAgent(id).entity->getPath() && !world.isSimulationPaused(),
			"Idle arrival disabled behaviour or scheduled autonomous work");
	}

	void activationSuspendsStateAndFreezesTimers(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "activation.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "activation.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local starts = 0
    local private_state = "secret_instance_155"
    local deactivations = 0
    local activations = 0
    return {
      on_start = function(context)
        starts = starts + 1
        if starts ~= 1 then error("on_start repeated") end
        context.set_timer("frozen_timer_155", 3)
      end,
      on_event = function(event, context)
        if event.type == "deactivated" then
          deactivations = deactivations + 1
          private_state = private_state .. ":suspended"
          if deactivations ~= 1 or event.destination ~= nil
              or context.agent.active or not context.agent.suspended then
            error("deactivation was missing or duplicated")
          end
          local command = context.move_to(configuration.destination)
          if command.accepted or command.status ~= "inactive_agent" then
            error("deactivated callback issued a command")
          end
        elseif event.type == "activated" then
          activations = activations + 1
          if activations ~= 1 or starts ~= 1
              or private_state ~= "secret_instance_155:suspended"
              or not context.agent.active or context.agent.suspended then
            error("reactivation replaced private state or reran on_start")
          end
        else
          error("unexpected lifecycle event " .. event.type)
        end
        if type(event.tick) ~= "number" or type(event.sequence) ~= "number"
            or pcall(function() event.type = "changed" end) then
          error("mutable lifecycle event")
        end
      end,
      on_timer = function(name, context)
        if name ~= "frozen_timer_155" or starts ~= 1 or activations ~= 1 then
          error("timer state was not preserved")
        end
        local moved = context.move_to(configuration.destination)
        if not moved.accepted then error(moved.status) end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Activation",
			"activation.lua", {
				{ "destination", core::AgentBehaviourSchemaType::Marker }
			});

		core::World world("Activation lifetime", 12, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 12, 1);
		world.addSectorMarker(room, 0, 10.5f, "Destination");
		world.finishBuild();
		auto const agent = world.createAgent("Sleeper", room, 0, 0.5f);
		auto const destination = world.getMarkerIds().front();
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("activation.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", destination } }),
			"Could not assign activation lifetime fixture");
		require(world.resumeSimulation(), "Could not start activation fixture");
		world.consumeSimulationEvents();
		world.advanceTick(); // on_start at tick 0; timer due at tick 3.
		world.pauseSimulation();
		world.consumeSimulationEvents();
		require(world.setAgentActive(agent, false), "Could not deactivate Agent");
		require(world.setAgentActive(agent, false),
			"Idempotent deactivation was refused");
		unsigned deactivatedEvents = 0;
		for (auto const& event : world.consumeSimulationEvents())
			deactivatedEvents += event.type
				== core::SimulationEventType::AgentDeactivated;
		require(deactivatedEvents == 1,
			"Deactivation did not publish exactly one semantic transition");

		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("secret_instance_155") == std::string::npos
			&& yaml.find("frozen_timer_155") == std::string::npos,
			"Private instance state or timers entered World persistence");

		require(world.resumeSimulation(), "Could not run deactivated fixture");
		world.advanceTicks(5);
		require(!world.lookupAgent(agent).entity->getPath(),
			"A suspended timer or command moved a deactivated Agent");
		world.pauseSimulation();
		world.consumeSimulationEvents();
		require(world.setAgentActive(agent, true), "Could not reactivate Agent");
		unsigned activatedEvents = 0;
		for (auto const& event : world.consumeSimulationEvents())
			activatedEvents += event.type == core::SimulationEventType::AgentActivated;
		require(activatedEvents == 1,
			"Reactivation did not publish exactly one semantic transition");
		require(world.resumeSimulation(), "Could not resume reactivated fixture");
		world.advanceTick();
		world.advanceTick();
		require(!world.lookupAgent(agent).entity->getPath(),
			"Frozen timer used elapsed deactivation ticks");
		world.advanceTick();
		require(world.lookupAgent(agent).entity->getState() == core::Agent::State::RoutePlanning
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Reactivation did not resume the same instance at the remaining timer duration");
	}

	void interactionOutcomesAreImmutableSemanticValues(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "interactions.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "interactions.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local completed = nil
    return { on_event = function(event, context)
      if event.type == "interaction_completed" then
        if event.name ~= "Working control" or event.result ~= "succeeded"
            or event.reason ~= nil or type(event.interaction) ~= "userdata" then
          error("incorrect completed interaction payload")
        end
        completed = event.interaction
      elseif event.type == "interaction_failed" then
        if event.name ~= "Broken control" or event.reason ~= "failed"
            or event.result ~= nil or type(event.interaction) ~= "userdata"
            or event.interaction == completed then
          error("incorrect failed interaction payload")
        end
        local moved = context.move_to(configuration.destination)
        if not moved.accepted then error(moved.status) end
      else
        error("unexpected interaction event")
      end
      for _, forbidden in ipairs({ "actor", "point", "operations", "request",
          "snapshot", "device_operation", "destination" }) do
        if event[forbidden] ~= nil then error("exposed " .. forbidden) end
      end
      if type(event.tick) ~= "number" or type(event.sequence) ~= "number"
          or pcall(function() event.name = "changed" end)
          or pcall(function() event.interaction.value = 1 end) then
        error("mutable interaction event")
      end
    end }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Interactions",
			"interactions.lua", {
				{ "destination", core::AgentBehaviourSchemaType::Marker }
			});

		core::World world("Interaction outcomes", 10, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 10, 1);
		world.addSectorMarker(room, 0, 8.5f, "Destination");
		auto const sector = core::SectorId{ static_cast<uint64_t>(room) + 1 };
		core::InteractionBinding command{
			{ core::DeviceCommandType::SetSectorLights, sector, true },
			core::InteractionBindingRequirement::Required };
		auto const working = world.createInteractionPoint("Working control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		auto const broken = world.createInteractionPoint("Broken control", sector,
			{ 0.5f, 0.0f }, 0.6f, 0.0f, { command });
		world.finishBuild();
		auto const agent = world.createAgent("Operator", room, 0, 0.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("interactions.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			{ { "destination", world.getMarkerIds().front() } }),
			"Could not assign interaction outcome fixture");
		require(world.resumeSimulation(), "Could not start interaction fixture");
		world.advanceTick(); // Construct and start the instance.

		auto const completedRequest = world.requestInteraction(working, agent);
		auto completed = world.lookupInteractionRequest(completedRequest);
		require(completed && !completed.entity->getOperations().empty(),
			"Could not create completed interaction fixture");
		world.lookupDeviceOperation(completed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Succeeded);
		world.advanceTick(); // Publish completion.
		world.advanceTick(); // Deliver completion.

		auto const failedRequest = world.requestInteraction(broken, agent);
		auto failed = world.lookupInteractionRequest(failedRequest);
		require(failed && !failed.entity->getOperations().empty(),
			"Could not create failed interaction fixture");
		world.lookupDeviceOperation(failed.entity->getOperations().front().first)
			.entity->setState(core::DeviceOperationState::Failed);
		world.advanceTick(); // Publish failure.
		world.advanceTick(); // Deliver failure and its movement command.
		require(world.lookupAgent(agent).entity->getState() == core::Agent::State::RoutePlanning
			&& world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"Immutable semantic interaction outcomes were not delivered correctly");
	}

	void teardownIsReadOnlyAndBestEffort(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "teardown.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "failure.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function()
    return {
      on_start = function(context) context.set_timer("fail", 1) end,
      on_timer = function() error("primary callback failure") end,
      on_stop = function(reason, context)
        if reason ~= "instance_failure" or context.move_to ~= nil
            or context.cancel_movement ~= nil or context.set_timer ~= nil
            or context.cancel_timer ~= nil or context.random_number ~= nil
            or context.random_integer ~= nil or type(context.state.name) ~= "string"
            or pcall(function() context.tick = 0 end) then
          error("on_stop context was not read-only")
        end
        error("best-effort stop failure")
      end
    }
  end
}
)lua");
		auto const failureBehaviour = registry->addAgentBehaviour("Failure teardown",
			"failure.lua", {});
		writeRuntimeText(package / "unassignment.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function()
    return { on_stop = function(reason, context)
      if reason ~= "unassignment" or context.move_to ~= nil
          or context.set_timer ~= nil or context.random_integer ~= nil then
        error("wrong unassignment teardown")
      end
      error("unassignment stop observed")
    end }
  end
}
)lua");
		auto const unassignmentBehaviour = registry->addAgentBehaviour("Unassignment",
			"unassignment.lua", {});
		writeRuntimeText(package / "close.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function()
    return { on_stop = function(reason, context)
      if reason ~= "world_close" or context.cancel_movement ~= nil
          or context.cancel_timer ~= nil or context.random_number ~= nil then
        error("wrong World-close teardown")
      end
      error("close failure must not escape")
    end }
  end
}
)lua");
		auto const closeBehaviour = registry->addAgentBehaviour("Close",
			"close.lua", {});

		auto makeWorld = [&]
		{
			auto world = std::make_unique<core::World>("Teardown", 6, 2);
			auto const room = world->addRoom("Room", 0, 0, 0, 6, 1);
			world->finishBuild();
			auto const agent = world->createAgent("Agent", room, 0, 0.5f);
			world->pauseSimulation();
			world->attachAgentBehaviourRegistry("teardown.behaviours", registry);
			return std::pair{ std::move(world), agent };
		};

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, failureBehaviour,
				registry->lookupAgentBehaviour(failureBehaviour)->getRevision(), {}),
				"Could not assign failure teardown fixture");
			require(world->resumeSimulation(), "Could not start failure teardown");
			world->advanceTicks(2);
			auto writer = core::YamlSerializer::toString();
			core::SerializationWorkData work;
			work.markSerializedUnmodified = false;
			world->serialize(*writer, work);
			writer->serialize();
			require(writer->getSerializedString().find("primary callback failure")
					== std::string::npos
				&& writer->getSerializedString().find("best-effort stop failure")
					== std::string::npos,
				"Runtime diagnostics entered World persistence");
			auto diagnostics = world->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 2
				&& diagnostics[0].callback == "on_timer"
				&& diagnostics[0].diagnostic.find("primary callback failure")
					!= std::string::npos
				&& diagnostics[1].callback == "on_stop"
				&& diagnostics[1].diagnostic.find("best-effort stop failure")
					!= std::string::npos
				&& !world->agentBehaviourOwnsMovement(agent),
				"Instance failure did not complete best-effort teardown");
		}

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, unassignmentBehaviour,
				registry->lookupAgentBehaviour(unassignmentBehaviour)->getRevision(), {}),
				"Could not assign unassignment teardown fixture");
			require(world->resumeSimulation(), "Could not start unassignment fixture");
			world->advanceTick();
			world->pauseSimulation();
			require(world->clearAgentBehaviourAssignment(agent),
				"A failing on_stop vetoed unassignment");
			auto diagnostics = world->consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.size() == 1 && diagnostics[0].callback == "on_stop"
				&& diagnostics[0].diagnostic.find("unassignment stop observed")
					!= std::string::npos
				&& !world->agentBehaviourOwnsMovement(agent),
				"Unassignment did not finish after on_stop failed");
		}

		{
			auto [world, agent] = makeWorld();
			require(world->setAgentBehaviourAssignment(agent, closeBehaviour,
				registry->lookupAgentBehaviour(closeBehaviour)->getRevision(), {}),
				"Could not assign World-close teardown fixture");
			require(world->resumeSimulation(), "Could not start close fixture");
			world->advanceTick();
			world.reset(); // A failing on_stop must not block or escape close.
		}
	}
}

void behaviour_smoke::registerRuntimeCallbacks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "programmingErrorDisablesMovementOwnership", [](smoke::Context const& context)
	{
		programmingErrorDisablesMovementOwnership(context);
	} });
	checks.push_back({ "activationSuspendsStateAndFreezesTimers", [](smoke::Context const& context)
	{
		activationSuspendsStateAndFreezesTimers(context);
	} });
	checks.push_back({ "interactionOutcomesAreImmutableSemanticValues", [](smoke::Context const& context)
	{
		interactionOutcomesAreImmutableSemanticValues(context);
	} });
	checks.push_back({ "teardownIsReadOnlyAndBestEffort", [](smoke::Context const& context)
	{
		teardownIsReadOnlyAndBestEffort(context);
	} });
}
