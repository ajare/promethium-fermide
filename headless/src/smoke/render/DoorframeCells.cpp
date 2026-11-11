// A cell a Door stands in wears its wall's doorframe tile (#1054).
//
// #1037 pinned the geometry of a Sector surface, #1039 the seam that asks
// which tile each of those cells wears, and #1040 what a recorded variant
// does at that seam. This is the next thing to answer through the same
// seam, and the first whose answer comes from a live object rather than
// from what an author painted: a Door set into a Room or a Corridor puts
// that wall's doorframe tile on the cells it covers, and leaves nothing
// behind when it goes away.
//
//   * a 1-wide Door in a Room draws `doorframe-room-1r-L0` across its
//     cell, and a 2-wide Door draws `doorframe-room-2r-L0` and
//     `doorframe-room-2r-R0` across its two;
//   * a tall Door draws the tall frame for its width, and the same three
//     cases draw the Corridor's frames in a Corridor;
//   * the frame arrives through the per-cell resolver, so it is drawn
//     under the same floor-anchored crop and the same culling the base tile
//     is - only the picture changes, never the geometry;
//   * a variant painted on a door cell is outranked while the Door stands
//     there and is still on the Sector while that happens, which is why
//     taking the Door out puts the painting back;
//   * two doors side by side each frame their own cells and neither
//     reaches into the other;
//   * the frame is keyed by the wall the Door is set into, and by its
//     front side only: the Sector behind the same threshold draws its own
//     base, because it is seen through the opening rather than framing it;
//   * a wall with no frame authored - a Lift, or an atlas written before
//     doorframes existed - draws its base rather than the missing-art
//     colour, which is what makes the frame additive rather than a break.
//
// The atlas is a synthetic one with every tile in its own column, so a
// quad's tile is read back out of the UVs it sampled rather than taken on
// trust from a name the renderer kept to itself.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

extern UISettings gUISettings;

namespace
{
	using smoke::require;

	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// A tint no production path picks by default, so a cell that ignored
	// the caller's tint fails instead of looking right.
	pf::render::PackedColourValue const kTint = packColour(214, 96, 32, 255);
	pf::render::PackedColourValue const kMagenta = packColour(255, 0, 255, 255);

	WorldDrawList::ClipRectangle const kNoClip{ { -100000.0f, -100000.0f },
		{ 100000.0f, 100000.0f } };

	// The nine frames the shipped Room and Corridor set authors. A
	// Corridor gets no tall frame because a tall Door is refused in one.
	std::vector<std::string> const kFrames{
		"doorframe-room-1r-L0", "doorframe-room-2r-L0", "doorframe-room-2r-R0",
		"doorframe-room-1t-L0", "doorframe-room-2t-L0", "doorframe-room-2t-R0",
		"doorframe-corridor-1r-L0", "doorframe-corridor-2r-L0",
		"doorframe-corridor-2r-R0" };

	// ------------------------------------------------------------------
	// The atlas.
	// ------------------------------------------------------------------

	// One full-cell tile per name, each in its own column of a one-row
	// sheet, so no two tiles share a texel column and a quad's tile is
	// readable back out of the UVs it sampled.
	//
	// `withFrames` off is the atlas a World drawn before doorframes existed
	// would have: the same surfaces, no frames anywhere.
	SectorTileset installAtlas(bool withFrames)
	{
		SectorTileset atlas;
		atlas.family = SectorTilesetFamily::New;
		int column = 0;
		for (auto const* name : { "room", "room-V1", "corridor" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		if (withFrames)
			for (auto const& name : kFrames)
				atlas.overlays.emplace(name, SectorTileRegion{
					column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		atlas.width = column * kCellWidth;
		atlas.height = kCellHeight;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The synthetic doorframe atlas did not come up as the New family");
		return atlas;
	}

	// The tile one UV falls on, by the centre of the sampled range, across
	// both maps. A frame and a surface are disjoint sets of names, so the
	// centre lands on exactly one of them either way.
	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		auto const inMap = [&](std::map<std::string, SectorTileRegion> const& map)
		{
			for (auto const& [name, region] : map)
			{
				auto const left = float(region.x) / float(atlas.width);
				auto const right = float(region.x + region.width) / float(atlas.width);
				auto const top = float(region.y) / float(atlas.height);
				auto const bottom = float(region.y + region.height) / float(atlas.height);
				if (centreU >= left && centreU < right && centreV >= top
						&& centreV < bottom)
					return name;
			}
			return std::string{};
		};
		auto const surface = inMap(atlas.surfaces);
		return surface.empty() ? inMap(atlas.overlays) : surface;
	}

	// ------------------------------------------------------------------
	// The viewport.
	// ------------------------------------------------------------------

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

	// ------------------------------------------------------------------
	// What came out of the draw.
	// ------------------------------------------------------------------

	struct RecordedCell
	{
		std::string tile;   // empty for a cell filled with no texture at all
		float left{}, top{}, right{}, bottom{};
		// The sampled fraction of the tile the cell drew, so a crop shows
		// as a shortened v range rather than as a resized rectangle.
		float u0{}, u1{}, v0{}, v1{};
		bool textured{ true };
	};

	std::string describe(RecordedCell const& cell)
	{
		return (cell.textured ? cell.tile : std::string("<flat>"))
			+ " at (" + std::to_string(cell.left) + "," + std::to_string(cell.top)
			+ ")-(" + std::to_string(cell.right) + "," + std::to_string(cell.bottom)
			+ ") sampling v " + std::to_string(cell.v0) + ".."
			+ std::to_string(cell.v1);
	}

	// Records the front Sector's own surface pass.
	//
	// A Door draws the Sector behind through its aperture after the front
	// surface is down, under a clip pushed to the doorway. Those cells are
	// the back Sector's surface, not this one's, and they are told apart by
	// the clip they were emitted under: the front surface pass runs under
	// the clip this recorder opened and nothing else does.
	std::vector<RecordedCell> recordFrontSurface(
		std::shared_ptr<const core::Sector> sector, uint32_t const layer,
		SectorTileset const& atlas,
		WorldDrawList::ClipRectangle const& clip)
	{
		WorldDrawList commands(clip);
		renderSector(sector, layer, LayerRenderStyle::Solid, false, kTint,
			&commands);

		auto const underOuterClip = [&](WorldDrawList::ClipRectangle const& c)
		{
			return near(c.minimum.x, clip.minimum.x)
				&& near(c.minimum.y, clip.minimum.y)
				&& near(c.maximum.x, clip.maximum.x)
				&& near(c.maximum.y, clip.maximum.y);
		};

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			// The surface pass is the only thing here that draws SectorAtlas
			// tiles or the missing-art fill for them. A Door leaf is an
			// ObjectAtlas sprite or a plain-colour fallback, and neither is
			// a Sector surface.
			if (triangle->texture != WorldDrawList::Texture::SectorAtlas
					&& triangle->colour != kMagenta)
				continue;
			if (!underOuterClip(triangle->clip)) continue;
			quads.push_back(triangle);
		}
		require(quads.size() % 2 == 0,
			"The Sector emitted an odd number of surface triangles, so they are "
			"not the pairs one quad makes");

		std::vector<RecordedCell> cells;
		for (size_t pair = 0; pair < quads.size(); pair += 2)
		{
			auto const& first = *quads[pair];
			auto const& second = *quads[pair + 1];
			require(first.colour == second.colour,
				"The two triangles of one surface quad were tinted differently");
			RecordedCell cell;
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
			if (first.texture == WorldDrawList::Texture::SectorAtlas)
			{
				require(first.colour == kTint,
					"A textured surface quad carried a colour other than the "
					"caller's tint");
				cell.textured = true;
				cell.tile = tileAt((uvLeft + uvRight) * 0.5f,
					(uvTop + uvBottom) * 0.5f, atlas);
				require(!cell.tile.empty(),
					"A textured surface quad sampled UVs that fall on no tile in "
					"the installed atlas");
				auto const region = cell.tile.empty()
					? SectorTileRegion{ 0, 0, 1, 1 }
					: (atlas.surfaces.count(cell.tile)
						? atlas.surfaces.at(cell.tile)
						: atlas.overlays.at(cell.tile));
				// Each axis undoes itself against the image extent it was
				// divided by, which is not the same number on a sheet that
				// is not square.
				auto const fraction = [&](float uv, int origin, int extent,
					int imageExtent)
				{
					return (uv * float(imageExtent) - (float(origin) + 0.5f))
						/ float(extent - 1);
				};
				cell.u0 = fraction(uvLeft, region.x, region.width, atlas.width);
				cell.u1 = fraction(uvRight, region.x, region.width, atlas.width);
				cell.v0 = fraction(uvTop, region.y, region.height, atlas.height);
				cell.v1 = fraction(uvBottom, region.y, region.height, atlas.height);
			}
			else
			{
				cell.textured = false;
				require(cell.left < cell.right && cell.top < cell.bottom,
					"An untextured surface cell covered no area");
				require(first.colour == kMagenta,
					"An untextured surface quad carried a colour other than the "
					"missing-art magenta");
			}
			cells.push_back(cell);
		}
		return cells;
	}

	// The recorded cells laid out in the Sector-local grid the resolver was
	// asked in: column from the Sector's left edge, level from its floor
	// line.
	std::map<std::pair<uint32_t, uint32_t>, RecordedCell> gridOf(
		std::vector<RecordedCell> const& cells, core::Sector const& sector)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const toScreen = [](core::Vector2 const& point)
		{
			return pf::render::Vec2{
				point.x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom
					+ gUISettings.worldViewportX + gUISettings.xOffset,
				gUISettings.worldViewportY + gUISettings.worldViewportHeight
					- point.y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom
					- gUISettings.yOffset };
		};
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		float const left = std::min(a.x, b.x);
		float const floorLine = std::max(a.y, b.y);
		float const cellWidth = CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		std::map<std::pair<uint32_t, uint32_t>, RecordedCell> grid;
		for (auto const& cell : cells)
		{
			auto const column = static_cast<uint32_t>(
				std::lround((cell.left - left) / cellWidth));
			auto const level = static_cast<uint32_t>(
				std::lround((floorLine - cell.bottom) / cellHeight));
			require(column < sector.getCellsWide() && level < sector.getLevelsHigh(),
				"A surface cell landed outside the Sector's own grid at column "
				+ std::to_string(column) + ", level " + std::to_string(level));
			require(grid.find({ column, level }) == grid.end(),
				"Two surface cells landed on column " + std::to_string(column)
				+ ", level " + std::to_string(level));
			grid.emplace(std::make_pair(column, level), cell);
		}
		return grid;
	}

	std::string tileAt(std::map<std::pair<uint32_t, uint32_t>, RecordedCell> const& grid,
		uint32_t column, uint32_t level)
	{
		auto const found = grid.find(std::make_pair(column, level));
		if (found == grid.end()) return std::string("<nothing>");
		return found->second.textured ? found->second.tile
			: std::string("<flat>");
	}

	// ------------------------------------------------------------------
	// The fixture Worlds.
	// ------------------------------------------------------------------

	// One fixture World and its front Location.
	//
	// `refresh` exists because a Door removal rebuilds the World from its
	// construction records, which replaces every Sector object. A held
	// pointer through that edit keeps drawing the World that used to be,
	// and a check that "the Door is gone" against it would pass while the
	// live World still had one.
	struct Scene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> front;
		std::string frontName;
		uint32_t frontLayer{};
		uint32_t frontIndex{};

		// Found by name and Layer rather than by index, because a rebuild
		// re-derives the Sector list from the construction records and an
		// index remembered across one can point anywhere. Name alone is not
		// enough either: a Corridor and the Corridor behind it share the
		// name their builder gave them, and the pair is exactly what this
		// fixture has.
		void refresh()
		{
			front.reset();
			frontIndex = ~0u;
			for (uint32_t index = 0; index < world->getNumSectors(); ++index)
			{
				auto const candidate = world->getSector(index);
				if (candidate && candidate->getName() == frontName
						&& candidate->getLayerIndex() == frontLayer)
				{
					front = candidate;
					frontIndex = index;
				}
			}
			require(front != nullptr, "The front Location '" + frontName
				+ "' on Layer " + std::to_string(frontLayer)
				+ " vanished from the World");
		}
	};

	// The front wall of the one Door in a fixture. Asked of the World
	// rather than guessed from the fixture's shape, because "which of these
	// two Locations is the front" is the Door's fact to state and the check
	// that guesses it is the check that can be wrong.
	std::shared_ptr<const core::Sector> doorFrontSector(core::World const& world)
	{
		std::shared_ptr<const core::Sector> found;
		int count{};
		for (uint32_t index = 0; index < world.getNumSectors(); ++index)
		{
			auto const sector = world.getSector(index);
			if (!sector) continue;
			for (uint32_t object = 0; object < sector->getNumObjects(); ++object)
			{
				auto const candidate = sector->getObject(object);
				if (!candidate
						|| candidate->getObjectType() != core::SectorObjectType::Door)
					continue;
				auto const door = std::static_pointer_cast<
					const core::DoorSectorObject>(candidate)->getDoor();
				// A Door is registered on both Sectors of its pair; only
				// the front one counts here.
				if (door && door->getFrontSector() == sector)
				{
					found = sector;
					++count;
				}
			}
		}
		require(count == 1, "The fixture carries " + std::to_string(count)
			+ " front Door walls; the check needs exactly one");
		return found;
	}

	uint32_t doorObjectIndex(core::Sector const& sector)
	{
		for (uint32_t index = 0; index < sector.getNumObjects(); ++index)
			if (sector.getObject(index)
					&& sector.getObject(index)->getObjectType()
						== core::SectorObjectType::Door)
				return index;
		return ~0u;
	}

	// One Location on the front Layer with the same Location behind it, and
	// one ordinary Door set into the front wall.
	//
	// The back Location is not decoration: a Door joins two Layers, and the
	// space behind is what the frame's hole shows. It is also the case the
	// frame rule has to refuse - the back wall is seen through the opening
	// rather than framing it - so the fixture has to carry one to prove it
	// stays unfurnished.
	Scene buildScene(std::string const& kind, uint32_t cellsWide,
		uint32_t levelsHigh, uint32_t doorWidth, core::Door::Height height,
		uint32_t doorX, uint32_t doorLevel, float frontTopLevelHeight)
	{
		auto world = std::make_shared<core::World>("Doorframe world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		if (kind == "corridor")
		{
			world->addCorridor(0, 0, 0, cellsWide, levelsHigh);
			world->addCorridor(1, 0, 0, cellsWide, levelsHigh);
		}
		else
		{
			world->addRoom("Front", 0, 0, 0, cellsWide, levelsHigh,
				frontTopLevelHeight);
			world->addRoom("Back", 1, 0, 0, cellsWide, levelsHigh,
				frontTopLevelHeight);
		}
		core::World::CreateDoorOptions options;
		options.width = doorWidth;
		options.height = height;
		world->addSectorDoor(0, doorLevel, doorX, options);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		// The Door knows which wall it was authored into; ask it rather than
		// guessing from the fixture's shape.
		auto const doorFront = doorFrontSector(*world);
		scene.frontName = doorFront->getName();
		scene.frontLayer = doorFront->getLayerIndex();
		scene.refresh();
		require(scene.front == doorFront,
			"The front Location resolved twice to different Sectors");
		require(scene.front->getCellsWide() == cellsWide,
			"The front Location is not the width it was authored as");
		require(scene.front->getLevelsHigh() == levelsHigh,
			"The front Location is not as many levels high as it was authored");
		require(doorObjectIndex(*scene.front) != ~0u,
			"The front Sector carries no Door object");
		return scene;
	}

	Scene roomScene(uint32_t cellsWide, uint32_t doorWidth,
		core::Door::Height height, uint32_t doorX, uint32_t doorLevel = 0,
		float topLevelHeight = CORE_ROOM_MAX_HEIGHT,
		uint32_t levelsHigh = 1)
	{
		return buildScene("room", cellsWide, levelsHigh, doorWidth, height,
			doorX, doorLevel, topLevelHeight);
	}

	Scene corridorScene(uint32_t cellsWide, uint32_t doorWidth,
		core::Door::Height height, uint32_t doorX, uint32_t doorLevel = 0)
	{
		// A Corridor is one level high and takes its own height, not the
		// Room's: the top-level height a Room author sets means nothing to
		// it, and the loader would say so.
		return buildScene("corridor", cellsWide, 1, doorWidth, height, doorX,
			doorLevel, CORE_CORRIDOR_HEIGHT);
	}

	std::map<std::pair<uint32_t, uint32_t>, RecordedCell> drawGrid(Scene& scene,
		SectorTileset& atlas,
		WorldDrawList::ClipRectangle const& clip = kNoClip)
	{
		return gridOf(recordFrontSurface(scene.front,
			scene.front->getLayerIndex(), atlas, clip), *scene.front);
	}

	// ------------------------------------------------------------------
	// The cases.
	// ------------------------------------------------------------------

	// A 1-wide Door in a Room draws `doorframe-room-1r-L0` across its
	// cell, and nothing else about the Room changes.
	void oneWideRoomDoorWearsItsFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const grid = drawGrid(scene, atlas);
		require(grid.size() == 4, "A four-cell Room drew "
			+ std::to_string(grid.size()) + " surface cells");
		require(tileAt(grid, 0, 0) == "room", "Column 0 drew "
			+ tileAt(grid, 0, 0) + "; a cell with no Door wears the Room tile");
		require(tileAt(grid, 1, 0) == "room", "Column 1 drew "
			+ tileAt(grid, 1, 0) + "; a cell with no Door wears the Room tile");
		require(tileAt(grid, 2, 0) == "doorframe-room-1r-L0", "The door cell "
			"drew " + tileAt(grid, 2, 0) + "; a 1-wide regular Door in a Room "
			"wears doorframe-room-1r-L0");
		require(tileAt(grid, 3, 0) == "room", "Column 3 drew "
			+ tileAt(grid, 3, 0) + "; a cell with no Door wears the Room tile");
	}

	// A 2-wide Door draws the pair, left cell `L0` and right cell `R0`,
	// one cell each and in that order.
	void twoWideRoomDoorWearsItsFramePair(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(5, 2, core::Door::Height::Regular, 2);
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 1, 0) == "room", "The cell beside the opening drew "
			+ tileAt(grid, 1, 0));
		require(tileAt(grid, 2, 0) == "doorframe-room-2r-L0", "The opening's "
			"left cell drew " + tileAt(grid, 2, 0) + "; a 2-wide regular Door "
			"wears doorframe-room-2r-L0 on its left cell");
		require(tileAt(grid, 3, 0) == "doorframe-room-2r-R0", "The opening's "
			"right cell drew " + tileAt(grid, 3, 0) + "; a 2-wide regular Door "
			"wears doorframe-room-2r-R0 on its right cell");
		require(tileAt(grid, 4, 0) == "room", "The cell past the opening drew "
			+ tileAt(grid, 4, 0));
	}

	// A tall Door draws the tall frame for its width - the tall pair for a
	// 2-wide opening, the single tall tile for a 1-wide one.
	void tallRoomDoorWearsItsTallFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = roomScene(4, 1, core::Door::Height::Tall, 1);
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 1, 0) == "doorframe-room-1t-L0", "A 1-wide "
				"tall Door drew " + tileAt(grid, 1, 0) + "; a tall opening "
				"wears the tall frame, not the regular one");
		}
		{
			auto scene = roomScene(5, 2, core::Door::Height::Tall, 2);
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 2, 0) == "doorframe-room-2t-L0", "A 2-wide "
				"tall Door's left cell drew " + tileAt(grid, 2, 0));
			require(tileAt(grid, 3, 0) == "doorframe-room-2t-R0", "A 2-wide "
				"tall Door's right cell drew " + tileAt(grid, 3, 0));
		}
	}

	// The same three cases in a Corridor draw the Corridor's frames.
	void corridorDoorsWearCorridorFrames(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = corridorScene(4, 1, core::Door::Height::Regular, 2);
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 2, 0) == "doorframe-corridor-1r-L0", "A 1-wide "
				"Door in a Corridor drew " + tileAt(grid, 2, 0) + "; the frame "
				"is keyed by the wall, so a Corridor doorway wears the "
				"Corridor frame");
		}
		{
			auto scene = corridorScene(5, 2, core::Door::Height::Regular, 1);
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 1, 0) == "doorframe-corridor-2r-L0", "A 2-wide "
				"Corridor opening's left cell drew " + tileAt(grid, 1, 0));
			require(tileAt(grid, 2, 0) == "doorframe-corridor-2r-R0", "A 2-wide "
				"Corridor opening's right cell drew " + tileAt(grid, 2, 0));
		}
	}

	// The frame is resolved through the per-cell surface resolver, so the
	// rules #1037 pins - the floor-anchored crop and the culling - are the
	// rules the frame draws under as well.
	//
	// A short Room crops the top of its top row. Put the Door in that row
	// and the frame cell keeps the same rectangle and the same sampled band
	// as the plain cell beside it, one cell over: the crop is the loop's,
	// and the loop did not change.
	void theFrameKeepsTheFloorAnchoredCrop(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		// A one-level Room whose ceiling is 0.7 of a cell: its only row is
		// the cropped one, and the Door stands in it.
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 1,
			/*doorLevel=*/ 0, /*topLevelHeight=*/ 0.7f);
		require(scene.front->getLevelsHigh() == 1,
			"The fixture Room is not one level high, so it crops nothing");
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 1, 0) == "doorframe-room-1r-L0", "The cropped row's "
			"door cell drew " + tileAt(grid, 1, 0));
		require(tileAt(grid, 3, 0) == "room", "The cropped row's plain cell "
			"drew " + tileAt(grid, 3, 0));
		auto const& frame = grid.at({ 1, 0 });
		auto const& plain = grid.at({ 3, 0 });
		// Same row, so the same vertical geometry: same top, same bottom,
		// same sampled band of the tile.
		require(near(frame.top, plain.top) && near(frame.bottom, plain.bottom),
			"The frame cell spans (" + std::to_string(frame.top) + ","
			+ std::to_string(frame.bottom) + ") on a row whose plain cell spans ("
			+ std::to_string(plain.top) + "," + std::to_string(plain.bottom)
			+ "); a frame must take the row's crop, not its own");
		require(near(frame.v0, plain.v0) && near(frame.v1, plain.v1),
			"The frame cell samples v " + std::to_string(frame.v0) + ".."
			+ std::to_string(frame.v1) + " where the plain cell samples "
			+ std::to_string(plain.v0) + ".." + std::to_string(plain.v1)
			+ "; the crop lands on the row, not on the tile it resolved to");
		// And one World cell apart, horizontally, as every column is.
		require(near(frame.left + 2.0f * CORE_CELL_WIDTH_PIXELS, plain.left)
				&& near(frame.right + 2.0f * CORE_CELL_WIDTH_PIXELS, plain.right),
			"The frame cell is not two World cells from the plain cell it was "
			"compared with: " + describe(frame) + " against " + describe(plain));
		// The row is genuinely cropped: neither cell samples the whole tile.
		require(!near(frame.v0, 0.0f), "The cropped row sampled from the top of "
			"its tile, so this fixture cropped nothing");
	}

	// A cell the clip hides entirely is never drawn, and so is never
	// resolved. The frame cannot make the loop reach for a cell the culling
	// rule already excluded.
	void aClippedDoorCellIsNeverDrawn(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 3);
		// A clip that stops one pixel inside the door cell's left edge: the
		// door cell lies wholly outside it, and the two cells to its left do
		// not.
		float const doorLeft = 3.0f * CORE_CELL_WIDTH_PIXELS;
		WorldDrawList::ClipRectangle const clip{ { 0.0f, -100000.0f },
			{ doorLeft - 1.0f, 100000.0f } };
		auto const grid = drawGrid(scene, atlas, clip);
		require(tileAt(grid, 3, 0) == "<nothing>", "The clipped door cell drew "
			+ tileAt(grid, 3, 0) + "; a cell wholly outside the clip is never "
			"resolved and never drawn");
		require(tileAt(grid, 0, 0) == "room" && tileAt(grid, 1, 0) == "room"
				&& tileAt(grid, 2, 0) == "room",
			"The cells inside the clip did not all draw");
	}

	// A variant painted on a door cell is outranked by the frame while the
	// Door stands there, is still on the Sector while that happens, and
	// draws again when the Door is taken out.
	void aVariantUnderADoorIsHiddenAndComesBack(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		std::string diagnostic;
		require(scene.world->setSectorSurfaceVariant(scene.frontIndex, 2, 0, 1u,
				&diagnostic).changed,
			"The fixture variant could not be painted on the door cell: "
			+ diagnostic);
		require(scene.front->surfaceVariantAt(2, 0) == 1u,
			"The painted entry did not reach the live Sector");

		auto const withDoor = drawGrid(scene, atlas);
		require(tileAt(withDoor, 2, 0) == "doorframe-room-1r-L0", "A door cell "
			"that wears a variant drew " + tileAt(withDoor, 2, 0) + "; the frame "
			"outranks the painting while the Door is there");
		require(scene.front->surfaceVariantAt(2, 0) == 1u,
			"The frame consumed the painted entry; removing the Door would have "
			"nothing left to restore");

		require(scene.world->removeSectorDoor(scene.frontIndex,
				doorObjectIndex(*scene.front)),
			"The fixture Door could not be removed");
		scene.refresh();
		require(doorObjectIndex(*scene.front) == ~0u,
			"The front Location still carries a Door object after the removal");
		require(scene.front->surfaceVariantAt(2, 0) == 1u,
			"Removing the Door took the painted entry with it");
		auto const withoutDoor = drawGrid(scene, atlas);
		require(tileAt(withoutDoor, 2, 0) == "room-V1", "With the Door gone the "
			"cell drew " + tileAt(withoutDoor, 2, 0) + "; the painting the "
			"author put there was supposed to come back");
	}

	// Two doors side by side each frame their own cells. Each cell answers
	// for exactly one Door, so neither opening's frame can reach into the
	// other's, and the pair of 1-wide openings draws two `L0` tiles rather
	// than one `L0` and one `R0` - the role comes from the Door the cell
	// belongs to, not from where the cell sits in the wall.
	void adjacentDoorsFrameTheirOwnCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto world = std::make_shared<core::World>("Two door world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 0, 0, 6, 1);
		world->addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.width = 1;
		world->addSectorDoor(0, 0, 2, options);
		world->addSectorDoor(0, 0, 3, options);
		options.width = 2;
		world->addSectorDoor(0, 0, 4, options);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.refresh();
		require(scene.front->getCellsWide() == 6,
			"The two-door fixture lost its front Room");
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 0, 0) == "room" && tileAt(grid, 1, 0) == "room",
			"The cells before the openings drew " + tileAt(grid, 0, 0) + " and "
			+ tileAt(grid, 1, 0));
		require(tileAt(grid, 2, 0) == "doorframe-room-1r-L0", "The first 1-wide "
			"door drew " + tileAt(grid, 2, 0));
		require(tileAt(grid, 3, 0) == "doorframe-room-1r-L0", "The second "
			"1-wide door drew " + tileAt(grid, 3, 0) + "; its neighbour is a "
			"different Door, so it is a left cell too");
		require(tileAt(grid, 4, 0) == "doorframe-room-2r-L0", "The 2-wide door's "
			"left cell drew " + tileAt(grid, 4, 0));
		require(tileAt(grid, 5, 0) == "doorframe-room-2r-R0", "The 2-wide door's "
			"right cell drew " + tileAt(grid, 5, 0));
	}

	// A Door removed from a cell reverts it to its base with no residue:
	// the same cells, the same tiles, the same rectangles as a Room that
	// never had a Door in it at all.
	void aRemovedDoorRevertsWithNoResidue(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto plain = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const framed = drawGrid(plain, atlas);
		require(tileAt(framed, 2, 0) == "doorframe-room-1r-L0",
			"The fixture door cell did not frame before it was removed");

		// A second World built the same way, with the Door taken back out.
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		require(scene.world->removeSectorDoor(scene.frontIndex,
				doorObjectIndex(*scene.front)),
			"The fixture Door could not be removed");
		scene.refresh();
		auto const reverted = drawGrid(scene, atlas);
		require(reverted.size() == framed.size(),
			"Removing the Door changed how many cells the Room drew");
		for (uint32_t column = 0; column < 4; ++column)
		{
			require(tileAt(reverted, column, 0) == "room", "Column "
				+ std::to_string(column) + " still draws "
				+ tileAt(reverted, column, 0) + " after the Door went");
			auto const& before = framed.at({ column, 0 });
			auto const& after = reverted.at({ column, 0 });
			require(near(before.left, after.left)
					&& near(before.right, after.right)
					&& near(before.top, after.top)
					&& near(before.bottom, after.bottom),
				"Column " + std::to_string(column) + " moved when the Door was "
				"removed: " + describe(before) + " against " + describe(after));
		}
	}

	// The frame is keyed by the wall the Door is set into, and by its front
	// side only. The same Door object is registered on the Sector behind,
	// and that Sector draws its own base: it is seen through the opening,
	// not framed by it.
	void theBackWallOfAThresholdTakesNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		std::shared_ptr<const core::Sector> back;
		for (uint32_t index = 0; index < scene.world->getNumSectors(); ++index)
		{
			auto const candidate = scene.world->getSector(index);
			if (candidate->getName() == "Back") back = candidate;
		}
		require(back != nullptr, "The fixture lost its back Room");
		require(doorObjectIndex(*back) != ~0u,
			"The Door is not registered on the back Sector, so this case proves "
			"nothing about a Door seen from behind");
		auto const grid = gridOf(recordFrontSurface(back, back->getLayerIndex(),
			atlas, kNoClip), *back);
		require(tileAt(grid, 2, 0) == "room", "The back wall of the opening drew "
			+ tileAt(grid, 2, 0) + "; a Door is drawn from its front Layer, and "
			"the Sector behind the same threshold keeps its own base tile");
	}

	// A wall with no frame authored draws its base rather than the
	// missing-art colour. That covers both ways a frame can be absent: a
	// Sector type nobody authored frames for, and an atlas written before
	// doorframes existed at all.
	void aWallWithNoFrameAuthoredDrawsItsBase(smoke::Context const&)
	{
		// The atlas with no frames anywhere: what a World drawn before this
		// feature existed has installed.
		auto noFrames = installAtlas(false);
		require(!hasSectorTileOverlay("doorframe-room-1r-L0"),
			"The frame-free atlas carries a doorframe, so the fallback below is "
			"not being tested");
		require(hasSectorTileSurface("room"),
			"The frame-free atlas carries no Room surface");
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const grid = drawGrid(scene, noFrames);
		require(tileAt(grid, 2, 0) == "room", "A door cell whose atlas has no "
			"frame drew " + tileAt(grid, 2, 0) + "; a frame the atlas does not "
			"carry is no frame at all, and the cell falls back to its wall's "
			"tile rather than to the missing-art colour");
		for (uint32_t column = 0; column < 4; ++column)
			require(tileAt(grid, column, 0) == "room", "Column "
				+ std::to_string(column) + " drew "
				+ tileAt(grid, column, 0) + " on a frame-free atlas");
	}

	// A frame is a tile of the atlas and not a Sector surface, and the two
	// queries keep that apart. The frame is reachable - the door cell above
	// drew it - without the surface query answering yes for it.
	void aFrameIsReachableWithoutBeingASurface(smoke::Context const&)
	{
		installAtlas(true);
		for (auto const& name : kFrames)
		{
			require(hasSectorTileOverlay(name), "The installed atlas carries no "
				"doorframe '" + name + "'");
			require(!hasSectorTileSurface(name), "The installed atlas answers "
				"'is this a Sector surface' yes for the doorframe '" + name
				+ "'; a frame is not one, and the borrow rule and the swatch "
				"strip both read that query looking for walls");
		}
		require(!hasSectorTileOverlay("room"), "A plain Sector surface answers "
			"the doorframe query yes");
	}
}

void render_smoke::registerDoorframeCells(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorframeCells/oneWideRoomDoorWearsItsFrame",
		isolated<oneWideRoomDoorWearsItsFrame> });
	checks.push_back({ "doorframeCells/twoWideRoomDoorWearsItsFramePair",
		isolated<twoWideRoomDoorWearsItsFramePair> });
	checks.push_back({ "doorframeCells/tallRoomDoorWearsItsTallFrame",
		isolated<tallRoomDoorWearsItsTallFrame> });
	checks.push_back({ "doorframeCells/corridorDoorsWearCorridorFrames",
		isolated<corridorDoorsWearCorridorFrames> });
	checks.push_back({ "doorframeCells/theFrameKeepsTheFloorAnchoredCrop",
		isolated<theFrameKeepsTheFloorAnchoredCrop> });
	checks.push_back({ "doorframeCells/aClippedDoorCellIsNeverDrawn",
		isolated<aClippedDoorCellIsNeverDrawn> });
	checks.push_back({ "doorframeCells/aVariantUnderADoorIsHiddenAndComesBack",
		isolated<aVariantUnderADoorIsHiddenAndComesBack> });
	checks.push_back({ "doorframeCells/adjacentDoorsFrameTheirOwnCells",
		isolated<adjacentDoorsFrameTheirOwnCells> });
	checks.push_back({ "doorframeCells/aRemovedDoorRevertsWithNoResidue",
		isolated<aRemovedDoorRevertsWithNoResidue> });
	checks.push_back({ "doorframeCells/theBackWallOfAThresholdTakesNoFrame",
		isolated<theBackWallOfAThresholdTakesNoFrame> });
	checks.push_back({ "doorframeCells/aWallWithNoFrameAuthoredDrawsItsBase",
		isolated<aWallWithNoFrameAuthoredDrawsItsBase> });
	checks.push_back({ "doorframeCells/aFrameIsReachableWithoutBeingASurface",
		isolated<aFrameIsReachableWithoutBeingASurface> });
}
