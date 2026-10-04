#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

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
	std::string const& name, std::string& diagnostic, DocumentHistory& history, int localDepth)
{
	if (snapX && std::isfinite(x)) x = std::round(x);
	if (!world || !world->canPlaceFurniture(sector, key, x, y, name, &diagnostic, localDepth)) return false;
	try
	{
		auto before = captureDocumentSnapshot(world, history);
		world->placeFurniture(sector, key, x, y, name, localDepth);
		world->finishBuild();
		commitDocumentEdit(std::move(before), history);
		diagnostic.clear(); return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

bool editSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, float x, float y, bool snapX, std::string const& name,
	std::string& diagnostic, DocumentHistory& history, std::optional<int> localDepth)
{
	if (snapX && std::isfinite(x)) x = std::round(x);
	if (!world || !world->canEditFurniture(id, x, y, name, &diagnostic, localDepth)) return false;
	try
	{
		auto before = captureDocumentSnapshot(world, history);
		if (!world->editFurniture(id, x, y, name, &diagnostic, localDepth)) return false;
		commitDocumentEdit(std::move(before), history);
		return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

bool deleteSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, std::string& diagnostic, DocumentHistory& history)
{
	if (!world || !world->canRemoveFurniture(id, &diagnostic)) return false;
	try
	{
		auto before = captureDocumentSnapshot(world, history);
		if (!world->removeFurniture(id, &diagnostic)) return false;
		commitDocumentEdit(std::move(before), history);
		return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

void renderFurniturePanel(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::shared_ptr<const core::Sector> const& selected)
{
	if (!world || !ImGui::CollapsingHeader("Furniture")) return;
	static char filename[256] = "chair.furniture.yaml", name[256] = "Chair";
	static float x = 0, y = 0;
	static int localDepth = 0;
	static bool snap = true;
	static std::string key, diagnostic;
	static std::weak_ptr<core::World> filenameWorld;
	static std::string loadedFilename;
	if (filenameWorld.lock() != world || loadedFilename != world->furnitureCatalogueFilename())
	{
		filenameWorld = world;
		loadedFilename = world->furnitureCatalogueFilename();
		std::snprintf(filename, sizeof(filename), "%s",
			loadedFilename.empty() ? "chair.furniture.yaml" : loadedFilename.c_str());
	}
	ImGui::InputText("Catalogue beside World", filename, sizeof(filename));
	if (ImGui::Button("Load Furniture catalogue"))
		selectFurnitureCatalogue(world, worldPath, filename, diagnostic);
	if (auto catalogue = world->furnitureCatalogue())
	{
		auto definition = catalogue->definition(key);
		if (!definition && !catalogue->definitions().empty())
		{
			key = catalogue->definitions().begin()->first;
			definition = catalogue->definition(key);
		}
		if (ImGui::BeginCombo("Furniture definition", definition ? definition->label.c_str() : "Select"))
		{
			for (auto const& [entryKey, entry] : catalogue->definitions())
				if (ImGui::Selectable(entry.label.c_str(), entryKey == key)) key = entryKey;
			ImGui::EndCombo();
		}
		static uint64_t instanceId = 0;
		static core::World const* selectionWorld = nullptr;
		if (selectionWorld != world.get()) { selectionWorld = world.get(); instanceId = 0; }
		auto instance = std::find_if(world->furniture().begin(), world->furniture().end(),
			[&](auto const& entry) { return entry.id == instanceId; });
		if (instance == world->furniture().end()) instanceId = 0;
		if (ImGui::BeginCombo("Furniture instance", instanceId ? instance->name.c_str() : "New instance"))
		{
			if (ImGui::Selectable("New instance", instanceId == 0)) instanceId = 0;
			for (auto const& entry : world->furniture())
			{
				ImGui::PushID(static_cast<int>(entry.id));
				if (ImGui::Selectable(entry.name.c_str(), entry.id == instanceId))
				{
					instanceId = entry.id; x = entry.x; y = entry.y; localDepth = entry.localDepth;
					std::snprintf(name, sizeof(name), "%s", entry.name.c_str());
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::InputText("Instance name", name, sizeof(name));
		ImGui::InputFloat("Furniture x (Location-local)", &x);
		ImGui::InputFloat("Supporting Level (Location-local)", &y);
		ImGui::InputInt("Furniture Local depth", &localDepth);
		ImGui::Checkbox("Snap Furniture x", &snap);
		ImGui::TextUnformatted("Select a Room, Corridor or Facade on the canvas.");
		if (instanceId)
		{
			if (ImGui::Button("Apply Furniture edit"))
				editSelectedFurniture(world, instanceId, x, y, snap, name, diagnostic, gWorldDocumentHistory, localDepth);
			ImGui::SameLine();
			if (ImGui::Button("Delete Furniture"))
				if (deleteSelectedFurniture(world, instanceId, diagnostic)) instanceId = 0;
			ImGui::TextUnformatted("Owned Markers may be renamed in Marker Selection; layout belongs to Furniture.");
		}
		else if (ImGui::Button("Place Furniture"))
		{
			if (!selected) diagnostic = "Select a Location before placing Furniture";
			else placeSelectedFurniture(world, selected->getIndex(), key, x, y, snap, name, diagnostic, gWorldDocumentHistory, localDepth);
		}
	}
	if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
}
