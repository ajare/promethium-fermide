#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <yaml-cpp/yaml.h>

#include "core/WorldDocument.h"

namespace headless_support
{
	// GPU-less standalone tools have no render ResourceManager. This resolver
	// reads the application manifest so Worlds can name their catalog Resources
	// (ADR 0010) exactly as the editor does. It is a headless-only converter from
	// Resource name to manifest source; core never parses the manifest.
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
				mSources.emplace(
					std::make_pair(resource["type"].as<std::string>(),
						resource["name"].as<std::string>()),
					base / resource["location"].as<std::string>());
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

	// Installs the resolver when the manifest exists. Returns true when installed.
	inline bool installManifestCatalogResolver(std::filesystem::path const& manifestPath)
	{
		if (!std::filesystem::is_regular_file(manifestPath)) return false;
		core::setCatalogResourceResolver(
			std::make_shared<ManifestCatalogResolver>(manifestPath));
		return true;
	}
}
