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

	void coroutineTimerEvents(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  assert(ctx.set_timer('cancelled', 1).accepted)
  assert(ctx.cancel_timer('cancelled').accepted)
  assert(ctx.cancel_timer('missing').status == 'no_op')
  assert(ctx.set_timer('z', 10).accepted)
  assert(ctx.set_timer('z', 1).accepted)
  assert(ctx.set_timer('a', 1).accepted)
  local a = wait()
  assert(a.type == 'timer_expired' and a.name == 'a' and a.tick == 1)
  assert(a.destination == nil and a.action == nil and a.result == nil)
  assert(type(a.sequence) == 'number')
  assert(not pcall(function() a.name = 'changed' end))
  -- Due timers are an immutable batch: cancelling z cannot retract its event.
  assert(ctx.cancel_timer('z').status == 'no_op')
  assert(ctx.set_timer('a', 1).accepted)
  local z = ctx.wait()
  assert(z.type == 'timer_expired' and z.name == 'z' and z.tick == 1)
  assert(z.sequence > a.sequence)
  local again = wait()
  assert(again.type == 'timer_expired' and again.name == 'a' and again.tick == 2)
  assert(again.sequence > z.sequence)
  assert(ctx.move_to('Far').accepted)
  local arrival = wait()
  assert(arrival.type == 'destination_reached' and arrival.sequence > again.sequence)
  ctx.log('timer journey complete')
  while true do
    local event = wait()
    assert(event.type ~= 'timer_expired', 'one-shot timer repeated')
  end
end end}
)lua");
		auto const advanced = fixture.world.advanceTicks(1600);
		require(advanced, "Coroutine timer journey failed: "
			+ (fixture.world.getAgentBehaviourRuntimeDiagnostics().empty() ? std::string("no diagnostic")
				: fixture.world.getAgentBehaviourRuntimeDiagnostics().front().diagnostic));
		auto logs = core::consumeLogMessages();
		require(std::count_if(logs.begin(), logs.end(), [](auto const& log)
			{ return log.msg == "timer journey complete"; }) == 1,
			"Named timer expiry did not resume the coroutine through arrival");
		require(fixture.world.getAgentBehaviourRuntimeDiagnostics().empty(),
			"Coroutine timer payload, ordering or one-shot semantics failed");
	}

	void coroutineTimersFreezeWhenInactive(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  assert(ctx.set_timer('frozen', 3).accepted)
  local activation = wait()
  assert(activation.type == 'activated' and activation.tick == 6)
  local timer = wait()
  assert(timer.type == 'timer_expired' and timer.name == 'frozen' and timer.tick == 8)
  assert(timer.sequence > activation.sequence)
  ctx.log('frozen timer resumed')
end end}
)lua");
		require(fixture.world.advanceTick(), "Frozen timer startup failed");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, false), "Could not deactivate timer Agent");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(5),
			"Inactive coroutine timer failed");
		require(core::consumeLogMessages().empty(),
			"Inactive timer resumed the coroutine");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, true), "Could not reactivate timer Agent");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(2),
			"Reactivated coroutine failed");
		require(core::consumeLogMessages().empty(),
			"Timer counted inactive ticks");
		require(fixture.world.advanceTick(), "Frozen timer expiry failed");
		auto logs = core::consumeLogMessages();
		require(logs.size() == 1 && logs.front().msg == "frozen timer resumed",
			"Timer did not preserve its remaining duration on reactivation");
	}

	void coroutineOutcomePrecedesTimer(smoke::Context const& context)
	{
		Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  ctx.set_timer('cancel-on-activation', 1)
  local activation = wait()
  assert(activation.type == 'activated' and activation.tick == 4)
  assert(ctx.cancel_timer('cancel-on-activation').accepted)
  ctx.set_timer('finish', 1)
  local timer = wait()
  assert(timer.type == 'timer_expired' and timer.name == 'finish' and timer.tick == 5)
  assert(timer.sequence > activation.sequence)
end end}
)lua");
		require(fixture.world.advanceTick(), "Outcome-order timer startup failed");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, false), "Could not suspend due timer");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(3),
			"Suspended due timer failed");
		fixture.world.pauseSimulation();
		require(fixture.world.setAgentActive(fixture.agent, true), "Could not reactivate due timer");
		require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(3)
			&& !fixture.world.agentBehaviourOwnsMovement(fixture.agent)
			&& fixture.world.getAgentBehaviourRuntimeDiagnostics().empty(),
			"Semantic outcome could not cancel a timer due at the same boundary");
	}

	void coroutineTimerBudgets(smoke::Context const& context)
	{
		for (bool queueLimit : { false, true })
		{
			core::AgentBehaviourRuntimeLimits limits;
			if (queueLimit) limits.pendingEventsPerInstance = 1;
			else limits.callbacksPerBoundary = 1;
			Fixture fixture(context, R"lua(
return {api_version=3, factory=function() return function(ctx)
  ctx.set_timer('a', 1)
  ctx.set_timer('b', 1)
  while true do wait() end
end end}
)lua", limits);
			require(fixture.world.advanceTick(), "Timer budget startup failed");
			require(!fixture.world.advanceTick() && fixture.world.isSimulationPaused(),
				"Timer expiry bypassed the coroutine queue or resume budget");
			auto diagnostics = fixture.world.getAgentBehaviourRuntimeDiagnostics();
			require(!diagnostics.empty() && diagnostics.front().callback == "resume"
				&& (!queueLimit || diagnostics.front().diagnostic.find("queue overflow") != std::string::npos),
				"Timer budget failure lacked a structured coroutine diagnostic");
			require(fixture.world.resumeSimulation() && fixture.world.advanceTicks(3),
				"Failed timer coroutine was not replaced by the default");
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
	checks.push_back({ "coroutineTimerEvents", coroutineTimerEvents });
	checks.push_back({ "coroutineTimersFreezeWhenInactive", coroutineTimersFreezeWhenInactive });
	checks.push_back({ "coroutineTimerBudgets", coroutineTimerBudgets });
	checks.push_back({ "coroutineOutcomePrecedesTimer", coroutineOutcomePrecedesTimer });
}
