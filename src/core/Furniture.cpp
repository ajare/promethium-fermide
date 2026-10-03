#include "core/Furniture.h"
#include "core/AgentTagRegistry.h"
#include "core/Marker.h"
#include "core/SerializationException.h"
#include <cmath>
#include <algorithm>
#include <set>
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
				if (!tiles.IsSequence() || tiles.size() == 0 || !points.IsSequence() || points.size() == 0
					|| entry["edges"] || entry["vertices"])
					throw SerializationException("Furniture requires artwork tiles and usable points, without side routes");
				if (entry["depth"] && entry["depth"].as<int>() != 0)
					throw SerializationException("Furniture currently requires fixed depth 0");
				std::set<std::pair<int, int>> offsets;
				bool first = true;
				for (auto tile : tiles)
				{
					FurnitureTile t{ tile["x"].as<int>(), tile["y"].as<int>(),
						tile["imageSet"].as<std::string>(), tile["image"].as<std::string>() };
					// Bound arithmetic and prohibit artwork below its supporting Floor.
					if (t.x < -65536 || t.x > 65536 || t.y < 0 || t.y > 65536
						|| !offsets.emplace(t.x, t.y).second)
						throw SerializationException("Invalid or duplicate Furniture tile offset");
					if (t.imageSet != "ObjectAtlas" || t.image.empty())
						throw SerializationException("Furniture artwork requires an ObjectAtlas Image-set region");
					if (first) { d.minX = t.x; d.minY = t.y; d.maxX = t.x + 1; d.maxY = t.y + 1; first = false; }
					else { d.minX = std::min(d.minX, t.x); d.minY = std::min(d.minY, t.y);
						d.maxX = std::max(d.maxX, t.x + 1); d.maxY = std::max(d.maxY, t.y + 1); }
					d.tiles.push_back(std::move(t));
				}
				std::string diagnostic;
				std::set<std::string> keys, labels;
				for (auto point : points)
				{
					FurnitureUsablePoint p{ point["key"].as<std::string>(), point["label"].as<std::string>(), point["x"].as<float>() };
					if ((point["y"] && point["y"].as<float>() != 0) || !std::isfinite(p.x)
						|| p.x < d.minX || p.x >= d.maxX || p.key.empty()
						|| !Marker::nameIsValid(p.label, &diagnostic) || Marker::trimName(p.label) != p.label
						|| !keys.insert(p.key).second || !labels.insert(p.label).second)
						throw SerializationException("Invalid Furniture usable point key, label or floor-height offset");
					d.usablePoints.push_back(std::move(p));
				}
				if (d.key.empty() || !Marker::nameIsValid(d.label, &diagnostic))
					throw SerializationException("Invalid Furniture definition key or label");
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
