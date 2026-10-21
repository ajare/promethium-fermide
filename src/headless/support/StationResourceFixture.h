#pragma once
#include "StationConfigurationResource.h"
#include <filesystem>
#include <fstream>
#include <yaml-cpp/yaml.h>
#include <willpower/common/Logger.h>
#include <willpower/application/resourcesystem/DirectoryResourceLocation.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace headless
{
// Exercise the real manifest entries and Directory scanner without creating
// unrelated GPU resources. Preserve source-relative paths in an isolated tree.
struct StationResourceFixture
{
	wp::Logger logger;
	wp::application::resourcesystem::ResourceManager manager{nullptr, nullptr, nullptr, &logger};
	std::vector<std::shared_ptr<StationConfigurationResource>> configurations;
	StationResourceFixture(std::filesystem::path const& resources, std::filesystem::path const& root)
	{
		namespace rs = wp::application::resourcesystem;
		std::filesystem::create_directories(root);
		auto manifest = YAML::LoadFile((resources / "Resources.yaml").string());
		YAML::Node selected;
		for (auto const& entry : manifest["Resources"]["Resource"])
		{
			if (entry["type"].as<std::string>() != "StationConfiguration") continue;
			selected["Resources"]["Resource"].push_back(entry);
			auto source = std::filesystem::path(entry["location"].as<std::string>());
			std::filesystem::create_directories((root / source).parent_path());
			std::filesystem::copy_file(resources / source, root / source,
				std::filesystem::copy_options::overwrite_existing);
		}
		{ std::ofstream out(root / "Resources.yaml"); out << selected; }
		registerStationConfigurationResourceType(manager);
		manager.addResourceLocationFactory("Directory", [this](auto const& path, auto const& definition) -> rs::ResourceLocation* {
			return new rs::DirectoryResourceLocation(&logger, path, definition);
		});
		manager.addResourceLocation("Directory", root.string(), "Resources.yaml");
		manager.scanLocations();
		configurations = loadStationConfigurationResources(manager);
	}
};
}
