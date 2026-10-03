#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include "core/EntityId.h"

namespace core
{
	// Slice 1 deliberately accepts only a single World tile and one fixed-depth-0
	// usable point. Later catalogue layouts must not be silently approximated.
	struct FurnitureDefinition
	{
		std::string key, label, imageSet, image, usableKey, usableLabel;
		float usableX{ 0.5f };
	};

	class FurnitureCatalogue
	{
		std::string mUuid;
		std::map<std::string, FurnitureDefinition> mDefinitions;
	public:
		std::string const& uuid() const { return mUuid; }
		auto const& definitions() const { return mDefinitions; }
		FurnitureDefinition const* definition(std::string const& key) const;
		static std::shared_ptr<const FurnitureCatalogue> readFile(std::filesystem::path const& path);
		static std::shared_ptr<const FurnitureCatalogue> load(std::filesystem::path const& path);
		// The rendering service installs its resource-managed loader. Core-only
		// document tools use the identical catalogue parser without GPU ownership.
		using Loader = std::function<std::shared_ptr<const FurnitureCatalogue>(std::filesystem::path const&)>;
		static void setResourceLoader(Loader loader);
	};

	struct FurnitureInstance
	{
		uint64_t id{ 0 };
		uint32_t sector{ 0 };
		float x{ 0 }, y{ 0 }; // Location-local supporting Floor position
		std::string definitionKey, name, usableKey;
		MarkerId marker{};
	};
}
