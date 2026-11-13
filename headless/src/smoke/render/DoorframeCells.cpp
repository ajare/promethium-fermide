// A Door keeps its wall's tile and lays its frame over the top (#1054, #1073).
//
// #1037 pinned the geometry of a Sector surface, #1039 the seam that asks
// which tile each of those cells wears, and #1040 what a recorded variant
// does at that seam. #1054 first put a doorframe at that seam by having a
// door cell answer the frame *instead of* its wall's tile. This file now
// pins the other shape of the same feature, and the shape the earlier one
// was always reaching for:
//
//   1. the wall's own tile is drawn across the Door's cells as it is
//      across every other cell of that wall;
//   2. the space behind is drawn clipped to the doorway aperture;
//   3. the leaves are drawn over it, under the same clip;
//   4. the frame is drawn last, over the wall and the leaf edges alike.
//
// What that ordering buys, and what these checks hold:
//
//   * the frame does not vary with its wall. The wall's look comes from
//     the wall's tile, so a frame laid over it has nothing left to say
//     about which wall it lies on, and six tiles - one per width, height
//     and role - frame a Room, a Corridor and a Facade alike. The fifteen
//     wall-keyed frames this replaced were three copies of artwork that
//     could not tell the copies apart;
//   * a frame whose hole does not match its opening is no longer a hole in
//     the wall. It used to be: the frame stood in for the wall, so a hole
//     taller than a scaled aperture drew the viewport background straight
//     through (#1062). Over the top, the mismatch costs a band of wall
//     inside the moulding, and the scale guard that feature needed is gone;
//   * the back Sector is clipped to the whole aperture rather than to the
//     strip the leaves have vacated, because the leaves cover what they
//     cover. Nothing has to compute how much of the doorway is open, and a
//     threshold nested behind this one is seen through the whole opening
//     rather than through whatever the parent's leaves happened to leave;
//   * a Door is still a reversible placement. Nothing is written over the
//     variant an author painted, because nothing outranks it any more -
//     the frame lands over both and takes nothing away.
//
// The atlas is a synthetic one with every tile in its own column, so a
// quad's tile is read back out of the UVs it sampled rather than taken on
// trust from a name the renderer kept to itself.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorDrawSheet.h"
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

	// The colour the pass is drawn with. It reaches the wireframe outline
	// and the fill emitted when no atlas is installed; it never reaches a
	// tile.
	pf::render::PackedColourValue const kTint = packColour(214, 96, 32, 255);
	// The colour every tile is drawn with: nothing is multiplied into the
	// artwork on the way to the screen.
	pf::render::PackedColourValue const kRaw = White;
	pf::render::PackedColourValue const kMagenta = packColour(255, 0, 255, 255);

	WorldDrawList::ClipRectangle const kNoClip{ { -100000.0f, -100000.0f },
		{ 100000.0f, 100000.0f } };

	// The six frames the shipped set authors. One per width, height and
	// role, and no wall in any of them: the frame lies over the wall's own
	// tile, so the wall is not a fact about the frame (#1073).
	std::vector<std::string> const kFrames{
		"doorframe-1r-L0", "doorframe-2r-L0", "doorframe-2r-R0",
		"doorframe-1t-L0", "doorframe-2t-L0", "doorframe-2t-R0" };

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
		for (auto const* name : { "room", "room-V1", "corridor", "facade",
				"background" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		if (withFrames)
			for (auto const& name : kFrames)
				atlas.overlays.emplace(name, SectorTileRegion{
					column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		atlas.width = column * kCellWidth;
		atlas.height = kCellHeight;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
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

	// A screen rect.
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

		bool sameAs(ScreenRect const& o) const
		{
			return near(left, o.left) && near(right, o.right)
				&& near(top, o.top) && near(bottom, o.bottom);
		}

		std::string str() const
		{
			return "(" + std::to_string(left) + "," + std::to_string(top) + ")-("
				+ std::to_string(right) + "," + std::to_string(bottom) + ")";
		}
	};

	// ------------------------------------------------------------------
	// What came out of the draw.
	// ------------------------------------------------------------------

	// One quad the pass emitted, with the one thing that decides which of
	// the four layers of a Door's draw it belongs to: which atlas it
	// sampled and whether it was drawn under a clip the Door pushed.
	struct DrawnQuad
	{
		size_t index{};
		std::string tile;   // empty for a quad filled with no texture at all
		WorldDrawList::Texture texture{};
		ScreenRect rect{};
		ScreenRect clip{};
		// The tint the quad was drawn with, which is what says whether a
		// frame came out in its wall's colour or somebody else's.
		pf::render::PackedColourValue colour{};
		// Drawn under the clip the recorder opened and nothing else - the
		// Sector's own pass, as opposed to a draw the Door clipped to the
		// doorway.
		bool ownClip{ true };
		// The sampled fraction of the tile the quad drew, so a crop shows
		// as a shortened v range rather than as a resized rectangle.
		float u0{}, u1{}, v0{}, v1{};
		bool textured{ true };
	};

	bool isFrameTile(std::string const& name)
	{
		return name.rfind("doorframe-", 0) == 0;
	}

	std::string tileList(std::vector<DrawnQuad> const& quads)
	{
		std::string out;
		for (auto const& q : quads)
		{
			if (!out.empty()) out += ", ";
			out += q.tile.empty() ? std::string("<flat>") : q.tile;
		}
		return out.empty() ? std::string("<nothing>") : out;
	}

	std::vector<DrawnQuad> byTile(std::vector<DrawnQuad> const& quads,
		std::string const& tile)
	{
		std::vector<DrawnQuad> found;
		for (auto const& q : quads)
			if (q.tile == tile) found.push_back(q);
		return found;
	}

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

	// The whole of one Sector pass, in the order it came out.
	//
	// A Door's draw is four layers deep and the order is the feature, so
	// the recorder keeps the stream rather than only the cells: the wall
	// tile, the space behind, the leaves, the frame. Which layer a quad
	// belongs to is read off the atlas it sampled and the clip it was
	// drawn under, which is exactly what separates them in production
	// without any cooperation from the draw.
	struct Pass
	{
		std::vector<DrawnQuad> quads;

		// The Sector's own surface: its tiles, under its own clip, and no
		// frame among them. A door cell is in here wearing its wall's tile.
		std::vector<DrawnQuad> wallCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas
						&& !isFrameTile(q.tile))
					out.push_back(q);
			return out;
		}

		// The frames: SectorAtlas tiles under the Sector's own clip, laid
		// over the wall rather than traded for it.
		std::vector<DrawnQuad> frameCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas
						&& isFrameTile(q.tile))
					out.push_back(q);
			return out;
		}

		// The space behind: the back Sector's own tiles, drawn under a
		// clip the Door pushed.
		std::vector<DrawnQuad> behindCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (!q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas)
					out.push_back(q);
			return out;
		}

		// The leaves: what the Door paints inside the doorway. A leaf is an
		// ObjectAtlas sprite; with no object atlas installed the draw falls
		// back to a plain-colour fill. Either way it is not a Sector tile,
		// and it is drawn under the doorway clip rather than the Sector's
		// own.
		std::vector<DrawnQuad> leaves() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (!q.ownClip && q.texture != WorldDrawList::Texture::SectorAtlas)
					out.push_back(q);
			return out;
		}

		// The Facade's flat fill: an untextured quad in the Facade's own
		// colour, under its own clip.
		std::vector<DrawnQuad> flatFill() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.ownClip && !q.textured) out.push_back(q);
			return out;
		}

		std::optional<size_t> firstIndexOf(std::vector<DrawnQuad> const& set) const
		{
			if (set.empty()) return std::nullopt;
			size_t first = set.front().index;
			for (auto const& q : set) first = std::min(first, q.index);
			return first;
		}

		std::optional<size_t> lastIndexOf(std::vector<DrawnQuad> const& set) const
		{
			if (set.empty()) return std::nullopt;
			size_t last = set.front().index;
			for (auto const& q : set) last = std::max(last, q.index);
			return last;
		}
	};

	Pass recordPass(std::shared_ptr<const core::Sector> sector,
		SectorTileset const& atlas,
		WorldDrawList::ClipRectangle const& clip = kNoClip,
		LayerRenderStyle const style = LayerRenderStyle::Solid)
	{
		auto const commands = renderPass(sector, sector->getLayerIndex(), clip,
			style);
		auto const underOuterClip = [&](WorldDrawList::ClipRectangle const& c)
		{
			return near(c.minimum.x, clip.minimum.x)
				&& near(c.minimum.y, clip.minimum.y)
				&& near(c.maximum.x, clip.maximum.x)
				&& near(c.maximum.y, clip.maximum.y);
		};

		std::vector<WorldDrawList::Triangle const*> triangles;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle) triangles.push_back(triangle);
		}
		require(triangles.size() % 2 == 0,
			"The Sector emitted an odd number of triangles, so they are not "
			"the pairs one quad makes");

		Pass pass;
		size_t quadIndex{};
		for (size_t pair = 0; pair < triangles.size(); pair += 2)
		{
			auto const& first = *triangles[pair];
			auto const& second = *triangles[pair + 1];
			require(first.colour == second.colour,
				"The two triangles of one quad were tinted differently");
			require(first.texture == second.texture,
				"The two triangles of one quad sampled different atlases");

			DrawnQuad q;
			q.index = quadIndex++;
			q.texture = first.texture;
			q.colour = first.colour;
			q.ownClip = underOuterClip(first.clip);
			// Open the sampled range before widening it. Left at zero, the
			// low end never moves and the centre read back out of it lands
			// on the wrong tile - which is how a frame gets reported as
			// whatever sits one column left of it.
			q.u0 = q.v0 = 1e9f;
			q.u1 = q.v1 = -1e9f;
			for (auto const* triangle : { &first, &second })
			{
				for (auto const& p : triangle->positions) q.rect.include(p);
				for (auto const& uv : triangle->texcoords)
				{
					q.u0 = std::min(q.u0, uv.x);
					q.u1 = std::max(q.u1, uv.x);
					q.v0 = std::min(q.v0, uv.y);
					q.v1 = std::max(q.v1, uv.y);
				}
			}
			q.clip.left = first.clip.minimum.x;
			q.clip.top = first.clip.minimum.y;
			q.clip.right = first.clip.maximum.x;
			q.clip.bottom = first.clip.maximum.y;

			if (first.texture == WorldDrawList::Texture::SectorAtlas)
			{
				q.tile = tileAt((q.u0 + q.u1) * 0.5f, (q.v0 + q.v1) * 0.5f,
					atlas);
				require(!q.tile.empty(),
					"A SectorAtlas quad sampled UVs that fall on no tile in the "
					"installed atlas");
			}
			q.textured = first.texture != WorldDrawList::Texture::None;
			pass.quads.push_back(q);
		}
		return pass;
	}

	// The recorded quads laid out in the Sector-local grid the surface loop
	// is asked in: column from the Sector's left edge, level from its floor
	// line.
	std::map<std::pair<uint32_t, uint32_t>, DrawnQuad> gridOf(
		std::vector<DrawnQuad> const& quads, core::Sector const& sector)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		float const left = std::min(a.x, b.x);
		float const floorLine = std::max(a.y, b.y);
		float const cellWidth = CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		float const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		std::map<std::pair<uint32_t, uint32_t>, DrawnQuad> grid;
		for (auto const& q : quads)
		{
			auto const column = static_cast<uint32_t>(
				std::lround((q.rect.left - left) / cellWidth));
			auto const level = static_cast<uint32_t>(
				std::lround((floorLine - q.rect.bottom) / cellHeight));
			require(grid.find({ column, level }) == grid.end(),
				"Two quads landed on column " + std::to_string(column)
				+ ", level " + std::to_string(level));
			grid.emplace(std::make_pair(column, level), q);
		}
		return grid;
	}

	std::string tileAt(std::map<std::pair<uint32_t, uint32_t>, DrawnQuad> const& grid,
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

	// The Door standing in a fixture Sector.
	std::shared_ptr<const core::Door> doorOf(core::Sector const& sector)
	{
		auto const index = doorObjectIndex(sector);
		require(index != ~0u, "The fixture Sector carries no Door to ask");
		return std::static_pointer_cast<const core::DoorSectorObject>(
			sector.getObject(index))->getDoor();
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

	// The Door's aperture in screen units, and the Sector-local columns the
	// Door stands in.
	struct DoorAperture
	{
		uint32_t firstColumn{};
		uint32_t cellsWide{};
		ScreenRect aperture{};
	};

	// Drives a Door to an exact open fraction through its own timing, so
	// the renderer reads a real device state rather than a remembered
	// number. A closed Door culls the space behind it - its leaves cover
	// the whole doorway, so drawing it would paint over itself - and every
	// check that wants to see that layer has to open the Door first.
	void driveTo(std::shared_ptr<core::Door> const& door, float fraction)
	{
		door->close();
		while (!door->isClosed()) door->update(door->getOpenCloseTime());
		if (fraction <= 0.0f) return;
		door->open();
		door->update(door->getOpenCloseTime() * fraction);
		require(door->getOpenPercentage() >= fraction,
			"The Door fixture could not be driven to " + std::to_string(fraction)
			+ " open, reached " + std::to_string(door->getOpenPercentage()));
	}

	std::shared_ptr<core::Door> doorOf(std::shared_ptr<const core::Sector> sector)
	{
		return std::const_pointer_cast<core::Door>(doorOf(*sector));
	}
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

	// The frame tile a Door of this width and height wears in this role.
	std::string expectedFrame(uint32_t cellsWide, core::Door::Height height,
		bool rightCell)
	{
		return doorframeTileName(cellsWide, height, rightCell);
	}

	// ------------------------------------------------------------------
	// The cases.
	// ------------------------------------------------------------------

	// The wall's own tile survives a Door. Every cell of a Room with a Door
	// in it draws the Room tile, the door cells included - which is the
	// whole of what changed, and the thing every check below reads back
	// against.
	void theDoorCellStillDrawsItsWallTile(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		require(wall.size() == 4, "A four-cell Room drew "
			+ std::to_string(wall.size()) + " wall cells; every cell of the "
			"wall, the door cell included, has to draw its own tile");
		for (uint32_t column = 0; column < 4; ++column)
			require(tileAt(wall, column, 0) == "room", "Column "
				+ std::to_string(column) + " drew " + tileAt(wall, column, 0)
				+ "; a Door keeps its wall's tile under it");
	}

	// The frame is laid over the cells the Door stands in: one per cell,
	// `L0` on the left cell and `R0` on the right, at the cell rects.
	void theFrameIsLaidOverTheDoorCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
			auto const pass = recordPass(scene.front, atlas);
			auto const frames = gridOf(pass.frameCells(), *scene.front);
			require(frames.size() == 1, "A 1-wide Door drew "
				+ std::to_string(frames.size()) + " frame cells");
			require(tileAt(frames, 2, 0) == "doorframe-1r-L0",
				"The door cell wore " + tileAt(frames, 2, 0));
		}
		{
			auto scene = roomScene(5, 2, core::Door::Height::Regular, 2);
			auto const pass = recordPass(scene.front, atlas);
			auto const frames = gridOf(pass.frameCells(), *scene.front);
			require(frames.size() == 2, "A 2-wide Door drew "
				+ std::to_string(frames.size()) + " frame cells");
			require(tileAt(frames, 2, 0) == "doorframe-2r-L0",
				"The opening's left cell wore " + tileAt(frames, 2, 0));
			require(tileAt(frames, 3, 0) == "doorframe-2r-R0",
				"The opening's right cell wore " + tileAt(frames, 3, 0));
		}
	}

	// A tall Door wears the tall frame for its width.
	void aTallDoorWearsItsTallFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = roomScene(4, 1, core::Door::Height::Tall, 1);
			auto const pass = recordPass(scene.front, atlas);
			require(tileAt(gridOf(pass.frameCells(), *scene.front), 1, 0)
					== "doorframe-1t-L0",
				"A 1-wide tall Door wore "
				+ tileAt(gridOf(pass.frameCells(), *scene.front), 1, 0));
		}
		{
			auto scene = roomScene(5, 2, core::Door::Height::Tall, 2);
			auto const pass = recordPass(scene.front, atlas);
			auto const frames = gridOf(pass.frameCells(), *scene.front);
			require(tileAt(frames, 2, 0) == "doorframe-2t-L0"
					&& tileAt(frames, 3, 0) == "doorframe-2t-R0",
				"A 2-wide tall Door wore " + tileList(pass.frameCells()));
		}
	}

	// The frame does not vary with its wall. The same opening in a Room, a
	// Corridor and a Facade draws the same tile names, because the wall's
	// look comes from the wall's tile and the frame only carries the
	// moulding. This is the check the tile reduction stands or falls on.
	void everyWallWearsTheSameFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		std::vector<std::string> drawn;
		drawn.push_back(tileList(recordPass(
			roomScene(4, 1, core::Door::Height::Regular, 2).front, atlas)
			.frameCells()));
		drawn.push_back(tileList(recordPass(
			corridorScene(4, 1, core::Door::Height::Regular, 2).front, atlas)
			.frameCells()));
		drawn.push_back(tileList(recordPass(
			facadeScene(4, 1, core::Door::Height::Regular, 2).front, atlas)
			.frameCells()));
		require(drawn[0] == drawn[1] && drawn[1] == drawn[2],
			"The same opening drew a different frame per wall: Room " + drawn[0]
				+ ", Corridor " + drawn[1] + ", Facade " + drawn[2]);
		require(drawn[0] == "doorframe-1r-L0",
			"A 1-wide regular opening drew " + drawn[0]);
	}

	// Nothing wall-keyed is drawn anywhere. The names the feature used to
	// author are refused here as well as in the tileset, so a frame cannot
	// come back wearing a wall it no longer needs.
	void noWallKeyedFrameIsDrawn(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		for (auto const* kind : { "room", "corridor", "facade" })
		{
			auto const scene = std::string{ kind } == "corridor"
				? corridorScene(5, 2, core::Door::Height::Regular, 2)
				: std::string{ kind } == "facade"
				? facadeScene(5, 2, core::Door::Height::Regular, 2)
				: roomScene(5, 2, core::Door::Height::Regular, 2);
			for (auto const& q : recordPass(scene.front, atlas).quads)
				require(q.tile.find(std::string("doorframe-") + kind + "-")
						== std::string::npos,
					"A " + std::string{ kind } + " drew the wall-keyed frame '"
						+ q.tile + "'");
		}
	}

	// The frame keeps the floor-anchored grid the wall tile beneath it is
	// drawn on: same cell rect, same crop, no stretch.
	void theFrameKeepsTheFloorAnchoredGrid(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(wall.count({ 2, 0 }) == 1 && frames.count({ 2, 0 }) == 1,
			"The door cell did not get both its wall tile and its frame");
		auto const& under = wall.at({ 2, 0 });
		auto const& over = frames.at({ 2, 0 });
		require(near(under.rect.left, over.rect.left)
				&& near(under.rect.right, over.rect.right)
				&& near(under.rect.top, over.rect.top)
				&& near(under.rect.bottom, over.rect.bottom),
			"The frame is not on the cell its wall tile is: wall "
				+ under.rect.str() + ", frame " + over.rect.str());
	}

	// The frame is drawn last: after the wall tile it lies over, after the
	// space behind, and after the leaves. That order is what makes the
	// moulding read as the edge of the opening whatever the leaves have
	// done underneath it.
	void theFrameIsDrawnLast(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		driveTo(doorOf(scene.front), 0.5f);
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = pass.wallCells();
		auto const frames = pass.frameCells();
		auto const behind = pass.behindCells();
		auto const leaves = pass.leaves();
		require(!frames.empty() && !wall.empty() && !behind.empty()
				&& !leaves.empty(),
			"The Door's draw was missing a layer: wall " + std::to_string(wall.size())
				+ ", behind " + std::to_string(behind.size()) + ", leaves "
				+ std::to_string(leaves.size()) + ", frames "
				+ std::to_string(frames.size()));
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(wall),
			"The frame was drawn before the wall tile it lies over");
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(behind),
			"The frame was drawn before the space behind");
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(leaves),
			"The frame was drawn before the leaves");
	}

	// The space behind is clipped to the whole aperture, not to the strip
	// the leaves have vacated. The leaves cover what they cover, so nothing
	// has to compute how much of the doorway is open - and a threshold
	// behind this one is seen through the whole opening rather than
	// through whatever the parent's leaves happened to leave.
	void theBackSectorIsClippedToTheWholeAperture(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		driveTo(doorOf(scene.front), 0.5f);
		auto const apertures = doorApertures(scene.front);
		require(apertures.size() == 1, "The fixture carries "
			+ std::to_string(apertures.size()) + " front Doors");
		auto const pass = recordPass(scene.front, atlas);
		auto const behind = pass.behindCells();
		require(!behind.empty(), "The Door drew no space behind it");
		for (auto const& q : behind)
			require(q.clip.sameAs(apertures[0].aperture),
				"The space behind was clipped to " + q.clip.str()
					+ ", not to the whole aperture "
					+ apertures[0].aperture.str());
	}

	// A frame the installed atlas does not carry is no frame at all: the
	// wall's own tile is already down, so there is nothing for a missing
	// frame to leave uncovered, and nothing draws in the missing-art colour.
	void aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta(smoke::Context const&)
	{
		auto atlas = installAtlas(false);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A frame drew from an atlas that "
			"authors none: " + tileList(pass.frameCells()));
		for (auto const& q : pass.quads)
			require(q.colour != kMagenta, "A cell drew the missing-art magenta "
				"for a frame that was never authored; a missing frame is no frame, "
				"not a hole");
		// The wall is still whole. Nothing was lost by the frame's absence.
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A missing frame took a wall cell with it");
	}

	// A variant an author painted on a door cell still draws. Nothing
	// outranks it any more - the frame lands over both - so the cell wears
	// what it was painted with and the frame lies over it.
	void aVariantUnderADoorStillDraws(smoke::Context const&)
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
		auto const pass = recordPass(scene.front, atlas);
		require(tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0) == "room-V1",
			"The painted variant did not draw under the Door; it drew "
			+ tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0));
		require(tileAt(gridOf(pass.frameCells(), *scene.front), 2, 0)
				== "doorframe-1r-L0",
			"The frame did not land over the painted variant");
	}

	// Taking the Door out takes its frame with it and leaves the wall
	// exactly as it was.
	void aRemovedDoorLeavesNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		require(!recordPass(scene.front, atlas).frameCells().empty(),
			"The fixture Door drew no frame to remove");
		require(scene.world->removeSectorDoor(scene.frontIndex,
				doorObjectIndex(*scene.front)),
			"The fixture Door could not be removed");
		scene.refresh();
		require(doorObjectIndex(*scene.front) == ~0u,
			"The front Location still carries a Door object after the removal");
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A removed Door left a frame: "
			+ tileList(pass.frameCells()));
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A removed Door did not leave the wall whole");
	}

	// Only the front wall of a threshold is framed. The Sector behind is
	// seen through the opening rather than framing it, so it keeps its own
	// tile and wears no moulding.
	void theBackWallOfAThresholdTakesNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		std::shared_ptr<const core::Sector> back;
		for (uint32_t index = 0; index < scene.world->getNumSectors(); ++index)
		{
			auto const candidate = scene.world->getSector(index);
			if (candidate && candidate->getLayerIndex() == 1) back = candidate;
		}
		require(back != nullptr, "The fixture has no Sector behind the Door");
		// Drawn on its own, the back wall is just a Room: no frame, because
		// it frames nothing.
		auto const pass = recordPass(back, atlas);
		require(pass.frameCells().empty(), "The Sector behind a threshold wore "
			"a frame: " + tileList(pass.frameCells()));
	}

	// Two doors side by side each frame their own cells and neither reaches
	// into the other's opening.
	void adjacentDoorsFrameTheirOwnCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto world = std::make_shared<core::World>("Two door world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 0, 0, 6, 1);
		world->addRoom("Back", 1, 0, 0, 6, 1);
		core::World::CreateDoorOptions options;
		options.width = 1u;
		options.height = core::Door::Height::Regular;
		world->addSectorDoor(0, 0, 2, options);
		world->addSectorDoor(0, 0, 3, options);
		options.width = 2u;
		world->addSectorDoor(0, 0, 4, options);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.frontLayer = 0;
		scene.refresh();
		require(scene.front->getCellsWide() == 6,
			"The two-door fixture lost its front Room");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(tileAt(frames, 0, 0) == "<nothing>"
				&& tileAt(frames, 1, 0) == "<nothing>",
			"Cells with no Door in them wore a frame");
		require(tileAt(frames, 2, 0) == "doorframe-1r-L0"
				&& tileAt(frames, 3, 0) == "doorframe-1r-L0",
			"Two adjacent 1-wide Doors did not each wear a left frame: "
				+ tileList(pass.frameCells()));
		require(tileAt(frames, 4, 0) == "doorframe-2r-L0"
				&& tileAt(frames, 5, 0) == "doorframe-2r-R0",
			"The 2-wide Door did not frame its own two cells: "
				+ tileList(pass.frameCells()));
		// And every frame sits inside the opening it belongs to.
		for (auto const& frame : pass.frameCells())
		{
			bool inside = false;
			for (auto const& aperture : doorApertures(scene.front))
				if (frame.rect.overlaps(aperture.aperture)) inside = true;
			require(inside, "A frame at " + frame.rect.str()
				+ " lies outside every Door's aperture on this wall");
		}
	}

	// A scaled Door wears its frame now. The frame lies over the wall's
	// own tile, so the band between the top of a scaled aperture and the
	// top of the frame's hole draws the wall rather than the viewport
	// background straight through - which is the whole of why the scale
	// guard #1062 needed is gone.
	void aScaledDoorWearsItsFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2, 0,
			CORE_ROOM_MAX_HEIGHT, 1, 0.5f);
		auto const pass = recordPass(scene.front, atlas);
		require(tileAt(gridOf(pass.frameCells(), *scene.front), 2, 0)
				== "doorframe-1r-L0",
			"A scaled Door wore no frame; over the wall's own tile there is "
			"nothing a mismatched hole can uncover, so it wears one");
		require(tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0) == "room",
			"A scaled Door's cell drew "
			+ tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0)
			+ "; the wall under it has to be there for the band above the "
			"aperture to be wall and not a hole");
	}

	// The Facade's own surface still covers every cell, and the frame
	// lands over it. A Facade draws a tile of its family rather than a
	// colour the renderer packed from its authored triple, so the rule
	// ADR 0003 states - that nothing in the threshold path paints over an
	// aperture - is held by the same per-cell layout every wall uses, and
	// the frame narrows nothing.
	void theFacadeSurfaceStillCoversEveryCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
		auto const pass = recordPass(scene.front, atlas);
		auto const surface = pass.wallCells();
		require(surface.size() == 4, "The Facade's surface came out as "
			+ std::to_string(surface.size())
			+ " tiles for a Facade four cells wide");
		for (auto const& q : surface)
			require(q.tile == "facade", "The Facade drew '" + q.tile
				+ "'; its surface is its own tile, not a borrowed wall's");
		require(tileAt(gridOf(pass.frameCells(), *scene.front), 2, 0)
				== "doorframe-1r-L0",
			"The Facade's door cell wore no frame over its own tile");
	}

	// The frame is drawn as its own artwork. It carries no colour of its
	// own and none is multiplied into it, whether the wall it moulds is a
	// Facade or a Room: what the moulding looks like is settled by the
	// frame's own tiles and by nothing else.
	void theFrameIsDrawnRaw(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = facadeScene(4, 1, core::Door::Height::Regular, 2);
			auto const pass = recordPass(scene.front, atlas);
			require(!pass.frameCells().empty(), "The Facade drew no frame");
			for (auto const& q : pass.frameCells())
				require(q.colour == kRaw,
					"A Facade frame was drawn with a colour other than its raw "
					"artwork; the moulding is its own picture and takes no "
					"colour from the wall it lies over");
		}
		{
			// A Room frame, same rule: the frame's artwork is the picture,
			// and the pass that drew it contributes no colour to it.
			auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
			auto const pass = recordPass(scene.front, atlas);
			require(!pass.frameCells().empty(), "The Room drew no frame");
			for (auto const& q : pass.frameCells())
				require(q.colour == kRaw,
					"A Room frame was drawn with a colour other than its raw "
					"artwork");
		}
	}

	// A wall with no Door in it draws no frame at all - and, the other half
	// of the same rule, draws its own tile on every cell as it always did.
	void aWallWithNoDoorDrawsNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		// A Room built with no Door in it at all, rather than one with the
		// Door taken back out: the rule is about there being no Door, not
		// about the removal.
		auto world = std::make_shared<core::World>("Door-less world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 0, 0, 4, 1);
		world->addRoom("Back", 1, 0, 0, 4, 1);
		world->finishBuild();
		world->pauseSimulation();
		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.frontLayer = 0;
		scene.refresh();
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A door-less wall drew a frame: "
			+ tileList(pass.frameCells()));
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A door-less wall did not draw all four of its cells");
	}

	// The frame follows the wall through the styles the viewport draws it
	// with. A Solid pass frames; a wireframe pass contributes outlines and
	// no moulding, because a wireframe Layer never paints filled geometry
	// over the selection.
	void theFrameFollowsTheWallThroughEveryStyle(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 1, core::Door::Height::Regular, 2);
		{
			auto const pass = recordPass(scene.front, atlas, kNoClip,
				LayerRenderStyle::Solid);
			require(!pass.frameCells().empty(), "The Solid pass drew no frame");
		}
		{
			auto const pass = recordPass(scene.front, atlas, kNoClip,
				LayerRenderStyle::Wireframe);
			require(pass.frameCells().empty(), "The wireframe pass painted "
				"filled frame geometry over the selection: "
				+ tileList(pass.frameCells()));
		}
	}

	// The frame is derived from the Door that owns it, not from a walk of
	// the Sector's object list. A Sector with no Doors therefore walks
	// nothing looking for one, and a Sector with Doors asks each Door for
	// its own cells. The check that used to sit here counted a per-Sector
	// frame map that no longer exists; what is worth pinning now is that
	// the frame arrives with the Door and goes with it.
	void theFrameArrivesWithItsDoorAndGoesWithIt(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(6, 1, core::Door::Height::Regular, 1);
		require(recordPass(scene.front, atlas).frameCells().size() == 1,
			"The fixture Door drew no frame");
		require(scene.world->removeSectorDoor(scene.frontIndex,
				doorObjectIndex(*scene.front)),
			"The fixture Door could not be removed");
		scene.refresh();
		require(recordPass(scene.front, atlas).frameCells().empty(),
			"Removing the Door left its frame behind");
	}

	// The name the render path asks for is the name the atlas carries, for
	// every width, height and role. Stated as a function of the three
	// facts so a name can be pinned without a live Door to derive it -
	// which is what lets the tall frames be checked even where a tall Door
	// would be refused.
	void theFrameNameIsTheOpeningAndNothingElse(smoke::Context const&)
	{
		installAtlas(true);
		require(doorframeTileName(1, core::Door::Height::Regular, false)
				== "doorframe-1r-L0", "1-wide regular left");
		require(doorframeTileName(2, core::Door::Height::Regular, false)
				== "doorframe-2r-L0", "2-wide regular left");
		require(doorframeTileName(2, core::Door::Height::Regular, true)
				== "doorframe-2r-R0", "2-wide regular right");
		require(doorframeTileName(1, core::Door::Height::Tall, false)
				== "doorframe-1t-L0", "1-wide tall left");
		require(doorframeTileName(2, core::Door::Height::Tall, false)
				== "doorframe-2t-L0", "2-wide tall left");
		require(doorframeTileName(2, core::Door::Height::Tall, true)
				== "doorframe-2t-R0", "2-wide tall right");
		for (auto const& name : kFrames)
			require(hasSectorTileOverlay(drawingSectorSheet(), name), name
				+ " is named by the render path but the installed atlas does "
				"not carry it");
		require(!hasSectorTileSurface(drawingSectorSheet(), "doorframe-1r-L0"),
			"A doorframe answered the Sector-surface query; it is a tile of "
			"the atlas but not a surface a Sector is authored with");
	}
}

void render_smoke::registerDoorframeCells(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorframeCells/theDoorCellStillDrawsItsWallTile",
		isolated<theDoorCellStillDrawsItsWallTile> });
	checks.push_back({ "doorframeCells/theFrameIsLaidOverTheDoorCells",
		isolated<theFrameIsLaidOverTheDoorCells> });
	checks.push_back({ "doorframeCells/aTallDoorWearsItsTallFrame",
		isolated<aTallDoorWearsItsTallFrame> });
	checks.push_back({ "doorframeCells/everyWallWearsTheSameFrame",
		isolated<everyWallWearsTheSameFrame> });
	checks.push_back({ "doorframeCells/noWallKeyedFrameIsDrawn",
		isolated<noWallKeyedFrameIsDrawn> });
	checks.push_back({ "doorframeCells/theFrameKeepsTheFloorAnchoredGrid",
		isolated<theFrameKeepsTheFloorAnchoredGrid> });
	checks.push_back({ "doorframeCells/theFrameIsDrawnLast",
		isolated<theFrameIsDrawnLast> });
	checks.push_back({ "doorframeCells/theBackSectorIsClippedToTheWholeAperture",
		isolated<theBackSectorIsClippedToTheWholeAperture> });
	checks.push_back({ "doorframeCells/aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta",
		isolated<aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta> });
	checks.push_back({ "doorframeCells/aVariantUnderADoorStillDraws",
		isolated<aVariantUnderADoorStillDraws> });
	checks.push_back({ "doorframeCells/aRemovedDoorLeavesNoFrame",
		isolated<aRemovedDoorLeavesNoFrame> });
	checks.push_back({ "doorframeCells/theBackWallOfAThresholdTakesNoFrame",
		isolated<theBackWallOfAThresholdTakesNoFrame> });
	checks.push_back({ "doorframeCells/adjacentDoorsFrameTheirOwnCells",
		isolated<adjacentDoorsFrameTheirOwnCells> });
	checks.push_back({ "doorframeCells/aScaledDoorWearsItsFrame",
		isolated<aScaledDoorWearsItsFrame> });
	checks.push_back({ "doorframeCells/theFacadeSurfaceStillCoversEveryCell",
		isolated<theFacadeSurfaceStillCoversEveryCell> });
	checks.push_back({ "doorframeCells/theFrameIsDrawnRaw",
		isolated<theFrameIsDrawnRaw> });
	checks.push_back({ "doorframeCells/aWallWithNoDoorDrawsNoFrame",
		isolated<aWallWithNoDoorDrawsNoFrame> });
	checks.push_back({ "doorframeCells/theFrameFollowsTheWallThroughEveryStyle",
		isolated<theFrameFollowsTheWallThroughEveryStyle> });
	checks.push_back({ "doorframeCells/theFrameArrivesWithItsDoorAndGoesWithIt",
		isolated<theFrameArrivesWithItsDoorAndGoesWithIt> });
	checks.push_back({ "doorframeCells/theFrameNameIsTheOpeningAndNothingElse",
		isolated<theFrameNameIsTheOpeningAndNothingElse> });
}
