#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "core/EntityId.h"
#include "core/ActionRegistry.h"

namespace core
{
	struct FurnitureTile
	{
		int x{ 0 }, y{ 0 };
		std::string imageSet, image;
	};
	enum class UsablePointAction { Sit, Lying };

	struct FurnitureUsablePoint
	{
		std::string key, label;
		float x{ 0.5f };
		bool blocksPathing{ true }; // default for newly created owned Markers
		std::optional<UsablePointAction> action{};
	};
	struct FurnitureRoutingVertex
	{
		std::string key;
		float x{ 0 };
		// A usable-point key binds this vertex to its World-owned Marker.
		std::string usablePoint;
		bool external{ false };
	};
	struct FurnitureRoutingEdge
	{
		std::string from, to;
		std::optional<int> depthOffset; // absent means fixed 0
	};
	struct FurnitureDefinition
	{
		std::string key, label;
		std::vector<FurnitureTile> tiles;
		std::vector<FurnitureUsablePoint> usablePoints;
		std::vector<FurnitureRoutingVertex> vertices;
		std::vector<FurnitureRoutingEdge> edges;
		bool sideRoutes{ false }; // replace the ordinary floor span
		bool hasUse{ false }; // validated paired Lua callbacks
		// Full artwork rectangle, including transparent pixels and layout gaps.
		int minX{ 0 }, minY{ 0 }, maxX{ 0 }, maxY{ 0 };
	};
	struct FurnitureDestination
	{
		std::string key;
		MarkerId marker{};
		std::string name;
		uint32_t properties{ 0 };
	};

	class FurnitureCatalogue
	{
		std::string mUuid;
		// Accepted executable source snapshot, never serialized or exposed as VM state.
		std::string mLuaSource;
		std::map<std::string, FurnitureDefinition> mDefinitions;
		friend class World;
		ActionExecutionResult executeUse(std::string_view key, bool finishing, ActionViews const& views) const
		{ return ActionRegistry::executeFurniture(mLuaSource, key, finishing, views); }
	public:
		std::string const& uuid() const { return mUuid; }
		auto const& definitions() const { return mDefinitions; }
		FurnitureDefinition const* definition(std::string const& key) const;
		static bool filenameIsValid(std::string const& filename);
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
		std::string definitionKey, name;
		std::vector<FurnitureDestination> destinations;
		// First destination retained for compatibility with the chair authoring API.
		MarkerId marker{};
		int localDepth{ 0 };
	};
}
