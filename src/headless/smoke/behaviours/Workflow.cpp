#include "Checks.h"
#include "TemporaryDirectory.h"
// End-to-end deterministic Agent behaviour workflow verification for #164.

#include <bit>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"


namespace
{
	using behaviour_smoke::TemporaryDirectory;
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(text.data(), static_cast<std::streamsize>(text.size()));
		if (!output) throw std::runtime_error("Could not write workflow fixture");
	}

	std::vector<core::AgentBehaviourSchemaField> scheduleSchema()
	{
		std::vector<core::AgentBehaviourSchemaField> entry{
			{ "duration", core::AgentBehaviourSchemaType::Duration },
			{ "destination", core::AgentBehaviourSchemaType::Marker }
		};
		return {
			{ "code", core::AgentBehaviourSchemaType::String },
			{ "start_delay", core::AgentBehaviourSchemaType::Duration },
			{ "fallback", core::AgentBehaviourSchemaType::Marker },
			{ "fail_on_interaction", core::AgentBehaviourSchemaType::Boolean },
			{ "schedule", core::AgentBehaviourSchemaType::List, {
				{ "entry", core::AgentBehaviourSchemaType::Record, std::move(entry) }
			} }
		};
	}

	std::string const ScheduleSource = R"lua(
local host = require("promethium.v3")
return {
  api_version = host.api_version,
  factory = function(configuration)
    local index = 1
    local function trace_resume(context, value, tick)
      context.log("CB:" .. configuration.code .. ":" .. value .. ":" .. (tick or context.tick))
    end
    local function move(context, destination, label, tick)
      local result = context.move_to(destination)
      context.log("CMD:" .. configuration.code .. ":" .. label .. ":" .. result.status .. ":" .. (tick or context.tick))
      if not result.accepted then error("move " .. label .. ":" .. result.status) end
    end
    return function(context)
      trace_resume(context, "start")
      context.set_timer("depart", configuration.start_delay)
      while true do
        local event = wait()
        if event.type == "timer_expired" then
          local name = event.name
          trace_resume(context, "timer-" .. name, event.tick)
          if name == "depart" then
            move(context, configuration.schedule[index].destination, "depart", event.tick)
          elseif name == "cancel" then
            local result = context.cancel_movement()
            context.log("CMD:" .. configuration.code .. ":cancel:" .. result.status .. ":" .. event.tick)
            if not result.accepted then error("cancel:" .. result.status) end
          else
            error("unexpected timer " .. name)
          end
        elseif event.type == "route_lost" then
          local destination, reason, outcome = event.destination, event.reason, event
          trace_resume(context, "route-" .. reason, event.tick)
          move(context, configuration.fallback, "route-fallback", event.tick)
        elseif event.type ~= "timer_expired" and event.type ~= "route_lost" then
          trace_resume(context, "event-" .. event.type, event.tick)
          if event.type == "destination_reached" and index < 2 then
            index = index + 1
            move(context, configuration.schedule[index].destination, "next", event.tick)
            context.set_timer("cancel", configuration.schedule[index].duration)
          elseif event.type == "movement_cancelled" then
            move(context, configuration.fallback, "after-cancel", event.tick)
          elseif event.type == "interaction_failed" and configuration.fail_on_interaction then
            error("expected interaction failure for " .. configuration.code)
          end
        end
      end
    end
  end
}
)lua";

	core::AgentBehaviourConfiguration scheduleConfiguration(std::string code,
		uint64_t delay, core::MarkerId fallback, bool failOnInteraction,
		std::vector<std::pair<core::MarkerId, uint64_t>> const& entries)
	{
		core::AgentBehaviourConfigurationList schedule;
		for (auto const& [destination, duration] : entries)
			schedule.emplace_back(core::AgentBehaviourConfigurationRecord{
				{ "duration", core::AgentBehaviourDuration{ duration } },
				{ "destination", destination }
			});
		return {
			{ "code", std::move(code) },
			{ "start_delay", core::AgentBehaviourDuration{ delay } },
			{ "fallback", fallback },
			{ "fail_on_interaction", failOnInteraction },
			{ "schedule", std::move(schedule) }
		};
	}

	struct WorkflowDigest
	{
		std::string snapshots;
		std::string events;
		std::string diagnostics;
		std::string callbacks;
		std::string commands;

		bool operator==(WorkflowDigest const&) const = default;
	};

	struct WorkflowObservations
	{
		bool deactivated{ false };
		bool activated{ false };
		bool routeLost{ false };
		bool cancelled{ false };
		bool reached{ false };
		bool interactionCompleted{ false };
		bool interactionFailed{ false };
	};

	void appendSnapshot(std::ostringstream& output,
		core::SimulationSnapshot const& snapshot)
	{
		output << snapshot.tick << ':' << snapshot.paused << ':'
			<< snapshot.topologyGeneration << '[';
		for (auto const& agent : snapshot.agents)
			output << agent.id.value << ',' << agent.sectorId.value << ','
				<< std::bit_cast<uint32_t>(agent.globalPosition.x) << ','
				<< std::bit_cast<uint32_t>(agent.globalPosition.y) << ','
				<< static_cast<unsigned>(agent.state) << ',' << agent.active << ','
				<< agent.hasPath << ',' << agent.targetPathNode << ','
				<< agent.pathNodeCount << ';';
		output << "]{";
		for (auto const& request : snapshot.interactionRequests)
			output << request.id.value << ',' << request.actor.value << ','
				<< static_cast<unsigned>(request.result) << ';';
		output << "}|";
	}

	void appendEvents(std::ostringstream& output,
		std::vector<core::SimulationEvent> const& events,
		WorkflowObservations& observed)
	{
		for (auto const& event : events)
		{
			bool semantic = false;
			switch (event.type)
			{
			case core::SimulationEventType::DestinationReached:
				observed.reached = semantic = true; break;
			case core::SimulationEventType::MovementCancelled:
				observed.cancelled = semantic = true; break;
			case core::SimulationEventType::RouteLost:
				observed.routeLost = semantic = true; break;
			case core::SimulationEventType::AgentActivated:
				observed.activated = semantic = true; break;
			case core::SimulationEventType::AgentDeactivated:
				observed.deactivated = semantic = true; break;
			case core::SimulationEventType::InteractionRequestChanged:
				if (event.interactionRequest.result != core::InteractionResult::Pending)
				{
					semantic = true;
					if (event.interactionRequest.result == core::InteractionResult::Succeeded
						|| event.interactionRequest.result
							== core::InteractionResult::SucceededWithBestEffortFailure)
						observed.interactionCompleted = true;
					else observed.interactionFailed = true;
				}
				break;
			default: break;
			}
			if (!semantic) continue;
			output << event.sequence << ':' << event.tick << ':'
				<< static_cast<unsigned>(event.type) << ':' << event.agent.id.value
				<< ':' << event.destinationMarker.value << ':'
				<< static_cast<unsigned>(event.routeLossReason) << ':'
				<< event.interactionRequest.id.value << ':'
				<< static_cast<unsigned>(event.interactionRequest.result) << '|';
		}
	}

	void appendDiagnostics(std::ostringstream& output,
		std::vector<core::AgentBehaviourRuntimeDiagnostic> const& diagnostics)
	{
		for (auto const& item : diagnostics)
			output << static_cast<unsigned>(item.failure) << ':'
				<< static_cast<unsigned>(item.stage) << ':' << item.agent.value << ':'
				<< item.behaviour.value << ':' << item.tick << ':' << item.callback
				<< ':' << item.diagnostic << ':' << item.traceback << '|';
	}

	void appendLogs(std::ostringstream& callbacks, std::ostringstream& commands)
	{
		for (auto const& message : core::consumeLogMessages())
		{
			if (message.msg.starts_with("CB:")) callbacks << message.msg << '|';
			if (message.msg.starts_with("CMD:")) commands << message.msg << '|';
		}
	}

	WorkflowDigest runCompleteWorkflow(smoke::Context const& context)
	{
		(void)core::consumeLogMessages();
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "shared.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "schedule.lua", ScheduleSource);
		auto const behaviour = registry->addAgentBehaviour(
			"Shared schedule", "schedule.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());

		auto world = std::make_shared<core::World>("Complete workflow", 40, 2);
		auto const mainRoom = world->addRoom("Main", 0, 0, 0, 40, 1);
		auto const isolatedRoom = world->addRoom("Isolated", 1, 0, 0, 10, 1);
		world->addSectorMarker(mainRoom, 0, 3.5f, "Fallback");
		world->addSectorMarker(mainRoom, 0, 8.5f, "Work");
		world->addSectorMarker(mainRoom, 0, 18.5f, "Lunch");
		world->addSectorMarker(mainRoom, 0, 30.5f, "Home");
		world->addSectorMarker(isolatedRoom, 0, 4.5f, "Isolated destination");
		auto const sector = core::SectorId{ static_cast<uint64_t>(mainRoom) + 1 };
		core::InteractionBinding binding{
			{ core::DeviceCommandType::SetSectorLights, sector, true },
			core::InteractionBindingRequirement::Required };
		auto const working = world->createInteractionPoint("Working control", sector,
			{ 0.5f, 0.0f }, 100.0f, 0.0f, { binding });
		auto const broken = world->createInteractionPoint("Broken control", sector,
			{ 0.5f, 0.0f }, 100.0f, 0.0f, { binding });
		world->finishBuild();
		auto const first = world->createAgent("First", mainRoom, 0, 0.5f);
		auto const second = world->createAgent("Second", mainRoom, 0, 1.5f);
		auto const third = world->createAgent("Third", mainRoom, 0, 2.5f);
		world->pauseSimulation();
		world->attachAgentBehaviourRegistry("shared.behaviours", registry);
		auto const markers = world->getMarkerIds();
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(first, behaviour, revision,
			scheduleConfiguration("A", 3, markers[0], false,
				{ { markers[1], 1 }, { markers[3], 3 } }), &diagnostic)
			&& world->setAgentBehaviourAssignment(second, behaviour, revision,
				scheduleConfiguration("B", 1, markers[0], false,
					{ { markers[4], 1 }, { markers[2], 2 } }), &diagnostic)
			&& world->setAgentBehaviourAssignment(third, behaviour, revision,
				scheduleConfiguration("C", 1, markers[0], true,
					{ { markers[2], 1 }, { markers[3], 2 } }), &diagnostic),
			"Could not assign the independently configured shared schedules: " + diagnostic);
		require(world->resumeSimulation(), "Could not start the complete workflow");
		world->consumeSimulationEvents();

		WorkflowObservations observed;
		std::ostringstream snapshots, events, diagnostics, callbacks, commands;
		auto advance = [&]
		{
			auto const advanced = world->advanceTick();
			appendSnapshot(snapshots, world->getSimulationSnapshot());
			appendEvents(events, world->consumeSimulationEvents(), observed);
			appendDiagnostics(diagnostics,
				world->consumeAgentBehaviourRuntimeDiagnostics());
			appendLogs(callbacks, commands);
			return advanced;
		};

		require(advance(), "The startup boundary failed");
		world->pauseSimulation();
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->setAgentActive(first, false),
			"Could not deactivate the timer-driven Agent");
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->resumeSimulation(), "Could not resume with a suspended Agent");
		world->consumeSimulationEvents();
		for (unsigned tick = 0; tick < 4; ++tick)
			require(advance(), "A suspended timer stopped the workflow");
		world->pauseSimulation();
		world->consumeSimulationEvents();
		require(world->setAgentActive(first, true),
			"Could not reactivate the timer-driven Agent");
		appendEvents(events, world->consumeSimulationEvents(), observed);
		require(world->resumeSimulation(), "Could not resume the reactivated Agent");
		world->consumeSimulationEvents();

		for (unsigned tick = 0; tick < 2400
			&& (!observed.routeLost || !observed.cancelled || !observed.reached); ++tick)
		{
			if (tick == 12)
			{
				world->pauseSimulation();
				require(world->resumeSimulation(),
					"Ordinary pause/resume did not preserve the schedules");
				world->consumeSimulationEvents();
			}
			auto const advanced = advance();
			require(advanced, "A healthy schedule boundary stopped unexpectedly at loop "
				+ std::to_string(tick) + ", simulation tick "
				+ std::to_string(world->getSimulationTick()) + ", paused="
				+ std::to_string(world->isSimulationPaused()) + ": diagnostics="
				+ diagnostics.str());
		}
		require(observed.deactivated && observed.activated && observed.routeLost
			&& observed.cancelled && observed.reached,
			"The shared schedules did not cover activation, route loss, cancellation, and destinations");
		// Require several idle boundaries so a just-published destination event
		// has time to launch its configured second leg before we call it settled.
		unsigned idleBoundaries = 0;
		for (unsigned tick = 0; tick < 10'000 && idleBoundaries < 3; ++tick)
		{
			bool allIdle = true;
			for (auto const& item : world->getSimulationSnapshot().agents)
				allIdle = allIdle && !item.hasPath
					&& item.state == core::AgentPathState::Idle;
			idleBoundaries = allIdle ? idleBoundaries + 1 : 0;
			if (idleBoundaries < 3)
				require(advance(), "The schedules did not settle before interactions");
		}
		for (auto const& item : world->getSimulationSnapshot().agents)
			require(!item.hasPath && item.state == core::AgentPathState::Idle,
				"Scheduled Agent " + std::to_string(item.id.value)
					+ " was still moving when interaction coverage began (state "
					+ std::to_string(static_cast<unsigned>(item.state)) + ", x "
					+ std::to_string(item.globalPosition.x) + ", hasPath "
					+ std::to_string(item.hasPath) + ")");

		auto completeInteraction = [&](core::InteractionPointId point,
			core::AgentId agent, core::DeviceOperationState result)
		{
			auto const request = world->requestInteraction(point, agent);
			auto lookup = world->lookupInteractionRequest(request);
			auto actor = world->lookupAgent(agent);
			require(lookup && !lookup.entity->getOperations().empty(),
				"Could not create workflow interaction point "
					+ std::to_string(point.value) + " for Agent "
					+ std::to_string(agent.value) + " (request "
					+ std::to_string(request.value) + ", state "
					+ std::to_string(actor ? static_cast<unsigned>(actor.entity->getState()) : 99u)
					+ ", sector " + std::to_string(actor
						? actor.entity->getSector()->getIndex() : 999u) + ")");
			auto const operation = lookup.entity->getOperations().front().first;
			world->lookupDeviceOperation(operation).entity->setState(result);
		};
		completeInteraction(working, second, core::DeviceOperationState::Succeeded);
		require(advance() && advance(), "The successful interaction stopped the workflow");
		completeInteraction(broken, third, core::DeviceOperationState::Failed);
		require(advance(), "Publishing the failed interaction stopped too early");
		require(!advance() && world->isSimulationPaused(),
			"The expected callback diagnostic did not stop before the next tick");
		require(observed.interactionCompleted && observed.interactionFailed,
			"Public interaction outcomes were consumed or omitted by Lua");
		require(diagnostics.str().find("expected interaction failure for C")
				!= std::string::npos
			&& !world->agentBehaviourOwnsMovement(third)
			&& world->agentBehaviourOwnsMovement(first)
			&& world->agentBehaviourOwnsMovement(second),
			"The expected diagnostic lacked stable detail or failure isolation");
		require(world->resumeSimulation() && advance(),
			"The healthy instances could not resume after failure acknowledgement");

		world->pauseSimulation();
		world->resetSimulation();
		require(world->getSimulationTick() == 0
			&& world->getAgentBehaviourAssignmentCount() == 3,
			"Reset did not replace runtime state while retaining authored schedules");
		require(world->resumeSimulation() && advance(),
			"Reset schedules did not recreate their instances");
		require(callbacks.str().find("CB:C:start:0") != std::string::npos,
			"Reset did not recreate a previously failed instance");

		world->pauseSimulation();
		std::vector<core::AgentBehaviourReloadDiagnostic> reloadDiagnostics;
		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
			&diagnostic, &reloadDiagnostics) && reloadDiagnostics.empty(),
			"An unchanged external source reload was not atomic: " + diagnostic);
		require(world->resumeSimulation() && advance(),
			"Reloaded schedules did not recreate and resume");

		require(callbacks.str().find("CB:A:event-deactivated") == std::string::npos
			&& callbacks.str().find("CB:A:event-activated") != std::string::npos
			&& callbacks.str().find("CB:B:route-unreachable") != std::string::npos
			&& commands.str().find("CMD:A:cancel:accepted") != std::string::npos
			&& commands.str().find("CMD:B:route-fallback:accepted") != std::string::npos,
			"Callback or command-order observations omitted a required workflow stage");

		return { snapshots.str(), events.str(), diagnostics.str(),
			callbacks.str(), commands.str() };
	}

	void completeWorkflowReplaysIdentically(smoke::Context const& context)
	{
		auto const first = runCompleteWorkflow(context);
		auto const second = runCompleteWorkflow(context);
		require(first == second,
			"Repeated fixed-tick workflow runs changed a snapshot, event, diagnostic, callback, or command digest");
	}
}

void behaviour_smoke::registerWorkflow(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "completeWorkflowReplaysIdentically", [](smoke::Context const& context)
	{
		completeWorkflowReplaysIdentically(context);
	} });
}
