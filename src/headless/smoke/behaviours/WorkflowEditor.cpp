#include "Checks.h"
#include "TemporaryDirectory.h"
#include "EditorState.h"
#include "ImGuiContext.h"
// End-to-end deterministic Agent behaviour workflow verification for #164.

#include <bit>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "AgentBehaviourAssignmentPanel.h"
#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "MarkerPanel.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"


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

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml,
		std::shared_ptr<core::AgentBehaviourRegistry> const& registry = {})
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work),
			"A workflow World document did not deserialize");
		if (registry && world->hasAgentBehaviourRegistryReference())
			world->resolveAgentBehaviourRegistry(registry);
		return world;
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
      sleep(configuration.start_delay)
      local elapsed = context.tick + configuration.start_delay
      trace_resume(context, "sleep-depart", elapsed)
      move(context, configuration.schedule[index].destination, "depart", elapsed)
      while true do
        local event = wait()
        if event.type == "route_lost" then
          local destination, reason, outcome = event.destination, event.reason, event
          trace_resume(context, "route-" .. reason, event.tick)
          move(context, configuration.fallback, "route-fallback", event.tick)
        else
          trace_resume(context, "event-" .. event.type, event.tick)
          if event.type == "destination_reached" and index < 2 then
            index = index + 1
            move(context, configuration.schedule[index].destination, "next", event.tick)
            sleep(configuration.schedule[index].duration)
            local tick = event.tick + configuration.schedule[index].duration
            trace_resume(context, "sleep-cancel", tick)
            local result = context.cancel_movement()
            context.log("CMD:" .. configuration.code .. ":cancel:" .. result.status .. ":" .. tick)
            if not result.accepted then error("cancel:" .. result.status) end
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

	void persistenceHistoryMigrationAndReplacementAreAtomic(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "documents.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "schedule.lua", ScheduleSource);
		auto const behaviour = registry->addAgentBehaviour(
			"Schedule", "schedule.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());
		auto registryRoundTrip = core::AgentBehaviourRegistry::loadFrom(
			(package / "behaviours.yaml").string());
		require(registryRoundTrip->getUuid() == registry->getUuid()
			&& registryRoundTrip->getBehaviourCount() == 1,
			"The version-1 registry did not round-trip");

		auto world = std::make_shared<core::World>("Documents", 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->addSectorMarker(room, 0, 2.5f, "Alpha");
		world->addSectorMarker(room, 0, 9.5f, "Beta");
		world->finishBuild();
		auto const agent = world->createAgent("Assigned", room, 0, 0.5f);
		world->pauseSimulation();
		world->attachAgentBehaviourRegistry("documents.behaviours", registry);
		auto const markers = world->getMarkerIds();
		auto const revision = registry->lookupAgentBehaviour(behaviour)->getRevision();
		auto configuration = scheduleConfiguration("D", 1, markers[0], false,
			{ { markers[0], 1 }, { markers[1], 2 } });
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour, revision,
			configuration, &diagnostic), "Could not author the document fixture");
		auto const currentYaml = serializeWorld(*world);
		require(currentYaml.find("version: 64") != std::string::npos,
			"The current World schema did not include the version-11 Marker data lineage");
		auto currentRoundTrip = deserializeWorld(currentYaml, registry);
		currentRoundTrip->pauseSimulation();
		require(currentRoundTrip->getAgentBehaviourAssignment(agent)
			== world->getAgentBehaviourAssignment(agent)
			&& currentRoundTrip->lookupMarker(markers[0])->getName() == "Alpha",
			"The complete World assignment and Marker state did not round-trip");

		// Version 11 is the Marker identity boundary. A document without later
		// behaviour fields still retains exact Marker identities and names.
		core::World markerOnly("Version 11", 8, 2);
		auto const markerRoom = markerOnly.addRoom("Room", 0, 0, 0, 8, 1);
		markerOnly.addSectorMarker(markerRoom, 0, 3.5f, "Named Marker");
		auto version11 = serializeWorld(markerOnly);
		auto versionOffset = version11.find("version: 64");
		require(versionOffset != std::string::npos, "Missing World version field");
		version11.replace(versionOffset, std::string("version: 64").size(), "version: 11");
		auto loaded11 = deserializeWorld(version11);
		require(loaded11->getMarkerIds().size() == 1
			&& loaded11->lookupMarker(loaded11->getMarkerIds().front())->getName()
				== "Named Marker",
			"A version-11 World did not retain named Marker identity");

		auto legacy = version11;
		versionOffset = legacy.find("version: 11");
		legacy.replace(versionOffset, std::string("version: 11").size(), "version: 10");
		auto migrated = deserializeWorld(legacy);
		require(migrated->getMarkerIds().front().value == 1
			&& migrated->lookupMarker(core::MarkerId{ 1 })->getName() == "Marker 1",
			"Legacy unnamed Marker migration was not deterministic");

		auto future = currentYaml;
		versionOffset = future.find("version: 64");
		future.replace(versionOffset, std::string("version: 64").size(), "version: 65");
		bool futureRefused = false;
		try { (void)deserializeWorld(future); }
		catch (std::exception const& error)
		{
			futureRefused = std::string(error.what()).find("Unsupported")
				!= std::string::npos;
		}
		require(futureRefused,
			"A reader outside its supported World version boundary did not refuse");

		auto malformed = version11;
		auto const nameOffset = malformed.find("name: Named Marker");
		require(nameOffset != std::string::npos, "Missing serialized Marker name");
		malformed.replace(nameOffset, std::string("name: Named Marker").size(),
			"name: ' '");
		auto current = currentRoundTrip;
		auto const beforeReplacement = serializeWorld(*current);
		auto replaceDocument = [&](std::string const& candidate)
		{
			try
			{
				auto parsed = deserializeWorld(candidate, registry);
				current = std::move(parsed);
				return true;
			}
			catch (...) { return false; }
		};
		require(!replaceDocument(malformed)
			&& serializeWorld(*current) == beforeReplacement,
			"A malformed whole-document replacement disturbed the open World");

		gWorldDocumentHistory.clear();
		auto edited = current->getAgentBehaviourAssignment(agent)->configuration;
		auto* code = core::agentBehaviourConfigurationGetIf<std::string>(
			&edited.at("code"));
		*code = "Edited";
		require(commitAgentBehaviourAssignment(current, agent, behaviour, revision,
			edited, diagnostic), "The assignment edit failed: " + diagnostic);
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			return replaceDocument(snapshot.yaml);
		};
		require(gWorldDocumentHistory.undo(
			gWorldDocumentHistory.capture(serializeWorld(*current)), restore)
			&& *core::agentBehaviourConfigurationGetIf<std::string>(
				&current->getAgentBehaviourAssignment(agent)->configuration.at("code")) == "D",
			"Undo did not replace the complete authored World");
		require(gWorldDocumentHistory.redo(
			gWorldDocumentHistory.capture(serializeWorld(*current)), restore)
			&& *core::agentBehaviourConfigurationGetIf<std::string>(
				&current->getAgentBehaviourAssignment(agent)->configuration.at("code"))
					== "Edited",
			"Redo did not replace the complete authored World");
		gWorldDocumentHistory.clear();

		writeText(package / "malformed.yaml", "version: 1\nuuid: not-a-uuid\n");
		bool malformedRegistryRefused = false;
		try
		{
			(void)core::AgentBehaviourRegistry::loadFrom(
				(package / "malformed.yaml").string());
		}
		catch (...) { malformedRegistryRefused = true; }
		require(malformedRegistryRefused,
			"A malformed version-1 registry input was accepted");
	}

	void realPanelsRenderPausedRunningAndDiagnosticStates(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "panels.behaviours";
		std::filesystem::create_directories(package);
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		writeText(package / "panel.lua", R"lua(
return { api_version = 3, factory = function()
    return function(context)
      error("panel diagnostic")
      while true do wait() end
    end
end }
)lua");
		auto const behaviour = registry->addAgentBehaviour(
			"Panel schedule", "panel.lua", scheduleSchema());
		registry->saveTo((package / "behaviours.yaml").string());

		auto world = std::make_shared<core::World>("Panels", 12, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 12, 1);
		auto const markerObject = world->addSectorMarker(room, 0, 9.5f, "Destination");
		world->finishBuild();
		auto const agent = world->createAgent("Panel Agent", room, 0, 0.5f);
		world->pauseSimulation();
		auto const worldPath = temporary.path / "panels.world.yaml";
		world->saveTo(worldPath.string());
		world->attachAgentBehaviourRegistry("panels.behaviours", registry);
		auto const marker = world->getMarkerIds().front();
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour,
			registry->lookupAgentBehaviour(behaviour)->getRevision(),
			scheduleConfiguration("Panel", 1, marker, false,
				{ { marker, 1 }, { marker, 1 } }), &diagnostic),
			"Could not assign the panel fixture: " + diagnostic);
		auto markerSelection = world->getSector(room)->getObject(markerObject.index);

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1024, 768);
		unsigned char* pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		(void)pixels; (void)width; (void)height;
		auto renderFrame = [&]
		{
			ImGui::NewFrame();
			ImGui::Begin("Registry and behaviour");
			(void)renderBehavioursPanel(world, worldPath.string());
			ImGui::End();
			ImGui::Begin("Assignment, schedule, status, diagnostics");
			renderAgentBehaviourAssignmentCell(world, agent);
			renderAgentBehaviourConfigurationPanel(world, agent);
			ImGui::End();
			ImGui::Begin("Marker");
			renderMarkerEditorPanel(world, markerSelection);
			ImGui::End();
			ImGui::Render();
		};

		renderFrame(); // paused: editable nested schedule and named Marker controls
		require(world->resumeSimulation(), "Could not render the running panel state");
		renderFrame(); // running: assignment and schedule controls are disabled
		require(!world->advanceTick() && world->isSimulationPaused(),
			"The panel diagnostic fixture did not fail visibly");
		renderFrame(); // paused after failure: status, diagnostic, traceback, clear control
		auto const& diagnostics = world->getAgentBehaviourRuntimeDiagnostics();
		require(diagnostics.size() == 2 && diagnostics[0].callback == "resume"
			&& diagnostics[1].callback == "close",
			"Rendering diagnostics acknowledged them without the explicit Clear control");
		resetBehavioursPanelState();
	}
}

void behaviour_smoke::registerWorkflowEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "behaviours/persistenceHistoryMigrationAndReplacementAreAtomic", [](smoke::Context const& context)
	{
		EditorState state;
		persistenceHistoryMigrationAndReplacementAreAtomic(context);
	} });
	checks.push_back({ "behaviours/realPanelsRenderPausedRunningAndDiagnosticStates", [](smoke::Context const& context)
	{
		EditorState state;
		realPanelsRenderPausedRunningAndDiagnosticStates(context);
	} });
}
