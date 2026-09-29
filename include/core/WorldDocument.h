#pragma once

#include <filesystem>
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
