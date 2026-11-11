// A tall Door draws the tall leaf family (#1058).
//
// A Tall ordinary Door opens 0.9 World units against the regular Door's
// 0.5. Drawn from the regular leaf, that extra height above the jamb is
// dead wall inside the doorway: the panel stops where a shorter door's
// panel stops and the aperture above it shows nothing but the surround.
// The ticket gives the tall Door its own pair - `door-tall-L0` and
// `door-tall-R0` - and makes the height class pick them, so a 0.9
// opening shows a taller panel instead of a regular leaf with dead space
// above it.
//
// The tall pair is the same full-cell contract every leaf family got in
// #1056: the tile is drawn at its whole cell and the doorway aperture is
// what cuts it. That is why the extra height is a clip fact rather than a
// drawn-size fact, and why these checks read the clip the leaves were
// painted under instead of the extent of the quads - a tall leaf's
// vertices reach a whole Level up whatever the Door's height.
//
// OpenUp is where the taller aperture has to show up twice. The leaf's
// travel is the aperture height, so a tall leaf travels further than a
// regular one; and because the Door's vertical speed is the regular
// Door's speed regardless of its own height, the tall Door takes
// proportionally longer to open and close. Travel further at the same
// speed, and the two facts are the same fact asserted twice - which is
// what keeps a tall OpenUp Door from either snapping open or crawling.
//
// Regular Doors are the control. They keep the ordinary pair, the ordinary
// aperture and the ordinary timing, so the new selection cannot have
// quietly re-skinned or re-scaled a door nobody made tall. And a transport
// landing keeps its own leaves even with its height forced Tall, because
// ownership is asked before the height class - the two rules cannot trade
// places and re-skin a lift as a tall room door.
//
// Everything is asserted on the recorded WorldDrawList command vector,
// the stable CPU seam ADR 0010 names: which tile reached the screen is
// read back off the recorded texture coordinates, and the clip is read
// back off the draw command that consumed the geometry. Because a missing
// tile falls back to a primitive fill, the tall renders are also repeated
// with the Objects atlas pulled out, which is what proves the tall leaves
// really came from the atlas rather than from the fallback that would have
// hidden an absent tile.

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
#include "core/Facade.h"
#include "core/SectorType.h"
#include "core/World.h"

#include "imgui/imgui.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
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

	std::string point(float a, float b) { return std::to_string(a) + "," + std::to_string(b); }

	std::string styleName(core::Door::OpenStyle style)
	{
		switch (style)
		{
		case core::Door::OpenStyle::OpenUp: return "OpenUp";
		case core::Door::OpenStyle::OpenLeft: return "OpenLeft";
		case core::Door::OpenStyle::OpenRight: return "OpenRight";
		case core::Door::OpenStyle::OpenApart: return "OpenApart";
		}
		return "unknown";
	}

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

	// The leaf families, by what picks them.
	char const* const kOrdinaryFamily[] = { "door-L0", "door-R0" };
	char const* const kTallFamily[] = { "door-tall-L0", "door-tall-R0" };
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

	ScreenRect intersect(ScreenRect const& a, ScreenRect const& b)
	{
		return {std::max(a.left, b.left), std::max(a.top, b.top),
			std::min(a.right, b.right), std::min(a.bottom, b.bottom)};
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
		WorldDrawList::ClipRectangle clip{};
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
			std::string tile = "<untagged>";
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

			// The two triangles of one quad share a tile, a destination and
			// a clip, so they fold together. Two cells wearing the same tile
			// sit at different destinations and stay apart.
			bool merged = false;
			for (auto& other : quads)
			{
				if (other.tile != tile || !sameRect(other.rect, rect)
						|| !sameRect({other.clip.minimum.x, other.clip.minimum.y,
								other.clip.maximum.x, other.clip.maximum.y},
							{triangle->clip.minimum.x, triangle->clip.minimum.y,
								triangle->clip.maximum.x, triangle->clip.maximum.y}))
					continue;
				merged = true;
				break;
			}
			if (!merged) quads.push_back({ tile, rect, triangle->clip });
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

	// The doorway aperture in screen pixels, read off the Door's own shape
	// so the check follows whatever the height class resolved to.
	ScreenRect apertureOnScreen(std::shared_ptr<const core::Door> const& door)
	{
		core::Vector2 lo, hi;
		door->getFullShape(lo, hi);
		auto const left = std::min(lo.x, hi.x), right = std::max(lo.x, hi.x);
		auto const bottom = std::min(lo.y, hi.y), top = std::max(lo.y, hi.y);
		return { left * kCellWidth, kCanvasHeight - top * kLevelHeight,
			right * kCellWidth, kCanvasHeight - bottom * kLevelHeight };
	}

	// ------------------------------------------------------------------
	// Fixtures
	// ------------------------------------------------------------------

	struct DoorScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::Door> door;
		uint32_t layer{ 0 };
		std::string what;
	};

	std::shared_ptr<core::Door> doorAt(core::World::CreateDoorResult const& made)
	{
		auto const object = made.door.sector->getObject(made.door.index);
		require(bool(object), "A Door has no SectorObject");
		auto const doorObject = std::dynamic_pointer_cast<core::DoorSectorObject>(object);
		require(bool(doorObject), "A Door is not a Door SectorObject");
		auto const door = doorObject->getDoor();
		require(bool(door), "A Door SectorObject has no Door");
		return door;
	}

	// Two Rooms on adjacent Layers with one ordinary Door between them, at
	// the height class and opening style the caller asked for. A tall Door
	// is only available in a Room, which is exactly what the front here is.
	DoorScene buildOrdinaryLanding(uint32_t width, core::Door::Height height,
		core::Door::OpenStyle style)
	{
		auto world = std::make_shared<core::World>("Tall Door artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addRoom("Front", 0, 0, 0, kWorldCellsWide, 1);
		world->addRoom("Back", 1, 0, 0, kWorldCellsWide, 1);
		core::World::CreateDoorOptions options;
		options.width = width;
		options.height = height;
		options.openStyle = style;
		auto const made = world->addSectorDoor(0, 0, 3, options);
		world->finishBuild();

		DoorScene scene;
		scene.world = world;
		scene.door = doorAt(made);
		scene.layer = scene.door->getFrontLayer();
		scene.what = std::string(height == core::Door::Height::Tall ? "tall" : "regular")
			+ " " + std::to_string(width) + "-cell " + styleName(style) + " Door";
		return scene;
	}

	// A Facade in front of a Room with one ordinary Door set into the Facade
	// wall (#1060). The wall is the only difference from `buildOrdinaryLanding`:
	// the leaf family is picked by the Door's height class and its ownership,
	// never by what stands behind it, so a tall Facade Door has to draw the
	// same tall pair a tall Room Door does.
	DoorScene buildFacadeLanding(uint32_t width, core::Door::Height height,
		core::Door::OpenStyle style)
	{
		auto world = std::make_shared<core::World>("Facade tall Door artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addFacade("Front", 0, 0, 0, kWorldCellsWide, 1,
			CORE_ROOM_MAX_HEIGHT, core::Facade::defaultColour());
		world->addRoom("Back", 1, 0, 0, kWorldCellsWide, 1);
		core::World::CreateDoorOptions options;
		options.width = width;
		options.height = height;
		options.openStyle = style;
		auto const made = world->addSectorDoor(0, 0, 3, options);
		world->finishBuild();

		DoorScene scene;
		scene.world = world;
		scene.door = doorAt(made);
		scene.layer = scene.door->getFrontLayer();
		scene.what = std::string(height == core::Door::Height::Tall ? "tall" : "regular")
			+ " " + std::to_string(width) + "-cell " + styleName(style)
			+ " Facade Door";
		return scene;
	}

	// A Lift landing and a Shuttle hatch, for the control that the height
	// class never reaches a transport's leaves.
	DoorScene buildLiftLanding(uint32_t shaftWidth)
	{
		auto world = std::make_shared<core::World>("Lift landing artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addCorridor(0, 0, kWorldCellsWide);
		world->addCorridor(3, 0, kWorldCellsWide);
		core::World::CreateLiftOptions options;
		options.cellsWide = shaftWidth;
		options.stopOffsets = { 0, 3 };
		options.capacity = 2;
		auto const made = world->addLift(1, 0, 4, options);
		world->finishBuild();
		require(made.doors.size() == 2, "The Lift fixture made "
			+ std::to_string(made.doors.size()) + " landing Doors, expected 2");

		DoorScene scene;
		scene.world = world;
		scene.door = doorAt(made.doors[0]);
		scene.layer = scene.door->getFrontLayer();
		scene.what = "a " + std::to_string(shaftWidth) + "-cell Lift landing";
		return scene;
	}

	DoorScene buildShuttleLanding()
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

		DoorScene scene;
		scene.world = world;
		scene.door = doorAt(made.doors[0]);
		scene.layer = scene.door->getFrontLayer();
		scene.what = "a Shuttle hatch landing";
		return scene;
	}

	WorldDrawList renderLanding(DoorScene const& scene)
	{
		WorldDrawList drawing{ fullCanvas() };
		RenderWorldScope scope(scene.world);
		renderDoor(scene.door, scene.layer, LayerRenderStyle::Solid, false, &drawing);
		return drawing;
	}

	// Drives a Door to an exact open fraction through its own timing, so
	// the renderer reads a real device state rather than a remembered
	// number.
	void driveTo(std::shared_ptr<core::Door> const& door, float fraction)
	{
		door->close();
		while (!door->isClosed()) door->update(door->getOpenCloseTime());
		if (fraction <= 0.0f) return;
		door->open();
		door->update(door->getOpenCloseTime() * fraction);
		require(near(door->getOpenPercentage(), fraction),
			"The Door fixture could not be driven to " + std::to_string(fraction)
			+ " open, reached " + std::to_string(door->getOpenPercentage()));
	}

	// Asserts a rendered Door wears the named tiles and carries nothing
	// from the forbidden ones.
	void wearsOnly(std::vector<DrawnQuad> const& quads,
		std::vector<std::string> const& expected,
		std::vector<std::string> const& forbidden,
		std::string const& what)
	{
		for (auto const& tile : expected)
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
	// A tall ordinary Door resolves to the tall leaf family by its height
	// class, and a two-cell-wide one joins the tall pair across their
	// shared edge.
	// ------------------------------------------------------------------
	void tallOrdinaryDoorResolvesToTheTallFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const lift = stringsOf(kLiftFamily, std::size(kLiftFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		for (uint32_t width : { 1u, 2u })
		{
			auto const scene = buildOrdinaryLanding(width,
				core::Door::Height::Tall, core::Door::OpenStyle::OpenUp);
			require(scene.door->getHeight() == core::Door::Height::Tall,
				"The tall fixture is not Tall, so the check would prove nothing");
			require(!scene.door->isLiftOwned() && !scene.door->isShuttleOwned(),
				"The tall fixture reports itself transport-owned");

			auto const drawing = renderLanding(scene);
			auto const quads = objectQuads(drawing, atlas.authored);

			std::vector<std::string> expected{ "door-tall-L0" };
			if (width == 2u) expected.push_back("door-tall-R0");
			wearsOnly(quads, expected, ordinary, scene.what);
			wearsOnly(quads, expected, lift, scene.what);
			wearsOnly(quads, expected, shuttle, scene.what);

			// The joined pair: one left cell and one right cell, each a
			// whole cell wide, meeting on their shared edge. A single tile
			// stretched over the doorway would fail the per-cell width.
			auto const left = byTile(quads, "door-tall-L0");
			require(left.size() == 1, scene.what + " drew "
				+ std::to_string(left.size()) + " left-hand tall leaf cells, "
				"expected one; it drew " + tileList(quads));
			require(near(left[0].rect.width(), kCellWidth), scene.what
				+ " did not draw its left tall leaf cell one cell wide: "
				+ std::to_string(left[0].rect.width()) + ", expected "
				+ std::to_string(kCellWidth));
			if (width == 2u)
			{
				auto const right = byTile(quads, "door-tall-R0");
				require(right.size() == 1, scene.what + " drew "
					+ std::to_string(right.size()) + " right-hand tall leaf "
					"cells, expected one; it drew " + tileList(quads));
				require(near(right[0].rect.width(), kCellWidth), scene.what
					+ " did not draw its right tall leaf cell one cell wide: "
					+ std::to_string(right[0].rect.width()));
				require(near(left[0].rect.right, right[0].rect.left), scene.what
					+ " did not join its tall leaf cells on their shared edge: "
					"the left ends at " + std::to_string(left[0].rect.right)
					+ " and the right starts at "
					+ std::to_string(right[0].rect.left));
				require(near(left[0].rect.top, right[0].rect.top)
						&& near(left[0].rect.bottom, right[0].rect.bottom),
					scene.what + " joined tall leaf cells disagree vertically: "
					+ describe(left[0].rect) + " against " + describe(right[0].rect));
			}
			else
			{
				require(byTile(quads, "door-tall-R0").empty(), scene.what
					+ " reached a right-hand tall leaf it has no cell for; it drew "
					+ tileList(quads));
			}

			require(fallbackLeafFills(drawing).empty(), scene.what
				+ " fell back to the primitive leaf fill with the Objects atlas "
				"installed, so its leaf has no artwork behind it");
		}
	}

	// ------------------------------------------------------------------
	// The taller aperture clips correctly in all four opening styles. The
	// clip every leaf is painted under is the tall doorway itself: its top
	// is the tall jamb, which sits above where a regular Door's jamb
	// would be, and its bottom stands on the cell floor. A closed leaf
	// covers that whole taller aperture, which is the difference between
	// a taller panel and a regular panel with dead wall above it.
	// ------------------------------------------------------------------
	void tallApertureClipsInEveryOpeningStyle(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const tall = buildOrdinaryLanding(2, core::Door::Height::Tall,
			core::Door::OpenStyle::OpenUp);
		auto const regular = buildOrdinaryLanding(2, core::Door::Height::Regular,
			core::Door::OpenStyle::OpenUp);

		auto const tallAperture = apertureOnScreen(tall.door);
		auto const regularAperture = apertureOnScreen(regular.door);

		// The tall aperture really is the taller one, by exactly the two
		// heights' difference. Without this the clip check below could be
		// satisfied by a tall Door that was never made taller.
		require(tallAperture.top < regularAperture.top, "A tall Door's jamb is "
			"not above a regular Door's jamb: " + std::to_string(tallAperture.top)
			+ " against " + std::to_string(regularAperture.top));
		require(near(tallAperture.height(), CORE_DOOR_TALL_HEIGHT * kLevelHeight),
			"A tall Door's aperture is not the tall height: "
			+ std::to_string(tallAperture.height()) + ", expected "
			+ std::to_string(CORE_DOOR_TALL_HEIGHT * kLevelHeight));
		require(near(regularAperture.height(), CORE_DOOR_HEIGHT * kLevelHeight),
			"The regular control's aperture is not the regular height: "
			+ std::to_string(regularAperture.height()));
		require(near(tallAperture.height() - regularAperture.height(),
				(CORE_DOOR_TALL_HEIGHT - CORE_DOOR_HEIGHT) * kLevelHeight),
			"The tall aperture is not the regular one plus the tall step");
		// Both stand on the same cell floor, so the extra height is all
		// above the jamb and none of it is below it.
		require(near(tallAperture.bottom, regularAperture.bottom),
			"The tall aperture does not stand on the same cell floor as the "
			"regular one: " + std::to_string(tallAperture.bottom) + " against "
			+ std::to_string(regularAperture.bottom));

		for (uint32_t const width : { 1u, 2u })
		for (auto const style : { core::Door::OpenStyle::OpenUp,
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight,
			core::Door::OpenStyle::OpenApart })
		{
			auto const scene = buildOrdinaryLanding(width, core::Door::Height::Tall,
				style);
			// Every tall doorway shares the tall jamb line and the cell floor
			// whatever its width; the jambs themselves belong to the Door, so
			// the clip is checked against this Door's own aperture.
			auto const aperture = apertureOnScreen(scene.door);
			require(near(aperture.top, tallAperture.top)
					&& near(aperture.bottom, tallAperture.bottom),
				"A tall " + std::to_string(width) + "-cell Door does not stand on "
				"the same floor and jamb as the reference tall Door: "
				+ describe(aperture) + " against " + describe(tallAperture));
			for (float const fraction : { 0.0f, 0.25f, 0.5f, 0.75f })
			{
				driveTo(scene.door, fraction);
				auto const drawing = renderLanding(scene);
				auto const quads = objectQuads(drawing, atlas.authored);
				auto const label = std::to_string(width) + "-cell "
					+ styleName(style) + " at " + std::to_string(fraction) + " open";

				require(!quads.empty(), "A tall " + label
					+ " Door emitted no leaf artwork at all");
				for (auto const& quad : quads)
				{
					require(quad.tile == "door-tall-L0" || quad.tile == "door-tall-R0",
						"A tall " + label + " Door drew a '" + quad.tile
						+ "' leaf, which is not the tall family; it drew "
						+ tileList(quads));
					// The clip is the tall doorway: every leaf is cut to
					// the tall jamb above and the cell floor below, so no
					// leaf paints over the wall its collision stops in and
					// none stops short of the taller opening.
					ScreenRect const clip{ quad.clip.minimum.x, quad.clip.minimum.y,
						quad.clip.maximum.x, quad.clip.maximum.y };
					require(near(clip.top, aperture.top), "A tall " + label
						+ " leaf is not clipped to the tall jamb: clip top "
						+ std::to_string(clip.top) + ", tall jamb "
						+ std::to_string(aperture.top));
					require(near(clip.bottom, aperture.bottom), "A tall "
						+ label + " leaf is not clipped to the cell floor: clip "
						"bottom " + std::to_string(clip.bottom) + ", floor "
						+ std::to_string(aperture.bottom));
					require(near(clip.left, aperture.left)
							&& near(clip.right, aperture.right),
						"A tall " + label + " leaf is not clipped to the doorjambs: "
						+ describe(clip) + " against " + describe(aperture));
					// The clip carries the extra height rather than the
					// regular Door's jamb cutting the tall leaf short.
					require(clip.top <= regularAperture.top + 0.001f, "A tall "
						+ label + " leaf is cut at the regular Door's jamb, so the "
						"top of the tall opening is never painted");
				}

				if (fraction > 0.0f) continue;

				// Closed: the leaves cover the whole taller aperture. Any
				// uncovered strip at the top is the dead space this ticket
				// exists to remove.
				ScreenRect covered{};
				covered.left = covered.top = 1e9f;
				covered.right = covered.bottom = -1e9f;
				for (auto const& quad : quads)
				{
					auto const visible = intersect(quad.rect, aperture);
					covered.left = std::min(covered.left, visible.left);
					covered.top = std::min(covered.top, visible.top);
					covered.right = std::max(covered.right, visible.right);
					covered.bottom = std::max(covered.bottom, visible.bottom);
				}
				require(sameRect(covered, aperture), "A closed tall " + label
					+ " Door leaves part of its taller aperture "
					"uncovered: covered " + describe(covered) + " against "
					+ describe(aperture));
			}
		}
	}

	// ------------------------------------------------------------------
	// OpenUp travel scales with the tall aperture so the leaf keeps the
	// regular Door's vertical speed: it travels further, and it takes
	// proportionally longer, which is the same statement about the same
	// pixels per second.
	// ------------------------------------------------------------------
	void openUpTravelScalesWithTheTallAperture(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const tall = buildOrdinaryLanding(1, core::Door::Height::Tall,
			core::Door::OpenStyle::OpenUp);
		auto const regular = buildOrdinaryLanding(1, core::Door::Height::Regular,
			core::Door::OpenStyle::OpenUp);

		auto const tallAperture = apertureOnScreen(tall.door);
		auto const regularAperture = apertureOnScreen(regular.door);
		float const tallTravelPx = tallAperture.height();
		float const regularTravelPx = regularAperture.height();
		require(tallTravelPx > regularTravelPx + 1.0f, "The tall aperture is not "
			"taller than the regular one, so the travel comparison proves nothing");

		// The leaf's vertical speed is the regular Door's, whatever the
		// height it is carrying: distance over the open/close duration is
		// the same for both, and both are the regular Door's height per
		// CORE_DOOR_OPEN_CLOSE_TIME.
		auto const verticalSpeed = [](std::shared_ptr<const core::Door> const& door,
			float travelPx)
		{
			return travelPx / door->getOpenCloseTime();
		};
		float const expectedSpeed = CORE_DOOR_HEIGHT * kLevelHeight
			/ CORE_DOOR_OPEN_CLOSE_TIME;
		require(near(verticalSpeed(tall.door, tallTravelPx), expectedSpeed),
			"A tall OpenUp Door's leaf does not keep the regular Door's vertical "
			"speed: " + std::to_string(verticalSpeed(tall.door, tallTravelPx))
			+ " px/s, expected " + std::to_string(expectedSpeed));
		require(near(verticalSpeed(regular.door, regularTravelPx), expectedSpeed),
			"The regular OpenUp control does not hold the shared vertical speed: "
			+ std::to_string(verticalSpeed(regular.door, regularTravelPx)));
		// Which is why the tall Door takes longer, and not by a rounding:
		// the same speed over more pixels.
		require(tall.door->getOpenCloseTime() > regular.door->getOpenCloseTime(),
			"A tall OpenUp Door does not take longer to open than a regular one, "
			"so its leaf must be moving faster than the regular Door's");
		require(near(tall.door->getOpenCloseTime() / regular.door->getOpenCloseTime(),
				tallTravelPx / regularTravelPx),
			"The tall Door's open/close time does not scale with its aperture: "
			+ std::to_string(tall.door->getOpenCloseTime() / regular.door->getOpenCloseTime())
			+ " against " + std::to_string(tallTravelPx / regularTravelPx));

		// And the geometry follows: at each open fraction the tall leaf has
		// risen fraction * tallApertureHeight, further than the regular
		// leaf rose in the same fraction of its own shorter travel.
		for (float const fraction : { 0.25f, 0.5f, 0.75f })
		{
			driveTo(tall.door, fraction);
			driveTo(regular.door, fraction);
			auto const tallQuads = objectQuads(renderLanding(tall), atlas.authored);
			auto const regularQuads = objectQuads(renderLanding(regular),
				atlas.authored);

			driveTo(tall.door, 0.0f);
			driveTo(regular.door, 0.0f);
			auto const tallClosed = objectQuads(renderLanding(tall), atlas.authored);
			auto const regularClosed = objectQuads(renderLanding(regular),
				atlas.authored);

			auto const risen = [](std::vector<DrawnQuad> const& open,
				std::vector<DrawnQuad> const& closed, std::string const& tile,
				std::string const& what)
			{
				require(!byTile(open, tile).empty() && !byTile(closed, tile).empty(),
					what + " lost its '" + tile + "' leaf while measuring its rise");
				return byTile(closed, tile)[0].rect.top - byTile(open, tile)[0].rect.top;
			};

			auto const tallRise = risen(tallQuads, tallClosed, "door-tall-L0",
				"A tall OpenUp Door");
			auto const regularRise = risen(regularQuads, regularClosed, "door-L0",
				"A regular OpenUp Door");
			require(near(tallRise, fraction * tallTravelPx), "A tall OpenUp leaf "
				"did not rise the tall aperture at " + std::to_string(fraction)
				+ " open: " + std::to_string(tallRise) + ", expected "
				+ std::to_string(fraction * tallTravelPx));
			require(near(regularRise, fraction * regularTravelPx),
				"The regular OpenUp control did not rise its own aperture at "
				+ std::to_string(fraction) + " open: "
				+ std::to_string(regularRise));
			require(tallRise > regularRise + 0.5f, "A tall OpenUp leaf rose no "
				"further than a regular one at " + std::to_string(fraction)
				+ " open, so the travel never saw the height class");
		}
	}

	// ------------------------------------------------------------------
	// Regular Doors are unaffected: they keep the ordinary pair, the
	// ordinary aperture and the ordinary timing, and never reach a tall
	// tile.
	// ------------------------------------------------------------------
	void regularDoorsKeepTheRegularFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const tall = stringsOf(kTallFamily, std::size(kTallFamily));

		for (uint32_t width : { 1u, 2u })
		{
			for (auto const style : { core::Door::OpenStyle::OpenUp,
				core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight,
				core::Door::OpenStyle::OpenApart })
			{
				auto const scene = buildOrdinaryLanding(width,
					core::Door::Height::Regular, style);
				require(scene.door->getHeight() == core::Door::Height::Regular,
					"The regular control is not Regular");

				// Closed, the whole ordinary pair is on screen: a 1-wide
				// Door never reaches a right-hand leaf, a 2-wide one joins
				// the pair across their shared edge.
				driveTo(scene.door, 0.0f);
				auto const closed = renderLanding(scene);
				std::vector<std::string> expected{ "door-L0" };
				if (width == 2u) expected.push_back("door-R0");
				wearsOnly(objectQuads(closed, atlas.authored), expected, tall,
					scene.what + " closed");
				require(fallbackLeafFills(closed).empty(), scene.what
					+ " closed fell back to the primitive leaf fill with the "
					"Objects atlas installed");

				// Part open, the leaves that have slid clear of the doorway
				// emit no geometry, so only the pair's remaining cells are
				// asked about - but no tall tile appears at any point, and
				// what is drawn is still clipped to the regular doorway.
				driveTo(scene.door, 0.5f);
				auto const drawing = renderLanding(scene);
				auto const quads = objectQuads(drawing, atlas.authored);
				require(!quads.empty(), scene.what
					+ " at 0.5 open emitted no leaf artwork at all");
				for (auto const& tile : tall)
					require(byTile(quads, tile).empty(), scene.what
						+ " at 0.5 open drew a '" + tile
						+ "' leaf from the tall family; it drew " + tileList(quads));

				// The regular aperture is untouched by the tall family's
				// arrival, and its clip is that aperture rather than a cell.
				auto const aperture = apertureOnScreen(scene.door);
				require(near(aperture.height(), CORE_DOOR_HEIGHT * kLevelHeight),
					scene.what + "'s aperture is not the regular height: "
					+ std::to_string(aperture.height()));
				for (auto const& quad : quads)
				{
					ScreenRect const clip{ quad.clip.minimum.x, quad.clip.minimum.y,
						quad.clip.maximum.x, quad.clip.maximum.y };
					require(near(clip.top, aperture.top)
							&& near(clip.bottom, aperture.bottom),
						scene.what + " at 0.5 open is not clipped to its own "
						"regular doorway: " + describe(clip) + " against "
						+ describe(aperture));
				}

				require(fallbackLeafFills(drawing).empty(), scene.what
					+ " fell back to the primitive leaf fill with the Objects "
					"atlas installed");
			}
		}
	}

	// ------------------------------------------------------------------
	// A transport landing keeps its own leaves even when its height reads
	// Tall. Ownership is asked before the height class, so the two rules
	// cannot trade places and re-skin a lift as a tall room door. The
	// forced height here is a probe, not a supported authoring case: a
	// transport landing is authored regular, and the renderer must not
	// care either way.
	// ------------------------------------------------------------------
	void transportLandingsNeverWearTheTallFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const tall = stringsOf(kTallFamily, std::size(kTallFamily));
		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));

		std::vector<DoorScene> scenes;
		scenes.push_back(buildLiftLanding(1));
		scenes.push_back(buildLiftLanding(2));
		scenes.push_back(buildShuttleLanding());

		for (auto const& scene : scenes)
		{
			require(scene.door->isLiftOwned() || scene.door->isShuttleOwned(),
				scene.what + " is not a transport landing");

			// As authored: regular height, own family.
			{
				auto const quads = objectQuads(renderLanding(scene), atlas.authored);
				wearsOnly(quads, { scene.door->isLiftOwned() ? "door-lift-L0"
						: "door-shuttle-L0" },
					tall, scene.what);
			}

			// Forced Tall: still its own family, and still nothing tall.
			scene.door->setHeight(core::Door::Height::Tall);
			require(scene.door->getHeight() == core::Door::Height::Tall,
				scene.what + " could not be forced Tall, so the probe below "
				"proves nothing");
			auto const drawing = renderLanding(scene);
			auto const quads = objectQuads(drawing, atlas.authored);
			wearsOnly(quads, { scene.door->isLiftOwned() ? "door-lift-L0"
					: "door-shuttle-L0" },
				tall, scene.what + " with its height forced Tall");
			wearsOnly(quads, { scene.door->isLiftOwned() ? "door-lift-L0"
					: "door-shuttle-L0" },
				ordinary, scene.what + " with its height forced Tall");
			require(fallbackLeafFills(drawing).empty(), scene.what
				+ " fell back to the primitive leaf fill with the Objects atlas "
				"installed");
		}
	}

	// ------------------------------------------------------------------
	// A tall Door set into a Facade renders the tall leaf family (#1060).
	//
	// The leaf family is picked by the Door's ownership and its height
	// class, and by nothing about the wall, so admitting a tall Door into a
	// Facade changes nothing here - only the frame behind the leaf changes,
	// and that is the side DoorframeCells pins. This check is the other half
	// of "a tall Facade Door renders the tall leaf family and the Facade's
	// tall frame", and it is what fails if the wall ever creeps into the
	// family selection.
	// ------------------------------------------------------------------
	void aTallFacadeDoorRendersTheTallLeaves(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const lift = stringsOf(kLiftFamily, std::size(kLiftFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		for (uint32_t width : { 1u, 2u })
		{
			auto const scene = buildFacadeLanding(width,
				core::Door::Height::Tall, core::Door::OpenStyle::OpenUp);
			require(scene.door->getHeight() == core::Door::Height::Tall,
				scene.what + " is not Tall, so the render below proves nothing");
			require(scene.door->getFrontSector()->getType() == core::SectorType::Facade,
				scene.what + " was not authored with a Facade on its front side");
			require(!scene.door->isLiftOwned() && !scene.door->isShuttleOwned(),
				scene.what + " reports itself transport-owned");

			auto const drawing = renderLanding(scene);
			auto const quads = objectQuads(drawing, atlas.authored);
			std::vector<std::string> expected{ "door-tall-L0" };
			if (width >= 2u) expected.push_back("door-tall-R0");
			wearsOnly(quads, expected, ordinary, scene.what);
			wearsOnly(quads, expected, lift, scene.what);
			wearsOnly(quads, expected, shuttle, scene.what);
			require(fallbackLeafFills(drawing).empty(), scene.what
				+ " fell back to the primitive leaf fill with the Objects atlas "
				"installed, so its tall leaf has no artwork behind it");
		}

		// The regular Facade Door is the control: the same wall with a
		// regular height still draws the ordinary pair, so the tall render
		// above came from the height class and not from the Facade.
		auto const control = buildFacadeLanding(1, core::Door::Height::Regular,
			core::Door::OpenStyle::OpenUp);
		auto const controlDrawing = renderLanding(control);
		wearsOnly(objectQuads(controlDrawing, atlas.authored),
			{ "door-L0" }, stringsOf(kTallFamily, std::size(kTallFamily)),
			control.what);
	}

	// ------------------------------------------------------------------
	// Every shipped tall Door renders from the tall family, on the atlas
	// the application installs. Sweeping the bundled documents rather
	// than one fixture is what keeps a tall Door added to any shipped
	// World from drawing the short panel.
	// ------------------------------------------------------------------

	std::vector<std::filesystem::path> shippedWorldDocuments(
		smoke::Context const& context)
	{
		// `fixture()` answers for files only, so the sweep starts from one
		// it knows is there and walks out from its parent.
		auto const root = context.fixture("resources/Resources.yaml").parent_path();
		std::vector<std::filesystem::path> docs;
		auto const collect = [&](std::filesystem::path const& directory)
		{
			if (!std::filesystem::is_directory(directory)) return;
			for (auto const& entry : std::filesystem::directory_iterator(directory))
			{
				if (!entry.is_regular_file()) continue;
				if (entry.path().filename().string().find(".world.yaml")
					== std::string::npos) continue;
				docs.push_back(entry.path());
			}
		};
		collect(root);
		collect(root / "test-worlds");
		std::sort(docs.begin(), docs.end());
		return docs;
	}

	std::vector<std::pair<std::shared_ptr<core::Door>, std::string>> tallDoorsIn(
		std::shared_ptr<core::World> const& world, std::string const& document)
	{
		std::vector<std::pair<std::shared_ptr<core::Door>, std::string>> found;
		for (uint32_t layer = 0; layer < world->getLevelsHigh(); ++layer)
		{
			for (auto const& sector : world->getSectors(layer))
			{
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object
							|| object->getObjectType() != core::SectorObjectType::Door)
						continue;
					auto const doorObject = std::dynamic_pointer_cast<
						const core::DoorSectorObject>(object);
					if (!doorObject) continue;
					auto const door = doorObject->getDoor();
					if (!door) continue;
					if (door->getHeight() != core::Door::Height::Tall) continue;
					found.push_back({ door, document + " on Layer "
						+ std::to_string(layer) + " of "
						+ sector->getDescription() + ", "
						+ std::to_string(door->getCellsWide()) + " cells wide, "
						+ styleName(door->getOpenStyle()) });
				}
			}
		}
		return found;
	}

	void everyShippedTallDoorRendersFromTheTallFamily(smoke::Context const& context)
	{
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto const ordinary = stringsOf(kOrdinaryFamily, std::size(kOrdinaryFamily));
		auto const lift = stringsOf(kLiftFamily, std::size(kLiftFamily));
		auto const shuttle = stringsOf(kShuttleFamily, std::size(kShuttleFamily));

		auto const docs = shippedWorldDocuments(context);
		require(docs.size() >= 4, "The shipped World sweep found only "
			+ std::to_string(docs.size()) + " bundled World documents, so it "
			"cannot claim to cover the shipped tall Doors");

		uint32_t tallDoors{ 0 };
		std::string unreadable;
		for (auto const& doc : docs)
		{
			std::shared_ptr<core::World> loaded;
			try
			{
				loaded = std::shared_ptr<core::World>(
					core::loadWorldDocument(doc).release());
			}
			catch (std::exception const& error)
			{
				unreadable += "\n  - " + doc.filename().string() + ": " + error.what();
				continue;
			}
			if (!loaded)
			{
				unreadable += "\n  - " + doc.filename().string() + ": loaded nothing";
				continue;
			}

			auto const relative = doc.filename().string();
			for (auto const& [door, where] : tallDoorsIn(loaded, relative))
			{
				WorldDrawList drawing{ fullCanvas() };
				{
					RenderWorldScope scope(loaded);
					renderDoor(door, door->getFrontLayer(), LayerRenderStyle::Solid,
						false, &drawing);
				}
				auto const quads = objectQuads(drawing, atlas.authored);
				std::vector<std::string> expected{ "door-tall-L0" };
				if (door->getCellsWide() >= 2u) expected.push_back("door-tall-R0");
				wearsOnly(quads, expected, ordinary, where);
				wearsOnly(quads, expected, lift, where);
				wearsOnly(quads, expected, shuttle, where);
				require(fallbackLeafFills(drawing).empty(), where
					+ " fell back to the primitive leaf fill with the Objects "
					"atlas installed, so its leaf has no artwork behind it");
				++tallDoors;
			}
		}

		require(unreadable.empty(), "The shipped World sweep could not load every "
			"bundled World document:" + unreadable);
		require(tallDoors > 0, "The shipped World sweep covered "
			+ std::to_string(docs.size()) + " documents and found no tall Door "
			"at all, so it proves nothing about the tall family");
	}

}

void render_smoke::registerTallDoorLeaves(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "tallDoorLeaves/tallOrdinaryDoorResolvesToTallFamily",
		isolated<tallOrdinaryDoorResolvesToTheTallFamily> });
	checks.push_back({ "tallDoorLeaves/tallApertureClipsInEveryOpeningStyle",
		isolated<tallApertureClipsInEveryOpeningStyle> });
	checks.push_back({ "tallDoorLeaves/openUpTravelScalesWithTheTallAperture",
		isolated<openUpTravelScalesWithTheTallAperture> });
	checks.push_back({ "tallDoorLeaves/regularDoorsKeepTheRegularFamily",
		isolated<regularDoorsKeepTheRegularFamily> });
	checks.push_back({ "tallDoorLeaves/transportLandingsNeverWearTheTallFamily",
		isolated<transportLandingsNeverWearTheTallFamily> });
	checks.push_back({ "tallDoorLeaves/aTallFacadeDoorRendersTheTallLeaves",
		isolated<aTallFacadeDoorRendersTheTallLeaves> });
	checks.push_back({ "tallDoorLeaves/everyShippedTallDoorRendersFromTheTallFamily",
		isolated<everyShippedTallDoorRendersFromTheTallFamily> });
}
