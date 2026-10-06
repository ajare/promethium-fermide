#pragma once

#include <filesystem>
#include <memory>

#include "core/WorldDocument.h"
#include "ManifestCatalogResolver.h"

namespace headless_support
{
	// GPU-less standalone tools have no render ResourceManager. This alias keeps
	// the headless include surface stable while sharing the single manifest
	// resolver with the editor, so Worlds name their catalog Resources
	// (ADR 0010) exactly as the editor does.
	using ManifestCatalogResolver = ::ManifestCatalogResolver;

	// Installs the resolver when the manifest exists. Returns true when installed.
	inline bool installManifestCatalogResolver(std::filesystem::path const& manifestPath)
	{
		if (!std::filesystem::is_regular_file(manifestPath)) return false;
		core::setCatalogResourceResolver(
			std::make_shared<::ManifestCatalogResolver>(manifestPath));
		return true;
	}
}
