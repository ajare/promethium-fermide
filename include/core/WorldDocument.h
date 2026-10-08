#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

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
	// paths; `core` resolves them through this seam so it stays independent of
	// the application resource system. When no resolver is installed,
	// loadWorldDocument falls back to a file beside the World, which keeps
	// GPU-less tools and hand-authored fixtures working.
	class CatalogResourceResolver
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
	void setCatalogResourceResolver(
		std::shared_ptr<CatalogResourceResolver> resolver);
	std::shared_ptr<CatalogResourceResolver> catalogResourceResolver();

	// Returns the manifest source for a catalogue Resource, or an empty path
	// when no resolver is installed or the Resource is unknown.
	std::filesystem::path resolveCatalogSource(std::string const& type,
		std::string const& resourceName);

	bool isWorldDocumentPath(std::filesystem::path const& filepath);
	void requireWorldDocumentPath(std::filesystem::path const& filepath);
	WorldDocumentFormat worldDocumentFormat(std::filesystem::path const& filepath);

	// Applies the default binary suffix to a suffixless Save As result and
	// refuses every path that does not have one of the exact supported suffixes.
	std::filesystem::path worldDocumentSavePath(
		std::filesystem::path const& filepath);

	// Returns the path with the complete supported suffix removed. For example,
	// both /project/station.world and /project/station.world.yaml become
	// /project/station.
	std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath);
}
