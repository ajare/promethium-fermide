#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include <cmath>


namespace
{
	std::weak_ptr<core::World> furnitureSelectionWorld;
	std::weak_ptr<const core::Sector> furnitureSelectionSector;
	std::weak_ptr<const core::FurnitureCatalogue> furnitureSelectionCatalogue;
	uint64_t furnitureSelectionId{};
}

core::FurnitureInstance const* selectedFurnitureInstance(std::shared_ptr<core::World> const& world)
{
	if (world && furnitureSelectionWorld.lock() == world
		&& furnitureSelectionCatalogue.lock() == world->furnitureCatalogue())
		for (auto const& entry : world->furniture())
			if (entry.id == furnitureSelectionId && entry.sector < world->getNumSectors()
				&& furnitureSelectionSector.lock() == world->getSector(entry.sector)) return &entry;
	furnitureSelectionId = 0;
	return nullptr;
}

bool selectFurnitureInstance(std::shared_ptr<core::World> const& world, uint64_t id)
{
	furnitureSelectionId = 0;
	if (world) for (auto const& entry : world->furniture())
		if (entry.id == id)
		{
			furnitureSelectionWorld = world; furnitureSelectionId = id;
			furnitureSelectionSector = world->getSector(entry.sector);
			furnitureSelectionCatalogue = world->furnitureCatalogue();
			return true;
		}
	return false;
}

bool selectFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string const& filename,
	std::string& diagnostic, DocumentHistory& history)
{
	try
	{
		if (!world || worldPath.empty() || !std::filesystem::is_regular_file(worldPath))
			throw std::runtime_error("Save the World before loading a Furniture catalogue");
		std::filesystem::path path(filename);
		if (path.has_parent_path() || filename.empty() || !core::FurnitureCatalogue::filenameIsValid(filename))
			throw std::runtime_error("Select a .furniture.lua catalogue beside the World; YAML Furniture requires conversion to Lua");
		auto catalogue = core::FurnitureCatalogue::load(worldPath.parent_path() / path);
		auto before = captureDocumentSnapshot(world, history);
		world->attachFurnitureCatalogue(filename, std::move(catalogue));
		commitDocumentEdit(std::move(before), history);
		diagnostic.clear(); return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

bool reloadSelectedFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string& diagnostic)
{
	if (!world || worldPath.empty()) { diagnostic = "Save the World before reloading its Furniture catalogue"; return false; }
	return world->reloadFurnitureCatalogue(worldPath.parent_path() / world->furnitureCatalogueFilename(), &diagnostic);
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
		auto selected = selectedFurnitureInstance(world);
		bool retainSelection = selected && selected->id == id;
		if (!world->editFurniture(id, x, y, name, &diagnostic, localDepth)) return false;
		if (retainSelection) selectFurnitureInstance(world, id);
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
	std::filesystem::path const& worldPath, FurnitureCataloguePathChooser const& choosePath,
	DocumentHistory& history)
{
	if (!world || !ImGui::CollapsingHeader("Furniture")) return;
	static std::weak_ptr<core::World> displayedWorld;
	static std::filesystem::path displayedPath;
	static std::string diagnostic;
	if (displayedWorld.lock() != world || displayedPath != worldPath)
	{
		displayedWorld = world; displayedPath = worldPath; diagnostic.clear();
	}
	auto const& filename = world->furnitureCatalogueFilename();
	ImGui::Text("Catalogue: %s", filename.empty() ? "None" : filename.c_str());
	std::error_code error;
	bool saved = !worldPath.empty() && std::filesystem::is_regular_file(worldPath, error);
	ImGui::BeginDisabled(!saved || !choosePath);
	if (ImGui::Button("Select Furniture catalogue..."))
		try
		{
			if (auto path = choosePath())
			{
				auto chosen = std::filesystem::absolute(std::filesystem::path(*path));
				auto directory = std::filesystem::absolute(worldPath).parent_path();
				if (!std::filesystem::equivalent(chosen.parent_path(), directory))
					throw std::runtime_error("Select a .furniture.lua catalogue beside the World");
				selectFurnitureCatalogue(world, worldPath, chosen.filename().string(), diagnostic, history);
			}
		}
		catch (std::exception const& failure) { diagnostic = failure.what(); }
	ImGui::EndDisabled();
	ImGui::BeginDisabled(!saved || filename.empty() || !world->isSimulationPaused());
	if (ImGui::Button("Reload Furniture catalogue"))
		reloadSelectedFurnitureCatalogue(world, worldPath, diagnostic);
	ImGui::EndDisabled();
	if (!saved) ImGui::TextUnformatted("Save the World before selecting a Furniture catalogue.");
	if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
}
