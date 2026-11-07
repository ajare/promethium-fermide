#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include "core/Platform.h"

namespace core
{
	inline constexpr std::string_view BinaryWorldDocumentFilenameSuffix{ ".world" };
	inline constexpr std::string_view YamlWorldDocumentFilenameSuffix{ ".world.yaml" };
	inline constexpr auto WorldDocumentFilenameSuffix = BinaryWorldDocumentFilenameSuffix;

	enum class WorldDocumentFormat
	{
		Binary,
		Yaml
	};

	// Maps an external catalogue Resource name (ADR 0010) to the manifest source
	// it was declared with. World documents store Resource names, never file
	// paths; imported Agent-type names encode their canonical source so they
	// remain resolvable without changing the manifest. `core` resolves them
	// through this seam so it stays independent of
	// the application resource system. When no resolver is installed,
	// loadWorldDocument falls back to a file beside the World, which keeps
	// GPU-less tools and hand-authored fixtures working.
	class PF_CORE_API CatalogResourceResolver
	{
	public:
		virtual ~CatalogResourceResolver() = default;

		// `type` is "FurnitureCatalogue", "AgentTagRegistry",
		// "AgentBehaviourRegistry", or "AgentType". Returns an empty path when
		// the Resource is unknown.
		virtual std::filesystem::path catalogSource(
			std::string const& type, std::string const& resourceName) const = 0;
	};

	// Installs the process-wide resolver. Passing nullptr restores the
	// adjacent-file fallback.
	PF_CORE_API void setCatalogResourceResolver(
		std::shared_ptr<CatalogResourceResolver> resolver);
	PF_CORE_API std::shared_ptr<CatalogResourceResolver> catalogResourceResolver();

	// Returns the declared source for a catalogue Resource, including an explicit
	// imported Agent-type reference, or an empty path when it is unknown.
	PF_CORE_API std::filesystem::path resolveCatalogSource(std::string const& type,
		std::string const& resourceName);

	PF_CORE_API bool isWorldDocumentPath(std::filesystem::path const& filepath);
	PF_CORE_API void requireWorldDocumentPath(std::filesystem::path const& filepath);
	PF_CORE_API WorldDocumentFormat worldDocumentFormat(std::filesystem::path const& filepath);

	// Applies the default binary suffix to a suffixless Save As result and
	// refuses every path that does not have one of the exact supported suffixes.
	PF_CORE_API std::filesystem::path worldDocumentSavePath(
		std::filesystem::path const& filepath);

	// Returns the path with the complete supported suffix removed. For example,
	// both /project/station.world and /project/station.world.yaml become
	// /project/station.
	PF_CORE_API std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath);
}
