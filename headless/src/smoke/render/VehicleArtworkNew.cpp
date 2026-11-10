// The vehicles - the Lift car, the Platform lift and the Shuttle carriage -
// draw from ObjectAtlasNew (#1021).
//
// The Shuttle carriage is where the naming convention earns its keep. A
// carriage is composed from three role tiles - `shuttle-L0`, `shuttle-M0`,
// `shuttle-R0` - and the only difference between a three-cell carriage and
// a four-cell one is how many times the middle gets reused, because the
// end-versus-middle difference is carried by the role rather than by the
// art. Widening a carriage buys cells, never tiles.
//
// The coupling between two carriages keeps a bare name. It is drawn at the
// join, in the one-cell gap that belongs to neither carriage, so it is not
// a cell position and must not read as one.
//
// The Lift car and the Platform lift are both drawn into their own live
// shape: the car fills the cell it occupies and travels with the car up
// the shaft, and the slab is drawn where the slab actually is rather than
// where the shaft was authored.
//
// Everything is asserted on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names. The carriage goes through the real
// renderShuttle pass at widths a World will not author - a two-cell
// carriage is below the authored minimum of three - and the vehicles are
// moved through `setCoordinatedPosition`, the same hook the traversal
// coordinator uses to keep the renderable shape in step with the moving
// thing. Which tile reached the screen is read back off the recorded
// texture coordinates rather than remembered at the call site: the claim
// under test is what was drawn, not what was asked for.
//
// Screen space is top-left origin and the World is drawn with y growing up,
// so a World point (x, y) lands at screen (x * cell width, canvas height -
// y * level height) at zoom 1 with the viewport at the origin.

#include "Checks.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Lift.h"
#include "core/LiftSectorObject.h"
#include "core/LiftTransit.h"
#include "core/Shuttle.h"
#include "core/World.h"

#include "imgui/imgui.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
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

	bool relNear(float a, float b, float tolerance = 0.001f)
	{
		auto const scale = std::max(std::abs(a), std::abs(b));
		return scale == 0.0f || std::abs(a - b) / scale < tolerance;
	}

	std::string point(float x, float y) { return std::to_string(x) + "," + std::to_string(y); }

	float const kCellWidth = float(CORE_CELL_WIDTH_PIXELS);
	float const kLevelHeight = float(CORE_LEVEL_HEIGHT_PIXELS);

	// Sixteen cells by four Levels: room for a Shuttle train of three
	// four-cell carriages end to end, and for a shaft to run three Levels.
	uint32_t const kWorldCellsWide = 16;
	uint32_t const kWorldLevelsHigh = 4;
	float const kCanvasWidth = float(kWorldCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kWorldLevelsHigh) * kLevelHeight;

	// The Platform lift slab's thickness, as the domain authors it.
	float const kSlabThickness = 0.05f;

	struct ScreenRect
	{
		float left{}, top{}, right{}, bottom{};

		float width() const { return right - left; }
		float height() const { return bottom - top; }
	};

	bool sameRect(ScreenRect const& a, ScreenRect const& b)
	{
		return near(a.left, b.left) && near(a.top, b.top)
			&& near(a.right, b.right) && near(a.bottom, b.bottom);
	}

	bool overlaps(ScreenRect const& a, ScreenRect const& b)
	{
		return a.left < b.right - 0.01f && b.left < a.right - 0.01f
			&& a.top < b.bottom - 0.01f && b.top < a.bottom - 0.01f;
	}

	std::string describe(ScreenRect const& r)
	{
		return point(r.left, r.top) + " -> " + point(r.right, r.bottom);
	}

	ScreenRect worldToScreen(float worldLeft, float worldBottom, float width,
		float height)
	{
		return {
			worldLeft * kCellWidth,
			kCanvasHeight - (worldBottom + height) * kLevelHeight,
			(worldLeft + width) * kCellWidth,
			kCanvasHeight - worldBottom * kLevelHeight
		};
	}

	// The screen rectangle a World-unit extent maps to, with the corners
	// normalised. A Platform lift slab is authored with a negative height -
	// it hangs below its anchor - so min and max are not the corners they
	// look like until they are sorted.
	ScreenRect extentToScreen(core::Vector2 a, core::Vector2 b)
	{
		auto const left = std::min(a.x, b.x), right = std::max(a.x, b.x);
		auto const bottom = std::min(a.y, b.y), top = std::max(a.y, b.y);
		return worldToScreen(left, bottom, right - left, top - bottom);
	}

	// ------------------------------------------------------------------
	// The atlas, read and installed the way the application does.
	// ------------------------------------------------------------------

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
		std::string location;
		std::string imageReference;
		AtlasPixelRegions regions;
	};

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

	std::string const kTileset = "resources/textures/objects-new.tileset.yaml";

	struct NewObjectsAtlas
	{
		ObjectTileset authored;
		AtlasPixelRegions regions;
		int width{}, height{};
	};

	NewObjectsAtlas loadNewObjectsAtlas(smoke::Context const& context)
	{
		auto const manifest = YAML::LoadFile(
			context.fixture("resources/Resources.yaml").string());
		auto const set = manifestResource(manifest, "ObjectAtlasNew");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no ObjectAtlasNew ImageSet");

		NewObjectsAtlas atlas;
		atlas.authored = ObjectTileset::load(context.fixture(kTileset));
		atlas.regions = set.regions;
		atlas.width = atlas.authored.width;
		atlas.height = atlas.authored.height;
		return atlas;
	}

	NewObjectsAtlas installNewObjectsAtlas(smoke::Context const& context)
	{
		auto const atlas = loadNewObjectsAtlas(context);
		auto const tiles = installAtlasTiles(atlas.authored.sprites, atlas.regions,
			atlas.width, atlas.height, "ObjectAtlasNew");
		ObjectTileset installed;
		installed.width = atlas.width;
		installed.height = atlas.height;
		installed.sprites = tiles;
		setObjectTileset(std::move(installed), static_cast<TextureId>(1));
		return atlas;
	}

	// ------------------------------------------------------------------
	// Canvas and command-stream inspection
	// ------------------------------------------------------------------

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

	WorldDrawList::ClipRectangle fullCanvas()
	{
		return { { 0, 0 }, { kCanvasWidth, kCanvasHeight } };
	}

	// One ObjectAtlas quad the renderer emitted, tagged with the authored
	// tile it wore.
	struct DrawnQuad
	{
		std::string tile;
		ScreenRect rect{};
		int triangles{ 0 };
	};

	// Half-pixel slack: the sprite helper insets a tile's UVs by half a
	// pixel at each edge so it never samples its neighbour's border.
	float const kUvSlackPixels = 1.5f;

	// Every ObjectAtlas quad in the stream, tagged by matching its recorded
	// texture range back to the authored regions. The tiles sit in disjoint
	// cells, so one range lands in at most one tile.
	std::vector<DrawnQuad> objectQuads(WorldDrawList const& drawing,
		ObjectTileset const& atlas)
	{
		auto const w = float(atlas.width), h = float(atlas.height);
		std::vector<DrawnQuad> quads;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
				continue;

			float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
			for (auto const& uv : triangle->texcoords)
			{
				u0 = std::min(u0, uv.x * w); u1 = std::max(u1, uv.x * w);
				v0 = std::min(v0, uv.y * h); v1 = std::max(v1, uv.y * h);
			}
			std::string tile;
			for (auto const& [name, art] : atlas.sprites)
			{
				auto const& r = art.region;
				if (u0 >= float(r.x) - kUvSlackPixels
						&& u1 <= float(r.x + r.width) + kUvSlackPixels
						&& v0 >= float(r.y) - kUvSlackPixels
						&& v1 <= float(r.y + r.height) + kUvSlackPixels)
				{
					tile = name;
					break;
				}
			}

			ScreenRect rect{};
			rect.left = rect.top = 1e9f;
			rect.right = rect.bottom = -1e9f;
			for (auto const& p : triangle->positions)
			{
				rect.left = std::min(rect.left, p.x);
				rect.top = std::min(rect.top, p.y);
				rect.right = std::max(rect.right, p.x);
				rect.bottom = std::max(rect.bottom, p.y);
			}

			// The two triangles of one quad share a tile and a destination,
			// so they fold together. Two cells wearing the same tile sit at
			// different destinations and stay apart, which is how a reused
			// middle tile is counted.
			bool merged = false;
			for (auto& other : quads)
			{
				if (other.tile != tile || !sameRect(other.rect, rect)) continue;
				++other.triangles;
				merged = true;
				break;
			}
			if (!merged) quads.push_back({ tile, rect, 1 });
		}
		return quads;
	}

	std::vector<DrawnQuad> byTile(std::vector<DrawnQuad> const& quads,
		std::string const& tile)
	{
		std::vector<DrawnQuad> kept;
		for (auto const& q : quads)
			if (q.tile == tile) kept.push_back(q);
		return kept;
	}

	std::string tileList(std::vector<DrawnQuad> const& quads)
	{
		std::string out;
		for (auto const& q : quads)
			out += (out.empty() ? "" : ", ") + q.tile + "[" + describe(q.rect) + "]";
		return out.empty() ? "<none>" : out;
	}

	// ------------------------------------------------------------------
	// Fixtures
	// ------------------------------------------------------------------

	// A bare Shuttle hauling `numCars` carriages of `carWidth` cells each.
	// Built directly rather than through the World because a two-cell
	// carriage is below the authored minimum of three, and because the
	// pass under test is the carriage composition itself: the train's
	// transport width is set to the whole train - carriages plus the
	// coupling gap each - so the shape the pass lays the carriages out
	// against is the shape a real Shuttle carries.
	std::shared_ptr<core::Shuttle> makeCarriage(uint32_t carWidth, uint32_t numCars)
	{
		auto const trainWidth = float(carWidth * numCars + (numCars - 1));
		return std::make_shared<core::Shuttle>(0, 0, 0.0f, 0.0f, trainWidth,
			CORE_SHUTTLE_CAR_HEIGHT, numCars, carWidth,
			std::vector<uint32_t>{ 0, 20 });
	}

	struct PlatformLiftScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::LiftSectorObject> object;
		std::shared_ptr<core::Lift> lift;
	};

	PlatformLiftScene buildPlatformLiftScene()
	{
		auto world = std::make_shared<core::World>("Platform lift artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		auto const room = world->addRoom("Shaft room", 0, 0, 0, kWorldCellsWide,
			kWorldLevelsHigh);
		// Each stop the slab serves needs standing room in the shaft column at
		// its Level, which is what the domain checks for when it accepts the
		// stop.
		for (uint32_t level : { 1u, 3u })
			for (uint32_t x = 0; x < kWorldCellsWide; ++x)
				world->addSectorWalkway(room, level, x);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 3 };
		options.capacity = 2;
		auto const made = world->addSectorPlatformLift(room, 0, 4, options);
		world->finishBuild();

		PlatformLiftScene scene;
		scene.world = world;
		scene.object = std::static_pointer_cast<const core::LiftSectorObject>(
			made.lift.sector->getObject(made.lift.index));
		require(bool(scene.object), "The Platform lift fixture has no SectorObject");
		scene.lift = scene.object->getLift();
		require(bool(scene.lift), "The Platform lift fixture has no Lift");
		return scene;
	}

	struct LiftScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::Lift> lift;
	};

	// A one-cell enclosed Lift shaft with stops at the bottom and top
	// Levels, so the car has somewhere to travel to.
	LiftScene buildLiftScene()
	{
		auto world = std::make_shared<core::World>("Lift artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addCorridor(0, 0, kWorldCellsWide);
		world->addCorridor(3, 0, kWorldCellsWide);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.stopOffsets = { 0, 3 };
		options.capacity = 2;
		auto const made = world->addLift(1, 0, 4, options);
		world->finishBuild();

		LiftScene scene;
		scene.world = world;
		scene.lift = std::static_pointer_cast<const core::LiftTransit>(
			made.lift.sector)->getLift();
		require(bool(scene.lift), "The Lift fixture has no Lift");
		return scene;
	}

	ScreenRect liveShapeOnScreen(std::shared_ptr<const core::Object> const& object)
	{
		core::Vector2 lo, hi;
		object->getCurrentShape(lo, hi);
		return extentToScreen(lo, hi);
	}

	// ------------------------------------------------------------------
	// The six vehicle tiles: authored sizes, tintability, generated pair.
	// ------------------------------------------------------------------
	void vehicleTilesAreRegisteredInObjectAtlasNew(smoke::Context const& context)
	{
		auto const atlas = loadNewObjectsAtlas(context);

		struct TileExpectation
		{
			char const* name;
			float drawnWidth, drawnHeight;
			int contentWidth, contentHeight;
		};

		// Each drawn size is the vehicle's own extent for the regular
		// one-cell case: a Lift car is the cell less CORE_LIFT_CAR_BORDER
		// on each side and CORE_LIFT_CAR_HEIGHT tall; a carriage cell is
		// one cell across CORE_SHUTTLE_CAR_HEIGHT; the Platform lift slab
		// is one cell across its 0.05 thickness. At the 128 x 320 authored
		// grid those come to 115 x 176, 128 x 176 and 128 x 16.
		std::vector<TileExpectation> const kTiles{
			{ "lift-car", 0.9f, 0.55f, 115, 176 },
			{ "platform-lift", 1.0f, 0.05f, 128, 16 },
			{ "shuttle-L0", 1.0f, 0.55f, 128, 176 },
			{ "shuttle-M0", 1.0f, 0.55f, 128, 176 },
			{ "shuttle-R0", 1.0f, 0.55f, 128, 176 },
			{ "shuttle-connector", 1.0f, 0.55f, 128, 176 },
		};

		for (auto const& tile : kTiles)
		{
			auto const found = atlas.authored.sprites.find(tile.name);
			require(found != atlas.authored.sprites.end(),
				std::string("ObjectAtlasNew registers no '") + tile.name
				+ "' tile");
			auto const& art = found->second;
			require(relNear(art.drawnSize.x, tile.drawnWidth)
					&& relNear(art.drawnSize.y, tile.drawnHeight),
				std::string("'") + tile.name + "' draws at "
				+ point(art.drawnSize.x, art.drawnSize.y) + ", expected "
				+ point(tile.drawnWidth, tile.drawnHeight));
			require(art.region.width == tile.contentWidth
					&& art.region.height == tile.contentHeight,
				std::string("'") + tile.name + "' content is "
				+ std::to_string(art.region.width) + "x"
				+ std::to_string(art.region.height) + ", expected "
				+ std::to_string(tile.contentWidth) + "x"
				+ std::to_string(tile.contentHeight));
			// None of the vehicles takes a tint. Their colour is the
			// vehicle's own, and letting a Layer tint multiply into a
			// carriage would make one train read in as many.
			require(!art.tintable,
				std::string("'") + tile.name
				+ "' is a vehicle and must not take a Layer tint");
			require(near(art.offset.x, 0.0f) && near(art.offset.y, 0.0f),
				std::string("'") + tile.name
				+ "' is drawn at the vehicle's own anchor and declares no offset");

			auto const region = atlas.regions.find(tile.name);
			require(region != atlas.regions.end(),
				std::string("Authored tile '") + tile.name
				+ "' has no generated ImageSet entry");
			require(region->second.x == art.region.x
					&& region->second.y == art.region.y
					&& region->second.width == art.region.width
					&& region->second.height == art.region.height,
				std::string("Generated ImageSet entry for '") + tile.name
				+ "' drifted from the authored tileset");
		}

		// The three carriage roles are three different regions. The role is
		// carried by the artwork, so two roles sharing a region would make
		// the end-versus-middle difference invisible on screen.
		auto const regionOf = [&](std::string const& name)
		{
			return atlas.authored.sprites.at(name).region;
		};
		auto const left = regionOf("shuttle-L0");
		auto const middle = regionOf("shuttle-M0");
		auto const right = regionOf("shuttle-R0");
		require((left.x != middle.x || left.y != middle.y)
				&& (middle.x != right.x || middle.y != right.y)
				&& (left.x != right.x || left.y != right.y),
			"The carriage role tiles share a region, so a carriage would draw "
			"the same end at both ends");

		// The connector is a bare name with no role variants. A
		// `shuttle-connector-L0` would claim the coupling was a cell
		// position on a carriage, which is the one thing it is not.
		for (auto const& [name, art] : atlas.authored.sprites)
			require(name.rfind("shuttle-connector-", 0) != 0,
				"The carriage connector has a role variant ('" + name
				+ "'), which reads as a cell position on a carriage");

		// The old left/middle/right role tiles are gone from this set: the
		// convention replaced them rather than sitting alongside them.
		for (auto const* legacy : { "shuttle-car-left", "shuttle-car-middle",
			"shuttle-car-right", "shuttle-car-connector" })
			require(atlas.authored.sprites.count(legacy) == 0,
				std::string("ObjectAtlasNew still carries the pre-convention tile '")
				+ legacy + "'");
	}

	// ------------------------------------------------------------------
	// Two, three and four cells wide, reusing the middle tile.
	// ------------------------------------------------------------------
	void carriageComposesFromRoleTilesAtTwoThreeAndFourCells(
		smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const carriageHeight = CORE_SHUTTLE_CAR_HEIGHT * kLevelHeight;
		auto const& middleArt = atlas.authored.sprites.at("shuttle-M0");

		for (uint32_t carWidth : { 2u, 3u, 4u })
		{
			WorldDrawList drawing{ fullCanvas() };
			renderShuttle(makeCarriage(carWidth, 1), 0, LayerRenderStyle::Solid,
				false, &drawing);

			std::vector<DrawnQuad> cells;
			for (auto const& q : objectQuads(drawing, atlas.authored))
				if (q.tile == "shuttle-L0" || q.tile == "shuttle-M0"
					|| q.tile == "shuttle-R0")
					cells.push_back(q);
			std::sort(cells.begin(), cells.end(), [](auto const& a, auto const& b)
			{ return a.rect.left < b.rect.left; });

			auto const width = std::to_string(carWidth);
			require(cells.size() == carWidth,
				"A " + width + "-cell carriage drew " + std::to_string(cells.size())
				+ " cells, expected " + width + ": " + tileList(cells));

			require(cells.front().tile == "shuttle-L0",
				"The left end of a " + width + "-cell carriage wore '"
				+ cells.front().tile + "', expected 'shuttle-L0'");
			require(cells.back().tile == "shuttle-R0",
				"The right end of a " + width + "-cell carriage wore '"
				+ cells.back().tile + "', expected 'shuttle-R0'");

			uint32_t middles = 0;
			for (size_t i = 1; i + 1 < cells.size(); ++i)
			{
				require(cells[i].tile == "shuttle-M0",
					"Cell " + std::to_string(i) + " of a " + width
					+ "-cell carriage wore '" + cells[i].tile
					+ "', expected the middle tile");
				++middles;
			}
			// The whole point of the convention: a wider carriage reuses
			// the same middle rather than needing new art per width.
			require(middles == carWidth - 2,
				"A " + width + "-cell carriage used the middle tile "
				+ std::to_string(middles) + " times, expected "
				+ std::to_string(carWidth - 2));

			for (auto const& cell : cells)
			{
				require(relNear(cell.rect.width(), kCellWidth),
					"A carriage cell is " + std::to_string(cell.rect.width())
					+ "px wide, expected one cell of " + std::to_string(kCellWidth));
				require(relNear(cell.rect.height(), carriageHeight),
					"A carriage cell is " + std::to_string(cell.rect.height())
					+ "px tall, expected the carriage height of "
					+ std::to_string(carriageHeight));
			}

			// The declared drawn size is the carriage cell's own extent, so
			// the authored semantics and the rendered geometry agree rather
			// than one of them being decoration.
			require(relNear(middleArt.drawnSize.x * kCellWidth, kCellWidth)
					&& relNear(middleArt.drawnSize.y * kLevelHeight, carriageHeight),
				"The middle tile's declared drawn size is not the carriage cell's "
				"own extent");

			for (size_t i = 1; i < cells.size(); ++i)
				require(near(cells[i].rect.left, cells[i - 1].rect.right),
					"Carriage cells " + std::to_string(i - 1) + " and "
					+ std::to_string(i) + " are not laid end to end: "
					+ describe(cells[i - 1].rect) + " then "
					+ describe(cells[i].rect));

			// One carriage has no join to couple, so it draws no connector.
			require(byTile(objectQuads(drawing, atlas.authored),
					"shuttle-connector").empty(),
				"A single-carriage Shuttle drew a connector with nothing to "
				"connect");
		}
	}

	// ------------------------------------------------------------------
	// The connector sits at the join, not at a cell position.
	// ------------------------------------------------------------------
	void connectorSitsAtTheSeamBetweenCarriages(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		uint32_t const carWidth = 3;
		for (uint32_t numCars : { 1u, 2u, 3u })
		{
			WorldDrawList drawing{ fullCanvas() };
			renderShuttle(makeCarriage(carWidth, numCars), 0,
				LayerRenderStyle::Solid, false, &drawing);

			auto const quads = objectQuads(drawing, atlas.authored);
			auto connectors = byTile(quads, "shuttle-connector");
			std::sort(connectors.begin(), connectors.end(),
				[](auto const& a, auto const& b)
				{ return a.rect.left < b.rect.left; });

			std::vector<ScreenRect> carriageCells;
			for (auto const& q : quads)
				if (q.tile == "shuttle-L0" || q.tile == "shuttle-M0"
					|| q.tile == "shuttle-R0")
					carriageCells.push_back(q.rect);

			require(connectors.size() == numCars - 1,
				std::to_string(numCars) + " carriages drew "
				+ std::to_string(connectors.size()) + " connectors, expected "
				+ std::to_string(numCars - 1));

			for (size_t join = 0; join < connectors.size(); ++join)
			{
				// Carriage `join` occupies world cells
				// [join * (carWidth + 1), +carWidth). The coupling is the
				// one cell after it, which is neither carriage's.
				auto const gapLeft = float(join * (carWidth + 1) + carWidth);
				require(near(connectors[join].rect.left, gapLeft * kCellWidth),
					"The connector after carriage " + std::to_string(join)
					+ " starts at " + std::to_string(connectors[join].rect.left)
					+ "px, expected the seam at "
					+ std::to_string(gapLeft * kCellWidth));
				require(relNear(connectors[join].rect.width(), kCellWidth),
					"The connector is " + std::to_string(connectors[join].rect.width())
					+ "px wide, expected the one-cell gap of "
					+ std::to_string(kCellWidth));
				require(relNear(connectors[join].rect.height(),
						CORE_SHUTTLE_CAR_HEIGHT * kLevelHeight),
					"The connector does not span the carriage height");

				// It is at the join and nowhere else: it shares no pixel with
				// a carriage cell, and it is not lined up on one.
				for (size_t cell = 0; cell < carriageCells.size(); ++cell)
				{
					require(!overlaps(carriageCells[cell], connectors[join].rect),
						"The connector overlaps carriage cell " + std::to_string(cell)
						+ ", so it is drawn on the carriage rather than at the seam");
					require(!near(carriageCells[cell].left, connectors[join].rect.left),
						"The connector is lined up on a carriage cell position instead "
						"of in the gap between carriages");
				}
			}
		}
	}

	// ------------------------------------------------------------------
	// The Platform lift sprite follows the slab, not the shaft.
	// ------------------------------------------------------------------
	void platformLiftSpriteFollowsTheLiveSlab(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();
		auto const scene = buildPlatformLiftScene();

		core::Vector2 shaft0, shaft1;
		scene.object->getBounds(shaft0, shaft1);
		auto const shaft = extentToScreen(shaft0, shaft1);
		require(shaft.height() > kSlabThickness * kLevelHeight * 2.0f,
			"The Platform lift shaft fixture is too short to tell a slab from "
			"the shaft it runs in");

		std::vector<ScreenRect> seen;
		for (float position : { 0.0f, 1.5f, 3.0f })
		{
			scene.lift->setCoordinatedPosition(position);
			WorldDrawList drawing{ fullCanvas() };
			renderPlatformLift(scene.object, 0, LayerRenderStyle::Solid, false,
				&drawing);

			auto const quads = objectQuads(drawing, atlas.authored);
			require(quads.size() == 1,
				"The Platform lift drew " + std::to_string(quads.size())
				+ " sprites at position " + std::to_string(position)
				+ ", expected one: " + tileList(quads));
			require(quads[0].tile == "platform-lift",
				"The Platform lift wore '" + quads[0].tile
				+ "', expected 'platform-lift'");

			auto const slab = liveShapeOnScreen(scene.lift);
			require(sameRect(quads[0].rect, slab),
				"The Platform lift sprite is at " + describe(quads[0].rect)
				+ " but the live slab is at " + describe(slab)
				+ ", so the sprite does not follow the slab");
			require(!sameRect(quads[0].rect, shaft),
				"The Platform lift sprite was drawn at the shaft bounds, not the "
				"slab's");
			require(relNear(quads[0].rect.height(), kSlabThickness * kLevelHeight),
				"The Platform lift sprite is " + std::to_string(quads[0].rect.height())
				+ "px tall, expected the slab's "
				+ std::to_string(kSlabThickness * kLevelHeight));

			seen.push_back(quads[0].rect);
		}

		// The slab actually moved between the renders, so the checks above
		// could not have been satisfied by one frozen rectangle.
		require(!sameRect(seen.front(), seen.back()),
			"The Platform lift sprite did not move as the slab was driven up the "
			"shaft, so nothing proved it follows the slab");
	}

	// ------------------------------------------------------------------
	// The Lift car fills its cell and tracks the car through the shaft.
	// ------------------------------------------------------------------
	void liftCarFillsItsCellAndTracksTheCar(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();
		auto const scene = buildLiftScene();
		auto const& art = atlas.authored.sprites.at("lift-car");

		std::vector<ScreenRect> seen;
		for (float position : { 0.0f, 1.5f, 3.0f })
		{
			scene.lift->setCoordinatedPosition(position);
			WorldDrawList drawing{ fullCanvas() };
			renderLift(scene.lift, 0, LayerRenderStyle::Solid, false, &drawing);

			auto const quads = objectQuads(drawing, atlas.authored);
			require(quads.size() == 1,
				"The Lift drew " + std::to_string(quads.size())
				+ " sprites at position " + std::to_string(position)
				+ ", expected one: " + tileList(quads));
			require(quads[0].tile == "lift-car",
				"The Lift car wore '" + quads[0].tile + "', expected 'lift-car'");

			auto const car = liveShapeOnScreen(scene.lift);
			require(sameRect(quads[0].rect, car),
				"The Lift car sprite is at " + describe(quads[0].rect)
				+ " but the live car is at " + describe(car)
				+ ", so the sprite does not track the car");

			// Fills its cell: the car spans the cell width less the
			// CORE_LIFT_CAR_BORDER it is inset by on each side, and its
			// full car height, and the declared drawn size is that same
			// extent rather than a decoration beside it.
			auto const ownWidth = (1.0f - 2.0f * CORE_LIFT_CAR_BORDER) * kCellWidth;
			require(relNear(quads[0].rect.width(), ownWidth),
				"The Lift car sprite is " + std::to_string(quads[0].rect.width())
				+ "px wide, expected the car's own " + std::to_string(ownWidth)
				+ " across its cell");
			require(relNear(quads[0].rect.height(),
					CORE_LIFT_CAR_HEIGHT * kLevelHeight),
				"The Lift car sprite is " + std::to_string(quads[0].rect.height())
				+ "px tall, expected the car's "
				+ std::to_string(CORE_LIFT_CAR_HEIGHT * kLevelHeight));
			require(relNear(art.drawnSize.x * kCellWidth, quads[0].rect.width())
					&& relNear(art.drawnSize.y * kLevelHeight, quads[0].rect.height()),
				"The 'lift-car' tile's declared drawn size is not the extent the "
				"car is actually drawn at");

			seen.push_back(quads[0].rect);
		}

		require(!sameRect(seen.front(), seen.back()),
			"The Lift car sprite stayed put while the car was driven up the shaft, "
			"so nothing proved it tracks the car");
	}

	// ------------------------------------------------------------------
	// The role names are preferred; a set without them keeps its own art.
	// ------------------------------------------------------------------
	void aSetWithoutRoleTilesStillDrawsItsOwnCarriageArt(
		smoke::Context const& /* context */)
	{
		prepareCanvas();

		// A pre-convention set: the old left/middle/right/connector names
		// and nothing else. It must still compose a carriage, because
		// adopting the convention is an upgrade an older sheet survives
		// rather than a break it takes.
		ObjectTileset legacy;
		legacy.width = 500;
		legacy.height = 100;
		legacy.sprites.emplace("shuttle-car-left", ObjectSprite{ { 0, 0, 50, 100 }, false });
		legacy.sprites.emplace("shuttle-car-middle", ObjectSprite{ { 100, 0, 50, 100 }, false });
		legacy.sprites.emplace("shuttle-car-right", ObjectSprite{ { 200, 0, 50, 100 }, false });
		legacy.sprites.emplace("shuttle-car-connector", ObjectSprite{ { 300, 0, 50, 100 }, false });
		setObjectTileset(legacy, static_cast<TextureId>(1));

		WorldDrawList drawing{ fullCanvas() };
		renderShuttle(makeCarriage(3, 2), 0, LayerRenderStyle::Solid, false,
			&drawing);
		auto const quads = objectQuads(drawing, legacy);
		std::vector<DrawnQuad> cells;
		for (auto const& q : quads)
			if (q.tile != "shuttle-car-connector") cells.push_back(q);
		std::sort(cells.begin(), cells.end(), [](auto const& a, auto const& b)
		{ return a.rect.left < b.rect.left; });

		require(cells.size() == 6,
			"A pre-convention set composed " + std::to_string(cells.size())
			+ " carriage cells for two three-cell carriages, expected 6: "
			+ tileList(quads));
		require(cells[0].tile == "shuttle-car-left"
				&& cells[1].tile == "shuttle-car-middle"
				&& cells[2].tile == "shuttle-car-right",
			"A pre-convention set did not fall back to its own left, middle and "
			"right carriage art: " + tileList(cells));
		require(byTile(quads, "shuttle-car-connector").size() == 1,
			"A pre-convention set drew no coupling between its two carriages");

		// A set with neither the role tiles nor the old ones draws no
		// carriage artwork at all, which is the flat-fill path rather than
		// a crash or a half-composed train.
		ObjectTileset bare;
		bare.width = 100;
		bare.height = 100;
		bare.sprites.emplace("door", ObjectSprite{ { 0, 0, 50, 100 }, false });
		setObjectTileset(std::move(bare), static_cast<TextureId>(1));

		WorldDrawList unstyled{ fullCanvas() };
		renderShuttle(makeCarriage(3, 2), 0, LayerRenderStyle::Solid, false,
			&unstyled);
		require(objectQuads(unstyled, ObjectTileset{}).empty(),
			"A set with no carriage tiles at all still emitted carriage "
			"artwork instead of falling back to the flat fill");
	}
}

void render_smoke::registerVehicleArtworkNew(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "vehicleArtworkNew/vehicleTilesAreRegistered",
		isolated<vehicleTilesAreRegisteredInObjectAtlasNew> });
	checks.push_back({ "vehicleArtworkNew/carriageComposesAtTwoThreeAndFourCellsWide",
		isolated<carriageComposesFromRoleTilesAtTwoThreeAndFourCells> });
	checks.push_back({ "vehicleArtworkNew/connectorSitsAtTheSeamNotACellPosition",
		isolated<connectorSitsAtTheSeamBetweenCarriages> });
	checks.push_back({ "vehicleArtworkNew/platformLiftSpriteFollowsTheLiveSlab",
		isolated<platformLiftSpriteFollowsTheLiveSlab> });
	checks.push_back({ "vehicleArtworkNew/liftCarFillsItsCellAndTracksTheCar",
		isolated<liftCarFillsItsCellAndTracksTheCar> });
	checks.push_back({ "vehicleArtworkNew/aSetWithoutRoleTilesKeepsItsOwnCarriageArt",
		isolated<aSetWithoutRoleTilesStillDrawsItsOwnCarriageArt> });
}
