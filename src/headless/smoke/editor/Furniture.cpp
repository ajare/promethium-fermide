#include "Checks.h"
#include "State.h"
#include "FurniturePanel.h"
#include "LocationPlan.h"
#include "PaletteLayout.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "core/YamlSerializer.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include "core/AgentTagRegistryDocument.h"
#include <fstream>
#include <limits>
#include <yaml-cpp/yaml.h>

namespace
{
	void bundledLuaWorkflow(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot() / "bundled-editor"; std::filesystem::create_directories(root);
		for (auto name : {"chair", "desk", "layouts", "attachments", "composition", "furniture", "furniture-integration"})
		{
			auto filename = std::string(name) + ".furniture.lua";
			std::filesystem::copy_file(context.fixture("resources/test-worlds/" + filename), root / filename);
			auto path = root / (std::string(name) + ".world.yaml");
			auto world = std::make_shared<core::World>("Bundled editor", 24, 3);
			world->addRoom("Room", 0, 0, 0, 24, 2); world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
			DocumentHistory history; std::string diagnostic;
			require(selectFurnitureCatalogue(world, path, filename, diagnostic, history), diagnostic);
			auto catalogue = world->furnitureCatalogue();
			for (auto const& [key, definition] : catalogue->definitions())
			{
				require(placeSelectedFurniture(world, 0, key, 4.125f, 0, false, "Example", diagnostic, history, 4), diagnostic);
				auto instance = world->furniture().back();
				require(selectFurnitureInstance(world, instance.id), "Cannot select converted definition " + filename + "/" + key);
				for (auto suffix : {"world.yaml", "world"})
				{
					auto output = root / (std::string(name) + "-" + key + "." + suffix); world->saveTo(output.string());
					auto loaded = core::loadWorldDocument(output);
					require(loaded->furnitureCatalogueFilename() == filename && loaded->furniture().back().id == instance.id
						&& loaded->furniture().back().marker == instance.marker && loaded->furniture().back().localDepth == 4,
						"Converted editor placement lost identity/depth on reopen");
				}
				require(deleteSelectedFurniture(world, instance.id, diagnostic, history), diagnostic);
			}
		}
	}

	void luaWorkflow(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot() / "lua-editor"; std::filesystem::create_directories(root);
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/objects.furniture.lua"), root / "objects.furniture.lua");
		auto path = root / "editor.world.yaml";
		auto world = std::make_shared<core::World>("Lua authoring", 12, 2);
		world->addRoom("Room", 0, 0, 0, 12, 1); world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history; std::string diagnostic;
		require(selectFurnitureCatalogue(world, path, "objects.furniture.lua", diagnostic, history), diagnostic);
		require(history.undoCount() == 1, "Lua catalogue selection missed history");
		require(placeSelectedFurniture(world, 0, "desk", 2.125f, 0, false, "Desk", diagnostic, history, 2), diagnostic);
		auto desk = world->furniture().front();
		require(selectFurnitureInstance(world, desk.id), "Cannot select Lua instance");
		require(editSelectedFurniture(world, desk.id, 4.125f, 0, false, "Edited", diagnostic, history, 3), diagnostic);
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto loaded = deserializeDocumentSnapshot(snapshot, world, path);
			if (!loaded) return false;
			world = std::move(loaded); world->pauseSimulation(); return true;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().front().x == 2.125f && world->furniture().front().marker == desk.marker,
			"Lua edit undo lost identity");
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().empty(), "Lua placement undo failed");
		require(history.undo(captureDocumentSnapshot(world, history), restore) && !world->furnitureCatalogue(), "Lua selection undo failed");
		for (int i = 0; i < 3; ++i) require(history.redo(captureDocumentSnapshot(world, history), restore), "Lua redo failed");
		require(world->furniture().front().marker == desk.marker && world->furniture().front().x == 4.125f,
			"Lua history reconstructed different authored data");
		auto before = captureDocumentSnapshot(world, history)->yaml; auto count = history.undoCount();
		{ std::ofstream output(root / "broken.furniture.lua"); output << "while true do end"; }
		for (auto filename : {"broken.furniture.lua", "missing.furniture.lua", "../objects.furniture.lua"})
			require(!selectFurnitureCatalogue(world, path, filename, diagnostic, history) && !diagnostic.empty()
				&& history.undoCount() == count && captureDocumentSnapshot(world, history)->yaml == before,
				"Rejected Lua selection modified editor history");
		require(!placeSelectedFurniture(world, 0, "desk", 5, 0, false, "Overlap", diagnostic, history, 3)
			&& history.undoCount() == count && captureDocumentSnapshot(world, history)->yaml == before,
			"Rejected Lua placement modified history");
		for (auto suffix : {"world.yaml", "world"})
		{
			auto output = root / (std::string("edited.") + suffix); world->saveTo(output.string());
			auto loaded = core::loadWorldDocument(output);
			require(loaded->furnitureCatalogueFilename() == "objects.furniture.lua" && loaded->furniture().front().marker == desk.marker,
				"Editor Lua save/reopen lost package or Marker identity");
		}
		require(deleteSelectedFurniture(world, desk.id, diagnostic, history), diagnostic);
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().front().marker == desk.marker,
			"Lua deletion undo lost identity");
	}

	void demoActions(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot();
		std::filesystem::copy_file(context.fixture("resources/test-worlds/furniture.furniture.lua"), root / "furniture.furniture.lua");
		auto path = root / "demo.world.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/furniture.world.yaml"), path);
		auto world = core::loadWorldDocument(path); world->pauseSimulation();
		DocumentHistory history; std::string diagnostic;
		// The bundled demo's Room is sector 0; keep this chair clear of its desk/chair span.
		// Depth 1 also supports the teaching chair's relative depth -1 front route.
		auto placed = placeSelectedFurniture(world, 0, "chair", 5.125f, 0, false, "Authored chair", diagnostic, history, 1);
		require(placed, "Demo chair placement failed: " + diagnostic);
		auto chair = world->furniture().back();
		auto edited = editSelectedFurniture(world, chair.id, 5.375f, 0, false, "Edited chair", diagnostic, history);
		require(edited, "Demo chair edit failed: " + diagnostic);
		auto catalogue = world->furnitureCatalogue();
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work); world->pauseSimulation(); return restored;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().back().x == 5.125f,
			"Bundled authoring workflow cannot undo movement");
		require(history.redo(captureDocumentSnapshot(world, history), restore)
			&& world->furniture().back().marker == chair.marker && world->furniture().back().x == 5.375f,
			"Bundled authoring history lost placement/identity");
		auto renamed = world->renameMarker(chair.marker, "Authored destination", &diagnostic);
		require(renamed, "Demo destination rename failed: " + diagnostic);
		world->addSectorMarker(0, 0, 0.125f, "Room entrance");
		auto rebuilt = world->rebuildTraversalTopology();
		require(rebuilt, "Demo topology rebuild failed: " + world->getTopologyDiagnostic());
		std::shared_ptr<const core::Vertex> destination;
		for (auto const& vertex : world->getGraph()->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getId() == chair.marker)
				destination = vertex;
		require(destination != nullptr, "Editor-placed sample destination Marker is missing");
		auto const destinationPosition = destination->getPosition();
		auto visitorId = world->createAgent("Authored visitor", 0, 0, 0.125f);
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
			require(reopened->lookupAgent(visitorId).entity->getGlobalPosition() == destinationPosition,
				"Reopened editor-authored journey did not arrive");
		}
	}

	void catalogueReattachmentHistory(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto root = context.temporaryRoot() / "catalogue-reattachment";
		std::filesystem::create_directory(root);
		auto path = root / "catalogue-history.world.yaml";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"),
			root / "chair.furniture.yaml");
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/layouts.furniture.yaml"),
			root / "layouts.furniture.yaml");
		auto world = std::make_shared<core::World>("Catalogue history", 8, 2);
		world->addRoom("Room", 0, 0, 0, 8, 1);
		world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history;
		std::string diagnostic;
		require(selectFurnitureCatalogue(world, path, "chair.furniture.yaml", diagnostic, history), diagnostic);
		auto const chairUuid = world->furnitureCatalogue()->uuid();
		require(selectFurnitureCatalogue(world, path, "layouts.furniture.yaml", diagnostic, history), diagnostic);
		auto const layoutsUuid = world->furnitureCatalogue()->uuid();
		require(chairUuid != layoutsUuid, "Catalogue reattachment regression needs distinct UUIDs");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = deserializeDocumentSnapshot(snapshot, world, path);
			if (!loaded) return false;
			world = std::move(loaded);
			return true;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->furnitureCatalogueFilename() == "chair.furniture.yaml"
			&& world->furnitureCatalogue()->uuid() == chairUuid,
			"Undo did not reattach the snapshot's Furniture catalogue");
		require(history.redo(captureDocumentSnapshot(world, history), restore)
			&& world->furnitureCatalogueFilename() == "layouts.furniture.yaml"
			&& world->furnitureCatalogue()->uuid() == layoutsUuid,
			"Redo did not reattach the snapshot's Furniture catalogue");

		// Keep this definition-selection test independent of teaching-layout depth requirements.
		auto sample = YAML::LoadFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/furniture.furniture.yaml").string());
		auto legacyChair = YAML::LoadFile((root / "chair.furniture.yaml").string());
		sample["furnitureCatalogue"]["definitions"][0] = legacyChair["furnitureCatalogue"]["definitions"][0];
		{ std::ofstream file(root / "furniture.furniture.yaml"); file << sample; }
		require(selectFurnitureCatalogue(world, path, "furniture.furniture.yaml", diagnostic, history), diagnostic);
		world->saveTo(path.string());
		world = core::loadWorldDocument(path); world->pauseSimulation();
		auto canPlaceChair = world->canPlaceFurniture(0, "chair", 2, 0, "New chair", &diagnostic, 0);
		require(canPlaceChair, "Chair placement at x=2, Level=0, depth=0 was rejected: " + diagnostic);
		require(world->furnitureCatalogueFilename() == "furniture.furniture.yaml",
			"Save/reopen reverted the selected Furniture catalogue");
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = {1000, 800};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text;
		auto previousClipboardData = io.ClipboardUserData;
		auto previousSetClipboardText = io.SetClipboardTextFn;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		auto panelShows = [&](std::string const& filename)
		{
			text.clear();
			ImGui::NewFrame(); ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({900, 700});
			ImGui::Begin("Reopened Furniture catalogue", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::LogToClipboard(); ImGui::SetNextItemOpen(true);
			renderFurniturePanel(world, path);
			ImGui::LogFinish(); ImGui::End(); ImGui::Render();
			require(text.find("Catalogue: " + filename) != std::string::npos,
				"Furniture panel does not display the loaded catalogue: " + text);
		};
		panelShows("furniture.furniture.yaml");
		require(text.find("Select Furniture catalogue...") != std::string::npos
			&& text.find("Place Furniture") == std::string::npos && text.find("Furniture definition") == std::string::npos,
			"Catalogue panel still exposes Furniture editing controls: " + text);
		require(selectFurnitureCatalogue(world, path, "layouts.furniture.yaml", diagnostic, history), diagnostic);
		panelShows("layouts.furniture.yaml");
		require(text.find("Furniture definition") == std::string::npos,
			"Catalogue change reintroduced Furniture editing controls: " + text);
		require(selectFurnitureCatalogue(world, path, "chair.furniture.yaml", diagnostic, history), diagnostic);
		panelShows("chair.furniture.yaml");
		world = core::loadWorldDocument(path);
		panelShows("furniture.furniture.yaml");
		// Release the callback's borrowed string before this check returns.
		io.ClipboardUserData = previousClipboardData; io.SetClipboardTextFn = previousSetClipboardText;
	}

	void cataloguePicker(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto root = context.temporaryRoot() / "catalogue-picker";
		std::filesystem::create_directories(root / "elsewhere");
		for (auto filename : {"chair.furniture.yaml", "desk.furniture.yaml"})
			std::filesystem::copy_file(context.fixture(std::string("src/headless/smoke/fixtures/legacy-furniture/") + filename), root / filename);
		std::filesystem::copy_file(root / "chair.furniture.yaml", root / "elsewhere/chair.furniture.yaml");
		std::filesystem::copy_file(root / "chair.furniture.yaml", root / "wrong.yaml");
		{ std::ofstream output(root / "broken.furniture.yaml"); output << "not a catalogue"; }
		auto path = root / "picker.world.yaml";
		auto world = std::make_shared<core::World>("Picker", 8, 2);
		world->addRoom("Room", 0, 0, 0, 8, 1); world->finishBuild(); world->pauseSimulation(); world->saveTo(path.string());
		DocumentHistory history;
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1000, 800}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text; io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		std::optional<std::string> picked; unsigned choices = 0; bool pickerError = false;
		auto frame = [&]
		{
			text.clear(); ImGui::NewFrame(); ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({600, 350});
			ImGui::Begin("Catalogue picker test"); ImGui::LogToClipboard(); ImGui::SetNextItemOpen(true);
			renderFurniturePanel(world, path, [&]() -> std::optional<std::string>
			{
				++choices;
				if (pickerError) throw std::runtime_error("Native picker failed");
				return picked;
			}, history);
			ImGui::LogFinish(); ImGui::End(); ImGui::Render();
		};
		frame(); frame();
		auto button = ImGui::FindWindowByName("Catalogue picker test")->GetID("Select Furniture catalogue...");
		ImVec2 buttonPosition{}; bool found = false;
		for (float y = 35; y < 140 && !found; y += 8)
			for (float x = 20; x < 270 && !found; x += 16)
			{
				io.AddMousePosEvent(x, y); frame(); frame();
				if (ImGui::GetHoveredID() == button) { found = true; buttonPosition = {x, y}; }
			}
		require(found, "Catalogue picker button is missing");
		auto click = [&]
		{
			io.AddMousePosEvent(buttonPosition.x, buttonPosition.y); frame();
			io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame(); frame();
		};
		picked = (root / "chair.furniture.yaml").string(); click();
		require(choices == 1 && world->furnitureCatalogueFilename() == "chair.furniture.yaml"
			&& history.undoCount() == 1 && world->furniture().empty(), "Picker did not attach the selected catalogue as one edit");
		for (auto label : {"Furniture definition", "Furniture instance", "Instance name", "Furniture x", "Supporting Level",
			"Furniture Local depth", "Snap Furniture", "Place Furniture", "Apply Furniture edit", "Delete Furniture", "Catalogue beside World"})
			require(text.find(label) == std::string::npos, "Catalogue-only header retained editing control: " + std::string(label));
		auto unchanged = captureDocumentSnapshot(world, history)->yaml;
		picked.reset(); click();
		require(choices == 2 && captureDocumentSnapshot(world, history)->yaml == unchanged && history.undoCount() == 1,
			"Cancelled picker mutated the document/history");
		for (auto file : {"elsewhere/chair.furniture.yaml", "wrong.yaml", "broken.furniture.yaml"})
		{
			picked = (root / file).string(); click();
			require(captureDocumentSnapshot(world, history)->yaml == unchanged && history.undoCount() == 1
				&& text.find("Catalogue: chair.furniture.yaml") != std::string::npos,
				"Rejected picker selection changed the catalogue/history");
			if (std::string(file).starts_with("elsewhere"))
				require(text.find("beside the World") != std::string::npos, "Outside-directory selection lost its diagnostic");
		}
		pickerError = true; click(); pickerError = false;
		require(text.find("Native picker failed") != std::string::npos && history.undoCount() == 1,
			"Picker exception escaped the panel or committed history");
		picked = (root / "desk.furniture.yaml").string(); click();
		require(world->furnitureCatalogueFilename() == "desk.furniture.yaml" && history.undoCount() == 2
			&& text.find("Native picker failed") == std::string::npos, "Successful selection retained an error or missed history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = deserializeDocumentSnapshot(snapshot, world, path);
			if (!loaded) return false;
			world = std::move(loaded); return true;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Picker catalogue undo failed"); frame();
		require(text.find("Catalogue: chair.furniture.yaml") != std::string::npos, "Panel did not refresh catalogue after undo");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Picker catalogue redo failed"); frame();
		require(text.find("Catalogue: desk.furniture.yaml") != std::string::npos, "Panel did not refresh catalogue after redo");
		auto savedPath = path; path.clear(); auto beforeChoices = choices; click();
		require(choices == beforeChoices && history.undoCount() == 2 && text.find("Save the World") != std::string::npos,
			"Unsaved World opened a catalogue picker or mutated history");
		path = savedPath;
		io.ClipboardUserData = nullptr; io.SetClipboardTextFn = nullptr;
	}

	void attachmentActions(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto path = context.temporaryRoot() / "attachment.world.yaml";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/attachments.furniture.yaml"), path.parent_path() / "attachments.furniture.yaml");
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/composition.furniture.yaml"), path.parent_path() / "composition.furniture.yaml");
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"), path.parent_path() / "chair.furniture.yaml");
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/layouts.furniture.yaml"), path.parent_path() / "layouts.furniture.yaml");
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/desk.furniture.yaml"), path.parent_path() / "desk.furniture.yaml");
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
		renderFurniturePanel(world, path);
		ImGui::End(); ImGui::EndFrame();
	}
}
namespace
{
	void locationPlanPlacement(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto world = std::make_shared<core::World>("Plan placement", 24, 8);
		auto room = world->addRoom("Pinned", 1, 3, 7, 6, 3);
		auto other = world->addRoom("Other", 0, 0, 0, 6, 1);
		for (uint32_t x = 0; x < 4; ++x) world->addSectorWalkway(room, 1, x);
		world->finishBuild(); world->pauseSimulation();
		auto catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		world->attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		DocumentHistory history;
		LocationPlan plan; require(plan.open(world, world->getSector(room), 4), "Cannot open placement plan");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1600, 1000}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImVec2 tray{20, 430}, viewport{}, viewportSize{};
		std::vector<std::string> rowLabels, depths;
		unsigned previews = 0, invalidPreviews = 0;
		bool paletteHovered = true;
		auto frame = [&]
		{
			rowLabels.clear(); depths.clear(); previews = invalidPreviews = 0;
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({10, 420}); ImGui::SetNextWindowSize({1300, 200});
			ImGui::Begin("Palette workflow", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::InvisibleButton("##WorldCanvas", ImGui::GetContentRegionAvail());
			WorldDrawList row({{0, 0}, {1600, 1000}});
			plan.renderPaletteRow(world, row, tray, paletteHovered);
			for (auto const& command : row.commands())
				if (auto text = std::get_if<WorldDrawList::Text>(&command)) rowLabels.push_back(text->value);
			ImGui::End();
			ImGui::SetNextWindowPos({300, 10}); ImGui::SetNextWindowSize({600, 360});
			plan.render(world, [&](WorldDrawList const& commands, ImVec2 pos, ImVec2 size)
			{
				viewport = pos; viewportSize = size;
				for (auto const& command : commands.commands())
				{
					if (auto text = std::get_if<WorldDrawList::Text>(&command))
						if (std::abs(text->position.x - pos.x - 16) < .01f) depths.push_back(text->value);
					if (auto line = std::get_if<WorldDrawList::Line>(&command))
						{
							if (line->colour == IM_COL32(80, 200, 120, 255) || line->colour == IM_COL32(244, 67, 54, 255)) ++previews;
							if (line->colour == IM_COL32(244, 67, 54, 255)) ++invalidPreviews;
						}
				}
			}, history);
			ImGui::GetCurrentContext()->NextWindowData.ClearFlags(); ImGui::Render();
		};
		frame(); frame();
		auto mouse = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frame(); frame(); };
		auto slot = paletteFurnitureSlotMin(tray, 0);
		auto start = [&]
		{
			paletteHovered = true; mouse({slot.x + 20, slot.y + 18});
			io.AddMouseButtonEvent(0, true); frame();
			paletteHovered = false;
		};
		auto point = [&](float x, float depth)
		{
			return ImVec2{viewport.x + 48 + x * (viewportSize.x - 60) / 6,
				viewport.y + viewportSize.y - 28 - (depth + .5f) * (viewportSize.y - 36) / depths.size()};
		};
		auto release = [&] { io.AddMouseButtonEvent(0, false); frame(); frame(); };
		require(rowLabels == std::vector<std::string>{"Chair"}, "Palette does not reflect catalogue");
		// Neither palette focus nor ordinary Selection changes the pinned target.
		start(); mouse(point(1.3f, 3));
		require(previews == 4 && world->furniture().empty() && history.undoCount() == 0,
			"Preview missing or mutated authored state");
		require(invalidPreviews == 0, "Supported drop preview is marked invalid");
		io.AddKeyEvent(ImGuiMod_Shift, true); frame(); release();
		require(world->furniture().size() == 1 && world->furniture().back().sector == room
			&& world->furniture().back().x == 1 && world->furniture().back().y == 1
			&& world->furniture().back().localDepth == 3 && world->furniture().back().name == "Chair"
			&& history.undoCount() == 1 && depths.size() == 5,
			"Shift drop did not snap, convert Level, expand depth, or commit exactly once");
		auto first = world->furniture().back();
		start(); mouse(point(2.2f, 2)); release();
		require(world->furniture().back().name == "Chair 2" && history.undoCount() == 2,
			"Repeated drop did not generate a unique name");
		io.AddKeyEvent(ImGuiMod_Shift, false); frame();
		auto second = world->furniture().back();
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto result = world->deserialize(*reader, work); world->pauseSimulation(); return result;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().size() == 1,
			"Drop undo failed"); frame();
		require(history.redo(captureDocumentSnapshot(world, history), restore) && world->furniture().back().id == second.id
			&& world->furniture().back().marker == second.marker && world->furniture().front().marker == first.marker,
			"Drop redo changed instance/Marker identities"); frame();
		world->addSectorMarker(other, 0, .5f, "Chair 3 Seat"); world->finishBuild();
		start(); mouse(point(0, 0)); release();
		require(world->furniture().back().name == "Chair 4", "Generated name collided with a standalone owned-Marker name");
		auto reject = [&](ImVec2 destination, bool onGrid = true)
		{
			auto before = captureDocumentSnapshot(world, history)->yaml; auto count = history.undoCount();
			start(); mouse(destination);
			require(onGrid ? invalidPreviews == 4 : previews == 0,
				"Rejected drop did not show invalid preview, or another surface accepted it");
			release();
			require(captureDocumentSnapshot(world, history)->yaml == before && history.undoCount() == count,
				"Rejected drop mutated World or history");
		};
		reject(point(1, 3)); // overlap
		reject(point(4, 1)); // unsupported upper-Level Floor
		reject(point(5.8f, 0)); // snapped outside Location
		reject({50, 300}, false); // another UI/World surface
		reject({viewport.x + 20, viewport.y + 20}, false); // grid label margin
		// Replacement catalogue invalidates an in-flight drag even with the same key.
		start(); mouse(point(0, 0));
		std::string diagnostic;
		while (!world->furniture().empty())
			require(deleteSelectedFurniture(world, world->furniture().back().id, diagnostic, history), diagnostic);
		catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		world->attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		auto count = history.undoCount(); release();
		require(world->furniture().empty() && history.undoCount() == count, "Stale catalogue drag placed Furniture");
		start(); mouse(point(0, 0)); release();
		require(world->furniture().size() == 1 && world->furniture().back().sector != other,
			"Reattached catalogue cannot place into pinned Location");
		auto root = context.temporaryRoot() / "location-plan-placement";
		std::filesystem::create_directories(root);
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"), root / "chair.furniture.yaml");
		auto path = root / "plan.world.yaml"; world->saveTo(path.string());
		auto reopened = core::loadWorldDocument(path);
		require(reopened->furniture().size() == 1 && reopened->furniture().front().marker == world->furniture().front().marker
			&& reopened->furniture().front().y == 1, "Plan placement did not survive ordinary save/reopen");
		while (!world->furniture().empty())
			require(deleteSelectedFurniture(world, world->furniture().back().id, diagnostic, history), diagnostic);
		// A catalogue wider than the tray is paged, never compressed/overlapped.
		auto many = YAML::LoadFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml").string());
		auto prototype = YAML::Clone(many["furnitureCatalogue"]["definitions"][0]);
		many["furnitureCatalogue"]["definitions"] = YAML::Node(YAML::NodeType::Sequence);
		for (unsigned i = 0; i < 15; ++i)
		{
			auto entry = YAML::Clone(prototype); entry["key"] = "chair" + std::to_string(10 + i);
			entry["label"] = "Chair " + std::to_string(10 + i);
			many["furnitureCatalogue"]["definitions"].push_back(entry);
		}
		auto manyPath = root / "many.furniture.yaml";
		{ std::ofstream output(manyPath); output << many; }
		catalogue = core::FurnitureCatalogue::readFile(manyPath);
		world->attachFurnitureCatalogue("many.furniture.yaml", catalogue); frame();
		require(rowLabels.size() == 12 && rowLabels.front() == "Chair 10" && rowLabels.back() == "Next page",
			"Large catalogue row does not fit tray or reset on replacement");
		auto next = paletteFurnitureSlotMin(tray, paletteColumnCount() - 1);
		paletteHovered = true; mouse({next.x + 20, next.y + 18});
		io.AddMouseButtonEvent(0, true); frame(); release();
		require(rowLabels.size() == 5 && rowLabels.front() == "Chair 21" && rowLabels[3] == "Chair 24",
			"Catalogue definitions beyond first page are inaccessible");
		start(); mouse(point(0, 0)); release();
		require(world->furniture().size() == 1 && world->furniture().back().definitionKey == "chair21",
			"Paged palette placed the wrong definition");
		start(); mouse(point(1, 0));
		world = std::make_shared<core::World>("No catalogue", 10, 4);
		auto emptyRoom = world->addRoom("Empty", 0, 0, 0, 6, 1); world->finishBuild();
		release(); require(rowLabels.empty() && world->furniture().empty(), "Replacement World retained stale plan/row/drag");
		require(plan.open(world, world->getSector(emptyRoom), 0), "Cannot open empty-catalogue plan"); frame();
		require(rowLabels == std::vector<std::string>{"No Furniture catalogue"} && plan.isOpen(world),
			"Absent catalogue removed plan row or retained definitions");
		plan.close(); frame(); require(rowLabels.empty(), "Closing plan retained third row");
	}

	void locationPlanHover(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto world = std::make_shared<core::World>("Hover", 20, 5);
		auto room = world->addRoom("Pinned", 0, 1, 3, 8, 2);
		for (uint32_t x = 0; x < 8; ++x) world->addSectorWalkway(room, 1, x);
		world->addSectorMarker(room, 0, 6.5f, "Standalone target");
		world->addSectorMarker(room, 1, 5.5f, "Other Level target");
		world->finishBuild(); world->pauseSimulation();
		world->attachFurnitureCatalogue("desk.furniture.yaml",
			core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/desk.furniture.yaml")));
		world->placeFurniture(room, "desk", 1.25f, 0, "Desk", 2);
		world->finishBuild();
		auto marker = world->furniture().front().marker;
		std::string diagnostic;
		require(world->renameMarker(marker, "Seat % named", &diagnostic), diagnostic);
		DocumentHistory history; LocationPlan plan;
		require(plan.open(world, world->getSector(room), 1), "Cannot open hover plan");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1200, 900}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		ImVec2 viewport{}, size{}; unsigned rows = 0;
		auto frame = [&]
		{
			text.clear(); rows = 0;
			ImGui::NewFrame(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({300, 10}); ImGui::SetNextWindowSize({600, 420});
			plan.render(world, [&](WorldDrawList const& commands, ImVec2 p, ImVec2 s)
			{
				viewport = p; size = s;
				for (auto const& command : commands.commands())
					if (auto label = std::get_if<WorldDrawList::Text>(&command);
						label && std::abs(label->position.x - p.x - 16) < .01f) ++rows;
			}, history);
			ImGui::GetCurrentContext()->NextWindowData.ClearFlags();
			ImGui::LogFinish(); ImGui::Render();
		};
		frame(); frame();
		auto point = [&](float x, float depth) { return ImVec2{viewport.x + 48 + x * (size.x - 60) / 8,
			viewport.y + size.y - 28 - depth * (size.y - 36) / rows}; };
		auto mouse = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frame(); frame(); };
		auto expectVertex = [&](float x, float depth, std::string const& name)
		{
			mouse(point(x, depth));
			require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand && text.find(name) != std::string::npos,
				"Vertex hover lost hand cursor or tooltip '" + name + "': " + text);
		};
		auto before = captureDocumentSnapshot(world, history)->yaml;
		// A vertex at the footprint boundary is tested before the Furniture quad.
		expectVertex(1.5f, 2, "Desk / frontLeft");
		expectVertex(2, 2, "Seat % named");
		auto overlap = point(2, 2); overlap.y -= 2; mouse(overlap);
		require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand && text.find("Seat % named") != std::string::npos,
			"Furniture hit testing outranked a vertex overlapping its quad");
		// Resolve the external port, not its coincident inferred floor anchor.
		expectVertex(1.25f, 0, "Desk / left");
		auto margin = point(1.25f, 0); margin.y += 2; mouse(margin);
		require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand && text.find("Desk / left") != std::string::npos,
			"Depth-0 vertex was not hoverable in the graph margin");
		expectVertex(6.5f, 0, "Standalone target");
		mouse(point(1.8f, 2.5f));
		require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Hand
			&& text.find("Seat % named") == std::string::npos && text.find("Desk / frontLeft") == std::string::npos,
			"Furniture hover lost hand cursor or retained a vertex tooltip");
		mouse(point(5.5f, 0));
		require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow && text.find("Other Level target") == std::string::npos,
			"Other Level's vertex received hover feedback");
		mouse(point(5, 2.5f)); require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow, "Empty plan kept hand cursor");
		require(captureDocumentSnapshot(world, history)->yaml == before && history.undoCount() == 0
			&& !selectedFurnitureInstance(world), "Hover mutated selection, World or history");
		// Rebuild/name changes must be resolved on the next hover, not from cached vertices.
		require(world->renameMarker(marker, "Renamed seat", &diagnostic), diagnostic);
		expectVertex(2, 2, "Renamed seat");
		require(world->removeFurniture(world->furniture().front().id, &diagnostic), diagnostic);
		mouse(point(2, 2));
		require(ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow && text.find("Renamed seat") == std::string::npos,
			"Removed Furniture retained stale vertex or quad hover");
		io.ClipboardUserData = nullptr; io.SetClipboardTextFn = nullptr;
	}

	void locationPlanMovement(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto world = std::make_shared<core::World>("Movement", 24, 8);
		auto room = world->addRoom("Pinned", 1, 3, 2, 8, 3);
		world->addRoom("Other", 0, 0, 0, 8, 1);
		for (uint32_t x = 0; x < 6; ++x) world->addSectorWalkway(room, 1, x);
		world->finishBuild(); world->pauseSimulation();
		auto catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		world->attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		auto id = world->placeFurniture(room, "chair", 1.25f, 1, "Selected chair", 0);
		world->placeFurniture(room, "chair", 4, 1, "Obstacle", 1);
		auto marker = world->furniture().front().marker;
		require(world->renameMarker(marker, "Authored seat"), "Cannot rename owned Marker");
		DocumentHistory history; LocationPlan plan;
		require(plan.open(world, world->getSector(room), 4), "Cannot open movement plan");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1600, 1000}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImVec2 viewport{}, size{}; size_t rows = 4; unsigned validLines = 0, invalidLines = 0, selectionLines = 0;
		float previewX = 0;
		auto frame = [&]
		{
			rows = 0; validLines = invalidLines = selectionLines = 0; previewX = std::numeric_limits<float>::max();
			ImGui::NewFrame();
			ImGui::SetNextWindowPos({300, 10}); ImGui::SetNextWindowSize({600, 360});
			plan.render(world, [&](WorldDrawList const& commands, ImVec2 p, ImVec2 s)
			{
				viewport = p; size = s;
				for (auto const& command : commands.commands())
				{
					if (auto text = std::get_if<WorldDrawList::Text>(&command))
						if (std::abs(text->position.x - p.x - 16) < .01f) ++rows;
					if (auto line = std::get_if<WorldDrawList::Line>(&command))
					{
						if (line->colour == IM_COL32(80, 200, 120, 255))
						{
							++validLines;
							previewX = std::min(previewX, (line->from.x - p.x - 48) * 8 / (s.x - 60));
						}
						if (line->colour == IM_COL32(244, 67, 54, 255)) ++invalidLines;
						if (line->colour == IM_COL32(251, 188, 4, 255)) ++selectionLines;
					}
				}
			}, history);
			ImGui::GetCurrentContext()->NextWindowData.ClearFlags(); ImGui::Render();
		};
		frame(); frame();
		auto point = [&](float x, float depth) { return ImVec2{viewport.x + 48 + x * (size.x - 60) / 8,
			viewport.y + size.y - 28 - (depth + .5f) * (size.y - 36) / rows}; };
		auto mouse = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frame(); frame(); };
		auto start = [&] { auto const& instance = world->furniture().front(); mouse(point(instance.x + .5f, instance.localDepth));
			io.AddMouseButtonEvent(0, true); frame(); };
		auto release = [&] { io.AddMouseButtonEvent(0, false); frame(); frame(); };
		start(); release();
		require(selectedFurnitureInstance(world) && selectedFurnitureInstance(world)->id == id && selectionLines == 4
			&& history.undoCount() == 0, "Plan click did not share/highlight selection or created an edit: selected="
			+ std::to_string(selectedFurnitureInstance(world) ? selectedFurnitureInstance(world)->id : 0)
			+ " lines=" + std::to_string(selectionLines) + " rows=" + std::to_string(rows));
		// Catalogue-only controls must leave the plan's selection untouched.
		std::string text;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		ImGui::NewFrame(); ImGui::SetNextWindowPos({950, 10}); ImGui::SetNextWindowSize({600, 700});
		ImGui::Begin("Movement controls"); ImGui::LogToClipboard();
		ImGui::SetNextItemOpen(true); renderFurniturePanel(world, {});
		ImGui::LogFinish(); ImGui::End(); ImGui::Render();
		require(text.find("Apply Furniture edit") == std::string::npos && text.find("Furniture x") == std::string::npos
			&& selectedFurnitureInstance(world) && selectedFurnitureInstance(world)->id == id,
			"Catalogue-only panel exposed editing or disturbed plan selection");
		frame();
		auto before = captureDocumentSnapshot(world, history)->yaml;
		start(); mouse(point(2.8f, 3));
		require(validLines == 4 && captureDocumentSnapshot(world, history)->yaml == before && history.undoCount() == 0,
			"Valid movement preview mutated state or was not presented");
		require(std::abs(previewX - 2) < .001f, "Ordinary movement preview did not snap X");
		io.AddKeyEvent(ImGuiMod_Shift, true); frame();
		require(std::abs(previewX - 2.3f) < .01f, "Pressing Shift did not immediately unsnap the preview: " + std::to_string(previewX));
		io.AddKeyEvent(ImGuiMod_Shift, false); frame();
		require(std::abs(previewX - 2) < .001f, "Releasing Shift did not immediately snap the preview"); release();
		require(world->furniture().front().x == 2 && world->furniture().front().localDepth == 3
			&& world->furniture().front().y == 1 && world->furniture().front().sector == room
			&& history.undoCount() == 1 && rows == 5 && selectedFurnitureInstance(world)
			&& selectedFurnitureInstance(world)->id == id, "Shift release did not snap or retain target/Level/one-edit expansion");
		start(); io.AddKeyEvent(ImGuiMod_Shift, true); frame(); mouse(point(3.875f, 2)); release();
		require(std::abs(world->furniture().front().x - 3.375f) < .001f && world->furniture().front().localDepth == 2
			&& history.undoCount() == 2 && rows == 5, "Shift movement lost fractional X/integer depth or shrank range");
		io.AddKeyEvent(ImGuiMod_Shift, false); frame();
		require(world->furniture().front().id == id && world->furniture().front().marker == marker
			&& world->lookupMarker(marker)->getName() == "Authored seat", "Movement changed instance or Marker identity/name");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto result = world->deserialize(*reader, work); world->pauseSimulation(); return result;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Movement undo failed"); frame();
		require(world->furniture().front().x == 2 && world->furniture().front().localDepth == 3
			&& !selectedFurnitureInstance(world), "Undo lost position/depth or retained stale selection");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Movement redo failed"); frame();
		require(world->furniture().front().id == id && world->furniture().front().marker == marker
			&& std::abs(world->furniture().front().x - 3.375f) < .001f, "Redo changed identity/position");
		auto root = context.temporaryRoot() / "location-plan-movement";
		std::filesystem::create_directories(root);
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"), root / "chair.furniture.yaml");
		auto path = root / "movement.world.yaml"; world->saveTo(path.string());
		auto reopened = core::loadWorldDocument(path);
		require(reopened->furniture().front().id == id && reopened->furniture().front().marker == marker
			&& reopened->furniture().front().localDepth == 2 && reopened->furniture().front().y == 1
			&& std::abs(reopened->furniture().front().x - 3.375f) < .001f
			&& reopened->lookupMarker(marker)->getName() == "Authored seat", "Moved Furniture did not survive save/reopen");
		auto reject = [&](float x, int depth)
		{
			auto snapshot = captureDocumentSnapshot(world, history)->yaml; auto count = history.undoCount();
			start(); mouse(point(x + .5f, depth));
			require(invalidLines == 4, "Invalid movement preview not presented"); release();
			require(captureDocumentSnapshot(world, history)->yaml == snapshot && history.undoCount() == count,
				"Rejected movement changed authored state, topology or history");
		};
		reject(4, 1); reject(6, 0); reject(7, 0);
		start(); mouse(point(2.5f, 0));
		auto count = history.undoCount(); auto snapshot = captureDocumentSnapshot(world, history);
		require(restore(*snapshot), "Cannot reconstruct World during drag"); release();
		require(history.undoCount() == count && std::abs(world->furniture().front().x - 3.375f) < .001f,
			"Reconstructed World accepted stale drag");
		start(); mouse(point(2.5f, 0));
		catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		require(restore(*captureDocumentSnapshot(world, history)), "Cannot reload catalogue through World reconstruction"); release();
		require(!selectedFurnitureInstance(world) && history.undoCount() == count, "Catalogue switch retained selection/gesture");
		start(); mouse(point(2.5f, 0)); std::string diagnostic;
		require(world->removeFurniture(id, &diagnostic), diagnostic); release();
		require(!selectedFurnitureInstance(world) && history.undoCount() == count, "Deleted instance retained selection/gesture");
		io.ClipboardUserData = nullptr; io.SetClipboardTextFn = nullptr;
	}

	void locationPlanDeletion(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto world = std::make_shared<core::World>("Deletion", 20, 6);
		auto room = world->addRoom("Pinned", 0, 1, 3, 6, 2);
		auto other = world->addRoom("Other", 1, 0, 0, 6, 1);
		world->finishBuild(); world->pauseSimulation();
		auto catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		world->attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		auto id = world->placeFurniture(room, "chair", 1, 0, "Deep chair", 6);
		auto marker = world->furniture().front().marker;
		auto otherId = world->placeFurniture(other, "chair", 1, 0, "Other chair", 0);
		std::string diagnostic;
		require(world->renameMarker(marker, "Authored seat", &diagnostic), diagnostic);
		auto registry = core::AgentBehaviourRegistry::create();
		world->attachAgentBehaviourRegistry("editor.behaviours", registry);
		auto behaviour = registry->addAgentBehaviour("Visit", "visit.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
		});
		auto agent = world->createAgent("Plan visitor", room, 0, .5f);
		require(world->setAgentBehaviourAssignment(agent, behaviour, 1, {{"destination", marker}}, &diagnostic), diagnostic);
		DocumentHistory history; LocationPlan plan;
		require(plan.open(world, world->getSector(room), 1), "Cannot open deletion plan");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1200, 800}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		ImVec2 viewport{}, size{}; unsigned rows = 0, outlines = 0;
		std::vector<std::string> labels;
		auto frame = [&]
		{
			text.clear(); rows = outlines = 0; labels.clear();
			ImGui::NewFrame(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({300, 10}); ImGui::SetNextWindowSize({600, 420});
			plan.render(world, [&](WorldDrawList const& commands, ImVec2 p, ImVec2 s)
			{
				viewport = p; size = s;
				for (auto const& command : commands.commands())
				{
					if (auto label = std::get_if<WorldDrawList::Text>(&command))
					{
						labels.push_back(label->value);
						if (std::abs(label->position.x - p.x - 16) < .01f) ++rows;
					}
					if (auto line = std::get_if<WorldDrawList::Line>(&command))
						if (line->colour == IM_COL32(251, 188, 4, 255)) ++outlines;
				}
			}, history);
			ImGui::GetCurrentContext()->NextWindowData.ClearFlags();
			ImGui::LogFinish(); ImGui::Render();
		};
		auto click = [&](ImVec2 p)
		{
			io.AddMousePosEvent(p.x, p.y); frame(); frame();
			io.AddMouseButtonEvent(0, true); frame();
			io.AddMouseButtonEvent(0, false); frame(); frame();
		};
		frame(); frame();
		require(rows == 8, "Deep Furniture did not expand plan before deletion");
		click({viewport.x + 48 + 1.5f * (size.x - 60) / 6,
			viewport.y + size.y - 28 - 6.5f * (size.y - 36) / rows});
		require(selectedFurnitureInstance(world) && selectedFurnitureInstance(world)->id == id && outlines == 4,
			"Plan did not select intended Furniture for deletion");
		// Locate the production button using ImGui's item identity, not controller internals.
		auto* window = ImGui::FindWindowByName("Location plan");
		auto button = window->GetID("Delete selected Furniture");
		ImVec2 deletePosition{}; bool found = false;
		for (float y = window->Pos.y + 50; y < viewport.y && !found; y += 4)
			for (float x = window->Pos.x + 10; x < window->Pos.x + 260; x += 16)
			{
				io.AddMousePosEvent(x, y); frame(); frame();
				if (ImGui::GetHoveredID() == button) { deletePosition = {x, y}; found = true; break; }
			}
		require(found, "Plan does not expose deletion control");
		auto before = captureDocumentSnapshot(world, history)->yaml;
		click(deletePosition);
		require(text.find("Plan visitor") != std::string::npos && text.find("destination") != std::string::npos
			&& captureDocumentSnapshot(world, history)->yaml == before && history.undoCount() == 0
			&& selectedFurnitureInstance(world) && selectedFurnitureInstance(world)->id == id && outlines == 4,
			"Refused plan deletion lost diagnostic/selection/reference or changed World/history");
		require(world->clearAgentBehaviourAssignment(agent, &diagnostic), diagnostic);
		click(deletePosition);
		require(world->furniture().size() == 1 && world->furniture().front().id == otherId
			&& !world->lookupMarker(marker) && !selectedFurnitureInstance(world) && outlines == 0
			&& history.undoCount() == 1 && rows == 8 && text.find("Plan visitor") == std::string::npos
			&& std::find(labels.begin(), labels.end(), "Deep chair") == labels.end(),
			"Successful deletion left owned content/selection, removed wrong instance, or shrank range");
		// A stale selection or release cannot repeat deletion or finish a prior gesture.
		click(deletePosition); frame();
		require(history.undoCount() == 1 && world->furniture().size() == 1, "Stale deletion created an edit");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto result = world->deserialize(*reader, work); world->pauseSimulation(); return result;
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Plan deletion undo failed"); frame();
		require(world->furniture().front().id == id && world->furniture().front().marker == marker
			&& world->lookupMarker(marker)->getName() == "Authored seat" && rows == 8
			&& !selectedFurnitureInstance(world) && text.find("Location: Pinned") != std::string::npos
			&& std::find(labels.begin(), labels.end(), "Deep chair") != labels.end(),
			"Undo lost instance/Marker identity, pinned target, or restored presentation");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Plan deletion redo failed"); frame();
		require(world->furniture().size() == 1 && world->furniture().front().id == otherId
			&& !world->lookupMarker(marker) && !selectedFurnitureInstance(world) && rows == 8
			&& text.find("Location: Pinned") != std::string::npos,
			"Redo left stale owned content/selection or retargeted/shrank plan");
		// Shared panel selection outside this plan must not be deleted by its control.
		require(selectFurnitureInstance(world, otherId), "Cannot select other Location's Furniture");
		before = captureDocumentSnapshot(world, history)->yaml; click(deletePosition);
		require(captureDocumentSnapshot(world, history)->yaml == before && history.undoCount() == 1
			&& selectedFurnitureInstance(world)->id == otherId, "Pinned plan deleted unrelated selection");
		// Reconstructed catalogue/instance selection cannot be used by a stale control.
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Cannot restore chair for stale-state check"); frame();
		require(selectFurnitureInstance(world, id), "Cannot select restored chair");
		catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/chair.furniture.yaml"));
		require(restore(*captureDocumentSnapshot(world, history)), "Cannot reconstruct catalogue");
		before = captureDocumentSnapshot(world, history)->yaml;
		auto count = history.undoCount(); click(deletePosition);
		require(!selectedFurnitureInstance(world) && captureDocumentSnapshot(world, history)->yaml == before
			&& history.undoCount() == count && plan.isOpen(world), "Stale catalogue/instance selection was deleted");
		require(selectFurnitureInstance(world, id), "Cannot select current chair");
		require(world->removeFurniture(id, &diagnostic), diagnostic); click(deletePosition);
		require(!selectedFurnitureInstance(world) && history.undoCount() == count && rows == 8,
			"External instance deletion left actionable selection or shrank range");
		auto replacement = std::make_shared<core::World>("Replacement", 20, 6);
		replacement->addRoom("Pinned", 0, 1, 3, 6, 2); replacement->finishBuild();
		world = replacement; frame();
		require(!plan.isOpen(world) && world->furniture().empty() && history.undoCount() == count,
			"World replacement retained deletion target");
		io.ClipboardUserData = nullptr; io.SetClipboardTextFn = nullptr;
	}

	void locationPlanWorkflow(smoke::Context const& context)
	{
		editor_smoke::State state; using smoke::require;
		auto world = std::make_shared<core::World>("Plan editor", 20, 8);
		auto room = world->addRoom("Pinned room", 1, 3, 7, 4, 3);
		auto corridor = world->addCorridor(0, 1, 2, 5, 1);
		auto facade = world->addFacade("Retargeted facade", 0, 0, 10, 4, 1);
		auto background = world->addBackground(1, 0, 0, 2, 1);
		world->addRoom("Lower landing", 0, 0, 15, 2, 1);
		world->addRoom("Upper landing", 0, 1, 15, 2, 1);
		auto transit = world->addLadder(1, 0, 15, {2, false, true}).ladder.sector->getIndex();
		world->finishBuild(); world->pauseSimulation();
		auto catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/desk.furniture.yaml"));
		world->attachFurnitureCatalogue("desk.furniture.yaml", catalogue);
		world->placeFurniture(room, "desk", 0.25f, 0, "Ground desk", 5);
		DocumentHistory history; std::string diagnostic;
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
			auto restored = world->deserialize(*reader, work); world->pauseSimulation(); return restored;
		};
		auto authoredBefore = captureDocumentSnapshot(world, DocumentHistory{})->yaml;
		LocationPlan plan;
		auto selection = world->getSector(room);
		uint32_t selectedLevel = 4;
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = {1000, 800};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		ImVec2 actionPosition; unsigned presentations = 0, graphEdges = 0, graphVertices = 0;
		std::vector<std::string> labels, depths;
		auto hasDepth = [&](std::string const& label) { return std::find(depths.begin(), depths.end(), label) != depths.end(); };
		auto hasLabel = [&](std::string const& label) { return std::find(labels.begin(), labels.end(), label) != labels.end(); };
		auto frame = [&]
		{
			text.clear(); presentations = graphEdges = graphVertices = 0; labels.clear(); depths.clear();
			ImGui::NewFrame();
			ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({250, 100});
			ImGui::Begin("Selection workflow", nullptr, ImGuiWindowFlags_NoSavedSettings);
			plan.renderSelectionAction(world, selection, selectedLevel);
			auto rect = ImGui::GetCurrentContext()->LastItemData.Rect;
			actionPosition = {(rect.Min.x + rect.Max.x) / 2, (rect.Min.y + rect.Max.y) / 2};
			ImGui::End();
			auto selectionText = text;
			text.clear(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({300, 10}); ImGui::SetNextWindowSize({600, 360});
			plan.render(world, [&](WorldDrawList const& commands, ImVec2 position, ImVec2)
			{
				++presentations;
				for (auto const& command : commands.commands())
				{
					if (auto label = std::get_if<WorldDrawList::Text>(&command))
					{
						labels.push_back(label->value);
						if (std::abs(label->position.x - position.x - 16) < .01f) depths.push_back(label->value);
					}
					if (auto edge = std::get_if<WorldDrawList::Line>(&command);
						edge && edge->colour == IM_COL32(80, 210, 220, 255)) ++graphEdges;
					if (auto vertex = std::get_if<WorldDrawList::Triangle>(&command);
						vertex && (vertex->colour == IM_COL32(150, 245, 255, 255)
							|| vertex->colour == IM_COL32(105, 230, 140, 255)
							|| vertex->colour == IM_COL32(255, 190, 60, 255))) ++graphVertices;
				}
				require(!commands.commands().empty(), "Open plan did not present a command stream");
			});
			// If the plan is closed it does not consume next-window settings.
			ImGui::GetCurrentContext()->NextWindowData.ClearFlags();
			ImGui::LogFinish(); text += selectionText; ImGui::Render();
		};
		auto click = [&](ImVec2 position)
		{
			io.AddMousePosEvent(position.x, position.y); frame(); frame();
			io.AddMouseButtonEvent(0, true); frame();
			io.AddMouseButtonEvent(0, false); frame();
		};
		for (auto index : {room, corridor, facade, background, transit})
		{
			selection = world->getSector(index); frame();
			bool eligible = index == room || index == corridor || index == facade;
			require((text.find("Location plan") != std::string::npos) == eligible,
				"Selection Location-plan eligibility is incorrect");
		}
		selection = world->getSector(room); frame(); click(actionPosition); frame();
		require(presentations == 1 && text.find("Location: Pinned room") != std::string::npos
			&& text.find("{ 4 } Level") != std::string::npos && text.find("Layer: 1") != std::string::npos,
			"Selection did not open the plan on selected World Level/Layer: " + text);
		require(!hasLabel("Ground desk") && hasDepth("3") && !hasDepth("4"),
			"Other supporting Level expanded or populated the plan");
		selection = world->getSector(facade); frame();
		require(text.find("Location: Pinned room") != std::string::npos,
			"Ordinary Selection retargeted a pinned plan");
		// Change the Level via real combo and selectable mouse input.
		auto* window = ImGui::FindWindowByName("Location plan");
		auto comboId = window->GetID("Level");
		auto findHovered = [&](ImGuiID id, ImVec2 min, ImVec2 max)
		{
			for (float y = min.y; y < max.y; y += 5)
				for (float x = min.x; x < max.x; x += 16)
				{
					io.AddMousePosEvent(x, y); frame(); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2{x, y};
				}
			throw std::runtime_error("Cannot find Location plan control " + std::to_string(id)
				+ " within " + std::to_string(min.x) + "," + std::to_string(min.y)
				+ " to " + std::to_string(max.x) + "," + std::to_string(max.y) + ": " + text);
		};
		click(findHovered(comboId, {310, 65}, {870, 125})); frame(); frame();
		auto* popup = ImGui::FindWindowByName("##Combo_00");
		require(popup && popup->Active, "Level selector did not open");
		click(findHovered(popup->GetID("5"), popup->Pos,
			{popup->Pos.x + popup->Size.x, popup->Pos.y + popup->Size.y}));
		frame();
		require(text.find("{ 5 } Level") != std::string::npos,
			"Multi-Level Room selector did not switch World Level: " + text);
		require(!hasLabel("Ground desk"), "Level selector showed ground-Level Furniture");
		require(captureDocumentSnapshot(world, DocumentHistory{})->yaml == authoredBefore,
			"Read-only plan input changed the World document");
		// External edits use the normal panel/history authority, not a plan-only API.
		for (uint32_t x = 0; x < 4; ++x) world->addSectorWalkway(room, 2, x);
		require(placeSelectedFurniture(world, room, "desk", 0.25f, 2, false, "Upper desk", diagnostic, history, 2), diagnostic);
		frame(); require(hasLabel("Upper desk") && hasDepth("4") && !hasDepth("5"),
			"Instance/vertex at depth 3 did not expose depth 4 after an external edit");
		require(graphEdges > 0 && graphVertices > 0 && text.find("Vertices: usable gold / external green / other cyan") != std::string::npos,
			"Production plan did not present live vertices/edges and colour legend");
		auto upper = world->furniture().back();
		require(editSelectedFurniture(world, upper.id, 0.5f, 2, false, "Upper desk", diagnostic, history, 5), diagnostic);
		frame(); require(hasDepth("7") && !hasDepth("8"), "Resolved depth 6 did not expose through 7");
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Plan edit undo failed");
		frame(); require(presentations == 1 && hasLabel("Upper desk") && hasDepth("7"),
			"Undo reconstruction lost contents or shrank the retained range");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Plan edit redo failed");
		frame(); require(hasLabel("Upper desk") && hasDepth("7"), "Redo reconstruction lost plan contents");
		require(editSelectedFurniture(world, upper.id, .75f, 2, false, "Upper desk", diagnostic, history, 1), diagnostic);
		frame(); require(hasDepth("7"), "Moving Furniture to a lower depth shrank the expanded plan");
		require(deleteSelectedFurniture(world, upper.id, diagnostic, history), diagnostic);
		frame(); require(!hasLabel("Upper desk") && hasDepth("7"), "Deletion failed to refresh or shrank plan");
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Plan deletion undo failed");
		frame(); require(hasLabel("Upper desk"), "Deletion undo did not restore displayed contents");
		// Retargeting starts a new compact range and excludes the Room's contents.
		selection = world->getSector(facade);
		frame(); click(actionPosition); frame();
		require(!hasLabel("Upper desk") && !hasDepth("7"), "Retarget retained another Location's contents/range");
		require(presentations == 1 && text.find("Location: Retargeted facade") != std::string::npos
			&& text.find("{ 0 } Level") != std::string::npos,
			"Explicit opening did not retarget the single window");
		window = ImGui::FindWindowByName("Location plan");
		click({window->Pos.x + window->Size.x - 10, window->Pos.y + 10});
		frame(); require(presentations == 0, "Window close still presents the plan");
		selection = world->getSector(room); selectedLevel = 3; frame(); click(actionPosition); frame();
		require(hasLabel("Ground desk") && hasDepth("7"), "Pre-existing resolved depths were hidden on opening");
		selection = world->getSector(corridor); selectedLevel = 1; frame(); click(actionPosition); frame();
		require(presentations == 1 && text.find("{ 1 } Level") != std::string::npos, "Reopening failed");
		while (!world->furniture().empty())
			require(deleteSelectedFurniture(world, world->furniture().back().id, diagnostic, history), diagnostic);
		frame(); require(!hasLabel("Ground desk") && !hasLabel("Upper desk"), "External removal did not refresh plan");
		catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/layouts.furniture.yaml"));
		world->attachFurnitureCatalogue("layouts.furniture.yaml", catalogue);
		require(placeSelectedFurniture(world, corridor, "sofa", .25f, 0, false, "New catalogue sofa", diagnostic, history, 6), diagnostic);
		frame(); require(hasLabel("New catalogue sofa") && hasDepth("7"),
			"Plan retained stale catalogue definitions or failed to expand");
		auto heldOldTarget = selection;
		// Same World object, new Sectors, as used by document history replay.
		auto snapshot = captureDocumentSnapshot(world, DocumentHistory{});
		auto reader = core::YamlSerializer::fromString(snapshot->yaml); reader->deserialize();
		core::SerializationWorkData work; work.furnitureCatalogue = catalogue;
		require(world->deserialize(*reader, work), "Cannot reconstruct plan World");
		frame(); require(presentations == 1, "History reconstruction failed to refresh unchanged target");
		require(!plan.open(world, heldOldTarget, 1), "Opening accepted a stale Location");
		selection = world->getSector(room); selectedLevel = 4; frame(); click(actionPosition); frame();
		require(presentations == 1, "Cannot reopen reconstructed World");
		auto replacement = std::make_shared<core::World>("Replacement", 20, 8);
		replacement->addRoom("Same index", 1, 3, 7, 4, 3); replacement->finishBuild();
		world = replacement; frame(); require(presentations == 0, "World replacement retained stale plan");
		selection = world->getSector(0); frame(); click(actionPosition); frame();
		require(presentations == 1, "Cannot open replacement World plan");
		world->pauseSimulation();
		auto removal = world->planRemoveLocation(0);
		require(removal.valid, "Cannot delete target Location");
		world->applyLocationEdit(removal);
		frame(); require(presentations == 0, "Target deletion retained stale plan");
		world.reset(); frame(); require(presentations == 0, "No-World plan did not close");
		io.ClipboardUserData = nullptr; io.SetClipboardTextFn = nullptr;
	}
}
void editor_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/bundledLuaWorkflow", bundledLuaWorkflow });
	checks.push_back({ "furniture/luaWorkflow", luaWorkflow });
	checks.push_back({ "furniture/demoActions", demoActions });
	checks.push_back({ "locationPlan/workflow", locationPlanWorkflow });
	checks.push_back({ "locationPlan/placement", locationPlanPlacement });
	checks.push_back({ "locationPlan/movement", locationPlanMovement });
	checks.push_back({ "locationPlan/hover", locationPlanHover });
	checks.push_back({ "locationPlan/deletion", locationPlanDeletion });
	checks.push_back({ "furniture/chairActions", chairActions });
	checks.push_back({ "furniture/catalogueReattachmentHistory", catalogueReattachmentHistory });
	checks.push_back({ "furniture/attachmentActions", attachmentActions });
	checks.push_back({ "furniture/cataloguePicker", cataloguePicker });
	checks.push_back({ "furniture/compositionActions", compositionActions });
}
