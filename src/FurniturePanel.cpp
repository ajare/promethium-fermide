#include "FurniturePanel.h"
#include "imgui/imgui.h"
#include "core/WorldDocument.h"
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
	std::filesystem::path const& worldPath, std::string const& resourceName,
	std::string& diagnostic, DocumentHistory& history)
{
	try
	{
		if (!world || worldPath.empty() || !std::filesystem::is_regular_file(worldPath))
			throw std::runtime_error("Save the World before loading a Furniture catalogue");
		std::filesystem::path name(resourceName);
		if (resourceName.empty() || name.is_absolute() || name.has_parent_path()
			|| name.filename().string() != resourceName)
			throw std::runtime_error("Select a Furniture catalogue Resource");
		auto source = core::resolveCatalogSource("FurnitureCatalogue", resourceName);
		if (source.empty()) source = worldPath.parent_path() / name;
		auto catalogue = core::FurnitureCatalogue::load(source);
		auto before = captureDocumentSnapshot(world, history);
		world->attachFurnitureCatalogue(resourceName, std::move(catalogue));
		commitDocumentEdit(std::move(before), history);
		diagnostic.clear(); return true;
	}
	catch (std::exception const& error) { diagnostic = error.what(); return false; }
}

bool reloadSelectedFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string& diagnostic)
{
	if (!world || worldPath.empty()) { diagnostic = "Save the World before reloading its Furniture catalogue"; return false; }
	auto const& resourceName = world->furnitureCatalogueResourceName();
	auto source = core::resolveCatalogSource("FurnitureCatalogue", resourceName);
	if (source.empty()) source = worldPath.parent_path() / resourceName;
	return world->reloadFurnitureCatalogue(source, &diagnostic);
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
	std::filesystem::path const& worldPath,
	FurnitureCatalogueResourceProvider const& resources,
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
	auto const& resourceName = world->furnitureCatalogueResourceName();
	ImGui::Text("Catalogue: %s", resourceName.empty() ? "None" : resourceName.c_str());
	std::error_code error;
	bool saved = !worldPath.empty() && std::filesystem::is_regular_file(worldPath, error);
	auto const available = resources ? resources() : std::vector<std::string>{};
	ImGui::BeginDisabled(!saved || available.empty());
	if (ImGui::BeginCombo("##furnitureCatalogueResource",
		resourceName.empty() ? "Select Furniture catalogue..." : resourceName.c_str()))
	{
		for (auto const& candidate : available)
			if (ImGui::Selectable(candidate.c_str(), candidate == resourceName))
				selectFurnitureCatalogue(world, worldPath, candidate, diagnostic, history);
		ImGui::EndCombo();
	}
	ImGui::EndDisabled();
	if (!saved) ImGui::TextUnformatted("Save the World before selecting a Furniture catalogue.");
	ImGui::BeginDisabled(!saved || resourceName.empty() || !world->isSimulationPaused());
	if (ImGui::Button("Reload Furniture catalogue"))
		reloadSelectedFurnitureCatalogue(world, worldPath, diagnostic);
	ImGui::EndDisabled();
	if (!diagnostic.empty()) ImGui::TextWrapped("%s", diagnostic.c_str());
}
