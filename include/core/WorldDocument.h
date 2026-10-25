#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace core
{
	inline constexpr std::string_view BinaryWorldDocumentFilenameSuffix{ ".world" };

	// The newest World/shared-station schema this build's readers know. The
	// The writer stamps a tier at or below this depending on which opt-in feature
	// blocks a document actually carries (111 plain, 112 coolant-leak Faults,
	// 113 Condition, 114 latching Trips, 115 repair-parts wiring, 116 Wear
	// accumulators, 117 thermal-plant Fault catalogues, 118 power-source
	// Fault catalogues), so a
	// document's own version is no longer the reader boundary. Anything above
	// this constant is future and must be refused; tests that probe the future
	// boundary derive from here rather than from a written document's tier.
	// 119 adds opt-in contamination-chain Faults; 120 adds atmosphere/airlock Faults;
	// 121 adds the machine Wear catalogue (#848).
	inline constexpr std::uint32_t kWorldSchemaVersion = 122;
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

	// Returns the declared source for a catalogue Resource, including an explicit
	// imported Agent-type reference, or an empty path when it is unknown.
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
