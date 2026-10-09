// Agent behaviour instances runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <bit>
#include <cmath>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	std::string runStartupMovement(
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry,
		core::AgentBehaviourId behaviour)
	{
		core::World world("Lua startup", 14, 2);
		auto const room = world.addRoom("Room", 0, 0, 0, 14, 1);
		world.addSectorMarker(room, 0, 4.5f, "Near");
		world.addSectorMarker(room, 0, 11.5f, "Far");
		world.finishBuild();
		auto const first = world.createAgent("First", room, 0, 0.5f);
		auto const second = world.createAgent("Second", room, 0, 1.5f);
		auto const markers = world.getMarkerIds();

		world.pauseSimulation();
		world.consumeSimulationEvents();
		world.attachAgentBehaviourRegistry("startup.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		require(world.setAgentBehaviourAssignment(first, behaviour, revision,
			{ { "destination", markers[0] } }),
			"Could not assign the first startup behaviour");
		require(world.setAgentBehaviourAssignment(second, behaviour, revision,
			{ { "destination", markers[1] } }),
			"Could not assign the second startup behaviour");
		require(world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
			"Assigned enabled behaviours did not acquire movement ownership");
		require(world.moveAgentToMarker(first, markers[0]).status
				== core::MovementCommandStatus::BehaviourOwned
			&& world.cancelAgentMovement(first).status
				== core::MovementCommandStatus::BehaviourOwned,
			"Manual movement commands bypassed behaviour ownership");
		auto firstAgent = world.lookupAgent(first).entity;
		auto manualTarget = world.getGraph()->getClosestVertexInSector(
			firstAgent->getSector(), { 3.5f, 0.0f });
		firstAgent->setPath(world.getGraph()->calculatePath(firstAgent, manualTarget), true);
		require(!firstAgent->getPath(),
			"Direct manual Path assignment bypassed behaviour ownership");
		require(world.resumeSimulation(), "Could not resume the Lua startup fixture");
		world.consumeSimulationEvents();

		world.advanceTick();
		auto firstTick = world.getSimulationSnapshot();
		require(firstTick.tick == 1 && firstTick.agents.size() == 2
			&& !firstTick.agents[0].hasPath && !firstTick.agents[1].hasPath
			&& firstTick.agents[0].state == core::AgentPathState::RoutePlanning
			&& firstTick.agents[1].state == core::AgentPathState::RoutePlanning
			&& firstTick.agents[0].routePlanningRemainingTicks == firstTick.agents[0].routePlanningTotalTicks - 1
			&& firstTick.agents[0].globalPosition.x == 0.5f
			&& firstTick.agents[1].globalPosition.x == 1.5f,
			"on_start did not enter stationary planning before the first complete tick");

		std::ostringstream digest;
		unsigned reached = 0;
		auto observePublicEvents = [&]
		{
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type != core::SimulationEventType::DestinationReached) continue;
				++reached;
				digest << event.tick << ':' << event.sequence << ':'
					<< event.agent.id.value << ':' << event.destinationMarker.value << '|';
			}
		};
		observePublicEvents();
		for (unsigned tick = 0; tick < 1500 && reached < 2; ++tick)
		{
			world.advanceTick();
			observePublicEvents();
		}
		require(reached == 2,
			"Independently configured Lua Agents did not reach both Markers");

		auto const completed = world.getSimulationSnapshot();
		require(std::fabs(completed.agents[0].globalPosition.x - 4.5f) < 0.001f
			&& std::fabs(completed.agents[1].globalPosition.x - 11.5f) < 0.001f,
			"Shared behaviour instances did not retain distinct Marker configuration");
		world.advanceTicks(5);
		observePublicEvents();
		require(reached == 2, "on_start or destination_reached delivery ran more than once");
		require(world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
			"A valid immutable destination_reached callback disabled its instance");
		digest << completed.tick << ':'
			<< std::bit_cast<uint32_t>(completed.agents[0].globalPosition.x) << ':'
			<< std::bit_cast<uint32_t>(completed.agents[1].globalPosition.x);
		return digest.str();
	}

	void independentStartupInstancesMoveDeterministically(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "startup.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo(manifest.string());
		writeRuntimeText(package / "startup.lua", R"lua(
local host = require("promethium.v3")
local factories_in_this_environment = 0
return {
  api_version = host.api_version,
  factory = function(configuration)
    factories_in_this_environment = factories_in_this_environment + 1
    if factories_in_this_environment ~= 1 then
      error("module environment was shared between Agents")
    end
    local instance = { starts = 0 }
    return function(context)
      instance.starts = instance.starts + 1
      if instance.starts ~= 1 then error("instance state was shared or restarted") end
      if context.configuration ~= configuration then
        error("callback did not receive its immutable configuration")
      end
      if type(configuration.destination) ~= "userdata"
          or tonumber(configuration.destination) ~= nil then
        error("Marker was not an opaque handle")
      end
      if pcall(function() configuration.destination = false end) then
        error("configuration was mutable")
      end
      local result = context.move_to(configuration.destination)
      if not result.accepted or result.status ~= "accepted" then
        error("move_to did not return a semantic accepted result")
      end
      if pcall(function() result.status = "changed" end) then
        error("command result was mutable")
      end
      instance.retained_context = context
      while true do
        local event = wait()
        if event.type ~= "timer_expired" and event.type ~= "route_lost" then
          instance.events = (instance.events or 0) + 1
          if instance.events ~= 1
              or event.type ~= "destination_reached"
              or type(event.tick) ~= "number"
              or type(event.sequence) ~= "number"
              or event.destination ~= configuration.destination then
            error("destination_reached payload was missing, mutable, or duplicated")
          end
          if pcall(function() event.type = "changed" end) then
            error("semantic movement event was mutable")
          end
          local cancellation = context.cancel_movement()
          if not cancellation.accepted or cancellation.status ~= "no_op" then
            error("idle cancellation did not return semantic no_op")
          end
        end
      end
    end
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Startup", "startup.lua",
			{ { "destination", core::AgentBehaviourSchemaType::Marker } });
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The real startup Lua fixture did not preflight");

		auto const first = runStartupMovement(registry, behaviour);
		auto const second = runStartupMovement(registry, behaviour);
		require(first == second,
			"Per-World Lua startup and movement were not deterministic");
	}

	void manifestHelpersHavePrivatePerAgentGraphs(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "helpers.behaviours";
		std::filesystem::create_directories(package);
		auto const manifest = package / "behaviours.yaml";
		auto const uuid = std::string("123e4567-e89b-42d3-a456-426614174156");
		auto const helperSource = R"lua(
local calls = 0
return {
  nested = { value = 7 },
  increment = function()
    calls = calls + 1
    return calls
  end
}
)lua";
		writeRuntimeText(package / "counter.lua", helperSource);
		writeRuntimeText(package / "private.lua", R"lua(
local host = require("promethium.v3")
local counter = require("helpers.counter")
local cached = require("helpers.counter")
if counter ~= cached then error("helper cache was not instance-local") end
if pcall(function() counter.increment = false end)
    or pcall(function() counter.nested.value = 0 end) then
  error("helper exports were mutable")
end
return {
  api_version = host.api_version,
  factory = function(configuration)
    if counter.increment() ~= 1 then
      error("helper upvalues leaked between Agent instances")
    end
    return function(context)
      local result = context.move_to(configuration.destination)
      if not result.accepted then error(result.status) end
      while true do wait() end
    end
  end
}
)lua");
		auto manifestText = [&](uint64_t revision, std::string const& helperPath)
		{
			return ""
				"  version: 1\n"
				"  uuid: " + uuid + "\n"
				"  revision: " + std::to_string(revision) + "\n"
				"  modules:\n"
				"    - name: helpers.counter\n"
				"      source: " + helperPath + "\n"
				"  nextBehaviourId: 2\n"
				"  behaviours:\n"
				"    - id: 1\n"
				"      name: Private helpers\n"
				"      revision: 1\n"
				"      source: private.lua\n"
				"      schema:\n"
				"        - name: destination\n"
				"          type: marker\n";
		};
		writeRuntimeText(manifest, manifestText(1, "counter.lua"));
		auto registry = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		auto const behaviour = core::AgentBehaviourId{ 1 };
		auto const* helper = registry->lookupHelperModule("helpers.counter");
		require(registry->getPackageRevision() == 1 && helper
			&& helper->getModuleStatus() == core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"Manifest helper declarations did not participate in package status");

		// Runtime construction consumes the text admitted by preflight, never an
		// un-reloaded filesystem edit. Every Agent still executes that cached graph.
		writeRuntimeText(package / "counter.lua", "error('unpreflighted edit executed')\n");
		auto const first = runStartupMovement(registry, behaviour);
		auto const second = runStartupMovement(registry, behaviour);
		require(first == second,
			"Private helper graphs were not deterministic across World runtimes");

		// Changing the declared dependency set is a registry revision change, not
		// an invisible path substitution.
		writeRuntimeText(package / "counter-v2.lua", helperSource);
		writeRuntimeText(manifest, manifestText(1, "counter-v2.lua"));
		auto sameRevision = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		std::string diagnostic;
		require(!registry->replaceDefinitionsFrom(std::move(*sameRevision), &diagnostic)
			&& diagnostic.find("package revision") != std::string::npos,
			"A helper dependency changed without advancing the package revision");
		writeRuntimeText(manifest, manifestText(2, "counter-v2.lua"));
		auto advanced = core::AgentBehaviourRegistry::loadFrom(manifest.string());
		require(registry->replaceDefinitionsFrom(std::move(*advanced), &diagnostic)
			&& registry->getPackageRevision() == 2
			&& registry->lookupHelperModule("helpers.counter")->getSourceModulePath()
				== "counter-v2.lua",
			"An advanced helper dependency revision was not adopted deterministically");
	}
}

void behaviour_smoke::registerRuntimeInstances(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "independentStartupInstancesMoveDeterministically", [](smoke::Context const& context)
	{
		independentStartupInstancesMoveDeterministically(context);
	} });
	checks.push_back({ "manifestHelpersHavePrivatePerAgentGraphs", [](smoke::Context const& context)
	{
		manifestHelpersHavePrivatePerAgentGraphs(context);
	} });
}
