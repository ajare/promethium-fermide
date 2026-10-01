// Agent behaviour movement runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/AgentBehaviourRuntime.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	using behaviour_smoke::writeRuntimeText;
	using smoke::require;

	void bundledMovementWorkflows(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "bundled.behaviours";
		std::filesystem::create_directories(package);
		std::filesystem::copy_file(context.fixture(
			"resources/test-worlds/door-test-1.behaviours/marker-patrol.lua"),
			package / "patrol.lua");
		std::filesystem::copy_file(context.fixture(
			"resources/test-worlds/new-world.behaviours/random-marker-wander.lua"),
			package / "wander.lua");
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto patrol = registry->addAgentBehaviour("Patrol", "patrol.lua", {
			{ "first_marker", core::AgentBehaviourSchemaType::Marker },
			{ "second_marker", core::AgentBehaviourSchemaType::Marker }
		});
		auto wander = registry->addAgentBehaviour("Wander", "wander.lua", {
			{ "markers", core::AgentBehaviourSchemaType::List, {
				{ "marker", core::AgentBehaviourSchemaType::Marker }
			} }
		});
		core::World world("Bundled workflows", 10, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
		auto isolated = world.addRoom("Isolated", 1, 0, 0, 10, 1);
		world.addSectorMarker(room, 0, 3.5f, "First");
		world.addSectorMarker(room, 0, 6.5f, "Second");
		world.addSectorMarker(isolated, 0, 5.5f, "Unreachable");
		world.finishBuild();
		auto const markers = world.getMarkerIds();
		auto patroller = world.createAgent("Patroller", room, 0, 1.5f);
		auto wanderer = world.createAgent("Wanderer", room, 0, 1.5f);
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("bundled.behaviours", registry);
		require(world.setAgentBehaviourAssignment(patroller, patrol,
			registry->lookupAgentBehaviour(patrol)->getRevision(), {
				{ "first_marker", markers[0] }, { "second_marker", markers[1] }
			}), "Could not assign bundled patrol");
		core::AgentBehaviourConfigurationList destinations;
		for (auto marker : markers) destinations.emplace_back(marker);
		require(world.setAgentBehaviourAssignment(wanderer, wander,
			registry->lookupAgentBehaviour(wander)->getRevision(), {
				{ "markers", std::move(destinations) }
			}), "Could not assign bundled wander");
		require(world.resumeSimulation(), "Could not resume bundled workflows");
		unsigned arrivals[2]{};
		uint64_t previousTick[2]{};
		core::MarkerId previousMarker[2]{};
		for (unsigned tick = 0; tick < 6000 && (arrivals[0] < 4 || arrivals[1] < 4); ++tick)
		{
			require(world.advanceTick(), "Bundled behaviour failed during movement");
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type != core::SimulationEventType::DestinationReached) continue;
				auto const index = event.agent.id == patroller ? 0 : 1;
				require(event.destinationMarker != previousMarker[index]
					&& event.destinationMarker != markers[2],
					"Bundled workflow repeated an arrival or reached an isolated Marker");
				if (index == 0)
					require(event.destinationMarker == markers[arrivals[index] % 2],
						"Bundled patrol changed its alternating destinations");
				if (arrivals[index] != 0)
					require(event.tick - previousTick[index] >= (index == 0 ? 300u : 180u),
						"Bundled workflow skipped its arrival wait");
				previousMarker[index] = event.destinationMarker;
				previousTick[index] = event.tick;
				++arrivals[index];
			}
		}
		require(arrivals[0] >= 4 && arrivals[1] >= 4
			&& world.getAgentBehaviourRuntimeDiagnostics().empty(),
			"Bundled v2 workflows did not complete repeated trips");
	}

	void planningIntentReplacement(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "planning.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		std::string const source = R"lua(
assert(require("prometheum.v1").api_version == 1)
assert(require("prometheum.v2").api_version == 2)
return {
  api_version = version,
  factory = function(configuration)
    local cancellations = 0
    return {
      on_start = function(context)
        assert(context.move_to(configuration.first).status == "accepted")
        context.set_timer("duplicate", 1)
      end,
      on_timer = function(name, context)
        assert(context.agent.movement_state == (version == 1 and "idle" or "route_planning"))
        assert(context.agent.route_planning_remaining_ticks == nil)
        assert(context.agent.route_planning_total_ticks == nil)
        assert(context.agent.random_state == nil and context.random_state == nil)
        if name == "duplicate" then
          local duplicate = context.move_to(configuration.first)
          assert(duplicate.accepted and duplicate.status == "no_op")
          context.set_timer("replace", 1)
        elseif name == "replace" then
          local replacement = context.move_to(configuration.second)
          assert(replacement.accepted == (version == 2))
          assert(replacement.status == (version == 2 and "superseded" or "agent_busy"))
          context.set_timer("cancel", 2)
        else
          assert(context.cancel_movement().accepted)
        end
      end,
      on_event = function(event, context)
        if event.type == "movement_cancelled" then
          cancellations = cancellations + 1
          assert(event.reason == (version == 2 and cancellations == 1 and "superseded" or "explicit"))
          assert(cancellations <= (version == 2 and 2 or 1))
        end
      end
    }
  end
}
)lua";
		writeRuntimeText(package / "planning.lua", "local version = 2\n" + source);
		writeRuntimeText(package / "legacy.lua", "local version = 1\n" + source);
		auto legacy = registry->addAgentBehaviour("Legacy", "legacy.lua", {
			{ "first", core::AgentBehaviourSchemaType::Marker },
			{ "second", core::AgentBehaviourSchemaType::Marker }
		});
		auto behaviour = registry->addAgentBehaviour("Planning", "planning.lua", {
			{ "first", core::AgentBehaviourSchemaType::Marker },
			{ "second", core::AgentBehaviourSchemaType::Marker }
		});
		core::World world("Behaviour planning", 10, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
		world.addSectorMarker(room, 0, 8.5f, "First");
		world.addSectorMarker(room, 0, 6.5f, "Second");
		world.finishBuild();
		auto id = world.createAgent("Planner", room, 0, 1.5f);
		auto legacyId = world.createAgent("Legacy planner", room, 0, 2.5f);
		auto markers = world.getMarkerIds();
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("planning.behaviours", registry);
		require(world.setAgentBehaviourAssignment(id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {
				{ "first", markers[0] }, { "second", markers[1] }
			}), "Could not assign planning behaviour");
		require(world.setAgentBehaviourAssignment(legacyId, legacy,
			registry->lookupAgentBehaviour(legacy)->getRevision(), {
				{ "first", markers[0] }, { "second", markers[1] }
			}), "Could not assign legacy planning behaviour");
		registry->saveTo((package / "behaviours.yaml").string());
		for (unsigned run = 0; run < 2; ++run)
		{
			if (run != 0)
			{
				world.pauseSimulation();
				std::string diagnostic;
				require(core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic),
					"Could not reload mixed-version registry: " + diagnostic);
			}
			require(world.resumeSimulation(), "Could not resume planning behaviour");
			world.advanceTicks(10);
			unsigned cancellations = 0;
			unsigned legacyCancellations = 0;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled)
				{
					if (event.agent.id == legacyId)
					{
						require(event.destinationMarker == markers[0]
							&& event.movementCancellationReason == core::MovementCancellationReason::Explicit,
							"Legacy behaviour exposed supersession");
						++legacyCancellations;
						continue;
					}
					require(cancellations < 2 && event.destinationMarker == markers[cancellations]
						&& event.movementCancellationReason == (cancellations == 0
							? core::MovementCancellationReason::Superseded : core::MovementCancellationReason::Explicit),
						"Behaviour cancellation payload incorrect");
					++cancellations;
				}
			require(cancellations == 2 && legacyCancellations == 1,
				"Mixed-version planning replacement/cancellation did not execute");
			require(world.getAgentBehaviourRuntimeDiagnostics().empty(),
				"Behaviour planning semantic assertions failed");
		}
	}

	void routeLossAndTopologyLifecycle(smoke::Context const& context, int version)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "lifecycle.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "lifecycle.lua", "local version = " + std::to_string(version) + R"lua(
local host = require("prometheum.v" .. version)
return {
  api_version = host.api_version,
  factory = function(configuration)
    local losses = 0
    return {
      on_start = function(context)
        local result = context.move_to(configuration.destination)
        if not result.accepted or result.status ~= "accepted" then error(result.status) end
      end,
      on_route_lost = function(destination, reason, context)
        losses = losses + 1
        if losses ~= 1 or destination ~= configuration.destination
            or reason ~= configuration.expected_reason then
          error("incorrect or duplicate route-loss callback")
        end
        local result = context.move_to(configuration.fallback)
        if not result.accepted or result.status ~= "accepted" then error(result.status) end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Lifecycle", "lifecycle.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "fallback", core::AgentBehaviourSchemaType::Marker },
			{ "expected_reason", core::AgentBehaviourSchemaType::String }
		});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The route-loss lifecycle fixture did not preflight");

		auto runTopology = [&](bool disconnect)
		{
			core::World world(disconnect ? "Lost route" : "Replacement route", 8, 2);
			auto const front = world.addRoom("Front", 0, 0, 0, 8, 1);
			auto const back = world.addRoom("Back", 1, 0, 0, 8, 1);
			auto const door = world.addSectorDoor(front, 0, 2, {});
			world.addSectorMarker(back, 0, 6.5f, "Destination");
			world.addSectorMarker(front, 0, 0.5f, "Fallback");
			world.finishBuild();
			auto const markers = world.getMarkerIds();
			auto const id = world.createAgent("Walker", front, 0, 0.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("lifecycle.behaviours", registry);
			auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
			require(world.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "destination", markers[0] }, { "fallback", markers[1] },
				{ "expected_reason", std::string("topology_changed") }
			}), "Could not assign topology lifecycle behaviour");
			require(world.resumeSimulation(), "Could not start topology lifecycle fixture");
			world.consumeSimulationEvents();
			world.advanceTicks(5);
			// Invalidate an installed Path, not the behaviour's initial planning episode.
			world.advanceTicks(world.lookupAgent(id).entity->getRoutePlanningRemainingTicks());
			world.consumeSimulationEvents();
			world.pauseSimulation();
			if (disconnect)
				require(world.removeSectorDoor(front, door.door.index),
					"Could not remove the topology fixture Door");
			world.finishBuild();
			require(world.resumeSimulation(), "Could not resume rebuilt topology fixture");
			world.consumeSimulationEvents();

			unsigned losses = 0, reached = 0;
			core::MarkerId reachedMarker;
			for (unsigned tick = 0; tick < 3000 && reached == 0; ++tick)
			{
				world.advanceTick();
				for (auto const& event : world.consumeSimulationEvents())
				{
					if (event.type == core::SimulationEventType::RouteLost)
					{
						++losses;
						require(event.destinationMarker == markers[0]
							&& event.routeLossReason == core::RouteLossReason::TopologyChanged,
							"Topology route loss lacked its destination or semantic reason");
					}
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						++reached;
						reachedMarker = event.destinationMarker;
					}
				}
			}
			require(reached == 1 && losses == (disconnect ? 1u : 0u)
				&& reachedMarker == markers[disconnect ? 1u : 0u],
				std::string(disconnect
					? "Failed topology restoration did not call on_route_lost exactly once"
					: "Valid same-destination topology replanning called Lua or lost its goal")
					+ " (losses=" + std::to_string(losses)
					+ ", reached=" + std::to_string(reached)
					+ ", marker=" + std::to_string(reachedMarker.value) + ")");
		};
		runTopology(false);
		runTopology(true);

		core::World unreachable("Initial route loss", 12, 2);
		auto const origin = unreachable.addRoom("Origin", 0, 0, 0, 6, 1);
		auto const isolated = unreachable.addRoom("Isolated", 1, 0, 6, 6, 1);
		unreachable.addSectorMarker(isolated, 0, 3.5f, "Destination");
		unreachable.addSectorMarker(origin, 0, 4.5f, "Fallback");
		unreachable.finishBuild();
		auto const markers = unreachable.getMarkerIds();
		auto const id = unreachable.createAgent("Walker", origin, 0, 0.5f);
		unreachable.pauseSimulation();
		unreachable.attachAgentBehaviourRegistry("lifecycle.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		require(unreachable.setAgentBehaviourAssignment(id, behaviour, revision, {
			{ "destination", markers[0] }, { "fallback", markers[1] },
			{ "expected_reason", std::string("unreachable") }
		}), "Could not assign initial route-loss behaviour");
		require(unreachable.resumeSimulation(), "Could not start initial route-loss fixture");
		unreachable.consumeSimulationEvents();
		unsigned losses = 0, reached = 0;
		for (unsigned tick = 0; tick < 1500 && reached == 0; ++tick)
		{
			unreachable.advanceTick();
			for (auto const& event : unreachable.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::RouteLost)
				{
					++losses;
					require(event.destinationMarker == markers[0]
						&& event.routeLossReason == core::RouteLossReason::Unreachable,
						"Initial route loss lacked its destination or unreachable reason");
				}
				if (event.type == core::SimulationEventType::DestinationReached) ++reached;
			}
		}
		require(losses == 1 && reached == 1,
			"Initial unreachability did not clear the goal before one fallback command");
	}
}

void behaviour_smoke::registerRuntimeMovement(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "bundledMovementWorkflows", [](smoke::Context const& context)
	{
		bundledMovementWorkflows(context);
	} });
	checks.push_back({ "planningIntentReplacement", [](smoke::Context const& context)
	{
		planningIntentReplacement(context);
	} });
	checks.push_back({ "routeLossAndTopologyLifecycleV1", [](smoke::Context const& context)
	{
		routeLossAndTopologyLifecycle(context, 1);
	} });
	checks.push_back({ "routeLossAndTopologyLifecycleV2", [](smoke::Context const& context)
	{
		routeLossAndTopologyLifecycle(context, 2);
	} });
}
