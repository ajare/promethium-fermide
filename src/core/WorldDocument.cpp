#include "core/WorldDocument.h"

#include "core/SerializationException.h"

namespace core
{
	namespace
	{
		std::shared_ptr<CatalogResourceResolver> gCatalogResourceResolver;
	}

	void setCatalogResourceResolver(
		std::shared_ptr<CatalogResourceResolver> resolver)
	{
		gCatalogResourceResolver = std::move(resolver);
	}

	std::shared_ptr<CatalogResourceResolver> catalogResourceResolver()
	{
		return gCatalogResourceResolver;
	}

	std::filesystem::path resolveCatalogSource(std::string const& type,
		std::string const& resourceName)
	{
		if (!gCatalogResourceResolver || resourceName.empty()) return {};
		return gCatalogResourceResolver->catalogSource(type, resourceName);
	}

	namespace
	{
		bool hasCompleteSuffix(std::string const& filename, std::string_view suffix)
		{
			return filename.size() > suffix.size() && filename.ends_with(suffix);
		}
	}

	bool isWorldDocumentPath(std::filesystem::path const& filepath)
	{
		auto const filename = filepath.filename().string();
		return hasCompleteSuffix(filename, BinaryWorldDocumentFilenameSuffix)
			|| hasCompleteSuffix(filename, YamlWorldDocumentFilenameSuffix);
	}

	void requireWorldDocumentPath(std::filesystem::path const& filepath)
	{
		if (!isWorldDocumentPath(filepath))
		{
			throw SerializationException(
				"A World document file must end with .world or .world.yaml");
		}
	}

	WorldDocumentFormat worldDocumentFormat(std::filesystem::path const& filepath)
	{
		auto const filename = filepath.filename().string();
		if (hasCompleteSuffix(filename, YamlWorldDocumentFilenameSuffix))
			return WorldDocumentFormat::Yaml;
		if (hasCompleteSuffix(filename, BinaryWorldDocumentFilenameSuffix))
			return WorldDocumentFormat::Binary;
		requireWorldDocumentPath(filepath);
		throw SerializationException("Unsupported World document format");
	}

	std::filesystem::path worldDocumentSavePath(
		std::filesystem::path const& filepath)
	{
		auto const filename = filepath.filename().string();
		if (filename.empty() || filename == BinaryWorldDocumentFilenameSuffix
			|| filename == YamlWorldDocumentFilenameSuffix)
		{
			requireWorldDocumentPath(filepath);
		}
		if (isWorldDocumentPath(filepath)) return filepath;
		if (!filepath.has_extension())
			return filepath.string() + std::string(BinaryWorldDocumentFilenameSuffix);
		requireWorldDocumentPath(filepath);
		throw SerializationException("Unsupported World document save path");
	}

	std::filesystem::path worldDocumentBasePath(
		std::filesystem::path const& filepath)
	{
		auto const format = worldDocumentFormat(filepath);
		auto const suffix = format == WorldDocumentFormat::Binary
			? BinaryWorldDocumentFilenameSuffix : YamlWorldDocumentFilenameSuffix;
		auto filename = filepath.filename().string();
		filename.erase(filename.size() - suffix.size());
		return filepath.parent_path() / filename;
	}
}
