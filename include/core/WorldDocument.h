#pragma once

#include <filesystem>
#include <string_view>

namespace core
{
	inline constexpr std::string_view BinaryWorldDocumentFilenameSuffix{ ".world" };
	inline constexpr std::string_view YamlWorldDocumentFilenameSuffix{ ".world.yaml" };
	// Retained until the editor's default-save workflow chooses between formats.
	inline constexpr auto WorldDocumentFilenameSuffix = YamlWorldDocumentFilenameSuffix;

	enum class WorldDocumentFormat
	{
		Binary,
		Yaml
	};

	bool isWorldDocumentPath(std::filesystem::path const& filepath);
	void requireWorldDocumentPath(std::filesystem::path const& filepath);
	WorldDocumentFormat worldDocumentFormat(std::filesystem::path const& filepath);

	// Returns the path with the complete supported suffix removed. For example,
	// both /project/station.world and /project/station.world.yaml become
	// /project/station.
	std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath);
}
