#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include "core/World.h"
#include "DocumentEdit.h"

// Shared transient instance identity; stale World, Sector or catalogue clears it.
bool selectFurnitureInstance(std::shared_ptr<core::World> const& world, uint64_t id);
core::FurnitureInstance const* selectedFurnitureInstance(std::shared_ptr<core::World> const& world);

bool selectFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string const& filename,
	std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
bool placeSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint32_t sector, std::string const& key, float x, float y, bool snapX,
	std::string const& name, std::string& diagnostic,
	DocumentHistory& history = gWorldDocumentHistory, int localDepth = 0);
bool editSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, float x, float y, bool snapX, std::string const& name,
	std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory,
	std::optional<int> localDepth = std::nullopt);
bool deleteSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
// Platform dialogs stay in the GUI; headless tests drive the same panel seam.
using FurnitureCataloguePathChooser = std::function<std::optional<std::string>()>;
void renderFurniturePanel(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, FurnitureCataloguePathChooser const& choosePath = {},
	DocumentHistory& history = gWorldDocumentHistory);
