// Agent behaviour scheduling runtime checks (#289).
#include "Checks.h"
#include "RuntimeFixtures.h"

#include <filesystem>
#include <memory>
#include <sstream>
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

	std::string runDeterministicTimersAndSemanticState(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "timers.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "timers.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local fired = {}
    local function check_state(context, moving)
      local state = context.agent
      if state ~= context.state or type(state.identity) ~= "userdata"
          or state.name ~= configuration.expected_name
          or state.active ~= true or state.suspended ~= false
          or state.status ~= "active" or state.sector_name ~= "Room"
          or type(state.sector) ~= "userdata"
          or state.sector_identity ~= state.sector
          or state.sector_display_name ~= state.sector_name
          or type(state.global_position.x) ~= "number"
          or type(state.global_position.y) ~= "number"
          or state.position ~= state.global_position
          or state.tick ~= context.tick or state.simulation_tick ~= context.tick then
        error("semantic Agent state is incomplete")
      end
      if moving then
        if (state.movement_state ~= "moving" and state.movement_state ~= "idle")
            or state.movement ~= state.movement_state
            or state.destination ~= configuration.destination
            or state.destination_marker ~= configuration.destination then
          error("semantic movement state or destination is incorrect")
        end
      elseif state.movement_state ~= "idle" or state.destination ~= nil then
        error("initial semantic movement state is incorrect")
      end
      for _, prohibited in ipairs({ "path", "vertex", "traversal_resource",
          "request", "permit", "queue", "snapshot", "userdata" }) do
        if state[prohibited] ~= nil then error("exposed " .. prohibited) end
      end
      if pcall(function() state.name = "changed" end)
          or pcall(function() state.global_position.x = 0 end)
          or pcall(function() state.sector.value = 1 end) then
        error("semantic Agent state was mutable")
      end
    end
    return {
      on_start = function(context)
        check_state(context, false)
        if configuration.overflow then
          context.set_timer("one", 1)
          context.set_timer("two", 1)
          context.set_timer("three", 1)
          return
        end
        context.set_timer("cancelled", 1)
        local cancelled = context.cancel_timer("cancelled")
        local replaced = context.set_timer("z", 3)
        local first = context.set_timer("a", 1)
        local replacement = context.set_timer("z", 1)
        local missing = context.cancel_timer("missing")
        if replaced.status ~= "accepted" or first.status ~= "accepted"
            or replacement.status ~= "accepted" or missing.status ~= "no_op"
            or cancelled.status ~= "accepted" then
          error("timer command result was incorrect")
        end
      end,
      on_timer = function(name, context)
        fired[#fired + 1] = name .. ":" .. context.tick
        if #fired == 1 then
          if fired[1] ~= "a:1" then error("first timer was not lexical a:1") end
        elseif #fired == 2 then
          if fired[2] ~= "z:1" then error("replacement or lexical order failed") end
          context.set_timer("finish", 1)
        elseif #fired == 3 then
          if fired[3] ~= "finish:2" then error("one-tick timer did not fire after tick N+1") end
          local moved = context.move_to(configuration.destination)
          if moved.status ~= "accepted" then error(moved.status) end
          context.set_timer("inspect", 1)
        elseif #fired == 4 then
          if fired[4] ~= "inspect:3" then error("timer callback sequence changed") end
          check_state(context, true)
        else
          error("one-shot timer fired more than once")
        end
      end
    }
  end
}
)lua");
		auto const behaviour = registry->addAgentBehaviour("Timers", "timers.lua", {
			{ "expected_name", core::AgentBehaviourSchemaType::String },
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "overflow", core::AgentBehaviourSchemaType::Boolean }
		});
		writeRuntimeText(package / "timer-order.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function()
    local trace = ""
    return {
      on_start = function(context)
        context.set_timer("b", 1)
        context.set_timer("a", 1)
      end,
      on_timer = function(name)
        trace = trace .. name
        if name == "b" then
          if trace ~= "ab" then error("nonlexical:" .. trace) end
          error("ordered:" .. trace)
        end
      end
    }
  end
}
)lua");
		auto const orderingBehaviour = registry->addAgentBehaviour(
			"Timer order", "timer-order.lua", {});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded
			&& registry->lookupAgentBehaviour(orderingBehaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The deterministic timer fixtures did not preflight");

		core::World world("Timers", 16, 2,
			{ 64u * 1024u * 1024u, 100'000u, 2u });
		auto const room = world.addRoom("Room", 0, 0, 0, 16, 1);
		world.addSectorMarker(room, 0, 13.5f, "Destination");
		world.finishBuild();
		auto const overflow = world.createAgent("Overflow", room, 0, 0.5f);
		auto const first = world.createAgent("First", room, 0, 1.5f);
		auto const second = world.createAgent("Second", room, 0, 2.5f);
		auto const orderFirst = world.createAgent("Order first", room, 0, 3.5f);
		auto const orderSecond = world.createAgent("Order second", room, 0, 4.5f);
		auto const destination = world.getMarkerIds().front();
		world.pauseSimulation();
		world.consumeSimulationEvents();
		world.attachAgentBehaviourRegistry("timers.behaviours", registry);
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		auto assign = [&](core::AgentId id, std::string name, bool exceedsLimit)
		{
			require(world.setAgentBehaviourAssignment(id, behaviour, revision, {
				{ "expected_name", std::move(name) }, { "destination", destination },
				{ "overflow", exceedsLimit }
			}), "Could not assign deterministic timer fixture");
		};
		assign(overflow, "Overflow", true);
		assign(first, "First", false);
		assign(second, "Second", false);
		auto const orderingRevision = registry->lookupAgentBehaviour(
			orderingBehaviour)->getRevision();
		require(world.setAgentBehaviourAssignment(orderFirst, orderingBehaviour,
				orderingRevision, {})
			&& world.setAgentBehaviourAssignment(orderSecond, orderingBehaviour,
				orderingRevision, {}),
			"Could not assign callback-order timer fixtures");
		require(world.getAgentBehaviourRuntimeLimits().timersPerInstance == 2,
			"The configured per-instance timer limit was not retained");
		require(world.resumeSimulation(), "Could not resume timer fixture");
		world.consumeSimulationEvents();

		std::ostringstream digest;
		unsigned phaseEvents = 0;
		auto consume = [&]
		{
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phaseEvents;
				if (event.type == core::SimulationEventType::AgentChanged
					|| event.type == core::SimulationEventType::DestinationReached)
					digest << event.tick << ':' << event.sequence << ':'
						<< static_cast<unsigned>(event.type) << ':'
						<< event.agent.id.value << '|';
			}
		};
		// The overflowing startup batch is reported headlessly and pauses before
		// tick 1. Healthy instances retain their complete startup batches.
		require(!world.advanceTick() && world.isSimulationPaused(),
			"A timer storm did not stop the headless boundary visibly");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging the timer storm");
		consume();
		// Tick 1 first completes; its due timers run at the following boundary.
		require(world.advanceTick(),
			"Healthy startup batches did not permit tick 1 to complete");
		consume();
		// Both ordered timer failures occur at that boundary in Agent-ID order,
		// after healthy timer callbacks have completed atomically.
		require(!world.advanceTick() && world.isSimulationPaused(),
			"Ordered callback failures did not stop the headless boundary");
		require(world.resumeSimulation(),
			"Could not resume after acknowledging ordered callback failures");
		consume();
		for (unsigned tick = 0; tick < 4; ++tick)
		{
			require(world.advanceTick(),
				"A healthy timer callback unexpectedly stopped the run");
			consume();
		}
		auto diagnostics = world.consumeAgentBehaviourRuntimeDiagnostics();
		require(diagnostics.size() == 3 && diagnostics[0].agent == overflow
			&& diagnostics[0].callback == "on_start"
			&& diagnostics[0].diagnostic.find("timer limit of 2") != std::string::npos
			&& diagnostics[1].agent == orderFirst
			&& diagnostics[1].callback == "on_timer"
			&& diagnostics[1].diagnostic.find("ordered:ab") != std::string::npos
			&& diagnostics[2].agent == orderSecond
			&& diagnostics[2].callback == "on_timer"
			&& diagnostics[2].diagnostic.find("ordered:ab") != std::string::npos,
			"Timer names were not lexical, callbacks were not in Agent-ID order, or the timer limit had the wrong scope");
		require(!world.agentBehaviourOwnsMovement(overflow)
			&& world.agentBehaviourOwnsMovement(first)
			&& world.agentBehaviourOwnsMovement(second),
			"One instance's timer limit affected another instance");
		require(world.lookupAgent(first).entity->getState() == core::Agent::State::RoutePlanning
			&& world.lookupAgent(second).entity->getState() == core::Agent::State::RoutePlanning,
			"Lexically ordered one-shot timers did not apply their movement commands");

		unsigned reached = 0;
		for (unsigned tick = 0; tick < 2000 && reached < 2; ++tick)
		{
			world.advanceTick();
			for (auto const& event : world.consumeSimulationEvents())
			{
				if (event.type == core::SimulationEventType::PhaseCompleted) ++phaseEvents;
				if (event.type != core::SimulationEventType::DestinationReached) continue;
				++reached;
				digest << event.tick << ':' << event.sequence << ":reached:"
					<< event.agent.id.value << '|';
			}
		}
		require(reached == 2 && phaseEvents != 0,
			"Timer-driven Agents did not finish, or Lua consumed the public event queue");
		world.advanceTicks(5);
		consume();
		require(world.consumeAgentBehaviourRuntimeDiagnostics().empty(),
			"A one-shot timer repeated or semantic state changed unexpectedly");
		return digest.str();
	}

	void deterministicTimersExposeOnlySemanticState(smoke::Context const& context)
	{
		auto const first = runDeterministicTimersAndSemanticState(context);
		auto const second = runDeterministicTimersAndSemanticState(context);
		require(first == second,
			"Timer callbacks or independently consumable public events were nondeterministic");
	}

	void configuredSchedulesAndRandomStreamsReplay(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "schedule.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "schedule.lua", R"lua(
local host = require("promethium.v1")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local index = 1
    local reached = 0
    local function move(context)
      local result = context.move_to(configuration.schedule[index].destination)
      if not result.accepted then error(result.status) end
    end
    return {
      on_start = function(context)
        local integer = context.random_integer(-7, 11)
        local number = context.random_number()
        if type(integer) ~= "number" or integer < -7 or integer > 11
            or type(number) ~= "number" or number < 0 or number >= 1 then
          error("deterministic random operation returned an invalid range")
        end
        move(context)
      end,
      on_event = function(event, context)
        if event.type ~= "destination_reached" then return end
        reached = reached + 1
        local jitter = context.random_integer(0, 3)
        local sample = context.random_number()
        if sample < 0 or sample >= 1 then error("invalid random number") end
        context.set_timer("advance", configuration.schedule[index].duration + jitter)
      end,
      on_timer = function(name, context)
        if name ~= "advance" then error("unexpected schedule timer") end
        index = index == 1 and 2 or 1
        move(context)
      end
    }
  end
}
)lua");
		std::vector<core::AgentBehaviourSchemaField> entryFields{
			{ "duration", core::AgentBehaviourSchemaType::Duration },
			{ "destination", core::AgentBehaviourSchemaType::Marker }
		};
		auto const behaviour = registry->addAgentBehaviour("Schedule", "schedule.lua", {
			{ "schedule", core::AgentBehaviourSchemaType::List, {
				{ "entry", core::AgentBehaviourSchemaType::Record, entryFields }
			} }
		});
		require(registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"The composite schedule fixture did not preflight");

		struct ScheduleWorld
		{
			std::shared_ptr<core::World> world;
			core::AgentId first;
			core::AgentId second;
		};
		auto makeWorld = [&](bool extra, uint64_t seed = 0x1545eedu)
		{
			ScheduleWorld fixture;
			fixture.world = std::make_shared<core::World>("Schedules", 36, 2);
			auto const room = fixture.world->addRoom("Room", 0, 0, 0, 36, 1);
			fixture.world->addSectorMarker(room, 0, 4.5f, "Work");
			fixture.world->addSectorMarker(room, 0, 10.5f, "Lunch");
			fixture.world->addSectorMarker(room, 0, 17.5f, "Home");
			fixture.world->addSectorMarker(room, 0, 24.5f, "Gym");
			fixture.world->addSectorMarker(room, 0, 31.5f, "Park");
			fixture.world->finishBuild();
			fixture.first = fixture.world->createAgent("First", room, 0, 0.5f);
			fixture.second = fixture.world->createAgent("Second", room, 0, 1.5f);
			auto const third = extra
				? fixture.world->createAgent("Noisy", room, 0, 2.5f)
				: core::AgentId{};
			fixture.world->pauseSimulation();
			std::string diagnostic;
			require(fixture.world->setRandomSeed(seed, &diagnostic),
				"Could not author the World random seed: " + diagnostic);
			fixture.world->attachAgentBehaviourRegistry("schedule.behaviours", registry);
			auto const markers = fixture.world->getMarkerIds();
			auto schedule = [](core::MarkerId firstMarker, uint64_t firstDuration,
				core::MarkerId secondMarker, uint64_t secondDuration)
			{
				return core::AgentBehaviourConfiguration{ { "schedule",
					core::AgentBehaviourConfigurationList{
						core::AgentBehaviourConfigurationRecord{
							{ "duration", core::AgentBehaviourDuration{ firstDuration } },
							{ "destination", firstMarker } },
						core::AgentBehaviourConfigurationRecord{
							{ "duration", core::AgentBehaviourDuration{ secondDuration } },
							{ "destination", secondMarker } }
					} } };
			};
			auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
			require(fixture.world->setAgentBehaviourAssignment(fixture.first,
				behaviour, revision, schedule(markers[0], 2, markers[1], 3), &diagnostic)
				&& fixture.world->setAgentBehaviourAssignment(fixture.second,
					behaviour, revision, schedule(markers[2], 4, markers[3], 1), &diagnostic),
				"Could not assign distinct composite schedules: " + diagnostic);
			if (third)
				require(fixture.world->setAgentBehaviourAssignment(third,
					behaviour, revision, schedule(markers[4], 1, markers[4], 1), &diagnostic),
					"Could not assign the independent-stream noise Agent");
			return fixture;
		};

		auto run = [](ScheduleWorld const& fixture)
		{
			if (fixture.world->isSimulationPaused())
				require(fixture.world->resumeSimulation(),
					"Could not resume a schedule replay");
			fixture.world->consumeSimulationEvents();
			std::ostringstream digest;
			unsigned firstReached = 0, secondReached = 0;
			// Four journeys now include independently sampled initial planning time.
			for (unsigned tick = 0; tick < 6000
				&& (firstReached < 4 || secondReached < 4); ++tick)
			{
				if (tick == 10)
				{
					fixture.world->pauseSimulation();
					require(fixture.world->resumeSimulation(),
						"Pause/resume did not preserve schedule state");
				}
				fixture.world->advanceTick();
				for (auto const& event : fixture.world->consumeSimulationEvents())
				{
					if (event.type != core::SimulationEventType::DestinationReached
						|| (event.agent.id != fixture.first
							&& event.agent.id != fixture.second)) continue;
					if (event.agent.id == fixture.first)
					{
						if (firstReached >= 4) continue;
						++firstReached;
					}
					else
					{
						if (secondReached >= 4) continue;
						++secondReached;
					}
					digest << event.tick << ':' << event.agent.id.value << ':'
						<< event.destinationMarker.value << '|';
				}
			}
			auto diagnostics = fixture.world->consumeAgentBehaviourRuntimeDiagnostics();
			std::string detail = " (first=" + std::to_string(firstReached)
				+ ", second=" + std::to_string(secondReached) + ")";
			if (!diagnostics.empty()) detail += ": " + diagnostics.front().diagnostic;
			require(firstReached == 4 && secondReached == 4,
				"The shared schedule module did not advance both private schedules" + detail);
			require(diagnostics.empty(),
				"A configured schedule or random operation failed" + detail);
			return digest.str();
		};

		auto fixture = makeWorld(false);
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData writeWork;
		writeWork.markSerializedUnmodified = false;
		fixture.world->serialize(*writer, writeWork);
		writer->serialize();
		auto const authoredYaml = writer->getSerializedString();
		require(authoredYaml.find("randomSeed: 22306541") != std::string::npos,
			"The authored World random seed was not persisted");

		auto const first = run(fixture);
		fixture.world->resetSimulation();
		require(fixture.world->getRandomSeed() == 0x1545eedu,
			"Simulation reset lost the authored World random seed");
		auto const afterReset = run(fixture);
		require(first == afterReset,
			"Simulation reset did not recreate schedule state and random streams");

		auto reopened = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(authoredYaml);
		reader->deserialize();
		core::SerializationWorkData readWork;
		require(reopened->deserialize(*reader, readWork),
			"The authored schedule World did not reload");
		reopened->resolveAgentBehaviourRegistry(registry);
		ScheduleWorld loaded{ reopened, fixture.first, fixture.second };
		require(run(loaded) == first,
			"Save/load did not reproduce configured schedule outcomes");

		auto noisy = makeWorld(true);
		require(run(noisy) == first,
			"Another Agent's callbacks altered an independent random stream");
		auto differentSeed = makeWorld(false, 0x1545eedu + 1u);
		require(run(differentSeed) != first,
			"The authored World seed did not affect deterministic random streams");
	}
}

void behaviour_smoke::registerRuntimeScheduling(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "deterministicTimersExposeOnlySemanticState", [](smoke::Context const& context)
	{
		deterministicTimersExposeOnlySemanticState(context);
	} });
	checks.push_back({ "configuredSchedulesAndRandomStreamsReplay", [](smoke::Context const& context)
	{
		configuredSchedulesAndRandomStreamsReplay(context);
	} });
}
