// A 1x1 Window wears a windowframe over its wall (#1075).
//
// The tracer bullet for the windowframe series. Everything the wide, tall
// and corner tickets hang off is pinned here, in the smallest complete
// shape the feature can take: the atlas carries one frame, the renderer
// derives it from the live Window rather than from anything painted, and
// it lands over the wall's own tile without taking anything away.
//
// The four layers a Window puts down, in order:
//
//   1. the wall's own tile, drawn by the Sector surface pass across the
//      Window's cells as it is across every other cell of that wall;
//   2. the space behind, drawn under the aperture clip the Window pushes;
//   3. the glass, drawn over the space behind;
//   4. the frame, last, over the wall's tile and the glass edge alike.
//
// What that ordering buys, and what these checks hold:
//
//   * the frame does not vary with its wall. The wall's look comes from
//     the wall's tile, so a frame laid over it has nothing left to say
//     about which wall it lies on, and one tile frames a Room, a Corridor
//     and a Facade alike;
//   * nothing is consumed. A variant an author painted on the Window's
//     cell keeps drawing underneath, because nothing outranks it - the
//     frame lands over both and takes nothing away;
//   * a frame the atlas does not carry is no frame at all, which is what
//     lets the role grid be filled one tile at a time without any World
//     drawing a hole where a frame has not been drawn yet;
//   * a BoothWindow wears none, because it *is* a 1x1 Window and already
//     owns the frame its shutter's clip is cut from.
//
// The atlas is a synthetic one with every tile in its own column, so a
// quad's tile is read back out of the UVs it sampled rather than taken on
// trust from a name the renderer kept to itself.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "core/Background.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorSurfaceVariant.h"
#include "core/Window.h"
#include "core/WindowSectorObject.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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

	// The one frame the shipped set authors so far. The role grid it sits
	// in has fifteen cells; this is the single-column, single-level one,
	// and the rest arrive with the wide, tall and corner work.
	std::vector<std::string> const kWindowframes{ "windowframe-SS" };

	// The Facade's own colour, distinct from the tint every other pass in
	// this file is drawn with, so "the frame lies over the Facade's own
	// tile" cannot be confused with the caller's Layer tint.
	core::BackgroundColour const kFacadeColour{ 210, 66, 138 };

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
	// that reads a Window's glass rect cannot disagree about where a World
	// point lands.
	pf::render::Vec2 toScreen(core::Vector2 const& point)
	{
		return pf::render::Vec2{
			point.x * CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom
				+ gUISettings.worldViewportX + gUISettings.xOffset,
			gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- point.y * CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom
				- gUISettings.yOffset };
	}

	// ------------------------------------------------------------------
	// The atlases.
	// ------------------------------------------------------------------

	// One full-cell tile per name, each in its own column of a one-row
	// sheet, so no two tiles share a texel column and a quad's tile is
	// readable back out of the UVs it sampled.
	//
	// `withFrames` off is the atlas a World drawn before windowframes
	// existed would have: the same surfaces, no frame anywhere.
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
			for (auto const& name : kWindowframes)
				atlas.overlays.emplace(name, SectorTileRegion{
					column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		atlas.width = column * kCellWidth;
		atlas.height = kCellHeight;
		pinViewport();
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The synthetic windowframe atlas did not come up as the New family");
		return atlas;
	}

	// The Object-side art, each sprite in its own column of its own sheet,
	// so an Object quad is distinguishable from a frame quad by which atlas
	// it sampled and the frame's turn in the order is checkable against it.
	//
	// The BoothWindow's own frame and shutter are in here for the same
	// reason the glass is: the check that refuses a windowframe over a
	// hatch has to be able to see that the hatch itself drew, rather than
	// reading an empty pass as a successful exclusion.
	void installObjectArt()
	{
		ObjectTileset art;
		art.width = kCellWidth * 5;
		art.height = kCellHeight;
		int column = 0;
		for (auto const* name : { "window-clear", "window-frosted",
				"window-tinted", "booth-window-shutter", "booth-window-open" })
			art.sprites.emplace(name, TileArtwork{
				{ column++ * kCellWidth, 0, kCellWidth, kCellHeight },
				{ 0.0f, 0.0f }, { 1.0f, 1.0f } });
		setObjectTileset(std::move(art), static_cast<TextureId>(2));
		require(hasObjectTileset(),
			"The synthetic Object atlas did not install, so the glass layer "
			"the frame is supposed to draw over would not exist to check");
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

	// One quad the pass emitted, with the two things that decide which of
	// the four layers of a Window's draw it belongs to: which atlas it
	// sampled, and whether it was drawn under a clip the Window pushed.
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
		// Sector's own pass, as opposed to a draw the Window clipped to
		// its aperture.
		bool ownClip{ true };
		// The sampled fraction of the tile the quad drew, so a crop shows
		// as a shortened v range rather than as a resized rectangle.
		float u0{}, u1{}, v0{}, v1{};
		bool textured{ true };
	};

	bool isWindowframeTile(std::string const& name)
	{
		return name.rfind("windowframe-", 0) == 0;
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
	// A Window's draw is four layers deep and the order is the feature, so
	// the recorder keeps the stream rather than only the cells: the wall
	// tile, the space behind, the glass, the frame. Which layer a quad
	// belongs to is read off the atlas it sampled and the clip it was
	// drawn under, which is exactly what separates them in production
	// without any cooperation from the draw.
	struct Pass
	{
		std::vector<DrawnQuad> quads;

		// The Sector's own surface: its tiles, under its own clip, and no
		// frame among them. A window cell is in here wearing its wall's
		// tile.
		std::vector<DrawnQuad> wallCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas
						&& !isWindowframeTile(q.tile))
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
						&& isWindowframeTile(q.tile))
					out.push_back(q);
			return out;
		}

		// The space behind: the back Sector's own tiles, drawn under a
		// clip the Window pushed.
		std::vector<DrawnQuad> behindCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (!q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas)
					out.push_back(q);
			return out;
		}

		// The glass: an ObjectAtlas sprite drawn over the space behind.
		// It is not a Sector tile, and it is drawn inside the aperture
		// rather than under the Sector's own clip.
		std::vector<DrawnQuad> glass() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.texture == WorldDrawList::Texture::ObjectAtlas)
					out.push_back(q);
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
			else if (first.texture == WorldDrawList::Texture::ObjectAtlas)
			{
				// The glass is the only ObjectAtlas thing this fixture
				// draws, and its name is what the pass asked for; the
				// check that cares about the order only needs to know the
				// quad was glass rather than a Sector tile.
				q.tile = "glass";
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

	// Whether a Sector object is one of the two Window kinds. The frame
	// derivation is gated on the ordinary kind only, so the fixture has to
	// tell them apart rather than treat both as "a Window".
	bool isWindowObject(core::SectorObject const& object)
	{
		auto const type = object.getObjectType();
		return type == core::SectorObjectType::Window
			|| type == core::SectorObjectType::BoothWindow;
	}

	std::shared_ptr<const core::Window> windowOf(
		core::Sector const& sector, uint32_t ordinal = 0)
	{
		uint32_t seen{};
		for (uint32_t index = 0; index < sector.getNumObjects(); ++index)
		{
			auto const object = sector.getObject(index);
			if (!object || !isWindowObject(*object)) continue;
			if (seen++ != ordinal) continue;
			return std::static_pointer_cast<const core::WindowSectorObject>(
				object)->getWindow();
		}
		return nullptr;
	}

	// One fixture World and its front Location.
	//
	// `refresh` exists because a Window removal rebuilds the World from its
	// construction records, which replaces every Sector object. A held
	// pointer through that edit keeps drawing the World that used to be,
	// and a check that "the Window is gone" against it would pass while the
	// live World still had one.
	struct Scene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> front;
		std::shared_ptr<const core::Window> window;
		std::string frontName;
		uint32_t frontLayer{};
		uint32_t frontIndex{};

		// Found by name and Layer rather than by index, because a rebuild
		// re-derives the Sector list from the construction records and an
		// index remembered across one can point anywhere. Name alone is not
		// enough either: a Room and the Room behind it share the name their
		// builder gave them, and the pair is exactly what this fixture has.
		void refresh()
		{
			front.reset();
			window.reset();
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
			window = windowOf(*front);
		}
	};

	// The front wall of the one Window in a fixture. Asked of the World
	// rather than guessed from the fixture's shape, because "which of these
	// two Locations is the front" is the Window's fact to state and the
	// check that guesses it is the check that can be wrong.
	std::shared_ptr<const core::Sector> windowFrontSector(core::World const& world)
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
				if (!candidate || !isWindowObject(*candidate)) continue;
				auto const window = windowOf(*sector);
				// A Window is registered on both Sectors of its pair; only
				// the front one counts here.
				if (window && window->getFrontSector() == sector)
				{
					found = sector;
					++count;
				}
			}
		}
		require(count == 1, "The fixture carries " + std::to_string(count)
			+ " front Window walls; the check needs exactly one");
		return found;
	}

	uint32_t windowObjectIndex(core::Sector const& sector, uint32_t ordinal = 0)
	{
		uint32_t seen{};
		for (uint32_t index = 0; index < sector.getNumObjects(); ++index)
		{
			auto const object = sector.getObject(index);
			if (!object || !isWindowObject(*object)) continue;
			if (seen++ != ordinal) continue;
			return index;
		}
		return ~0u;
	}

	// One Location on the front Layer with the same Location behind it, and
	// one Window set into the front wall.
	//
	// The back Location is not decoration: a Window joins two Layers, and
	// the space behind is what the frame's hole shows. It is also the case
	// the frame rule has to refuse - the back wall is seen through the
	// opening rather than framing it - so the fixture has to carry one to
	// prove it stays unfurnished.
	Scene buildScene(std::string const& kind, uint32_t cellsWide,
		uint32_t levelsHigh, uint32_t windowX, uint32_t windowLevel,
		float frontTopLevelHeight, bool booth)
	{
		auto world = std::make_shared<core::World>("Windowframe world", 16, 6);
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
		if (booth) world->addBoothWindow(0, windowLevel, windowX);
		else world->addSectorWindow(0, windowLevel, windowX, 1, 1);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		// The Window knows which wall it was authored into; ask it rather
		// than guessing from the fixture's shape.
		auto const front = windowFrontSector(*world);
		scene.frontName = front->getName();
		scene.frontLayer = front->getLayerIndex();
		scene.refresh();
		require(scene.front == front,
			"The front Location resolved twice to different Sectors");
		require(scene.front->getCellsWide() == cellsWide,
			"The front Location is not the width it was authored as");
		require(windowObjectIndex(*scene.front) != ~0u,
			"The front Sector carries no Window object");
		require(scene.window != nullptr,
			"The front Sector's Window could not be read back");
		return scene;
	}

	Scene roomScene(uint32_t cellsWide = 4, uint32_t windowX = 2,
		uint32_t windowLevel = 0, float topLevelHeight = CORE_ROOM_MAX_HEIGHT)
	{
		return buildScene("room", cellsWide, 1, windowX, windowLevel,
			topLevelHeight, false);
	}

	Scene corridorScene(uint32_t cellsWide = 4, uint32_t windowX = 2)
	{
		// A Corridor is one level high and takes its own height, not the
		// Room's: the top-level height a Room author sets means nothing to
		// it, and the loader would say so.
		return buildScene("corridor", cellsWide, 1, windowX, 0,
			CORE_CORRIDOR_HEIGHT, false);
	}

	// One Facade on the front Layer, one Room behind it, one Window set
	// into the Facade.
	Scene facadeScene(uint32_t cellsWide = 4, uint32_t windowX = 2)
	{
		return buildScene("facade", cellsWide, 1, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false);
	}

	Scene boothScene(uint32_t cellsWide = 4, uint32_t windowX = 2)
	{
		return buildScene("room", cellsWide, 1, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, true);
	}

	// The glass rect of the one Window a fixture carries, in screen units.
	ScreenRect glassRect(core::Window const& window)
	{
		core::Vector2 lo{}, hi{};
		window.getFullShape(lo, hi);
		ScreenRect rect;
		rect.include(toScreen(lo));
		rect.include(toScreen(hi));
		return rect;
	}

	// ------------------------------------------------------------------
	// The cases.
	// ------------------------------------------------------------------

	// The wall's own tile survives a Window. Every cell of a Room with a
	// Window in it draws the Room tile, the window cell included - which is
	// the whole of what changed, and the thing every check below reads back
	// against.
	void theWindowCellStillDrawsItsWallTile(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		require(wall.size() == 4, "A four-cell Room drew "
			+ std::to_string(wall.size()) + " wall cells; every cell of the "
			"wall, the window cell included, has to draw its own tile");
		for (uint32_t column = 0; column < 4; ++column)
			require(tileAt(wall, column, 0) == "room", "Column "
				+ std::to_string(column) + " drew " + tileAt(wall, column, 0)
				+ "; a Window keeps its wall's tile under it");
	}

	// The frame is laid over the cell the Window stands in, at the cell
	// rect, with the role the cell's place in the span demands.
	void theFrameIsLaidOverTheWindowCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 1, "A 1x1 Window drew "
			+ std::to_string(frames.size()) + " frame cells");
		require(tileAt(frames, 2, 0) == "windowframe-SS",
			"The window cell wore " + tileAt(frames, 2, 0)
				+ "; a 1x1 Window's single cell is single in both axes and "
				"wears the block's SS tile");
		require(tileAt(frames, 1, 0) == "<nothing>"
				&& tileAt(frames, 3, 0) == "<nothing>",
			"A cell with no Window in it wore a frame: "
				+ tileList(pass.frameCells()));
	}

	// The frame is drawn last: after the wall tile it lies over, after the
	// space behind, and after the glass. That order is what makes the
	// moulding read as the edge of the opening whatever the glass
	// underneath it is doing.
	void theFrameIsDrawnLast(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = pass.wallCells();
		auto const frames = pass.frameCells();
		auto const behind = pass.behindCells();
		auto const glass = pass.glass();
		require(!frames.empty() && !wall.empty() && !behind.empty()
				&& !glass.empty(),
			"The Window's draw was missing a layer: wall "
				+ std::to_string(wall.size()) + ", behind "
				+ std::to_string(behind.size()) + ", glass "
				+ std::to_string(glass.size()) + ", frames "
				+ std::to_string(frames.size()));
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(wall),
			"The frame was drawn before the wall tile it lies over");
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(behind),
			"The frame was drawn before the space behind");
		require(*pass.firstIndexOf(frames) > *pass.lastIndexOf(glass),
			"The frame was drawn before the glass it covers the seam of");
	}

	// The frame keeps the floor-anchored grid the wall tile beneath it is
	// drawn on: same cell rect, same crop, no stretch.
	void theFrameKeepsTheFloorAnchoredGrid(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(wall.count({ 2, 0 }) == 1 && frames.count({ 2, 0 }) == 1,
			"The window cell did not get both its wall tile and its frame");
		auto const& under = wall.at({ 2, 0 });
		auto const& over = frames.at({ 2, 0 });
		require(near(under.rect.left, over.rect.left)
				&& near(under.rect.right, over.rect.right)
				&& near(under.rect.top, over.rect.top)
				&& near(under.rect.bottom, over.rect.bottom),
			"The frame is not on the cell its wall tile is: wall "
				+ under.rect.str() + ", frame " + over.rect.str());
	}

	// The frame stays pinned to the wall as the World canvas scrolls. The
	// frame and the wall tile it lies on move together, so a scroll cannot
	// detach the moulding from the thing it is moulding.
	void theFrameStaysPinnedToTheWallAsTheCanvasScrolls(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		auto const still = recordPass(scene.front, atlas);
		auto const stillWall = gridOf(still.wallCells(), *scene.front).at({ 2, 0 });
		auto const stillFrame = gridOf(still.frameCells(), *scene.front).at({ 2, 0 });

		// Scroll the viewport by an amount that is not a whole number of
		// cells, so a frame anchored on the wrong thing would land off the
		// wall tile rather than on it.
		gUISettings.worldViewportX -= 77.0f;
		gUISettings.worldViewportY -= 41.0f;
		auto const scrolled = recordPass(scene.front, atlas);
		auto const scrolledWall = gridOf(scrolled.wallCells(), *scene.front).at({ 2, 0 });
		auto const scrolledFrame = gridOf(scrolled.frameCells(), *scene.front).at({ 2, 0 });

		require(near(scrolledWall.rect.left - stillWall.rect.left, -77.0f)
				&& near(scrolledFrame.rect.left - stillFrame.rect.left, -77.0f),
			"A scroll did not carry the wall and its frame by the same amount: "
			"wall moved "
				+ std::to_string(scrolledWall.rect.left - stillWall.rect.left)
				+ ", frame moved "
				+ std::to_string(scrolledFrame.rect.left - stillFrame.rect.left));
		require(near(scrolledFrame.rect.left, scrolledWall.rect.left)
				&& near(scrolledFrame.rect.top, scrolledWall.rect.top),
			"After a scroll the frame is off the wall tile it moulds: wall "
				+ scrolledWall.rect.str() + ", frame "
				+ scrolledFrame.rect.str());
	}

	// The frame is trimmed at the ceiling. A Room stands 0.9 of a cell per
	// level while the cell is 1.0, so an untrimmed frame would stand a
	// tenth of a cell of moulding above the ceiling it is moulding.
	void theFrameIsTrimmedAtTheCeiling(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene(4, 2, 0, CORE_ROOM_MAX_HEIGHT);
		core::Vector2 lo{}, hi{};
		scene.front->getBounds(lo, hi);
		auto const ceiling = std::min(toScreen(lo).y, toScreen(hi).y);

		auto const pass = recordPass(scene.front, atlas);
		require(!pass.frameCells().empty(),
			"The fixture Window drew no frame to trim");
		for (auto const& frame : pass.frameCells())
			require(frame.rect.top >= ceiling - 0.01f,
				"A frame stood " + std::to_string(ceiling - frame.rect.top)
					+ " pixels above the ceiling it is moulding at "
					+ frame.rect.str());
		// And the wall tile it lies on is trimmed the same way, so the two
		// land on the same crop rather than one above the other.
		for (auto const& wall : pass.wallCells())
			require(wall.rect.top >= ceiling - 0.01f,
				"A wall tile stood above its own ceiling; the trim cannot be "
				"compared against a wall that is not trimmed either");
	}

	// A variant an author painted on a window cell still draws. Nothing
	// outranks it any more - the frame lands over both - so the cell wears
	// what it was painted with and the frame lies over it.
	void aVariantUnderAWindowStillDraws(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		std::string diagnostic;
		require(scene.world->setSectorSurfaceVariant(scene.frontIndex, 2, 0, 1u,
				&diagnostic).changed,
			"The fixture variant could not be painted on the window cell: "
				+ diagnostic);
		require(scene.front->surfaceVariantAt(2, 0) == 1u,
			"The painted entry did not reach the live Sector");
		auto const pass = recordPass(scene.front, atlas);
		require(tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0) == "room-V1",
			"The painted variant did not draw under the Window; it drew "
			+ tileAt(gridOf(pass.wallCells(), *scene.front), 2, 0));
		require(tileAt(gridOf(pass.frameCells(), *scene.front), 2, 0)
				== "windowframe-SS",
			"The frame did not land over the painted variant");
	}

	// Taking the Window out takes its frame with it and leaves the wall
	// exactly as it was.
	void aRemovedWindowLeavesNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		require(!recordPass(scene.front, atlas).frameCells().empty(),
			"The fixture Window drew no frame to remove");
		require(scene.world->removeSectorWindow(scene.frontIndex,
				windowObjectIndex(*scene.front)),
			"The fixture Window could not be removed");
		scene.refresh();
		require(windowObjectIndex(*scene.front) == ~0u,
			"The front Location still carries a Window object after the removal");
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A removed Window left a frame: "
			+ tileList(pass.frameCells()));
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A removed Window did not leave the wall whole");
	}

	// Only the front side of the Layer pair is framed. A Window is
	// registered on both Sectors of the pair it joins, and the Sector
	// behind is seen *through* the opening rather than framing it, so it
	// keeps its own base tile and wears no moulding.
	void theBackSectorOfTheLayerPairWearsNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
		std::shared_ptr<const core::Sector> back;
		for (uint32_t index = 0; index < scene.world->getNumSectors(); ++index)
		{
			auto const candidate = scene.world->getSector(index);
			if (candidate && candidate->getLayerIndex() == 1) back = candidate;
		}
		require(back != nullptr, "The fixture has no Sector behind the Window");
		require(windowOf(*back) != nullptr,
			"The Sector behind the Window does not carry the Window's back "
			"registration, so this check would prove nothing about it");
		// Drawn on its own, the back wall is just a Room: no frame, because
		// it frames nothing.
		auto const pass = recordPass(back, atlas);
		require(pass.frameCells().empty(), "The Sector behind a Window wore "
			"a frame: " + tileList(pass.frameCells()));
		require(gridOf(pass.wallCells(), *back).size() == 4,
			"The back Sector failed to draw its own wall, so the absence of a "
			"frame there would prove nothing");
	}

	// The frame does not vary with its wall. The same opening in a Room, a
	// Corridor and a Facade draws the same tile names, because the wall's
	// look comes from the wall's tile and the frame only carries the
	// moulding.
	void everyWallWearsTheSameFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		std::vector<std::string> drawn;
		drawn.push_back(tileList(recordPass(roomScene().front, atlas)
			.frameCells()));
		drawn.push_back(tileList(recordPass(corridorScene().front, atlas)
			.frameCells()));
		drawn.push_back(tileList(recordPass(facadeScene().front, atlas)
			.frameCells()));
		require(drawn[0] == drawn[1] && drawn[1] == drawn[2],
			"The same opening drew a different frame per wall: Room " + drawn[0]
				+ ", Corridor " + drawn[1] + ", Facade " + drawn[2]);
		require(drawn[0] == "windowframe-SS",
			"A 1x1 opening drew " + drawn[0]);
	}

	// The Facade's own surface still covers every cell, and the frame
	// lands over it. A Facade draws a tile of its family rather than a
	// colour the renderer packed from its authored triple, so the rule
	// ADR 0003 states - that nothing in the threshold path paints over an
	// aperture - is held by the same per-cell layout every wall uses, and
	// the frame narrows nothing and punches nothing.
	void theFacadeSurfaceStillCoversEveryCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = facadeScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const surface = pass.wallCells();
		require(surface.size() == 4, "The Facade's surface came out as "
			+ std::to_string(surface.size())
			+ " tiles for a Facade four cells wide");
		for (auto const& q : surface)
			require(q.tile == "facade", "The Facade drew '" + q.tile
				+ "'; its surface is its own tile, not a borrowed wall's");
		require(tileAt(gridOf(pass.frameCells(), *scene.front), 2, 0)
				== "windowframe-SS",
			"The Facade's window cell wore no frame over its own tile");
	}

	// The frame is drawn as its own artwork. It carries no colour of its
	// own and none is multiplied into it, whether the wall it moulds is a
	// Facade or a Room: what the moulding looks like is settled by the
	// frame's own tiles and by nothing else.
	void theFrameIsDrawnRaw(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		{
			auto scene = facadeScene();
			auto const pass = recordPass(scene.front, atlas);
			require(!pass.frameCells().empty(), "The Facade drew no frame");
			for (auto const& q : pass.frameCells())
				require(q.colour == kRaw,
					"A Facade frame was drawn with a colour other than its raw "
					"artwork; the moulding is its own picture and takes no "
					"colour from the wall it lies over");
		}
		{
			auto scene = roomScene();
			auto const pass = recordPass(scene.front, atlas);
			require(!pass.frameCells().empty(), "The Room drew no frame");
			for (auto const& q : pass.frameCells())
				require(q.colour == kRaw,
					"A Room frame was drawn with a colour other than its raw "
					"artwork");
		}
	}

	// A BoothWindow wears no windowframe.
	//
	// A BoothWindow *is* a 1x1 Window - a subclass, constructed as one
	// cell by one level - so a derivation from the opening that did not
	// gate on it would land a second moulding over every service hatch.
	// The frame it already owns is not interchangeable with this one: the
	// shutter's clip is derived from that frame's inner rect, and the
	// whole closed-versus-open read depends on the shutter and the frame
	// staying in lockstep. Migrating the hatch into the windowframe system
	// is its own decision with the shutter's clip inside it, and until
	// that decision is made this is the check that keeps the two frames
	// from meeting.
	void aBoothWindowWearsNoWindowframe(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = boothScene();
		require(scene.window->isBoothWindow(),
			"The fixture is not a BoothWindow, so this check would prove "
			"nothing about one");
		// The hatch is drawn - it is a Window, and its own frame and
		// shutter go down - so the absence of a windowframe is the frame
		// rule refusing it rather than the whole Window pass being skipped.
		auto const pass = recordPass(scene.front, atlas);
		require(!pass.glass().empty() || !pass.behindCells().empty(),
			"The BoothWindow drew nothing at all, so the absence of a "
			"windowframe here would prove nothing");
		require(pass.frameCells().empty(),
			"A BoothWindow wore a windowframe over the frame it already owns: "
				+ tileList(pass.frameCells()));
		// And the wall under it is whole, so the exclusion did not take
		// the wall's own tile with it either.
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A BoothWindow's exclusion took a wall cell with it");
	}

	// A frame the installed atlas does not carry is no frame at all: the
	// wall's own tile is already down, so there is nothing for a missing
	// frame to leave uncovered, and nothing draws in the missing-art
	// colour. This is what lets the role grid be filled one tile at a
	// time.
	void aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta(smoke::Context const&)
	{
		auto atlas = installAtlas(false);
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A frame drew from an atlas that "
			"authors none: " + tileList(pass.frameCells()));
		for (auto const& q : pass.quads)
			require(q.colour != kMagenta, "A cell drew the missing-art magenta "
				"for a frame that was never authored; a missing frame is no "
				"frame, not a hole");
		// The wall is still whole. Nothing was lost by the frame's absence.
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A missing frame took a wall cell with it");
	}

	// A wall with no Window in it draws no frame at all - and, the other
	// half of the same rule, draws its own tile on every cell as it always
	// did.
	void aWallWithNoWindowDrawsNoFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		// A Room built with no Window in it at all, rather than one with
		// the Window taken back out: the rule is about there being no
		// Window, not about the removal.
		auto world = std::make_shared<core::World>("Window-less world", 16, 6);
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
		require(windowOf(*scene.front) == nullptr,
			"The window-less fixture grew a Window");
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().empty(), "A window-less wall drew a frame: "
			+ tileList(pass.frameCells()));
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"A window-less wall did not draw all four of its cells");
	}

	// The frame follows the wall through the styles the viewport draws it
	// with. A Solid pass frames; a wireframe pass contributes outlines and
	// no moulding, because a wireframe Layer never paints filled geometry
	// over the selection.
	void theFrameFollowsTheWallThroughEveryStyle(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = roomScene();
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

	// The frame is derived from the Window that owns it, not from anything
	// painted and not from a walk that could mistake one Window for
	// another. Two Windows on one wall each frame their own cell and
	// neither reaches into the other's opening.
	void adjacentWindowsFrameTheirOwnCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto world = std::make_shared<core::World>("Two window world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 0, 0, 6, 1);
		world->addRoom("Back", 1, 0, 0, 6, 1);
		world->addSectorWindow(0, 0, 1, 1, 1);
		world->addSectorWindow(0, 0, 4, 1, 1);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.frontLayer = 0;
		scene.refresh();
		require(scene.front->getCellsWide() == 6,
			"The two-window fixture lost its front Room");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 2, "Two 1x1 Windows drew "
			+ std::to_string(frames.size()) + " frame cells; each is its own "
			"single-column opening and each frames only its own cell");
		require(tileAt(frames, 1, 0) == "windowframe-SS"
				&& tileAt(frames, 4, 0) == "windowframe-SS",
			"Two adjacent 1x1 Windows did not each wear their own SS frame: "
				+ tileList(pass.frameCells()));
		// And every frame sits inside the glass of the Window it belongs
		// to, rather than somewhere along the wall.
		for (auto const& frame : pass.frameCells())
		{
			bool inside = false;
			for (uint32_t ordinal = 0; ordinal < 2; ++ordinal)
			{
				auto const window = windowOf(*scene.front, ordinal);
				if (window && frame.rect.overlaps(glassRect(*window))) inside = true;
			}
			require(inside, "A frame at " + frame.rect.str()
				+ " lies outside every Window's glass on this wall");
		}
	}

	// The name the render path asks for is the name the atlas carries, for
	// every cell of the role grid. Stated as a function of the four facts
	// so a name can be pinned without a live Window to derive it - which
	// is what lets the roles no shipped Window reaches yet be checked
	// against the convention the block is laid out on.
	void theFrameNameIsTheOpeningAndNothingElse(smoke::Context const&)
	{
		installAtlas(true);
		require(windowframeTileName(1, 1, 0, 0) == "windowframe-SS",
			"1x1: single column, single level");
		require(windowframeTileName(2, 1, 0, 0) == "windowframe-SL",
			"2x1 left cell: leftmost column, single level");
		require(windowframeTileName(2, 1, 1, 0) == "windowframe-SR",
			"2x1 right cell: rightmost column, single level");
		require(windowframeTileName(3, 1, 1, 0) == "windowframe-SM",
			"3x1 middle cell: middle column, single level");
		require(windowframeTileName(1, 2, 0, 0) == "windowframe-BS",
			"1x2 bottom cell: single column, bottom of a taller span");
		require(windowframeTileName(1, 2, 0, 1) == "windowframe-TS",
			"1x2 top cell: single column, top of a taller span");
		require(windowframeTileName(3, 3, 0, 1) == "windowframe-ML",
			"3x3 middle-left: leftmost column, middle level");
		// The all-middle cell asks for no tile rather than wearing a
		// transparent one: a span that is middle in both directions is all
		// glass with no moulding on it, and the empty name is the same
		// answer a missing tile gives.
		require(windowframeTileName(3, 3, 1, 1).empty(),
			"The all-middle cell of a 3x3 asked for a tile; it is all glass "
			"with no moulding and asks for nothing");

		// The name carries the opening and nothing else. No wall, no glass
		// style, no Window State appears in any name the derivation
		// produces.
		for (uint32_t wide = 1; wide <= 4; ++wide)
			for (uint32_t high = 1; high <= 4; ++high)
				for (uint32_t column = 0; column < wide; ++column)
					for (uint32_t level = 0; level < high; ++level)
					{
						auto const name = windowframeTileName(wide, high, column,
							level);
						if (name.empty()) continue;
						require(name.rfind("windowframe-", 0) == 0
								&& name.size() == std::string("windowframe-SS").size(),
							"A windowframe name is not the two-role shape: "
								+ name);
						for (auto const* wall : { "room", "corridor", "facade",
								"clear", "frosted", "tinted" })
							require(name.find(wall) == std::string::npos,
								"A windowframe name carries '" + std::string{ wall }
									+ "', which is not a fact about the opening: "
									+ name);
					}

		require(hasSectorTileOverlay("windowframe-SS"),
			"windowframe-SS is named by the render path but the installed "
			"atlas does not carry it");
		require(!hasSectorTileSurface("windowframe-SS"),
			"A windowframe answered the Sector-surface query; it is a tile of "
			"the atlas but not a surface a Sector is authored with");
	}
}

void render_smoke::registerWindowframeCells(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "windowframeCells/theWindowCellStillDrawsItsWallTile",
		isolated<theWindowCellStillDrawsItsWallTile> });
	checks.push_back({ "windowframeCells/theFrameIsLaidOverTheWindowCell",
		isolated<theFrameIsLaidOverTheWindowCell> });
	checks.push_back({ "windowframeCells/theFrameIsDrawnLast",
		isolated<theFrameIsDrawnLast> });
	checks.push_back({ "windowframeCells/theFrameKeepsTheFloorAnchoredGrid",
		isolated<theFrameKeepsTheFloorAnchoredGrid> });
	checks.push_back({ "windowframeCells/theFrameStaysPinnedToTheWallAsTheCanvasScrolls",
		isolated<theFrameStaysPinnedToTheWallAsTheCanvasScrolls> });
	checks.push_back({ "windowframeCells/theFrameIsTrimmedAtTheCeiling",
		isolated<theFrameIsTrimmedAtTheCeiling> });
	checks.push_back({ "windowframeCells/aVariantUnderAWindowStillDraws",
		isolated<aVariantUnderAWindowStillDraws> });
	checks.push_back({ "windowframeCells/aRemovedWindowLeavesNoFrame",
		isolated<aRemovedWindowLeavesNoFrame> });
	checks.push_back({ "windowframeCells/theBackSectorOfTheLayerPairWearsNoFrame",
		isolated<theBackSectorOfTheLayerPairWearsNoFrame> });
	checks.push_back({ "windowframeCells/everyWallWearsTheSameFrame",
		isolated<everyWallWearsTheSameFrame> });
	checks.push_back({ "windowframeCells/theFacadeSurfaceStillCoversEveryCell",
		isolated<theFacadeSurfaceStillCoversEveryCell> });
	checks.push_back({ "windowframeCells/theFrameIsDrawnRaw",
		isolated<theFrameIsDrawnRaw> });
	checks.push_back({ "windowframeCells/aBoothWindowWearsNoWindowframe",
		isolated<aBoothWindowWearsNoWindowframe> });
	checks.push_back({ "windowframeCells/aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta",
		isolated<aFrameTheAtlasDoesNotCarryDrawsNoFrameAndNoMagenta> });
	checks.push_back({ "windowframeCells/aWallWithNoWindowDrawsNoFrame",
		isolated<aWallWithNoWindowDrawsNoFrame> });
	checks.push_back({ "windowframeCells/theFrameFollowsTheWallThroughEveryStyle",
		isolated<theFrameFollowsTheWallThroughEveryStyle> });
	checks.push_back({ "windowframeCells/adjacentWindowsFrameTheirOwnCells",
		isolated<adjacentWindowsFrameTheirOwnCells> });
	checks.push_back({ "windowframeCells/theFrameNameIsTheOpeningAndNothingElse",
		isolated<theFrameNameIsTheOpeningAndNothingElse> });
}
