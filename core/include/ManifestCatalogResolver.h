#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "core/WorldDocument.h"

// Maps a catalogue Resource name (ADR 0010) to the manifest source declared in
// Resources.yaml, so core's World loader can resolve Resource references
// without depending on the application resource system. `core` never parses
// the manifest; this resolver is the single implementation shared by the
// editor and headless tools.
//
// It reads the manifest `location` directly rather than through the resource
// manager because composite Resources (those that also declare dependent
// resources, such as a Furniture catalogue's Artwork dependency) report an
// empty source to the resource manager, which would resolve to the manifest
// directory instead of the declared file.
class ManifestCatalogResolver final : public core::CatalogResourceResolver
{
public:
	explicit ManifestCatalogResolver(std::filesystem::path const& manifestPath)
	{
		auto const base = manifestPath.parent_path();
		auto root = YAML::LoadFile(manifestPath.string());
		auto resources = root["Resources"]["Resource"];
		if (!resources || !resources.IsSequence()) return;
		for (auto const& resource : resources)
		{
			if (!resource["type"] || !resource["name"] || !resource["location"])
				continue;
			auto source = std::filesystem::path(resource["location"].as<std::string>());
			if (!source.is_absolute()) source = base / source;
			mSources.emplace(
				std::make_pair(resource["type"].as<std::string>(),
					resource["name"].as<std::string>()),
				std::move(source));
		}
	}

	std::filesystem::path catalogSource(std::string const& type,
		std::string const& resourceName) const override
	{
		auto found = mSources.find({ type, resourceName });
		if (found == mSources.end()) return {};
		auto source = found->second;
		// The manifest names a behaviour package by its inner manifest file;
		// the file-based loader expects the package directory.
		if (type == "AgentBehaviourRegistry"
			&& std::filesystem::is_regular_file(source))
			source = source.parent_path();
		return source;
	}

private:
	std::map<std::pair<std::string, std::string>, std::filesystem::path> mSources;
};
