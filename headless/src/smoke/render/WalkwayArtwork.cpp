// A Walkway's artwork hangs below the floor line of the cell it is authored
// in, while the Walkway's physical extent stays exactly what it is: zero
// height, never an obstruction to routing, collision or support (#1014).
//
// The artwork is installed the way the render stack installs it - pixel
// regions from the `ImageSet` the application manifest declares, render
// semantics from the authored tileset the atlas-set registry names for it -
// so these checks exercise the same pairing the application does. Everything is asserted on the recorded WorldDrawList
// command vector, the stable CPU seam ADR 0010 names.
//
// Screen space is top-left origin and the World is drawn with y growing up,
// so a World point (x, y) lands at screen (x * cell width, canvas height -
// y * level height) at zoom 1 with the viewport at the origin.

#include "Checks.h"

#include "ObjectTileset.h"
#include "Render.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Graph.h"
#include "core/Walkway.h"
#include "core/WalkwaySectorObject.h"
#include "core/World.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
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

	// The fixture: a 8-cell, 3-Level Room on Layer 0, with a Walkway at
	// Level 1 two cells in, and a second Walkway beside it so the pair has
	// somewhere to route to.
	uint32_t const kRoomCellsWide = 8;
	uint32_t const kRoomLevelsHigh = 3;
	uint32_t const kWalkwayLevel = 1;
	uint32_t const kWalkwayOffset = 2;
	float const kCanvasWidth = float(kRoomCellsWide) * kCellWidth;
	float const kCanvasHeight = float(kRoomLevelsHigh) * kLevelHeight;

	// The authored slab: 24px of a 320px Level, hung below the floor line.
	float const kSlabWorldHeight = 0.075f;
	float const kSlabPixels = kSlabWorldHeight * kLevelHeight;

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

	// The World-space cell the fixture's Walkway is authored in.
	float walkwayCellX() { return float(kWalkwayOffset); }
	float walkwayFloorY() { return float(kWalkwayLevel); }

	// The artwork's destination: the authored offset from the cell's
	// bottom-left anchor, drawn at the authored size.
	ScreenRect walkwayArtworkRect()
	{
		return worldToScreen(walkwayCellX() + 0.0f, walkwayFloorY() - kSlabWorldHeight,
			1.0f, kSlabWorldHeight);
	}

	// The floor line of the cell the Walkway is authored in, in screen y.
	float walkwayFloorLineY()
	{
		return kCanvasHeight - walkwayFloorY() * kLevelHeight;
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

	// The authored atlas-set registry beside the manifest: which tileset is
	// asked about for which atlas. The manifest cannot name it itself - the
	// vendored Willpower `ImageSet` schema fixes the Options an `ImageSet`
	// may carry - so the binding is authored here instead.
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

	// The authored render semantics behind one atlas, read with the loader
	// its declared format calls for.
	TileArtworkMap authoredTilesFor(smoke::Context const& context,
		std::string const& atlas, std::string const& tileset)
	{
		auto const path = context.fixture("resources/" + tileset);
		if (atlasSetRegistry(context).at(atlas).kind == "sector")
			return SectorTileset::load(path).tiles;
		return ObjectTileset::load(path).sprites;
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
		require(image.options.count("filtering") && image.options.at("filtering") == "none"
			&& image.options.count("uv-style")
			&& image.options.at("uv-style") == "atlas",
			"ObjectAtlas's paired Image must be an unfiltered atlas image");

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

	// Installs the new Objects atlas through the one install path the render
	// stack uses: pixel regions from the ImageSet, render semantics from
	// the authored tileset.
	void installNewObjectsAtlas(smoke::Context const& context)
	{
		auto const atlas = loadNewObjectsAtlas(context);
		auto const tiles = installAtlasTiles(atlas.authored.sprites, atlas.regions,
			atlas.width, atlas.height, "ObjectAtlas");
		ObjectTileset installed;
		installed.width = atlas.width;
		installed.height = atlas.height;
		installed.sprites = tiles;
		setObjectTileset(std::move(installed), static_cast<TextureId>(1));
	}

	struct WalkwayFixture
	{
		std::shared_ptr<core::World> world;
		uint32_t room{};
		std::shared_ptr<const core::SectorObject> object;
		std::shared_ptr<const core::Walkway> walkway;
		std::shared_ptr<const core::SectorObject> neighbour;
	};

	// Two Walkways side by side at Level 1: one is the subject of every
	// assertion, the other gives it somewhere to route to.
	WalkwayFixture buildWalkwayFixture()
	{
		WalkwayFixture fixture;
		fixture.world = std::make_shared<core::World>("Walkway artwork",
			kRoomCellsWide, kRoomLevelsHigh);
		fixture.room = fixture.world->addRoom("Room", 0, 0, 0, kRoomCellsWide,
			kRoomLevelsHigh);
		auto const created = fixture.world->addSectorWalkway(fixture.room,
			kWalkwayLevel, kWalkwayOffset);
		require(created.type == core::SectorObjectType::Walkway,
			"The fixture placement did not create a Walkway");
		require(fixture.world->addSectorWalkway(fixture.room, kWalkwayLevel,
			kWalkwayOffset + 1).type == core::SectorObjectType::Walkway,
			"The fixture's second Walkway was not created");
		fixture.world->finishBuild();
		// Read the objects back out of the built World, so every pointer here
		// is the one the routing graph holds.
		auto const sector = fixture.world->getSector(fixture.room);
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto const candidate = sector->getObject(index);
			auto const walkwayObject =
				std::dynamic_pointer_cast<const core::WalkwaySectorObject>(candidate);
			if (!walkwayObject || walkwayObject->getCellY()
				!= sector->getCellY() + kWalkwayLevel)
				continue;
			if (walkwayObject->getCellX() == sector->getCellX() + kWalkwayOffset)
			{
				fixture.object = candidate;
				fixture.walkway = walkwayObject->getWalkway();
			}
			else if (walkwayObject->getCellX() == sector->getCellX() + kWalkwayOffset + 1)
				fixture.neighbour = candidate;
		}
		require(bool(fixture.walkway), "The fixture SectorObject carries no Walkway");
		require(bool(fixture.neighbour), "The fixture's second Walkway is missing");
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

	struct AtlasQuad
	{
		unsigned count{};
		float left{}, top{}, right{}, bottom{};
	};

	// The object-atlas triangles drawn over one World column, as one
	// bounding box. The fixture has two Walkways, so the stream holds two
	// quads and each check asks for the one it is looking at.
	AtlasQuad objectAtlasQuad(WorldDrawList const& drawing, float cellX)
	{
		AtlasQuad quad;
		quad.left = quad.top = 1e9f;
		quad.right = quad.bottom = -1e9f;
		float const columnLeft = cellX * kCellWidth;
		float const columnRight = columnLeft + kCellWidth;
		for (auto const& command : drawing.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::ObjectAtlas)
				continue;
			float const minX = std::min({ triangle->positions[0].x,
				triangle->positions[1].x, triangle->positions[2].x });
			float const maxX = std::max({ triangle->positions[0].x,
				triangle->positions[1].x, triangle->positions[2].x });
			if (maxX < columnLeft + 0.01f || minX > columnRight - 0.01f) continue;
			++quad.count;
			for (auto const& point : triangle->positions)
			{
				quad.left = std::min(quad.left, point.x);
				quad.top = std::min(quad.top, point.y);
				quad.right = std::max(quad.right, point.x);
				quad.bottom = std::max(quad.bottom, point.y);
			}
		}
		if (quad.count)
		{
			require(quad.count == 2,
				"One sprite draws one two-triangle quad, saw " + std::to_string(quad.count));
			return quad;
		}
		quad.left = quad.top = quad.right = quad.bottom = 0.0f;
		return quad;
	}

	// A line lying on the Walkway's floor line across the cell it is
	// authored in: what the renderer drew before the tile existed.
	bool floorLineAtWalkway(WorldDrawList const& drawing)
	{
		float const floor = walkwayFloorLineY();
		float const left = walkwayCellX() * kCellWidth;
		float const right = left + kCellWidth;
		for (auto const& command : drawing.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (!line) continue;
			if (near(line->from.y, floor) && near(line->to.y, floor)
				&& near(line->from.x, left) && near(line->to.x, right))
				return true;
		}
		return false;
	}

	// ------------------------------------------------------------------
	// The manifest pair, and the generated ImageSet entries.
	// ------------------------------------------------------------------
	void newObjectsAtlasIsRegisteredAndGeneratedFromItsTileset(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const manifest = bundledManifest(context);
		auto const set = manifestResource(manifest, "ObjectAtlas");
		require(set.found && set.type == "ImageSet",
			"ObjectAtlas is not registered as an ImageSet in the application manifest");
		require(set.imageReference == "ObjectAtlasImage",
			"ObjectAtlas's paired Image is " + set.imageReference);
		auto const image = manifestResource(manifest, "ObjectAtlasImage");
		require(image.found && image.type == "Image",
			"ObjectAtlasImage is not registered as an Image in the application manifest");
		require(image.location == "textures/objects-new.png",
			"ObjectAtlasImage points at " + image.location);

		auto const atlas = loadNewObjectsAtlas(context);
		// The ImageSet entries are generated from the authored tileset: the
		// region each entry declares is the tileset's cell plus its content
		// offset, and the tileset has an entry for every region and no others.
		require(atlas.regions.size() == atlas.authored.sprites.size(),
			"The manifest's ObjectAtlas entries and the authored tileset differ in count");
		for (auto const& [name, region] : atlas.regions)
		{
			auto const authored = atlas.authored.sprites.find(name);
			require(authored != atlas.authored.sprites.end(),
				"ObjectAtlas declares tile '" + name
				+ "' that its authored tileset never defines");
		}
		for (auto const& [name, sprite] : atlas.authored.sprites)
		{
			auto const region = atlas.regions.find(name);
			require(region != atlas.regions.end(),
				"Authored tile '" + name + "' has no generated ImageSet entry");
			require(region->second.x == sprite.region.x
				&& region->second.y == sprite.region.y
				&& region->second.width == sprite.region.width
				&& region->second.height == sprite.region.height,
				"Generated ImageSet entry for '" + name
				+ "' drifted from the authored tileset");
		}
		// The Walkway tile is the one live entry, and it carries the two
		// render semantics a version 1 tileset left the renderer to guess.
		auto const walkway = atlas.authored.sprites.find("walkway");
		require(walkway != atlas.authored.sprites.end(),
			"The authored ObjectAtlas tileset has no walkway tile");
		require(near(walkway->second.offset.y, -kSlabWorldHeight)
			&& near(walkway->second.offset.x, 0.0f),
			"The Walkway tile's authored offset must hang below the floor line");
		require(near(walkway->second.drawnSize.x, 1.0f)
			&& near(walkway->second.drawnSize.y, kSlabWorldHeight),
			"The Walkway tile's authored drawn size must be the slab");
	}

	// Every atlas the manifest registers must be covered by the registry,
	// and every tileset the registry names must load and author every tile
	// the atlas declares. This is what stops the mapping rotting quietly.
	void everyAtlasIsCoveredByItsAuthoredTileset(smoke::Context const& context)
	{
		using smoke::require;
		auto const manifest = bundledManifest(context);
		auto const sets = atlasSetRegistry(context);
		unsigned atlases = 0;
		for (auto const& resource : manifest["Resources"]["Resource"])
		{
			if (!resource.IsMap() || resource["type"].as<std::string>() != "ImageSet")
				continue;
			auto const name = resource["name"].as<std::string>();
			++atlases;
			auto const found = sets.find(name);
			require(found != sets.end(),
				name + " is registered as an ImageSet but the atlas-set registry "
				"names no tileset for it");
			auto const authored = authoredTilesFor(context, name, found->second.tileset);
			for (auto const& [tile, region] : manifestResource(manifest, name).regions)
				require(authored.count(tile),
					name + " declares tile '" + tile + "' that its authored tileset "
					+ found->second.tileset + " never defines");
		}
		require(atlases >= 7, "The bundled manifest should register every atlas "
			"pair - the four viewport atlases and the three themed sheets; saw "
			+ std::to_string(atlases) + " ImageSets");
	}

	// ------------------------------------------------------------------
	// The install seam: ImageSet pixels, authored semantics, no drift.
	// ------------------------------------------------------------------
	void installTakesPixelsFromTheImageSetAndSemanticsFromTheTileset(
		smoke::Context const& context)
	{
		using smoke::require;
		auto const atlas = loadNewObjectsAtlas(context);
		auto const tiles = installAtlasTiles(atlas.authored.sprites, atlas.regions,
			atlas.width, atlas.height, "ObjectAtlas");
		require(tiles.size() == atlas.regions.size(),
			"The install dropped a declared tile");
		auto const& installed = tiles.at("walkway");
		// Pixels from the ImageSet, not from the tileset's own resolution.
		auto const& region = atlas.regions.at("walkway");
		require(installed.region.x == region.x && installed.region.y == region.y
			&& installed.region.width == region.width
			&& installed.region.height == region.height,
			"The installed region did not come from the ImageSet");
		// Semantics from the tileset, not derived from the name.
		require(near(installed.offset.x, atlas.authored.sprites.at("walkway").offset.x)
			&& near(installed.offset.y, atlas.authored.sprites.at("walkway").offset.y)
			&& near(installed.drawnSize.x, atlas.authored.sprites.at("walkway").drawnSize.x)
			&& near(installed.drawnSize.y, atlas.authored.sprites.at("walkway").drawnSize.y),
			"The installed render semantics did not come from the authored tileset");

		// A tile the ImageSet declares that the tileset never authored is
		// refused: the renderer would have no declared visual extent for it.
		auto drifted = atlas.regions;
		drifted.emplace("not-authored", AtlasPixelRegion{ 0, 0, 16, 16 });
		bool refused = false;
		try
		{
			installAtlasTiles(atlas.authored.sprites, drifted, atlas.width,
				atlas.height, "ObjectAtlas");
		}
		catch (std::exception const& error)
		{
			refused = true;
			require(std::string(error.what()).find("not-authored") != std::string::npos,
				std::string("The refusal must name the drifted tile: ") + error.what());
		}
		require(refused, "An ImageSet tile with no authored definition was accepted");

		// A region that escapes the atlas image is refused too.
		auto escaped = atlas.regions;
		escaped["walkway"] = AtlasPixelRegion{ 0, atlas.height - 4, 128, 24 };
		bool escapedRefused = false;
		try
		{
			installAtlasTiles(atlas.authored.sprites, escaped, atlas.width,
				atlas.height, "ObjectAtlas");
		}
		catch (std::exception const& error)
		{
			escapedRefused = true;
			require(std::string(error.what()).find("outside the atlas image")
				!= std::string::npos,
				std::string("The refusal must state the image bound: ") + error.what());
		}
		require(escapedRefused, "A pixel region escaping the atlas image was accepted");
	}

	// ------------------------------------------------------------------
	// The Walkway draws its artwork at the offset destination.
	// ------------------------------------------------------------------
	void walkwayDrawsItsArtworkBelowTheFloorLine(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto fixture = buildWalkwayFixture();
		prepareCanvas();
		auto const drawing = drawWorld(fixture.world);

		auto const quad = objectAtlasQuad(drawing, walkwayCellX());
		require(quad.count == 2,
			"The Walkway tile drew no object-atlas quad into the World");
		auto const expected = walkwayArtworkRect();
		require(near(quad.left, expected.left) && near(quad.top, expected.top)
			&& near(quad.right, expected.right) && near(quad.bottom, expected.bottom),
			"Walkway quad was " + std::to_string(quad.left) + "," + std::to_string(quad.top)
			+ " -> " + std::to_string(quad.right) + "," + std::to_string(quad.bottom)
			+ " expected " + std::to_string(expected.left) + "," + std::to_string(expected.top)
			+ " -> " + std::to_string(expected.right) + "," + std::to_string(expected.bottom));

		// The slab hangs below the floor line: its top edge is the floor
		// line and it extends downward from there, into the space below the
		// cell it is authored in.
		float const floor = walkwayFloorLineY();
		require(near(quad.top, floor),
			"The Walkway slab must start at its cell's floor line");
		require(quad.bottom > quad.top,
			"The Walkway slab must hang below the floor line");
		require(near(quad.bottom - quad.top, kSlabPixels),
			"The Walkway slab must be as thick as it is authored");

		// Not a line at the floor.
		require(!floorLineAtWalkway(drawing),
			"The Walkway still drew the primitive line at the floor");
	}

	// ------------------------------------------------------------------
	// The physical extent is untouched: zero height, routing unaffected.
	// ------------------------------------------------------------------
	void walkwayPhysicalExtentStaysZeroHeightAndRoutes(smoke::Context const& context)
	{
		using smoke::require;
		installNewObjectsAtlas(context);
		auto fixture = buildWalkwayFixture();

		// Zero physical height, exactly as it was authored.
		auto const& size = fixture.walkway->getSize();
		require(near(size.y, 0.0f),
			"A Walkway's physical height must stay zero, saw " + std::to_string(size.y));
		core::Vector2 minimum, maximum;
		fixture.walkway->getFullShape(minimum, maximum);
		require(near(minimum.y, maximum.y),
			"A Walkway's physical shape must stay flat");
		require(near(maximum.x - minimum.x, 1.0f),
			"A Walkway still spans one cell horizontally");
		require(near(minimum.x, walkwayCellX()) && near(minimum.y, walkwayFloorY()),
			"The artwork offset must not move the Walkway's own anchor");

		// The cell still reports the Walkway as its floor: asking to place a
		// second one there is refused as already occupied, which is what a
		// Walkway-owned floor cell reads as.
		std::string diagnostic;
		require(!fixture.world->canAddSectorWalkway(fixture.room, kWalkwayLevel,
				kWalkwayOffset, &diagnostic)
			&& diagnostic.find("already occupies") != std::string::npos,
			"The Walkway no longer owns its cell's floor: " + diagnostic);

		// Routing still sees the Walkway. The run produces its own vertices
		// at the Level it was authored on, and a path exists across them:
		// the artwork hanging below the floor line changed none of that.
		auto const graph = fixture.world->getGraph();
		std::vector<std::shared_ptr<const core::Vertex>> atLevel;
		for (auto const& vertex : graph->getVertices())
			if (near(vertex->getPosition().y, float(kWalkwayLevel)))
				atLevel.push_back(vertex);
		require(atLevel.size() >= 2,
			"The Walkway run produced no routing vertices at its Level");
		auto const agentId = fixture.world->createAgent("Walker", fixture.room,
			kWalkwayLevel, float(kWalkwayOffset) + 0.5f);
		auto const agent = fixture.world->lookupAgent(agentId);
		require(bool(agent), "Could not create the routing fixture Agent");
		auto const path = graph->calculatePath(
			dynamic_cast<core::Agent*>(agent.entity), atLevel.front(), atLevel.back());
		require(bool(path), "A route across the Walkway run was refused");

		// And the artwork cannot be read as a physical extent: it lies
		// entirely below the floor line of the cell it is authored in, so it
		// never covers the space an Agent moves through.
		prepareCanvas();
		auto const drawing = drawWorld(fixture.world);
		auto const quad = objectAtlasQuad(drawing, walkwayCellX());
		require(quad.count == 2 && quad.top >= walkwayFloorLineY() - 0.01f,
			"The Walkway artwork reaches up into the cell it must not occupy");
	}

}

void render_smoke::registerWalkwayArtwork(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "walkwayArtwork/manifestPairIsGeneratedFromTheTileset",
		isolated<newObjectsAtlasIsRegisteredAndGeneratedFromItsTileset> });
	checks.push_back({ "walkwayArtwork/everyAtlasIsCoveredByItsAuthoredTileset",
		isolated<everyAtlasIsCoveredByItsAuthoredTileset> });
	checks.push_back({ "walkwayArtwork/installSeparatesPixelsFromSemantics",
		isolated<installTakesPixelsFromTheImageSetAndSemanticsFromTheTileset> });
	checks.push_back({ "walkwayArtwork/walkwayDrawsBelowTheFloorLine",
		isolated<walkwayDrawsItsArtworkBelowTheFloorLine> });
	checks.push_back({ "walkwayArtwork/physicalExtentStaysZeroAndRoutes",
		isolated<walkwayPhysicalExtentStaysZeroHeightAndRoutes> });
}
