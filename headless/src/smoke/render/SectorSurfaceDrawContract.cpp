// The Sector surface draw contract, pinned before the draw path changes (#1037).
//
// This is a contract, not a snapshot. It states, in the check, the rule the
// renderer is under today for a Sector surface that has no per-cell artwork
// variants - which is every Sector surface that exists right now - and then
// holds the recorded command stream to that rule, cell by cell:
//
//   * which tile region each cell samples, and that every cell of a
//     no-variant Sector samples the same one: the type's own base tile;
//   * the destination rectangle each cell lands on, in screen pixels,
//     against the Sector's own drawn area;
//   * the tint each cell carries;
//   * that the repeat runs one tile per World cell, `cellsWide` columns by
//     `levelsHigh` rows, anchored on the Sector's floor line;
//   * that a top level shorter than a full cell is anchored on its own
//     bottom edge and cropped at the top, at one artwork pixel to one
//     screen pixel, rather than stretched to fill the partial rect.
//
// The next change to this path (#1036) replaces the whole-area repeat with
// a loop that resolves one tile per cell. That rewrite has to satisfy what
// is asserted here rather than a visual impression, because the one thing
// it must not do is quietly re-anchor every short Room and 0.7 Corridor in
// every saved World while looking like a decorative change. A variant is a
// different picture, never a different geometry: with no variant recorded,
// the per-cell loop must emit exactly the cells recorded below, at the same
// rectangles, sampling the same tile region with the same crop.
//
// It is written against the current path on purpose. Written after the new
// path existed it would pin whatever the new path happened to do, and be
// worthless as a guard. Nothing in this file changes production code, and
// nothing here should be relaxed to make the new path pass: if the new path
// disagrees with a line below, that is the regression this file exists to
// find.
//
// The expectations are derived from the domain rule, not copied out of the
// renderer. The Sector's own geometry - cells wide, levels high, top-level
// height - is read back off the built World and checked against what the
// case declared, so a fixture that drifted cannot quietly move the target.
//
// Every assertion is on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names. The atlas is installed the way the
// render stack installs it, so the tile regions asserted here are the
// pixel regions the application's own `ImageSet` declares.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
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

	std::string const kTileset = "resources/textures/sectors-new.tileset.yaml";

	// The tint every check here asks the renderer for. It is a colour no
	// production path picks by default, so a surface that ignored the
	// caller's tint - drawing white, or the Layer's colour, or the
	// lights-off tint - fails instead of looking right.
	pf::render::PackedColourValue const kTint = packColour(214, 96, 32, 255);

	std::string rectText(float left, float top, float right, float bottom)
	{
		std::ostringstream out;
		out << "(" << left << "," << top << ")-(" << right << "," << bottom << ")";
		return out.str();
	}

	// ------------------------------------------------------------------
	// The atlas, installed the way the render stack installs it.
	// ------------------------------------------------------------------

	// The pixel regions one `ImageSet` entry declares, and the authored
	// tileset that carries the render semantics for the same names. Read
	// straight from the committed application manifest and the atlas-set
	// registry, so the regions asserted below are the ones the game draws
	// from and not a copy that can drift.
	std::map<std::string, AtlasPixelRegion> imageSetRegions(
		smoke::Context const& context, std::string const& setName)
	{
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		std::map<std::string, AtlasPixelRegion> regions;
		for (auto const& resource : manifest["Resources"]["Resource"])
		{
			if (!resource.IsMap()) continue;
			if (resource["name"].as<std::string>() != setName) continue;
			require(resource["type"].as<std::string>() == "ImageSet",
				setName + " is not registered as an ImageSet");
			// Definitions -> Definition -> Images -> Image, the shape the
			// application manifest is written in.
			if (auto const definitions = resource["Definitions"])
				if (auto const definition = definitions["Definition"])
					if (auto const images = definition["Images"])
						for (auto const& image : images["Image"])
							regions.emplace(image["name"].as<std::string>(),
								AtlasPixelRegion{ image["x"].as<int>(),
									image["y"].as<int>(),
									image["width"].as<int>(),
									image["height"].as<int>() });
			return regions;
		}
		throw std::runtime_error(
			"The application manifest registers no " + setName + " resource");
	}

	std::string registryTileset(smoke::Context const& context,
		std::string const& setName)
	{
		auto const root = YAML::LoadFile(
			context.fixture("resources/atlas-sets.yaml").string());
		require(root.IsMap() && root["sets"].IsMap(),
			"resources/atlas-sets.yaml has no `sets` map of atlas name to "
			"authored tileset");
		auto const found = root["sets"][setName];
		require(bool(found) && found["tileset"],
			"The atlas-set registry names no tileset for " + setName);
		return found["tileset"].as<std::string>();
	}

	SectorTileset installNewSectorAtlas(smoke::Context const& context)
	{
		require(registryTileset(context, "SectorAtlas")
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
		installed.family = authored.family;
		installed.tiles = installAtlasTiles(authored.tiles,
			imageSetRegions(context, "SectorAtlas"), authored.width,
			authored.height, "SectorAtlas");
		for (auto const& [name, artwork] : installed.tiles)
			installed.surfaces.emplace(name, artwork.region);
		// An install that produced no surfaces would leave every draw below
		// falling back to the missing-tile rectangle, and the contract would
		// be pinning nothing. The count of tiles in the set is
		// `sectorAtlasNew`'s own business; what matters here is that the
		// surfaces the cases ask for are actually installed.
		require(!installed.surfaces.empty(),
			"The installed SectorAtlas carries no surface tiles");
		setSectorTileset(installed, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The installed new Sector atlas did not come up as the New family");
		return installed;
	}

	// ------------------------------------------------------------------
	// The viewport this contract draws in.
	// ------------------------------------------------------------------
	//
	// Origin and both offsets pinned to zero and the zoom pinned to one, so
	// the expected screen rectangles are arithmetic on the Sector's own
	// World bounds rather than a restatement of whatever the last check in
	// the module left in the shared settings.
	void pinViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 2048.0f;
		gUISettings.worldViewportHeight = 2048.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = 1.0f;
	}

	// The World-to-screen transform, mirroring `transformPosition` in
	// Render.cpp: x scales by the cell width, y by the Level height and
	// flips, and the viewport and pan offsets are added. Stated here so the
	// contract can name absolute screen rectangles; the settings above are
	// pinned, so this is exact rather than approximate.
	pf::render::Vec2 toScreen(core::Vector2 const& point, float const zoom)
	{
		return { point.x * CORE_CELL_WIDTH_PIXELS * zoom
				+ gUISettings.worldViewportX + gUISettings.xOffset,
			gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- point.y * CORE_LEVEL_HEIGHT_PIXELS * zoom
				- gUISettings.yOffset };
	}

	// The Sector's own area on screen, in the orientation the renderer
	// uses: the floor line is the greatest screen y, the ceiling the least.
	struct SectorArea
	{
		float left{};
		float right{};
		float top{};
		float floorLine{};
		float cellWidth{};
		float cellHeight{};
	};

	SectorArea areaOf(core::Sector const& sector, float const zoom)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const a = toScreen(low, zoom);
		auto const b = toScreen(high, zoom);
		SectorArea area;
		area.left = std::min(a.x, b.x);
		area.right = std::max(a.x, b.x);
		area.top = std::min(a.y, b.y);
		area.floorLine = std::max(a.y, b.y);
		area.cellWidth = CORE_CELL_WIDTH_PIXELS * zoom;
		area.cellHeight = CORE_LEVEL_HEIGHT_PIXELS * zoom;
		return area;
	}

	// ------------------------------------------------------------------
	// The fixtures: Sectors several cells wide and several levels high.
	// ------------------------------------------------------------------

	struct SurfaceFixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> sector;
	};

	std::shared_ptr<const core::Sector> sectorNamed(core::World const& world,
		std::string const& name)
	{
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
			if (world.getSector(index)->getName() == name)
				return world.getSector(index);
		return nullptr;
	}

	SurfaceFixture roomFixture(uint32_t const cellsWide, uint32_t const levelsHigh,
		float const topLevelHeight)
	{
		auto world = std::make_shared<core::World>("Room draw contract", 8, 8);
		world->addRoom("Room", 0, 0, 0, cellsWide, levelsHigh, topLevelHeight);
		world->finishBuild();
		return { world, sectorNamed(*world, "Room") };
	}

	SurfaceFixture corridorFixture(uint32_t const cellsWide,
		uint32_t const levelsHigh, float const)
	{
		auto world = std::make_shared<core::World>("Corridor draw contract", 8, 8);
		world->addCorridor(0, 0, 0, cellsWide, levelsHigh);
		world->finishBuild();
		return { world, sectorNamed(*world, "Corridor") };
	}

	// A Lift Transit is one or two cells wide and a whole number of levels
	// high: its top level is a full cell, so it is the case here that draws
	// no crop at all. The landings sit on the Layer in front, at the two
	// extreme stops, which is what fixes the shaft's height.
	SurfaceFixture liftFixture(uint32_t const cellsWide, uint32_t const levelsHigh,
		float const)
	{
		auto world = std::make_shared<core::World>("Lift draw contract", 8, 8);
		world->addCorridor(0, 0, 7);
		world->addCorridor(levelsHigh - 1, 0, 7);
		core::World::CreateLiftOptions options;
		options.cellsWide = cellsWide;
		options.stopOffsets = { 0, levelsHigh - 1 };
		options.capacity = 2;
		world->addLift(1, 0, 2, options);
		world->finishBuild();
		return { world, sectorNamed(*world, "Lift") };
	}

	// One Sector the contract is asserted against. `topLevelHeight` is the
	// domain fact the fixture is required to build; the check reads it back
	// off the live Sector before asserting anything about the draw, so a
	// fixture that drifted cannot quietly move the target.
	struct SurfaceCase
	{
		std::string surface;
		std::string label;
		uint32_t cellsWide;
		uint32_t levelsHigh;
		float topLevelHeight;
		SurfaceFixture (*make)(uint32_t, uint32_t, float);
	};

	// The multi-cell cases. A Room and a Corridor, both with a top level
	// shorter than the cell they are drawn for, and a Lift whose every level
	// is a whole cell. Together they cover a grid with a cropped top row and
	// one without, at more than one width and more than one height.
	std::vector<SurfaceCase> multiCellCases()
	{
		return {
			{ "room", "a Room four cells wide and three levels high", 4, 3,
				CORE_ROOM_STANDARD_HEIGHT, &roomFixture },
			{ "corridor", "a Corridor six cells wide and three levels high", 6, 3,
				CORE_CORRIDOR_HEIGHT, &corridorFixture },
			{ "lift", "a Lift two cells wide and four levels high", 2, 4, 1.0f,
				&liftFixture },
		};
	}

	// Builds the case's Sector and checks its geometry says what the case
	// claims before any drawing is asserted.
	SurfaceFixture builtCase(SurfaceCase const& testCase)
	{
		auto const fixture = testCase.make(testCase.cellsWide, testCase.levelsHigh,
			testCase.topLevelHeight);
		require(fixture.sector != nullptr,
			"No Sector was built for " + testCase.label);
		require(fixture.sector->getCellsWide() == testCase.cellsWide,
			testCase.label + " came out "
				+ std::to_string(fixture.sector->getCellsWide())
				+ " cells wide, the case says "
				+ std::to_string(testCase.cellsWide));
		require(fixture.sector->getLevelsHigh() == testCase.levelsHigh,
			testCase.label + " came out "
				+ std::to_string(fixture.sector->getLevelsHigh())
				+ " levels high, the case says "
				+ std::to_string(testCase.levelsHigh));
		require(near(fixture.sector->getEffectiveTopLevelHeight(),
				testCase.topLevelHeight),
			testCase.label + "'s top level is "
				+ std::to_string(fixture.sector->getEffectiveTopLevelHeight())
				+ " of a cell, the case says "
				+ std::to_string(testCase.topLevelHeight));
		require(fixture.sector->areLightsOn(),
			testCase.label + " was built with its lights off, so the tint asked "
				"for here would be replaced by the lights-off tint");
		return fixture;
	}

	// ------------------------------------------------------------------
	// The recorded cell.
	// ------------------------------------------------------------------

	// One surface quad as it came out of the command stream: the two
	// triangles one `AddImage` call emitted, read back as one rectangle on
	// the screen and one fraction of one tile in the atlas.
	struct RecordedCell
	{
		std::string tile;
		SectorTileRegion region{};
		float left{}, top{}, right{}, bottom{};
		float u0{}, u1{}, v0{}, v1{};
		pf::render::PackedColourValue tint{};

		float height() const { return bottom - top; }
		// The artwork texels the quad samples along the tile's vertical axis.
		float sampledTexelHeight() const
		{
			return (v1 - v0) * float(region.height - 1);
		}
	};

	std::string describe(RecordedCell const& cell)
	{
		return cell.tile + " -> " + rectText(cell.left, cell.top, cell.right,
			cell.bottom) + " sampling u " + std::to_string(cell.u0) + ".."
			+ std::to_string(cell.u1) + ", v " + std::to_string(cell.v0) + ".."
			+ std::to_string(cell.v1);
	}

	// The tile a sampled UV falls on, by the centre of the sampled range.
	// A crop moves the corner of that range but can never move its centre
	// off the tile it came from, and the tiles are disjoint regions of one
	// sheet, so the centre lands on exactly one of them.
	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom =
				float(region.y + region.height) / float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
				&& centreV < bottom)
				return name;
		}
		return {};
	}

	// Records the Sector's surface quads out of a real `renderSector` pass.
	// Only SectorAtlas triangles are of interest here: a Lift's procedural
	// car, a Door's art and a wireframe are other passes and say nothing
	// about the surface contract.
	std::vector<RecordedCell> recordSurfaceCells(
		std::shared_ptr<const core::Sector> sector, uint32_t const layer,
		SectorTileset const& atlas)
	{
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderSector(sector, layer, LayerRenderStyle::Solid, false, kTint,
			&commands);

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle
				&& triangle->texture == WorldDrawList::Texture::SectorAtlas)
				quads.push_back(triangle);
		}
		require(quads.size() % 2 == 0,
			"The Sector emitted an odd number of SectorAtlas triangles, so they "
			"are not the pairs one AddImage call makes per quad");

		std::vector<RecordedCell> cells;
		for (size_t pair = 0; pair < quads.size(); pair += 2)
		{
			auto const& first = *quads[pair];
			auto const& second = *quads[pair + 1];
			require(first.colour == second.colour,
				"The two triangles of one surface quad were tinted differently");
			RecordedCell cell;
			cell.tint = first.colour;

			// The pair's extents, read once over all six vertices. A quad from
			// AddImage is axis-aligned, so the min and max of the two
			// triangles' corners is the rectangle it covers.
			cell.left = cell.top = 1e9f;
			cell.right = cell.bottom = -1e9f;
			float uvLeft = 1e9f, uvRight = -1e9f, uvTop = 1e9f, uvBottom = -1e9f;
			for (auto const* triangle : { &first, &second })
			{
				for (auto const& p : triangle->positions)
				{
					cell.left = std::min(cell.left, p.x);
					cell.right = std::max(cell.right, p.x);
					cell.top = std::min(cell.top, p.y);
					cell.bottom = std::max(cell.bottom, p.y);
				}
				for (auto const& uv : triangle->texcoords)
				{
					uvLeft = std::min(uvLeft, uv.x);
					uvRight = std::max(uvRight, uv.x);
					uvTop = std::min(uvTop, uv.y);
					uvBottom = std::max(uvBottom, uv.y);
				}
			}
			cell.tile = tileAt((uvLeft + uvRight) * 0.5f, (uvTop + uvBottom) * 0.5f,
				atlas);
			require(!cell.tile.empty(),
				"A surface quad sampled UVs that fall on no tile in the "
				"installed atlas");
			cell.region = atlas.surfaces.at(cell.tile);
			// The sampled fraction of the tile, undoing the half-texel inset
			// the renderer uses so linear filtering never reads a neighbouring
			// tile's edge texel.
			auto const fraction = [&](float uv, int origin, int extent)
			{
				return (uv * float(atlas.width) - (float(origin) + 0.5f))
					/ float(extent - 1);
			};
			cell.u0 = fraction(uvLeft, cell.region.x, cell.region.width);
			cell.u1 = fraction(uvRight, cell.region.x, cell.region.width);
			cell.v0 = fraction(uvTop, cell.region.y, cell.region.height);
			cell.v1 = fraction(uvBottom, cell.region.y, cell.region.height);
			cells.push_back(cell);
		}
		return cells;
	}

	// ------------------------------------------------------------------
	// The grid the rule says the Sector draws, stated in the check.
	// ------------------------------------------------------------------

	struct ExpectedCell
	{
		uint32_t column{};
		uint32_t row{};   // 0 is the row standing on the floor line
		float left{}, top{}, right{}, bottom{};
		float u0{}, u1{}, v0{}, v1{};
	};

	// One tile per World cell, `cellsWide` columns across, rows stacked
	// upward from the Sector's floor line. The bottom of each row is one
	// whole cell below the row beneath it; the top of the topmost row is
	// the Sector's own ceiling, so that row is short whenever the Sector is
	// not a whole number of cells high, and it samples only the part of its
	// tile that stands above its own top edge.
	std::vector<ExpectedCell> expectedGrid(core::Sector const& sector,
		SectorArea const& area)
	{
		std::vector<ExpectedCell> cells;
		for (uint32_t row = 0; row < sector.getLevelsHigh(); ++row)
		{
			ExpectedCell cell;
			cell.row = row;
			cell.bottom = area.floorLine - float(row) * area.cellHeight;
			cell.top = std::max(cell.bottom - area.cellHeight, area.top);
			cell.left = area.left;
			cell.right = area.right;
			cell.u0 = 0.0f;
			cell.u1 = 1.0f;
			cell.v1 = 1.0f;
			cell.v0 = 1.0f - (cell.bottom - cell.top) / area.cellHeight;
			for (uint32_t column = 0; column < sector.getCellsWide(); ++column)
			{
				cell.column = column;
				cell.left = area.left + float(column) * area.cellWidth;
				cell.right = cell.left + area.cellWidth;
				cells.push_back(cell);
			}
		}
		return cells;
	}

	bool sameRect(ExpectedCell const& expected, RecordedCell const& recorded)
	{
		return near(expected.left, recorded.left)
			&& near(expected.right, recorded.right)
			&& near(expected.top, recorded.top)
			&& near(expected.bottom, recorded.bottom);
	}

	// Pairs each expected cell with the recorded cell that landed on it, and
	// fails if any expected cell is missing or any recorded cell landed
	// outside the grid. The contract is on what is drawn, not on the order
	// it is emitted in: a per-cell loop may walk the grid in any order.
	std::vector<RecordedCell> matchedTo(std::vector<ExpectedCell> const& expected,
		std::vector<RecordedCell> const& recorded, std::string const& label)
	{
		require(recorded.size() == expected.size(),
			label + " emitted " + std::to_string(recorded.size())
				+ " surface quads; the grid is " + std::to_string(expected.size())
				+ " cells");
		std::vector<bool> taken(recorded.size(), false);
		std::vector<RecordedCell> matched;
		for (auto const& cell : expected)
		{
			size_t found = recorded.size();
			for (size_t index = 0; index < recorded.size(); ++index)
			{
				if (taken[index]) continue;
				if (sameRect(cell, recorded[index]))
				{
					found = index;
					break;
				}
			}
			require(found != recorded.size(),
				label + " drew nothing at " + rectText(cell.left, cell.top,
					cell.right, cell.bottom) + " (column "
					+ std::to_string(cell.column) + ", row "
					+ std::to_string(cell.row) + ")");
			taken[found] = true;
			matched.push_back(recorded[found]);
		}
		for (size_t index = 0; index < recorded.size(); ++index)
			require(taken[index],
				label + " drew a surface quad at " + describe(recorded[index])
					+ " that is not on the per-cell grid");
		return matched;
	}

	// ------------------------------------------------------------------
	// The contract, cell by cell.
	// ------------------------------------------------------------------

	// Every cell of a no-variant Sector draws the type's one base tile, on
	// the rectangle its World cell occupies, in the tint the caller asked
	// for. Nothing else is drawn and nothing is left undrawn.
	void everyCellOfANoVariantSectorDrawsItsBaseTile(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		pinViewport();
		for (auto const& testCase : multiCellCases())
		{
			auto const fixture = builtCase(testCase);
			require(atlas.surfaces.count(testCase.surface),
				"The installed atlas carries no '" + testCase.surface
					+ "' surface for " + testCase.label);
			auto const recorded = recordSurfaceCells(fixture.sector,
				fixture.sector->getLayerIndex(), atlas);
			auto const expected = expectedGrid(*fixture.sector,
				areaOf(*fixture.sector, gUISettings.worldZoom));
			require(expected.size()
					== size_t(testCase.cellsWide) * size_t(testCase.levelsHigh),
				testCase.label + ": the grid is not cellsWide x levelsHigh cells");
			auto const matched = matchedTo(expected, recorded, testCase.label);
			auto const baseRegion = atlas.surfaces.at(testCase.surface);
			for (size_t index = 0; index < matched.size(); ++index)
			{
				auto const& cell = matched[index];
				auto const& want = expected[index];
				require(cell.tile == testCase.surface,
					testCase.label + " drew '" + cell.tile + "' at column "
						+ std::to_string(want.column) + ", row "
						+ std::to_string(want.row) + "; a no-variant Sector "
						"wears its own base tile on every cell");
				require(cell.region.x == baseRegion.x && cell.region.y == baseRegion.y
						&& cell.region.width == baseRegion.width
						&& cell.region.height == baseRegion.height,
					testCase.label + " sampled a region other than its base "
						"tile's at column " + std::to_string(want.column)
						+ ", row " + std::to_string(want.row));
				require(near(cell.u0, want.u0) && near(cell.u1, want.u1)
						&& near(cell.v0, want.v0) && near(cell.v1, want.v1),
					testCase.label + " sampled u " + std::to_string(cell.u0) + ".."
						+ std::to_string(cell.u1) + ", v "
						+ std::to_string(cell.v0) + ".." + std::to_string(cell.v1)
						+ " at column " + std::to_string(want.column) + ", row "
						+ std::to_string(want.row) + "; the grid says "
						+ std::to_string(want.u0) + ".." + std::to_string(want.u1)
						+ ", " + std::to_string(want.v0) + ".."
						+ std::to_string(want.v1));
				require(near(cell.left, want.left) && near(cell.right, want.right)
						&& near(cell.top, want.top)
						&& near(cell.bottom, want.bottom),
					testCase.label + " landed at " + describe(cell) + " at column "
						+ std::to_string(want.column) + ", row "
						+ std::to_string(want.row) + "; the grid says "
						+ rectText(want.left, want.top, want.right, want.bottom));
				require(cell.tint == kTint,
					testCase.label + " carried a tint other than the one asked "
						"for at column " + std::to_string(want.column) + ", row "
						+ std::to_string(want.row));
			}
		}
	}

	// The repeat is anchored on the Sector's floor line, not on its
	// ceiling and not on the viewport. Every row stands on the row below it,
	// the row on the floor line is a whole cell, and every row samples the
	// tile's own floor end at its own bottom edge.
	void theRepeatIsAnchoredOnTheSectorFloorLine(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		pinViewport();
		for (auto const& testCase : multiCellCases())
		{
			auto const fixture = builtCase(testCase);
			auto const area = areaOf(*fixture.sector, gUISettings.worldZoom);
			auto const recorded = recordSurfaceCells(fixture.sector,
				fixture.sector->getLayerIndex(), atlas);
			require(recorded.size()
					== size_t(testCase.cellsWide) * size_t(testCase.levelsHigh),
				testCase.label + " emitted " + std::to_string(recorded.size())
					+ " surface quads; the repeat is "
					+ std::to_string(testCase.cellsWide) + " columns by "
					+ std::to_string(testCase.levelsHigh) + " rows");

			// Group the recorded quads into rows by their bottom edge.
			std::vector<float> rowBottoms;
			for (auto const& cell : recorded)
				if (std::none_of(rowBottoms.begin(), rowBottoms.end(),
						[&](float bottom) { return near(bottom, cell.bottom); }))
					rowBottoms.push_back(cell.bottom);
			std::sort(rowBottoms.begin(), rowBottoms.end(),
				std::greater<float>());
			require(rowBottoms.size() == testCase.levelsHigh,
				testCase.label + " drew " + std::to_string(rowBottoms.size())
					+ " distinct rows; the repeat is "
					+ std::to_string(testCase.levelsHigh) + " rows high");

			// The lowest row stands on the Sector's floor line. Anchored on
			// the ceiling instead, this is where a short Sector would fail:
			// its bottom row would stop short of the floor.
			require(near(rowBottoms.front(), area.floorLine),
				testCase.label + "'s lowest surface row bottoms at "
					+ std::to_string(rowBottoms.front()) + ", the Sector's floor "
					"line is " + std::to_string(area.floorLine)
					+ ": the repeat must be anchored on the floor line");

			// Rows are contiguous: each one starts where the row below
			// ended, one cell at a time, all the way to the ceiling.
			for (size_t index = 1; index < rowBottoms.size(); ++index)
			{
				float const gap = rowBottoms[index - 1] - rowBottoms[index];
				require(near(gap, area.cellHeight),
					testCase.label + " spaced its rows " + std::to_string(gap)
						+ "px apart; one World cell is "
						+ std::to_string(area.cellHeight) + "px at this zoom");
			}
			// The topmost row reaches the Sector's ceiling: its bottom edge is
			// at or below the ceiling, and one cell below that edge is at or
			// above it, so the rows cover the whole area and the ceiling is
			// where the crop bounds them.
			require(rowBottoms.back() >= area.top - 0.01f
					&& rowBottoms.back() - area.cellHeight <= area.top + 0.01f,
				testCase.label + "'s topmost row spans neither to nor past its "
					"ceiling at " + std::to_string(area.top));

			// Every row samples its tile's floor end at its own bottom edge.
			// That is the same anchor stated in the atlas instead of on the
			// screen: v 1 is the bottom of the tile, and it lands on the
			// floor line for the bottom row and on each row's own bottom for
			// the rows above.
			for (auto const& cell : recorded)
				require(near(cell.v1, 1.0f),
					testCase.label + "'s quad at " + describe(cell)
						+ " stops short of the tile's floor end; the repeat must "
						"put the tile's floor end on the row's bottom edge");

			// The row on the floor line is a whole cell. If the anchor had
			// moved to the ceiling, the short row would be here instead. Only
			// stated for a Sector more than one level high: a single-level Room
			// or Corridor is one row, and that row is its partial top level.
			if (testCase.levelsHigh > 1)
				for (auto const& cell : recorded)
				{
					if (!near(cell.bottom, area.floorLine)) continue;
					require(near(cell.height(), area.cellHeight),
						testCase.label + "'s bottom row is "
							+ std::to_string(cell.height()) + "px tall; only the top "
							"row may be short, and only by the amount the top level "
							"is short");
				}
		}
	}

	// A Room whose top level is a fraction of a full cell. Its top row is
	// anchored on its own bottom edge - one whole cell above the row below
	// it, continuing the grid the lower rows started - and is cropped at
	// the top: it draws that fraction of a cell from that same fraction of
	// its tile, at one artwork pixel to one screen pixel. It is not
	// stretched, and it is not pulled up to the Sector's ceiling.
	void aPartialTopLevelIsCroppedAtItsTopNotStretched(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewSectorAtlas(context);
		pinViewport();
		SurfaceCase testCase{ "room",
			"a Room three cells wide and four levels high with a 0.6 top level",
			3, 4, 0.6f, &roomFixture };
		auto const fixture = builtCase(testCase);
		auto const area = areaOf(*fixture.sector, gUISettings.worldZoom);
		auto const recorded = recordSurfaceCells(fixture.sector,
			fixture.sector->getLayerIndex(), atlas);
		auto const expected = expectedGrid(*fixture.sector, area);
		auto const matched = matchedTo(expected, recorded, testCase.label);
		require(matched.size() == 12,
			testCase.label + " did not draw twelve cells");

		// The full rows below the top give the pixels-per-artwork-texel
		// ratio the top row has to match. Anything else is a squash or a
		// stretch.
		std::vector<RecordedCell> fullRows;
		for (auto const& cell : matched)
			if (near(cell.height(), area.cellHeight)) fullRows.push_back(cell);
		require(fullRows.size() == size_t(testCase.cellsWide)
				* (testCase.levelsHigh - 1),
			testCase.label + " drew " + std::to_string(fullRows.size())
				+ " full-cell rows; three of its four levels are whole cells");
		require(!fullRows.empty(),
			testCase.label + " drew no full-cell row to compare against");
		float const fullRatio = fullRows.front().height()
			/ fullRows.front().sampledTexelHeight();
		require(near(fullRatio, 1.0f),
			testCase.label + " draws a full cell at " + std::to_string(fullRatio)
				+ " screen px per artwork texel; a full cell is one to one");

		// The top row: the cells that are not a whole cell. There is exactly
		// one row of them, one per column.
		std::vector<RecordedCell> shortCells;
		for (auto const& cell : matched)
			if (!near(cell.height(), area.cellHeight)) shortCells.push_back(cell);
		require(shortCells.size() == testCase.cellsWide,
			testCase.label + " has " + std::to_string(shortCells.size())
				+ " short cells; only the top level is a fraction of a cell, so "
				"exactly one row of " + std::to_string(testCase.cellsWide)
				+ " cells may be short");
		for (auto const& cell : shortCells)
			require(near(cell.bottom, shortCells.front().bottom)
					&& near(cell.top, shortCells.front().top),
				testCase.label + "'s short cells are not one row: "
					+ describe(cell) + " against "
					+ describe(shortCells.front()));
		auto const& top = shortCells.front();

		// Anchored on its own bottom edge: one whole cell above the row
		// below it, which is where the grid the lower rows started puts it.
		// Not at the ceiling of a stretched grid, and not one cell short of
		// the floor line because the whole area was re-anchored.
		float const expectedBottom = area.floorLine
			- float(testCase.levelsHigh - 1) * area.cellHeight;
		require(near(top.bottom, expectedBottom),
			testCase.label + "'s top row bottoms at " + std::to_string(top.bottom)
				+ "; anchored on its own bottom edge it should bottom at "
				+ std::to_string(expectedBottom)
				+ ", one whole cell above the row below");
		require(near(top.top, area.top),
			testCase.label + "'s top row tops out at " + std::to_string(top.top)
				+ ", the Sector's ceiling is " + std::to_string(area.top));

		// Cropped, not stretched: the drawn fraction of the cell equals the
		// sampled fraction of the tile, so the pixels per artwork texel are
		// what a full cell gets.
		float const drawnFraction = top.height() / area.cellHeight;
		float const sampledFraction = top.v1 - top.v0;
		require(near(drawnFraction, testCase.topLevelHeight),
			testCase.label + "'s top row draws " + std::to_string(drawnFraction)
				+ " of a cell; its top level is "
				+ std::to_string(testCase.topLevelHeight));
		require(near(sampledFraction, drawnFraction),
			testCase.label + "'s top row samples " + std::to_string(sampledFraction)
				+ " of its tile but draws " + std::to_string(drawnFraction)
				+ " of its cell: the tile was stretched, not cropped");
		require(sampledFraction < 0.99f,
			testCase.label + "'s top row sampled the whole tile, which is what a "
				"stretched partial cell looks like");
		float const topRatio = top.height() / top.sampledTexelHeight();
		require(near(topRatio, fullRatio),
			testCase.label + "'s top row draws " + std::to_string(topRatio)
				+ " screen px per artwork texel against "
				+ std::to_string(fullRatio) + " for a full cell: the partial "
				"level must keep the same scale and lose the top of the tile");

		// The crop is at the top: the tile's floor end still lands on the
		// row's own bottom edge, and what was dropped is the band that
		// would have stood above the Sector's ceiling.
		require(near(top.v1, 1.0f),
			testCase.label + "'s top row stops at v " + std::to_string(top.v1)
				+ "; its tile's floor end must land on the row's bottom edge");
		require(near(top.v0, 1.0f - testCase.topLevelHeight),
			testCase.label + "'s top row starts at v " + std::to_string(top.v0)
				+ "; the top " + std::to_string(1.0f - testCase.topLevelHeight)
				+ " of the tile is what gets dropped");
	}
}

void render_smoke::registerSectorSurfaceDrawContract(
	std::vector<smoke::Check>& checks)
{
	checks.push_back(
		{ "sectorSurfaceDrawContract/everyCellOfANoVariantSectorDrawsItsBaseTile",
			isolated<everyCellOfANoVariantSectorDrawsItsBaseTile> });
	checks.push_back(
		{ "sectorSurfaceDrawContract/theRepeatIsAnchoredOnTheSectorFloorLine",
			isolated<theRepeatIsAnchoredOnTheSectorFloorLine> });
	checks.push_back(
		{ "sectorSurfaceDrawContract/aPartialTopLevelIsCroppedAtItsTopNotStretched",
			isolated<aPartialTopLevelIsCroppedAtItsTopNotStretched> });
}
