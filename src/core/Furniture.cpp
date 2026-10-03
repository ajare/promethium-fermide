#include "core/Furniture.h"
#include "core/AgentTagRegistry.h"
#include "core/Marker.h"
#include "core/SerializationException.h"
#include <cmath>
#include <utility>
#include <yaml-cpp/yaml.h>

namespace core
{
	namespace { FurnitureCatalogue::Loader resourceLoader; }

	FurnitureDefinition const* FurnitureCatalogue::definition(std::string const& key) const
	{
		auto found = mDefinitions.find(key);
		return found == mDefinitions.end() ? nullptr : &found->second;
	}

	std::shared_ptr<const FurnitureCatalogue> FurnitureCatalogue::readFile(std::filesystem::path const& path)
	{
		try
		{
			if (!std::filesystem::is_regular_file(path))
				throw SerializationException("Missing Furniture catalogue: " + path.string());
			auto root = YAML::LoadFile(path.string())["furnitureCatalogue"];
			if (!root || root["version"].as<unsigned>() != 1)
				throw SerializationException("Unsupported Furniture catalogue version");
			auto result = std::make_shared<FurnitureCatalogue>();
			result->mUuid = root["uuid"].as<std::string>();
			if (!AgentTagRegistry::uuidIsValid(result->mUuid))
				throw SerializationException("Invalid Furniture catalogue UUID");
			auto definitions = root["definitions"];
			if (!definitions.IsSequence() || definitions.size() == 0)
				throw SerializationException("Furniture catalogue needs definitions");
			for (auto entry : definitions)
			{
				FurnitureDefinition d;
				d.key = entry["key"].as<std::string>();
				d.label = entry["label"].as<std::string>();
				auto tiles = entry["tiles"];
				auto points = entry["usablePoints"];
				if (!tiles.IsSequence() || tiles.size() != 1 || !points.IsSequence() || points.size() != 1
					|| entry["edges"] || entry["vertices"])
					throw SerializationException("Furniture slice 1 requires one tile and one usable point, without side routes");
				auto tile = tiles[0];
				if (tile["x"].as<int>() != 0 || tile["y"].as<int>() != 0)
					throw SerializationException("Furniture slice 1 tile offset must be (0, 0)");
				d.imageSet = tile["imageSet"].as<std::string>();
				d.image = tile["image"].as<std::string>();
				if (d.imageSet != "ObjectAtlas" || d.image.empty())
					throw SerializationException("Furniture slice 1 artwork requires an ObjectAtlas Image-set region");
				d.usableKey = points[0]["key"].as<std::string>();
				d.usableLabel = points[0]["label"].as<std::string>();
				d.usableX = points[0]["x"].as<float>();
				if ((points[0]["y"] && points[0]["y"].as<float>() != 0)
					|| (entry["depth"] && entry["depth"].as<int>() != 0))
					throw SerializationException("Furniture slice 1 requires floor-height points and fixed depth 0");
				std::string diagnostic;
				if (d.key.empty() || d.usableKey.empty() || !Marker::nameIsValid(d.label, &diagnostic)
					|| !Marker::nameIsValid(d.usableLabel, &diagnostic)
					|| !std::isfinite(d.usableX) || d.usableX < 0 || d.usableX >= 1)
					throw SerializationException("Invalid Furniture definition key, label or usable point");
				if (!result->mDefinitions.emplace(d.key, d).second)
					throw SerializationException("Duplicate Furniture definition key: " + d.key);
			}
			return result;
		}
		catch (std::exception const& error)
		{
			throw SerializationException("Could not load Furniture catalogue " + path.string() + ": " + error.what());
		}
	}

	std::shared_ptr<const FurnitureCatalogue> FurnitureCatalogue::load(std::filesystem::path const& path)
	{
		return resourceLoader ? resourceLoader(path) : readFile(path);
	}
	void FurnitureCatalogue::setResourceLoader(Loader loader) { resourceLoader = std::move(loader); }
}
