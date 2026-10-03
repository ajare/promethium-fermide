#include "Checks.h"
#include "State.h"
#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include "core/YamlSerializer.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include "core/AgentTagRegistryDocument.h"

namespace
{
	void demoActions(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot();
		std::filesystem::copy_file(context.fixture("resources/test-worlds/furniture.furniture.yaml"), root / "furniture.furniture.yaml");
		auto path = root / "demo.world.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/furniture.world.yaml"), path);
		auto world = core::loadWorldDocument(path); world->pauseSimulation();
		DocumentHistory history; std::string diagnostic;
		require(placeSelectedFurniture(world, 1, "chair", 2.125f, 0, false, "Authored chair", diagnostic, history), diagnostic);
		auto chair = world->furniture().back();
		require(editSelectedFurniture(world, chair.id, 2.375f, 0, false, "Edited chair", diagnostic, history), diagnostic);
		auto catalogue = world->furnitureCatalogue();
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work); world->pauseSimulation(); return restored;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().back().x == 2.125f,
			"Bundled authoring workflow cannot undo movement");
		require(history.redo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().back().marker == chair.marker && world->furniture().back().x == 2.375f,
			"Bundled authoring history lost placement/identity");
		require(world->renameMarker(chair.marker, "Authored destination", &diagnostic), diagnostic);
		world->addSectorMarker(1, 0, 0.125f, "Corridor entrance");
		require(world->rebuildTraversalTopology(), world->getTopologyDiagnostic());
		std::shared_ptr<const core::Vertex> destination;
		for (auto const& vertex : world->getGraph()->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getId() == chair.marker)
				destination = vertex;
		auto visitorId = world->createAgent("Authored visitor", 1, 0, 0.125f);
		auto visitor = world->lookupAgent(visitorId).entity;
		auto route = world->getGraph()->calculatePath(visitor, destination);
		require(route != nullptr, "Editor-placed sample destination cannot be targeted");
		visitor->setPath(route, true);
		for (auto extension : {"world.yaml", "world"})
		{
			auto output = root / (std::string("edited.") + extension); world->saveTo(output.string());
			auto reopened = core::loadWorldDocument(output);
			require(reopened->furniture().back().id == chair.id && reopened->furniture().back().marker == chair.marker
				&& reopened->lookupMarker(chair.marker)->getName() == "Authored destination",
				"Editor workflow lost identity/name on reopen");
			reopened->advanceTicks(1200);
			require(reopened->lookupAgent(visitorId).entity->getGlobalPosition().x == 14.875f,
				"Reopened editor-authored journey did not arrive");
		}
	}

	void attachmentActions(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto path = context.temporaryRoot() / "attachment.world.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/attachments.furniture.yaml"), path.parent_path() / "attachments.furniture.yaml");
		auto world = std::make_shared<core::World>("Attachment editor", 8, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->addSectorMarker(room, 0, 0.5f, "Entrance");
		world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history; std::string diagnostic;
		require(selectFurnitureCatalogue(world, path, "attachments.furniture.yaml", diagnostic, history), diagnostic);
		require(placeSelectedFurniture(world, room, "desk", 2.125f, 0, false, "Desk", diagnostic, history, 2), diagnostic);
		auto agentId = world->createAgent("Observer", room, 0, 0.5f);
		require(placeSelectedFurniture(world, room, "chair", 3.125f, 0, false, "Chair", diagnostic, history, 1), diagnostic);
		auto chair = world->furniture().back();
		auto catalogue = world->furnitureCatalogue();
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work); world->pauseSimulation(); return restored;
		};
		auto connected = [&](bool expected) {
			std::shared_ptr<const core::Vertex> seat, entrance;
			for (uint32_t i = 0; i < world->getSector(room)->getNumObjects(); ++i)
				if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world->getSector(room)->getObject(i));
					object)
				{
					if (object->getMarker()->getId() == chair.marker) seat = world->getGraph()->getVertexForObject(object);
					if (object->getMarker()->getName() == "Entrance") entrance = world->getGraph()->getVertexForObject(object);
				}
			core::Agent query("Query");
			require(seat && bool(world->getGraph()->calculatePath(&query,
				entrance, seat)) == expected,
				"Editor history retained stale attachment connectivity");
			require(world->furniture().back().marker == chair.marker, "History changed attached Marker identity");
			require(world->lookupAgent(agentId).entity->getGlobalPosition().x == 0.5f, "Furniture history teleported an Agent");
		};
		connected(true);
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().size() == 1 && !world->lookupMarker(chair.marker), "Attachment placement undo left contributions");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Attachment placement redo failed");
		connected(true);
		// Move the port to a private endpoint: overlap remains, attachment does not.
		require(editSelectedFurniture(world, chair.id, 2.375f, 0, false, "Moved", diagnostic, history), diagnostic);
		connected(false);
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Attachment movement undo failed");
		connected(true);
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Attachment movement redo failed");
		connected(false);
		require(editSelectedFurniture(world, chair.id, 3.125f, 0, false, "Reattached", diagnostic, history), diagnostic);
		connected(true);
		require(deleteSelectedFurniture(world, chair.id, diagnostic, history), diagnostic);
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Attachment deletion undo failed");
		connected(true);
	}

	void compositionActions(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto path = context.temporaryRoot() / "composition.world.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/composition.furniture.yaml"), path.parent_path() / "composition.furniture.yaml");
		auto world = std::make_shared<core::World>("Composition editor", 12, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->addSectorMarker(room, 0, 0.5f, "Entrance");
		world->addSectorMarker(room, 0, 5, "Internal boundary");
		world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history; std::string diagnostic;
		require(selectFurnitureCatalogue(world, path, "composition.furniture.yaml", diagnostic, history), diagnostic);
		require(placeSelectedFurniture(world, room, "outer", 2, 0, false, "Outer", diagnostic, history, 2), diagnostic);
		require(placeSelectedFurniture(world, room, "inner", 5, 0, false, "Inner", diagnostic, history, 3), diagnostic);
		auto inner = world->furniture().back(); auto catalogue = world->furnitureCatalogue();
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work); world->pauseSimulation(); return restored;
		};
		auto check = [&] {
			std::shared_ptr<const core::Vertex> entrance, seat, boundary;
			for (uint32_t i = 0; i < world->getSector(room)->getNumObjects(); ++i)
				if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world->getSector(room)->getObject(i)); object)
				{
					auto vertex = world->getGraph()->getVertexForObject(object);
					if (object->getMarker()->getName() == "Entrance") entrance = vertex;
					if (object->getMarker()->getName() == "Internal boundary") boundary = vertex;
					if (object->getMarker()->getId() == inner.marker) seat = vertex;
				}
			core::Agent query("Query");
			require(seat && world->getGraph()->calculatePath(&query, entrance, seat)
				&& !world->getGraph()->calculatePath(&query, entrance, boundary), "History lost composition or restored internal floor attachment");
		};
		check();
		auto before = captureDocumentSnapshot(world, history)->yaml; auto count = history.undoCount();
		require(!placeSelectedFurniture(world, room, "inner", 3, 0, false, "Refused", diagnostic, history, 2), "Same-depth placement accepted");
		require(!editSelectedFurniture(world, inner.id, 3, 0, false, "Refused", diagnostic, history, 2), "Same-depth depth/movement edit accepted");
		require(history.undoCount() == count && captureDocumentSnapshot(world, history)->yaml == before,
			"Composition refusal mutated instances, destinations or history");
		require(history.undo(captureDocumentSnapshot(world, history), restore) && !world->lookupMarker(inner.marker), "Composition placement undo failed");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Composition placement redo failed"); check();
		for (float x : {3.f, 6.f})
		{
			require(editSelectedFurniture(world, inner.id, x, 0, false, "Moved", diagnostic, history), diagnostic); check();
			require(history.undo(captureDocumentSnapshot(world, history), restore), "Composition movement undo failed"); check();
			require(history.redo(captureDocumentSnapshot(world, history), restore), "Composition movement redo failed"); check();
		}
		for (auto id : {world->furniture().front().id, inner.id})
		{
			require(deleteSelectedFurniture(world, id, diagnostic, history), diagnostic);
			require(history.undo(captureDocumentSnapshot(world, history), restore), "Composition deletion undo failed"); check();
		}
	}

	void chairActions(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto path = context.temporaryRoot() / "editor.world.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/chair.furniture.yaml"), path.parent_path() / "chair.furniture.yaml");
		auto world = std::make_shared<core::World>("Chair editor", 12, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 6, 1);
		auto corridor = world->addCorridor(0, 1, 0, 6, 1);
		auto facade = world->addFacade("Facade", 0, 0, 6, 6, 1);
		auto background = world->addBackground(1, 0, 0, 6, 1);
		world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history; std::string diagnostic;
		require(selectFurnitureCatalogue(world, path, "chair.furniture.yaml", diagnostic, history), diagnostic);
		auto catalogue = world->furnitureCatalogue();
		require(placeSelectedFurniture(world, room, "chair", 1.25f, 0, false, "Room chair", diagnostic, history), diagnostic);
		auto marker = world->furniture().front().marker;
		require(world->furniture().front().x == 1.25f, "Snap-disabled editor discarded fractional x");
		require(placeSelectedFurniture(world, corridor, "chair", 1.25f, 0, true, "Hall chair", diagnostic, history), diagnostic);
		require(world->furniture().back().x == 1, "Snap-enabled editor did not align x");
		require(placeSelectedFurniture(world, facade, "chair", 1.25f, 0, false, "Facade chair", diagnostic, history), diagnostic);
		auto count = history.undoCount();
		require(!placeSelectedFurniture(world, background, "chair", 1, 0, false, "Invalid", diagnostic, history)
			&& !diagnostic.empty() && history.undoCount() == count, "Refused placement acquired history");
		require(!placeSelectedFurniture(world, room, "chair", 4, 0.2f, true, "Floating", diagnostic, history), "Horizontal snapping allowed fractional y");
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work);
			world->pauseSimulation();
			return restored;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().size() == 2, "Chair placement undo failed");
		require(history.redo(captureDocumentSnapshot(world, history), restore) && world->furniture().size() == 3
			&& world->furniture().front().marker == marker, "Chair placement redo changed Marker identity");
		auto id = world->furniture().front().id;
		require(world->renameMarker(marker, "Authored seat", &diagnostic), diagnostic);
		require(editSelectedFurniture(world, id, 3.25f, 0, false, "Edited chair", diagnostic, history), diagnostic);
		require(world->furniture().front().x == 3.25f && world->lookupMarker(marker)->getName() == "Authored seat", "Editor move changed authored Marker name");
		count = history.undoCount();
		require(!editSelectedFurniture(world, id, 5.5f, 0, false, "Overhang", diagnostic, history)
			&& !diagnostic.empty() && history.undoCount() == count, "Refused movement acquired history");
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().front().x == 1.25f && world->lookupMarker(marker)->getName() == "Authored seat", "Furniture move undo lost ownership or name");
		require(history.redo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().front().x == 3.25f && world->furniture().front().name == "Edited chair", "Furniture move redo lost placement or instance name");
		require(deleteSelectedFurniture(world, id, diagnostic, history) && !world->lookupMarker(marker), diagnostic);
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().front().id == id && world->furniture().front().marker == marker
			&& world->lookupMarker(marker)->getName() == "Authored seat", "Furniture delete undo lost identities or authored name");
		require(history.redo(captureDocumentSnapshot(world, history), restore)
			&& !world->lookupMarker(marker) && world->furniture().size() == 2, "Furniture delete redo left destination");
		auto remaining = world->furniture().front();
		auto registry = core::AgentBehaviourRegistry::create();
		world->attachAgentBehaviourRegistry("editor.behaviours", registry);
		auto behaviour = registry->addAgentBehaviour("Visit", "visit.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
		});
		auto agent = world->createAgent("Editor visitor", remaining.sector, 0, 0.5f);
		require(world->setAgentBehaviourAssignment(agent, behaviour, 1, {{"destination", remaining.marker}}, &diagnostic), "Could not configure Furniture reference");
		count = history.undoCount();
		auto protectedSnapshot = captureDocumentSnapshot(world, history)->yaml;
		require(!deleteSelectedFurniture(world, remaining.id, diagnostic, history)
			&& diagnostic.find("Editor visitor") != std::string::npos && diagnostic.find("destination") != std::string::npos
			&& history.undoCount() == count && captureDocumentSnapshot(world, history)->yaml == protectedSnapshot,
			"Editor deletion did not protect references without history or mutation");
		// Exercise multi-point layouts through the same production history actions.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/layouts.furniture.yaml"), path.parent_path() / "layouts.furniture.yaml");
		auto layouts = std::make_shared<core::World>("Layout editor", 20, 4);
		auto layoutRoom = layouts->addRoom("Room", 0, 0, 0, 20, 4);
		layouts->finishBuild(); layouts->pauseSimulation(); layouts->saveTo(path.string());
		DocumentHistory layoutHistory;
		require(selectFurnitureCatalogue(layouts, path, "layouts.furniture.yaml", diagnostic, layoutHistory), diagnostic);
		auto layoutCatalogue = layouts->furnitureCatalogue();
		require(placeSelectedFurniture(layouts, layoutRoom, "sofa", 1.375f, 0, false, "Sofa", diagnostic, layoutHistory), diagnostic);
		require(placeSelectedFurniture(layouts, layoutRoom, "larger", 7.375f, 0, true, "Large", diagnostic, layoutHistory), diagnostic);
		require(layouts->furniture()[0].x == 1.375f && layouts->furniture()[1].x == 7, "Layout snap mode changed offsets or ignored toggle");
		auto sofa = layouts->furniture()[0]; auto large = layouts->furniture()[1];
		require(layouts->renameMarker(sofa.destinations[1].marker, "Right seat independently renamed", &diagnostic), diagnostic);
		auto restoreLayout = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = layoutCatalogue;
			auto restored = layouts->deserialize(*reader, work); layouts->pauseSimulation(); return restored;
		};
		require(editSelectedFurniture(layouts, sofa.id, 12.625f, 0, true, "Snapped sofa", diagnostic, layoutHistory), diagnostic);
		require(layouts->furniture()[0].x == 13, "Whole sofa did not snap horizontally");
		require(editSelectedFurniture(layouts, large.id, 8.625f, 0, false, "Fractional larger", diagnostic, layoutHistory), diagnostic);
		require(layouts->furniture()[1].x == 8.625f
			&& layouts->lookupMarker(sofa.destinations[0].marker)->getCellX() + layouts->lookupMarker(sofa.destinations[0].marker)->getOffset() == 13.25f
			&& layouts->lookupMarker(sofa.destinations[1].marker)->getCellX() + layouts->lookupMarker(sofa.destinations[1].marker)->getOffset() == 14.625f
			&& layouts->lookupMarker(large.destinations[0].marker)->getCellX() + layouts->lookupMarker(large.destinations[0].marker)->getOffset() == 7.875f
			&& layouts->lookupMarker(large.destinations[1].marker)->getCellX() + layouts->lookupMarker(large.destinations[1].marker)->getOffset() == 9.f
			&& layouts->lookupMarker(large.destinations[2].marker)->getCellX() + layouts->lookupMarker(large.destinations[2].marker)->getOffset() == 10.375f,
			"Editor snapping/movement changed rigid fractional offsets");
		count = layoutHistory.undoCount();
		auto layoutBefore = captureDocumentSnapshot(layouts, layoutHistory)->yaml;
		for (bool snap : {false, true})
			require(!editSelectedFurniture(layouts, large.id, 8.625f, 0.25f, snap, "Floating", diagnostic, layoutHistory)
				&& layoutHistory.undoCount() == count && captureDocumentSnapshot(layouts, layoutHistory)->yaml == layoutBefore,
				"Snap toggle allowed fractional y or acquired refusal history");
		require(!editSelectedFurniture(layouts, large.id, 13, 0, false, "Overlap", diagnostic, layoutHistory)
			&& layoutHistory.undoCount() == count, "Multi-point overlap failure acquired history");
		require(layoutHistory.undo(captureDocumentSnapshot(layouts, layoutHistory), restoreLayout)
			&& layouts->furniture()[1].x == 7, "Larger movement undo failed");
		require(layoutHistory.redo(captureDocumentSnapshot(layouts, layoutHistory), restoreLayout)
			&& layouts->furniture()[1].x == 8.625f, "Larger movement redo failed");
		for (size_t i = 0; i < large.destinations.size(); ++i)
			require(layouts->furniture()[1].destinations[i].marker == large.destinations[i].marker, "History changed larger destination identity");
		require(deleteSelectedFurniture(layouts, sofa.id, diagnostic, layoutHistory), diagnostic);
		for (auto const& point : sofa.destinations) require(!layouts->lookupMarker(point.marker), "Editor deletion left a destination");
		require(layoutHistory.undo(captureDocumentSnapshot(layouts, layoutHistory), restoreLayout)
			&& layouts->furniture()[0].destinations[1].marker == sofa.destinations[1].marker
			&& layouts->lookupMarker(sofa.destinations[1].marker)->getName() == "Right seat independently renamed",
			"Multi-point delete undo changed identity or name");
		std::filesystem::copy_file(context.fixture("resources/test-worlds/desk.furniture.yaml"), path.parent_path() / "desk.furniture.yaml");
		auto desks = std::make_shared<core::World>("Desk editor", 8, 2);
		auto deskRoom = desks->addRoom("Room", 0, 0, 0, 8, 1);
		desks->finishBuild(); desks->pauseSimulation(); desks->saveTo(path.string());
		DocumentHistory deskHistory;
		require(selectFurnitureCatalogue(desks, path, "desk.furniture.yaml", diagnostic, deskHistory), diagnostic);
		require(placeSelectedFurniture(desks, deskRoom, "desk", 2.125f, 0, false, "Desk", diagnostic, deskHistory, 2), diagnostic);
		auto deskId = desks->furniture().front().id; auto deskSeat = desks->furniture().front().marker;
		require(desks->furniture().front().localDepth == 2 && desks->getMarkerIds().size() == 1,
			"Editor did not place chosen depth or exposed routing-only destinations");
		require(placeSelectedFurniture(desks, deskRoom, "desk", 2.125f, 0, false, "Behind", diagnostic, deskHistory, 3), diagnostic);
		auto depthBefore = captureDocumentSnapshot(desks, deskHistory)->yaml;
		count = deskHistory.undoCount();
		for (int invalidDepth : { -1, 3 })
			require(!editSelectedFurniture(desks, deskId, 2.125f, 0, false, "Desk", diagnostic, deskHistory, invalidDepth)
				&& !diagnostic.empty() && deskHistory.undoCount() == count
				&& captureDocumentSnapshot(desks, deskHistory)->yaml == depthBefore, "Refused depth edit mutated history/document");
		require(!placeSelectedFurniture(desks, deskRoom, "desk", 5, 0, false, "Negative", diagnostic, deskHistory, -1)
			&& deskHistory.undoCount() == count, "Negative placement acquired history");
		require(editSelectedFurniture(desks, deskId, 2.125f, 0, false, "Desk", diagnostic, deskHistory, 4), diagnostic);
		auto deskCatalogue = desks->furnitureCatalogue();
		auto restoreDesk = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = deskCatalogue;
			auto restored = desks->deserialize(*reader, work); desks->pauseSimulation(); return restored;
		};
		require(deskHistory.undo(captureDocumentSnapshot(desks, deskHistory), restoreDesk)
			&& desks->furniture().front().localDepth == 2, "Depth edit undo failed");
		require(deskHistory.redo(captureDocumentSnapshot(desks, deskHistory), restoreDesk)
			&& desks->furniture().front().localDepth == 4 && desks->furniture().front().marker == deskSeat,
			"Depth edit redo lost depth or Marker identity");
		auto walkerId = desks->createAgent("History visitor", deskRoom, 0, 0.5f);
		auto walker = desks->lookupAgent(walkerId).entity;
		std::shared_ptr<const core::Vertex> seat;
		for (auto const& vertex : desks->getGraph()->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
				marker && marker->getId() == deskSeat) seat = vertex;
		walker->setPath(desks->getGraph()->calculatePath(walker, seat), true);
		require(desks->resumeSimulation(), "History movement resume failed"); desks->advanceTicks(10);
		auto moving = walker->getGlobalPosition(); desks->pauseSimulation();
		require(editSelectedFurniture(desks, deskId, 4.125f, 0, false, "Moved", diagnostic, deskHistory, 5), diagnostic);
		walker = desks->lookupAgent(walkerId).entity;
		require(walker->getGlobalPosition() == moving && walker->getLocalDepth() == 0,
			"Editor action teleported underway Agent");
		require(deskHistory.undo(captureDocumentSnapshot(desks, deskHistory), restoreDesk), "Moving destination undo failed");
		require(deskHistory.redo(captureDocumentSnapshot(desks, deskHistory), restoreDesk), "Moving destination redo failed");
		walker = desks->lookupAgent(walkerId).entity;
		core::World::TopologyPathIntent intent;
		require(walker->getGlobalPosition().x == 0.5f && desks->getPausedPathIntent(*walker, intent)
			&& intent.destinationMarker == deskSeat, "History lost authored origin or stable destination intent");
		require(desks->resumeSimulation(), "Restored history resume failed"); desks->advanceTicks(2000);
		require(walker->getGlobalPosition().x == 4.875f && walker->getLocalDepth() == 5,
			"History destination did not reach moved Furniture");
		walker->clearPath(); desks->pauseSimulation();
		auto stationary = walker->getGlobalPosition();
		require(editSelectedFurniture(desks, deskId, 4.125f, 0, false, "New depth", diagnostic, deskHistory, 7), diagnostic);
		for (bool redo : { false, true })
		{
			require(redo ? deskHistory.redo(captureDocumentSnapshot(desks, deskHistory), restoreDesk)
				: deskHistory.undo(captureDocumentSnapshot(desks, deskHistory), restoreDesk), "Stationary depth history failed");
			walker = desks->lookupAgent(walkerId).entity;
			require(walker->getGlobalPosition() == stationary && walker->getLocalDepth() == 5
				&& desks->furniture().front().marker == deskSeat, "Depth history changed stationary position/depth/identity");
		}
		require(deleteSelectedFurniture(desks, deskId, diagnostic, deskHistory), diagnostic);
		require(deskHistory.undo(captureDocumentSnapshot(desks, deskHistory), restoreDesk), "Stationary deletion undo failed");
		require(deskHistory.redo(captureDocumentSnapshot(desks, deskHistory), restoreDesk), "Stationary deletion redo failed");
		walker = desks->lookupAgent(walkerId).entity;
		require(walker->getGlobalPosition() == stationary && walker->getLocalDepth() == 5
			&& !desks->lookupMarker(deskSeat), "Deletion history changed retained stationary history");
		ImGui::GetIO().DisplaySize = {800, 600}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Furniture actions");
		renderFurniturePanel(world, path, world->getSector(room));
		ImGui::End(); ImGui::EndFrame();
	}
}
void editor_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/demoActions", demoActions });
	checks.push_back({ "furniture/chairActions", chairActions });
	checks.push_back({ "furniture/attachmentActions", attachmentActions });
	checks.push_back({ "furniture/compositionActions", compositionActions });
}
