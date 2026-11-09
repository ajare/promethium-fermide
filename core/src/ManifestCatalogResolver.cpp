#include "core/ManifestCatalogResolver.h"

#include <yaml-cpp/yaml.h>

namespace core
{
	ManifestCatalogResolver::ManifestCatalogResolver(
		std::filesystem::path const& manifestPath)
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

	std::filesystem::path ManifestCatalogResolver::catalogSource(
		std::string const& type, std::string const& resourceName) const
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
}
