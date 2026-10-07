#include "core/Furniture.h"
#include "core/AgentTagRegistry.h"
#include "core/Marker.h"
#include "core/SerializationException.h"
#include <cmath>
#include <algorithm>
#include <set>
#include <utility>
#include <fstream>
#include "FurnitureLua.h"

namespace core
{
	namespace { FurnitureCatalogue::Loader resourceLoader; }

	namespace
	{
		void readDefinitions(script::FurnitureValue const& root, std::map<std::string, FurnitureDefinition>& definitionsOut)
		{
			auto definitions = root["definitions"];
			if (!definitions.IsSequence() || definitions.size() == 0)
				throw SerializationException("Furniture catalogue needs definitions");
			for (auto entry : definitions)
			{
				FurnitureDefinition d;
				d.key = entry["key"].template as<std::string>();
				d.label = entry["label"].template as<std::string>();
				auto tiles = entry["tiles"];
				auto points = entry["usablePoints"];
				if (!tiles.IsSequence() || tiles.size() == 0 || !points.IsSequence())
					throw SerializationException("Furniture requires artwork tiles and a usable-points list (which may be empty)");
				std::set<std::pair<int, int>> offsets;
				bool first = true;
				for (auto tile : tiles)
				{
					FurnitureTile t{ tile["x"].template as<int>(), tile["y"].template as<int>(),
						tile["imageSet"].template as<std::string>(), tile["image"].template as<std::string>() };
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
					FurnitureUsablePoint p{ point["key"].template as<std::string>(), point["label"].template as<std::string>(), point["x"].template as<float>(),
						point["blocksPathing"] ? point["blocksPathing"].template as<bool>() : true,
						point["supportElevation"] ? point["supportElevation"].template as<float>() : 0.f };
					if ((point["y"] && point["y"].template as<float>() != 0) || !std::isfinite(p.x)
						|| !std::isfinite(p.supportElevation) || p.supportElevation < 0.f
						|| p.x < d.minX || p.x >= d.maxX || p.key.empty()
						|| !Marker::nameIsValid(p.label, &diagnostic) || Marker::trimName(p.label) != p.label
						|| !keys.insert(p.key).second || !labels.insert(p.label).second)
						throw SerializationException("Invalid Furniture usable point key, label or floor-height offset");
					d.usablePoints.push_back(std::move(p));
				}
				d.sideRoutes = entry["sideRoutes"] ? entry["sideRoutes"].template as<bool>() : false;
				if (d.sideRoutes && !entry["vertices"])
					throw SerializationException("Furniture side routes require explicit vertices and edges");
				if (entry["vertices"] || entry["edges"])
				{
					auto vertices = entry["vertices"], edges = entry["edges"];
					if (!vertices.IsSequence() || vertices.size() == 0 || !edges.IsSequence())
						throw SerializationException("Furniture routes require explicit vertices and edges");
					std::set<std::string> vertexKeys, boundPoints;
					for (auto vertex : vertices)
					{
						FurnitureRoutingVertex v{ vertex["key"].template as<std::string>(), vertex["x"].template as<float>(),
							vertex["usablePoint"] ? vertex["usablePoint"].template as<std::string>() : "",
							vertex["external"] ? vertex["external"].template as<bool>() : false };
						if (v.key.empty() || !vertexKeys.insert(v.key).second || !std::isfinite(v.x)
							|| v.x < d.minX || v.x > d.maxX || (vertex["y"] && vertex["y"].template as<float>() != 0))
							throw SerializationException("Invalid Furniture routing vertex");
						if (!v.usablePoint.empty())
						{
							auto point = std::find_if(d.usablePoints.begin(), d.usablePoints.end(),
								[&](auto const& p) { return p.key == v.usablePoint && p.x == v.x; });
							if (point == d.usablePoints.end() || !boundPoints.insert(v.usablePoint).second)
								throw SerializationException("Invalid Furniture usable vertex binding");
						}
						d.vertices.push_back(std::move(v));
					}
					if (boundPoints.size() != d.usablePoints.size())
						throw SerializationException("Every Furniture usable point needs an explicit vertex");
					std::set<std::pair<std::string, std::string>> connections;
					for (auto edge : edges)
					{
						FurnitureRoutingEdge e{ edge["from"].template as<std::string>(), edge["to"].template as<std::string>(), {} };
						if (edge["depthOffset"]) e.depthOffset = edge["depthOffset"].template as<int>();
						if (!vertexKeys.contains(e.from) || !vertexKeys.contains(e.to) || e.from == e.to
							|| !connections.emplace(std::min(e.from, e.to), std::max(e.from, e.to)).second)
							throw SerializationException("Invalid or duplicate Furniture routing edge");
						d.edges.push_back(std::move(e));
					}
				}
				if (d.key.empty() || !Marker::nameIsValid(d.label, &diagnostic))
					throw SerializationException("Invalid Furniture definition key or label");
				if (!definitionsOut.emplace(d.key, d).second)
					throw SerializationException("Duplicate Furniture definition key: " + d.key);
			}
		}
	}

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
			if (!filenameIsValid(path.filename().string()))
				throw SerializationException("Furniture catalogue must end with .furniture.lua; YAML Furniture requires conversion to Lua");
			auto result = std::make_shared<FurnitureCatalogue>();
			std::ifstream file(path, std::ios::binary);
			if (!file) throw SerializationException("Cannot read Furniture catalogue");
			char buffer[4096];
			while (file.read(buffer, sizeof(buffer)) || file.gcount())
			{
				result->mLuaSource.append(buffer, static_cast<size_t>(file.gcount()));
				if (result->mLuaSource.size() > 256 * 1024) throw SerializationException("Furniture source exceeds budget");
			}
			if (file.bad()) throw SerializationException("Cannot read Furniture catalogue");
			auto root = script::readFurnitureLua(result->mLuaSource);
			script::validateFurnitureLua(root);
			result->mUuid = root["uuid"].as<std::string>();
			if (!AgentTagRegistry::uuidIsValid(result->mUuid)) throw SerializationException("Invalid Furniture catalogue UUID");
			readDefinitions(root, result->mDefinitions);
			for (auto const& entry : root["definitions"])
				result->mDefinitions.at(entry["key"].as<std::string>()).hasUse = static_cast<bool>(entry["use"]);
			return result;
		}
		catch (std::exception const& error)
		{
			throw SerializationException("Could not load Furniture catalogue " + path.string() + ": " + error.what());
		}
	}

	bool FurnitureCatalogue::filenameIsValid(std::string const& filename)
	{
		return !filename.empty() && filename.ends_with(".furniture.lua");
	}

	std::shared_ptr<const FurnitureCatalogue> FurnitureCatalogue::load(std::filesystem::path const& path)
	{
		if (!filenameIsValid(path.filename().string()))
			throw SerializationException("Furniture catalogue must end with .furniture.lua; YAML Furniture requires conversion to Lua: " + path.string());
		return resourceLoader ? resourceLoader(path) : readFile(path);
	}
	void FurnitureCatalogue::setResourceLoader(Loader loader) { resourceLoader = std::move(loader); }
}
