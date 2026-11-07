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

	void deterministicSleepsExposeOnlySemanticState(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "sleep.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "sleep.lua", R"lua(
return { api_version=3, factory=function(configuration) return function(ctx)
  assert(ctx.agent == ctx.state and ctx.agent.tick == ctx.tick)
  local state = ctx.agent
  assert(state.name == configuration.expected_name and type(state.identity) == 'userdata')
  assert(state.active and not state.suspended and state.status == 'active')
  assert(state.sector_name == 'Room' and type(state.sector) == 'userdata')
  assert(state.sector_identity == state.sector and state.sector_display_name == state.sector_name)
  assert(state.position == state.global_position and type(state.position.x) == 'number'
    and type(state.position.y) == 'number' and state.simulation_tick == ctx.tick)
  assert(state.movement == 'idle' and state.movement_state == 'idle' and state.destination == nil)
  for _, prohibited in ipairs({'path', 'vertex', 'traversal_resource', 'request',
      'permit', 'queue', 'snapshot', 'userdata'}) do
    assert(state[prohibited] == nil)
  end
  assert(not pcall(function() state.name = 'changed' end))
  assert(not pcall(function() state.position.x = 0 end))
  assert(not pcall(function() state.sector.value = 1 end))
  assert(ctx.set_timer == nil and ctx.cancel_timer == nil)
  sleep(1)
  ctx.sleep(1)
  assert(ctx.move_to(configuration.destination).accepted)
  local event = wait()
  assert(event.type == 'destination_reached' and event.tick > ctx.tick)
  assert(state.movement_state == 'idle' and state.destination == nil) -- retained startup snapshot
end end }
)lua");
		auto behaviour = registry->addAgentBehaviour("Sleep", "sleep.lua", {
			{ "expected_name", core::AgentBehaviourSchemaType::String },
			{ "destination", core::AgentBehaviourSchemaType::Marker } });
		auto run = [&]()
		{
			core::World world("Sleeps", 16, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 16, 1);
			world.addSectorMarker(room, 0, 13.5f, "Destination");
			world.finishBuild();
			auto agent = world.createAgent("First", room, 0, 1.5f);
			world.pauseSimulation();
			world.attachAgentBehaviourRegistry("sleep.behaviours", registry);
			require(world.setAgentBehaviourAssignment(agent, behaviour,
				registry->lookupAgentBehaviour(behaviour)->getRevision(), {
					{ "expected_name", std::string("First") },
					{ "destination", world.getMarkerIds().front() } }), "Could not assign sleep fixture");
			require(world.resumeSimulation(), "Could not resume sleep fixture");
			std::ostringstream digest;
			unsigned reached = 0, phases = 0;
			for (unsigned i = 0; i < 2000; ++i)
			{
				require(world.advanceTick(), "Sleep fixture failed");
				for (auto const& event : world.consumeSimulationEvents())
				{
					if (event.type == core::SimulationEventType::PhaseCompleted) ++phases;
					if (event.type == core::SimulationEventType::DestinationReached)
					{
						++reached;
						digest << event.tick << ':' << event.sequence << ':' << event.agent.id.value;
					}
				}
			}
			require(reached == 1 && phases != 0 && !world.agentBehaviourOwnsMovement(agent)
				&& world.getAgentBehaviourRuntimeDiagnostics().empty(),
				"Sleep did not complete one journey or exposed mutable state");
			return digest.str();
		};
		require(run() == run(), "Sleep scheduling did not replay deterministically");
	}

	void configuredSchedulesAndRandomStreamsReplay(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "schedule.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeRuntimeText(package / "schedule.lua", R"lua(
local host = require("promethium.v3")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local index = 1
    local reached = 0
    local function move(context)
      local result = context.move_to(configuration.schedule[index].destination)
      if not result.accepted then error(result.status) end
    end
    return function(context)
      local integer = context.random_integer(-7, 11)
      local number = context.random_number()
      if type(integer) ~= "number" or integer < -7 or integer > 11
          or type(number) ~= "number" or number < 0 or number >= 1 then
        error("deterministic random operation returned an invalid range")
      end
      move(context)
      while true do
        local event = wait()
        if event.type == "destination_reached" then
          reached = reached + 1
          local jitter = context.random_integer(0, 3)
          local sample = context.random_number()
          if sample < 0 or sample >= 1 then error("invalid random number") end
          sleep(configuration.schedule[index].duration + jitter)
          index = index == 1 and 2 or 1
          move(context)
        end
        ::next_event::
      end
    end
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
	checks.push_back({ "deterministicSleepsExposeOnlySemanticState", [](smoke::Context const& context)
	{
		deterministicSleepsExposeOnlySemanticState(context);
	} });
	checks.push_back({ "configuredSchedulesAndRandomStreamsReplay", [](smoke::Context const& context)
	{
		configuredSchedulesAndRandomStreamsReplay(context);
	} });
}
