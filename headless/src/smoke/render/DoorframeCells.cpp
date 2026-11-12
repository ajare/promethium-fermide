// A cell a Door stands in wears its wall's doorframe tile (#1054, #1059).
//
// #1037 pinned the geometry of a Sector surface, #1039 the seam that asks
// which tile each of those cells wears, and #1040 what a recorded variant
// does at that seam. This is the next thing to answer through the same
// seam, and the first whose answer comes from a live object rather than
// from what an author painted: a Door set into a Room, a Corridor or a
// Facade puts that wall's doorframe tile on the cells it covers, and
// leaves nothing behind when it goes away.
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
//     colour, which is what makes the frame additive rather than a break;
//   * a Door whose aperture was scaled below the height its frame's hole
//     is baked at wears no frame for the same reason and with the same
//     fallback. The frame replaces the wall's tile rather than lying over
//     it, so a frame whose hole is bigger than its opening leaves the band
//     between the two drawing nothing - the viewport background straight
//     through the wall (#1062).
//
// A Facade answers the same seam through a different door, because it has
// no surface for the resolver to stand on (#1059). Its surface is a flat
// fill emitted before the thresholds, so its frame is laid over that fill
// on the door cells and nowhere else, and the checks that ride it are the
// ones the flat-fill rule was written to protect:
//
//   * the fill still covers the whole Facade, every cell included, so the
//     frame is a tile laid over it and not a hole punched in it;
//   * the frame is emitted before anything clipped to an aperture, so it
//     can sit under a threshold's opening and never over one;
//   * no frame quad reaches an aperture belonging to a different Door;
//   * a Facade cell that is not a door cell resolves to no tile at all and
//     draws nothing - not the missing-art colour, because nothing was
//     asked for;
//   * the tall Facade frame is derived, carried and drawn - #1059 authored
//     the tall pair ahead of the tall Facade Door that wears it, and #1060
//     admits that Door.
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

#include "core/AgentTagRegistryDocument.h"
#include "core/Background.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <optional>
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

	// The frames the shipped Room, Corridor and Facade set authors. A
	// Corridor gets no tall frame because a tall Door is refused in one; a
	// Facade gets the tall pair even though a tall Door is refused in one
	// too today, because that refusal is #1060's to lift.
	std::vector<std::string> const kFrames{
		"doorframe-room-1r-L0", "doorframe-room-2r-L0", "doorframe-room-2r-R0",
		"doorframe-room-1t-L0", "doorframe-room-2t-L0", "doorframe-room-2t-R0",
		"doorframe-corridor-1r-L0", "doorframe-corridor-2r-L0",
		"doorframe-corridor-2r-R0",
		"doorframe-facade-1r-L0", "doorframe-facade-2r-L0",
		"doorframe-facade-2r-R0", "doorframe-facade-1t-L0",
		"doorframe-facade-2t-L0", "doorframe-facade-2t-R0" };

	// The height a regular frame's transparent hole is baked at, in World
	// units above the cell floor. Every regular frame - Room, Corridor and
	// Facade alike - is cut at 0.5, which is the whole of an unscaled
	// regular aperture. A Door scaled below it has no frame that fits, and
	// the band between the two is what #1062 is about.
	float const kRegularFrameHoleTop = 0.5f;

	// The Facade's own colour, distinct from the tint every other pass in
	// this file is drawn with, so "the frame wears the Facade's colour"
	// cannot be confused with the caller's Layer tint.
	core::BackgroundColour const kFacadeColour{ 210, 66, 138 };
	pf::render::PackedColourValue const kFacadeFill
	{
		packColour(kFacadeColour.r, kFacadeColour.g, kFacadeColour.b)
	};

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

	// The World-to-screen mapping the viewport applies at the zoom pinned
	// above, stated once so the check that reads a cell grid and the check
	// that reads a Door's aperture cannot disagree about where a World point
	// lands.
	pf::render::Vec2 toScreen(core::Vector2 const& point)
	{
		return pf::render::Vec2{
			point.x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom
				+ gUISettings.worldViewportX + gUISettings.xOffset,
			gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- point.y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom
				- gUISettings.yOffset };
	}

	// A screen rect, with the one question this file asks of a pair of them.
	struct ScreenRect
	{
		float left{ 1e9f }, top{ 1e9f }, right{ -1e9f }, bottom{ -1e9f };

		void include(pf::render::Vec2 const& point)
		{
			left = std::min(left, point.x);
			right = std::max(right, point.x);
			top = std::min(top, point.y);
			bottom = std::max(bottom, point.y);
		}

		bool overlaps(ScreenRect const& o) const
		{
			return left < o.right && right > o.left && top < o.bottom
				&& bottom > o.top;
		}
	};

	// Renders one Sector into a command stream of its own, under the clip
	// the caller reads the pass back against.
	WorldDrawList renderPass(std::shared_ptr<const core::Sector> sector,
		uint32_t const layer, WorldDrawList::ClipRectangle const& clip,
		LayerRenderStyle const style = LayerRenderStyle::Solid)
	{
		WorldDrawList commands(clip);
		renderSector(sector, layer, style, false, kTint, &commands);
		return commands;
	}

	// Records the front Sector's own surface pass.
	//
	// A Door draws the Sector behind through its aperture after the front
	// surface is down, under a clip pushed to the doorway. Those cells are
	// the back Sector's surface, not this one's, and they are told apart by
	// the clip they were emitted under: the front surface pass runs under
	// the clip this recorder opened and nothing else does.
	//
	// `expectedTint` is the colour every textured cell has to carry. For a
	// Room or a Corridor that is the caller's Layer tint; for a Facade it is
	// the Facade's own colour, because the frame is moulding on that wall
	// and that wall's colour is the Facade's (#1059).
	std::vector<RecordedCell> recordFrontSurface(WorldDrawList const& commands,
		SectorTileset const& atlas,
		WorldDrawList::ClipRectangle const& clip,
		pf::render::PackedColourValue const& expectedTint = kTint)
	{
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
				require(first.colour == expectedTint,
					"A textured surface quad carried a colour other than the "
					"tint this pass draws with");
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
	// The Facade's own pass, read out whole.
	// ------------------------------------------------------------------

	// Everything a Facade's pass emits, and the order it came out in.
	//
	// A Facade is the case where the order is the whole of the safety, so
	// the recorder keeps it rather than only the cells: the flat fill exists
	// because nothing in the threshold path may paint over an aperture, and
	// the frame has to keep that true while covering the fill it sits on.
	struct FacadePass
	{
		std::map<std::pair<uint32_t, uint32_t>, RecordedCell> cells;
		// The flat fill: how many triangles it came out as, the rect they
		// cover between them, and where the first of them landed.
		int fillTriangles{};
		ScreenRect fill{};
		size_t firstFillIndex{ std::numeric_limits<size_t>::max() };
		size_t lastFrameIndex{};
		// The first command drawn under any clip other than the recorder's
		// own. Every aperture-clipped draw - a leaf, a back Sector seen
		// through an opening - lands here, and nothing else does.
		size_t firstClippedIndex{ std::numeric_limits<size_t>::max() };
		bool hasFrame{ false };
	};

	FacadePass recordFacade(std::shared_ptr<const core::Sector> sector,
		SectorTileset& atlas,
		WorldDrawList::ClipRectangle const& clip = kNoClip,
		LayerRenderStyle const style = LayerRenderStyle::Solid)
	{
		auto const commands = renderPass(sector, sector->getLayerIndex(), clip,
			style);
		FacadePass pass;
		pass.cells = gridOf(recordFrontSurface(commands, atlas, clip,
			kFacadeFill), *sector);
		size_t index{};
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle)
			{
				auto const& c = triangle->clip;
				bool const outer = near(c.minimum.x, clip.minimum.x)
					&& near(c.minimum.y, clip.minimum.y)
					&& near(c.maximum.x, clip.maximum.x)
					&& near(c.maximum.y, clip.maximum.y);
				if (triangle->texture == WorldDrawList::Texture::SectorAtlas
						&& outer)
				{
					pass.hasFrame = true;
					pass.lastFrameIndex = index;
				}
				else if (!outer)
				{
					pass.firstClippedIndex = std::min(pass.firstClippedIndex, index);
				}
				else if (triangle->colour == kFacadeFill)
				{
					++pass.fillTriangles;
					pass.firstFillIndex = std::min(pass.firstFillIndex, index);
					for (auto const& p : triangle->positions) pass.fill.include(p);
				}
			}
			++index;
		}
		return pass;
	}

	// Every Door the front Sector carries, as the Sector-local columns it
	// stands in and the aperture it owns, in screen coordinates.
	struct DoorAperture
	{
		uint32_t firstColumn{};
		uint32_t cellsWide{};
		ScreenRect aperture{};
	};

	std::vector<DoorAperture> doorApertures(
		std::shared_ptr<const core::Sector> sector)
	{
		std::vector<DoorAperture> found;
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto const object = sector->getObject(index);
			if (!object || object->getObjectType() != core::SectorObjectType::Door)
				continue;
			auto const door = std::static_pointer_cast<
				const core::DoorSectorObject>(object)->getDoor();
			// Only the front wall's own Doors; a Door registered from behind
			// frames nothing and owns no aperture here.
			if (!door || door->getFrontSector() != sector) continue;
			core::Vector2 lo{}, hi{};
			door->getFullShape(lo, hi);
			DoorAperture entry;
			entry.firstColumn = object->getCellX() - sector->getCellX0();
			entry.cellsWide = door->getCellsWide();
			entry.aperture.include(toScreen(lo));
			entry.aperture.include(toScreen(hi));
			found.push_back(entry);
		}
		return found;
	}

	ScreenRect cellRect(RecordedCell const& cell)
	{
		ScreenRect rect;
		rect.include(pf::render::Vec2{ cell.left, cell.top });
		rect.include(pf::render::Vec2{ cell.right, cell.bottom });
		return rect;
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

	// The Door standing in a fixture Sector, for the checks that have to ask
	// the Door what it was authored as rather than assume the fixture set
	// what it meant to set.
	std::shared_ptr<const core::Door> doorOf(core::Sector const& sector)
	{
		auto const index = doorObjectIndex(sector);
		require(index != ~0u, "The fixture Sector carries no Door to ask");
		return std::static_pointer_cast<const core::DoorSectorObject>(
			sector.getObject(index))->getDoor();
	}

	// Whether a resolved tile is a doorframe. Asked of the name rather than
	// of a list, because the checks below need to say "nothing that is a
	// frame drew here" over a whole row, and the frame family is exactly the
	// names the derivation makes.
	bool isFrameTile(std::string const& name)
	{
		return name.rfind("doorframe-", 0) == 0;
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
		uint32_t doorX, uint32_t doorLevel, float frontTopLevelHeight,
		std::optional<float> doorHeightScale = std::nullopt)
	{
		auto world = std::make_shared<core::World>("Doorframe world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		if (kind == "corridor")
		{
			world->addCorridor(0, 0, 0, cellsWide, levelsHigh);
			world->addCorridor(1, 0, 0, cellsWide, levelsHigh);
		}
		else if (kind == "facade")
		{
			// A Facade in front and a Room behind: the Facade is the wall
			// being framed and the Room is the space its opening shows.
			world->addFacade("Front", 0, 0, 0, cellsWide, levelsHigh,
				frontTopLevelHeight, kFacadeColour);
			world->addRoom("Back", 1, 0, 0, cellsWide, levelsHigh,
				frontTopLevelHeight);
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
		options.heightScale = doorHeightScale;
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
		uint32_t levelsHigh = 1,
		std::optional<float> doorHeightScale = std::nullopt)
	{
		return buildScene("room", cellsWide, levelsHigh, doorWidth, height,
			doorX, doorLevel, topLevelHeight, doorHeightScale);
	}

	Scene corridorScene(uint32_t cellsWide, uint32_t doorWidth,
		core::Door::Height height, uint32_t doorX, uint32_t doorLevel = 0,
		std::optional<float> doorHeightScale = std::nullopt)
	{
		// A Corridor is one level high and takes its own height, not the
		// Room's: the top-level height a Room author sets means nothing to
		// it, and the loader would say so.
		return buildScene("corridor", cellsWide, 1, doorWidth, height, doorX,
			doorLevel, CORE_CORRIDOR_HEIGHT, doorHeightScale);
	}

	// One Facade on the front Layer, one Room behind it, one ordinary Door
	// set into the Facade.
	Scene facadeScene(uint32_t cellsWide, uint32_t doorWidth,
		core::Door::Height height, uint32_t doorX, uint32_t doorLevel = 0,
		std::optional<float> doorHeightScale = std::nullopt)
	{
		return buildScene("facade", cellsWide, 1, doorWidth, height, doorX,
			doorLevel, CORE_ROOM_MAX_HEIGHT, doorHeightScale);
	}

	std::map<std::pair<uint32_t, uint32_t>, RecordedCell> drawGrid(Scene& scene,
		SectorTileset& atlas,
		WorldDrawList::ClipRectangle const& clip = kNoClip,
		pf::render::PackedColourValue const& expectedTint = kTint)
	{
		return gridOf(recordFrontSurface(renderPass(scene.front,
				scene.front->getLayerIndex(), clip), atlas, clip, expectedTint),
			*scene.front);
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
		auto const grid = gridOf(recordFrontSurface(renderPass(back,
			back->getLayerIndex(), kNoClip), atlas, kNoClip), *back);
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

	// ------------------------------------------------------------------
	// A Door whose aperture does not fill its frame wears no frame (#1062).
	// ------------------------------------------------------------------
	//
	// A frame's hole is baked at its height class. `heightScale` shrinks
	// the aperture below the hole it would stand in, and because the frame
	// *replaces* the wall's tile rather than lying over it, the band from
	// the top of the aperture to the top of the hole draws nothing - the
	// viewport background straight through the wall. A Door with no frame
	// that fits is answered the way a wall with no frame authored is: the
	// cell takes its base tile, or the variant its author painted there.
	//
	// The guard is on the mismatch, not on the key being present. A scale
	// of exactly 1.0 leaves the aperture at the height the hole was cut
	// at, and that Door wears its frame; a Door with no scale at all is
	// the same Door wearing the same frame.

	// A 0.5-scaled regular Door in a Room draws the Room's base tile on
	// its cell, and no frame anywhere on the wall.
	void aScaledRegularDoorWearsNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		require(hasSectorTileOverlay("doorframe-room-1r-L0"),
			"The atlas carries no regular Room frame, so the fallback below is "
			"tested against a wall that could not have framed anything");
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2, 0,
			CORE_ROOM_MAX_HEIGHT, 1, /*doorHeightScale=*/ 0.5f);
		auto const door = doorOf(*scene.front);
		require(door->getHeightScale() == std::optional<float>{0.5f},
			"The fixture's height scale never reached the Door, so this case is "
			"an ordinary unscaled Door and pins nothing");
		// The mismatch, stated as the two numbers that have to disagree for
		// the band to exist: the aperture the leaf and the reveal are
		// clipped to, and the height the frame's artwork is cut at.
		require(near(door->getSize().y, CORE_DOOR_HEIGHT * 0.5f),
			"The scaled aperture is not half a regular Door, so the fixture is "
			"not the mismatch this case describes");
		require(door->getSize().y < kRegularFrameHoleTop,
			"The scaled aperture reaches the frame's hole, so nothing would have "
			"drawn through the wall and the guard below is not being tested");
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 2, 0) == "room", "The cell a 0.5-scaled regular "
			"Door stands in drew " + tileAt(grid, 2, 0) + "; a scaled aperture "
			"is smaller than the frame's baked hole, so the Door wears no frame "
			"and the cell falls back to its wall's base tile");
		for (uint32_t column = 0; column < 4; ++column)
			require(!isFrameTile(tileAt(grid, column, 0)), "Column "
				+ std::to_string(column) + " drew " + tileAt(grid, column, 0)
				+ "; a scaled Door frames no cell of its wall");
		// And the neighbours are untouched: taking the frame off the door
		// cell changed nothing else about the Room.
		require(tileAt(grid, 0, 0) == "room" && tileAt(grid, 1, 0) == "room"
				&& tileAt(grid, 3, 0) == "room",
			"Scaling one Door changed what its neighbours draw");
	}

	// A scale of exactly 1.0 is not a mismatch. The aperture is the height
	// the hole was baked at, the band is nothing, and the Door wears its
	// frame - as it does with the scale left unset entirely. The guard is
	// on the mismatch, not on the key being present.
	void aScaleOfOneStillWearsItsFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = roomScene(4, 1, core::Door::Height::Regular, 2, 0,
				CORE_ROOM_MAX_HEIGHT, 1, /*doorHeightScale=*/ 1.0f);
			auto const door = doorOf(*scene.front);
			require(door->getHeightScale() == std::optional<float>{1.0f},
				"The explicit 1.0 scale did not reach the Door, so this case is "
				"not testing the key being present");
			require(near(door->getSize().y, CORE_DOOR_HEIGHT),
				"A 1.0-scaled aperture is not the full regular height, so the "
				"frame it wears would not line up with its hole anyway");
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 2, 0) == "doorframe-room-1r-L0", "A regular "
				"Door authored with heightScale 1.0 drew " + tileAt(grid, 2, 0)
					+ "; the guard is on the mismatch between aperture and hole, "
					"not on the scale key being present");
		}
		{
			// The unset control: the same cell, the same frame, no scale at
			// all. Both halves of the guard's boundary answer the same way.
			auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
			require(!doorOf(*scene.front)->getHeightScale().has_value(),
				"An unscaled fixture Door reports a height scale, so the control "
				"below is not the unset case");
			auto const grid = drawGrid(scene, atlas);
			require(tileAt(grid, 2, 0) == "doorframe-room-1r-L0", "An unscaled "
				"regular Door drew " + tileAt(grid, 2, 0) + "; taking the scale "
				"away is not a reason to take the frame away");
		}
	}

	// The fallback is the wall's own answer, variant included. A variant
	// painted on the door cell shows through while a scaled Door stands
	// there, because nothing outranks it: the frame that used to is the one
	// that would have left the hole.
	void aScaledDoorStillShowsTheVariantUnderIt(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2, 0,
			CORE_ROOM_MAX_HEIGHT, 1, /*doorHeightScale=*/ 0.4f);
		require(doorOf(*scene.front)->getHeightScale()
				== std::optional<float>{0.4f},
			"The fixture's height scale never reached the Door");
		std::string diagnostic;
		require(scene.world->setSectorSurfaceVariant(scene.frontIndex, 2, 0, 1u,
				&diagnostic).changed,
			"The fixture variant could not be painted on the door cell: "
			+ diagnostic);
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 2, 0) == "room-V1", "The cell a 0.4-scaled Door "
			"stands in drew " + tileAt(grid, 2, 0) + "; with no frame to "
			"outrank it the cell answers the variant its author painted, which "
			"is the same fallback the missing-frame case answers with");
		require(scene.front->surfaceVariantAt(2, 0) == 1u,
			"The painted entry did not stay on the Sector while the scaled Door "
			"stood there");
	}

	// The same rule in a Corridor, where the shipped defect sits: the
	// Corridor's frame is cut at the same 0.5 and its scaled Door gets the
	// same answer - the Corridor's own base tile.
	void aScaledDoorInACorridorWearsNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		require(hasSectorTileOverlay("doorframe-corridor-1r-L0"),
			"The atlas carries no regular Corridor frame, so the fallback below "
			"is tested against a wall that could not have framed anything");
		auto scene = corridorScene(4, 1, core::Door::Height::Regular, 2, 0,
			/*doorHeightScale=*/ 0.5f);
		require(doorOf(*scene.front)->getHeightScale()
				== std::optional<float>{0.5f},
			"The fixture's height scale never reached the Corridor Door");
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 2, 0) == "corridor", "The cell a 0.5-scaled Door "
			"in a Corridor stands in drew " + tileAt(grid, 2, 0) + "; the "
			"Corridor's frame is cut at the same hole height, so a scaled "
			"opening in one wears no frame either");
		for (uint32_t column = 0; column < 4; ++column)
			require(!isFrameTile(tileAt(grid, column, 0)), "Column "
				+ std::to_string(column) + " of the Corridor drew "
				+ tileAt(grid, column, 0) + "; a scaled Door frames no cell");
	}

	// A Tall Door is unaffected. Its frame's hole is baked at its own
	// height class, and the domain never lets a Tall Door carry a scale in
	// the first place - so the guard written for the regular mismatch has
	// nothing to take away here.
	void aTallDoorIsUnaffectedByTheScaleGuard(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Tall, 1);
		auto const door = doorOf(*scene.front);
		require(!door->getHeightScale().has_value(),
			"A Tall Door came to carry a height scale; the domain rule is that "
			"Tall refuses one, and a Tall Door that carried a scale would lose "
			"its tall frame to a guard written for the regular case");
		// And it still refuses one at the authoring seam, which is what "the
		// guard cannot reach a Tall Door" actually rests on.
		auto const object = std::static_pointer_cast<const core::DoorSectorObject>(
			scene.front->getObject(doorObjectIndex(*scene.front)));
		std::string diagnostic;
		require(!scene.world->setSectorDoorHeightScale(
				scene.front->getLayerIndex(), object->getCellY(),
				object->getCellX(), door->getCellsWide(), 0.5f, &diagnostic),
			"The authoring seam accepted a height scale on a Tall Door: "
			+ diagnostic);
		auto const grid = drawGrid(scene, atlas);
		require(tileAt(grid, 1, 0) == "doorframe-room-1t-L0", "A Tall Door "
			"drew " + tileAt(grid, 1, 0) + "; the scale guard is not allowed "
			"to take a tall frame off a tall opening");
		require(isFrameTile(tileAt(grid, 1, 0)),
			"The tall cell's tile is not a frame at all, so the check above "
			"matched a fallback rather than a tall frame being worn");
	}

	// ------------------------------------------------------------------
	// The Facade's frames (#1059).
	// ------------------------------------------------------------------

	// A 1-wide Door in a Facade draws `doorframe-facade-1r-L0` on its
	// cell. Every other cell draws no tile at all: a Facade carries no
	// surface, so the atlas is only ever asked for on a door cell, and what
	// shows elsewhere is the flat fill - a fill, not a cell of the atlas.
	void oneWideFacadeDoorWearsItsFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
		auto const grid = drawGrid(scene, atlas, kNoClip, kFacadeFill);
		require(grid.size() == 1, "A four-cell Facade drew "
			+ std::to_string(grid.size()) + " tiled cells; a Facade carries no "
			"surface tile, so only a door cell resolves to the atlas at all");
		require(tileAt(grid, 2, 0) == "doorframe-facade-1r-L0", "The door cell "
			"drew " + tileAt(grid, 2, 0) + "; a 1-wide regular Door in a "
			"Facade wears doorframe-facade-1r-L0");
		require(tileAt(grid, 2, 0) == doorframeTileName("facade", 1,
				core::Door::Height::Regular, false),
			"The tile that drew is not the name the derivation states for a "
			"1-wide regular Facade opening; the draw and the naming rule have "
			"come apart");
		require(tileAt(grid, 0, 0) == "<nothing>"
				&& tileAt(grid, 1, 0) == "<nothing>"
				&& tileAt(grid, 3, 0) == "<nothing>",
			"A Facade cell with no Door in it drew a tile; a Facade resolves to "
			"no tile on every cell that is not a door cell, and the flat fill "
			"covers those");
	}

	// A 2-wide Door in a Facade draws the pair across its two cells, left
	// `L0` and right `R0`, one cell each and in that order.
	void twoWideFacadeDoorWearsItsFramePair(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(5, 2, core::Door::Height::Regular, 2);
		auto const grid = drawGrid(scene, atlas, kNoClip, kFacadeFill);
		require(grid.size() == 2, "A 2-wide Facade opening drew "
			+ std::to_string(grid.size()) + " tiled cells; the pair is two, one "
			"per cell of the opening");
		require(tileAt(grid, 2, 0) == "doorframe-facade-2r-L0", "The opening's "
			"left cell drew " + tileAt(grid, 2, 0) + "; a 2-wide regular Door "
			"in a Facade wears doorframe-facade-2r-L0 on its left cell");
		require(tileAt(grid, 3, 0) == "doorframe-facade-2r-R0", "The opening's "
			"right cell drew " + tileAt(grid, 3, 0) + "; a 2-wide regular Door "
			"in a Facade wears doorframe-facade-2r-R0 on its right cell");
	}

	// The Facade's flat-colour fill still covers every cell that is not a
	// door cell - and the door cells too, which is exactly what lets the
	// frame be a tile laid over the fill rather than a hole punched out of
	// it. The frame lands over the fill, never under it.
	void theFlatFillStillCoversEveryCellThatIsNotADoorCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(5, 2, core::Door::Height::Regular, 2);
		auto const pass = recordFacade(scene.front, atlas);
		require(pass.hasFrame, "The framed Facade drew no frame at all, so the "
			"ordering below proves nothing");
		require(pass.fillTriangles == 2, "The Facade's fill came out as "
			+ std::to_string(pass.fillTriangles)
			+ " triangles; one whole-area rect makes two, and a fill narrowed "
			"to route around the door cells would make more");
		core::Vector2 lo{}, hi{};
		scene.front->getBounds(lo, hi);
		ScreenRect area;
		area.include(toScreen(lo));
		area.include(toScreen(hi));
		require(near(pass.fill.left, area.left) && near(pass.fill.right, area.right)
				&& near(pass.fill.top, area.top)
				&& near(pass.fill.bottom, area.bottom),
			"The fill spans (" + std::to_string(pass.fill.left) + ","
			+ std::to_string(pass.fill.top) + ")-("
			+ std::to_string(pass.fill.right) + ","
			+ std::to_string(pass.fill.bottom) + ") where the Facade spans ("
			+ std::to_string(area.left) + "," + std::to_string(area.top)
			+ ")-(" + std::to_string(area.right) + ","
			+ std::to_string(area.bottom) + "); the fill has to cover the whole "
			"Facade, every cell of it included");
		require(pass.firstFillIndex < pass.lastFrameIndex,
			"The frame came out at index " + std::to_string(pass.lastFrameIndex)
			+ ", before the fill at " + std::to_string(pass.firstFillIndex)
			+ "; a frame under the fill is a frame nobody can see");
	}

	// The frame is emitted before anything clipped to an aperture. That is
	// the structural reason it cannot paint one out: every threshold draw -
	// this Door's own and every other one's - comes after it and paints over
	// the frame rather than the reverse, which is the same reason the
	// Facade's fill is emitted this early in the pass.
	void theFrameIsEmittedBeforeEveryAperture(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(5, 1, core::Door::Height::Regular, 2);
		auto const pass = recordFacade(scene.front, atlas);
		require(pass.hasFrame, "The Facade fixture drew no frame at all, so "
			"the ordering below proves nothing");
		require(pass.firstClippedIndex != std::numeric_limits<size_t>::max(),
			"The fixture drew nothing under a pushed clip, so the ordering "
			"below would prove nothing about apertures");
		require(pass.lastFrameIndex < pass.firstClippedIndex,
			"A command clipped to an aperture came out at index "
			+ std::to_string(pass.firstClippedIndex) + ", before the frame at "
			+ std::to_string(pass.lastFrameIndex) + "; a frame drawn after an "
			"aperture can paint it out, which is the thing the flat-fill rule "
			"exists to prevent");
	}

	// Two 1-wide Doors side by side in one Facade. Each frame is confined
	// to the cell its own Door stands in, so neither reaches the other's
	// aperture - the case the flat-fill rule was written for, with the frame
	// now in the picture.
	void theFrameNeverReachesAnotherThresholdsAperture(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto world = std::make_shared<core::World>("Two facade door world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addFacade("Front", 0, 0, 0, 6, 1, CORE_ROOM_MAX_HEIGHT,
			kFacadeColour);
		world->addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.width = 1;
		world->addSectorDoor(0, 0, 2, options);
		world->addSectorDoor(0, 0, 3, options);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.frontLayer = 0;
		scene.refresh();
		auto const apertures = doorApertures(scene.front);
		require(apertures.size() == 2, "The two-door Facade fixture carries "
			+ std::to_string(apertures.size()) + " front Doors; the check needs "
			"exactly two");

		auto const pass = recordFacade(scene.front, atlas);
		require(tileAt(pass.cells, 2, 0) == "doorframe-facade-1r-L0"
				&& tileAt(pass.cells, 3, 0) == "doorframe-facade-1r-L0",
			"The two adjacent Facade openings drew " + tileAt(pass.cells, 2, 0)
			+ " and " + tileAt(pass.cells, 3, 0) + "; each is its own 1-wide "
			"opening and each wears its own left frame");

		for (auto const& [where, cell] : pass.cells)
		{
			// The positive control first: every frame cell does overlap the
			// aperture of the Door standing in that cell. Without it the
			// non-overlap checks below would pass on an aperture rect that was
			// simply empty, which is the one way this test could lie.
			bool const framed = std::any_of(apertures.begin(), apertures.end(),
				[&](DoorAperture const& own)
				{
					return where.first >= own.firstColumn
						&& where.first < own.firstColumn + own.cellsWide
						&& cellRect(cell).overlaps(own.aperture);
				});
			require(framed, "The frame on column " + std::to_string(where.first)
				+ " does not overlap the aperture of the Door standing in that "
				"cell, so the aperture rects this check reads are not the real "
				"ones");
			for (auto const& other : apertures)
			{
				bool const owns = where.first >= other.firstColumn
					&& where.first < other.firstColumn + other.cellsWide;
				if (owns) continue;
				require(!cellRect(cell).overlaps(other.aperture),
					"The frame on column " + std::to_string(where.first)
					+ " covers the aperture of the Door at column "
					+ std::to_string(other.firstColumn) + ": " + describe(cell)
					+ " against the aperture (" + std::to_string(other.aperture.left)
					+ "," + std::to_string(other.aperture.top) + ")-("
					+ std::to_string(other.aperture.right) + ","
					+ std::to_string(other.aperture.bottom) + ")");
			}
		}
	}

	// The tall Facade frame is derived, carried and drawn. #1059 authored
	// the tall pair ahead of the Door that wears it; #1060 admits that Door,
	// so the tile now reaches the screen through a live tall Facade opening
	// rather than only through the naming rule that predicted it.
	void theTallFacadeFrameIsDerivedAndDrawn(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		struct Case { uint32_t cellsWide; bool rightCell; std::string want; };
		std::vector<Case> const cases{
			{ 1, false, "doorframe-facade-1t-L0" },
			{ 2, false, "doorframe-facade-2t-L0" },
			{ 2, true, "doorframe-facade-2t-R0" } };
		for (auto const& c : cases)
		{
			auto const name = doorframeTileName("facade", c.cellsWide,
				core::Door::Height::Tall, c.rightCell);
			require(name == c.want, "A " + std::to_string(c.cellsWide)
				+ "-wide tall Facade opening's "
				+ std::string(c.rightCell ? "right" : "left")
				+ " cell resolves to '" + name + "'; expected " + c.want);
			require(hasSectorTileOverlay(name), "The installed atlas carries no "
				"tall Facade frame '" + name + "'");
			require(!hasSectorTileSurface(name), "The installed atlas answers "
				"'is this a Sector surface' yes for the tall Facade frame '"
				+ name + "'; a frame is not one");
		}
		// The live draw: a tall 1-wide Facade Door wears the tall single, and
		// a tall 2-wide one wears the tall pair in order. A tall opening that
		// drew the regular moulding would be the dead-space picture #1058 was
		// written to kill, wearing a frame.
		{
			auto scene = facadeScene(4, 1, core::Door::Height::Tall, 2);
			auto const grid = drawGrid(scene, atlas, kNoClip, kFacadeFill);
			require(tileAt(grid, 2, 0) == "doorframe-facade-1t-L0",
				"A tall 1-wide Facade Door's cell drew " + tileAt(grid, 2, 0)
					+ "; a tall Facade opening wears the tall frame");
			require(tileAt(grid, 1, 0) == "<nothing>"
					&& tileAt(grid, 3, 0) == "<nothing>",
				"A tall Facade opening framed cells its Door does not stand in");
		}
		{
			auto scene = facadeScene(5, 2, core::Door::Height::Tall, 2);
			auto const grid = drawGrid(scene, atlas, kNoClip, kFacadeFill);
			require(tileAt(grid, 2, 0) == "doorframe-facade-2t-L0",
				"A tall 2-wide Facade opening's left cell drew "
				+ tileAt(grid, 2, 0));
			require(tileAt(grid, 3, 0) == "doorframe-facade-2t-R0",
				"A tall 2-wide Facade opening's right cell drew "
				+ tileAt(grid, 3, 0));
		}
		// And the tall Facade Door keeps the flat-fill invariant the regular
		// one has: the fill spans the whole Facade, the frame lands over it,
		// and nothing clipped to an aperture is drawn before the frame.
		{
			auto scene = facadeScene(5, 2, core::Door::Height::Tall, 2);
			auto const pass = recordFacade(scene.front, atlas);
			require(pass.hasFrame, "The tall framed Facade drew no frame at all");
			require(pass.fillTriangles == 2, "The tall Facade's fill came out as "
				+ std::to_string(pass.fillTriangles)
				+ " triangles; the fill is not narrowed for a taller Door");
			require(pass.firstFillIndex < pass.lastFrameIndex,
				"The tall frame came out under the fill");
			require(pass.lastFrameIndex < pass.firstClippedIndex,
				"A command clipped to an aperture came out before the tall frame "
				"at index " + std::to_string(pass.lastFrameIndex) + "; a frame "
				"drawn after an aperture can paint it out");
		}
	}

	// A Facade drawn on an atlas that carries no Facade frames draws its
	// fill and nothing else - not the missing-art colour. The frame is
	// additive: an older atlas keeps the older picture rather than breaking,
	// and the fill never depended on the atlas in the first place.
	void aFacadeWithNoFacadeFramesAuthoredDrawsItsFill(smoke::Context const&)
	{
		auto atlas = installAtlas(false);
		require(!hasSectorTileOverlay("doorframe-facade-1r-L0"),
			"The frame-free atlas carries a Facade frame, so the fallback below "
			"is not being tested");
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
		auto const pass = recordFacade(scene.front, atlas);
		require(!pass.hasFrame, "A Facade door cell drew a SectorAtlas tile on "
			"an atlas with no Facade frames; a frame the atlas does not carry "
			"is no frame at all");
		require(pass.cells.empty(), "The frame-free Facade resolved "
			+ std::to_string(pass.cells.size()) + " cells to tiles; none should "
			"have been, and none should have come back as missing art");
		require(pass.fillTriangles == 2, "The frame-free Facade stopped filling "
			"as well as framing; the fill is not the part that depends on the "
			"atlas");
	}

	// A scaled Door set into a Facade lays no frame over its fill. A
	// Facade's wall is a flat fill rather than a tile, so what the guard
	// leaves behind is the picture a Facade with no frame authored gives:
	// the wall colour across the whole area, the opening drawn over it in
	// the pass that owns the aperture.
	void aScaledFacadeDoorDrawsNoFrameOverItsFill(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2, 0,
			/*doorHeightScale=*/ 0.5f);
		require(doorOf(*scene.front)->getHeightScale()
				== std::optional<float>{0.5f},
			"The fixture's height scale never reached the Facade Door");
		auto const pass = recordFacade(scene.front, atlas);
		require(!pass.hasFrame, "A 0.5-scaled Facade Door laid a frame over its "
			"wall; that frame's hole is cut at 0.5 and the aperture stops at "
			"0.25, so the band between them would show the viewport "
			"background straight through the Facade");
		require(pass.cells.empty(), "A scaled Facade opening resolved "
			+ std::to_string(pass.cells.size()) + " cells to tiles; none of "
			"them may be a frame");
		// The wall itself is untouched: the fill still covers the whole
		// Facade, and the opening is still drawn over it rather than being
		// painted out by it.
		require(pass.fillTriangles == 2, "The scaled Facade stopped filling as "
			"well as not framing; the fill is not the part that depends on the "
			"frame");
		require(pass.firstClippedIndex
				!= std::numeric_limits<size_t>::max(),
			"The scaled Facade Door drew nothing under an aperture clip, so the "
			"opening itself is missing and the checks above would pass on a "
			"wall with no doorway in it");
		require(pass.firstFillIndex < pass.firstClippedIndex,
			"The aperture was drawn before the fill, so the fill paints the "
			"opening out");
	}

	// The style gate is the fill's own. A Facade that fills wears its frame,
	// including when it is being seen through an aperture in the Layer in
	// front; the wireframe overlay contributes an outline only and takes no
	// tile at all.
	void theFrameFollowsTheFillThroughEveryStyle(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
		{
			auto const pass = recordFacade(scene.front, atlas, kNoClip,
				LayerRenderStyle::Aperture);
			require(pass.hasFrame
					&& tileAt(pass.cells, 2, 0) == "doorframe-facade-1r-L0",
				"A Facade drawn through an aperture drew "
				+ tileAt(pass.cells, 2, 0) + "; a Facade that fills wears its "
				"frame whatever Layer it is being seen from");
		}
		{
			auto const pass = recordFacade(scene.front, atlas, kNoClip,
				LayerRenderStyle::Wireframe);
			require(!pass.hasFrame, "The wireframe overlay drew a Facade frame; "
			"the overlay contributes an outline only and no tile at all");
		}
	}

	// ------------------------------------------------------------------
	// The shipped Worlds (#1062).
	// ------------------------------------------------------------------
	//
	// The fixture cases above pin the rule; this one pins that the
	// bundled documents the defect was reported against no longer put the
	// band on screen. It sweeps every bundled World rather than only the
	// two named, because the thing worth keeping false is "some shipped
	// Door wears a frame its aperture does not fill", and an edit to any
	// one document can make that true.
	//
	// The positive control matters as much as the negative one here: a
	// sweep that found no frames at all - an atlas that installed none,
	// a draw that never reached a wall - would pass the scaled case for
	// the wrong reason. So the same sweep insists that the unscaled Doors
	// in the same documents do wear their frames.

	// The atlas the application actually installs, so a shipped World is
	// drawn with the artwork it ships with rather than with the synthetic
	// sheet the fixture cases use.
	SectorTileset installShippedSectorAtlas(smoke::Context const& context)
	{
		auto const atlas = SectorTileset::load(
			context.fixture("resources/textures/sectors-new.tileset.yaml"));
		require(atlas.family == SectorTilesetFamily::New,
			"The bundled sector tileset is not the New family the application "
			"installs, so the sweep below is not drawing shipped artwork");
		require(atlas.overlays.count("doorframe-room-1r-L0") > 0
				&& atlas.overlays.count("doorframe-corridor-1r-L0") > 0,
			"The bundled atlas carries no regular doorframes, so a sweep that "
			"finds no frame on a scaled Door would be finding nothing at all");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(hasSectorTileOverlay("doorframe-room-1r-L0"),
			"The bundled frames did not come up as installed overlays");
		return atlas;
	}

	// Every bundled World document, the same way the tall-leaf sweep finds
	// them: `fixture()` answers for files, so the walk starts from one and
	// goes out from its parent.
	std::vector<std::filesystem::path> shippedWorldDocuments(
		smoke::Context const& context)
	{
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

	// Whether this wall is one the shipped atlas authors frames for. A
	// Lift, a Chamber or a Background has no frame to wear, so its Doors
	// are not the control this sweep needs.
	bool isFramedWallKind(core::Sector const& sector)
	{
		auto const* own = sectorSurfaceKind(sector);
		if (!own) return false;
		std::string const kind{ own };
		return kind == "room" || kind == "corridor";
	}

	// The tiles one Sector's own surface pass drew, keyed by the
	// Sector-local cell each landed on. Narrower on purpose than the
	// recorder the fixture cases use: a shipped World carries furniture,
	// windows and object art this sweep installs no atlas for, and their
	// fallback fills are not Sector surfaces.
	std::map<std::pair<uint32_t, uint32_t>, std::string> surfaceCellsOf(
		std::shared_ptr<const core::Sector> sector, SectorTileset const& atlas)
	{
		WorldDrawList commands(kNoClip);
		renderSector(sector, sector->getLayerIndex(), LayerRenderStyle::Solid,
			false, kTint, &commands);

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle
					|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			// Anything under a clip other than the recorder's own is the
			// Sector behind seen through an aperture, not this wall's face.
			if (!near(triangle->clip.minimum.x, kNoClip.minimum.x)
					|| !near(triangle->clip.maximum.x, kNoClip.maximum.x)
					|| !near(triangle->clip.minimum.y, kNoClip.minimum.y)
					|| !near(triangle->clip.maximum.y, kNoClip.maximum.y))
				continue;
			quads.push_back(triangle);
		}
		require(quads.size() % 2 == 0, "The shipped Sector emitted an odd "
			"number of surface triangles, so they are not the pairs one quad "
			"makes");

		core::Vector2 low{}, high{};
		sector->getBounds(low, high);
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		float const left = std::min(a.x, b.x);
		float const floorLine = std::max(a.y, b.y);
		float const cellWidth = CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		std::map<std::pair<uint32_t, uint32_t>, std::string> cells;
		for (size_t pair = 0; pair + 1 < quads.size(); pair += 2)
		{
			float uvLeft = 1e9f, uvRight = -1e9f, uvTop = 1e9f, uvBottom = -1e9f;
			float cellLeft = 1e9f, cellBottom = -1e9f;
			for (auto const* triangle : { quads[pair], quads[pair + 1] })
			{
				for (auto const& uv : triangle->texcoords)
				{
					uvLeft = std::min(uvLeft, uv.x);
					uvRight = std::max(uvRight, uv.x);
					uvTop = std::min(uvTop, uv.y);
					uvBottom = std::max(uvBottom, uv.y);
				}
				for (auto const& point : triangle->positions)
				{
					cellLeft = std::min(cellLeft, point.x);
					cellBottom = std::max(cellBottom, point.y);
				}
			}
			auto const column = static_cast<uint32_t>(
				std::lround((cellLeft - left) / cellWidth));
			auto const level = static_cast<uint32_t>(
				std::lround((floorLine - cellBottom) / cellHeight));
			require(column < sector->getCellsWide()
					&& level < sector->getLevelsHigh(),
				"A shipped surface cell landed outside its Sector's own grid at "
				"column " + std::to_string(column) + ", level "
				+ std::to_string(level));
			auto const tile = tileAt((uvLeft + uvRight) * 0.5f,
				(uvTop + uvBottom) * 0.5f, atlas);
			require(!tile.empty(), "A shipped surface quad sampled UVs that fall "
				"on no tile in the bundled atlas");
			cells[std::make_pair(column, level)] = tile;
		}
		return cells;
	}

	struct SweptDoor
	{
		uint32_t firstColumn{};
		uint32_t level{};
		uint32_t cellsWide{};
		std::string scale;
	};

	void noShippedDoorWearsAFrameItsApertureDoesNotFill(
		smoke::Context const& context)
	{
		auto const atlas = installShippedSectorAtlas(context);
		pinViewport();
		// Wider than the fixture viewport, so no bundled World's cells are
		// culled out of the draw before the sweep can read them.
		gUISettings.worldViewportWidth = 8192.0f;
		gUISettings.worldViewportHeight = 8192.0f;

		auto const docs = shippedWorldDocuments(context);
		require(docs.size() >= 4, "The shipped World sweep found only "
			+ std::to_string(docs.size()) + " bundled World documents, so it "
			"cannot claim to cover the shipped Doors");

		uint32_t scaledDoors{ 0 }, framedDoors{ 0 };
		for (auto const& doc : docs)
		{
			std::shared_ptr<core::World> loaded;
			try
			{
				loaded.reset(core::loadWorldDocument(doc).release());
			}
			catch (std::exception const& error)
			{
				require(false, "Bundled World document " + doc.filename().string()
					+ " did not load: " + error.what());
			}
			require(bool(loaded), "Bundled World document " + doc.filename().string()
				+ " loaded nothing");
			std::string const document = doc.filename().string();
			RenderWorldScope scope(loaded);
			for (uint32_t layer = 0; layer < loaded->getLevelsHigh(); ++layer)
			{
				for (auto const& sector : loaded->getSectors(layer))
				{
					if (!sector) continue;
					// This wall's own front Doors, collected first so the
					// surface is drawn once per Sector rather than once per
					// Door standing in it.
					std::vector<SweptDoor> scaled, framed;
					for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
					{
						auto const object = std::dynamic_pointer_cast<
							const core::DoorSectorObject>(sector->getObject(i));
						if (!object) continue;
						auto const door = object->getDoor();
						if (!door || door->getFrontSector() != sector) continue;
						auto const cellsWide = door->getCellsWide();
						if (cellsWide == 0 || cellsWide > 2) continue;
						SweptDoor entry;
						entry.firstColumn = object->getCellX()
							- sector->getCellX0();
						entry.level = object->getCellY() - sector->getCellY0();
						entry.cellsWide = cellsWide;
						auto const scale = door->getHeightScale();
						if (scale && *scale != 1.0f)
						{
							entry.scale = std::to_string(*scale);
							scaled.push_back(entry);
						}
						else if (isFramedWallKind(*sector))
						{
							framed.push_back(entry);
						}
					}
					if (scaled.empty() && framed.empty()) continue;
					auto const cells = surfaceCellsOf(sector, atlas);
					for (auto const& door : scaled)
					{
						++scaledDoors;
						for (uint32_t offset = 0; offset < door.cellsWide; ++offset)
						{
							auto const found = cells.find(std::make_pair(
								door.firstColumn + offset, door.level));
							std::string const tile = found == cells.end()
								? std::string("<nothing>") : found->second;
							require(!isFrameTile(tile), document + ": the cell a "
								+ std::to_string(door.cellsWide) + "-wide Door with "
								"heightScale " + door.scale + " stands in drew "
								+ tile + "; that frame's hole is baked at the "
								"unscaled height, so the band between the top of "
								"the aperture and the top of the hole draws the "
								"viewport background through the wall");
						}
					}
					for (auto const& door : framed)
					{
						// The control: an unscaled Door in a Room or a
						// Corridor still wears its frame on every cell of its
						// opening, which is what makes the scaled result above
						// a fallback rather than a draw that never framed
						// anything at all.
						++framedDoors;
						for (uint32_t offset = 0; offset < door.cellsWide; ++offset)
						{
							auto const found = cells.find(std::make_pair(
								door.firstColumn + offset, door.level));
							std::string const tile = found == cells.end()
								? std::string("<nothing>") : found->second;
							require(isFrameTile(tile), document + ": the cell an "
								"unscaled " + std::to_string(door.cellsWide)
								+ "-wide Door stands in drew " + tile + "; the "
								"control for the scaled case is that unscaled "
								"Doors in the same documents still frame, and "
								"this one does not");
						}
					}
				}
			}
		}
		require(scaledDoors > 0, "The shipped World sweep found no Door with a "
			"height scale at all, so the scaled case above is untested against "
			"shipped data");
		require(framedDoors > 0, "The shipped World sweep found no framed "
			"unscaled Door at all, so its scaled result proves nothing about "
			"frames being available");
	}

	// The assert beside the frame `emplace` (#1070) rests on a placement
	// rule rather than on anything the frame map settles for itself, and an
	// assert is silent in a Release build. Sweep the bundled Worlds and
	// require the rule the assert names holds over the data that ships: no
	// two Doors with the same front Sector stand in the same cell.
	//
	// The key space here is the frame map's own - Sector-local column and
	// level - so a collision found here is exactly the collision the map
	// would refuse. Were one cell claimed twice, the map would keep whichever
	// Door the Sector's object list reached first and drop the other's frame,
	// and the wall would draw one opening where two were authored.
	//
	// The swept Door count is the control: a sweep over Worlds with no front
	// Doors in them would pass by finding nothing that could contest.
	void noShippedDoorsContestAFrameCell(smoke::Context const& context)
	{
		auto const docs = shippedWorldDocuments(context);
		require(docs.size() >= 4, "The bundled World sweep found only "
			+ std::to_string(docs.size()) + " World documents, so it cannot "
			"claim to cover the shipped Doors");

		uint32_t sweptDoors{ 0 };
		for (auto const& doc : docs)
		{
			std::shared_ptr<core::World> loaded;
			try
			{
				loaded.reset(core::loadWorldDocument(doc).release());
			}
			catch (std::exception const& error)
			{
				require(false, "Bundled World document " + doc.filename().string()
					+ " did not load: " + error.what());
			}
			require(bool(loaded), "Bundled World document " + doc.filename().string()
				+ " loaded nothing");
			std::string const document = doc.filename().string();
			for (uint32_t layer = 0; layer < loaded->getLevelsHigh(); ++layer)
			{
				for (auto const& sector : loaded->getSectors(layer))
				{
					if (!sector) continue;
					std::map<std::pair<std::int64_t, std::int64_t>, std::string>
						claimed;
					for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
					{
						auto const object = std::dynamic_pointer_cast<
							const core::DoorSectorObject>(sector->getObject(i));
						if (!object) continue;
						auto const door = object->getDoor();
						// Only a wall's own front Doors reach the frame map. The
						// Sector behind a threshold wears no frame, so its copy
						// of the same Door is not a claim on a cell there.
						if (!door || door->getFrontSector() != sector) continue;
						++sweptDoors;
						std::string const here = document + ", " + sector->getName()
							+ ", Door at " + std::to_string(object->getCellX())
							+ "," + std::to_string(object->getCellY());
						for (uint32_t offset = 0; offset < door->getCellsWide();
								++offset)
						{
							auto const key = std::make_pair(
								static_cast<std::int64_t>(object->getCellX()) + offset
									- static_cast<std::int64_t>(sector->getCellX0()),
								static_cast<std::int64_t>(object->getCellY())
									- static_cast<std::int64_t>(sector->getCellY0()));
							auto const found = claimed.emplace(key, here);
							require(found.second, here + " stands in a cell that "
								+ found.first->second + " already claims. Two Doors in "
								"one cell of one wall would leave the frame map keeping "
								"whichever it iterated first and dropping the other "
								"with no diagnostic (#1070)");
						}
					}
				}
			}
		}
		require(sweptDoors >= 8, "The bundled World sweep found only "
			+ std::to_string(sweptDoors) + " front Doors, so it proves nothing "
			"about shipped frames going uncontested");
	}



	// ------------------------------------------------------------------
	// What the derivation walks, and what it does not (#1071).
	// ------------------------------------------------------------------
	//
	// A frame is derived from what stands in the wall, so the only way to
	// know whether a Sector's cells wear one is to look. #1071 does not
	// stop that look; it stops taking it when nothing is going to be drawn.
	// The picture cannot tell the two apart - a Sector that looked and found
	// no Door and a Sector that never looked both come out blank - so these
	// checks read the derivation's own counters, which move only when the
	// walk happens.

	// A Sector with no objects has no Door standing in its wall, and the
	// walk that finds the Doors would visit nothing on the way to an empty
	// map. A Room with nothing placed in it is the common case a World is
	// mostly made of; a Background hosts no objects by domain definition
	// and is drawn flat besides. Neither is worth a walk every frame, and
	// the walk is worth nothing even once.
	void aSectorWithNoObjectsWalksNothingForFrames(smoke::Context const&)
	{
		installAtlas(true);
		pinViewport();
		auto world = std::make_shared<core::World>("No-object world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Quiet", 0, 0, 0, 4, 1);
		world->addBackground(1, 0, 0, 4, 1);
		world->finishBuild();
		world->pauseSimulation();

		std::shared_ptr<const core::Sector> room;
		std::shared_ptr<const core::Sector> ground;
		for (uint32_t index = 0; index < world->getNumSectors(); ++index) {
			auto const sector = world->getSector(index);
			if (!sector) continue;
			if (sector->getName() == "Quiet") room = sector;
			if (sector->getLayerIndex() == 1) ground = sector;
		}
		require(room != nullptr && ground != nullptr,
			"The no-object fixture lost a Sector");
		require(room->getNumObjects() == 0, "The fixture Room carries "
			+ std::to_string(room->getNumObjects()) + " objects, so it is not "
			"the no-object Sector this check is about");
		require(ground->getNumObjects() == 0, "The Background carries "
			+ std::to_string(ground->getNumObjects()) + " objects, which a "
			"Background does not host");

		// Three draws, because the claim is about per-frame work. One draw
		// that walks nothing is worth nothing if the next one walks.
		auto const before = doorframeDerivationStats();
		renderPass(room, 0, kNoClip);
		renderPass(room, 0, kNoClip);
		renderPass(ground, 1, kNoClip);
		auto const after = doorframeDerivationStats();
		require(after.sectorWalks == before.sectorWalks, "A Sector with no "
			"objects walked " + std::to_string(after.sectorWalks
				- before.sectorWalks)
			+ " Sector list(s) for frames across three draws; a Sector with "
			"nothing in it has no Door to find and should not be walked at all "
			"(#1071)");
		require(after.objectsExamined == before.objectsExamined, "A Sector "
			"with no objects examined " + std::to_string(after.objectsExamined
				- before.objectsExamined) + " object(s) for frames");
	}

	// The clip the Sector's own rect lies wholly to the right of.
	WorldDrawList::ClipRectangle offscreenOf(core::Sector const& sector)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		float const left = std::min(a.x, b.x);
		return WorldDrawList::ClipRectangle{ { left - 256.0f, -100000.0f },
			{ left - 1.0f, 100000.0f } };
	}

	// A Sector wholly outside the draw list's clip derives no frame map.
	// The cull that hides it lives inside the surface loop, so the pass
	// asks the loop's own predicate before it derives anything: a Sector
	// the loop would not hand a single cell to is a Sector whose Doors
	// nobody is looking at, and #1071 is the difference between deriving
	// a frame for it and not.
	void aSectorWhollyOutsideTheClipDerivesNoFrames(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		pinViewport();
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		require(doorObjectIndex(*scene.front) != ~0u,
			"The fixture front Sector carries no Door, so the control in "
			"everyDrawnPassDerivesItsFramesAnew would prove nothing");

		auto const clip = offscreenOf(*scene.front);
		auto const before = doorframeDerivationStats();
		auto const cells = recordFrontSurface(renderPass(scene.front,
			scene.front->getLayerIndex(), clip), atlas, clip);
		auto const after = doorframeDerivationStats();
		require(cells.empty(), "A Sector wholly outside the clip drew "
			+ std::to_string(cells.size()) + " surface cells, so the fixture "
			"is not off screen after all");
		require(after.sectorWalks == before.sectorWalks, "A Sector wholly "
			"outside the clip walked " + std::to_string(after.sectorWalks
				- before.sectorWalks)
			+ " Sector list(s) for frames; nothing there is going to be drawn, "
			"so nothing there should be derived (#1071)");
		require(after.objectsExamined == before.objectsExamined, "A Sector "
			"wholly outside the clip examined " + std::to_string(
				after.objectsExamined - before.objectsExamined)
			+ " object(s) for frames");
	}

	// The Facade frames itself through a separate derivation (#1059), so
	// it takes the same cull before it builds its own map for the same
	// reason. Its flat fill is emitted whether or not the clip reaches the
	// Facade; the frame over it is not worth deriving when nothing lands on it.
	void aFacadeWhollyOutsideTheClipDerivesNoFrames(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		pinViewport();
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
		require(doorObjectIndex(*scene.front) != ~0u,
			"The fixture Facade carries no Door, so the frame pass below has "
			"nothing to skip");

		auto const clip = offscreenOf(*scene.front);
		auto const before = doorframeDerivationStats();
		auto const pass = recordFacade(scene.front, atlas, clip);
		auto const after = doorframeDerivationStats();
		require(!pass.hasFrame, "A Facade wholly outside the clip drew a "
			"doorframe over its fill");
		require(after.sectorWalks == before.sectorWalks, "A Facade wholly "
			"outside the clip walked " + std::to_string(after.sectorWalks
				- before.sectorWalks)
			+ " Sector list(s) for frames; its frame pass should not run at "
			"all (#1071)");
	}

	// Nothing about a frame is cached between draws. The same Sector drawn
	// twice in a row derives the same amount of framing work both times,
	// which is the shape live derivation has: no memo, no revision, no
	// entry that can go stale when a Door is added or taken away. The
	// removal half of that promise is pinned by
	// `aVariantUnderADoorIsHiddenAndComesBack` and
	// `aRemovedDoorRevertsWithNoResidue`; this pins the half only the
	// counters can see (#1071).
	void everyDrawnPassDerivesItsFramesAnew(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		pinViewport();
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);

		auto const before = doorframeDerivationStats();
		renderPass(scene.front, scene.front->getLayerIndex(), kNoClip);
		auto const first = doorframeDerivationStats();
		renderPass(scene.front, scene.front->getLayerIndex(), kNoClip);
		auto const second = doorframeDerivationStats();

		auto const firstWalks = first.sectorWalks - before.sectorWalks;
		auto const secondWalks = second.sectorWalks - first.sectorWalks;
		require(firstWalks >= 1, "Drawing a Room with a Door standing in it "
			"walked no Sector list at all, so the counters #1071 reads are "
			"not counting the walk");
		require(secondWalks == firstWalks, "Two identical draws of one Sector "
			"walked " + std::to_string(firstWalks) + " and then "
			+ std::to_string(secondWalks) + " Sector lists; a frame that does "
			"not cost the same on both is not being derived live on both");
		require(first.objectsExamined - before.objectsExamined
				== second.objectsExamined - first.objectsExamined,
			"Two identical draws of one Sector examined different numbers of "
			"objects for frames, so the derivation is not the same live "
			"derivation on both");
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
	checks.push_back({ "doorframeCells/aScaledRegularDoorWearsNoFrame",
		isolated<aScaledRegularDoorWearsNoFrame> });
	checks.push_back({ "doorframeCells/aScaleOfOneStillWearsItsFrame",
		isolated<aScaleOfOneStillWearsItsFrame> });
	checks.push_back({
		"doorframeCells/aScaledDoorStillShowsTheVariantUnderIt",
		isolated<aScaledDoorStillShowsTheVariantUnderIt> });
	checks.push_back({ "doorframeCells/aScaledDoorInACorridorWearsNoFrame",
		isolated<aScaledDoorInACorridorWearsNoFrame> });
	checks.push_back({
		"doorframeCells/aTallDoorIsUnaffectedByTheScaleGuard",
		isolated<aTallDoorIsUnaffectedByTheScaleGuard> });
	checks.push_back({ "doorframeCells/oneWideFacadeDoorWearsItsFrame",
		isolated<oneWideFacadeDoorWearsItsFrame> });
	checks.push_back({ "doorframeCells/twoWideFacadeDoorWearsItsFramePair",
		isolated<twoWideFacadeDoorWearsItsFramePair> });
	checks.push_back({
		"doorframeCells/theFlatFillStillCoversEveryCellThatIsNotADoorCell",
		isolated<theFlatFillStillCoversEveryCellThatIsNotADoorCell> });
	checks.push_back({ "doorframeCells/theFrameIsEmittedBeforeEveryAperture",
		isolated<theFrameIsEmittedBeforeEveryAperture> });
	checks.push_back({
		"doorframeCells/theFrameNeverReachesAnotherThresholdsAperture",
		isolated<theFrameNeverReachesAnotherThresholdsAperture> });
	checks.push_back({ "doorframeCells/theTallFacadeFrameIsDerivedAndDrawn",
		isolated<theTallFacadeFrameIsDerivedAndDrawn> });
	checks.push_back({
		"doorframeCells/aFacadeWithNoFacadeFramesAuthoredDrawsItsFill",
		isolated<aFacadeWithNoFacadeFramesAuthoredDrawsItsFill> });
	checks.push_back({ "doorframeCells/theFrameFollowsTheFillThroughEveryStyle",
		isolated<theFrameFollowsTheFillThroughEveryStyle> });
	checks.push_back({
		"doorframeCells/aScaledFacadeDoorDrawsNoFrameOverItsFill",
		isolated<aScaledFacadeDoorDrawsNoFrameOverItsFill> });
	checks.push_back({
		"doorframeCells/noShippedDoorWearsAFrameItsApertureDoesNotFill",
		isolated<noShippedDoorWearsAFrameItsApertureDoesNotFill> });
	checks.push_back({
		"doorframeCells/noShippedDoorsContestAFrameCell",
		isolated<noShippedDoorsContestAFrameCell> });
	checks.push_back({
		"doorframeCells/aSectorWithNoObjectsWalksNothingForFrames",
		isolated<aSectorWithNoObjectsWalksNothingForFrames> });
	checks.push_back({
		"doorframeCells/aSectorWhollyOutsideTheClipDerivesNoFrames",
		isolated<aSectorWhollyOutsideTheClipDerivesNoFrames> });
	checks.push_back({
		"doorframeCells/aFacadeWhollyOutsideTheClipDerivesNoFrames",
		isolated<aFacadeWhollyOutsideTheClipDerivesNoFrames> });
	checks.push_back({
		"doorframeCells/everyDrawnPassDerivesItsFramesAnew",
		isolated<everyDrawnPassDerivesItsFramesAnew> });
}
