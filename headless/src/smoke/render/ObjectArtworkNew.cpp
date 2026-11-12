// Doors, Windows, Booth Windows, Buttons and Markers draw from ObjectAtlas
// at their declared drawn sizes (#1020).
//
// One Door tile serves all four opening styles: the style decides which way
// the leaf crops, never which artwork is used. A sliding leaf therefore
// crops instead of squashing into the strip of aperture it still covers,
// and a leaf that has slid clean out emits no geometry at all. The crop is
// driven by the Door's own open fraction, read off the live device state,
// so a Door half open shows exactly half open however long it took to get
// there.
//
// A Booth Window keeps its frame fixed and its shutter moving: the frame's
// transparent centre is what makes the slide visible, and the shutter is
// clipped to that centre so it can never paint over the frame it retracts
// behind. A Button's two states are two tiles rather than one tile and two
// tints, so the state is legible from the artwork alone at the size the
// control actually occupies. A Marker stays tintable, which is what keeps
// the selection gold it carries reaching the sheet instead of going white.
//
// Everything is asserted on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names. The atlas is installed the way the
// render stack installs it - pixel regions from the `ImageSet` the
// application manifest declares, render semantics from the authored
// tileset the atlas-set registry names for it - so these checks exercise
// the same pairing the application does.
//
// Screen space is top-left origin and the World is drawn with y growing up,
// so a World point (x, y) lands at screen (x * cell width, canvas height -
// y * level height) at zoom 1 with the viewport at the origin.

#include "Checks.h"

#include "FontMetrics.h"
#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/Button.h"
#include "core/Coordination.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "core/Object.h"
#include "core/Window.h"
#include "core/World.h"

#include "imgui/imgui.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
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

	// Screen-pixel quantities run to hundreds and texture fractions run to
	// thousandths, so a single absolute epsilon would be meaningless
	// against one and far too loose against the other.
	bool relNear(float a, float b, float tolerance = 0.001f)
	{
		auto const scale = std::max(std::abs(a), std::abs(b));
		return scale == 0.0f || std::abs(a - b) / scale < tolerance;
	}

	std::string point(float x, float y) { return std::to_string(x) + "," + std::to_string(y); }

	float const kCellWidth = float(CORE_CELL_WIDTH_PIXELS);
	float const kLevelHeight = float(CORE_LEVEL_HEIGHT_PIXELS);

	// The fixture Worlds are 8 cells by 2 Levels: a front Location, one
	// behind it, and room to spare.
	uint32_t const kWorldCellsWide = 8;
	uint32_t const kWorldLevelsHigh = 2;
	float const kCanvasWidth = float(kWorldCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kWorldLevelsHigh) * kLevelHeight;

	// The renderer's selection highlight colour. It is a Render.cpp global
	// with no header declaration, so the checks name the colour itself.
	PackedColourValue const kSelectionColour = packColour(255, 255, 0);

	// The Booth Window frame's transparent centre, as a fraction of the
	// frame's own extent. The renderer insets by 5/52 across and 5/48 down
	// because those are the ratios the frame art is authored with, so they
	// follow the World cell instead of a remembered pixel count.
	float const kFrameInsetXRatio = 5.0f / 52.0f;
	float const kFrameInsetYRatio = 5.0f / 48.0f;

	// The tiles this ticket lands, with the drawn size and content
	// rectangle each is authored at and whether each takes a tint.
	struct TileExpectation
	{
		char const* name;
		float drawnWidth, drawnHeight;
		int contentWidth, contentHeight;
		bool tintable;
	};

	std::vector<TileExpectation> const kTiles{
		// A Door leaf is a full World cell: the artwork fills the tile and
		// the aperture clip, not the drawn size, bounds it (#1056).
		{ "door-L0", 1.0f, 1.0f, 128, 320, false },
		{ "door-R0", 1.0f, 1.0f, 128, 320, false },
		// The tall Door's leaves are the same full-cell shape; what makes
		// them a different family is the aperture they are clipped by, not
		// the extent they are drawn at (#1058).
		{ "door-tall-L0", 1.0f, 1.0f, 128, 320, false },
		{ "door-tall-R0", 1.0f, 1.0f, 128, 320, false },
		// The Lift landing and Shuttle hatch leaves are the same shape for
		// the same reason - full cell, zero offset, clipped not scaled - and
		// each carries three variants like the ordinary leaf (#1057).
		{ "door-lift-L0", 1.0f, 1.0f, 128, 320, false },
		{ "door-lift-R0", 1.0f, 1.0f, 128, 320, false },
		{ "door-shuttle-L0", 1.0f, 1.0f, 128, 320, false },
		{ "window-clear", 0.8f, 0.3f, 102, 96, false },
		{ "window-frosted", 0.8f, 0.3f, 102, 96, false },
		{ "window-tinted", 0.8f, 0.3f, 102, 96, false },
		{ "booth-window-open", 0.8f, 0.3f, 102, 96, false },
		{ "booth-window-shutter", 0.6461538f, 0.2375f, 83, 76, false },
		{ "button-enabled", 0.1f, 0.04f, 13, 13, false },
		{ "button-disabled", 0.1f, 0.04f, 13, 13, false },
		{ "marker", 0.25f, 0.1375f, 32, 44, true },
	};

	struct ScreenRect
	{
		float left{}, top{}, right{}, bottom{};

		float width() const { return right - left; }
		float height() const { return bottom - top; }
	};

	ScreenRect shifted(ScreenRect const& r, float dx, float dy)
	{
		return { r.left + dx, r.top + dy, r.right + dx, r.bottom + dy };
	}

	bool sameRect(ScreenRect const& a, ScreenRect const& b)
	{
		return near(a.left, b.left) && near(a.top, b.top)
			&& near(a.right, b.right) && near(a.bottom, b.bottom);
	}

	std::string describe(ScreenRect const& r)
	{
		return point(r.left, r.top) + " -> " + point(r.right, r.bottom);
	}

	// The screen rectangle a World-unit rectangle maps to at this check's
	// viewport. World y grows up, screen y grows down.
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

	// The screen rectangle one object's own World shape maps to.
	ScreenRect shapeToScreen(std::shared_ptr<const core::Object> const& object)
	{
		core::Vector2 lo, hi;
		object->getFullShape(lo, hi);
		auto const left = std::min(lo.x, hi.x), right = std::max(lo.x, hi.x);
		auto const bottom = std::min(lo.y, hi.y), top = std::max(lo.y, hi.y);
		return worldToScreen(left, bottom, right - left, top - bottom);
	}

	// ------------------------------------------------------------------
	// The atlas, read the way the application reads it.
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
	// asked about for which atlas.
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

	// The PNG's own dimensions, read from its IHDR chunk. The pixel regions
	// are cut against this size, so the committed file has to agree with
	// what the tileset states.
	std::pair<int, int> pngSize(std::filesystem::path const& path)
	{
		std::ifstream file(path, std::ios::binary);
		require(bool(file), "Could not open the atlas image: " + path.string());
		unsigned char header[24]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		require(file.gcount() == static_cast<std::streamsize>(sizeof(header)),
			"The atlas image is too short to carry its header: " + path.string());
		auto const be = [&](int offset) -> unsigned int {
			return (unsigned(header[offset]) << 24) | (unsigned(header[offset + 1]) << 16)
				| (unsigned(header[offset + 2]) << 8) | unsigned(header[offset + 3]);
		};
		require(be(0) == 0x89504e47u && be(12) == 0x49484452u,
			"The atlas image is not a PNG: " + path.string());
		return { int(be(16)), int(be(20)) };
	}

	std::string const kTileset = "resources/textures/objects-new.tileset.yaml";

	// The new Objects atlas as the application holds it: the authored
	// tileset, the manifest's paired Image, and the pixel regions the
	// manifest's ImageSet declares.
	struct NewObjectsAtlas
	{
		ObjectTileset authored;
		AtlasPixelRegions regions;
		int width{}, height{};
	};

	NewObjectsAtlas loadNewObjectsAtlas(smoke::Context const& context)
	{
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "ObjectAtlas");
		require(set.found && set.type == "ImageSet",
			"The application manifest registers no ObjectAtlas ImageSet");
		require(atlasSetTilesets(context)["ObjectAtlas"]
			== "textures/objects-new.tileset.yaml",
			"The atlas-set registry does not name objects-new.tileset.yaml for "
			"ObjectAtlas");
		auto const image = manifestResource(manifest, set.imageReference);
		require(image.found && image.type == "Image",
			"ObjectAtlas's paired Image resource is missing");

		NewObjectsAtlas atlas;
		atlas.authored = ObjectTileset::load(context.fixture(kTileset));
		atlas.regions = set.regions;
		atlas.width = atlas.authored.width;
		atlas.height = atlas.authored.height;
		auto const sheet = pngSize(context.fixture("resources/" + image.location));
		require(sheet.first == atlas.width && sheet.second == atlas.height,
			"The committed objects-new.png is " + std::to_string(sheet.first) + "x"
			+ std::to_string(sheet.second) + ", the tileset states "
			+ std::to_string(atlas.width) + "x" + std::to_string(atlas.height));
		return atlas;
	}

	// Installs an atlas through the one install path the render stack uses:
	// pixel regions from the ImageSet, render semantics from the authored
	// tileset.
	void installAtlas(ObjectTileset const& authored, AtlasPixelRegions const& regions,
		std::string const& atlasName)
	{
		auto const tiles = installAtlasTiles(authored.sprites, regions,
			authored.width, authored.height, atlasName);
		ObjectTileset installed;
		installed.width = authored.width;
		installed.height = authored.height;
		installed.sprites = tiles;
		setObjectTileset(std::move(installed), static_cast<TextureId>(1));
	}

	NewObjectsAtlas installNewObjectsAtlas(smoke::Context const& context)
	{
		auto const atlas = loadNewObjectsAtlas(context);
		installAtlas(atlas.authored, atlas.regions, "ObjectAtlas");
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
		gUISettings.renderMarkers = true;
		gUISettings.renderNextLayerWireframe = false;
	}

	WorldDrawList drawWorld(std::shared_ptr<core::World> const& world)
	{
		WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
		RenderWorldScope scope(world);
		renderWorld(world, &drawing);
		return drawing;
	}

	// One drawn piece of tile artwork: where it landed on screen, which part
	// of the tile it was cut from, and the tint it carries. The texture
	// range is what tells a check the art slid out of view instead of being
	// squashed into the part still showing.
	struct TileQuad
	{
		bool drawn{ false };
		int triangles{ 0 };
		ScreenRect rect{};
		float uvMinX{}, uvMaxX{}, uvMinY{}, uvMaxY{};
		PackedColourValue colour{};
		WorldDrawList::ClipRectangle clip{};
	};

	// Half-pixel slack: the sprite helper insets a tile's UVs by half a
	// pixel at each edge so it never samples its neighbour's border.
	float const kUvSlackPixels = 1.5f;

	bool triangleUsesRegion(WorldDrawList::Triangle const& triangle,
		SectorTileRegion const& region, float atlasWidth, float atlasHeight)
	{
		for (auto const& uv : triangle.texcoords)
			if (uv.x * atlasWidth < float(region.x) - kUvSlackPixels
				|| uv.x * atlasWidth > float(region.x + region.width) + kUvSlackPixels
				|| uv.y * atlasHeight < float(region.y) - kUvSlackPixels
				|| uv.y * atlasHeight > float(region.y + region.height) + kUvSlackPixels)
				return false;
		return true;
	}

	// Every distinct piece of one tile's artwork the stream carries, as one
	// record per drawn quad. The two triangles of a quad share a texture
	// range and fold together; two leaves wearing different halves of the
	// same tile stay apart, which is how an OpenApart pair is told in two.
	std::vector<TileQuad> tileGroups(WorldDrawList const& drawing,
		SectorTileRegion const& region, int atlasWidth, int atlasHeight)
	{
		auto const w = float(atlasWidth), h = float(atlasHeight);
		std::vector<TileQuad> groups;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
				continue;
			if (!triangleUsesRegion(*triangle, region, w, h)) continue;
			TileQuad q;
			q.drawn = true;
			q.triangles = 1;
			q.rect.left = q.rect.top = 1e9f;
			q.rect.right = q.rect.bottom = -1e9f;
			q.uvMinX = q.uvMinY = 1e9f;
			q.uvMaxX = q.uvMaxY = -1e9f;
			for (auto const& p : triangle->positions)
			{
				q.rect.left = std::min(q.rect.left, p.x);
				q.rect.top = std::min(q.rect.top, p.y);
				q.rect.right = std::max(q.rect.right, p.x);
				q.rect.bottom = std::max(q.rect.bottom, p.y);
			}
			for (auto const& uv : triangle->texcoords)
			{
				q.uvMinX = std::min(q.uvMinX, uv.x);
				q.uvMaxX = std::max(q.uvMaxX, uv.x);
				q.uvMinY = std::min(q.uvMinY, uv.y);
				q.uvMaxY = std::max(q.uvMaxY, uv.y);
			}
			q.colour = triangle->colour;
			q.clip = triangle->clip;
			auto const sameQuad = [&](TileQuad const& other)
			{
				return near(other.uvMinX * w, q.uvMinX * w)
					&& near(other.uvMaxX * w, q.uvMaxX * w)
					&& near(other.uvMinY * h, q.uvMinY * h)
					&& near(other.uvMaxY * h, q.uvMaxY * h);
			};
			bool merged = false;
			for (auto& other : groups)
			{
				if (!sameQuad(other)) continue;
				other.rect.left = std::min(other.rect.left, q.rect.left);
				other.rect.top = std::min(other.rect.top, q.rect.top);
				other.rect.right = std::max(other.rect.right, q.rect.right);
				other.rect.bottom = std::max(other.rect.bottom, q.rect.bottom);
				++other.triangles;
				merged = true;
				break;
			}
			if (!merged) groups.push_back(q);
		}
		return groups;
	}

	// The one piece of tile artwork a caller is asking about, optionally
	// narrowed by a predicate on the texture range.
	TileQuad tileQuad(WorldDrawList const& drawing, SectorTileRegion const& region,
		int atlasWidth, int atlasHeight,
		std::function<bool(TileQuad const&)> keep = {})
	{
		TileQuad result;
		for (auto const& q : tileGroups(drawing, region, atlasWidth, atlasHeight))
		{
			if (keep && !keep(q)) continue;
			if (!result.drawn)
			{
				result = q;
				continue;
			}
			result.rect.left = std::min(result.rect.left, q.rect.left);
			result.rect.top = std::min(result.rect.top, q.rect.top);
			result.rect.right = std::max(result.rect.right, q.rect.right);
			result.rect.bottom = std::max(result.rect.bottom, q.rect.bottom);
			result.uvMinX = std::min(result.uvMinX, q.uvMinX);
			result.uvMaxX = std::max(result.uvMaxX, q.uvMaxX);
			result.uvMinY = std::min(result.uvMinY, q.uvMinY);
			result.uvMaxY = std::max(result.uvMaxY, q.uvMaxY);
			result.triangles += q.triangles;
		}
		return result;
	}

	// The texture edges of a whole tile, matching the half-pixel inset the
	// sprite helper applies, and the span between them.
	float tileLeftU(SectorTileRegion const& r, int atlasWidth)
	{
		return (float(r.x) + 0.5f) / float(atlasWidth);
	}

	float tileRightU(SectorTileRegion const& r, int atlasWidth)
	{
		return (float(r.x + r.width) - 0.5f) / float(atlasWidth);
	}

	float tileTopV(SectorTileRegion const& r, int atlasHeight)
	{
		return (float(r.y) + 0.5f) / float(atlasHeight);
	}

	float tileBottomV(SectorTileRegion const& r, int atlasHeight)
	{
		return (float(r.y + r.height) - 0.5f) / float(atlasHeight);
	}

	bool anyPrimitiveFill(WorldDrawList const& drawing, PackedColourValue colour)
	{
		for (auto const& command : drawing.commands())
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->texture == WorldDrawList::Texture::None
				&& triangle->colour == colour)
				return true;
		return false;
	}

	bool anyTextRun(WorldDrawList const& drawing, std::string const& text)
	{
		for (auto const& command : drawing.commands())
			if (auto const* run = std::get_if<WorldDrawList::Text>(&command);
				run && run->value == text)
				return true;
		return false;
	}

	// ------------------------------------------------------------------
	// Fixtures
	// ------------------------------------------------------------------

	struct DoorScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::Door> door;
	};

	// Two Rooms on adjacent Layers with one ordinary Door between them.
	DoorScene buildDoorScene(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, kWorldCellsWide,
			kWorldLevelsHigh);
		world->addRoom("Front", 0, 0, 0, kWorldCellsWide, 1);
		world->addRoom("Back", 1, 0, 0, kWorldCellsWide, 1);
		auto const created = world->addSectorDoor(0, 0, 3, {});
		world->finishBuild();
		DoorScene scene;
		scene.world = world;
		scene.door = std::static_pointer_cast<core::DoorSectorObject>(
			created.door.sector->getObject(created.door.index))->getDoor();
		require(bool(scene.door), "The Door fixture has no Door");
		return scene;
	}

	// Drives a Door to an exact open fraction through its own timing, so
	// the renderer reads a real device state rather than a remembered
	// number. The style is set first because the open/close duration is a
	// function of it.
	void driveTo(std::shared_ptr<core::Door> const& door,
		core::Door::OpenStyle style, float fraction)
	{
		door->setOpenStyle(style);
		door->close();
		while (!door->isClosed()) door->update(door->getOpenCloseTime());
		if (fraction <= 0.0f) return;
		door->open();
		door->update(door->getOpenCloseTime() * fraction);
		require(near(door->getOpenPercentage(), fraction),
			"The Door fixture could not be driven to " + std::to_string(fraction)
			+ " open, reached " + std::to_string(door->getOpenPercentage()));
	}

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

	// ------------------------------------------------------------------
	// The ticket's tiles: authored sizes, tintability, generated manifest
	// pair.
	// ------------------------------------------------------------------
	void authoredTilesAreRegisteredInObjectAtlas(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = loadNewObjectsAtlas(context);

		// The sheet is the 13x4 grid of 128x320 cells the set is sized for.
		// It was 6x3 until the vehicles arrived, the row has stayed since
		// #1031 dropped the Booth Window's undrawn closed panel, and #1056
		// widened it by two columns for the Door leaf families - each of
		// which owns its whole column, because `-Vn` derives at `baseRow +
		// n`. #1057 took it three columns further for the Lift and Shuttle
		// leaf families and #1058 two more for the tall pair, which own
		// their columns the same way. The image is sized for the art still
		// to come, not trimmed to the tile count of the moment (#1021).
		require(atlas.width == 1664 && atlas.height == 1280,
			"ObjectAtlas is not the 1664x1280 sheet its set is sized for, saw "
			+ std::to_string(atlas.width) + "x" + std::to_string(atlas.height));

		for (auto const& tile : kTiles)
		{
			auto const found = atlas.authored.sprites.find(tile.name);
			require(found != atlas.authored.sprites.end(),
				std::string("ObjectAtlas registers no '") + tile.name
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
			require(art.tintable == tile.tintable,
				std::string("'") + tile.name + "' tintability is "
				+ std::string(art.tintable ? "on" : "off") + ", expected "
				+ (tile.tintable ? "on" : "off"));
			// Every one of these draws from its own anchor with no render
			// offset: the object's own geometry places it.
			require(near(art.offset.x, 0.0f) && near(art.offset.y, 0.0f),
				std::string("'") + tile.name
				+ "' is drawn at the object's own anchor and declares no offset");

			// The generated ImageSet entry agrees with the authored tile, so
			// the runtime pixel contract cannot drift from what was authored.
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

		// No two tiles' artwork overlaps: two tiles sharing pixels would read
		// as one corrupted sprite, and a shutter that overlapped its frame
		// could never be told apart from it.
		for (auto const& first : atlas.authored.sprites)
			for (auto const& second : atlas.authored.sprites)
			{
				if (first.first == second.first) continue;
				auto const& a = first.second.region;
				auto const& b = second.second.region;
				auto const overlapX = std::min(a.x + a.width, b.x + b.width)
					- std::max(a.x, b.x);
				auto const overlapY = std::min(a.y + a.height, b.y + b.height)
					- std::max(a.y, b.y);
				require(overlapX <= 0 || overlapY <= 0,
					"'" + first.first + "' and '" + second.first
					+ "' overlap their artwork, so one of them has no clear cell");
			}

		// The two Button states are two different regions: the state is
		// carried by the art, not by a tint, so identical regions would make
		// the state invisible.
		auto const& enabledTile = atlas.authored.sprites.at("button-enabled").region;
		auto const& disabledTile = atlas.authored.sprites.at("button-disabled").region;
		require(enabledTile.x != disabledTile.x || enabledTile.y != disabledTile.y
				|| enabledTile.width != disabledTile.width
				|| enabledTile.height != disabledTile.height,
			"The enabled and disabled Button tiles are the same region, so the "
			"control's state cannot be read from its artwork");
	}

	// ------------------------------------------------------------------
	// Every leaf family carries three variants, and each variant is its own
	// full-cell tile rather than a re-use of its base's pixels (#1057).
	// ------------------------------------------------------------------
	void everyLeafFamilyCarriesThreeVariants(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = loadNewObjectsAtlas(context);

		// The families the tickets name. The ordinary pair landed in
		// #1056; the Lift pair and the Shuttle hatch in #1057; the tall
		// pair in #1058.
		for (auto const* base : { "door-L0", "door-R0", "door-tall-L0",
			"door-tall-R0", "door-lift-L0",
			"door-lift-R0", "door-shuttle-L0" })
		{
			auto const stated = atlas.authored.variantCounts.find(base);
			require(stated != atlas.authored.variantCounts.end(),
				std::string("Leaf family '") + base
				+ "' declares no variant family, so a Door could not pick a "
				"second picture of it");
			require(stated->second == 3,
				std::string("Leaf family '") + base + "' declares "
				+ std::to_string(stated->second)
				+ " variants, expected the three every leaf family carries");

			for (int variant = 1; variant <= 3; ++variant)
			{
				auto const name = std::string(base) + "-V" + std::to_string(variant);
				auto const found = atlas.authored.sprites.find(name);
				require(found != atlas.authored.sprites.end(),
					"Variant '" + name + "' of leaf family '" + base
					+ "' was not derived into the sheet");
				require(found->second.region.width == 128
						&& found->second.region.height == 320,
					"'" + name + "' is not a full-cell tile: "
					+ std::to_string(found->second.region.width) + "x"
					+ std::to_string(found->second.region.height));
				// A variant that shares its base's pixels is the same picture
				// twice, which is not a variant of anything.
				auto const& baseRegion = atlas.authored.sprites.at(base).region;
				require(found->second.region.x != baseRegion.x
						|| found->second.region.y != baseRegion.y,
					"'" + name + "' wears its base's pixels, so the family has "
					"one picture and three names");
				require(atlas.regions.count(name) > 0,
					"'" + name + "' has no generated ImageSet entry, so the "
					"runtime cannot install it");
			}
		}
	}

	// ------------------------------------------------------------------
	// The Door leaf slides under the aperture clip on every style. It is
	// never cropped from its own edge and never squashed: the animation is
	// a translation of a full-cell tile, and the clip does the cropping
	// (#1056).
	// ------------------------------------------------------------------
	void doorLeafSlidesUnderTheApertureOnEveryStyle(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();
		auto const& region = atlas.authored.sprites.at("door-L0").region;
		auto const leftU = tileLeftU(region, atlas.width);
		auto const rightU = tileRightU(region, atlas.width);
		auto const topV = tileTopV(region, atlas.height);
		auto const bottomV = tileBottomV(region, atlas.height);
		auto const spanU = rightU - leftU;
		auto const spanV = bottomV - topV;
		auto const midU = leftU + spanU * 0.5f;

		auto const scene = buildDoorScene("Door leaf artwork");
		core::Vector2 lo, hi;
		scene.door->getFullShape(lo, hi);
		float const apertureWidthPx = std::abs(hi.x - lo.x) * kCellWidth;
		float const apertureHeightPx = std::abs(hi.y - lo.y) * kLevelHeight;
		// The leaf's cell is the aperture widened back out by the inset on
		// each side, so a 1-wide Door's cell is a whole 128 x 320 cell.
		float const cellWidthPx = apertureWidthPx
			+ 2.0f * CORE_DOOR_X_INSET * kCellWidth;
		// A sideways leaf's visible edge travels one aperture width. The
		// inset the leaf's own cell starts outside the far jamb by is the
		// offset it starts from, not extra distance - which is the whole
		// of #1061: spending that inset at the start instead stalls the
		// visible edge behind the far jamb for it.
		float const sideTravelPx = apertureWidthPx;

		// No squashing: the leaf draws at the same pixels-per-texture-unit
		// the whole tile draws at, in both axes, however far it has slid.
		auto const drawnWithoutSquashing = [&](TileQuad const& leaf,
			std::string const& what)
		{
			require(relNear(leaf.rect.width() / (leaf.uvMaxX - leaf.uvMinX),
					cellWidthPx / spanU)
					&& relNear(leaf.rect.height() / (leaf.uvMaxY - leaf.uvMinY),
						kLevelHeight / spanV),
				what + " squashed its artwork: "
				+ point(leaf.rect.width() / (leaf.uvMaxX - leaf.uvMinX),
					leaf.rect.height() / (leaf.uvMaxY - leaf.uvMinY))
				+ " against " + point(cellWidthPx / spanU, kLevelHeight / spanV));
		};

		// The whole tile's uv band, never a band cut by the open fraction.
		auto const wearsTheWholeTile = [&](TileQuad const& leaf,
			std::string const& what)
		{
			require(relNear(leaf.uvMinX, leftU, 1e-4f)
					&& relNear(leaf.uvMaxX, rightU, 1e-4f)
					&& relNear(leaf.uvMinY, topV, 1e-4f)
					&& relNear(leaf.uvMaxY, bottomV, 1e-4f),
				what + " was cropped from its own edge instead of sliding whole "
				"under the aperture clip");
		};

		auto const render = [&](WorldDrawList& into)
		{
			RenderWorldScope scope(scene.world);
			renderDoor(scene.door, 0, LayerRenderStyle::Solid, false, &into);
		};

		auto const apartHalves = [&](WorldDrawList& from)
		{
			return std::pair<TileQuad, TileQuad>{
				tileQuad(from, region, atlas.width, atlas.height,
					[&](TileQuad const& q) { return q.uvMaxX < midU + 1e-4f; }),
				tileQuad(from, region, atlas.width, atlas.height,
					[&](TileQuad const& q) { return q.uvMinX > midU - 1e-4f; })};
		};

		for (auto const style : { core::Door::OpenStyle::OpenUp,
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight,
			core::Door::OpenStyle::OpenApart })
		{
			// Baseline: where the closed leaf sits, so the travel is read
			// as a delta rather than re-derived from the transform.
			driveTo(scene.door, style, 0.0f);
			WorldDrawList closed{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
			render(closed);

			float closedLeftX = 0.0f;
			float closedTopY = 0.0f;

			if (style == core::Door::OpenStyle::OpenApart)
			{
				// A 1-wide OpenApart Door splits its single L0 at the cell
				// centre and wears both halves - the art constraint nothing
				// can validate.
				auto const halves = apartHalves(closed);
				require(halves.first.drawn && halves.second.drawn,
					"A closed OpenApart Door lost one of its two half leaves");
				require(relNear(halves.first.uvMinX, leftU, 1e-4f)
						&& relNear(halves.first.uvMaxX, midU, 1e-4f),
					"The left OpenApart leaf does not wear the tile's left half");
				require(relNear(halves.second.uvMinX, midU, 1e-4f)
						&& relNear(halves.second.uvMaxX, rightU, 1e-4f),
					"The right OpenApart leaf does not wear the tile's right half");
				drawnWithoutSquashing(halves.first, "The left OpenApart leaf");
				drawnWithoutSquashing(halves.second, "The right OpenApart leaf");
				closedLeftX = halves.first.rect.left;
			}
			else
			{
				auto const leaf = tileQuad(closed, region, atlas.width, atlas.height);
				require(leaf.drawn,
					"A closed " + styleName(style) + " Door drew no artwork");
				wearsTheWholeTile(leaf, "The closed " + styleName(style) + " leaf");
				drawnWithoutSquashing(leaf,
					"The closed " + styleName(style) + " leaf");
				require(relNear(leaf.rect.width(), cellWidthPx)
						&& relNear(leaf.rect.height(), kLevelHeight),
					"The closed leaf is not drawn at its full cell rect");
				closedLeftX = leaf.rect.left;
				closedTopY = leaf.rect.top;
			}

			for (float const fraction : { 0.25f, 0.5f, 0.75f })
			{
				driveTo(scene.door, style, fraction);
				WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
				render(drawing);
				auto const label = styleName(style) + " at "
					+ std::to_string(fraction) + " open";

				if (style == core::Door::OpenStyle::OpenApart)
				{
					auto const halves = apartHalves(drawing);
					require(halves.first.drawn && halves.second.drawn,
						"An OpenApart Door lost one of its two leaves, " + label);
					// The crop never changes: each half keeps its half of the
					// art whatever the open percentage. The gap grows because
					// the halves translate, not because they are re-cropped.
					require(relNear(halves.first.uvMinX, leftU, 1e-4f)
							&& relNear(halves.first.uvMaxX, midU, 1e-4f)
							&& relNear(halves.second.uvMinX, midU, 1e-4f)
							&& relNear(halves.second.uvMaxX, rightU, 1e-4f),
						"An OpenApart leaf changed its crop as it slid, " + label);
					drawnWithoutSquashing(halves.first,
						"The left OpenApart leaf");
					drawnWithoutSquashing(halves.second,
						"The right OpenApart leaf");
					// Each half travels half the aperture width, outward.
					require(relNear(halves.first.rect.left - closedLeftX,
							-fraction * apertureWidthPx * 0.5f),
						"The left OpenApart leaf did not travel half the aperture "
						"width, " + label);
					continue;
				}

				auto const leaf = tileQuad(drawing, region, atlas.width, atlas.height);
				require(leaf.drawn, "The " + label + " Door drew no artwork");
				wearsTheWholeTile(leaf, "The " + label + " leaf");
				drawnWithoutSquashing(leaf, "The " + label + " leaf");
				// The rect never changes size - only where it sits.
				require(relNear(leaf.rect.width(), cellWidthPx)
						&& relNear(leaf.rect.height(), kLevelHeight),
					"A " + styleName(style) + " leaf changed its rect as it slid, "
					+ label);

				if (style == core::Door::OpenStyle::OpenUp)
				{
					require(relNear(leaf.rect.top - closedTopY,
							-fraction * apertureHeightPx),
						"An OpenUp leaf did not travel the aperture height, "
						+ label);
					require(relNear(leaf.rect.left, closedLeftX),
						"An OpenUp leaf moved sideways, which it never does, "
						+ label);
				}
				else if (style == core::Door::OpenStyle::OpenLeft)
				{
					require(relNear(leaf.rect.left - closedLeftX,
							-fraction * sideTravelPx),
						"An OpenLeft leaf did not travel the doorway clear, "
						+ label);
				}
				else
				{
					require(relNear(leaf.rect.left - closedLeftX,
							fraction * sideTravelPx),
						"An OpenRight leaf did not travel the doorway clear, "
						+ label);
				}
			}
		}
	}

	// ------------------------------------------------------------------
	// A fully open Door emits no leaf geometry on any style.
	// ------------------------------------------------------------------
	void aFullyOpenDoorEmitsNoLeafGeometry(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();
		auto const& region = atlas.authored.sprites.at("door-L0").region;
		// The fallback fill the renderer uses when no tile is installed. If
		// a fully open leaf emitted this, the doorway would look shut.
		auto const fallbackColour = packColour(64, 192, 255);

		for (auto const style : { core::Door::OpenStyle::OpenUp,
			core::Door::OpenStyle::OpenLeft, core::Door::OpenStyle::OpenRight,
			core::Door::OpenStyle::OpenApart })
		{
			auto const scene = buildDoorScene("Fully open " + styleName(style));
			driveTo(scene.door, style, 1.0f);
			require(scene.door->isOpen(), "The " + styleName(style)
				+ " Door fixture never reached Open");

			WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
			RenderWorldScope scope(scene.world);
			renderDoor(scene.door, 0, LayerRenderStyle::Solid, false, &drawing);
			require(!tileQuad(drawing, region, atlas.width, atlas.height).drawn,
				"A fully open " + styleName(style)
				+ " Door still drew leaf artwork, so the doorway looks shut");
			require(!anyPrimitiveFill(drawing, fallbackColour),
				"A fully open " + styleName(style)
				+ " Door fell back to a leaf fill, so the doorway looks shut");
		}
	}

	// ------------------------------------------------------------------
	// The Booth Window: fixed frame, shutter translating behind it.
	// ------------------------------------------------------------------
	void boothWindowShutterTranslatesBehindTheStationaryFrame(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto world = std::make_shared<core::World>("Booth artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addRoom("Front", 0, 0, 0, 7, 2);
		world->addRoom("Back", 1, 0, 0, 7, 2);
		auto const booth = std::static_pointer_cast<const core::BoothWindow>(
			world->addBoothWindow(0, 0, 3).object);
		require(bool(booth), "The Booth Window fixture has no Booth Window");
		world->finishBuild();

		auto const& frameRegion
			= atlas.authored.sprites.at("booth-window-open").region;
		auto const& shutterRegion
			= atlas.authored.sprites.at("booth-window-shutter").region;

		auto const frame = shapeToScreen(booth);
		ScreenRect const inner{
			frame.left + frame.width() * kFrameInsetXRatio,
			frame.top + frame.height() * kFrameInsetYRatio,
			frame.right - frame.width() * kFrameInsetXRatio,
			frame.bottom - frame.height() * kFrameInsetYRatio
		};
		float const innerHeight = inner.height();

		// The shutter's declared drawn size is the frame's transparent
		// centre, not the frame: that is the extent it slides across.
		auto const& shutterArt = atlas.authored.sprites.at("booth-window-shutter");
		require(relNear(shutterArt.drawnSize.x, inner.width() / kCellWidth)
				&& relNear(shutterArt.drawnSize.y, innerHeight / kLevelHeight),
			"The shutter tile's drawn size is not the frame's inner rect: "
			+ point(shutterArt.drawnSize.x, shutterArt.drawnSize.y) + " against "
			+ point(inner.width() / kCellWidth, innerHeight / kLevelHeight));

		auto const toggle = [&]
		{
			core::DeviceCommand command;
			command.type = core::DeviceCommandType::ToggleBoothWindow;
			command.boothWindow = booth->getDeviceId();
			require(bool(world->submitDeviceCommand(command)),
				"The Booth Window toggle was refused");
		};

		auto const drawAt = [&](float expected)
		{
			require(near(booth->getProgress(), expected),
				"The Booth Window fixture is at "
				+ std::to_string(booth->getProgress()) + ", expected "
				+ std::to_string(expected));
			auto const drawing = drawWorld(world);
			auto const drawnFrame = tileQuad(drawing, frameRegion, atlas.width,
				atlas.height);
			auto const drawnShutter = tileQuad(drawing, shutterRegion, atlas.width,
				atlas.height);
			require(drawnFrame.drawn, "The Booth Window drew no frame artwork");
			require(drawnShutter.drawn, "The Booth Window drew no shutter artwork");

			// The frame never moves. Its rect is the Booth Window's own shape
			// at every point in the shutter's travel.
			require(sameRect(drawnFrame.rect, frame),
				"The Booth Window frame moved with the shutter at "
				+ std::to_string(expected) + ": " + describe(drawnFrame.rect)
				+ " against " + describe(frame));

			// The shutter translates upward behind it by the inner height
			// times the live progress, and keeps its size while doing so.
			auto const expectedShutter = shifted(inner, 0.0f,
				-innerHeight * expected);
			require(sameRect(drawnShutter.rect, expectedShutter),
				"The Booth Window shutter was " + describe(drawnShutter.rect)
				+ " at " + std::to_string(expected) + ", expected "
				+ describe(expectedShutter));
			require(near(drawnShutter.rect.width(), inner.width())
					&& near(drawnShutter.rect.height(), innerHeight),
				"The Booth Window shutter changed size instead of translating");

			// Clipped to the centre: the shutter can never paint over the
			// frame it slides behind.
			require(near(drawnShutter.clip.minimum.x, inner.left)
					&& near(drawnShutter.clip.maximum.x, inner.right)
					&& near(drawnShutter.clip.minimum.y, inner.top)
					&& near(drawnShutter.clip.maximum.y, inner.bottom),
				"The Booth Window shutter escaped the clip to the frame's centre: "
				+ point(drawnShutter.clip.minimum.x, drawnShutter.clip.minimum.y)
				+ " -> " + point(drawnShutter.clip.maximum.x,
					drawnShutter.clip.maximum.y));

			// Draw order: the shutter goes down first and the frame lands on
			// top of it, which is what makes the frame stay fixed while the
			// shutter moves behind it.
			int frameIndex = -1, shutterIndex = -1, index = 0;
			for (auto const& command : drawing.commands())
			{
				++index;
				auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
					continue;
				if (triangleUsesRegion(*triangle, frameRegion, float(atlas.width),
					float(atlas.height))) frameIndex = index;
				if (triangleUsesRegion(*triangle, shutterRegion, float(atlas.width),
					float(atlas.height))) shutterIndex = index;
			}
			require(frameIndex > shutterIndex && shutterIndex > 0,
				"The Booth Window frame stopped drawing over its shutter");
		};

		drawAt(0.0f);
		toggle();
		require(world->advanceTicks(12), "Opening the Booth Window failed");
		drawAt(0.25f);
		require(world->advanceTicks(12), "Opening the Booth Window failed");
		drawAt(0.5f);
		require(world->advanceTicks(12), "Opening the Booth Window failed");
		drawAt(0.75f);
		require(world->advanceTicks(12), "Opening the Booth Window failed");
		drawAt(1.0f);
	}

	// ------------------------------------------------------------------
	// The Button's two states are two tiles at the control's drawn size.
	// ------------------------------------------------------------------
	void buttonStateIsLegibleAtTheDrawnSize(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto world = std::make_shared<core::World>("Button artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		world->addRoom("Front", 0, 0, 0, kWorldCellsWide, 1);
		world->addRoom("Back", 1, 0, 0, kWorldCellsWide, 1);
		auto const created = world->addSectorDoor(0, 0, 3, {});
		world->finishBuild();
		world->pauseSimulation();
		auto const frontIndex = created.door.sector->getIndex();
		world->addSectorDoorButton(frontIndex, created.door.index);
		world->finishBuild();

		auto const front = world->getSector(frontIndex);
		std::shared_ptr<core::Button> button;
		for (uint32_t i = 0; i < front->getNumObjects(); ++i)
			if (auto const found = std::dynamic_pointer_cast<core::Button>(
					front->getObject(i)->_getObject()); found) button = found;
		require(bool(button), "The Button fixture has no Button");

		auto const& enabledRegion
			= atlas.authored.sprites.at("button-enabled").region;
		auto const& disabledRegion
			= atlas.authored.sprites.at("button-disabled").region;
		// The control's own shape is what its artwork is drawn into, and at
		// this cell size that is a 12.8 x 12.8 px square.
		auto const shape = shapeToScreen(button);
		require(relNear(shape.width(), 0.1f * kCellWidth)
				&& relNear(shape.height(), 0.04f * kLevelHeight),
			"The Button's drawn extent is " + describe(shape)
			+ ", which is not the 0.1 x 0.04 World-unit control its tiles are "
			"authored for");

		auto const draw = [&]
		{
			WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
			RenderWorldScope scope(world);
			renderSector(front, 0, LayerRenderStyle::Solid, false,
				packColour(192, 192, 255), &drawing);
			return drawing;
		};

		{
			auto const drawing = draw();
			require(button->isEnabled(), "The fixture Button should start enabled");
			auto const art = tileQuad(drawing, enabledRegion, atlas.width,
				atlas.height);
			require(art.drawn, "The enabled Button drew no artwork");
			require(sameRect(art.rect, shape),
				"The enabled Button drew at " + describe(art.rect) + ", expected "
				+ describe(shape));
			require(!tileQuad(drawing, disabledRegion, atlas.width, atlas.height).drawn,
				"The enabled Button drew the disabled tile as well as its own");
			// The fallback fill would replace the artwork with a flat square,
			// which is the state this ticket ends.
			require(!anyPrimitiveFill(drawing, packColour(0, 255, 128)),
				"The enabled Button fell back to a flat fill instead of its tile");
		}

		button->disable();
		{
			auto const drawing = draw();
			require(!button->isEnabled(), "The fixture Button did not disable");
			auto const art = tileQuad(drawing, disabledRegion, atlas.width,
				atlas.height);
			require(art.drawn, "The disabled Button drew no artwork");
			require(sameRect(art.rect, shape),
				"The disabled Button drew at " + describe(art.rect) + ", expected "
				+ describe(shape));
			require(!tileQuad(drawing, enabledRegion, atlas.width, atlas.height).drawn,
				"The disabled Button still drew the enabled tile, so the state "
				"change is invisible");
			require(!anyPrimitiveFill(drawing, packColour(192, 128, 128)),
				"The disabled Button fell back to a flat fill instead of its tile");
		}
	}

	// ------------------------------------------------------------------
	// The Marker stays tintable, and selection still reads.
	// ------------------------------------------------------------------
	void markerStaysTintableAndFollowsSelection(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = installNewObjectsAtlas(context);
		prepareCanvas();

		auto world = std::make_shared<core::World>("Marker artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		// A Colour tag whose override is nothing like the Marker's gold, so
		// the two can be told apart: the tag moves the Agent and leaves the
		// destination where selection puts it.
		auto registry = core::AgentTagRegistry::create();
		auto const amber = registry->addAgentTag("amber");
		std::string diagnostic;
		require(registry->addAgentTagColour(amber, &diagnostic),
			"Could not add a Colour to the fixture tag: " + diagnostic);
		require(registry->setAgentTagColour(amber, { 200, 120, 20 }, &diagnostic),
			"Could not override the fixture tag's Colour: " + diagnostic);
		world->attachAgentTagRegistry("marker.tags.yaml", std::move(registry));
		auto const room = world->addRoom("Front", 0, 0, 0, kWorldCellsWide, 1);
		auto const created = world->addSectorMarker(room, 0, 3.5f, "Goal");
		world->finishBuild();
		auto const marker = std::static_pointer_cast<const core::MarkerSectorObject>(
			created.sector->getObject(created.index))->getMarker();
		require(bool(marker), "The Marker fixture has no Marker");

		auto const agentId = world->createAgent("Human", "Human", room, 0, 2.0f);
		auto const agent = world->lookupAgent(agentId).entity;
		require(agent != nullptr, "The Marker fixture has no Agent");
		world->pauseSimulation();
		auto const assigned = world->assignAgentTag(agentId, amber, &diagnostic);
		require(assigned, "Could not tag the fixture Agent: " + diagnostic);

		auto const& region = atlas.authored.sprites.at("marker").region;
		// The gold the renderer carries is the Agent selection colour, which
		// is what lets a destination read as part of the Agent colour
		// vocabulary rather than as a fixed decal.
		auto const gold = packColour(core::SelectedAgentColour.r,
			core::SelectedAgentColour.g, core::SelectedAgentColour.b);
		// The same seam the Agent body draws with. The Marker's gold is what
		// that seam resolves to for a selected Agent, and not what it resolves
		// to for this one's own tag override - which is the difference
		// between following the Agent colour vocabulary and copying an Agent.
		require(gold == agentRenderColour(*agent, true),
			"The Marker's gold is no longer what the Agent colour seam resolves "
			"to for a selection");
		require(agentRenderColour(*agent, false)
				== packColour(200, 120, 20),
			"The fixture Agent's Colour tag override stopped driving its resolved "
			"colour, so the comparison below proves nothing");
		require(!(gold == agentRenderColour(*agent, false)),
			"The Marker's gold matches this Agent's own resolved colour, so the "
			"selection tie below is not a real one");

		auto const draw = [&](bool selected)
		{
			WorldDrawList drawing{ { { 0, 0 }, { kCanvasWidth, kCanvasHeight } } };
			RenderWorldScope scope(world);
			renderMarker(marker, 0, LayerRenderStyle::Solid, selected, &drawing);
			return drawing;
		};

		{
			auto const drawing = draw(false);
			auto const art = tileQuad(drawing, region, atlas.width, atlas.height);
			require(art.drawn, "The Marker drew no artwork");
			require(art.colour == gold,
				"The Marker's tint is no longer the Agent selection gold it "
				"carries, so the tile stopped taking the tint");
			require(!(art.colour == White),
				"The Marker drew white: an untintable tile would wash the icon out");
			// The icon came from the atlas, not from the glyph fallback.
			require(!anyTextRun(drawing, pf::render::MarkerIconGlyph),
				"The Marker fell back to drawing its glyph, so the tile was not "
				"installed as artwork");
		}

		{
			auto const drawing = draw(true);
			auto const art = tileQuad(drawing, region, atlas.width, atlas.height);
			require(art.drawn && art.colour == gold,
				"A selected Marker stopped drawing its tinted artwork");
			// Selection still draws its own highlight around the icon, on top
			// of the artwork rather than instead of it.
			bool highlighted = false;
			for (auto const& command : drawing.commands())
				if (auto const* line = std::get_if<WorldDrawList::Line>(&command);
					line && line->colour == kSelectionColour)
					highlighted = true;
			require(highlighted, "A selected Marker lost its selection highlight");
		}

		// Tintability is the authored fact that carries the gold. Flip it off
		// the same tile and the icon goes white, which is how a regression in
		// the authored flag would show up on screen.
		{
			auto untinted = atlas.authored;
			untinted.sprites.at("marker").tintable = false;
			installAtlas(untinted, atlas.regions, "ObjectAtlas");
			auto const drawing = draw(false);
			auto const art = tileQuad(drawing, region, atlas.width, atlas.height);
			require(art.drawn, "The untintable Marker fixture drew no artwork");
			require(art.colour == White,
				"An untintable Marker still took the gold, so tintability is not "
				"the authored flag doing the work");
		}
	}
}

void render_smoke::registerObjectArtworkNew(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "objectArtworkNew/authoredTilesAreRegistered",
		isolated<authoredTilesAreRegisteredInObjectAtlas> });
	checks.push_back({ "objectArtworkNew/everyLeafFamilyCarriesThreeVariants",
		isolated<everyLeafFamilyCarriesThreeVariants> });
	checks.push_back({ "objectArtworkNew/doorLeafSlidesUnderTheApertureOnEveryStyle",
		isolated<doorLeafSlidesUnderTheApertureOnEveryStyle> });
	checks.push_back({ "objectArtworkNew/aFullyOpenDoorEmitsNoLeafGeometry",
		isolated<aFullyOpenDoorEmitsNoLeafGeometry> });
	checks.push_back({ "objectArtworkNew/boothWindowShutterTranslatesBehindTheStationaryFrame",
		isolated<boothWindowShutterTranslatesBehindTheStationaryFrame> });
	checks.push_back({ "objectArtworkNew/buttonStateIsLegibleAtTheDrawnSize",
		isolated<buttonStateIsLegibleAtTheDrawnSize> });
	checks.push_back({ "objectArtworkNew/markerStaysTintableAndFollowsSelection",
		isolated<markerStaysTintableAndFollowsSelection> });
}
