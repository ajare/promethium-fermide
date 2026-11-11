// SectorAtlas fills every surface-bearing Sector type with its own
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
		auto const set = manifestResource(manifest, "SectorAtlas");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no SectorAtlas ImageSet");
		require(atlasSetTilesets(context)["SectorAtlas"]
			== "textures/sectors-new.tileset.yaml",
			"The atlas-set registry does not name sectors-new.tileset.yaml for "
			"SectorAtlas");
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
		// The declared variant counts travel with it for the same reason the
		// family does: the installed set is what a caller asks its counts
		// of, and building it here without them would leave that query
		// running on something the stack never installs (#1038).
		installed.variantCounts = authored.variantCounts;
		installed.tiles = installAtlasTiles(authored.tiles, set.regions,
			authored.width, authored.height, "SectorAtlas");
		for (auto const& [name, artwork] : installed.tiles)
			installed.surfaces.emplace(name, artwork.region);
		setSectorTileset(installed, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The installed new Sector atlas did not come up as the New family");
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
		// Ten base surfaces, each declaring a family of three, so the set
		// holds forty tiles and not the ten it carried before families were
		// derived.
		require(authored.surfaces.size() == kSurfaces.size() * 4,
			"SectorAtlas authors " + std::to_string(authored.surfaces.size())
			+ " surfaces; ten bases with three derived variants under each is forty");
		for (auto const& name : kSurfaces)
			require(authored.surfaces.count(name),
				"SectorAtlas registers no '" + name + "' surface");
		// The ten bases form one row across the top of the sheet. That is
		// what leaves every column below a base free to hold that base's
		// family, and why the sheet grew down rather than sideways.
		require(authored.width == 1280 && authored.height == 1280,
			"SectorAtlas is not the 1280x1280 sheet its set is sized for");
		for (auto const& name : kSurfaces)
			require(authored.surfaces.at(name).y == 0,
				"'" + name + "' is not in the base strip along the top of the sheet");
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
			"SectorAtlas carries boundary strips; the new set draws none");
		// The manifest's generated entries agree with the authored tileset,
		// so the runtime pixel contract cannot drift from what was authored.
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "SectorAtlas");
		require(set.found && set.type == "ImageSet",
			"SectorAtlas is not registered as an ImageSet");
		require(set.imageReference == "SectorAtlasImage",
			"SectorAtlas's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "SectorAtlasImage");
		require(image.found && image.type == "Image"
			&& image.location == "textures/sectors-new.png",
			"SectorAtlasImage is not the atlas image the tileset names");
		require(image.options.count("filtering")
				&& image.options.at("filtering") == "none"
				&& image.options.count("uv-style")
				&& image.options.at("uv-style") == "atlas",
			"SectorAtlasImage must be an unfiltered atlas image, as every "
			"viewport atlas is");
		require(set.regions.size() == authored.tiles.size(),
			"The manifest's SectorAtlas entries and the authored tileset "
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
		require(atlasSetTilesets(context)["SectorAtlas"]
			== "textures/sectors-new.tileset.yaml",
			"The atlas-set registry names no sector tileset for SectorAtlas");
	}

	// ------------------------------------------------------------------
	// No boundary regions: every edge takes the primitive line.
	// ------------------------------------------------------------------
	void everyEdgeFallsBackToThePrimitiveLine(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		require(atlas.boundaries.empty(),
			"The installed SectorAtlas carries boundary regions");
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

	// The full ten-surface set laid out across one row of a sheet deep
	// enough for each base's family to descend beneath it. The variant
	// count, the sheet depth and the row the bases sit in are the only
	// things a caller controls, so a refusal about one is never confounded
	// by another.
	std::string familyTilesetYaml(int variants, int sheetRows, int baseRow = 0)
	{
		std::ostringstream yaml;
		yaml << "version: 2\n"
			<< "image: invented.png\n"
			<< "size: [1280, " << (sheetRows * 320) << "]\n"
			<< "tile_size: [128, 320]\n"
			<< "sprites:\n";
		int column = 0;
		for (auto const& name : kSurfaces)
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column++ << ", " << baseRow << "]\n"
				<< "    variants: " << variants << "\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: [0.0, 0.0]\n"
				<< "    drawn_size: [1.0, 1.0]\n"
				<< "    tintable: true\n";
		}
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

	// The per-type count is what a caller sizes a swatch strip from, so it
	// answers from what the base sprite declared rather than from a constant
	// the caller would have to keep in step with the tileset.
	void variantFamiliesAreDerivedAndReported(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		for (auto const& name : kSurfaces)
			require(atlas.variantCount(name) == 3,
				"'" + name + "' reports no family of three on the installed atlas");
		// A derived tile is not the base of a family, so it holds no count
		// of its own; answering 3 there would invite a family under a family.
		require(atlas.variantCount("corridor-V1") == 0,
			"A derived tile reported a family of its own");
		require(atlas.variantCount("nope") == 0,
			"An unknown kind reported a family");
		// The same number reaches a caller that never held the set.
		require(sectorSurfaceVariantCount("corridor") == 3,
			"The installed-atlas query does not report the declared count");
		require(sectorSurfaceVariantCount("corridor-V1") == 0,
			"The installed-atlas query reports a count for a derived tile");
	}

	// A family that runs past the bottom of the declared sheet describes a
	// tile no cell exists for. The sheet cannot show what was asked, and a
	// sheet that quietly stopped short of the count is worse than a load
	// that says so: the author would ship believing there were four.
	void loaderRefusesAFamilyThatRunsPastTheSheet(smoke::Context const& context)
	{
		using smoke::require;
		// Four sheet rows, numbered 0 to 3. A base in the last row that
		// declares one variant wants a row 4 the sheet does not have.
		auto const refusal = loadRefusal(context, "family-past-the-sheet",
			familyTilesetYaml(1, 4, 3));
		require(refusal.find("run past the bottom") != std::string::npos,
			"A family descending past the last declared row was accepted: "
			+ refusal);
	}

	// The same set with room to stand loads, and every derived tile lands
	// where the rule puts it: its base's column, one authored tile height
	// per step down, with the base's content, offset, drawn size and
	// tintable flag carried through untouched.
	void loaderDerivesEachVariantOfAFamilyThatFits(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "family-fits.tileset.yaml";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << familyTilesetYaml(3, 4);
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		auto const loaded = SectorTileset::load(path);
		require(loaded.surfaces.size() == kSurfaces.size() * 4,
			"A conforming family set did not load forty tiles");
		for (auto const& name : kSurfaces)
		{
			auto const& base = loaded.tiles.at(name);
			require(loaded.variantCount(name) == 3,
				"'" + name + "' reports no count after a load that declared three");
			for (int step = 1; step <= 3; ++step)
			{
				auto const variant = name + "-V" + std::to_string(step);
				require(loaded.tiles.count(variant), variant + " was not derived");
				auto const& derived = loaded.tiles.at(variant);
				require(derived.region.x == base.region.x,
					variant + " left its base's column");
				require(derived.region.y == base.region.y + step * 320,
					variant + " is not one authored tile height per step below its base");
				require(derived.region.width == base.region.width
						&& derived.region.height == base.region.height,
					variant + " cuts different content than its base cuts");
				require(derived.offset.x == base.offset.x
						&& derived.offset.y == base.offset.y
						&& derived.drawnSize.x == base.drawnSize.x
						&& derived.drawnSize.y == base.drawnSize.y,
					variant + " did not inherit its base's render semantics");
				require(derived.tintable == base.tintable,
					variant + " did not inherit its base's tintable flag");
			}
		}
	}

	// A hand-authored name inside a derived family's own space is the one
	// way the derivation could be contradicted. It is refused by name
	// rather than shadowed by whichever of the two a map happened to keep.
	void loaderRefusesAVariantNameAuthoredBesideItsFamily(
		smoke::Context const& context)
	{
		using smoke::require;
		auto yaml = familyTilesetYaml(1, 2);
		yaml += "  corridor-V1:\n"
			"    cell: [0, 1]\n"
			"    content: [0, 0, 128, 320]\n"
			"    offset: [0.0, 0.0]\n"
			"    drawn_size: [1.0, 1.0]\n"
			"    tintable: true\n";
		auto const refusal = loadRefusal(context, "authored-beside-family", yaml);
		require(refusal.find("corridor-V1") != std::string::npos
				&& refusal.find("reserves") != std::string::npos,
			"A hand-authored name inside a derived family was accepted: " + refusal);
	}

	// `variants` means "a family descends from this sprite". Written on the
	// file it would read as a set-wide count with no base to descend from,
	// so it is refused rather than given a meaning nobody wrote.
	void loaderRefusesVariantsWrittenOnTheFile(smoke::Context const& context)
	{
		using smoke::require;
		auto yaml = familyTilesetYaml(0, 1);
		yaml.insert(yaml.find("sprites:"), "variants: 3\n");
		auto const refusal = loadRefusal(context, "variants-on-the-file", yaml);
		require(refusal.find("declared on a base sprite") != std::string::npos,
			"A set-wide `variants` key was accepted: " + refusal);
	}

	// The version 1 format's surfaces are bare rectangles with no cell to
	// descend from, so a set-wide `variants` key there is a family nothing
	// could ever derive.
	void loaderRefusesVariantFamiliesInTheOldFormat(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "v1-variants",
			"version: 1\nimage: invented.png\nsize: [1024, 1024]\n"
			"regions:\n  room: [0, 0, 128, 128]\nvariants: 3\n");
		require(refusal.find("no variant families") != std::string::npos,
			"A version 1 sector tileset declaring variant families was accepted: "
			+ refusal);
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

}

void render_smoke::registerSectorAtlas(std::vector<smoke::Check>& checks)
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
	checks.push_back({ "sectorAtlasNew/variantFamiliesAreDerivedAndReported",
		isolated<variantFamiliesAreDerivedAndReported> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesAFamilyThatRunsPastTheSheet",
		isolated<loaderRefusesAFamilyThatRunsPastTheSheet> });
	checks.push_back({ "sectorAtlasNew/loaderDerivesEachVariantOfAFamilyThatFits",
		isolated<loaderDerivesEachVariantOfAFamilyThatFits> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesAVariantNameAuthoredBesideItsFamily",
		isolated<loaderRefusesAVariantNameAuthoredBesideItsFamily> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesVariantsWrittenOnTheFile",
		isolated<loaderRefusesVariantsWrittenOnTheFile> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesVariantFamiliesInTheOldFormat",
		isolated<loaderRefusesVariantFamiliesInTheOldFormat> });
	checks.push_back({ "sectorAtlasNew/anOlderAtlasStillBorrowsForATypeItHasNoTileFor",
		isolated<anOlderAtlasStillBorrowsForATypeItHasNoTileFor> });
}
