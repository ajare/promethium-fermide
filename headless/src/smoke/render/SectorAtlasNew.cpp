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
#include <filesystem>
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

	// The ten surface-bearing Sector types that carry a variant family, by
	// the tile name each one asks the atlas for.
	std::vector<std::string> const kSurfaces{ "room", "corridor", "ladder",
		"lift", "shuttle", "stairwell", "staircase", "airlock", "chamber",
		"dumbwaiter" };

	// The two flat surfaces. They own a tile each on the same terms as
	// every wall does, and declare no variant family: one picture apiece,
	// and no second one for an author to pick.
	std::vector<std::string> const kFlatSurfaces{ "background", "facade" };

	// Every surface-bearing base a conforming set must carry: the ten that
	// carry variant families plus the two flat surfaces that do not.
	std::vector<std::string> allSurfaceBases()
	{
		auto bases = kSurfaces;
		bases.insert(bases.end(), kFlatSurfaces.begin(), kFlatSurfaces.end());
		return bases;
	}

	// The doorframes the set authors (#1054, #1073). Keyed by the opening
	// and nothing else: the width and height of the doorway, then the
	// cell's role in it - `L0` is the opening's left cell and `R0` its
	// right. A 1-wide opening has only a left cell.
	//
	// No wall is in any of these names. A frame lies over the wall's own
	// tile rather than standing in for it, so the wall is not a fact about
	// the frame; the fifteen wall-keyed tiles this replaced were three
	// copies of six tiles drawn to differ in a way nothing could see.
	std::vector<std::string> const kDoorframes{
		"doorframe-1r-L0", "doorframe-2r-L0", "doorframe-2r-R0",
		"doorframe-1t-L0", "doorframe-2t-L0", "doorframe-2t-R0" };

	// The windowframes the set authors (#1075, wide row #1076, tall column
	// #1077, corners and interior edges #1078). Keyed by where the cell
	// sits in the opening it frames - the vertical role first, so `BL`
	// reads as "bottom-left" the way it is said - and by nothing else: not
	// the glass style, not the Window's State, not the wall.
	//
	// The block is filled right out to its fifteen tiles. `windowframe-SS`
	// is the single-column, single-level cell, which is the 1x1 Window that
	// is eight of the ten windows every bundled document authors;
	// `SL`/`SM`/`SR` are the wide single-level opening, a jamb at each end
	// and no vertical moulding in the middle; `BS`/`MS`/`TS` are the tall
	// single-column opening, a sill under the bottom cell, jambs only down
	// the middle and a head over the top; and the wide columns of the
	// three tall rows are the openings that are tall *and* wide, where a
	// corner carries its jamb and its head or sill together
	// (`TL`/`TR`/`BL`/`BR`) and an interior edge carries one moulding
	// across a whole cell (`TM` head, `BM` sill, `ML`/`MR` jamb).
	//
	// Fifteen is the whole vocabulary, not a milestone. Sixteen cells in
	// the role grid less the all-middle one, which asks for no tile at all:
	// the middle of a span that is middle in both directions is all glass
	// with no moulding on it. Every cell of every span is one of these
	// fifteen positions, so the same tiles frame a 1x1 window and a 7x4
	// one and nothing further is ever needed for a bigger opening.
	std::vector<std::string> const kWindowframes{ "windowframe-SS",
		"windowframe-SL", "windowframe-SM", "windowframe-SR",
		"windowframe-TS", "windowframe-MS", "windowframe-BS",
		"windowframe-TL", "windowframe-TM", "windowframe-TR",
		"windowframe-ML", "windowframe-MR",
		"windowframe-BL", "windowframe-BM", "windowframe-BR" };

	// The sheet cell each frame holds, which is the pair of roles read off
	// the block rather than decoded from a name: columns 15..18 are the
	// horizontal role `S L M R`, rows 0..3 are the vertical role
	// `S T M B` from the top of the sheet down, so the bottom-of-window
	// tiles carry the highest y the way they sit lowest in the window.
	struct BlockCell
	{
		uint32_t column{};
		uint32_t row{};
	};

	std::map<std::string, BlockCell> const kWindowframeCells{
		{ "windowframe-SS", { 15, 0 } },
		{ "windowframe-SL", { 16, 0 } },
		{ "windowframe-SM", { 17, 0 } },
		{ "windowframe-SR", { 18, 0 } },
		{ "windowframe-TS", { 15, 1 } },
		{ "windowframe-TL", { 16, 1 } },
		{ "windowframe-TM", { 17, 1 } },
		{ "windowframe-TR", { 18, 1 } },
		{ "windowframe-MS", { 15, 2 } },
		{ "windowframe-ML", { 16, 2 } },
		{ "windowframe-MR", { 18, 2 } },
		{ "windowframe-BS", { 15, 3 } },
		{ "windowframe-BL", { 16, 3 } },
		{ "windowframe-BM", { 17, 3 } },
		{ "windowframe-BR", { 18, 3 } } };

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
		{
			// A doorframe is a tile of this atlas but not a Sector surface,
			// and the install the application runs routes it apart on the
			// strength of what the authored file said. Copying every tile
			// into `surfaces` here would install something the stack does
			// not, and would make the surface query answer yes for a frame
			// (#1054).
			if (authored.overlays.count(name) > 0)
				installed.overlays.emplace(name, artwork.region);
			else installed.surfaces.emplace(name, artwork.region);
		}
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
		// Ten bases each declaring a family of three, plus the two flat
		// surfaces standing alone with no family of their own.
		require(authored.surfaces.size()
				== kSurfaces.size() * 4 + kFlatSurfaces.size(),
			"SectorAtlas authors " + std::to_string(authored.surfaces.size())
			+ " surfaces; ten bases with three derived variants under each "
			"plus two flat surfaces is forty-two");
		for (auto const& name : allSurfaceBases())
			require(authored.surfaces.count(name),
				"SectorAtlas registers no '" + name + "' surface");
		// The twelve bases form one row across the top of the sheet. That is
		// what leaves every column below a base free to hold that base's
		// family, and why the sheet grew down rather than sideways. The
		// doorframe block stands to the right of the twelve surface columns
		// and the windowframe block to the right of that, so the sheet is
		// nineteen columns wide, and four rows deep because each base
		// carries three variant rows and neither frame needs a row per
		// height class or wall (#1054, #1059, #1075).
		require(authored.width == 2432 && authored.height == 1280,
			"SectorAtlas is not the 2432x1280 sheet its set is sized for");
		for (auto const& name : allSurfaceBases())
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
		// The doorframes are in the set, and in the overlays map rather than
		// the surfaces map: a frame is a tile of this atlas and not a surface
		// a Sector is authored with (#1054).
		require(authored.overlays.size()
				== kDoorframes.size() + kWindowframes.size(),
			"SectorAtlas authors " + std::to_string(authored.overlays.size())
			+ " overlays; the doorframe block is six, one per opening width, "
			"height class and role, and the windowframe block carries the "
			"whole role grid on top of them - fifteen tiles, sixteen cells "
			"less the all-middle one that asks for no tile");
		for (auto const& name : kDoorframes)
		{
			require(authored.overlays.count(name),
				"SectorAtlas authors no doorframe '" + name + "'");
			require(authored.surfaces.count(name) == 0,
				"The doorframe '" + name + "' is authored as a Sector surface");
			require(authored.tiles.count(name),
				"The doorframe '" + name
				+ "' is not in the tile map the install pairs against the "
				"manifest");
		}
		// The windowframes are overlays on the same terms, and the block's
		// place on the sheet is the role grid: the four columns from 15 are
		// the horizontal role `S L M R` and the four rows from 0 are the
		// vertical role `S T M B`, so each frame sits where its pair of
		// roles says and an artist reads the grid off the sheet. The
		// bottom-of-window tiles carry the highest y, the way they sit
		// lowest in the window.
		for (auto const& name : kWindowframes)
		{
			require(authored.overlays.count(name),
				"SectorAtlas authors no windowframe '" + name + "'");
			require(authored.surfaces.count(name) == 0,
				"The windowframe '" + name + "' is authored as a Sector surface");
			require(authored.tiles.count(name),
				"The windowframe '" + name
				+ "' is not in the tile map the install pairs against the "
				"manifest");
			auto const want = kWindowframeCells.at(name);
			require(authored.overlays.at(name).x == static_cast<int>(want.column) * 128
					&& authored.overlays.at(name).y == static_cast<int>(want.row) * 320,
				name + " is not in the windowframe block's cell for its role: "
				"the block's columns are 15..18 for S/L/M/R and its rows are "
				"0..3 for S/T/M/B from the top of the sheet down, so it "
				"belongs at column " + std::to_string(want.column) + ", row "
					+ std::to_string(want.row));
		}
		// No wall-keyed frame survives the change. A frame that carried its
		// wall's appearance was what the frame *replacing* the wall's tile
		// needed; lying over the tile instead, it has nothing left to say
		// about the wall, and an authored name the render path never asks
		// for reads as unfinished art.
		for (auto const* wall : { "room", "corridor", "facade" })
			for (auto const& [name, artwork] : authored.overlays)
				require(name.find(std::string("doorframe-") + wall + "-")
						== std::string::npos,
					"SectorAtlas authors the wall-keyed doorframe '" + name
						+ "'; the frame is keyed by its opening and not by its "
						"wall");
		// The same refusal for the windowframe, so the wall cannot come
		// back under the new name either.
		for (auto const* wall : { "room", "corridor", "facade" })
			for (auto const& [name, artwork] : authored.overlays)
				require(name.find(std::string("windowframe-") + wall + "-")
						== std::string::npos,
					"SectorAtlas authors the wall-keyed windowframe '" + name
						+ "'; a frame laid over the wall's own tile does not "
						"vary with the wall");
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
	// Background and Facade: each draws its own tile, as raw artwork.
	// ------------------------------------------------------------------
	void flatSurfaceSectorsDrawTheirOwnTiles(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);

		require(atlas.tiles.count("background") == 1,
			"The installed atlas carries no `background` tile");
		require(atlas.tiles.count("facade") == 1,
			"The installed atlas carries no `facade` tile");

		auto world = std::make_shared<core::World>("Flat surface sectors", 8, 2);
		auto const backgroundIndex = world->addBackground(0, 0, 0, 2, 1);
		auto const facadeIndex = world->addFacade("Facade", 0, 0, 4, 2, 1);
		world->finishBuild();

		std::vector<std::pair<core::SectorType, std::string>> const probes{
			{ core::SectorType::Background, "background" },
			{ core::SectorType::Facade, "facade" } };
		std::vector<uint32_t> const indices{ backgroundIndex, facadeIndex };

		for (std::size_t i = 0; i < probes.size(); ++i)
		{
			auto const sector = world->getSector(indices[i]);
			require(sector != nullptr && sector->getType() == probes[i].first,
				"The fixture Sector is not the flat-surface type under test");
			WorldDrawList commands({ { -100000.0f, -100000.0f },
				{ 100000.0f, 100000.0f } });
			renderSector(sector, 0, LayerRenderStyle::Solid, false, White,
				&commands);
			int onItsOwnTile = 0;
			for (auto const& command : commands.commands())
			{
				auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
					continue;
				++onItsOwnTile;
				require(triangle->colour == White,
					"A flat-surface tile was drawn with a colour other than its raw "
					"artwork");
			}
			require(onItsOwnTile > 0,
				"The " + probes[i].second + " Sector drew no tile of its own");
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
			<< "size: [640, 960]\n"
			<< "tile_size: [128, 320]\n"
			<< "sprites:\n";
		int column = 0, row = 0;
		for (auto const& name : surfaces)
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column << ", " << row << "]\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: " << offset << "\n"
				<< "    drawn_size: " << drawnSize << "\n";
			if (++column == 5) { column = 0; ++row; }
		}
		if (withBoundaries)
			yaml << "boundaries:\n  floor: [0, 0, 128, 32]\n";
		return yaml.str();
	}

	// The full twelve-surface set with no `overlays` key at all - the
	// shape every version 2 file written before the doorframe existed has,
	// and one of the three spellings of "no overlays" #1066 is about.
	std::string tenSurfaceTilesetYaml()
	{
		std::ostringstream yaml;
		yaml << "version: 2\n"
			<< "image: invented.png\n"
			<< "size: [1536, 640]\n"
			<< "tile_size: [128, 320]\n"
			<< "sprites:\n";
		int column = 0;
		for (auto const& name : allSurfaceBases())
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column++ << ", 0]\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: [0.0, 0.0]\n"
				<< "    drawn_size: [1.0, 1.0]\n";
		}
		return yaml.str();
	}

	// The full ten-surface set plus an `overlays` block holding one named
	// tile, so a refusal about an overlay is confounded by nothing else.
	// `extraKeys` is written into the overlay sprite itself, which is where
	// a `variants` key would go if an author put one there.
	std::string overlayTilesetYaml(std::string const& overlayName,
		std::string const& extraKeys = {})
	{
		return tenSurfaceTilesetYaml()
			+ "overlays:\n  " + overlayName + ":\n"
			  "    cell: [0, 1]\n"
			  "    content: [0, 0, 128, 320]\n"
			  "    offset: [0.0, 0.0]\n"
			  "    drawn_size: [1.0, 1.0]\n"
			+ extraKeys;
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
			<< "size: [1536, " << (sheetRows * 320) << "]\n"
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
				<< "    drawn_size: [1.0, 1.0]\n";
		}
		// The flat surfaces ride along so the set conforms; they declare no
		// family, so a refusal about the ten is never confounded by them.
		for (auto const& name : kFlatSurfaces)
		{
			yaml << "  " << name << ":\n"
				<< "    cell: [" << column++ << ", " << baseRow << "]\n"
				<< "    content: [0, 0, 128, 320]\n"
				<< "    offset: [0.0, 0.0]\n"
				<< "    drawn_size: [1.0, 1.0]\n";
		}
		return yaml.str();
	}

	// Writes a generated tileset and hands back its path, so every loader
	// seam below drives a real file rather than a node built in memory.
	std::filesystem::path writeTileset(smoke::Context const& context,
		std::string const& tag, std::string const& yaml)
	{
		auto const path = context.temporaryRoot() / (tag + ".tileset.yaml");
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << yaml;
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		return path;
	}

	// Loads a generated tileset and hands back the refusal, or empty when
	// the file was accepted.
	std::string loadRefusal(smoke::Context const& context, std::string const& tag,
		std::string const& yaml)
	{
		auto const path = writeTileset(context, tag, yaml);
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

	// Every one of the twelve surface-bearing types has to be present. A
	// set missing one would leave that Sector with no surface to draw.
	void loaderRefusesASetMissingASurface(smoke::Context const& context)
	{
		using smoke::require;
		auto missing = allSurfaceBases();
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
			tilesetYaml(allSurfaceBases(), "[1.0, 1.0]", "[0.0, 0.0]", true));
		require(!refusal.empty(),
			"A version 2 sector tileset declaring boundary strips was accepted");
		require(refusal.find("boundary") != std::string::npos
			&& refusal.find("primitive line") != std::string::npos,
			"The refusal must say the new set draws no boundary strips and the "
			"edge takes the primitive line: " + refusal);
	}

	// An overlay is a tile of the atlas that is not a Sector surface. A
	// name authored in both maps leaves the draw to settle which kind it is
	// by lookup order, and a frame drawing as a wall - or the other way
	// round - is not a thing two maps should be left to decide (#1054).
	void loaderRefusesAnOverlayThatIsAlsoASurface(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "overlay-is-a-surface",
			overlayTilesetYaml("room"));
		require(!refusal.empty(),
			"An overlay sharing a name with a Sector surface was accepted");
		require(refusal.find("Sector surface") != std::string::npos,
			"The refusal did not say the name is already a Sector surface: "
			+ refusal);
	}

	// A frame has no variant family. A surface's variants are different
	// pictures of the same thing an author picks from a swatch strip; a
	// frame is not picked at all, it is derived from the wall the Door
	// stands in, so a family of frames is a choice nobody can make. The
	// key is refused rather than ignored, which is the same reason the
	// version 1 formats refuse it.
	void loaderRefusesVariantsOnAnOverlay(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "overlay-declares-variants",
			overlayTilesetYaml("doorframe-room-1r-L0", "    variants: 2\n"));
		require(!refusal.empty(),
			"An overlay declaring a variant family was accepted");
		require(refusal.find("variants") != std::string::npos,
			"The refusal did not name the `variants` key: " + refusal);
	}

	// The `-Vn` shape belongs to the derivation, and the derivation exists
	// in this file whether or not the map being read takes part in it. The
	// overlays derive nothing, and the flag that turned the family column
	// off turned the name reservation off with it, so an overlay authored as
	// `doorframe-room-1r-L0-V1` loaded clean: a family nothing made, with
	// nothing in the file saying so. The reservation is asked separately now,
	// so the shape is refused in every map of a file that derives anywhere
	// (#1069).
	//
	// The fixture's ten surfaces declare no family at all, so nothing in
	// these files derives a single `-Vn` tile: every refusal below is about
	// the shape the name has, not about a tile it collided with. That is the
	// half the both-maps collision could never reach - a `-Vn` no current
	// family derives is still the shape the next family's declaration will.
	void loaderRefusesAnOverlayNamedLikeADerivedVariant(
		smoke::Context const& context)
	{
		using smoke::require;
		for (std::string const name : { "doorframe-room-1r-L0-V1",
				"doorframe-corridor-2t-R0-V12", "room-V1", "frame-V0" })
		{
			auto const refusal = loadRefusal(context, "overlay-" + name,
				overlayTilesetYaml(name));
			require(refusal.find(name) != std::string::npos
					&& refusal.find("reserves") != std::string::npos,
				"An overlay named '" + name + "' was accepted, though `-Vn` is "
				"the derivation's shape: " + refusal);
			require(refusal.find("sector overlay") != std::string::npos,
				"The refusal of '" + name + "' does not say which kind of tile it "
				"refused: " + refusal);
		}
		// A name merely near the shape is nobody's. The reservation is `-V`
		// followed by digits and nothing else, and widening it would refuse
		// frames that have always loaded.
		for (std::string const name : { "doorframe-room-1r-L0", "doorframe-VX",
				"doorframe-room-1r-L0-V1x", "doorframe--V" })
		{
			require(loadRefusal(context, "overlay-" + name,
					overlayTilesetYaml(name)).empty(),
				"An overlay named '" + name + "' was refused though it does not "
				"have the reserved shape");
		}
	}

	// The install routes a `boundary-` prefix to the boundary map before
	// it looks at anything else, so an overlay wearing that prefix would
	// arrive as edge artwork nobody asked for.
	void loaderRefusesAnOverlayWearingTheBoundaryPrefix(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "overlay-boundary-prefix",
			overlayTilesetYaml("boundary-floor"));
		require(!refusal.empty(),
			"An overlay wearing the `boundary-` prefix was accepted");
		require(refusal.find("boundary-") != std::string::npos,
			"The refusal did not name the prefix it refused: " + refusal);
	}

	// A set that authors overlays reads apart: the frames land in the
	// overlay map, the surfaces keep their ten, and the tile map the
	// install pairs against the manifest holds both kinds.
	void loaderReadsOverlaysApartFromSurfaces(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "overlays-apart.tileset.yaml";
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out << overlayTilesetYaml("doorframe-room-1r-L0");
		}
		auto const set = SectorTileset::load(path);
		require(set.surfaces.size() == allSurfaceBases().size(),
			"The overlay set carries " + std::to_string(set.surfaces.size())
			+ " surfaces; the twelve bases were expected");
		require(set.overlays.size() == 1,
			"The overlay set carries " + std::to_string(set.overlays.size())
			+ " overlays; one was authored");
		require(set.overlays.count("doorframe-room-1r-L0") == 1,
			"The authored overlay is not in the overlay map");
		require(set.tiles.size() == allSurfaceBases().size() + 1,
			"The tile map the install pairs against the manifest holds "
			+ std::to_string(set.tiles.size()) + " tiles instead of the twelve "
			"surfaces plus the overlay");
		// And the shipped set still loads with its doorframe block intact.
		auto const without = SectorTileset::load(
			context.fixture(kTileset));
		require(!without.overlays.empty(),
			"The shipped set lost its doorframe block");
	}

	// The three spellings of "no overlays" all read as no overlays: the key
	// absent, the key written with no body, and the key written as an empty
	// map. In yaml-cpp the second is a *defined* Null rather than an
	// undefined node, so guarding the second read on `IsDefined()` sent it
	// into the grid's own map check and failed a file the generator had
	// just declared clean - with a message saying the definitions were
	// missing when the author had written the key (#1066).
	//
	// The generator's half of this table is driven by
	// `scripts/tests/overlay_shapes.py`, which runs the real `--check`
	// against the same shapes. The two lists together are the agreement: a
	// shape added to either reader belongs in both.
	void loaderReadsEveryEmptyShapeOfOverlaysAsNoOverlays(
		smoke::Context const& context)
	{
		using smoke::require;
		struct Spelling { std::string tag; std::string clause; };
		for (auto const& shape : { Spelling{ "overlays-absent", "" },
				Spelling{ "overlays-null", "overlays:\n" },
				Spelling{ "overlays-empty-map", "overlays: {}\n" } })
		{
			SectorTileset set{};
			std::string refusal;
			try
			{
				set = SectorTileset::load(writeTileset(context, shape.tag,
					tenSurfaceTilesetYaml() + shape.clause));
			}
			catch (std::exception const& error) { refusal = error.what(); }
			require(refusal.empty(), "A set whose `overlays` is " + shape.tag
				+ " was refused: " + refusal);
			require(set.surfaces.size() == allSurfaceBases().size(),
				"A set whose `overlays` is " + shape.tag + " carries "
				+ std::to_string(set.surfaces.size()) + " surfaces; the twelve "
				"bases were expected");
			require(set.overlays.empty(),
				"A set whose `overlays` is " + shape.tag + " gained "
				+ std::to_string(set.overlays.size()) + " overlays; the shape "
				"authors none");
			require(set.tiles.size() == allSurfaceBases().size(),
				"A set whose `overlays` is " + shape.tag + " carries "
				+ std::to_string(set.tiles.size()) + " tiles; the twelve "
				"surfaces and nothing else were expected");
		}
	}

	// An `overlays` that is not a map is not a set of tile definitions,
	// and both readers refuse it: the loader here, the generator in
	// `overlay_sprites`. The refusal says the key is the wrong shape rather
	// than that it is missing, because the author did write it (#1066).
	void loaderRefusesAnOverlaysKeyThatIsNotAMap(smoke::Context const& context)
	{
		using smoke::require;
		struct Spelling { std::string tag; std::string clause; };
		for (auto const& shape : { Spelling{ "overlays-scalar",
						  "overlays: 3\n" },
				Spelling{ "overlays-empty-string", "overlays: \"\"\n" },
				Spelling{ "overlays-sequence",
						  "overlays: [doorframe-room-1r-L0]\n" } })
		{
			auto const refusal = loadRefusal(context, shape.tag,
				tenSurfaceTilesetYaml() + shape.clause);
			require(!refusal.empty(),
				"A set whose `overlays` is " + shape.tag + " was accepted");
			require(refusal.find("`overlays`") != std::string::npos
					&& refusal.find("is not a map") != std::string::npos,
				"The refusal for a " + shape.tag + " neither names the "
				"`overlays` key nor says it is not a map: " + refusal);
		}
	}

	// A populated `overlays` map is still read as populated: the empty and
	// null shapes above must not have made the loader treat the key as
	// decorative. One frame in, one frame out, on the same sheet.
	void loaderStillReadsAPopulatedOverlaysMap(smoke::Context const& context)
	{
		using smoke::require;
		auto const set = SectorTileset::load(writeTileset(context,
			"overlays-populated", overlayTilesetYaml("doorframe-room-1r-L0")));
		require(set.overlays.size() == 1
				&& set.overlays.count("doorframe-room-1r-L0") == 1,
			"A populated `overlays` map read back as "
			+ std::to_string(set.overlays.size()) + " overlays");
		require(set.tiles.size() == allSurfaceBases().size() + 1,
			"A populated `overlays` map left the tile map at "
			+ std::to_string(set.tiles.size()) + " tiles");
	}

	// The repeat is per World cell, so a surface drawn at any other size
	// repeats on a geometry the rule cannot express.
	void loaderRefusesASurfaceThatIsNotOneFullCell(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "drawn-two-cells",
			tilesetYaml(allSurfaceBases(), "[2.0, 1.0]"));
		require(refusal.find("one full World cell") != std::string::npos,
			"A surface drawn two cells wide was accepted: " + refusal);
	}

	// The same for a surface offset off its cell's anchor.
	void loaderRefusesASurfaceOffsetFromItsCell(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "offset-off-cell",
			tilesetYaml(allSurfaceBases(), "[1.0, 1.0]", "[0.5, 0.0]"));
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
		out << tilesetYaml(allSurfaceBases());
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		auto const loaded = SectorTileset::load(path);
		require(loaded.surfaces.size() == allSurfaceBases().size(),
			"A conforming set did not load all ten surfaces");
		require(loaded.boundaries.empty(),
			"A conforming version 2 set gained boundary regions");
		for (auto const& [name, artwork] : loaded.tiles)
		{
			require(near(artwork.drawnSize.x, 1.0f)
				&& near(artwork.drawnSize.y, 1.0f),
				"'" + name + "' lost its authored drawn size on load");
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
	// per step down, with the base's content, offset and drawn size
	// carried through untouched.
	void loaderDerivesEachVariantOfAFamilyThatFits(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "family-fits.tileset.yaml";
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out << familyTilesetYaml(3, 4);
		out.close();
		require(bool(out), "Could not write the fixture tileset: " + path.string());
		auto const loaded = SectorTileset::load(path);
		require(loaded.surfaces.size()
				== kSurfaces.size() * 4 + kFlatSurfaces.size(),
			"A conforming family set did not load forty-two tiles");
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
			"    drawn_size: [1.0, 1.0]\n";
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
	checks.push_back({ "sectorAtlasNew/flatSurfaceSectorsDrawTheirOwnTiles",
		isolated<flatSurfaceSectorsDrawTheirOwnTiles> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesASetMissingASurface",
		isolated<loaderRefusesASetMissingASurface> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesABoundaryStripInTheNewSet",
		isolated<loaderRefusesABoundaryStripInTheNewSet> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesAnOverlayThatIsAlsoASurface",
		isolated<loaderRefusesAnOverlayThatIsAlsoASurface> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesVariantsOnAnOverlay",
		isolated<loaderRefusesVariantsOnAnOverlay> });
	checks.push_back({
		"sectorAtlasNew/loaderRefusesAnOverlayNamedLikeADerivedVariant",
		isolated<loaderRefusesAnOverlayNamedLikeADerivedVariant> });
	checks.push_back({
		"sectorAtlasNew/loaderRefusesAnOverlayWearingTheBoundaryPrefix",
		isolated<loaderRefusesAnOverlayWearingTheBoundaryPrefix> });
	checks.push_back({ "sectorAtlasNew/loaderReadsOverlaysApartFromSurfaces",
		isolated<loaderReadsOverlaysApartFromSurfaces> });
	checks.push_back({
		"sectorAtlasNew/loaderReadsEveryEmptyShapeOfOverlaysAsNoOverlays",
		isolated<loaderReadsEveryEmptyShapeOfOverlaysAsNoOverlays> });
	checks.push_back({ "sectorAtlasNew/loaderRefusesAnOverlaysKeyThatIsNotAMap",
		isolated<loaderRefusesAnOverlaysKeyThatIsNotAMap> });
	checks.push_back({ "sectorAtlasNew/loaderStillReadsAPopulatedOverlaysMap",
		isolated<loaderStillReadsAPopulatedOverlaysMap> });
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
