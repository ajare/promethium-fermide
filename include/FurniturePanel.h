#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "core/World.h"
#include "DocumentEdit.h"

bool selectFurnitureCatalogue(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::string const& filename,
	std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
bool placeSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint32_t sector, std::string const& key, float x, float y, bool snapX,
	std::string const& name, std::string& diagnostic,
	DocumentHistory& history = gWorldDocumentHistory);
bool editSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, float x, float y, bool snapX, std::string const& name,
	std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
bool deleteSelectedFurniture(std::shared_ptr<core::World> const& world,
	uint64_t id, std::string& diagnostic, DocumentHistory& history = gWorldDocumentHistory);
void renderFurniturePanel(std::shared_ptr<core::World> const& world,
	std::filesystem::path const& worldPath, std::shared_ptr<const core::Sector> const& selected);
