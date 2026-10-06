#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "MarkerPanel.h"
#include "FurniturePanel.h"
#include "core/Agent.h"
#include "AgentBehaviourAssignmentPanel.h"
#include "core/World.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/Log.h"
#include "imgui/imgui.h"
#include <fstream>
#include <iterator>

namespace
{
	void behaviourConfiguration(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto const root = context.temporaryRoot();
		auto const package = root / "configured.behaviours";
		std::filesystem::create_directories(package);
		std::ofstream(package / "request.lua") << "return {api_version=2,factory=function() return {} end}";
		auto registry = core::AgentBehaviourRegistry::create();
		registry->saveTo((package / "behaviours.yaml").string());
		auto behaviour = registry->addAgentBehaviour("Configured Action", "request.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker },
			{ "action", core::AgentBehaviourSchemaType::Action, {}, false, core::AgentBehaviourAction{} }
		});
		registry->saveTo((package / "behaviours.yaml").string());
		registry = core::AgentBehaviourRegistry::loadFrom((package / "behaviours.yaml").string());
		auto path = root / "configured.actions.lua";
		std::ofstream(path) << "return {api_version=1,uuid='ad603358-5ebf-45bb-a686-c3f491152c61',actions={{key='greet',name='Greet',run=function() end}}}";
		std::string const action = "ad603358-5ebf-45bb-a686-c3f491152c61:greet";
		auto world = std::make_shared<core::World>("Configuration", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 6.5f, "Target");
		world->finishBuild();
		auto agent = world->createAgent("Author", room, 0, 1.5f);
		auto marker = world->getMarkerIds()[0];
		world->pauseSimulation();
		world->saveTo((root / "configured-setup.world.yaml").string());
		std::string diagnostic;
		require(commitActionRegistrySelection(world, path, diagnostic)
			&& commitMarkerActionAssignment(world, marker, {action}, diagnostic), "Configuration Action authoring failed: " + diagnostic);
		world->attachAgentBehaviourRegistry("configured.behaviours", registry);
		require(commitAgentBehaviourAssignment(world, agent, behaviour, 1, {{"destination", marker}}, diagnostic), diagnostic);
		auto selected = [&]()
		{
			return core::agentBehaviourConfigurationGetIf<core::AgentBehaviourAction>(
				&world->getAgentBehaviourAssignment(agent)->configuration.at("action"))->reference;
		};
		require(selected() == "idle", "Omitted configuration Action did not default to Idle");
		auto configuration = world->getAgentBehaviourAssignment(agent)->configuration;
		configuration["action"] = core::AgentBehaviourAction{action};
		require(commitAgentBehaviourAssignment(world, agent, behaviour, 1, configuration, diagnostic), diagnostic);
		auto count = gWorldDocumentHistory.undoCount();
		configuration["action"] = core::AgentBehaviourAction{"missing"};
		require(!commitAgentBehaviourAssignment(world, agent, behaviour, 1, configuration, diagnostic)
			&& gWorldDocumentHistory.undoCount() == count && selected() == action,
			"Invalid configured Action changed assignment/history");
		require(!world->clearActionRegistry(&diagnostic) && world->actionRegistry() && selected() == action,
			"Registry removal invalidated an authored behaviour Action");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto replacement = deserializeDocumentSnapshot(snapshot, world, {});
			if (!replacement) return false;
			world = std::move(replacement); world->pauseSimulation(); return true;
		};
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore) && selected() == "idle",
			"Undo did not restore configured Idle");
		require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore) && selected() == action,
			"Redo did not restore stable Action identity");
		auto& io = ImGui::GetIO(); io.DisplaySize = {800,600};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Behaviour Actions");
		renderAgentBehaviourConfigurationPanel(world, agent);
		ImGui::End(); ImGui::Render();
		for (bool binary : {false, true})
		{
			auto document = root / (binary ? "configured.world.bin" : "configured.world.yaml");
			auto writer = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toFile(document.string()));
			core::SerializationWorkData work;
			work.documentDirectory = root;
			work.markSerializedUnmodified = false;
			world->serialize(*writer, work); writer->serialize(); writer.reset();
			auto reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromFile(document.string()))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromFile(document.string()));
			reader->deserialize();
			core::World reopened("Loading", 1, 1);
			require(reopened.deserialize(*reader, work), "Configured Action document refused");
			reopened.resolveAgentBehaviourRegistry(registry);
			require(reopened.getAgentBehaviourAssignment(agent) == world->getAgentBehaviourAssignment(agent),
				"Action configuration changed through YAML/binary reopen");
		}
	}

	void furnitureUseWorkflow(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot() / "use-editor"; std::filesystem::create_directories(root);
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), root / "use.furniture.lua");
		auto document = root / "use.world.yaml";
		auto world = std::make_shared<core::World>("Editor use", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1); world->finishBuild();
		auto agent = world->createAgent("Visitor", room, 0, 1.5f);
		world->pauseSimulation(); world->saveTo(document.string());
		std::string diagnostic;
		require(selectFurnitureCatalogue(world, document, "use.furniture.lua", diagnostic), diagnostic);
		require(placeSelectedFurniture(world, room, "chair", 3, 0, false, "Chair", diagnostic), diagnostic);
		auto seat = world->furniture()[0].marker;
		require(world->setAgentIndividualMinimumRoutePlanningTime(agent, 0.1f)
			&& world->setAgentIndividualMaximumRoutePlanningTime(agent, 0.1f), "Editor use planning refused");
		require(world->availableAgentActions(seat) == std::vector<std::string>{"idle", "use-furniture"}, "Editor chair lacks derived use");
		auto& io = ImGui::GetIO(); io.DisplaySize = {800, 600}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Furniture Action selection");
		bool choosingAction = true;
		auto const popupHistory = gWorldDocumentHistory.undoCount();
		renderAgentMovementActionPopup(world, seat, true, choosingAction);
		ImGui::End(); ImGui::Render();
		ImGui::NewFrame(); ImGui::Begin("Furniture Action selection");
		auto const popupLog = root / "popup.txt";
		ImGui::LogToFile(-1, popupLog.string().c_str());
		require(!renderAgentMovementActionPopup(world, seat, false, choosingAction),
			"Opening the destination popup implicitly chose an Action");
		ImGui::LogFinish();
		std::ifstream popupInput(popupLog);
		std::string popupText((std::istreambuf_iterator<char>(popupInput)), {});
		require(choosingAction && popupText.find("Idle") != std::string::npos
			&& popupText.find(world->agentActionDisplayName(core::UseFurnitureAction)) != std::string::npos,
			"Furniture destination popup omitted its Actions");
		require(gWorldDocumentHistory.undoCount() == popupHistory,
			"Opening the popup changed document history");
		choosingAction = false;
		require(!renderAgentMovementActionPopup(world, seat, false, choosingAction) && !choosingAction,
			"Cancelling the popup chose an Action");
		ImGui::End(); ImGui::Render();
		auto count = gWorldDocumentHistory.undoCount();
		require(commitAgentMarkerActionRequest(world, agent, seat, core::UseFurnitureAction, diagnostic)
			&& gWorldDocumentHistory.undoCount() == count + 1, "Editor use request missed history: " + diagnostic);
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = deserializeDocumentSnapshot(snapshot, world, document);
			if (!loaded) return false;
			world = std::move(loaded); world->pauseSimulation(); return true;
		};
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Use request Undo/Redo failed");
		require(world->resumeSimulation(), "Editor use resume failed");
		bool arrived = false;
		for (unsigned tick = 0; tick < 1800 && !arrived; ++tick)
		{
			require(world->advanceTick(), "Editor use failed");
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{ arrived = true; require(event.selectedAction == core::UseFurnitureAction, "Editor selected another Action"); }
		}
		require(arrived && world->lookupAgent(agent).entity->getPose() == core::Pose::Sitting
			&& world->usablePointOccupant(seat) == agent, "Editor request did not use chair");
		world->pauseSimulation();
		require(commitAgentMarkerActionRequest(world, agent, seat, core::IdleAction, diagnostic), diagnostic);
		require(world->usablePointOccupant(seat) == agent, "Editor request acceptance vacated chair");
		require(world->resumeSimulation() && world->advanceTicks(60), "Editor Idle replacement failed");
		require(world->lookupAgent(agent).entity->getPose() == core::Pose::Standing && !world->usablePointOccupant(seat), "Editor replacement did not finish");
		world->pauseSimulation();
		require(commitAgentMarkerActionRequest(world, agent, seat, core::UseFurnitureAction, diagnostic), diagnostic);
		require(world->resumeSimulation() && world->advanceTicks(60) && world->usablePointOccupant(seat) == agent, "Editor edit-use setup failed");
		world->pauseSimulation(); core::consumeLogMessages();
		auto id = world->furniture()[0].id;
		count = gWorldDocumentHistory.undoCount();
		auto position = world->lookupAgent(agent).entity->getGlobalPosition();
		require(!editSelectedFurniture(world, id, -1, 0, false, "Refused", diagnostic)
			&& gWorldDocumentHistory.undoCount() == count && world->usablePointOccupant(seat) == agent,
			"Refused editor move changed use/history");
		require(!deleteSelectedFurniture(world, 99999, diagnostic) && gWorldDocumentHistory.undoCount() == count,
			"Refused editor deletion changed history");
		require(editSelectedFurniture(world, id, 4, 0, false, "Moved", diagnostic)
			&& gWorldDocumentHistory.undoCount() == count + 1, diagnostic);
		require(world->lookupAgent(agent).entity->getGlobalPosition() == position && !world->usablePointOccupant(seat)
			&& world->lookupAgent(agent).entity->getPose() == core::Pose::Standing, "Editor move stranded/teleported user");
		unsigned finishes = 0;
		for (auto const& log : core::consumeLogMessages()) if (log.msg.starts_with("finish:")) ++finishes;
		require(finishes == 1, "Editor move did not finish old use");
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& !world->usablePointOccupant(seat) && world->lookupAgent(agent).entity->getPose() == core::Pose::Standing
			&& gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Furniture move history restored transient use");
		require(deleteSelectedFurniture(world, id, diagnostic)
			&& gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Furniture deletion history failed");
		for (auto const& log : core::consumeLogMessages())
			require(!log.msg.starts_with("use:") && !log.msg.starts_with("finish:"), "History reconstruction replayed lifecycle effects");
	}

	void reloadWorkflow(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot();
		auto path = root / "editor-reload.actions.lua";
		auto write = [&](std::string const& name) {
			std::ofstream(path) << "return {api_version=1,uuid='ad603358-5ebf-45bb-a686-c3f491152c61',actions={{key='greet',name='"
				<< name << "',run=function() end}}}";
		};
		write("Old");
		auto world = std::make_shared<core::World>("Editor reload", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 1.5f, "Target"); world->finishBuild(); world->pauseSimulation();
		world->saveTo((root / "editor-reload.world.yaml").string());
		auto marker = world->getMarkerIds()[0]; std::string diagnostic;
		std::string action = "ad603358-5ebf-45bb-a686-c3f491152c61:greet";
		require(commitActionRegistrySelection(world, path, diagnostic)
			&& commitMarkerActionAssignment(world, marker, {action}, diagnostic), diagnostic);
		auto count = gWorldDocumentHistory.undoCount();
		auto before = world->actionRegistry();
		std::ofstream(path) << "return {}";
		require(!reloadSelectedActionRegistry(world, path, diagnostic) && !diagnostic.empty()
			&& world->actionRegistry() == before && gWorldDocumentHistory.undoCount() == count,
			"Failed editor reload changed package/history");
		write("New");
		require(reloadSelectedActionRegistry(world, path, diagnostic) && gWorldDocumentHistory.undoCount() == count,
			"Runtime reload authored an undo entry");
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto loaded = deserializeDocumentSnapshot(snapshot, world, {});
			if (!loaded) return false;
			world = std::move(loaded); world->pauseSimulation(); return true;
		};
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& world->markerActions(marker).empty() && world->agentActionDisplayName(action) == "New"
			&& gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore)
			&& world->markerActions(marker) == std::vector<std::string>{action}
			&& world->agentActionDisplayName(action) == "New", "History replayed old executable package");
		require(world->resumeSimulation() && !reloadSelectedActionRegistry(world, path, diagnostic)
			&& gWorldDocumentHistory.undoCount() == count, "Running editor reload changed history");
		world->pauseSimulation();
		auto furniturePath = root / "use.furniture.lua";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), furniturePath);
		auto document = root / "reload.world.yaml"; world->saveTo(document.string());
		require(selectFurnitureCatalogue(world, document, furniturePath.filename().string(), diagnostic)
			&& placeSelectedFurniture(world, room, "chair", 3, 0, false, "Chair", diagnostic), diagnostic);
		count = gWorldDocumentHistory.undoCount();
		auto catalogue = world->furnitureCatalogue();
		std::ofstream(furniturePath) << "return {}";
		require(!reloadSelectedFurnitureCatalogue(world, document, diagnostic) && !diagnostic.empty()
			&& world->furnitureCatalogue() == catalogue && gWorldDocumentHistory.undoCount() == count,
			"Failed editor Furniture reload changed history/package");
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/use.furniture.lua"), furniturePath,
			std::filesystem::copy_options::overwrite_existing);
		require(reloadSelectedFurnitureCatalogue(world, document, diagnostic)
			&& gWorldDocumentHistory.undoCount() == count && world->furniture()[0].marker,
			"Successful editor Furniture reload changed authored history");
	}

	void workflow(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto path = context.temporaryRoot() / "editor.actions.lua";
		std::ofstream(path) << R"lua(return {api_version=1, uuid='ad603358-5ebf-45bb-a686-c3f491152c61', actions={
{key='hello',name='Hello',run=function(a,w,m) w.log('editor:' .. m.name) end},
{key='other',name='Other',run=function() end},
{key='unassigned',name='Unassigned',run=function() end}}})lua";
		auto world = std::make_shared<core::World>("Editor",10,2);
		auto room = world->addRoom("Room",0,0,0,10,1);
		world->addSectorMarker(room,0,6.5f,"Target");
		world->finishBuild(); auto agent = world->createAgent("Visitor",room,0,1.5f);
		world->pauseSimulation();
		world->saveTo((context.temporaryRoot() / "editor.world.yaml").string());
		auto marker = world->getMarkerIds()[0];
		std::string const first = "ad603358-5ebf-45bb-a686-c3f491152c61:hello";
		std::string const second = "ad603358-5ebf-45bb-a686-c3f491152c61:other";
		std::string diagnostic;
		require(commitActionRegistrySelection(world,path,diagnostic), diagnostic);
		require(commitMarkerActionAssignment(world,marker,{second,first,second},diagnostic), diagnostic);
		require(commitAgentMarkerActionRequest(world,agent,marker,first,diagnostic), diagnostic);
		require(gWorldDocumentHistory.undoCount() == 3, "Authoring did not commit document history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto replacement = deserializeDocumentSnapshot(snapshot, world, {});
			if (!replacement) return false;
			world = std::move(replacement); world->pauseSimulation(); return true;
		};
		for (int i = 0; i < 3; ++i)
			require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore), "Action history undo failed");
		require(!world->actionRegistry() && world->markerActions(marker).empty(), "Registry/assignment undo lost authored state");
		for (int i = 0; i < 3; ++i)
			require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore), "Action history redo failed");
		require(world->markerActions(marker) == std::vector<std::string>{second,first}, "Action identity/order lost through history");
		require(world->renameMarker(marker,"Renamed"), "Marker rename refused");
		auto& io = ImGui::GetIO(); io.DisplaySize = {800,600};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Marker actions");
		bool choosingAction = true;
		renderAgentMovementActionPopup(world, marker, true, choosingAction);
		ImGui::End(); ImGui::Render();
		ImGui::NewFrame(); ImGui::Begin("Marker actions");
		auto const popupLog = context.temporaryRoot() / "marker-popup.txt";
		ImGui::LogToFile(-1, popupLog.string().c_str());
		require(!renderAgentMovementActionPopup(world, marker, false, choosingAction),
			"Destination popup selected an Action before user input");
		ImGui::LogFinish();
		std::ifstream popupInput(popupLog);
		std::string popupText((std::istreambuf_iterator<char>(popupInput)), {});
		auto const firstLabel = world->agentActionDisplayName(first);
		auto const secondLabel = world->agentActionDisplayName(second);
		require(popupText.find("Idle") != std::string::npos
			&& popupText.find(secondLabel) < popupText.find(firstLabel)
			&& popupText.find("Unassigned") == std::string::npos
			&& popupText.find(world->agentActionDisplayName(core::UseFurnitureAction)) == std::string::npos,
			"Destination popup omitted Idle or changed authored Action order");
		choosingAction = false;
		renderAgentMovementActionPopup(world, marker, false, choosingAction);
		auto sector = world->getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (auto object = sector->getObject(i); object && object->getObjectType() == core::SectorObjectType::Marker)
				renderMarkerEditorPanel(world,object,[&](auto const& error) { diagnostic = error; });
		ImGui::End(); ImGui::Render();
		require(diagnostic.empty(), "Headless editor surface produced an error");
		require(world->resumeSimulation(), "Editor World resume failed");
		auto const historyCount = gWorldDocumentHistory.undoCount();
		require(!commitMarkerActionAssignment(world,marker,{},diagnostic)
			&& gWorldDocumentHistory.undoCount() == historyCount, "Running assignment changed World/history");
		bool arrived = false;
		core::consumeLogMessages();
		for (int tick = 0; tick < 1800 && !arrived; ++tick)
		{
			require(world->advanceTick(), "Editor-authored Lua Action failed");
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{
					arrived = true; require(event.selectedAction == first, "Editor request selected a different Action");
				}
		}
		require(arrived, "Editor-authored request never arrived");
		bool logged = false;
		for (auto const& log : core::consumeLogMessages()) logged |= log.msg == "editor:Renamed";
		require(logged, "Editor workflow did not execute real Lua logging");
		world->pauseSimulation();
		auto undo = captureDocumentSnapshot(world);
		require(world->clearActionRegistry(&diagnostic), diagnostic);
		commitDocumentEdit(std::move(undo));
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore)
			&& world->markerActions(marker) == std::vector<std::string>{second,first}, "Registry removal cannot be undone");
	}

	void liveDestinationEdit(smoke::Context const&)
	{
		editor_smoke::State state;
		using smoke::require;
		auto world = std::make_shared<core::World>("Live destination", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 8.5f, "Target");
		world->finishBuild();
		auto agent = world->createAgent("Visitor", room, 0, 1.5f);
		auto marker = world->getMarkerIds().front();
		auto* entity = world->lookupAgent(agent).entity;
		auto target = world->getGraph()->getClosestVertexInSector(world->getSector(room).get(), { 8.5f, 0.5f });
		std::string diagnostic;

		// A live destination edit while running is a transient runtime request: it
		// starts the Agent moving without pausing and authors no document state (#469).
		require(!world->isSimulationPaused(), "Fixture did not start running");
		auto const history = gWorldDocumentHistory.undoCount();
		auto const wasModified = world->isModified();
		auto path = world->getGraph()->calculatePath(entity, nullptr, target);
		require(path && applyAgentPathEdit(world, entity, std::move(path), true, true,
			core::IdleAction, diagnostic), "Live destination edit refused: " + diagnostic);
		require(gWorldDocumentHistory.undoCount() == history && world->isModified() == wasModified,
			"Live destination edit authored document state");
		bool arrived = false;
		for (unsigned tick = 0; tick < 1800 && !arrived; ++tick)
		{
			require(world->advanceTick(), "Live destination journey failed");
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{ arrived = true; require(event.selectedAction == core::IdleAction, "Live destination selected another Action"); }
		}
		require(arrived, "Live destination edit never arrived");

		// A refused live edit surfaces a diagnostic and leaves history untouched.
		auto const refusedHistory = gWorldDocumentHistory.undoCount();
		path = world->getGraph()->calculatePath(entity, nullptr, target);
		diagnostic.clear();
		require(!applyAgentPathEdit(world, entity, std::move(path), true, true,
			core::UseFurnitureAction, diagnostic) && !diagnostic.empty()
			&& gWorldDocumentHistory.undoCount() == refusedHistory,
			"Refused live edit lost its diagnostic or changed history");

		// The authored, reset-persistent request still requires a pause.
		diagnostic.clear();
		require(!commitAgentMarkerActionRequest(world, agent, marker, core::IdleAction, diagnostic)
			&& !diagnostic.empty(), "Authored request was accepted while running");
		world->pauseSimulation();
		auto const authoredHistory = gWorldDocumentHistory.undoCount();
		path = world->getGraph()->calculatePath(entity, nullptr, target);
		diagnostic.clear();
		require(applyAgentPathEdit(world, entity, std::move(path), true, true, core::IdleAction, diagnostic)
			&& gWorldDocumentHistory.undoCount() == authoredHistory + 1,
			"Paused destination edit did not author document history: " + diagnostic);
		require(world->isModified(), "Paused destination edit did not dirty the document");
	}

	void registryDirectory(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto const root = context.temporaryRoot();
		auto const worldDirectory = root / "registry-world";
		auto const otherDirectory = root / "registry-elsewhere";
		std::filesystem::create_directories(worldDirectory);
		std::filesystem::create_directories(otherDirectory);
		auto const document = worldDirectory / "directory.world.yaml";
		auto const adjacent = worldDirectory / "adjacent.actions.lua";
		auto const elsewhere = otherDirectory / "elsewhere.actions.lua";
		auto const uuid = std::string("ad603358-5ebf-45bb-a686-c3f491152c61");
		auto write = [&](std::filesystem::path const& path, std::string const& name)
		{
			std::ofstream(path) << "return {api_version=1,uuid='" << uuid
				<< "',actions={{key='greet',name='" << name << "',run=function() end}}}";
		};
		write(adjacent, "Adjacent");
		write(elsewhere, "Elsewhere");
		auto world = std::make_shared<core::World>("Registry directory", 10, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 10, 1);
		world->addSectorMarker(room, 0, 6.5f, "Target");
		world->finishBuild(); world->pauseSimulation();
		auto marker = world->getMarkerIds()[0];
		std::string const action = uuid + ":greet";
		std::string diagnostic;

		// An unsaved World has no document directory to bind a registry to, so the
		// panel refuses rather than attaching a file its document cannot locate.
		require(!commitActionRegistrySelection(world, adjacent, diagnostic) && !diagnostic.empty()
			&& !world->actionRegistry(), "Unsaved World accepted an Action registry");

		world->saveTo(document.string());
		require(!world->documentDirectory().empty(), "Saving did not record the document directory");

		// The World seam rejects a registry from another directory even though it
		// parses, because reopen resolves the basename beside the document.
		diagnostic.clear();
		require(!world->selectActionRegistry(elsewhere, &diagnostic) && !diagnostic.empty()
			&& !world->actionRegistry(), "World accepted a registry outside its document directory");

		// The panel seam mirrors the Furniture adjacency rule and adds no history.
		auto history = gWorldDocumentHistory.undoCount();
		diagnostic.clear();
		require(!commitActionRegistrySelection(world, elsewhere, diagnostic) && !diagnostic.empty()
			&& !world->actionRegistry() && gWorldDocumentHistory.undoCount() == history,
			"Panel accepted a registry outside the World directory");

		// A bare relative reference resolves beside the saved World and is accepted.
		diagnostic.clear();
		require(commitActionRegistrySelection(world, adjacent.filename(), diagnostic)
			&& world->actionRegistryFilename() == adjacent.filename().string()
			&& gWorldDocumentHistory.undoCount() == history + 1, diagnostic);
		require(commitMarkerActionAssignment(world, marker, {action}, diagnostic), diagnostic);

		// The reference round-trips: the saved document resolves the registry beside it.
		world->saveTo(document.string());
		auto reopened = core::loadWorldDocument(document);
		require(reopened->actionRegistryFilename() == adjacent.filename().string()
			&& reopened->markerActions(marker) == std::vector<std::string>{action},
			"Saved document did not resolve its adjacent registry");
	}
}

void editor_smoke::registerMarkerActions(std::vector<smoke::Check>& checks)
{
	checks.push_back({"markerActions/reloadWorkflow",reloadWorkflow});
	checks.push_back({"markerActions/workflow",workflow});
	checks.push_back({"markerActions/behaviourConfiguration",behaviourConfiguration});
	checks.push_back({"markerActions/furnitureUseWorkflow",furnitureUseWorkflow});
	checks.push_back({"markerActions/liveDestinationEdit",liveDestinationEdit});
	checks.push_back({"markerActions/registryDirectory",registryDirectory});
}
