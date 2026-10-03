#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include <cmath>

bool selectFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string const& filename,
	std::string& diagnostic, DocumentHistory& history)
{
	try
	{
		if (!world || worldPath.empty() || !std::filesystem::is_regular_file(worldPath))
			throw std::runtime_error("Save the World before loading a Furniture catalogue");
		std::filesystem::path path(filename);
		if (path.has_parent_path() || filename.empty() || !filename.ends_with(".furniture.yaml"))
			throw std::runtime_error("Select a .furniture.yaml catalogue beside the World");
		auto catalogue = core::FurnitureCatalogue::load(worldPath.parent_path() / path);
		auto before = captureDocumentSnapshot(world, history);
		world->attachFurnitureCatalogue(filename, std::move(catalogue));
		commitDocumentEdit(std::move(before), history);
		diagnostic.clear(); return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

bool placeSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint32_t sector, std::string const& key, float x, float y, bool snapX,
	std::string const& name, std::string& diagnostic, DocumentHistory& history)
{
	if (snapX && std::isfinite(x)) x = std::round(x);
	if (!world || !world->canPlaceFurniture(sector, key, x, y, name, &diagnostic)) return false;
	try
	{
		auto before = captureDocumentSnapshot(world, history);
		world->placeFurniture(sector, key, x, y, name);
		world->finishBuild();
		commitDocumentEdit(std::move(before), history);
		diagnostic.clear(); return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

void renderFurniturePanel(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::shared_ptr<const core::Sector> const& selected)
{
	if (!world || !ImGui::CollapsingHeader("Furniture")) return;
	static char filename[256] = "chair.furniture.yaml", name[256] = "Chair";
	static float x = 0, y = 0;
	static bool snap = true;
	static std::string key, diagnostic;
	ImGui::InputText("Catalogue beside World", filename, sizeof(filename));
	if (ImGui::Button("Load Furniture catalogue"))
		selectFurnitureCatalogue(world, worldPath, filename, diagnostic);
	if (auto catalogue = world->furnitureCatalogue())
	{
		auto definition = catalogue->definition(key);
		if (ImGui::BeginCombo("Furniture definition", definition ? definition->label.c_str() : "Select"))
		{
			for (auto const& [entryKey, entry] : catalogue->definitions())
				if (ImGui::Selectable(entry.label.c_str(), entryKey == key)) key = entryKey;
			ImGui::EndCombo();
		}
		ImGui::InputText("Instance name", name, sizeof(name));
		ImGui::InputFloat("Furniture x (Location-local)", &x);
		ImGui::InputFloat("Supporting Level (Location-local)", &y);
		ImGui::Checkbox("Snap Furniture x", &snap);
		ImGui::TextUnformatted("Select a Room, Corridor or Facade on the canvas.");
		if (ImGui::Button("Place Furniture"))
		{
			if (!selected) diagnostic = "Select a Location before placing Furniture";
			else placeSelectedFurniture(world, selected->getIndex(), key, x, y, snap, name, diagnostic);
		}
	}
	if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
}
