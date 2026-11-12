// The Bulkhead Door and the Access panel stop being placeholder rectangles
// and draw real artwork (#1019).
//
// The Bulkhead Door is a solid physical slab straddling the boundary
// between two adjacent Locations - its geometry puts 0.1 into one cell and
// 0.1 into the next - and it draws at its own 0.2 x 0.7 aspect ratio, not
// the cell's 2:5. Its open progress is read from the live device state, so
// a door half open shows exactly half open rather than whatever a frame
// counter had reached.
//
// The Access panel draws on the visible back surface of a Room, Corridor
// or Facade at its authored width, height and bottom offset, with its leaf
// retracting into the top edge as it opens.
//
// The Force Bridge deliberately stays a primitive line: its span is
// continuous and arbitrary, so a fixed tile would be wrong at most lengths.
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

#include "ObjectTileset.h"
#include "Render.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/AccessPanel.h"
#include "core/BulkheadDoor.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/ForceBridge.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
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

	std::string point(float x, float y)
	{
		return std::to_string(x) + "," + std::to_string(y);
	}

	// The World cell's screen size at the zoom every check here uses.
	float const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	float const kLevelHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// The fixture Worlds are 12 cells by 3 Levels, which is what the Bulkhead
	// Door fixture needs for its two Rooms and what the panel fixture draws
	// against.
	uint32_t const kWorldCellsWide = 12;
	uint32_t const kWorldLevelsHigh = 3;
	float const kCanvasWidth = float(kWorldCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kWorldLevelsHigh) * kLevelHeight;

	// The Bulkhead Door fixture: two Rooms meeting at the cell 4 / cell 5
	// boundary, with the door placed at that shared open end. The slab's own
	// geometry then straddles that boundary, 0.1 into each cell.
	float const kBulkheadCellX = 4.0f;
	float const kBulkheadLevelY = 0.0f;
	float const kSlabWidth = 0.2f;
	float const kSlabHeight = 0.7f;
	// The authored content rectangle the parent spec states for this tile.
	int const kSlabContentWidth = 26;
	int const kSlabContentHeight = 224;

	// The Access panel's default authored geometry: 0.5 wide, 0.25 high,
	// sitting 0.25 above its cell floor - 64 x 80px on the 128x320 grid.
	float const kPanelWidth = 0.5f;
	float const kPanelHeight = 0.25f;
	float const kPanelBottomOffset = 0.25f;
	int const kPanelContentWidth = 64;
	int const kPanelContentHeight = 80;
	uint32_t const kPanelCellX = 3;
	uint32_t const kPanelLevel = 0;
	// A speed that moves the leaf exactly half its travel in one fixed tick,
	// so the half-open state is exact rather than approximate.
	float const kPanelHalfTickSpeed = 7.5f;

	struct ScreenRect
	{
		float left{}, top{}, right{}, bottom{};
	};

	// The screen rectangle a World-unit rectangle maps to at this check's
	// viewport. World y grows up, screen y grows down, so the World's top
	// edge is the screen's smaller y.
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

	bool sameRect(ScreenRect const& a, ScreenRect const& b)
	{
		return near(a.left, b.left) && near(a.top, b.top)
			&& near(a.right, b.right) && near(a.bottom, b.bottom);
	}

	std::string describe(ScreenRect const& r)
	{
		return point(r.left, r.top) + " -> " + point(r.right, r.bottom);
	}

	uint32_t bigEndian32(unsigned char const* bytes)
	{
		return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16)
			| (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
	}

	// The paired `Image` resource's own pixels, read straight from the PNG
	// header: the file the manifest points at, not a second hand-written
	// claim about it.
	void requirePngSize(std::filesystem::path const& path, int width, int height)
	{
		std::ifstream in(path, std::ios::binary);
		require(bool(in), "Could not open " + path.string());
		unsigned char header[24]{};
		in.read(reinterpret_cast<char*>(header), 24);
		require(in.gcount() == 24, path.string() + " is too short to be a PNG");
		require(header[0] == 0x89 && header[1] == 'P' && header[2] == 'N'
			&& header[3] == 'G', path.string() + " is not a PNG");
		require(int(bigEndian32(header + 16)) == width
			&& int(bigEndian32(header + 20)) == height,
			path.string() + " is not the size its authored tileset states");
	}

	struct ManifestResource
	{
		bool found{ false };
		std::string type;
		std::string location;
		std::string imageReference;
		std::map<std::string, std::string> options;
		AtlasPixelRegions regions;
	};

	// One entry from the application asset manifest, read the way the
	// resource layer reads it: Options become tags, Definitions carry the
	// ImageSet's pixel regions. Each step is guarded on its own because a
	// yaml-cpp node that was never written is a zombie, and subscripting one
	// throws rather than reading as absent.
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
				{
					if (!option.IsMap()) continue;
					found.options[option["name"].as<std::string>()] =
						option["value"].as<std::string>();
				}
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

	// One entry of the authored atlas-set registry: the tileset behind an
	// atlas and the authored format it is written in.
	struct AtlasSetEntry
	{
		std::string kind;
		std::string tileset;
	};

	std::map<std::string, AtlasSetEntry> atlasSetRegistry(
		smoke::Context const& context)
	{
		auto const path = context.fixture("resources/atlas-sets.yaml");
		auto const root = YAML::LoadFile(path.string());
		require(root.IsMap() && root["sets"].IsMap(),
			"resources/atlas-sets.yaml has no `sets` map of atlas name to authored "
			"tileset");
		std::map<std::string, AtlasSetEntry> sets;
		for (auto const& entry : root["sets"])
		{
			auto const value = entry.second;
			require(value.IsMap(), "The registry entry for "
				+ entry.first.as<std::string>()
				+ " is not a map of `kind` and `tileset`");
			sets[entry.first.as<std::string>()] = AtlasSetEntry{
				value["kind"].as<std::string>(),
				value["tileset"].as<std::string>() };
		}
		return sets;
	}

	// The tileset file the registry names for one atlas, as a
	// repository-relative fixture path.
	std::string authoredTilesetFor(smoke::Context const& context,
		std::string const& atlas)
	{
		auto const sets = atlasSetRegistry(context);
		auto const found = sets.find(atlas);
		require(found != sets.end(),
			"resources/atlas-sets.yaml names no tileset for " + atlas);
		return "resources/" + found->second.tileset;
	}

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
		require(atlasSetRegistry(context).at("ObjectAtlas").tileset
			== "textures/objects-new.tileset.yaml",
			"The atlas-set registry does not name objects-new.tileset.yaml for "
			"ObjectAtlas");
		auto const image = manifestResource(manifest, set.imageReference);
		require(image.found && image.type == "Image",
			"ObjectAtlas's paired Image resource is missing");

		NewObjectsAtlas atlas;
		atlas.authored = ObjectTileset::load(
			context.fixture(authoredTilesetFor(context, "ObjectAtlas")));
		atlas.regions = set.regions;
		atlas.width = atlas.authored.width;
		atlas.height = atlas.authored.height;
		requirePngSize(context.fixture("resources/" + image.location), atlas.width,
			atlas.height);
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

	void installNewObjectsAtlas(smoke::Context const& context)
	{
		auto const atlas = loadNewObjectsAtlas(context);
		installAtlas(atlas.authored, atlas.regions, "ObjectAtlas");
	}

	// ------------------------------------------------------------------
	// Fixtures
	// ------------------------------------------------------------------

	struct BulkheadFixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::BulkheadDoor> door;
	};

	// Two Rooms meeting at the cell 4 / cell 5 boundary on Layer 0, with a
	// Bulkhead Door at that shared open end. The Door object is registered on
	// both Sectors, exactly as it is in a real World.
	BulkheadFixture buildBulkheadFixture()
	{
		BulkheadFixture fixture;
		fixture.world = std::make_shared<core::World>("Bulkhead artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		fixture.world->addRoom("Left", 0, 0, 0, 5, 1);
		fixture.world->addRoom("Right", 0, 0, 5, 6, 1);
		auto const made = fixture.world->addSectorBulkheadDoor(0, 0, 5,
			CORE_SIDE_LEFT);
		fixture.door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(
			made.door.sector->getObject(made.door.index))->getDoor();
		require(bool(fixture.door), "The Bulkhead Door fixture has no Door");
		fixture.world->finishBuild();
		fixture.world->pauseSimulation();
		return fixture;
	}

	struct PanelFixture
	{
		std::shared_ptr<core::World> world;
		uint32_t room{};
		std::shared_ptr<const core::AccessPanel> panel;
	};

	// A Room with one Access panel on its back surface. The speed is chosen
	// so one fixed tick moves the leaf exactly half its travel.
	PanelFixture buildPanelFixture(core::AccessPanelGeometry geometry,
		std::optional<float> speed = {})
	{
		PanelFixture fixture;
		fixture.world = std::make_shared<core::World>("Access panel artwork",
			kWorldCellsWide, kWorldLevelsHigh);
		fixture.room = fixture.world->addRoom("Back", 0, 0, 0, 8, 3);
		auto const created = fixture.world->addAccessPanel(fixture.room,
			kPanelLevel, kPanelCellX, geometry, speed);
		require(created.type == core::SectorObjectType::AccessPanel,
			"The fixture placement did not create an Access panel");
		fixture.world->finishBuild();
		fixture.panel = std::static_pointer_cast<const core::AccessPanelSectorObject>(
			fixture.world->getSector(fixture.room)->getObject(created.index))->getPanel();
		require(bool(fixture.panel), "The fixture SectorObject carries no panel");
		return fixture;
	}

	struct BridgeFixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::ForceBridge> bridge;
	};

	// A Corridor with a two-cell Force Bridge spanning the air between two
	// Walkways, which is what gives the span its clear-air requirement.
	BridgeFixture buildBridgeFixture()
	{
		BridgeFixture fixture;
		fixture.world = std::make_shared<core::World>("Force bridge stays a line",
			kWorldCellsWide, kWorldLevelsHigh);
		auto const room = fixture.world->addRoom("Span", 0, 0, 0, 8, 3);
		fixture.world->addSectorWalkway(room, 1, 1);
		fixture.world->addSectorWalkway(room, 1, 4);
		auto const made = fixture.world->addSectorForceBridge(room, 1, 2,
			core::World::CreateForceBridgeOptions{ 2, CORE_SIDE_LEFT, true, true, 2 });
		fixture.bridge = std::static_pointer_cast<const core::ForceBridgeSectorObject>(
			made.forceBridge.sector->getObject(made.forceBridge.index))->getForceBridge();
		require(bool(fixture.bridge), "The Force Bridge fixture has no bridge");
		fixture.world->finishBuild();
		fixture.world->pauseSimulation();
		return fixture;
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

	struct AtlasExtent
	{
		unsigned triangles{};
		float left{}, top{}, right{}, bottom{};
		// The vertical texture range the artwork was cut from, which is what
		// tells a check the art slid out of view instead of being squashed
		// into the part still showing.
		float textureMinY{}, textureMaxY{};

		bool painted() const { return triangles > 0; }
		ScreenRect rect() const { return { left, top, right, bottom }; }
	};

	// The object-atlas triangles overlapping a screen area, as one bounding
	// box. A Bulkhead Door is registered on both Sectors it separates, so
	// the stream carries its quad twice; every check here asks about the
	// rectangle the art covers, not about how many times it was drawn.
	AtlasExtent objectAtlasExtent(WorldDrawList const& drawing, ScreenRect const& area)
	{
		AtlasExtent extent;
		extent.left = extent.top = 1e9f;
		extent.right = extent.bottom = -1e9f;
		extent.textureMinY = 1e9f;
		extent.textureMaxY = -1e9f;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
				continue;
			auto const minX = std::min({ triangle->positions[0].x,
				triangle->positions[1].x, triangle->positions[2].x });
			auto const maxX = std::max({ triangle->positions[0].x,
				triangle->positions[1].x, triangle->positions[2].x });
			auto const minY = std::min({ triangle->positions[0].y,
				triangle->positions[1].y, triangle->positions[2].y });
			auto const maxY = std::max({ triangle->positions[0].y,
				triangle->positions[1].y, triangle->positions[2].y });
			if (maxX <= area.left + 1.0f || minX >= area.right - 1.0f) continue;
			if (maxY <= area.top + 1.0f || minY >= area.bottom - 1.0f) continue;
			++extent.triangles;
			extent.left = std::min(extent.left, minX);
			extent.top = std::min(extent.top, minY);
			extent.right = std::max(extent.right, maxX);
			extent.bottom = std::max(extent.bottom, maxY);
			for (auto const& uv : triangle->texcoords)
			{
				extent.textureMinY = std::min(extent.textureMinY, uv.y);
				extent.textureMaxY = std::max(extent.textureMaxY, uv.y);
			}
		}
		return extent;
	}

	bool anyLine(WorldDrawList const& drawing, PackedColourValue colour)
	{
		for (auto const& command : drawing.commands())
			if (auto const* line = std::get_if<WorldDrawList::Line>(&command);
				line && line->colour == colour)
				return true;
		return false;
	}

	// ------------------------------------------------------------------
	// The authored tiles: sizes, offsets and the generated manifest pair.
	// ------------------------------------------------------------------
	void newTilesAreAuthoredAtTheirStatedContentSizes(smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = loadNewObjectsAtlas(context);

		auto const slab = atlas.authored.sprites.find("bulkhead-door");
		require(slab != atlas.authored.sprites.end(),
			"The authored ObjectAtlas tileset has no bulkhead-door tile");
		// The slab draws at its own 0.2 x 0.7 aspect ratio. The cell is 2:5,
		// so a tile drawn at the cell's shape would be five times too wide.
		require(near(slab->second.drawnSize.x, kSlabWidth)
			&& near(slab->second.drawnSize.y, kSlabHeight),
			"The Bulkhead Door tile must be drawn at 0.2 x 0.7, saw "
			+ point(slab->second.drawnSize.x, slab->second.drawnSize.y));
		require(near(slab->second.offset.x, 0.0f)
			&& near(slab->second.offset.y, 0.0f),
			"The Bulkhead Door tile draws from the door's own anchor, so it "
			"straddles the cell boundary exactly where the collision does");
		require(slab->second.region.width == kSlabContentWidth
			&& slab->second.region.height == kSlabContentHeight,
			"The Bulkhead Door content rectangle must be 26 x 224, saw "
			+ std::to_string(slab->second.region.width) + " x "
			+ std::to_string(slab->second.region.height));

		auto const panel = atlas.authored.sprites.find("access-panel");
		require(panel != atlas.authored.sprites.end(),
			"The authored ObjectAtlas tileset has no access-panel tile");
		require(near(panel->second.drawnSize.x, kPanelWidth)
			&& near(panel->second.drawnSize.y, kPanelHeight),
			"The Access panel tile's reference size must be 0.5 x 0.25, saw "
			+ point(panel->second.drawnSize.x, panel->second.drawnSize.y));
		require(panel->second.region.width == kPanelContentWidth
			&& panel->second.region.height == kPanelContentHeight,
			"The Access panel content rectangle must be 64 x 80, saw "
			+ std::to_string(panel->second.region.width) + " x "
			+ std::to_string(panel->second.region.height));

		// The generated ImageSet entries are generated from the tileset, so
		// the manifest cannot drift from the art it claims to cut.
		for (auto const* tile : { "bulkhead-door", "access-panel" })
		{
			auto const region = atlas.regions.find(tile);
			require(region != atlas.regions.end(),
				std::string("Authored tile '") + tile
				+ "' has no generated ImageSet entry");
			auto const& authored = atlas.authored.sprites.at(tile);
			require(region->second.x == authored.region.x
				&& region->second.y == authored.region.y
				&& region->second.width == authored.region.width
				&& region->second.height == authored.region.height,
				std::string("Generated ImageSet entry for '") + tile
				+ "' drifted from the authored tileset");
		}
	}

	// ------------------------------------------------------------------
	// The Bulkhead Door draws its slab, straddling the boundary.
	// ------------------------------------------------------------------
	void bulkheadDoorDrawsItsSlabAcrossTheCellBoundary(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto const fixture = buildBulkheadFixture();

		// The slab's own geometry: 0.1 into each of the two cells it
		// separates, over the full Corridor height.
		core::Vector2 full0, full1;
		fixture.door->getFullShape(full0, full1);
		require(near(full0.x, kBulkheadCellX + 0.9f)
			&& near(full1.x, kBulkheadCellX + 1.1f),
			"The Bulkhead Door must straddle its boundary 0.1 into each cell, saw "
			+ point(full0.x, full1.x));
		require(near(full1.y - full0.y, kSlabHeight),
			"The Bulkhead Door is as tall as a Corridor");

		prepareCanvas();
		auto const drawing = drawWorld(fixture.world);
		auto const expected = worldToScreen(kBulkheadCellX + 0.9f, kBulkheadLevelY,
			kSlabWidth, kSlabHeight);
		auto const drawn = objectAtlasExtent(drawing, expected);
		require(drawn.painted(),
			"The Bulkhead Door drew no object-atlas artwork into the World");
		require(sameRect(drawn.rect(), expected),
			"Bulkhead Door slab was " + describe(drawn.rect()) + " expected "
			+ describe(expected));

		// Its own aspect ratio, not the cell's 2:5.
		float const drawnWidth = drawn.right - drawn.left;
		float const drawnHeight = drawn.bottom - drawn.top;
		require(near(drawnWidth, kSlabWidth * kCellWidth)
			&& near(drawnHeight, kSlabHeight * kLevelHeight),
			"The slab must be 25.6 x 224 screen pixels, saw "
			+ point(drawnWidth, drawnHeight));
		require(drawnWidth < kCellWidth * 0.5f,
			"The slab drew at a cell-shaped width rather than its own 0.2");

		// It straddles: the boundary between the two cells it separates runs
		// through the middle of the drawn slab, 12.8px of art on each side.
		float const boundary = (kBulkheadCellX + 1.0f) * kCellWidth;
		require(drawn.left < boundary - 12.0f && drawn.right > boundary + 12.0f,
			"The slab does not straddle the boundary between the two Locations");
		require(near(boundary - drawn.left, drawn.right - boundary),
			"The slab must sit evenly across the boundary it separates");
	}

	// ------------------------------------------------------------------
	// Open progress is the device's own state, not a frame counter.
	// ------------------------------------------------------------------
	void bulkheadDoorProgressIsSyncedToTheDeviceState(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto const atlas = loadNewObjectsAtlas(context);
		auto const& slabRegion = atlas.authored.sprites.at("bulkhead-door").region;
		// The texture coordinate the source crop starts at for a given
		// retraction fraction, computed the way the sprite cutter does it: a
		// half-texel inset, then the fraction of the region's inner span.
		auto cropV = [&](float fraction) {
			return (float(slabRegion.y) + 0.5f
				+ fraction * float(slabRegion.height - 1)) / float(atlas.height);
		};

		// Each case drives the live Door to a stated open fraction by
		// advancing it a stated number of times, then asks whether the drawn
		// slab is exactly the part of the declared extent that is still down.
		// A renderer tracking a frame counter rather than the device state
		// could not satisfy all of these rows, because the same fraction is
		// reached through different numbers of updates.
		struct Case
		{
			float fraction;
			int updates;
			float perUpdate;
		};

		for (auto const& testCase : { Case{ 0.0f, 0, 0.0f }, Case{ 0.5f, 1, 0.5f },
			Case{ 0.5f, 2, 0.25f }, Case{ 0.25f, 1, 0.25f },
			Case{ 0.75f, 3, 0.25f } })
		{
			auto const fixture = buildBulkheadFixture();
			if (testCase.updates > 0)
			{
				fixture.door->open();
				for (int i = 0; i < testCase.updates; ++i)
					fixture.door->update(testCase.perUpdate
						* fixture.door->getOpenCloseTime());
			}
			require(near(fixture.door->getOpenPercentage(), testCase.fraction),
				"Fixture drive did not reach "
				+ std::to_string(testCase.fraction) + ", saw "
				+ std::to_string(fixture.door->getOpenPercentage()));

			prepareCanvas();
			auto const drawing = drawWorld(fixture.world);
			// The full declared extent, so the probe area covers the whole
			// slab column whatever is left of it.
			ScreenRect const column = worldToScreen(kBulkheadCellX + 0.9f,
				kBulkheadLevelY, kSlabWidth, kSlabHeight);
			auto const drawn = objectAtlasExtent(drawing, column);
			require(drawn.painted(),
				"The Bulkhead Door drew no artwork at fraction "
				+ std::to_string(testCase.fraction));

			// The top of the extent never moves: the slab retracts upward.
			require(near(drawn.top, column.top),
				"The slab must stay pinned to the top of its extent, saw "
				+ std::to_string(drawn.top) + " expected "
				+ std::to_string(column.top));
			// The visible height is exactly what is still down.
			float const expectedVisible = kSlabHeight * kLevelHeight
				* (1.0f - testCase.fraction);
			float const drawnHeight = drawn.bottom - drawn.top;
			require(near(drawnHeight, expectedVisible),
				"At " + std::to_string(testCase.fraction) + " open the slab drew "
				+ std::to_string(drawnHeight) + "px of a "
				+ std::to_string(kSlabHeight * kLevelHeight)
				+ "px extent, expected " + std::to_string(expectedVisible));
			// Half open is exactly half the full extent, on the command vector.
			if (near(testCase.fraction, 0.5f))
				require(near(drawnHeight, kSlabHeight * kLevelHeight * 0.5f),
					"A half-open Bulkhead Door must show exactly half open");
			// The slab never leaves its own column.
			require(drawn.left >= column.left - 0.01f
				&& drawn.right <= column.right + 0.01f,
				"The slab drew outside its own 0.2-wide column");
			// The artwork is cropped from the live state, so the slab slides out
			// of view through the top edge instead of being squashed into the
			// part still showing.
			require(near(drawn.textureMinY, cropV(testCase.fraction)),
				"At " + std::to_string(testCase.fraction) + " open the slab's art must be "
				"cropped from the device state, saw v "
				+ std::to_string(drawn.textureMinY) + " expected "
				+ std::to_string(cropV(testCase.fraction)));
			require(near(drawn.textureMaxY, cropV(1.0f)),
				"The slab's art must always run to the bottom of its tile region, saw v "
				+ std::to_string(drawn.textureMaxY));
		}
	}

	// A fully open Bulkhead Door is fully up: nothing of the slab is left to
	// draw, and the renderer emits no artwork rather than a zero-height
	// sliver of the tile.
	void fullyOpenBulkheadDoorDrawsNoSlab(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto const fixture = buildBulkheadFixture();
		fixture.door->open();
		fixture.door->update(fixture.door->getOpenCloseTime());
		require(near(fixture.door->getOpenPercentage(), 1.0f),
			"The fixture door did not reach fully open");

		prepareCanvas();
		auto const drawing = drawWorld(fixture.world);
		ScreenRect const column = worldToScreen(kBulkheadCellX + 0.9f,
			kBulkheadLevelY, kSlabWidth, kSlabHeight);
		require(!objectAtlasExtent(drawing, column).painted(),
			"A fully open Bulkhead Door still drew slab artwork");
	}

	// ------------------------------------------------------------------
	// The Access panel draws at its authored width, height and bottom offset.
	// ------------------------------------------------------------------
	void accessPanelDrawsAtItsAuthoredGeometry(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);

		// The default geometry, and a deliberately non-default one, to show
		// the artwork follows the panel's authored rect and not a fixed tile
		// size: an Access panel's geometry is authored per panel.
		struct Case
		{
			core::AccessPanelGeometry geometry;
			float expectedWidthPixels, expectedHeightPixels;
		};

		for (auto const& testCase : {
			Case{ core::AccessPanelGeometry{}, kPanelWidth * kCellWidth,
				kPanelHeight * kLevelHeight },
			Case{ core::AccessPanelGeometry{ 1.0f, 0.5f, 0.5f },
				1.0f * kCellWidth, 0.5f * kLevelHeight } })
		{
			auto const fixture = buildPanelFixture(testCase.geometry);
			auto const& geometry = fixture.panel->getGeometry();
			// The authored rect in World units: centred in the cell across
			// its width, sitting its authored bottom offset above the floor.
			float const worldLeft = float(kPanelCellX) + (1.0f - geometry.width) * 0.5f;
			float const worldBottom = float(kPanelLevel) + geometry.yOffset;
			auto const expected = worldToScreen(worldLeft, worldBottom,
				geometry.width, geometry.height);
			require(near(expected.right - expected.left,
				testCase.expectedWidthPixels),
				"The panel's drawn width must follow its authored width");
			require(near(expected.bottom - expected.top,
				testCase.expectedHeightPixels),
				"The panel's drawn height must follow its authored height");

			prepareCanvas();
			auto const drawing = drawWorld(fixture.world);
			auto const drawn = objectAtlasExtent(drawing, expected);
			require(drawn.painted(),
				"The Access panel drew no object-atlas artwork at its authored rect");
			require(sameRect(drawn.rect(), expected),
				"Access panel artwork was " + describe(drawn.rect())
				+ " expected " + describe(expected));
			// The bottom offset is the panel's: the art sits above the cell
			// floor by exactly the authored amount.
			float const cellFloor = kCanvasHeight - float(kPanelLevel) * kLevelHeight;
			require(near(cellFloor - drawn.bottom,
				geometry.yOffset * kLevelHeight),
				"The panel must sit its authored bottom offset above the cell floor, saw "
				+ std::to_string(cellFloor - drawn.bottom));
		}
	}

	// ------------------------------------------------------------------
	// The leaf retracts into the top edge, tracking progress.
	// ------------------------------------------------------------------
	void accessPanelLeafRetractsIntoTheTopEdge(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);

		// Closed: the whole authored rect is leaf.
		auto const fixture = buildPanelFixture(core::AccessPanelGeometry{},
			kPanelHalfTickSpeed);
		require(near(fixture.panel->getProgress(), 0.0f),
			"A fresh Access panel must start closed");

		auto const geometry = fixture.panel->getGeometry();
		float const worldLeft = float(kPanelCellX) + (1.0f - geometry.width) * 0.5f;
		float const worldBottom = float(kPanelLevel) + geometry.yOffset;
		ScreenRect const full = worldToScreen(worldLeft, worldBottom,
			geometry.width, geometry.height);

		prepareCanvas();
		auto closed = objectAtlasExtent(drawWorld(fixture.world), full);
		require(closed.painted() && sameRect(closed.rect(), full),
			"A closed panel must draw its leaf over its whole authored rect");

		// Open it: one fixed tick at this speed is exactly half travel.
		auto actor = fixture.world->createAgent("Operator", fixture.room,
			kPanelLevel, float(kPanelCellX) + 0.5f);
		require(bool(fixture.world->requestAccessPanel(fixture.panel->getId(),
			core::AccessPanel::Action::Open, actor)),
			"The half-open request was refused");
		fixture.world->resumeSimulation();
		// The first tick activates the press; travel starts on the next one.
		// Two ticks at this speed is exactly half the leaf's travel.
		fixture.world->advanceTicks(2);
		float const progress = fixture.panel->getProgress();
		require(progress > 0.0f && progress < 1.0f,
			"The panel should be part-open after two ticks, saw "
			+ std::to_string(progress));
		require(near(progress, 0.5f),
			"The fixture speed should give exactly half travel, saw "
			+ std::to_string(progress));

		prepareCanvas();
		auto const half = objectAtlasExtent(drawWorld(fixture.world), full);
		require(half.painted(), "A part-open panel drew no leaf artwork");
		// The leaf retracts into the top edge: the top stays put, the bottom
		// rises, and what is left is exactly the un-retracted fraction.
		require(near(half.top, full.top),
			"The leaf must retract into the top edge, leaving the top edge in place");
		require(near(half.bottom, full.top + (full.bottom - full.top) * (1.0f - progress)),
			"The leaf's bottom must sit at the live progress, saw "
			+ std::to_string(half.bottom));
		require(near(half.bottom - half.top, (full.bottom - full.top) * (1.0f - progress)),
			"The leaf must show exactly the fraction still extended");
		require(near(half.bottom - half.top, (full.bottom - full.top) * 0.5f),
			"A half-open leaf must be exactly half the authored height");
		// The leaf's artwork is cropped from the live progress, so it slides
		// into the top edge rather than being squashed into the strip still
		// showing. The crop is computed the way the sprite cutter computes it.
		auto const atlas = loadNewObjectsAtlas(context);
		auto const& panelRegion = atlas.authored.sprites.at("access-panel").region;
		auto const cropMinV = (float(panelRegion.y) + 0.5f
			+ progress * float(panelRegion.height - 1)) / float(atlas.height);
		auto const cropMaxV = (float(panelRegion.y) + 0.5f
			+ float(panelRegion.height - 1)) / float(atlas.height);
		require(near(half.textureMinY, cropMinV),
			"The leaf's art must be cropped from the live progress, saw v "
			+ std::to_string(half.textureMinY) + " expected "
			+ std::to_string(cropMinV));
		require(near(half.textureMaxY, cropMaxV),
			"The leaf's art must run to the bottom of its tile region, saw v "
			+ std::to_string(half.textureMaxY));
		// The bottom of the authored rect is revealed, not painted over.
		require(half.bottom < full.bottom - 1.0f,
			"The bottom of the panel was not revealed as the leaf retracted");

		// Fully open: no leaf artwork at all.
		fixture.world->advanceTicks(120);
		require(fixture.panel->getState() == core::AccessPanel::State::Open,
			"The panel did not reach Open");
		prepareCanvas();
		require(!objectAtlasExtent(drawWorld(fixture.world), full).painted(),
			"A fully open panel still drew leaf artwork");
	}

	// ------------------------------------------------------------------
	// The Force Bridge stays a primitive line and takes no tile.
	// ------------------------------------------------------------------
	void forceBridgeStillRendersAsAPrimitiveLine(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto const fixture = buildBridgeFixture();

		// The bridge's own span, probed above its line so the Walkway slabs
		// that support it - which hang below their floor line - cannot be
		// mistaken for bridge artwork.
		core::Vector2 span0, span1;
		fixture.bridge->getFullShape(span0, span1);
		require(span1.x > span0.x, "The Force Bridge fixture spans no width");
		ScreenRect const span = worldToScreen(span0.x, span0.y, span1.x - span0.x, 0.5f);

		prepareCanvas();
		auto const drawing = drawWorld(fixture.world);
		// The bridge is a line, so it contributes no object-atlas geometry
		// across its span at all - not a stretched tile, not a repeated one.
		require(objectAtlasExtent(drawing, span).triangles == 0,
			"The Force Bridge took tile artwork; it must stay a primitive line");
		require(anyLine(drawing, packColour(0, 255, 0)),
			"The Force Bridge no longer draws its primitive energy line");
	}

	// The refusal is what keeps the decision from being reversed by a tile
	// added to a sheet nobody thought about.
	void loaderRefusesAForceBridgeTile(smoke::Context const& context)
	{
		using smoke::require;
		auto const path = context.temporaryRoot() / "force-bridge.tileset.yaml";
		{
			std::ofstream out(path);
			require(bool(out), "Could not write the refusal fixture");
			out << "version: 2\n"
				<< "image: objects-new.png\n"
				<< "size: [768, 960]\n"
				<< "tile_size: [128, 320]\n"
				<< "sprites:\n"
				<< "  force-bridge:\n"
				<< "    cell: [0, 0]\n"
				<< "    content: [0, 0, 128, 8]\n"
				<< "    offset: [0.0, 0.0]\n"
				<< "    drawn_size: [1.0, 0.025]\n";
		}
		bool refused = false;
		try
		{
			ObjectTileset::load(path);
		}
		catch (std::exception const& error)
		{
			refused = true;
			require(std::string(error.what()).find("primitive") != std::string::npos,
				std::string("The refusal must say why: ") + error.what());
		}
		require(refused, "A version 2 object tileset with a force-bridge tile was accepted");
	}

}

void render_smoke::registerBulkheadAndPanelArtwork(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "bulkheadAndPanel/newTilesAreAuthoredAtTheirContentSizes",
		isolated<newTilesAreAuthoredAtTheirStatedContentSizes> });
	checks.push_back({ "bulkheadAndPanel/bulkheadDoorStraddlesTheCellBoundary",
		isolated<bulkheadDoorDrawsItsSlabAcrossTheCellBoundary> });
	checks.push_back({ "bulkheadAndPanel/bulkheadDoorProgressIsSyncedToTheDeviceState",
		isolated<bulkheadDoorProgressIsSyncedToTheDeviceState> });
	checks.push_back({ "bulkheadAndPanel/fullyOpenBulkheadDoorDrawsNoSlab",
		isolated<fullyOpenBulkheadDoorDrawsNoSlab> });
	checks.push_back({ "bulkheadAndPanel/accessPanelDrawsAtItsAuthoredGeometry",
		isolated<accessPanelDrawsAtItsAuthoredGeometry> });
	checks.push_back({ "bulkheadAndPanel/accessPanelLeafRetractsIntoTheTopEdge",
		isolated<accessPanelLeafRetractsIntoTheTopEdge> });
	checks.push_back({ "bulkheadAndPanel/forceBridgeStillRendersAsAPrimitiveLine",
		isolated<forceBridgeStillRendersAsAPrimitiveLine> });
	checks.push_back({ "bulkheadAndPanel/loaderRefusesAForceBridgeTile",
		isolated<loaderRefusesAForceBridgeTile> });
}
