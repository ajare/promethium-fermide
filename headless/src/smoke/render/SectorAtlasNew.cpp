// SectorAtlasNew fills every surface-bearing Sector type with its own
// full-cell Infrastructure tile (#1017).
//
// Room, Corridor, Ladder, Lift, Shuttle, Stairwell, Staircase, Airlock,
// Chamber and Dumbwaiter each get one tile that fills one World cell and
// repeats per cell across the Sector's area. Two of those surfaces become
// live that were declared in the old manifest but never drawn: the Shuttle
// and the Staircase used to borrow the Lift and Stairwell tiles, and no
// longer do in this family. Their procedural carriage and steps still draw
// on top, over their own art rather than over a borrowed shaft. The old
// family keeps borrowing both, because its sheet has always declared the
// two regions and never meant them to be drawn (#1027).
//
// The boundary strips are gone. The new set carries no boundary regions at
// all, so every Sector edge takes the renderer's primitive line fallback -
// which is an artwork removal only. The wall-span calculation, the
// partial-height opening handling and the floor-and-ceiling-at-bounds-only
// rule are unchanged, and the existing draw-order and Facade modules are
// what prove that.
//
// Background and Facade get no tile: they are authored flat colour by
// domain definition, and the renderer fills them without ever asking this
// set for a surface.
//
// The artwork is installed the way the render stack installs it - pixel
// regions from the `ImageSet` the application manifest declares, render
// semantics from the authored tileset the atlas-set registry names for it -
// so these checks exercise the same pairing the application does. Every
// assertion is on the recorded WorldDrawList command vector, the stable CPU
// seam ADR 0010 names, or on the refusal a malformed tileset gets at the
// loader seam.

#include "Checks.h"

#include "DumbwaiterFixture.h"
#include "ObjectTileset.h"
#include "Render.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	std::string const kTileset = "resources/textures/sectors-new.tileset.yaml";

	// The ten surface-bearing Sector types, by the tile name each one asks
	// the atlas for. Background and Facade are absent because they are
	// authored flat colour and never look up a surface.
	std::vector<std::string> const kSurfaces{ "room", "corridor", "ladder",
		"lift", "shuttle", "stairwell", "staircase", "airlock", "chamber",
		"dumbwaiter" };

	// ------------------------------------------------------------------
	// The atlas as the application holds it.
	// ------------------------------------------------------------------

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
		std::string location;
		std::string imageReference;
		std::map<std::string, std::string> options;
		AtlasPixelRegions regions;
	};

	// One entry of the application asset manifest. Each field is guarded on
	// its own because a yaml-cpp node that was never written is a zombie,
	// and subscripting one throws rather than reading as absent.
	ManifestResource manifestResource(YAML::Node const& manifest,
		std::string const& name)
	{
		ManifestResource found;
		for (auto const& resource : manifest["Resources"]["Resource"])
		{
			if (!resource.IsMap()) continue;
			if (resource["name"].as<std::string>() != name) continue;
			found.found = true;
			found.type = resource["type"].as<std::string>();
			if (resource["location"])
				found.location = resource["location"].as<std::string>();
			if (auto const options = resource["Option"])
				for (auto const& option : options)
					found.options[option["name"].as<std::string>()] =
						option["value"].as<std::string>();
			if (auto const dependencies = resource["DependentResources"])
			{
				auto const dependency = dependencies["DependentResource"];
				if (dependency.IsMap() && dependency["ref"])
					found.imageReference = dependency["ref"].as<std::string>();
			}
			if (auto const definitions = resource["Definitions"])
				if (auto const definition = definitions["Definition"])
					if (auto const images = definition["Images"])
						for (auto const& image : images["Image"])
							found.regions.emplace(image["name"].as<std::string>(),
								AtlasPixelRegion{ image["x"].as<int>(),
									image["y"].as<int>(), image["width"].as<int>(),
									image["height"].as<int>() });
		}
		return found;
	}

	YAML::Node bundledManifest(smoke::Context const& context)
	{
		return YAML::LoadFile(context.fixture("resources/Resources.yaml").string());
	}

	// The authored atlas-set registry beside the manifest: which tileset is
	// asked about for which atlas, and in which authored format.
	std::map<std::string, std::string> atlasSetTilesets(
		smoke::Context const& context)
	{
		auto const root = YAML::LoadFile(
			context.fixture("resources/atlas-sets.yaml").string());
		require(root.IsMap() && root["sets"].IsMap(),
			"resources/atlas-sets.yaml has no `sets` map of atlas name to authored "
			"tileset");
		std::map<std::string, std::string> sets;
		for (auto const& entry : root["sets"])
			sets[entry.first.as<std::string>()] =
				entry.second["tileset"].as<std::string>();
		return sets;
	}

	// The PNG's own dimensions, read from its IHDR chunk. The tileset states
	// the sheet size; the committed file has to agree with it, because the
	// pixel regions are cut against that size.
	std::pair<int, int> pngSize(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(bool(file), "Could not open the atlas image: " + path.string());
		unsigned char header[24]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		require(file.gcount() == static_cast<std::streamsize>(sizeof(header)),
			"The atlas image is too short to carry its header: " + path.string());
		auto const be = [&](int offset) {
			return (int(header[offset]) << 24) | (int(header[offset + 1]) << 16)
				| (int(header[offset + 2]) << 8) | int(header[offset + 3]);
		};
		require(be(0) == 0x89504e47 && be(12) == 0x49484452,
			"The atlas image is not a PNG: " + path.string());
		return { be(16), be(20) };
	}

	// Installs the new Sector atlas through the one install path the render
	// stack uses: pixel regions from the `ImageSet`, render semantics from
	// the authored tileset.
	SectorTileset installNewSectorAtlas(smoke::Context const& context)
	{
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "SectorAtlasNew");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no SectorAtlasNew ImageSet");
		require(atlasSetTilesets(context)["SectorAtlasNew"]
			== "textures/sectors-new.tileset.yaml",
			"The atlas-set registry does not name sectors-new.tileset.yaml for "
			"SectorAtlasNew");
		auto const authored = SectorTileset::load(context.fixture(kTileset));
		require(authored.family == SectorTilesetFamily::New,
			"sectors-new.tileset.yaml did not load as the New family its version 2 "
			"format declares");
		SectorTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		// The family is installed with the tiles, exactly as the render stack
		// installs it. Dropping it here would leave every check below running
		// on a set that is not the one the game draws (#1027).
		installed.family = authored.family;
		installed.tiles = installAtlasTiles(authored.tiles, set.regions,
			authored.width, authored.height, "SectorAtlasNew");
		for (auto const& [name, artwork] : installed.tiles)
			installed.surfaces.emplace(name, artwork.region);
		setSectorTileset(installed, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The installed new Sector atlas did not come up as the New family");
		return installed;
	}

	// Installs the old-family Sector atlas the way the render stack installs
	// one: pixel regions from the application manifest's `ImageSet`, render
	// semantics from the authored tileset the atlas-set registry names for
	// it, and the family the loader read out of that file's `version`. The
	// `boundary-` prefix the manifest carries is the install's convention
	// and not the tileset's, so it is split here the way the stack splits
	// it (#1027).
	SectorTileset installOldSectorAtlas(smoke::Context const& context)
	{
		auto const setName = std::string{ "SectorAtlas" };
		auto const set = manifestResource(bundledManifest(context), setName);
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no " + setName + " ImageSet");
		auto const tileset = atlasSetTilesets(context)[setName];
		require(tileset == "textures/sectors.tileset.yaml",
			"The atlas-set registry does not name the version 1 sector tileset for "
			+ setName);
		// The registry's path is relative to the registry file, which sits in
		// `resources/` beside the manifest.
		auto const authored = SectorTileset::load(
			context.fixture("resources/" + tileset));
		require(authored.family == SectorTilesetFamily::Old,
			setName + "'s tileset did not load as the Old family its version 1 "
			"format declares");
		SectorTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.family = authored.family;
		installed.tiles = installAtlasTiles(authored.tiles, set.regions,
			authored.width, authored.height, setName);
		for (auto const& [name, artwork] : installed.tiles)
		{
			if (name.rfind("boundary-", 0) == 0)
				installed.boundaries.emplace(name.substr(9), artwork.region);
			else
				installed.surfaces.emplace(name, artwork.region);
		}
		setSectorTileset(installed, static_cast<TextureId>(1));
		require(!sectorTilesetIsNewFamily(),
			"The installed " + setName + " reported as the New family");
		return installed;
	}

	// ------------------------------------------------------------------
	// One Sector per surface type, in the smallest World that hosts it.
	// ------------------------------------------------------------------

	std::shared_ptr<const core::Sector> sectorNamed(core::World const& world,
		std::string const& name)
	{
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			if (world.getSector(index)->getName() == name) return world.getSector(index);
		return nullptr;
	}

	struct SurfaceFixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> sector;
	};

	// Each layout is the smallest one the domain accepts for that Sector
	// type: a transit needs its landing Locations on the Layer in front of
	// it, an Airlock or Chamber lands in Locations on its own Layer, and a
	// Staircase needs the Location it rises through.
	SurfaceFixture fixtureFor(std::string const& surface)
	{
		if (surface == "room")
		{
			auto world = std::make_shared<core::World>("Room surface", 4, 1);
			world->addRoom("Room", 0, 0, 0, 2, 1);
			world->finishBuild();
			return { world, sectorNamed(*world, "Room") };
		}
		if (surface == "corridor")
		{
			auto world = std::make_shared<core::World>("Corridor surface", 4, 1);
			world->addCorridor(0, 0, 2);
			world->finishBuild();
			return { world, sectorNamed(*world, "Corridor") };
		}
		if (surface == "ladder")
		{
			auto world = std::make_shared<core::World>("Ladder surface", 6, 3);
			for (uint32_t y = 0; y < 3; ++y) world->addCorridor(y, 0, 6);
			world->addLadder(1, 0, 1, { 3, false, true });
			world->finishBuild();
			return { world, sectorNamed(*world, "Ladder") };
		}
		if (surface == "lift")
		{
			auto world = std::make_shared<core::World>("Lift surface", 8, 6);
			world->addCorridor(0, 0, 7);
			world->addCorridor(2, 0, 7);
			core::World::CreateLiftOptions options;
			options.cellsWide = 1;
			options.stopOffsets = { 0, 2 };
			options.capacity = 1;
			world->addLift(1, 0, 2, options);
			world->finishBuild();
			return { world, sectorNamed(*world, "Lift") };
		}
		if (surface == "shuttle")
		{
			auto world = std::make_shared<core::World>("Shuttle surface", 16, 2);
			world->addRoom("Left terminal", 0, 0, 0, 3, 1);
			world->addRoom("Right terminal", 0, 0, 10, 3, 1);
			core::World::CreateShuttleOptions options{ 1, 3, { 0, 10 }, 0 };
			options.doorMask = 0b101;
			world->addShuttle(1, 0, 0, 13, options);
			world->finishBuild();
			return { world, sectorNamed(*world, "Shuttle") };
		}
		if (surface == "stairwell")
		{
			auto world = std::make_shared<core::World>("Stairwell surface", 12, 4);
			while (world->getLayerCount() < 2) world->addLayer();
			// The Stairwell lands in the Layer immediately in front of it and
			// sits beside Locations on its own Layer, as the depot layout does.
			world->addCorridor(0, 2, 0, 12, 1);
			world->addCorridor(0, 3, 0, 12, 1);
			world->addCorridor(1, 2, 0, 8, 1);
			world->addCorridor(1, 3, 0, 8, 1);
			world->addStairwell(1, 2, 8, 2, CORE_SIDE_RIGHT);
			world->finishBuild();
			return { world, sectorNamed(*world, "Stairwell") };
		}
		if (surface == "staircase")
		{
			auto world = std::make_shared<core::World>("Staircase surface", 12, 3);
			while (world->getLayerCount() < 2) world->addLayer();
			world->addCorridor(0, 0, 6);
			world->addCorridor(1, 0, 7);
			auto const room = world->addRoom("Upper room", 0, 1, 7, 3, 1);
			world->removeLocationWall(room, 0, CORE_SIDE_LEFT);
			world->addStaircase(1, 0, 5,
				core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
			world->finishBuild();
			return { world, sectorNamed(*world, "Staircase") };
		}
		if (surface == "airlock")
		{
			auto world = std::make_shared<core::World>("Airlock surface", 10, 2);
			world->addRoom("Left", 0, 0, 0, 2, 1);
			world->addCorridor(0, 0, 5, 2, 1);
			world->addAirlock(0, 0, 2, 3);
			world->finishBuild();
			return { world, sectorNamed(*world, "Airlock") };
		}
		if (surface == "chamber")
		{
			auto world = std::make_shared<core::World>("Chamber surface", 12, 2);
			world->addRoom("Left", 0, 0, 0, 3, 1);
			world->addCorridor(0, 0, 6, 3, 1);
			world->addChamber(0, 0, 3, 3, true,
				core::ChamberSubtype::Decontamination);
			world->finishBuild();
			return { world, sectorNamed(*world, "Chamber") };
		}
		if (surface == "dumbwaiter")
		{
			auto world = dumbwaiter_fixture::make();
			world->addDumbwaiter(1, 0, 2);
			return { world, sectorNamed(*world, "Dumbwaiter") };
		}
		throw std::runtime_error("No fixture for the surface: " + surface);
	}

	// ------------------------------------------------------------------
	// Reading the surface back out of the command stream.
	// ------------------------------------------------------------------

	// The tile one textured quad started from. The comparison is on the centre
	// of the sampled UV range, not on its corner: a crop moves the corner - a
	// short Sector samples its tile from part-way down - but it can never move
	// the centre off the tile it came from. The tiles are disjoint regions of
	// one sheet, so the centre lands on exactly one of them.
	std::string tileSampledBy(WorldDrawList::Triangle const& triangle,
		SectorTileset const& atlas)
	{
		if (atlas.width <= 0 || atlas.height <= 0) return {};
		float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
		for (auto const& uv : triangle.texcoords)
		{
			u0 = std::min(u0, uv.x);
			v0 = std::min(v0, uv.y);
			u1 = std::max(u1, uv.x);
			v1 = std::max(v1, uv.y);
		}
		float const centreU = (u0 + u1) * 0.5f;
		float const centreV = (v0 + v1) * 0.5f;
		for (auto const& [name, artwork] : atlas.tiles)
		{
			auto const left = float(artwork.region.x) / float(atlas.width);
			auto const right =
				float(artwork.region.x + artwork.region.width) / float(atlas.width);
			auto const top = float(artwork.region.y) / float(atlas.height);
			auto const bottom =
				float(artwork.region.y + artwork.region.height) / float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
				&& centreV < bottom)
				return name;
		}
		return {};
	}

	// Every distinct tile the Sector's own surface sampled, in the order
	// they appeared. A Sector three cells wide samples its one tile three
	// times and reads as one name.
	std::vector<std::string> surfaceTiles(
		std::shared_ptr<const core::Sector> sector, uint32_t layer,
		SectorTileset const& atlas)
	{
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderSector(sector, layer, LayerRenderStyle::Solid, false, White,
			&commands);
		std::vector<std::string> names;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			auto const name = tileSampledBy(*triangle, atlas);
			if (std::find(names.begin(), names.end(), name) == names.end())
				names.push_back(name);
		}
		return names;
	}

	std::string joined(std::vector<std::string> const& names)
	{
		std::ostringstream out;
		for (size_t index = 0; index < names.size(); ++index)
			out << (index ? ", " : "") << (names[index].empty() ? "<none>" : names[index]);
		return out.str();
	}

	// ------------------------------------------------------------------
	// The registration: ten full-cell tiles, no boundaries.
	// ------------------------------------------------------------------
	void tenFullCellTilesAreRegistered(smoke::Context const& context)
	{
		using smoke::require;
		auto const authored = SectorTileset::load(context.fixture(kTileset));
		require(authored.surfaces.size() == kSurfaces.size(),
			"SectorAtlasNew authors " + std::to_string(authored.surfaces.size())
			+ " surfaces; the set is ten");
		for (auto const& name : kSurfaces)
			require(authored.surfaces.count(name),
				"SectorAtlasNew registers no '" + name + "' surface");
		// The sheet is the 5x2 grid of full cells the set is sized for, on
		// the 128x320 authored grid one World cell now draws at.
		require(authored.width == 640 && authored.height == 640,
			"SectorAtlasNew is not the 640x640 sheet its set is sized for");
		auto const sheet = pngSize(context.fixture("resources/textures/sectors-new.png"));
		require(sheet.first == authored.width && sheet.second == authored.height,
			"The committed sectors-new.png is " + std::to_string(sheet.first) + "x"
			+ std::to_string(sheet.second) + ", the tileset states "
			+ std::to_string(authored.width) + "x"
			+ std::to_string(authored.height));
		for (auto const& [name, artwork] : authored.tiles)
		{
			// One full World cell, drawn at the cell's anchor: the repeat is
			// per cell, so a tile drawn any other way could not repeat.
			require(near(artwork.drawnSize.x, 1.0f)
				&& near(artwork.drawnSize.y, 1.0f),
				"'" + name + "' must be drawn at one full World cell");
			require(near(artwork.offset.x, 0.0f) && near(artwork.offset.y, 0.0f),
				"'" + name + "' must be drawn at its cell's anchor");
			require(artwork.region.width == 128 && artwork.region.height == 320,
				"'" + name + "' must fill its 128x320 authored cell");
		}
		// No boundary artwork in the set at all.
		require(authored.boundaries.empty(),
			"SectorAtlasNew carries boundary strips; the new set draws none");
		// The manifest's generated entries agree with the authored tileset,
		// so the runtime pixel contract cannot drift from what was authored.
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "SectorAtlasNew");
		require(set.found && set.type == "ImageSet",
			"SectorAtlasNew is not registered as an ImageSet");
		require(set.imageReference == "SectorAtlasNewImage",
			"SectorAtlasNew's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "SectorAtlasNewImage");
		require(image.found && image.type == "Image"
			&& image.location == "textures/sectors-new.png",
			"SectorAtlasNewImage is not the atlas image the tileset names");
		require(image.options.count("filtering")
				&& image.options.at("filtering") == "none"
				&& image.options.count("uv-style")
				&& image.options.at("uv-style") == "atlas",
			"SectorAtlasNewImage must be an unfiltered atlas image, as every "
			"viewport atlas is");
		require(set.regions.size() == authored.tiles.size(),
			"The manifest's SectorAtlasNew entries and the authored tileset "
			"differ in count");
		for (auto const& [name, artwork] : authored.tiles)
		{
			auto const region = set.regions.find(name);
			require(region != set.regions.end(),
				"Authored tile '" + name + "' has no generated ImageSet entry");
			require(region->second.x == artwork.region.x
				&& region->second.y == artwork.region.y
				&& region->second.width == artwork.region.width
				&& region->second.height == artwork.region.height,
				"Generated ImageSet entry for '" + name
				+ "' drifted from the authored tileset");
		}
		// The registry names this tileset for this atlas, as sector tiles.
		require(atlasSetTilesets(context)["SectorAtlasNew"]
			== "textures/sectors-new.tileset.yaml",
			"The atlas-set registry names no sector tileset for SectorAtlasNew");
	}

	// ------------------------------------------------------------------
	// No boundary regions: every edge takes the primitive line.
	// ------------------------------------------------------------------
	void everyEdgeFallsBackToThePrimitiveLine(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		require(atlas.boundaries.empty(),
			"The installed SectorAtlasNew carries boundary regions");
		require(hasSectorTileSurface("room"),
			"The installed atlas answers a lookup for a surface it authors");
		// The four strips the old set drew. Each lookup misses, which is the
		// renderer's cue to draw its primitive line: the ceiling and floor
		// at the Sector's bounds and the wall spans between them still get
		// drawn, by the code that was already there.
		for (auto const* edge : { "floor", "ceiling", "left", "right" })
		{
			WorldDrawList commands({ { 0, 0 }, { 1000, 1000 } });
			require(!drawSectorTileBoundary(edge, &commands, { 0, 0 },
					{ CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS }),
				std::string("The new set answered a lookup for the '") + edge
					+ "' boundary strip, so the primitive line fallback was never reached");
			require(commands.commands().empty(),
				std::string("The '") + edge
					+ "' boundary lookup drew something; only the fallback should run");
		}
	}

	// ------------------------------------------------------------------
	// Every surface-bearing Sector type resolves to its own tile.
	// ------------------------------------------------------------------
	void everySurfaceBearingSectorTypeResolvesToItsOwnTile(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		for (auto const& surface : kSurfaces)
		{
			auto const fixture = fixtureFor(surface);
			require(fixture.sector != nullptr,
				"No Sector was built for the '" + surface + "' surface");
			auto const tiles = surfaceTiles(fixture.sector,
				fixture.sector->getLayerIndex(), atlas);
			require(tiles.size() == 1 && tiles.front() == surface,
				"The '" + surface + "' Sector filled with " + joined(tiles)
					+ " instead of its own tile");
		}
	}

	// What part of its tile one surface quad reads, as fractions of the tile.
	// Atlas rows run top down, so v runs with them: 0 is the tile's ceiling
	// end, 1 its floor end.
	struct TileSpan
	{
		float top{};
		float floor{};
	};

	TileSpan spanSampledBy(WorldDrawList::Triangle const& triangle,
		SectorTileRegion const& region, int const atlasHeight)
	{
		if (atlasHeight <= 0 || region.height <= 1) return {};
		float lo = 1e9f, hi = -1e9f;
		for (auto const& uv : triangle.texcoords)
		{
			lo = std::min(lo, uv.y);
			hi = std::max(hi, uv.y);
		}
		float const inset = float(region.y) + 0.5f;
		float const span = float(region.height - 1);
		return { (lo * float(atlasHeight) - inset) / span,
			(hi * float(atlasHeight) - inset) / span };
	}

	// A Corridor is 0.7 of a Level and a Room 0.9, so neither fills the cell
	// its tile is drawn for. The repeat is anchored on the Sector's floor
	// line, so what reaches the screen is the bottom of the tile at one
	// artwork pixel to one screen pixel, and what is dropped is the top - the
	// band that would have stood above the Sector's ceiling. Anchored on the
	// top instead, every short Sector would drop its own floor and keep its
	// ceiling band down at floor level.
	void aShortSectorCropsTheTopOfItsTileOntoItsFloorLine(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		std::map<std::string, float> const shortSectors{
			{ "corridor", CORE_CORRIDOR_HEIGHT },
			{ "room", CORE_ROOM_STANDARD_HEIGHT } };
		for (auto const& [surface, height] : shortSectors)
		{
			auto const fixture = fixtureFor(surface);
			require(fixture.sector != nullptr,
				"No Sector was built for the '" + surface + "' surface");
			WorldDrawList commands({ { -100000.0f, -100000.0f },
				{ 100000.0f, 100000.0f } });
			renderSector(fixture.sector, fixture.sector->getLayerIndex(),
				LayerRenderStyle::Solid, false, White, &commands);
			int drawn{};
			for (auto const& command : commands.commands())
			{
				auto const* triangle =
					std::get_if<WorldDrawList::Triangle>(&command);
				if (!triangle
					|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
					continue;
				if (tileSampledBy(*triangle, atlas) != surface) continue;
				float left = 1e9f, right = -1e9f, top = 1e9f, bottom = -1e9f;
				for (auto const& p : triangle->positions)
				{
					left = std::min(left, p.x);
					right = std::max(right, p.x);
					top = std::min(top, p.y);
					bottom = std::max(bottom, p.y);
				}
				// The column is one whole World cell wide, so its width says
				// what a cell costs on screen at this zoom and its height says
				// what the Sector actually had to draw into.
				float const cellOnScreen =
					(right - left) * CORE_CELL_YX_RENDER_RATIO;
				require(cellOnScreen > 0.f,
					"The '" + surface + "' surface quad is too thin to measure");
				float const drawnFraction = (bottom - top) / cellOnScreen;
				require(near(drawnFraction, height),
					"The '" + surface + "' Sector drew " + std::to_string(drawnFraction)
						+ " of a Level; the Sector is " + std::to_string(height)
						+ " high, so its tile must be cropped, not stretched");
				auto const span = spanSampledBy(*triangle,
					atlas.tiles.at(surface).region, atlas.height);
				require(near(span.floor, 1.0f),
					"The '" + surface + "' crop stopped at " + std::to_string(span.floor)
						+ " of its tile; the tile's floor end must land on the "
						+ "Sector's floor line");
				require(near(span.top, 1.0f - height),
					"The '" + surface + "' crop samples from " + std::to_string(span.top)
						+ " of its tile; the top " + std::to_string(1.0f - height)
						+ " must be dropped");
				// One artwork pixel to one screen pixel: the slice sampled and
				// the area drawn are the same fraction of their whole.
				require(near(span.floor - span.top, drawnFraction),
					"The '" + surface + "' tile was squashed rather than cropped: "
						"it draws " + std::to_string(drawnFraction) + " of a cell from "
						+ std::to_string(span.floor - span.top) + " of its tile");
				++drawn;
			}
			require(drawn > 0,
				"The '" + surface + "' Sector drew no surface quad at all");
		}
	}

	// The two borrowings the ticket ends, named directly: the Shuttle no
	// longer wears the Lift's art and the Staircase no longer wears the
	// Stairwell's.
	void shuttleAndStaircaseNoLongerBorrow(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		std::map<std::string, std::string> const borrowed{
			{ "shuttle", "lift" }, { "staircase", "stairwell" } };
		for (auto const& [surface, wasBorrowing] : borrowed)
		{
			auto const fixture = fixtureFor(surface);
			require(fixture.sector != nullptr,
				"No Sector was built for the '" + surface + "' surface");
			auto const tiles = surfaceTiles(fixture.sector,
				fixture.sector->getLayerIndex(), atlas);
			require(std::find(tiles.begin(), tiles.end(), wasBorrowing)
					== tiles.end(),
				"The " + surface + " Sector still fills with the " + wasBorrowing
					+ "'s tile");
			require(tiles.size() == 1 && tiles.front() == surface,
				"The " + surface + " Sector filled with " + joined(tiles)
					+ " instead of its own tile");
		}
	}

	// ------------------------------------------------------------------
	// The procedural geometry still draws over the new surfaces.
	// ------------------------------------------------------------------
	void carriageAndStepsStillDrawOnTop(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		ObjectTileset objects;
		objects.width = 500;
		objects.height = 100;
		objects.sprites.emplace("shuttle-car-left",
			ObjectSprite{ { 0, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-middle",
			ObjectSprite{ { 100, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-right",
			ObjectSprite{ { 200, 0, 50, 100 }, false });
		objects.sprites.emplace("shuttle-car-connector",
			ObjectSprite{ { 300, 0, 50, 100 }, false });
		objects.sprites.emplace("door", ObjectSprite{ { 400, 0, 50, 100 }, false });
		setObjectTileset(std::move(objects), static_cast<TextureId>(2));

		// The Shuttle's carriage: composed from its own images, and every
		// one of them lands after the surface quads it rides on.
		{
			auto const fixture = fixtureFor("shuttle");
			require(fixture.sector != nullptr, "No Shuttle Sector was built");
			WorldDrawList commands({ { -100000.0f, -100000.0f },
				{ 100000.0f, 100000.0f } });
			renderSector(fixture.sector, fixture.sector->getLayerIndex(),
				LayerRenderStyle::Solid, false, White, &commands);
			size_t lastSurface{};
			bool anySurface = false;
			bool anyCarriage = false;
			size_t firstCarriage = static_cast<size_t>(-1);
			auto const& stream = commands.commands();
			for (size_t index = 0; index < stream.size(); ++index)
			{
				auto const* triangle = std::get_if<WorldDrawList::Triangle>(&stream[index]);
				if (!triangle) continue;
				if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
				{
					anySurface = true;
					lastSurface = index + 1;
				}
				else if (triangle->texture == WorldDrawList::Texture::ObjectAtlas)
				{
					anyCarriage = true;
					if (firstCarriage == static_cast<size_t>(-1)) firstCarriage = index;
				}
			}
			require(anySurface, "The Shuttle Sector emitted no surface");
			require(anyCarriage && firstCarriage >= lastSurface,
				"The Shuttle's procedural carriage no longer draws over the "
				"Sector surface");
		}

		// The Staircase's steps: the dark and light treads the pass emits
		// both land after the surface quads.
		{
			auto const fixture = fixtureFor("staircase");
			require(fixture.sector != nullptr, "No Staircase Sector was built");
			WorldDrawList commands({ { -100000.0f, -100000.0f },
				{ 100000.0f, 100000.0f } });
			renderSector(fixture.sector, fixture.sector->getLayerIndex(),
				LayerRenderStyle::Solid, false, White, &commands);
			size_t lastSurface{};
			bool anySurface = false;
			bool dark = false, light = false;
			auto const& stream = commands.commands();
			for (size_t index = 0; index < stream.size(); ++index)
			{
				if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&stream[index]))
				{
					if (triangle->texture == WorldDrawList::Texture::SectorAtlas)
					{
						anySurface = true;
						lastSurface = index + 1;
					}
					continue;
				}
				auto const* line = std::get_if<WorldDrawList::Line>(&stream[index]);
				if (!line) continue;
				dark |= line->colour == packColour(32, 32, 32, 255);
				light |= line->colour == packColour(220, 220, 220, 255);
			}
			require(anySurface, "The Staircase Sector emitted no surface");
			// Both treads are drawn after the surface, in the order the pass
			// emits them: the dark underlay first, then the light tread over
			// it, and both over the Sector's own art.
			size_t firstTread = static_cast<size_t>(-1);
			for (size_t index = lastSurface; index < stream.size(); ++index)
			{
				auto const* line = std::get_if<WorldDrawList::Line>(&stream[index]);
				if (line && (line->colour == packColour(32, 32, 32, 255)
					|| line->colour == packColour(220, 220, 220, 255)))
				{
					firstTread = index;
					break;
				}
			}
			require(dark && light && firstTread >= lastSurface,
				"The Staircase's procedural steps no longer draw over the "
				"Sector surface");
		}
		clearObjectTileset();
	}

	// ------------------------------------------------------------------
	// Background and Facade: authored flat colour, no tile lookup.
	// ------------------------------------------------------------------
	void flatColourSectorsTakeNoTile(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);

		auto world = std::make_shared<core::World>("Flat colour sectors", 8, 2);
		auto const backgroundIndex = world->addBackground(0, 0, 0, 2, 1);
		auto const facadeIndex = world->addFacade("Facade", 0, 0, 4, 2, 1);
		world->finishBuild();

		for (auto index : { backgroundIndex, facadeIndex })
		{
			auto const sector = world->getSector(index);
			require(sector != nullptr, "No flat-colour Sector was built");
			require(rendersAsFlatColour(sector->getType()),
				std::string("A Sector that should render flat colour is a ")
					+ std::to_string(int(sector->getType())));
			WorldDrawList commands({ { -100000.0f, -100000.0f },
				{ 100000.0f, 100000.0f } });
			renderSector(sector, 0, LayerRenderStyle::Solid, false, White,
				&commands);
			auto const fill = flatSurfaceColour(*sector);
			require(fill.has_value(),
				"A flat-colour Sector has no authored surface colour");
			bool filledWithItsOwnColour = false;
			for (auto const& command : commands.commands())
			{
				auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				if (!triangle) continue;
				require(triangle->texture != WorldDrawList::Texture::SectorAtlas,
					"A flat-colour Sector looked up a tile; it is authored as a "
					"colour, not as artwork");
				if (triangle->texture == WorldDrawList::Texture::None
					&& triangle->colour == *fill)
					filledWithItsOwnColour = true;
			}
			require(filledWithItsOwnColour,
				"The flat-colour Sector did not fill with its authored colour");
		}
	}

	// ------------------------------------------------------------------
	// The loader seam: what the new format refuses.
	// ------------------------------------------------------------------

	// A conforming version 2 sector tileset, written out so every refusal is
	// driven by a generated fixture rather than by reaching into the loader.
	std::string tilesetYaml(std::vector<std::string> const& surfaces,
		std::string const& drawnSize = "[1.0, 1.0]",
		std::string const& offset = "[0.0, 0.0]",
		bool withBoundaries = false)
	{
		std::ostringstream yaml;
		yaml << "version: 2\n"
			<< "image: invented.png\n"
			<< "size: [640, 640]\n"
			<< "tile_size: [128, 320]\n"
			<< "sprites:\n";
		int column = 0, row = 0;
		for (auto const& name : surfaces)
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column << ", " << row << "]\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: " << offset << "\n"
				<< "    drawn_size: " << drawnSize << "\n"
				<< "    tintable: true\n";
			if (++column == 5) { column = 0; ++row; }
		}
		if (withBoundaries)
			yaml << "boundaries:\n  floor: [0, 0, 128, 32]\n";
		return yaml.str();
	}

	// Loads a generated tileset and hands back the refusal, or empty when
	// the file was accepted.
	std::string loadRefusal(smoke::Context const& context, std::string const& tag,
		std::string const& yaml)
	{
		auto const path = context.temporaryRoot() / (tag + ".tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml;
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		try
		{
			SectorTileset::load(path);
		}
		catch (std::exception const& error)
		{
			return std::string(error.what());
		}
		return {};
	}

	// Every one of the ten surface-bearing types has to be present. A set
	// missing one would leave that Sector with no surface to draw.
	void loaderRefusesASetMissingASurface(smoke::Context const& context)
	{
		using smoke::require;
		auto missing = kSurfaces;
		missing.erase(std::find(missing.begin(), missing.end(), "airlock"));
		auto const refusal = loadRefusal(context, "missing-surface",
			tilesetYaml(missing));
		require(refusal.find("Missing sector surface: airlock")
			!= std::string::npos,
			"A set with no Airlock surface was accepted: " + refusal);
	}

	// The boundary strips are dropped, and a file that still declares them
	// is refused rather than quietly ignored: an ignored map is how dropped
	// artwork comes back.
	void loaderRefusesABoundaryStripInTheNewSet(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "declared-boundaries",
			tilesetYaml(kSurfaces, "[1.0, 1.0]", "[0.0, 0.0]", true));
		require(!refusal.empty(),
			"A version 2 sector tileset declaring boundary strips was accepted");
		require(refusal.find("boundary") != std::string::npos
			&& refusal.find("primitive line") != std::string::npos,
			"The refusal must say the new set draws no boundary strips and the "
			"edge takes the primitive line: " + refusal);
	}

	// The repeat is per World cell, so a surface drawn at any other size
	// repeats on a geometry the rule cannot express.
	void loaderRefusesASurfaceThatIsNotOneFullCell(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "drawn-two-cells",
			tilesetYaml(kSurfaces, "[2.0, 1.0]"));
		require(refusal.find("one full World cell") != std::string::npos,
			"A surface drawn two cells wide was accepted: " + refusal);
	}

	// The same for a surface offset off its cell's anchor.
	void loaderRefusesASurfaceOffsetFromItsCell(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "offset-off-cell",
			tilesetYaml(kSurfaces, "[1.0, 1.0]", "[0.5, 0.0]"));
		require(refusal.find("anchor") != std::string::npos,
			"A surface offset off its cell's anchor was accepted: " + refusal);
	}

	// A conforming set loads, and every tile keeps the semantics the file
	// authored rather than anything the loader invented.
	void loaderAcceptsAConformingSet(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "conforming.tileset.yaml";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << tilesetYaml(kSurfaces);
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		auto const loaded = SectorTileset::load(path);
		require(loaded.surfaces.size() == kSurfaces.size(),
			"A conforming set did not load all ten surfaces");
		require(loaded.boundaries.empty(),
			"A conforming version 2 set gained boundary regions");
		for (auto const& [name, artwork] : loaded.tiles)
		{
			require(near(artwork.drawnSize.x, 1.0f)
				&& near(artwork.drawnSize.y, 1.0f),
				"'" + name + "' lost its authored drawn size on load");
			require(artwork.tintable,
				"'" + name + "' lost its authored tintable flag on load");
		}
	}

	// The fallback that keeps an older atlas honest. The v1 set has no
	// Airlock, Chamber or Dumbwaiter tile; those types keep drawing the
	// shared surface they borrowed instead of the missing-tile magenta.
	void anOlderAtlasStillBorrowsForATypeItHasNoTileFor(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const old = SectorTileset::load(
			context.fixture("resources/textures/sectors.tileset.yaml"));
		require(old.boundaries.size() == 4,
			"The version 1 sector tileset no longer carries the four boundary "
			"strips it was authored with");
		require(!old.surfaces.count("airlock"),
			"The version 1 set gained an Airlock tile; the fallback below would "
			"no longer be proving anything");
		setSectorTileset(old, static_cast<TextureId>(1));
		for (auto const& surface : { "airlock", "chamber", "dumbwaiter" })
		{
			auto const fixture = fixtureFor(surface);
			require(fixture.sector != nullptr,
				"No Sector was built for the '" + std::string(surface) + "' surface");
			auto const tiles = surfaceTiles(fixture.sector,
				fixture.sector->getLayerIndex(), old);
			require(tiles.size() == 1
					&& (tiles.front() == "corridor" || tiles.front() == "lift"),
				"An older atlas left the " + std::string(surface) + " Sector on "
					+ joined(tiles) + "; it should still borrow a surface it has");
		}
	}

	// The pin #1027 asks for. The old family declares `shuttle` and
	// `staircase` on its sheet and never drew either: the Shuttle's rail
	// corridor and the Staircase's steps are procedural, and both types
	// wore the plain Lift and Stairwell shafts underneath them. A repoint
	// keyed on whether the installed atlas carries the name - which is what
	// #1017 shipped - reads those dormant regions as own art, and every
	// World on `SectorAtlas` repaints: the Shuttle's rail corridor drops
	// from the Lift's 32x117 shaft to a 31x46 fragment, and a second fixed
	// staircase appears underneath the procedural steps.
	//
	// The check runs on the real old sheet with both regions present, and
	// asserts the presence test passes before it asserts the borrow. That
	// order is the point: if a future change makes these two types ask
	// presence again, this fails while the sheet still declares both names,
	// which is the case the #1017 suite never asked about.
	void theOldFamilyStillBorrowsForShuttleAndStaircase(
		smoke::Context const& context)
	{
		using smoke::require;
		// The old atlas as the application holds it, installed through the
		// same pairing the render stack uses.
		auto const old = installOldSectorAtlas(context);
		// The two dormant regions, present in the atlas the renderer
		// actually queries. If either ever leaves the sheet this check stops
		// pinning anything, and says so rather than passing on an empty
		// premise.
		require(old.surfaces.count("shuttle") && old.surfaces.count("staircase"),
			"The installed old-family atlas no longer carries the 'shuttle' and "
			"'staircase' regions this check pins the borrowing against");
		// Presence says yes to both, so presence is not what keeps the
		// borrowing here - the family is.
		require(hasSectorTileSurface("shuttle") && hasSectorTileSurface("staircase"),
			"The installed old-family atlas does not answer for the two regions it "
			"declares, so the borrow below is not being tested against a "
			"presence-based repoint");
		std::map<std::string, std::string> const stillBorrowed{
			{ "shuttle", "lift" }, { "staircase", "stairwell" } };
		for (auto const& [surface, borrowed] : stillBorrowed)
		{
			auto const fixture = fixtureFor(surface);
			require(fixture.sector != nullptr,
				"No Sector was built for the '" + surface + "' surface");
			auto const tiles = surfaceTiles(fixture.sector,
				fixture.sector->getLayerIndex(), old);
			require(std::find(tiles.begin(), tiles.end(), surface) == tiles.end(),
				"An Old-family atlas put the " + surface + " Sector on its own "
					"declared-but-never-drawn tile instead of borrowing the "
					+ borrowed + "'s surface, which is the #1027 repaint");
			require(tiles.size() == 1 && tiles.front() == borrowed,
				"An Old-family atlas left the " + surface + " Sector on "
					+ joined(tiles) + "; it must fill with the " + borrowed
					+ "'s surface, exactly as it did before #1017");
		}
	}
}

void render_smoke::registerSectorAtlasNew(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "sectorAtlasNew/tenFullCellTilesAreRegistered",
		isolated<tenFullCellTilesAreRegistered> });
	checks.push_back({ "sectorAtlasNew/everyEdgeFallsBackToThePrimitiveLine",
		isolated<everyEdgeFallsBackToThePrimitiveLine> });
	checks.push_back({ "sectorAtlasNew/everySurfaceBearingSectorTypeResolvesToItsOwnTile",
		isolated<everySurfaceBearingSectorTypeResolvesToItsOwnTile> });
	checks.push_back({ "sectorAtlasNew/aShortSectorCropsTheTopOfItsTileOntoItsFloorLine",
		isolated<aShortSectorCropsTheTopOfItsTileOntoItsFloorLine> });
	checks.push_back({ "sectorAtlasNew/shuttleAndStaircaseNoLongerBorrow",
		isolated<shuttleAndStaircaseNoLongerBorrow> });
	checks.push_back({ "sectorAtlasNew/carriageAndStepsStillDrawOnTop",
		isolated<carriageAndStepsStillDrawOnTop> });
	checks.push_back({ "sectorAtlasNew/flatColourSectorsTakeNoTile",
		isolated<flatColourSectorsTakeNoTile> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesASetMissingASurface",
		isolated<loaderRefusesASetMissingASurface> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesABoundaryStripInTheNewSet",
		isolated<loaderRefusesABoundaryStripInTheNewSet> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesASurfaceThatIsNotOneFullCell",
		isolated<loaderRefusesASurfaceThatIsNotOneFullCell> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesASurfaceOffsetFromItsCell",
		isolated<loaderRefusesASurfaceOffsetFromItsCell> });
	checks.push_back({ "sectorAtlasNew/loaderAcceptsAConformingSet",
		isolated<loaderAcceptsAConformingSet> });
	checks.push_back({ "sectorAtlasNew/anOlderAtlasStillBorrowsForATypeItHasNoTileFor",
		isolated<anOlderAtlasStillBorrowsForATypeItHasNoTileFor> });
	checks.push_back({ "sectorAtlasNew/theOldFamilyStillBorrowsForShuttleAndStaircase",
		isolated<theOldFamilyStillBorrowsForShuttleAndStaircase> });
}
