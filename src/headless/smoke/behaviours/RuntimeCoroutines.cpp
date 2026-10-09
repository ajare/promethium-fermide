// Branch-only expansion coverage for #527. All assertions use public seams.
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRuntime.h"
#include "core/World.h"
#include "core/Log.h"

namespace
{
	using smoke::require;

	struct Fixture
	{
		behaviour_smoke::TemporaryDirectory temporary;
		std::shared_ptr<core::AgentBehaviourRegistry> registry;
		core::World world;
		core::AgentId agent;
		core::MarkerId destination;

		Fixture(smoke::Context const& context, std::string const& source,
			core::AgentBehaviourRuntimeLimits limits = {}, bool twoAgents = false)
			: temporary{ context }, world("Coroutines", 14, 2, limits)
		{
			auto package = temporary.path / "coroutines.behaviours";
			std::filesystem::create_directories(package);
			registry = core::AgentBehaviourRegistry::create();
			registry->saveTo((package / "behaviours.yaml").string());
			behaviour_smoke::writeRuntimeText(package / "run.lua", source);
			auto behaviour = registry->addAgentBehaviour("Run", "run.lua", {});
			require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded, "Coroutine preflight failed");
			auto room = world.addRoom("Room", 0, 0, 0, 14, 1);
			world.addSectorMarker(room, 0, 11.5f, "Far");
			world.finishBuild();
			agent = world.createAgent("Agent", room, 0, 0.5f);
			auto second = twoAgents ? world.createAgent("Second", room, 0, 1.5f) : core::AgentId{};
			destination = world.getMarkerIds().front();
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("coroutines.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
				"Could not assign coroutine");
			if (second)
				require(world.setAgentBehaviourAssignment(second, behaviour,
					registry->lookupAgentBehaviour(behaviour)->getRevision(), {}),
					"Could not assign second coroutine");
			require(world.resumeSimulation(), "Could not resume coroutine fixture");
		}
	};

	void coroutineMovementAndCompletion(smoke::Context const& context)
	{
		for (bool waitForArrival : { false, true })
		{
			Fixture fixture(context, std::string(R"lua(
local host = require('promethium.v3')
local next_event = wait
local starts = 0
local factories = 0
return { api_version = host.api_version, factory = function()
  factories = factories + 1
  assert(factories == 1, 'factory environment was shared')
  return function(ctx)
    starts = starts + 1
    assert(starts == 1, 'completed instance restarted')
    assert(ctx.move_to('Far').accepted)
)lua") + (waitForArrival ? R"lua(
    local event = next_event()
    assert(event.type == 'destination_reached')
    assert(event.destination ~= nil and event.result == 'succeeded')
)lua" : "") + "  end\nend }\n", {}, true);
			if (!fixture.world.advanceTick())
				throw std::runtime_error("Coroutine startup failed: "
					+ fixture.world.getAgentBehaviourRuntimeDiagnostics().front().diagnostic);
			unsigned reached = 0;
			for (unsigned i = 0; i < 1600; ++i)
			{
				require(fixture.world.advanceTick(), "Coroutine journey failed");
				for (auto const& event : fixture.world.consumeSimulationEvents())
					if (event.type == core::SimulationEventType::DestinationReached) ++reached;
			}
			require(reached == 2, "Completion cancelled movement, shared instances or restarted the coroutine");
			require(!fixture.world.agentBehaviourOwnsMovement(fixture.agent),
				"Completed coroutine did not release movement ownership after arrival");
			require(std::fabs(fixture.world.getSimulationSnapshot().agents.front().globalPosition.x
				- 11.5f) < 0.001f, "Fire-and-forget movement did not arrive");
			require(fixture.world.getAgentBehaviourRuntimeDiagnostics().empty(),
				"Coroutine event delivery produced a diagnostic");
		}
	}

	void coroutineFailuresAreContained(smoke::Context const& context)
	{
		require(!core::AgentBehaviourRuntimeAdapter::preflightModule("Contract", "bad.lua",
			"return {api_version=3, factory=function() return {} end}").loaded,
			"V3 preflight accepted a callback table");
		for (auto const* body : {
			"error('coroutine exploded')",
			"ctx.move_to('Far'); ctx.move_to('Far')",
			"while true do end" })
		{
			Fixture fixture(context, std::string("return {api_version=3, factory=function() return function(ctx) ")
				+ body + " end end}");
			require(!fixture.world.advanceTick() && fixture.world.isSimulationPaused()
				&& fixture.world.getSimulationSnapshot().tick == 0,
				"Coroutine failure did not pause before tick entry");
			auto diagnostics = fixture.world.getAgentBehaviourRuntimeDiagnostics();
			require(!diagnostics.empty() && diagnostics.front().agent == fixture.agent
				&& diagnostics.front().callback == "resume" && !diagnostics.front().traceback.empty(),
				"Coroutine failure lacked a structured instance diagnostic: "
					+ (diagnostics.empty() ? std::string("none") : diagnostics.front().diagnostic));
			if (std::string_view(body) == "while true do end")
				require(diagnostics.front().failure == core::AgentBehaviourRuntimeFailure::InstructionBudgetExceeded,
					"Coroutine instruction budget failure was misclassified");
			require(fixture.world.resumeSimulation() && fixture.world.advanceTick(),
				"Failed coroutine was resumed again instead of installing the default");
		}
		// Closing a suspended installation must not resume its body, and the
		// default installation must remain inert after unassignment.
		Fixture removed(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  wait()
  error('unassigned coroutine was resumed')
end end}
)lua");
		require(removed.world.advanceTick(), "Suspended teardown startup failed");
		removed.world.pauseSimulation();
		require(removed.world.clearAgentBehaviourAssignment(removed.agent),
			"Could not unassign suspended coroutine");
		require(removed.world.resumeSimulation() && removed.world.advanceTick()
			&& removed.world.getAgentBehaviourRuntimeDiagnostics().empty(),
			"Teardown resumed the coroutine or left a stale installation");

		core::AgentBehaviourRuntimeLimits limits;
		limits.callbacksPerBoundary = 1;
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  while true do wait() end
end end}
)lua", limits);
		// Exercise explicit boundary event delivery through the public adapter seam.
		core::AgentBehaviourRuntimeAdapter adapter(limits);
		require(adapter.runBoundary(fixture.world), "Coroutine initial resume failed");
		core::SimulationEvent event;
		event.type = core::SimulationEventType::DestinationReached;
		event.agent.id = fixture.agent;
		event.destinationMarker = fixture.destination;
		event.sequence = 1;
		adapter.observeOutcome(event);
		event.sequence = 2;
		adapter.observeOutcome(event);
		require(!adapter.runBoundary(fixture.world) && fixture.world.isSimulationPaused(),
			"Coroutine boundary resume cap was not enforced");
	}

	void coroutineSleepEvents(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  assert(ctx.set_timer == nil and ctx.cancel_timer == nil)
  assert(not pcall(sleep) and not pcall(sleep, 1, 2))
  for _, invalid in ipairs({0, -1, 1.5, '1', math.huge}) do
    assert(not pcall(sleep, invalid))
  end
  assert(ctx.move_to('Far').accepted)
  sleep(2000)
  ctx.log('sleep completed')
  local arrival = wait()
  assert(arrival.type == 'destination_reached' and arrival.tick < 2000)
  assert(arrival.sequence > 0 and arrival.destination ~= nil)
  assert(not pcall(function() arrival.type = 'changed' end))
  ctx.log('queued arrival delivered')
  while true do
    local event = wait()
    assert(event.type ~= 'destination_reached', 'event delivered twice')
  end
end end}
)lua");
		require(fixture.world.advanceTicks(2000), "Sleep journey failed");
		auto duringSleep = core::consumeLogMessages();
		require(std::none_of(duringSleep.begin(), duringSleep.end(), [](auto const& log)
			{ return log.msg == "sleep completed" || log.msg == "queued arrival delivered"; }),
			"Arrival interrupted pure-time sleep");
		require(fixture.world.advanceTick(), "Sleep completion failed");
		auto logs = core::consumeLogMessages();
		require(logs.size() == 2 && logs[0].msg == "sleep completed"
			&& logs[1].msg == "queued arrival delivered", "Queued event was not delivered after sleep");
		require(fixture.world.advanceTicks(10) && core::consumeLogMessages().empty(),
			"Sleep or event repeated");
	}

	void coroutineSleepFreezesWhenInactive(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  sleep(3)
  local activation = wait()
  assert(activation.type == 'activated' and activation.tick == 6)
  ctx.log('frozen sleep resumed')
end end}
)lua");
		require(fixture.world.advanceTick(), "Sleep startup failed");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, false), "Could not deactivate Agent");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(5), "Inactive sleep failed");
		require(core::consumeLogMessages().empty(), "Inactive sleep resumed");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, true), "Could not reactivate Agent");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(2), "Reactivation failed");
		require(core::consumeLogMessages().empty(), "Sleep counted inactive ticks");
		require(fixture.world.advanceTick(), "Frozen sleep completion failed");
		auto logs = core::consumeLogMessages();
		require(logs.size() == 1 && logs.front().msg == "frozen sleep resumed",
			"Sleep did not retain duration or queued activation");
	}

	void coroutineSleepEventOrdering(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  assert(sleep(2) == nil)
  local first = wait()
  assert(first.sequence == 10)
  -- Starting another sleep must retain the remainder of this boundary's queue.
  ctx.sleep(1)
  local second = ctx.wait()
  assert(second.sequence == 11)
  local third = wait()
  assert(third.sequence == 12)
  ctx.log('ordered sleep events')
  while true do wait() end
end end}
)lua");
		core::AgentBehaviourRuntimeAdapter adapter;
		require(adapter.runBoundary(fixture.world), "Could not start ordered sleep");
		core::SimulationEvent event;
		event.type = core::SimulationEventType::DestinationReached;
		event.agent.id = fixture.agent;
		event.destinationMarker = fixture.destination;
		// Insert out of order; runtime delivery is by World event sequence.
		event.sequence = 11; adapter.observeOutcome(event);
		event.sequence = 10; adapter.observeOutcome(event);
		require(adapter.runBoundary(fixture.world) && core::consumeLogMessages().empty(),
			"Events interrupted sleep");
		require(fixture.world.advanceTicks(2) && adapter.runBoundary(fixture.world), "Sleep drain failed");
		require(core::consumeLogMessages().empty(), "Second queued event interrupted nested sleep");
		event.sequence = 12;
		adapter.observeOutcome(event);
		require(adapter.runBoundary(fixture.world) && core::consumeLogMessages().empty(),
			"New event interrupted nested sleep");
		require(fixture.world.advanceTick() && adapter.runBoundary(fixture.world), "Nested sleep failed");
		auto logs = core::consumeLogMessages();
		require(std::count_if(logs.begin(), logs.end(), [](auto const& log)
			{ return log.msg == "ordered sleep events"; }) == 1,
			"Queued events were not delivered in order exactly once");
		require(adapter.runBoundary(fixture.world) && core::consumeLogMessages().empty(), "Queue repeated");
	}

	void coroutineSleepBudgets(smoke::Context const& context)
	{
		for (bool queueLimit : { false, true })
		{
			core::AgentBehaviourRuntimeLimits limits;
			if (queueLimit) limits.pendingEventsPerInstance = 1;
			else limits.callbacksPerBoundary = 1;
			Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  sleep(1)
  while true do wait() end
end end}
)lua", limits);
			core::AgentBehaviourRuntimeAdapter adapter(limits);
			require(adapter.runBoundary(fixture.world), "Budget sleep startup failed");
			core::SimulationEvent event;
			event.type = core::SimulationEventType::DestinationReached;
			event.agent.id = fixture.agent;
			event.destinationMarker = fixture.destination;
			event.sequence = 1; adapter.observeOutcome(event);
			event.sequence = 2; adapter.observeOutcome(event);
			if (!queueLimit) require(fixture.world.advanceTick(), "Could not advance sleep");
			require(!adapter.runBoundary(fixture.world) && fixture.world.isSimulationPaused(),
				"Sleep queue or completion bypassed budget");
			auto diagnostics = adapter.getDiagnostics();
			require(!diagnostics.empty() && diagnostics.front().callback == "resume"
				&& diagnostics.front().diagnostic.find(queueLimit ? "queue overflow" : "resume limit") != std::string::npos,
				"Sleep budget failure lacked structured diagnostic");
			require(adapter.isInstanceDisabled(fixture.agent), "Failed sleep retained behaviour");
		}
	}

	void coroutineQueuesAndActivation(smoke::Context const& context)
	{
		core::AgentBehaviourRuntimeLimits limits;
		limits.pendingEventsPerInstance = 2;
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  local first = wait()
  assert(first.type == 'activated')
  local a = ctx.wait()
  local b = wait()
  assert(a.sequence == 10 and b.sequence == 11)
end end}
)lua", limits);
		core::AgentBehaviourRuntimeAdapter adapter(limits);
		if (!adapter.runBoundary(fixture.world))
			throw std::runtime_error("Coroutine startup failed: " + adapter.getDiagnostics().front().diagnostic);
		core::SimulationEvent event;
		event.agent.id = fixture.agent;
		event.type = core::SimulationEventType::AgentDeactivated;
		adapter.observeActivation(event);
		event.type = core::SimulationEventType::DestinationReached;
		event.destinationMarker = fixture.destination;
		for (unsigned i = 0; i < 100; ++i) adapter.observeOutcome(event);
		require(adapter.runBoundary(fixture.world), "Deactivated coroutine accumulated events");
		event.type = core::SimulationEventType::AgentActivated;
		event.sequence = 9;
		adapter.observeActivation(event);
		require(adapter.runBoundary(fixture.world), "Reactivation did not deliver activation");
		event.type = core::SimulationEventType::DestinationReached;
		// Insertion order must not replace stable semantic event order.
		event.sequence = 11;
		adapter.observeOutcome(event);
		event.sequence = 10;
		adapter.observeOutcome(event);
		require(adapter.runBoundary(fixture.world) && adapter.getDiagnostics().empty(),
			"Coroutine pending events were not delivered once in sequence order");

		core::AgentBehaviourRuntimeAdapter overflowing(limits);
		require(overflowing.runBoundary(fixture.world), "Overflow fixture startup failed");
		for (unsigned i = 0; i < 3; ++i) overflowing.observeOutcome(event);
		require(!overflowing.runBoundary(fixture.world), "Coroutine queue overflow was not contained");
		require(overflowing.getDiagnostics().front().diagnostic.find("queue overflow") != std::string::npos,
			"Queue overflow lacked a diagnostic");
	}
}

void behaviour_smoke::registerRuntimeCoroutines(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "coroutineMovementAndCompletion", coroutineMovementAndCompletion });
	checks.push_back({ "coroutineFailuresAreContained", coroutineFailuresAreContained });
	checks.push_back({ "coroutineQueuesAndActivation", coroutineQueuesAndActivation });
	checks.push_back({ "coroutineSleepEvents", coroutineSleepEvents });
	checks.push_back({ "coroutineSleepFreezesWhenInactive", coroutineSleepFreezesWhenInactive });
	checks.push_back({ "coroutineSleepBudgets", coroutineSleepBudgets });
	checks.push_back({ "coroutineSleepEventOrdering", coroutineSleepEventOrdering });
}
