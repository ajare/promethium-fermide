#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <utility>

#include "core/Platform.h"
#include "core/WorldDocument.h"

namespace core
{
	// Maps a catalogue Resource name (ADR 0010) to the manifest source declared in
	// Resources.yaml, so core's World loader can resolve Resource references
	// without depending on the application resource system. The World loader never
	// parses the manifest itself; this resolver is the single implementation
	// shared by the editor and the headless tools.
	//
	// It reads the manifest `location` directly rather than through the resource
	// manager because composite Resources (those that also declare dependent
	// resources, such as a Furniture catalogue's Artwork dependency) report an
	// empty source to the resource manager, which would resolve to the manifest
	// directory instead of the declared file.
	//
	// The parse lives in core/src/ManifestCatalogResolver.cpp so that yaml-cpp is
	// a dependency of the library alone: including this header no longer drags
	// <yaml-cpp/yaml.h> into every translation unit that resolves catalogue
	// Resources.
	class PF_CORE_API ManifestCatalogResolver final : public CatalogResourceResolver
	{
	public:
		// Reads `manifestPath` immediately. A missing or unreadable manifest
		// throws a yaml-cpp error; a manifest that declares no
		// `Resources.Resource` sequence yields a resolver that knows nothing, so
		// every lookup falls through to the loader's own fallback (see
		// `resolveCatalogSource`).
		explicit ManifestCatalogResolver(std::filesystem::path const& manifestPath);

		// Returns the manifest-declared source for the (type, name) pair, or an
		// empty path when the Resource is unknown.
		std::filesystem::path catalogSource(std::string const& type,
			std::string const& resourceName) const override;

	private:
		std::map<std::pair<std::string, std::string>, std::filesystem::path> mSources;
	};
}
