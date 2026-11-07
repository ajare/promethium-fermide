// Agent behaviour determinism runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/Log.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	void planningIgnoresBehaviourRandomConsumption(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "random-noise.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "noise.lua", R"lua(
return {
  api_version = 3,
  factory = function(configuration)
    return function(context)
      while true do
        sleep(1)
        for i = 1, configuration.draws do
          context.random_integer(1, 1000)
          context.random_number()
        end
      end
    end
  end
}
)lua");
		auto behaviour = registry->addAgentBehaviour("Noise", "noise.lua", {
			{ "draws", core::AgentBehaviourSchemaType::Integer }
		});
		registry->saveTo((package / "behaviours.yaml").string());
		auto run = [&](int draws)
		{
			core::World world("Random isolation", 10, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
			world.addSectorMarker(room, 0, 8.5f, "Destination");
			world.finishBuild();
			auto planner = world.createAgent("Planner", room, 0, 1.5f);
			auto noise = world.createAgent("Noise", room, 0, 2.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("random-noise.behaviours", registry);
			require(world.setAgentBehaviourAssignment(noise, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {{ "draws", int64_t(draws) }}),
				"Could not assign random-noise behaviour");
			require(world.resumeSimulation(), "Could not resume random isolation fixture");
			std::vector<uint64_t> trace;
			for (unsigned episode = 0; episode < 16; ++episode)
			{
				require(world.moveAgentToMarker(planner, world.getMarkerIds().front()).accepted(),
					"Random isolation planning refused");
				trace.push_back(world.lookupAgent(planner).entity->getRoutePlanningTotalTicks());
				world.advanceTicks(5);
				world.cancelAgentMovement(planner);
				world.advanceTick();
				world.consumeSimulationEvents();
			}
			require(world.getAgentBehaviourRuntimeDiagnostics().empty(), "Random-noise behaviour failed");
			return trace;
		};
		require(run(0) == run(37), "Lua random consumption changed the planning stream");
	}

	// Repro of #187: native pairs/next leak Lua's per-state string hash seed, and
	// native tostring leaks process addresses. Fresh World runtimes must replay
	// identically and unsupported identity operations must fail explicitly.
	void tableIterationAndIdentityReplayDeterministically(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "iteration.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "iteration.lua", R"lua(
local host = require("promethium.v3")
return {
  api_version = host.api_version,
  factory = function(configuration)
    return function(context)
      local choices = { a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8 }
      local pairsOrder = ""
      for key in pairs(choices) do pairsOrder = pairsOrder .. key end
      context.log("pairs:" .. pairsOrder)
      if pairsOrder ~= "abcdefgh" then error("unexpected pairs order: " .. pairsOrder) end

      local nextOrder = ""
      local key = next(choices)
      while key ~= nil do
        nextOrder = nextOrder .. key
        key = next(choices, key)
      end
      context.log("next:" .. nextOrder)
      if nextOrder ~= pairsOrder then error("pairs and next disagreed") end

      local mixed = { [true] = 1, [false] = 2, [2] = 3, [1] = 4,
        ["b"] = 5, ["a"] = 6, [1.5] = 7 }
      local mixedOrder = ""
      for value in pairs(mixed) do mixedOrder = mixedOrder .. tostring(value) .. "," end
      context.log("mixed:" .. mixedOrder)
      if mixedOrder ~= "false,true,1,1.5,2,a,b," then
        error("unexpected mixed key order: " .. mixedOrder)
      end

      local list = { "x", "y", "z" }
      local listOrder = ""
      for index, value in ipairs(list) do listOrder = listOrder .. index .. value end
      context.log("ipairs:" .. listOrder)
      if listOrder ~= "1x2y3z" then error("list iteration changed") end

      local firstIdentity = tostring({})
      local secondIdentity = tostring({})
      if firstIdentity ~= "table" or secondIdentity ~= "table"
          or tostring(function() end) ~= "function"
          or tostring(list) ~= "table" then
        error("identity output was not a stable type label: " .. firstIdentity)
      end
      context.log("identity:" .. firstIdentity .. "|" .. secondIdentity
      .. "|" .. tostring(function() end) .. "|" .. tostring(list))
      local formatted = string.format("%s", choices)
      if formatted ~= "table" or ("%s"):format(choices) ~= "table" then
        error("string.format leaked an address: " .. formatted)
      end
      context.log("format:" .. formatted)
      if pcall(string.format, "%p", choices)
          or pcall(function() return ("%p"):format(choices) end) then
        error("string.format accepted the nondeterministic %p conversion")
      end
      context.log("pointer-refused:" .. tostring(pcall(string.format, "%p", choices)))
      local identityRefused = pcall(function()
        local keyed = {}
        keyed[{}] = 1
        for _ in pairs(keyed) do end
    end)
      if identityRefused then error("identity-bearing keys were iterated") end
      context.log("identity-key-refused:" .. tostring(identityRefused))
      context.log("random:" .. context.random_integer(1, 1000))

      local destinations = { alpha = configuration.first, beta = configuration.second }
      local chosen = next(destinations)
      context.log("chosen:" .. chosen)
      local result = context.move_to(destinations[chosen])
      if not result.accepted then error(result.status) end
      while true do
        local event = wait()
        if event.type ~= "route_lost" then
          if event.type == "destination_reached" then
            context.log("reached:" .. tostring(event.destination))
          end
        end
      end
    end
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Iteration", "iteration.lua", {
			{ "first", core::AgentBehaviourSchemaType::Marker },
			{ "second", core::AgentBehaviourSchemaType::Marker }
		});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The deterministic iteration fixture did not preflight");

		auto run = [&]()
		{
			(void)core::consumeLogMessages();
			core::World world("Iteration replay", 12, 2);
			auto const room = world.addRoom("Room", 0, 0, 0, 12, 1);
			world.addSectorMarker(room, 0, 3.5f, "Alpha");
			world.addSectorMarker(room, 0, 8.5f, "Beta");
			world.finishBuild();
			auto const agent = world.createAgent("Iterator", room, 0, 0.5f);
			auto const markers = world.getMarkerIds();
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("iteration.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(),
				{ { "first", markers[0] }, { "second", markers[1] } }),
				"Could not assign the deterministic iteration fixture");
			require(world.resumeSimulation(),
				"Could not resume the deterministic iteration fixture");
			world.advanceTick();
			auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
			require(diagnostics.empty(),
				"A deterministic iteration callback failed: "
					+ (diagnostics.empty() ? std::string() : diagnostics.front().diagnostic));

			core::MarkerId destination;
			for (unsigned tick = 0; tick < 1500 && destination.value == 0; ++tick)
			{
				world.advanceTick();
				for (auto const& event : world.consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached)
						destination = event.destinationMarker;
			}
			require(destination.value != 0,
				"The deterministic iteration fixture never reached a destination");

			std::ostringstream digest;
			for (auto const& message : core::consumeLogMessages())
				digest << message.msg << '\n';
			digest << "destination:" << destination.value;
			return digest.str();
		};

		auto const first = run();
		auto const second = run();
		require(first == second,
			"Lua table iteration or identity output varied across fresh runtimes:\n"
				+ first + "\n---\n" + second);
	}
}

void behaviour_smoke::registerRuntimeDeterminism(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "planningIgnoresBehaviourRandomConsumption", [](smoke::Context const& context)
	{
		planningIgnoresBehaviourRandomConsumption(context);
	} });
	checks.push_back({ "tableIterationAndIdentityReplayDeterministically", [](smoke::Context const& context)
	{
		tableIterationAndIdentityReplayDeterministically(context);
	} });
}
