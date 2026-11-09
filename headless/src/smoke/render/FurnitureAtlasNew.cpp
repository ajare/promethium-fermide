// FurnitureAtlasNew places and renders a multi-cell piece from role tiles
// (#1016).
//
// A sofa placed in a World renders from `sofa-L0`, `sofa-M0` and
// `sofa-R0`, and widening it to four cells reuses the same middle tile
// rather than needing a new one. The tile name carries the position the
// tile holds in the piece's footprint - `-Ly` / `-My` / `-Ry` is a role
// plus a height level, `-XY` is an absolute offset from the anchor - and
// the catalogue's per-tile `{x, y}` stays authoritative for placement.
// The loader asserts the two agree, which is what stops a self-describing
// art sheet becoming a lie.
//
// The artwork is installed the way the render stack installs it - pixel
// regions from the `ImageSet` the application manifest declares, render
// semantics from the authored tileset the atlas-set registry names for it -
// so these checks exercise the same pairing the application does.
// Everything is asserted on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names, and on the refusal a malformed catalogue
// gets at the loader seam.

#include "Checks.h"

#include "CatalogueSource.h"
#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Furniture.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	// The World cell's screen size at the zoom every check here uses.
	float const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	float const kLevelHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// The fixture World: one Room wide enough for a three-cell sofa and a
	// four-cell one side by side, one Level high, on Layer 0.
	uint32_t const kRoomCellsWide = 12;
	uint32_t const kRoomLevelsHigh = 1;
	float const kCanvasWidth = float(kRoomCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kRoomLevelsHigh) * kLevelHeight;

	// Where the two sofas are placed.
	float const kSofaX = 1.0f;
	float const kWideSofaX = 6.0f;

	std::string const kFixture =
		"headless/src/smoke/fixtures/furniture/furniture-atlas-new.furniture.lua";
	std::string const kTileset = "resources/textures/furniture-new.tileset.yaml";

	// ------------------------------------------------------------------
	// The atlas as the application holds it.
	// ------------------------------------------------------------------

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
		std::string location;
		std::string imageReference;
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

	// Installs the new Furniture atlas through the one install path the
	// render stack uses: pixel regions from the ImageSet, render semantics
	// from the authored tileset.
	ObjectTileset installNewFurnitureAtlas(smoke::Context const& context)
	{
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "FurnitureAtlasNew");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no FurnitureAtlasNew ImageSet");
		require(atlasSetTilesets(context)["FurnitureAtlasNew"] == "textures/furniture-new.tileset.yaml",
			"The atlas-set registry does not name furniture-new.tileset.yaml for "
			"FurnitureAtlasNew");
		auto const authored = ObjectTileset::load(context.fixture(kTileset));
		ObjectTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.sprites = installAtlasTiles(authored.sprites, set.regions,
			authored.width, authored.height, "FurnitureAtlasNew");
		setFurnitureTileset(installed, static_cast<TextureId>(1));
		return installed;
	}

	// ------------------------------------------------------------------
	// The fixture World and its recorded draw.
	// ------------------------------------------------------------------

	std::shared_ptr<const core::FurnitureCatalogue> loadFixtureCatalogue(
		smoke::Context const& context)
	{
		return core::FurnitureCatalogue::load(context.fixture(kFixture));
	}

	std::shared_ptr<core::World> buildSofaWorld(smoke::Context const& context)
	{
		auto world = std::make_shared<core::World>("FurnitureAtlasNew role tiles",
			kRoomCellsWide, kRoomLevelsHigh);
		auto const room = world->addRoom("Room", 0, 0, 0, kRoomCellsWide,
			kRoomLevelsHigh);
		world->attachFurnitureCatalogue("furniture-atlas-new.furniture.lua",
			loadFixtureCatalogue(context));
		require(world->canPlaceFurniture(room, "sofa", kSofaX, 0, "Sofa"),
			"The three-cell sofa was refused placement");
		world->placeFurniture(room, "sofa", kSofaX, 0, "Sofa");
		require(world->canPlaceFurniture(room, "sofaWide", kWideSofaX, 0,
				"Wide sofa"),
			"The four-cell sofa was refused placement");
		world->placeFurniture(room, "sofaWide", kWideSofaX, 0, "Wide sofa");
		world->finishBuild();
		world->pauseSimulation();
		return world;
	}

	void prepareCanvas()
	{
		ImGui::GetIO().DisplaySize = { kCanvasWidth, kCanvasHeight };
		ImGui::GetIO().Fonts->AddFontDefault();
		ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame();
		gUISettings.worldZoom = 1;
		gUISettings.worldViewportWidth = kCanvasWidth;
		gUISettings.worldViewportHeight = kCanvasHeight;
		gUISettings.worldViewportX = 0;
		gUISettings.worldViewportY = 0;
		gUISettings.xOffset = 0;
		gUISettings.yOffset = 0;
		gUISettings.visibleLayer = 0;
	}

	WorldDrawList drawWorld(std::shared_ptr<core::World> const& world)
	{
		WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
		RenderWorldScope scope(world);
		renderWorld(world, &drawing);
		return drawing;
	}

	// One drawn Furniture cell: where it landed on screen and which part of
	// the atlas it sampled.
	struct DrawnCell
	{
		float left{}, top{}, right{}, bottom{};
		float uvLeft{}, uvTop{}, uvRight{}, uvBottom{};
	};

	// Every FurnitureAtlas quad in the stream, as one entry per drawn cell.
	// A sprite is two triangles; they are merged on their shared bounding
	// box, which is one World cell for every full-cell tile.
	std::vector<DrawnCell> drawnFurnitureCells(WorldDrawList const& drawing)
	{
		std::map<std::pair<int, int>, DrawnCell> cells;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::FurnitureAtlas)
				continue;
			DrawnCell cell;
			cell.left = cell.top = cell.uvLeft = cell.uvTop = 1e9f;
			cell.right = cell.bottom = cell.uvRight = cell.uvBottom = -1e9f;
			for (int index = 0; index < 3; ++index)
			{
				cell.left = std::min(cell.left, triangle->positions[index].x);
				cell.top = std::min(cell.top, triangle->positions[index].y);
				cell.right = std::max(cell.right, triangle->positions[index].x);
				cell.bottom = std::max(cell.bottom, triangle->positions[index].y);
				cell.uvLeft = std::min(cell.uvLeft, triangle->texcoords[index].x);
				cell.uvTop = std::min(cell.uvTop, triangle->texcoords[index].y);
				cell.uvRight = std::max(cell.uvRight, triangle->texcoords[index].x);
				cell.uvBottom = std::max(cell.uvBottom, triangle->texcoords[index].y);
			}
			cells[{ int(std::lround(cell.left)), int(std::lround(cell.top)) }] = cell;
		}
		std::vector<DrawnCell> out;
		for (auto const& [corner, cell] : cells) out.push_back(cell);
		return out;
	}

	// The tile whose installed region this quad sampled, by its UV bounds.
	// The name is what the renderer asked for, so this is the assertion the
	// ticket asks for: which tile was drawn where, not how the map is stored.
	std::string tileDrawnAt(DrawnCell const& cell, ObjectTileset const& atlas)
	{
		for (auto const& [name, artwork] : atlas.sprites)
		{
			auto const& r = artwork.region;
			auto const left = (float(r.x) + 0.5f) / float(atlas.width);
			auto const top = (float(r.y) + 0.5f) / float(atlas.height);
			auto const right = (float(r.x + r.width) - 0.5f) / float(atlas.width);
			auto const bottom = (float(r.y + r.height) - 0.5f) / float(atlas.height);
			if (near(cell.uvLeft, left) && near(cell.uvTop, top)
				&& near(cell.uvRight, right) && near(cell.uvBottom, bottom))
				return name;
		}
		return {};
	}

	float cellWidth(DrawnCell const& cell) { return cell.right - cell.left; }
	float cellHeight(DrawnCell const& cell) { return cell.bottom - cell.top; }

	// The tiles drawn across one placed piece, left to right, by screen
	// column. A piece four cells wide that reuses its middle tile reads as
	// four columns naming three distinct tiles.
	std::vector<std::string> tilesAcrossPiece(ObjectTileset const& atlas,
		WorldDrawList const& drawing, float pieceX, int width)
	{
		std::vector<std::string> names;
		for (int column = 0; column < width; ++column)
		{
			float const worldLeft = pieceX + float(column);
			DrawnCell found;
			bool hit = false;
			for (auto const& cell : drawnFurnitureCells(drawing))
			{
				if (near(cell.left, worldLeft * kCellWidth)
					&& near(cell.right, (worldLeft + 1.0f) * kCellWidth))
				{
					found = cell;
					hit = true;
					break;
				}
			}
			require(hit, "No Furniture cell was drawn at x = "
				+ std::to_string(worldLeft));
			// Full-cell drawn size: one World cell wide and one Level tall.
			require(near(cellWidth(found), kCellWidth)
				&& near(cellHeight(found), kLevelHeight),
				"A Furniture tile was not drawn at the full-cell size its "
				"tileset declares");
			auto const name = tileDrawnAt(found, atlas);
			require(!name.empty(), "A Furniture cell sampled no tile in "
				"FurnitureAtlasNew");
			names.push_back(name);
		}
		return names;
	}

	// ------------------------------------------------------------------
	// The registration: ten role tiles, full-cell drawn size.
	// ------------------------------------------------------------------
	void tenRoleTilesAreRegisteredAtFullCellSize(smoke::Context const& context)
	{
		using smoke::require;
		auto const authored = ObjectTileset::load(context.fixture(kTileset));
		std::set<std::string> const expected{
			"chair-L0",
			"sofa-L0", "sofa-M0", "sofa-R0",
			"desk-L0", "desk-M0", "desk-R0",
			"bed-L0", "bed-M0", "bed-R0" };
		require(authored.sprites.size() == expected.size(),
			"FurnitureAtlasNew authors " + std::to_string(authored.sprites.size())
			+ " tiles; the set is ten");
		for (auto const& name : expected)
			require(authored.sprites.count(name),
				"FurnitureAtlasNew registers no '" + name + "' tile");
		for (auto const& [name, artwork] : authored.sprites)
		{
			// Every Furniture tile is a role tile: the suffix is applied
			// because Furniture is the kind of thing that spans more than
			// one cell.
			require(name.size() > 3 && name[name.size() - 3] == '-'
				&& (name[name.size() - 2] == 'L' || name[name.size() - 2] == 'M'
					|| name[name.size() - 2] == 'R')
				&& name[name.size() - 1] == '0',
				"'" + name + "' is not named by role and height level");
			// Full-cell drawn size, at the anchor, so a piece one cell wide
			// covers one cell and a piece four cells wide covers four.
			require(near(artwork.drawnSize.x, 1.0f)
				&& near(artwork.drawnSize.y, 1.0f),
				"'" + name + "' must be drawn at one full World cell");
			require(near(artwork.offset.x, 0.0f) && near(artwork.offset.y, 0.0f),
				"'" + name + "' must be drawn at its cell's anchor");
			require(artwork.region.width == 128 && artwork.region.height == 320,
				"'" + name + "' must fill its 128x320 authored cell");
		}
		// The sheet is the 5x2 grid of full cells the spec states.
		require(authored.width == 640 && authored.height == 640,
			"FurnitureAtlasNew is not the 640x640 sheet its set is sized for");
		// The manifest's generated entries agree with the authored tileset,
		// so the runtime pixel contract cannot drift from what was authored.
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "FurnitureAtlasNew");
		require(set.found && set.type == "ImageSet",
			"FurnitureAtlasNew is not registered as an ImageSet");
		require(set.imageReference == "FurnitureAtlasNewImage",
			"FurnitureAtlasNew's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "FurnitureAtlasNewImage");
		require(image.found && image.type == "Image"
			&& image.location == "textures/furniture-new.png",
			"FurnitureAtlasNewImage is not the atlas image the tileset names");
		require(set.regions.size() == authored.sprites.size(),
			"The manifest's FurnitureAtlasNew entries and the authored tileset "
			"differ in count");
		for (auto const& [name, sprite] : authored.sprites)
		{
			auto const region = set.regions.find(name);
			require(region != set.regions.end(),
				"Authored tile '" + name + "' has no generated ImageSet entry");
			require(region->second.x == sprite.region.x
				&& region->second.y == sprite.region.y
				&& region->second.width == sprite.region.width
				&& region->second.height == sprite.region.height,
				"Generated ImageSet entry for '" + name
				+ "' drifted from the authored tileset");
		}
		// The registry names this tileset for this atlas, as object tiles.
		require(atlasSetTilesets(context)["FurnitureAtlasNew"]
			== "textures/furniture-new.tileset.yaml",
			"The atlas-set registry names no object tileset for FurnitureAtlasNew");
	}

	// ------------------------------------------------------------------
	// The render: a multi-cell piece draws from its role tiles.
	// ------------------------------------------------------------------
	void sofaRendersFromItsRoleTiles(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewFurnitureAtlas(context);
		auto const world = buildSofaWorld(context);
		prepareCanvas();
		auto const drawing = drawWorld(world);

		auto const tiles = tilesAcrossPiece(atlas, drawing, kSofaX, 3);
		require(tiles[0] == "sofa-L0" && tiles[1] == "sofa-M0"
			&& tiles[2] == "sofa-R0",
			"The sofa rendered from " + tiles[0] + ", " + tiles[1] + ", "
			+ tiles[2] + " instead of its left, middle and right tiles");
	}

	// Widening the piece to four cells reuses the same middle tile. The
	// fourth cell is not a new tile: two of the four columns sample the
	// identical atlas region, and the whole piece still names three tiles.
	void fourCellSofaReusesTheMiddleTile(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewFurnitureAtlas(context);
		auto const world = buildSofaWorld(context);
		prepareCanvas();
		auto const drawing = drawWorld(world);

		auto const tiles = tilesAcrossPiece(atlas, drawing, kWideSofaX, 4);
		require(tiles[0] == "sofa-L0" && tiles[3] == "sofa-R0",
			"The four-cell sofa lost a cap: " + tiles[0] + " ... " + tiles[3]);
		require(tiles[1] == "sofa-M0" && tiles[2] == "sofa-M0",
			"The four-cell sofa drew " + tiles[1] + " and " + tiles[2]
			+ " in its middle cells instead of reusing sofa-M0");
		std::set<std::string> distinct(tiles.begin(), tiles.end());
		require(distinct.size() == 3,
			"A four-cell sofa needed " + std::to_string(distinct.size())
			+ " distinct tiles; the role vocabulary makes it three");
		// The two middle cells sample the same atlas region, which is the
		// reuse stated as pixels rather than as names.
		std::vector<DrawnCell> middles;
		for (auto const& cell : drawnFurnitureCells(drawing))
			if (near(cell.left, (kWideSofaX + 1.0f) * kCellWidth)
				|| near(cell.left, (kWideSofaX + 2.0f) * kCellWidth))
				middles.push_back(cell);
		require(middles.size() == 2, "The four-cell sofa drew "
			+ std::to_string(middles.size()) + " middle cells");
		require(near(middles[0].uvLeft, middles[1].uvLeft)
			&& near(middles[0].uvTop, middles[1].uvTop)
			&& near(middles[0].uvRight, middles[1].uvRight)
			&& near(middles[0].uvBottom, middles[1].uvBottom),
			"The two middle cells sampled different atlas regions: the middle "
			"tile was not reused");
	}

	// ------------------------------------------------------------------
	// The loader seam: the name must agree with the placement.
	// ------------------------------------------------------------------

	// Loads a variant of the fixture catalogue with one Lua statement
	// appended, so each refusal is driven by a generated fixture rather
	// than by reaching into the parser.
	std::string loadRefusal(smoke::Context const& context, std::string const& tag,
		std::string const& mutation)
	{
		auto source = smoke::catalogueSource(context.fixture(kFixture));
		source += mutation + "\n";
		auto const filename = context.temporaryRoot() / (tag + ".furniture.lua");
		smoke::writeCatalogue(filename, source);
		try
		{
			core::FurnitureCatalogue::load(filename);
		}
		catch (std::exception const& error)
		{
			return std::string(error.what());
		}
		return {};
	}

	void loadAccepts(smoke::Context const& context, std::string const& tag,
		std::string const& mutation)
	{
		using smoke::require;
		auto source = smoke::catalogueSource(context.fixture(kFixture));
		source += mutation + "\n";
		auto const filename = context.temporaryRoot() / (tag + ".furniture.lua");
		smoke::writeCatalogue(filename, source);
		auto catalogue = core::FurnitureCatalogue::load(filename);
		require(bool(catalogue), "A conforming footprint was refused: " + tag);
	}

	// A `R` at a position its width forbids is refused. Adding a fourth
	// cell to the three-cell sofa leaves its right cap one cell short of the
	// footprint it now sits in.
	void loaderRefusesARoleAtAPositionItsWidthForbids(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "right-cap-misplaced",
			"table.insert(catalogue.definitions[1].tiles, "
			"{x = 3, y = 0, image = \"sofa-M0\"})");
		require(!refusal.empty(),
			"A right cap one cell short of its footprint's width was accepted");
		require(refusal.find("right cap") != std::string::npos
			&& refusal.find("sofa-R0") != std::string::npos,
			"The refusal must name the cap it refused: " + refusal);
		require(refusal.find("forbids") != std::string::npos,
			"The refusal must state the width's rule: " + refusal);
	}

	// `L` is always present because it is the anchor. A footprint of one
	// cell named only for its right cap has no anchor at all.
	void loaderRefusesAFootprintWithNoAnchor(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "no-anchor",
			"catalogue.definitions[3].tiles = "
			"{{x = 0, y = 0, image = \"sofa-R0\"}}");
		require(refusal.find("no L tile") != std::string::npos,
			"A footprint with no anchor was accepted: " + refusal);
	}

	// `R` appears when width >= 2: a two-cell piece with no right cap is
	// half a sofa.
	void loaderRefusesAMissingRightCap(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "no-right-cap",
			"table.remove(catalogue.definitions[2].tiles, 4)");
		require(refusal.find("no R tile") != std::string::npos,
			"A two-cell piece with no right cap was accepted: " + refusal);
	}

	// `M` appears when width >= 3: a three-cell piece that skips its middle
	// leaves a hole the naming convention says cannot be there.
	void loaderRefusesAMissingMiddle(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "no-middle",
			"table.remove(catalogue.definitions[1].tiles, 2)");
		require(refusal.find("no M tile") != std::string::npos,
			"A three-cell piece with no middle was accepted: " + refusal);
	}

	// One vocabulary per piece. A footprint that says `L0` here and `-00`
	// there is two pieces wearing one name.
	void loaderRefusesAMixedVocabulary(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "mixed-vocabulary",
			"catalogue.definitions[1].tiles[1].image = \"sofa-00\"");
		require(refusal.find("mixes") != std::string::npos
			&& refusal.find("role") != std::string::npos
			&& refusal.find("absolute") != std::string::npos,
			"A footprint mixing role and absolute tiles was accepted: "
			+ refusal);
	}

	// A bare name inside a suffixed footprint names no position, which is
	// the one thing a piece speaking the convention cannot afford.
	void loaderRefusesABareTileInASuffixedFootprint(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "bare-in-suffixed",
			"catalogue.definitions[1].tiles[2].image = \"sofa\"");
		require(refusal.find("unsuffixed") != std::string::npos,
			"A bare tile inside a suffixed footprint was accepted: " + refusal);
	}

	// For height > 1 every role present must be present at every height
	// level. A two-level piece that only finishes the bottom half is
	// half-specified and is refused.
	void loaderRefusesAHalfSpecifiedHeight(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "half-height",
			"table.insert(catalogue.definitions[1].tiles, "
			"{x = 0, y = 1, image = \"sofa-L1\"})");
		require(refusal.find("level 1") != std::string::npos,
			"A half-specified two-level footprint was accepted: " + refusal);
	}

	// The anchor is the bottom-left, so no offset is negative.
	void loaderRefusesANegativeOffset(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "negative-offset",
			"catalogue.definitions[1].tiles[1].x = -1");
		require(refusal.find("negatively") != std::string::npos,
			"A negative offset in a suffixed footprint was accepted: "
			+ refusal);
	}

	// One digit per axis: a tenth column cannot be named.
	void loaderRefusesAnOffsetPastOneDigit(smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "two-digit-offset",
			"catalogue.definitions[1].tiles[3].x = 10");
		require(refusal.find("one digit") != std::string::npos,
			"An offset past one digit was accepted: " + refusal);
	}

	// The catalogue's `{x, y}` is authoritative and the absolute name is
	// checked against it, so the escape hatch cannot say one thing while
	// the catalogue means another.
	void loaderRefusesAnAbsoluteNameThatDisagreesWithItsPlacement(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const refusal = loadRefusal(context, "absolute-mismatch",
			"catalogue.definitions[4].tiles[2].image = \"desk-20\"");
		require(refusal.find("desk-20") != std::string::npos
			&& refusal.find("but the catalogue places it at 1,0")
				!= std::string::npos,
			"An absolute name disagreeing with its placement was accepted: "
			+ refusal);
	}

	// A conforming footprint loads: the three-cell sofa, the four-cell sofa
	// that reuses its middle tile, the anchor-only chair, and the absolute
	// escape hatch.
	void loaderAcceptsConformingFootprints(smoke::Context const& context)
	{
		using smoke::require;
		loadAccepts(context, "accepts-role-footprints", "-- no mutation");
		// A five-cell sofa: still three tiles, one more middle.
		loadAccepts(context, "accepts-five-cell-sofa",
			"catalogue.definitions[2].tiles[4].x = 4\n"
			"table.insert(catalogue.definitions[2].tiles, 4, "
			"{x = 3, y = 0, image = \"sofa-M0\"})");
		// A two-level piece with every role at every level. The count is
		// taken first: appending into the table an ipairs loop is walking
		// would never end.
		loadAccepts(context, "accepts-two-level-sofa",
			"local n = #catalogue.definitions[1].tiles\n"
			"for i = 1, n do local t = catalogue.definitions[1].tiles[i] "
			"table.insert(catalogue.definitions[1].tiles, {x = t.x, y = 1, "
			"image = string.gsub(t.image, \"0$\", \"1\")}) end");
		// A single-cell piece keeps its bare name, and a single-cell piece
		// can equally be named by its anchor role.
		loadAccepts(context, "accepts-bare-single-cell", "-- no mutation");
		require(bool(loadFixtureCatalogue(context)->definition("bareChair")),
			"A single-cell piece with a bare name was refused");
		require(bool(loadFixtureCatalogue(context)->definition("chair")),
			"A single-cell piece named by its anchor role was refused");
	}

	// The old artwork's names predate the convention and stay loadable: the
	// convention binds a piece that speaks it, not every catalogue written
	// before it.
	void loaderLeavesLegacyNamesAlone(smoke::Context const& context)
	{
		using smoke::require;
		auto const catalogue = loadFixtureCatalogue(context);
		auto const legacy = catalogue->definition("legacySofa");
		require(bool(legacy) && legacy->tiles.size() == 2,
			"The old artwork's multi-cell names no longer load");
		require(legacy->tiles[0].image == "sofa-left"
			&& legacy->tiles[1].image == "sofa-right",
			"The legacy fixture stopped naming the tiles the old atlas holds");
	}
}

void render_smoke::registerFurnitureAtlasNew(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furnitureAtlasNew/tenRoleTilesAreRegisteredAtFullCellSize",
		isolated<tenRoleTilesAreRegisteredAtFullCellSize> });
	checks.push_back({ "furnitureAtlasNew/sofaRendersFromItsRoleTiles",
		isolated<sofaRendersFromItsRoleTiles> });
	checks.push_back({ "furnitureAtlasNew/fourCellSofaReusesTheMiddleTile",
		isolated<fourCellSofaReusesTheMiddleTile> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesARoleAtAPositionItsWidthForbids",
		isolated<loaderRefusesARoleAtAPositionItsWidthForbids> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAFootprintWithNoAnchor",
		isolated<loaderRefusesAFootprintWithNoAnchor> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAMissingRightCap",
		isolated<loaderRefusesAMissingRightCap> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAMissingMiddle",
		isolated<loaderRefusesAMissingMiddle> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAMixedVocabulary",
		isolated<loaderRefusesAMixedVocabulary> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesABareTileInASuffixedFootprint",
		isolated<loaderRefusesABareTileInASuffixedFootprint> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAHalfSpecifiedHeight",
		isolated<loaderRefusesAHalfSpecifiedHeight> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesANegativeOffset",
		isolated<loaderRefusesANegativeOffset> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAnOffsetPastOneDigit",
		isolated<loaderRefusesAnOffsetPastOneDigit> });
	checks.push_back({ "furnitureAtlasNew/loaderRefusesAnAbsoluteNameThatDisagreesWithItsPlacement",
		isolated<loaderRefusesAnAbsoluteNameThatDisagreesWithItsPlacement> });
	checks.push_back({ "furnitureAtlasNew/loaderAcceptsConformingFootprints",
		isolated<loaderAcceptsConformingFootprints> });
	checks.push_back({ "furnitureAtlasNew/loaderLeavesLegacyNamesAlone",
		isolated<loaderLeavesLegacyNamesAlone> });
}
