#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "core/World.h"
#include "DocumentEdit.h"

// Shared transient instance identity; stale World, Sector or catalogue clears it.
bool selectFurnitureInstance(std::shared_ptr<core::World> const& world, uint64_t id);
core::FurnitureInstance const* selectedFurnitureInstance(std::shared_ptr<core::World> const& world);

// Selection takes the application Resource name declared in Resources.yaml
// (ADR 0010), never a file path. When no resource resolver is installed the
// catalogue is loaded from a file of that name beside the World, which keeps
// headless fixtures and hand-authored documents working.
bool selectFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string const& resourceName,
	std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
bool reloadSelectedFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string& diagnostic);
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
// Platform resources stay in the GUI; headless tests drive the same panel seam
// by supplying the available Resource names and calling selectFurnitureCatalogue.
using FurnitureCatalogueResourceProvider = std::function<std::vector<std::string>()>;
void renderFurniturePanel(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath,
	FurnitureCatalogueResourceProvider const& resources = {},
	DocumentHistory& history = gWorldDocumentHistory);
