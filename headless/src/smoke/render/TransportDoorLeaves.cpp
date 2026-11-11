// A transport's landing Door draws its own leaf, not the leaf of the wall
// it happens to be set in (#1057).
//
// Before this, every Door drew `door-L0`/`door-R0` and a Lift landing read
// as an ordinary doorway with a shaft behind it. The ticket gives each
// transport family its own leaves - `door-lift-L0`/`door-lift-R0` and
// `door-shuttle-L0` - and moves the choice of family off the wall and
// onto the thing the Door *does*: `Door::isLiftOwned()` and
// `Door::isShuttleOwned()`. The wall stops being an input at all, which
// is what lets the same Lift landing read the same whether it faces a
// Corridor or a Room.
//
// The Shuttle family deliberately has no right-hand leaf. A Shuttle hatch
// is authored one cell wide, so the hatch tile stands in for both roles
// rather than the renderer reaching across families for art nobody drew.
// A check that the family has no `door-shuttle-R0` is therefore part of
// the artwork contract, not an omission.
//
// A two-cell-wide Lift landing joins its two leaf cells across their
// shared edge: the left cell wears `door-lift-L0`, the right wears
// `door-lift-R0`, they meet exactly, and together they span two cells.
// That is the difference between one joined two-cell leaf and one tile
// stretched over two cells, and it is asserted on the recorded quads
// rather than on the tile names alone.
//
// Ordinary Doors are the control case. They draw the ordinary family and
// nothing from a transport family, so the new selection cannot have
// quietly widened what counts as a transport landing.
//
// Everything is asserted on the recorded WorldDrawList command vector,
// the stable CPU seam ADR 0010 names. Which tile reached the screen is
// read back off the recorded texture coordinates rather than remembered
// at the call site: the claim under test is what was drawn, not what was
// asked for. And because a missing tile falls back to a primitive fill,
// the same renders are repeated with the Objects atlas pulled out, which
// is what proves the leaves really came from the atlas rather than from
// the fallback that would have hidden an absent tile.

#include "Checks.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/World.h"

#include "imgui/imgui.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
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

	std::string point(float x, float y) { return std::to_string(x) + "," + std::to_string(y); }

	float const kCellWidth = float(CORE_CELL_WIDTH_PIXELS);
	float const kLevelHeight = float(CORE_LEVEL_HEIGHT_PIXELS);

	uint32_t const kWorldCellsWide = 16;
	uint32_t const kWorldLevelsHigh = 4;
	float const kCanvasWidth = float(kWorldCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kWorldLevelsHigh) * kLevelHeight;

	// The colour renderDoorLeaves() fills a leaf with when the tile it
	// asked the Objects atlas for is not there. A leaf found in this
	// colour is a leaf with no artwork behind it.
	PackedColourValue const kFallbackLeafColour = packColour(64, 192, 255);

	// The three leaf families, by what owns the Door that wears them.
	char const* const kOrdinaryFamily[] = { "door-L0", "door-R0" };
	char const* const kLiftFamily[] = { "door-lift-L0", "door-lift-R0" };
	char const* const kShuttleFamily[] = { "door-shuttle-L0" };

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

	std::string describe(ScreenRect const& r)
	{
		return point(r.left, r.top) + " -> " + point(r.right, r.bottom);
	}

	// ------------------------------------------------------------------
	// The Objects atlas, read and installed the way the application does.
	// ------------------------------------------------------------------

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
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
		auto const set = manifestResource(manifest, "ObjectAtlas");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no ObjectAtlas ImageSet");

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
			atlas.width, atlas.height, "ObjectAtlas");
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

	struct DrawnQuad
	{
		std::string tile;
		ScreenRect rect{};
	};

	// Half-pixel slack: the sprite helper insets a tile's UVs by half a
	// pixel at each edge so it never samples its neighbour's border.
	float const kUvSlackPixels = 1.5f;

	// Every ObjectAtlas quad the pass emitted, tagged by matching its
	// recorded texture range back to the authored regions. The tiles sit
	// in disjoint cells, so one range lands in at most one tile.
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
			// different destinations and stay apart.
			bool merged = false;
			for (auto& other : quads)
			{
				if (other.tile != tile || !sameRect(other.rect, rect)) continue;
				merged = true;
				break;
			}
			if (!merged) quads.push_back({ tile, rect });
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

	// Every ObjectAtlas quad in the stream, whatever it matched.
	std::vector<DrawnQuad> atlasQuads(WorldDrawList const& drawing)
	{
		std::vector<DrawnQuad> quads;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
				continue;
			quads.push_back({ "<atlas>", {} });
		}
		return quads;
	}

	// The leaf cells that were painted with the no-artwork fallback.
	std::vector<ScreenRect> fallbackLeafFills(WorldDrawList const& drawing)
	{
		std::vector<ScreenRect> fills;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::None)
				continue;
			if (triangle->colour != kFallbackLeafColour) continue;
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
			bool merged = false;
			for (auto const& other : fills)
				if (sameRect(other, rect)) { merged = true; break; }
			if (!merged) fills.push_back(rect);
		}
		return fills;
	}

	// ------------------------------------------------------------------
	// Fixtures
	// ------------------------------------------------------------------

	struct LandingScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::Door> door;
		uint32_t layer{ 0 };
		std::string what;
	};

	std::shared_ptr<core::Door> doorAt(core::World::CreateDoorResult const& made)
	{
		auto const object = made.door.sector->getObject(made.door.index);
		require(bool(object), "A landing Door has no SectorObject");
		auto const doorObject = std::dynamic_pointer_cast<core::DoorSectorObject>(object);
		require(bool(doorObject), "A landing Door is not a Door SectorObject");
		auto const door = doorObject->getDoor();
		require(bool(door), "A landing Door SectorObject has no Door");
		return door;
	}

	// A Lift whose landings face the named kind of location. The kind is
	// the point: the wall the Door is set in is what used to decide the
	// leaf, and it must not decide it now.
	LandingScene buildLiftLanding(std::string const& frontKind, uint32_t shaftWidth)
	{
		auto world = std::make_shared<core::World>("Lift landing artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		// Both landings face the Layer directly in front of the shaft, one
		// at the bottom Level and one at the top; the stop offset is a
		// Level offset, not a Layer offset.
		if (frontKind == "Corridor")
		{
			world->addCorridor(0, 0, kWorldCellsWide);
			world->addCorridor(3, 0, kWorldCellsWide);
		}
		else
		{
			world->addRoom("Lower landing", 0, 0, 0, kWorldCellsWide, 1);
			world->addRoom("Upper landing", 0, 3, 0, kWorldCellsWide, 1);
		}
		core::World::CreateLiftOptions options;
		options.cellsWide = shaftWidth;
		options.stopOffsets = { 0, 3 };
		options.capacity = 2;
		auto const made = world->addLift(1, 0, 4, options);
		world->finishBuild();
		require(made.doors.size() == 2, "The Lift fixture made "
			+ std::to_string(made.doors.size()) + " landing Doors, expected 2");

		LandingScene scene;
		scene.world = world;
		scene.door = doorAt(made.doors[0]);
		scene.layer = scene.door->getFrontLayer();
		scene.what = "a " + std::to_string(shaftWidth) + "-cell Lift landing facing "
			+ frontKind;
		return scene;
	}

	// A Shuttle whose carriage doors land in a Room. The hatch is authored
	// one cell wide whatever the carriage width, which is why the family
	// carries no right-hand leaf.
	LandingScene buildShuttleLanding()
	{
		auto world = std::make_shared<core::World>("Shuttle hatch artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addRoom("Landing", 0, 0, 0, kWorldCellsWide, 1);
		core::World::CreateShuttleOptions options;
		options.numCars = 1;
		options.carWidth = 3;
		options.stopOffsets = { 0, 5 };
		options.initialStop = 0;
		options.capacity = 2;
		auto const made = world->addShuttle(1, 0, 0, 10, options);
		world->finishBuild();
		require(!made.doors.empty(), "The Shuttle fixture made no landing Doors");

		LandingScene scene;
		scene.world = world;
		scene.door = doorAt(made.doors[0]);
		scene.layer = scene.door->getFrontLayer();
		scene.what = "a Shuttle hatch landing";
		return scene;
	}

	// Two Rooms on adjacent Layers with one ordinary Door between them.
	LandingScene buildOrdinaryLanding(uint32_t width)
	{
		auto world = std::make_shared<core::World>("Ordinary Door artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addRoom("Front", 0, 0, 0, kWorldCellsWide, 1);
		world->addRoom("Back", 1, 0, 0, kWorldCellsWide, 1);
		core::World::CreateDoorOptions options;
		options.width = width;
		auto const made = world->addSectorDoor(0, 0, 3, options);
		world->finishBuild();

		LandingScene scene;
		scene.world = world;
		scene.door = doorAt(made);
		scene.layer = scene.door->getFrontLayer();
		scene.what = "an ordinary " + std::to_string(width) + "-cell Door";
		return scene;
	}

	WorldDrawList renderLanding(LandingScene const& scene)
	{
		WorldDrawList drawing{ fullCanvas() };
		RenderWorldScope scope(scene.world);
		renderDoor(scene.door, scene.layer, LayerRenderStyle::Solid, false, &drawing);
		return drawing;
	}

	// Asserts a rendered landing wears exactly the named family and carries
	// nothing from any other leaf family.
	void wearsOnly(std::vector<DrawnQuad> const& quads,
		std::vector<std::string> const& family,
		std::vector<std::string> const& forbidden,
		std::string const& what)
	{
		for (auto const& tile : family)
			require(!byTile(quads, tile).empty(), what + " drew no '" + tile
				+ "' leaf cell; it drew " + tileList(quads));
		for (auto const& tile : forbidden)
			require(byTile(quads, tile).empty(), what + " drew a '" + tile
				+ "' leaf cell from another family; it drew " + tileList(quads));
	}

	std::vector<std::string> stringsOf(char const* const* begin, size_t count)
	{
		return std::vector<std::string>(begin, begin + count);
	}

	// ------------------------------------------------------------------
	// A Lift landing resolves to the lift leaf family by transport
	// ownership, not by the wall it sits in.
	// ------------------------------------------------------------------
	void liftLandingResolvesByOwnershipNotByItsWall(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		// The same Lift landing set in a Corridor wall and in a Room wall.
		// An ordinary Door in either of those walls draws the ordinary
		// family; this one draws the lift family in both, because the Lift
		// owns it and the wall gets no vote.
		for (auto const* kind : { "Corridor", "Room" })
		{
			auto const scene = buildLiftLanding(kind, 1);
			require(scene.door->isLiftOwned(),
				"A Lift landing facing a " + std::string(kind)
				+ " does not report itself Lift-owned");
			require(!scene.door->isShuttleOwned(),
				"A Lift landing facing a " + std::string(kind)
				+ " reports itself Shuttle-owned");

			auto const drawing = renderLanding(scene);
			auto const quads = objectQuads(drawing, atlas.authored);
			wearsOnly(quads, {"door-lift-L0"}, ordinary, scene.what);
			require(fallbackLeafFills(drawing).empty(), scene.what
				+ " fell back to the primitive leaf fill with the Objects atlas "
				"installed, so its leaf has no artwork behind it");
		}
	}

	// ------------------------------------------------------------------
	// A two-cell-wide Lift landing joins its two leaf cells across their
	// shared edge.
	// ------------------------------------------------------------------
	void twoWideLiftLandingJoinsItsTwoLeafCells(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const scene = buildLiftLanding("Corridor", 2);
		auto const drawing = renderLanding(scene);
		auto const quads = objectQuads(drawing, atlas.authored);

		auto const left = byTile(quads, "door-lift-L0");
		auto const right = byTile(quads, "door-lift-R0");
		require(left.size() == 1, scene.what + " drew " + std::to_string(left.size())
			+ " left-hand lift leaf cells, expected the one cell of a joined pair; it drew "
			+ tileList(quads));
		require(right.size() == 1, scene.what + " drew " + std::to_string(right.size())
			+ " right-hand lift leaf cells, expected the one cell of a joined pair; it drew "
			+ tileList(quads));

		// Each cell is a whole cell: two cells of artwork, not one tile
		// stretched over the doorway.
		require(near(left[0].rect.width(), kCellWidth)
				&& near(right[0].rect.width(), kCellWidth),
			scene.what + " did not draw each leaf cell one cell wide: "
			+ point(left[0].rect.width(), right[0].rect.width()) + ", expected "
			+ point(kCellWidth, kCellWidth));

		// They meet on the shared edge: no gap of bare wall between them,
		// and no overlap that would double-paint the seam.
		require(near(left[0].rect.right, right[0].rect.left),
			scene.what + " did not join its leaf cells on their shared edge: the "
			"left cell ends at " + std::to_string(left[0].rect.right)
			+ " and the right cell starts at " + std::to_string(right[0].rect.left));

		// One joined leaf, not two strips stacked at different heights.
		require(near(left[0].rect.top, right[0].rect.top)
				&& near(left[0].rect.bottom, right[0].rect.bottom),
			scene.what + " joined leaf cells disagree vertically: "
			+ describe(left[0].rect) + " against " + describe(right[0].rect));

		// Together they span the two cells the shaft is wide.
		require(near(right[0].rect.right - left[0].rect.left, 2.0f * kCellWidth),
			scene.what + " spans "
			+ std::to_string(right[0].rect.right - left[0].rect.left)
			+ "px across its joined leaves, expected " + std::to_string(2.0f * kCellWidth));

		require(fallbackLeafFills(drawing).empty(), scene.what
			+ " fell back to the primitive leaf fill with the Objects atlas installed");
	}

	// ------------------------------------------------------------------
	// A Shuttle landing Door resolves to the shuttle leaf family.
	// ------------------------------------------------------------------
	void shuttleLandingResolvesToTheShuttleFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const scene = buildShuttleLanding();
		require(scene.door->isShuttleOwned(),
			"A Shuttle landing does not report itself Shuttle-owned");
		require(!scene.door->isLiftOwned(),
			"A Shuttle landing reports itself Lift-owned");

		auto const drawing = renderLanding(scene);
		auto const quads = objectQuads(drawing, atlas.authored);
		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const lift = stringsOf(kLiftFamily, std::size(kLiftFamily));
		wearsOnly(quads, {"door-shuttle-L0"}, ordinary, scene.what);
		wearsOnly(quads, {"door-shuttle-L0"}, lift, scene.what);

		// The hatch is authored one cell wide, so its family has no
		// right-hand leaf. A renderer that invented one would be drawing
		// art nobody authored; a family that grew one would be a second
		// hatch to keep in step for nothing.
		require(atlas.authored.sprites.count("door-shuttle-R0") == 0,
			"The shuttle leaf family grew a 'door-shuttle-R0' tile, but a "
			"Shuttle hatch is authored one cell wide and needs no right-hand leaf");

		require(fallbackLeafFills(drawing).empty(), scene.what
			+ " fell back to the primitive leaf fill with the Objects atlas installed");
	}

	// ------------------------------------------------------------------
	// Ordinary Doors are untouched: they keep the ordinary family and
	// take nothing from a transport family.
	// ------------------------------------------------------------------
	void ordinaryDoorsStillResolveToTheOrdinaryFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const transport = stringsOf(kLiftFamily, std::size(kLiftFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		for (uint32_t width : { 1u, 2u })
		{
			auto const scene = buildOrdinaryLanding(width);
			require(!scene.door->isLiftOwned() && !scene.door->isShuttleOwned(),
				scene.what + " reports itself transport-owned");

			auto const drawing = renderLanding(scene);
			auto const quads = objectQuads(drawing, atlas.authored);
			wearsOnly(quads, {"door-L0"}, transport, scene.what);
			wearsOnly(quads, {"door-L0"}, shuttle, scene.what);

			// A two-cell Door joins the ordinary pair the same way the
			// lift pair joins; a one-cell Door never reaches a right-hand
			// leaf at all.
			auto const right = byTile(quads, "door-R0");
			require(right.size() == (width == 2u ? 1u : 0u), scene.what
				+ " drew " + std::to_string(right.size()) + " 'door-R0' cells, "
				"expected " + std::to_string(width == 2u ? 1u : 0u) + "; it drew "
				+ tileList(quads));

			require(fallbackLeafFills(drawing).empty(), scene.what
				+ " fell back to the primitive leaf fill with the Objects atlas installed");
		}
	}

	// ------------------------------------------------------------------
	// Every shipped Lift and Shuttle landing in the bundled World
	// documents renders its own leaf, on the atlas the application
	// installs.
	// ------------------------------------------------------------------

	struct ShippedLanding
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::Door> door;
		uint32_t layer{ 0 };
		std::string where;
	};

	std::vector<ShippedLanding> transportLandingsIn(std::shared_ptr<core::World> const& world,
		std::string const& document)
	{
		std::vector<ShippedLanding> found;
		for (uint32_t layer = 0; layer < world->getLevelsHigh(); ++layer)
		{
			for (auto const& sector : world->getSectors(layer))
			{
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					auto const doorObject = std::dynamic_pointer_cast<const core::DoorSectorObject>(object);
					if (!doorObject) continue;
					auto const door = doorObject->getDoor();
					if (!door) continue;
					bool const lift = world->isLiftOwnedDoor(object);
					bool const shuttle = world->isShuttleOwnedDoor(object);
					if (!lift && !shuttle) continue;
					found.push_back({ world, door, door->getFrontLayer(),
						document + " on Layer " + std::to_string(layer) + " of "
						+ sector->getDescription() + ", owned by "
						+ (lift ? "a Lift" : "a Shuttle") });
				}
			}
		}
		return found;
	}

	void everyShippedLiftAndShuttleRendersItsOwnLeaf(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const lift = stringsOf(kLiftFamily, std::size(kLiftFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		uint32_t liftLandings = 0, shuttleLandings = 0;
		for (auto const* document : { "resources/Citadel.world.yaml",
			"resources/test-worlds/lift-test-1.world.yaml",
			"resources/test-worlds/shuttle-test-1.world.yaml" })
		{
			std::shared_ptr<core::World> loaded(
				core::loadWorldDocument(context.fixture(document)).release());
			require(bool(loaded), std::string("Bundled World document ") + document
				+ " did not load");
			auto const landings = transportLandingsIn(loaded, document);
			require(!landings.empty(), std::string("Bundled World document ") + document
				+ " contains no Lift or Shuttle landing Door at all");

			for (auto const& landing : landings)
			{
				WorldDrawList drawing{ fullCanvas() };
				{
					RenderWorldScope scope(landing.world);
					renderDoor(landing.door, landing.layer, LayerRenderStyle::Solid,
						false, &drawing);
				}
				auto const quads = objectQuads(drawing, atlas.authored);
				if (landing.door->isLiftOwned())
				{
					wearsOnly(quads, {"door-lift-L0"}, ordinary, landing.where);
					++liftLandings;
				}
				else
				{
					wearsOnly(quads, {"door-shuttle-L0"}, ordinary, landing.where);
					wearsOnly(quads, {"door-shuttle-L0"}, lift, landing.where);
					++shuttleLandings;
				}
				require(fallbackLeafFills(drawing).empty(), landing.where
					+ " fell back to the primitive leaf fill with the Objects atlas "
					"installed, so its leaf has no artwork behind it");
			}
		}

		require(liftLandings > 0 && shuttleLandings > 0,
			"The shipped sweep covered " + std::to_string(liftLandings)
			+ " Lift landings and " + std::to_string(shuttleLandings)
			+ " Shuttle landings; both families must be represented or the "
			"sweep proves nothing");
	}

	// ------------------------------------------------------------------
	// Pulling the Objects atlas out still trips the missing-art detector.
	// With no tile to draw, every transport leaf falls back to the
	// primitive fill - which is the signal that the leaves really came
	// from the atlas in the checks above rather than from the fallback
	// that would have hidden an absent tile.
	// ------------------------------------------------------------------
	void pullingTheObjectsAtlasOutTripsTheMissingArtDetector(
		smoke::Context const& /*context*/)
	{
		prepareCanvas();
		clearObjectTileset();

		std::vector<LandingScene> scenes;
		scenes.push_back(buildLiftLanding("Corridor", 1));
		scenes.push_back(buildLiftLanding("Room", 2));
		scenes.push_back(buildShuttleLanding());

		for (auto const& scene : scenes)
		{
			auto const drawing = renderLanding(scene);
			auto const fills = fallbackLeafFills(drawing);
			require(!fills.empty(), scene.what
				+ " drew no fallback leaf fill with the Objects atlas pulled out, so "
				"its leaf is not coming from the atlas at all");

			// And nothing from the atlas either. With no Objects atlas
			// installed no ObjectAtlas quad can exist, so the fallback is
			// the only leaf geometry on screen - which is what makes the
			// atlas-backed renders above evidence rather than assumption.
			require(atlasQuads(drawing).empty(), scene.what
				+ " drew an ObjectAtlas quad with no Objects atlas installed");
		}
	}

}

void render_smoke::registerTransportDoorLeaves(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "transportDoorLeaves/liftLandingResolvesByOwnership",
		isolated<liftLandingResolvesByOwnershipNotByItsWall> });
	checks.push_back({ "transportDoorLeaves/twoWideLiftLandingJoinsItsLeafCells",
		isolated<twoWideLiftLandingJoinsItsTwoLeafCells> });
	checks.push_back({ "transportDoorLeaves/shuttleLandingResolvesToShuttleFamily",
		isolated<shuttleLandingResolvesToTheShuttleFamily> });
	checks.push_back({ "transportDoorLeaves/ordinaryDoorsKeepTheOrdinaryFamily",
		isolated<ordinaryDoorsStillResolveToTheOrdinaryFamily> });
	checks.push_back({ "transportDoorLeaves/everyShippedTransportRendersItsOwnLeaf",
		isolated<everyShippedLiftAndShuttleRendersItsOwnLeaf> });
	checks.push_back({ "transportDoorLeaves/pullingTheAtlasOutTripsTheDetector",
		isolated<pullingTheObjectsAtlasOutTripsTheMissingArtDetector> });
}
