// Agent behaviour movement runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "core/Agent.h"
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
			"Bundled coroutine workflows did not complete repeated trips");
	}

	void scriptedActionOutcomes(smoke::Context const& context, bool scriptFailure = false)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "actions.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "actions.lua", R"lua(
return {api_version = 3, factory=function(configuration)
    local stage = 0
    local sequence = 0
    return function(context)
      assert(context.move_to(configuration.first, 'missing').status == 'unavailable_action')
      sleep(1)
      assert(context.move_to(configuration.first, configuration.action).accepted)
      while true do
        local event = wait()
        if event.type ~= "route_lost" then
          if event.type ~= 'destination_reached' and event.type ~= 'action_failed' then goto next_event end
          assert(event.sequence > sequence and event.destination ~= nil)
          sequence = event.sequence
          assert(not pcall(function() event.action='idle' end))
          if stage == 0 then
            assert(event.type == 'destination_reached' and event.result == 'succeeded')
            assert(event.destination == configuration.first and event.action == configuration.action)
            assert(context.move_to(configuration.second, configuration.action).status == 'unavailable_action')
            sleep(1)
            assert(context.move_to(configuration.first, configuration.action).accepted)
            stage = 1
          elseif stage == 1 then
            -- No autonomous Idle scheduling: this request comes only from this callback.
            assert(event.action == configuration.action)
            assert(context.move_to(configuration.first, configuration.refusal).accepted)
            stage = 2
          elseif stage == 2 then
            assert(event.type == 'action_failed' and event.result == 'failed')
            assert(event.action == configuration.refusal and event.reason == configuration.failure_reason)
            assert(#event.diagnostic > 0)
            assert(event.script_failure == (event.reason == 'refused' and 'none' or 'lua_error'))
            assert(context.move_to(configuration.second).accepted)
            stage = 3
          elseif stage == 3 then
            assert(event.destination == configuration.second)
            assert(event.action == 'idle' and event.result == 'succeeded')
            assert(context.move_to(configuration.first, configuration.action).accepted)
            stage = 4
          else
            assert(stage == 4 and event.action == configuration.action)
            context.log('completed action chain')
            stage = 5
          end
        end
        ::next_event::
      end
    end
end}
)lua");
		auto behaviour = registry->addAgentBehaviour("Action chain", "actions.lua", {
			{ "first", core::AgentBehaviourSchemaType::Marker },
			{ "second", core::AgentBehaviourSchemaType::Marker },
			{ "action", core::AgentBehaviourSchemaType::Action },
			{ "refusal", core::AgentBehaviourSchemaType::Action },
			{ "failure_reason", core::AgentBehaviourSchemaType::String }
		});
		auto path = temporary.path / "test.actions.lua";
		std::string const action = "ad603358-5ebf-45bb-a686-c3f491152c61:greet";
		std::string const refusal = "ad603358-5ebf-45bb-a686-c3f491152c61:refuse";
		writeRuntimeText(path, std::string(R"lua(return {api_version=1, uuid='ad603358-5ebf-45bb-a686-c3f491152c61', actions={
{key='greet', name='Greet', run=function(a,w,m) w.log('greeted') end},
{key='refuse', name='Refuse', run=function(a,w,m) w.set_pose('sitting'); )lua")
			+ (scriptFailure ? "error('Action exception')" : "w.claim()") + " end}}}");
		core::World world("Behaviour Actions", 10, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
		world.addSectorMarker(room, 0, 3.5f, "First");
		world.addSectorMarker(room, 0, 6.5f, "Second");
		world.finishBuild();
		auto agent = world.createAgent("Author", room, 0, 1.5f);
		auto markers = world.getMarkerIds();
		world.pauseSimulation();
		require(world.selectActionRegistry(path) && world.setMarkerActions(markers[0], {action, refusal}),
			"Could not author Actions");
		world.attachAgentBehaviourRegistry("actions.behaviours", registry);
		require(world.setAgentBehaviourAssignment(agent, behaviour, 1, {
			{ "first", markers[0] }, { "second", markers[1] },
			{ "action", core::AgentBehaviourAction{action} },
			{ "refusal", core::AgentBehaviourAction{refusal} },
			{ "failure_reason", scriptFailure ? "script_error" : "refused" }
		}), "Could not assign Action configuration");
		require(world.resumeSimulation(), "Could not resume Action chain");
		unsigned successes = 0, failures = 0;
		for (unsigned tick = 0; tick < 2500; ++tick)
		{
			auto const advanced = world.advanceTick();
			if (!advanced && !scriptFailure)
			{
				auto const& diagnostics = world.getAgentBehaviourRuntimeDiagnostics();
				throw std::runtime_error("Action chain failed: " + (diagnostics.empty()
					? std::string("no behaviour diagnostic") : diagnostics.back().diagnostic));
			}
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::DestinationReached) ++successes;
				if (event.type == core::SimulationEventType::ActionFailed) ++failures;
			}
			if (!advanced)
				require(failures == 1 && world.resumeSimulation(), "Script failure could not be observed after public resume");
		}
		require(successes == 4 && failures == 1 && world.getAgentBehaviourRuntimeDiagnostics().empty(),
			"Behaviour did not observe success/refusal/Idle and select its next target");
	}

	void furnitureUseOutcomes(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "furniture.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "choose.lua", R"lua(
return {api_version = 3, factory=function(configuration)
    local failed, completed = false, false
    local function choose(context)
      assert(not failed)
      failed = true
      assert(context.move_to(configuration.alternative, 'use-furniture').accepted)
    end
    return function(context)
      assert(not context.move_to(configuration.occupied, 'use-furniture').accepted)
      sleep(1)
      choose(context)
      while true do
        local event = wait()
        if event.type == 'destination_reached' then
          assert(failed and not completed and event.destination == configuration.alternative)
          assert(event.result == 'succeeded' and event.action == 'use-furniture')
          completed = true
        end
      end
    end
end}
)lua");
		auto behaviour = registry->addAgentBehaviour("Choose another seat", "choose.lua", {
			{ "occupied", core::AgentBehaviourSchemaType::Marker },
			{ "alternative", core::AgentBehaviourSchemaType::Marker }
		});
		// Occupancy eligibility refuses before movement, including coincident targets;
		// the behaviour can immediately choose another usable point.
		for (float start : {0.5f, 3.5f})
		{
			core::World world("Behaviour Furniture outcomes", 12, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
			world.attachFurnitureCatalogue("use.furniture.lua", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/use.furniture.lua")));
			world.placeFurniture(room, "sofa", 3, 0, "Independent seats");
			world.finishBuild();
			auto left = world.furniture().front().destinations[0].marker;
			auto right = world.furniture().front().destinations[1].marker;
			auto owner = world.createAgent("Owner", room, 0, 3.5f);
			require(world.moveAgentToMarker(owner, left, core::UseFurnitureAction).accepted()
				&& world.advanceTicks(600) && world.usablePointOccupant(left) == owner,
				"Could not establish occupied seat");
			auto chooser = world.createAgent("Chooser", room, 0, start);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("furniture.behaviours", registry);
			require(world.setAgentBehaviourAssignment(chooser, behaviour, 1,
				{{"occupied", left}, {"alternative", right}}), "Could not assign Furniture choice behaviour");
			world.consumeSimulationEvents();
			require(world.resumeSimulation(), "Could not resume Furniture choice");
			unsigned failures = 0, successes = 0;
			for (unsigned tick = 0; tick < 1800; ++tick)
			{
				if (!world.advanceTick())
				{
					auto const& diagnostics = world.getAgentBehaviourRuntimeDiagnostics();
					throw smoke::Failure("Furniture choice failed: " + (diagnostics.empty()
						? std::string("no behaviour diagnostic") : diagnostics.back().diagnostic));
				}
				for (auto const& event : world.consumeSimulationEvents())
				{
					if (event.agent.id != chooser) continue;
					if (event.type == core::SimulationEventType::ActionFailed || event.type == core::SimulationEventType::RouteLost)
					{
						++failures;
						require(event.destinationMarker == left && event.selectedAction == core::UseFurnitureAction
							&& event.scriptFailure == core::ScriptExecutionFailure::None
							&& event.agent.pose == core::Pose::Standing, "Seat conflict lost ordinary atomic request outcome");
					}
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						++successes;
						require(event.destinationMarker == right && event.selectedAction == core::UseFurnitureAction,
							"Behaviour did not select alternative Furniture use");
					}
				}
			}
			require(failures == 0 && successes == 1 && world.getAgentBehaviourRuntimeDiagnostics().empty()
				&& world.usablePointOccupant(left) == owner && world.usablePointOccupant(right) == chooser
				&& world.lookupAgent(owner).entity->getPose() == core::Pose::Sitting
				&& world.lookupAgent(chooser).entity->getPose() == core::Pose::Sitting,
				"Behaviour choice lost independent claims or produced duplicate outcomes");
		}
	}

	void scriptedActionCancellations(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "cancel.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "cancel.lua", R"lua(
return {api_version = 3, factory=function(configuration)
    return function(context)
      assert(context.move_to(configuration.destination, configuration.action).accepted)
      if configuration.reason == 'explicit' or configuration.reason == 'superseded' then
        sleep(1)
        if configuration.reason == 'explicit' then
          assert(context.cancel_movement().accepted)
        else
          assert(context.move_to(configuration.destination).status == 'superseded')
        end
      end
      while true do
        local event = wait()
        if event.type ~= "route_lost" then
          if event.type ~= 'movement_cancelled' or event.action == 'idle' then goto next_event end
          assert(event.destination ~= nil and event.action == configuration.action)
          assert(event.result == 'cancelled' and event.reason == configuration.reason)
          assert(context.move_to(configuration.fallback).accepted)
        end
        ::next_event::
      end
    end
end}
)lua");
		auto behaviour = registry->addAgentBehaviour("Cancellation", "cancel.lua", {
			{ "destination", core::AgentBehaviourSchemaType::String },
			{ "fallback", core::AgentBehaviourSchemaType::Marker },
			{ "action", core::AgentBehaviourSchemaType::Action },
			{ "reason", core::AgentBehaviourSchemaType::String }
		});
		auto path = temporary.path / "cancel.actions.lua";
		std::string const action = "ad603358-5ebf-45bb-a686-c3f491152c61:greet";
		writeRuntimeText(path, "return {api_version=1, uuid='ad603358-5ebf-45bb-a686-c3f491152c61', actions={{key='greet',name='Greet',run=function() end}}}");
		for (std::string const reason : {"explicit", "superseded", "action_unavailable", "target_deleted"})
		{
			core::World world("Cancelled Actions", 10, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 10, 1);
			world.addSectorMarker(room, 0, 8.5f, "Target");
			world.addSectorMarker(room, 0, 2.5f, "Fallback");
			world.finishBuild();
			auto agent = world.createAgent("Author", room, 0, 1.5f);
			auto markers = world.getMarkerIds();
			world.pauseSimulation();
			require(world.selectActionRegistry(path) && world.setMarkerActions(markers[0], {action}), "Cancellation Action refused");
			world.attachAgentBehaviourRegistry("cancel.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour, 1, {
				{ "destination", "Target" }, { "fallback", markers[1] },
				{ "action", core::AgentBehaviourAction{action} },
				{ "reason", reason }
			}), "Cancellation assignment refused");
			require(world.resumeSimulation() && world.advanceTick(), "Cancellation start failed");
			world.pauseSimulation();
			if (reason == "target_deleted")
			{
				require(world.removeSectorMarker(room, 0), "Target deletion refused");
				world.finishBuild();
			}
			else if (reason == "action_unavailable")
				require(world.setMarkerActions(markers[0], {}), "Action removal refused");
			require(world.resumeSimulation(), "Cancellation resume failed");
			unsigned cancelled = 0, reached = 0;
			for (unsigned tick = 0; tick < 1000; ++tick)
			{
				require(world.advanceTick(), "Cancellation callback failed");
				for (auto const& event : world.consumeSimulationEvents())
				{
					if (event.type == core::SimulationEventType::MovementCancelled && event.selectedAction == action)
					{
						require(event.destinationMarker == markers[0], "Cancellation lost accepted Marker identity");
						++cancelled;
					}
					if (event.type == core::SimulationEventType::DestinationReached) ++reached;
				}
			}
			require(cancelled == 1 && reached == 1 && world.getAgentBehaviourRuntimeDiagnostics().empty(),
				"Cancellation was not observed with Action identity and fallback");
		}
	}

	void planningIntentReplacement(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto package = temporary.path / "planning.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		std::string const source = R"lua(
local host = require("promethium.v3")
assert(host.api_version == 3)
return { api_version = host.api_version, factory = function(configuration)
    return function(context)
      local cancellations = 0
      assert(context.agent.movement_state == "idle") -- immutable startup snapshot
      assert(context.agent.route_planning_remaining_ticks == nil)
      assert(context.agent.route_planning_total_ticks == nil)
      assert(context.agent.random_state == nil and context.random_state == nil)
      assert(context.move_to(configuration.first).status == "accepted")
      sleep(1)
      local duplicate = context.move_to(configuration.first)
      assert(duplicate.accepted and duplicate.status == "no_op")
      sleep(1)
      local replacement = context.move_to(configuration.second)
      assert(replacement.accepted and replacement.status == "superseded")
      sleep(2)
      local replaced = wait()
      assert(replaced.type == "movement_cancelled" and replaced.reason == "superseded")
      cancellations = cancellations + 1
      assert(cancellations == 1)
      assert(context.cancel_movement().accepted)
      local cancelled = wait()
      assert(cancelled.type == "movement_cancelled" and cancelled.reason == "explicit")
      while true do wait() end
    end
end }
)lua";
		writeRuntimeText(package / "planning.lua", source);
		writeRuntimeText(package / "second.lua", source);
		auto second = registry->addAgentBehaviour("Second", "second.lua", {
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
		auto secondId = world.createAgent("Second planner", room, 0, 2.5f);
		auto markers = world.getMarkerIds();
		world.pauseSimulation();
		world.attachAgentBehaviourRegistry("planning.behaviours", registry);
		require(world.setAgentBehaviourAssignment(id, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(), {
				{ "first", markers[0] }, { "second", markers[1] }
			}), "Could not assign planning behaviour");
		require(world.setAgentBehaviourAssignment(secondId, second,
			registry->lookupAgentBehaviour(second)->getRevision(), {
				{ "first", markers[0] }, { "second", markers[1] }
			}), "Could not assign second planning behaviour");
		registry->saveTo((package / "behaviours.yaml").string());
		for (unsigned run = 0; run < 2; ++run)
		{
			if (run != 0)
			{
				world.pauseSimulation();
				std::string diagnostic;
				require(core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic),
					"Could not reload coroutine registry: " + diagnostic);
			}
			require(world.resumeSimulation(), "Could not resume planning behaviour");
			world.advanceTicks(10);
			unsigned cancellations = 0;
			unsigned secondCancellations = 0;
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::MovementCancelled)
				{
					if (event.agent.id == secondId)
					{
						require(secondCancellations < 2 && event.destinationMarker == markers[secondCancellations]
							&& event.movementCancellationReason == (secondCancellations == 0
								? core::MovementCancellationReason::Superseded : core::MovementCancellationReason::Explicit),
							"Second coroutine exposed an incorrect cancellation");
						++secondCancellations;
						continue;
					}
					require(cancellations < 2 && event.destinationMarker == markers[cancellations]
						&& event.movementCancellationReason == (cancellations == 0
							? core::MovementCancellationReason::Superseded : core::MovementCancellationReason::Explicit),
						"Behaviour cancellation payload incorrect");
					++cancellations;
				}
			require(cancellations == 2 && secondCancellations == 2,
				"Per-Agent coroutine planning replacement/cancellation did not execute");
			require(world.getAgentBehaviourRuntimeDiagnostics().empty(),
				"Behaviour planning semantic assertions failed");
		}
	}

	void routeLossAndTopologyLifecycle(smoke::Context const& context, bool replay)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "lifecycle.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "lifecycle.lua", R"lua(
local host = require("promethium.v3")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local losses = 0
    return function(context)
      local result = context.move_to(configuration.destination)
      if not result.accepted or result.status ~= "accepted" then error(result.status) end
      while true do
        local event = wait()
        if event.type == "route_lost" then
          local destination, reason, outcome = event.destination, event.reason, event
          assert(outcome.destination == destination and outcome.reason == reason)
          assert(outcome.action == 'idle' and outcome.result == 'failed')
          losses = losses + 1
          if losses ~= 1 or destination ~= configuration.destination
              or reason ~= configuration.expected_reason then
            error("incorrect or duplicate route-loss callback")
          end
          local result = context.move_to(configuration.fallback)
          if not result.accepted or result.status ~= "accepted" then error(result.status) end
        end
      end
    end
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
		if (replay)
		{
			runTopology(false);
			runTopology(true);
		}

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
	checks.push_back({ "scriptedActionOutcomes", [](auto const& context) { scriptedActionOutcomes(context); } });
	checks.push_back({ "scriptedActionScriptFailure", [](auto const& context) { scriptedActionOutcomes(context, true); } });
	checks.push_back({ "scriptedActionCancellations", scriptedActionCancellations });
	checks.push_back({ "furnitureUseOutcomes", furnitureUseOutcomes });
	checks.push_back({ "bundledMovementWorkflows", [](smoke::Context const& context)
	{
		bundledMovementWorkflows(context);
	} });
	checks.push_back({ "planningIntentReplacement", [](smoke::Context const& context)
	{
		planningIntentReplacement(context);
	} });
	checks.push_back({ "routeLossAndTopologyLifecycle", [](smoke::Context const& context)
	{
		routeLossAndTopologyLifecycle(context, false);
	} });
	checks.push_back({ "routeLossAndTopologyLifecycleReplay", [](smoke::Context const& context)
	{
		routeLossAndTopologyLifecycle(context, true);
	} });
}
