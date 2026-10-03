#include "Checks.h"
#include "State.h"
#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include "core/YamlSerializer.h"

namespace
{
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
			return world->deserialize(*reader, work);
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->furniture().size() == 2, "Chair placement undo failed");
		require(history.redo(captureDocumentSnapshot(world, history), restore) && world->furniture().size() == 3
			&& world->furniture().front().marker == marker, "Chair placement redo changed Marker identity");
		ImGui::GetIO().DisplaySize = {800, 600}; ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Furniture actions");
		renderFurniturePanel(world, path, world->getSector(room));
		ImGui::End(); ImGui::EndFrame();
	}
}
void editor_smoke::registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/chairActions", chairActions });
}
