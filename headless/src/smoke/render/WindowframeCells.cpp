// A 1x1 Window wears a windowframe over its wall (#1075), a wide Window
// frames its whole span as one opening (#1076), and a tall Window frames
// its span with a head across the top and a sill along the bottom
// (#1077).
//
// The tracer bullet for the windowframe series pinned everything the wide,
// tall and corner tickets hang off, in the smallest complete shape the
// feature can take: the atlas carries the frame, the renderer derives it
// from the live Window rather than from anything painted, and it lands
// over the wall's own tile without taking anything away. The wide work
// added the horizontal axis of the role grid; the tall work here adds the
// vertical one. The frame a cell wears now depends on where that cell sits
// in the opening's span on both axes, not only on the opening existing.
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
//   * a wide opening is framed as one opening: a jamb down the outside of
//     each end, the middle cells wearing the middle tile whose hole is the
//     full width of the cell, so the glass reads as continuous across the
//     span and no moulding stands between the panes;
//   * a tall opening is framed as one opening too, up the other axis: a
//     sill under the bottom cell, a head over the top one, and jambs only
//     down every level in between, so a tall window reads as tall rather
//     than as a stack of identical cells. The bottom cell's hole stops
//     above the cell's floor line where the glass does and the top cell's
//     hole reaches the cell's bottom edge where the glass runs down into
//     it, so the holes join into one continuous aperture across the cell
//     boundary;
//   * a Window on a back Layer, seen through a clear Window in the Layer
//     in front, wears its own frame. It arrives through the aperture
//     recursion - the far wall is drawn under the near Window's clip, and
//     the far Window that is part of that wall frames itself on the same
//     rule a front-wall Window does - rather than being suppressed by the
//     clip it arrives under, which is what keeps a nested aperture reading
//     as a window rather than as a hole punched in the view;
//   * two Windows side by side each frame their own cells, and no frame of
//     either one lies over the other's glass;
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

	// The frames the shipped set authors: the whole single-level row of
	// the role grid, and the whole single-column column of it - the wide
	// single-level opening and the tall single-column one. The grid has
	// fifteen cells; the wide columns of the three tall rows are the
	// corner work that hangs off this.
	std::vector<std::string> const kWindowframes{ "windowframe-SS",
		"windowframe-SL", "windowframe-SM", "windowframe-SR",
		"windowframe-TS", "windowframe-MS", "windowframe-BS" };

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

		// The frames that arrived *through* an aperture rather than under
		// the Sector's own clip: a Window on a back Layer, seen through the
		// clear Window in front, wearing its own moulding (#1077). Kept
		// apart from `frameCells()` because the two are different facts - one
		// is the wall this Sector frames, the other is a further wall framed
		// somewhere down the recursion - and a check that conflated them
		// could not tell a nested frame from a missing one.
		std::vector<DrawnQuad> nestedFrameCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (!q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas
						&& isWindowframeTile(q.tile))
					out.push_back(q);
			return out;
		}

		// The back Sector's own wall, with no frame of any nested opening
		// mixed in, so an ordering check against "the wall behind the glass"
		// is not comparing a frame against itself.
		std::vector<DrawnQuad> farWallCells() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : behindCells())
				if (!isWindowframeTile(q.tile)) out.push_back(q);
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

	// The Window authored *on* a Sector - the one whose front Sector is
	// this one - as distinct from the back registrations a Sector carries
	// for every Window set into the wall in front of it. A back Layer's
	// wall holds both, and only the one it was authored into frames itself.
	std::shared_ptr<const core::Window> frontWindowOf(
		std::shared_ptr<const core::Sector> const& sector)
	{
		if (!sector) return nullptr;
		for (uint32_t index = 0; index < sector->getNumObjects(); ++index)
		{
			auto const object = sector->getObject(index);
			if (!object || !isWindowObject(*object)) continue;
			auto const window = std::static_pointer_cast<const core::WindowSectorObject>(
				object)->getWindow();
			if (window && window->getFrontSector() == sector) return window;
		}
		return nullptr;
	}

	// How many Window objects a Sector carries, of either kind. A back wall
	// holds its own and the front wall's back registration, and a fixture
	// that means to tell them apart has to carry both.
	uint32_t windowObjectCount(core::Sector const& sector)
	{
		uint32_t seen{};
		for (uint32_t index = 0; index < sector.getNumObjects(); ++index)
		{
			auto const object = sector.getObject(index);
			if (object && isWindowObject(*object)) ++seen;
		}
		return seen;
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
		float frontTopLevelHeight, bool booth, uint32_t windowCellsWide = 1,
		uint32_t windowLevelsHigh = 1)
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
		else world->addSectorWindow(0, windowLevel, windowX, windowCellsWide,
			windowLevelsHigh);
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
		require(scene.window->getCellsWide() == windowCellsWide
				&& scene.window->getLevelsHigh() == windowLevelsHigh,
			"The fixture Window did not come back the size it was authored as");
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

	// A Room two cells wider than the one Window set into it, so a check
	// can see the cells either side of the span wearing no frame.
	Scene wideRoomScene(uint32_t wide, uint32_t windowX = 1)
	{
		return buildScene("room", wide + 2, 1, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, wide, 1);
	}

	Scene wideCorridorScene(uint32_t wide, uint32_t windowX = 1)
	{
		return buildScene("corridor", wide + 2, 1, windowX, 0,
			CORE_CORRIDOR_HEIGHT, false, wide, 1);
	}

	Scene wideFacadeScene(uint32_t wide, uint32_t windowX = 1)
	{
		return buildScene("facade", wide + 2, 1, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, wide, 1);
	}

	// A Room `tall` levels high with a single-column Window of the same
	// height set into it, so the window spans the whole height of the wall
	// it is framed in and every level of the span is on screen at once.
	Scene tallRoomScene(uint32_t tall, uint32_t windowX = 2)
	{
		return buildScene("room", 4, tall, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, 1, tall);
	}

	Scene tallCorridorScene(uint32_t tall, uint32_t windowX = 2)
	{
		return buildScene("corridor", 4, tall, windowX, 0,
			CORE_CORRIDOR_HEIGHT, false, 1, tall);
	}

	Scene tallFacadeScene(uint32_t tall, uint32_t windowX = 2)
	{
		return buildScene("facade", 4, tall, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, 1, tall);
	}

	// A fixture carrying more than one Window, which `buildScene` cannot
	// build because it resolves the front wall by asking the World which
	// Sector fronts a Window and insists the answer is one. Here the front
	// wall is named, and the Windows are placed by ordinal.
	Scene multiWindowScene(uint32_t roomWide,
		std::vector<std::pair<uint32_t, uint32_t>> const& spans)
	{
		auto world = std::make_shared<core::World>("Many window world", 16, 6);
		while (world->getLayerCount() < 2) world->addLayer();
		world->addRoom("Front", 0, 0, 0, roomWide, 1);
		world->addRoom("Back", 1, 0, 0, roomWide, 1);
		for (auto const& [x, wide] : spans)
			world->addSectorWindow(0, 0, x, wide, 1);
		world->finishBuild();
		world->pauseSimulation();

		Scene scene;
		scene.world = world;
		scene.frontName = "Front";
		scene.frontLayer = 0;
		scene.refresh();
		require(scene.front->getCellsWide() == roomWide,
			"The multi-window fixture lost its front Room");
		require(scene.front->getNumObjects() >= spans.size(),
			"The multi-window fixture did not place every Window");
		return scene;
	}

	// A Window on a back Layer, seen through a clear Window in the Layer in
	// front (#1077). Three Layers: the front wall the near Window is set
	// into, the middle wall that carries its *own* Window, and a third
	// behind that the back Window opens into, because a Window needs a Layer
	// behind it and a Window on the back-most Layer cannot be authored at
	// all.
	//
	// The near Window is three cells wide and the far one one cell, set at
	// the near span's middle column. That is deliberate twice over: the far
	// Window's jambs fall inside the near aperture rather than behind the
	// near glass, so "the far frame arrives" is a claim about artwork with
	// somewhere to be seen rather than about a quad the clip swallowed
	// whole; and the column the far frame lands on is one the *near* Window
	// also frames, wearing its own wide-middle tile there. Two different
	// tiles on one column, one from each opening, is what lets a check name
	// the far frame as the far Window's own rather than the near opening's
	// moulding seen from behind.
	struct NestedScene
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<const core::Sector> front;
		std::shared_ptr<const core::Sector> middle;
		std::shared_ptr<const core::Window> nearWindow;
		std::shared_ptr<const core::Window> farWindow;
		// The column both openings frame: the near span's middle, and the
		// cell the far Window was authored into.
		uint32_t sharedColumn{ 2 };
	};

	NestedScene nestedWindowScene()
	{
		auto world = std::make_shared<core::World>("Nested window world", 16, 6);
		while (world->getLayerCount() < 3) world->addLayer();
		world->addRoom("Front", 0, 0, 0, 4, 1);
		world->addRoom("Middle", 1, 0, 0, 4, 1);
		world->addRoom("Far", 2, 0, 0, 4, 1);
		// Near: Layer 0, cells 1 through 3. Far: Layer 1, cell 2 - authored
		// on the middle wall, so that wall is its front Sector and it frames
		// it.
		world->addSectorWindow(0, 0, 1, 3, 1);
		world->addSectorWindow(1, 0, 2, 1, 1);
		world->finishBuild();
		world->pauseSimulation();

		NestedScene scene;
		scene.world = world;
		for (uint32_t index = 0; index < world->getNumSectors(); ++index)
		{
			auto const sector = world->getSector(index);
			if (!sector) continue;
			if (sector->getLayerIndex() == 0) scene.front = sector;
			if (sector->getLayerIndex() == 1) scene.middle = sector;
		}
		require(scene.front != nullptr && scene.middle != nullptr,
			"The nested fixture lost a Layer's wall");
		scene.nearWindow = frontWindowOf(scene.front);
		scene.farWindow = frontWindowOf(scene.middle);
		require(scene.nearWindow != nullptr && scene.farWindow != nullptr,
			"The nested fixture lost a Window");
		require(scene.nearWindow->getCellsWide() == 3
				&& scene.nearWindow->getLevelsHigh() == 1,
			"The near Window is not the 3x1 opening the fixture was built for");
		require(scene.farWindow->getCellsWide() == 1
				&& scene.farWindow->getLevelsHigh() == 1,
			"The far Window is not the 1x1 opening the fixture was built for");
		// The far Window's own front Sector is the middle wall. If that were
		// not so, the middle wall would be a back registration like any
		// other and would frame nothing, and the check downstream would be
		// reading an empty pass as a suppression.
		require(scene.farWindow->getFrontSector() == scene.middle,
			"The back Window is not authored on the middle Layer, so there is "
			"no frame to look for down the aperture");
		// And the middle wall does carry the near Window's back registration
		// too, so the fixture really holds both kinds and the check tells
		// them apart rather than finding only one kind present.
		require(windowObjectCount(*scene.middle) == 2,
			"The middle wall carries " + std::to_string(windowObjectCount(*scene.middle))
				+ " Window objects; it needs both its own and the near Window's "
			"back registration for this fixture to prove the two are told apart");
		return scene;
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

	// The screen rect of one cell of a Sector, by its column and level in
	// that Sector's own floor-anchored grid. Used to say *which* cell a
	// frame landed on rather than only that a frame landed somewhere.
	ScreenRect cellRect(core::Sector const& sector, uint32_t column,
		uint32_t level)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const a = toScreen(low);
		auto const b = toScreen(high);
		auto const left = std::min(a.x, b.x);
		auto const floorLine = std::max(a.y, b.y);
		auto const cellWidth = CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom;
		auto const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
		ScreenRect rect;
		rect.include(pf::render::Vec2{ left + float(column) * cellWidth,
			floorLine - float(level) * cellHeight });
		rect.include(pf::render::Vec2{ left + float(column + 1) * cellWidth,
			floorLine - float(level + 1) * cellHeight });
		return rect;
	}

	// The glass rect of one of the Windows a multi-Window fixture carries.
	ScreenRect glassRectOf(core::Sector const& sector, uint32_t ordinal)
	{
		auto const window = windowOf(sector, ordinal);
		require(window != nullptr, "The fixture has no Window "
			+ std::to_string(ordinal));
		return glassRect(*window);
	}

	// The frame quads left to right, so "the next cell along" means the
	// same thing in every check that walks the span.
	std::vector<DrawnQuad> leftToRight(std::vector<DrawnQuad> const& quads)
	{
		auto out = quads;
		std::sort(out.begin(), out.end(), [](auto const& a, auto const& b)
		{
			return a.rect.left < b.rect.left;
		});
		return out;
	}

	// A 2-wide single-level Window frames its two cells as one opening: a
	// jamb down the outside of each end and nothing between them. The left
	// cell wears `windowframe-SL` and the right cell `windowframe-SR`, and
	// the two frame quads meet on the cell boundary rather than leaving a
	// strip of anything between them or overlapping across it.
	void aTwoWideWindowFramesItsTwoCellsAsOneOpening(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = wideRoomScene(2);
		require(scene.window->getCellsWide() == 2,
			"The fixture Window is not two cells wide");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 2, "A 2-wide Window drew "
			+ std::to_string(frames.size()) + " frame cells; a wide opening is "
			"framed across its whole span, one tile per cell");
		require(tileAt(frames, 1, 0) == "windowframe-SL",
			"The left cell of a 2-wide Window wore " + tileAt(frames, 1, 0)
				+ "; the span's left end takes the left jamb");
		require(tileAt(frames, 2, 0) == "windowframe-SR",
			"The right cell of a 2-wide Window wore " + tileAt(frames, 2, 0)
				+ "; the span's right end takes the right jamb");
		require(tileAt(frames, 0, 0) == "<nothing>"
				&& tileAt(frames, 3, 0) == "<nothing>",
			"A cell outside the span wore a frame: " + tileList(pass.frameCells()));

		// Each frame quad stays inside its own cell, so nothing is drawn
		// across the seam: the moulding of one cell stops where the next
		// cell's begins, and the two holes join into one aperture.
		for (auto const& frame : pass.frameCells())
		{
			auto const width = frame.rect.right - frame.rect.left;
			require(near(width, CORE_CELL_WIDTH_PIXELS * gUISettings.worldZoom),
				"A wide-span frame quad is not one cell wide, so it straddles "
				"the seam between cells: " + frame.rect.str());
		}
		for (uint32_t column = 1; column + 1 < 3; ++column)
		{
			auto const here = frames.at({ column, 0 });
			auto const next = frames.at({ column + 1, 0 });
			require(near(here.rect.right, next.rect.left),
				"Two cells of one wide span do not meet on the cell boundary: "
				+ here.rect.str() + " and " + next.rect.str());
		}
	}

	// A 3-wide single-level Window wears SL, SM and SR across its three
	// cells. The middle cell wears the middle tile, whose hole is the full
	// width of its cell - no vertical moulding in the middle, so the glass
	// reads as continuous across the whole span.
	void aThreeWideWindowFramesItsThreeCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = wideRoomScene(3);
		require(scene.window->getCellsWide() == 3,
			"The fixture Window is not three cells wide");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 3, "A 3-wide Window drew "
			+ std::to_string(frames.size()) + " frame cells");
		require(tileAt(frames, 1, 0) == "windowframe-SL",
			"The left cell of a 3-wide Window wore " + tileAt(frames, 1, 0));
		require(tileAt(frames, 2, 0) == "windowframe-SM",
			"The middle cell of a 3-wide Window wore " + tileAt(frames, 2, 0)
				+ "; a cell that is neither end of the span takes the middle "
				"tile, which has no vertical moulding in it");
		require(tileAt(frames, 3, 0) == "windowframe-SR",
			"The right cell of a 3-wide Window wore " + tileAt(frames, 3, 0));
		require(tileAt(frames, 0, 0) == "<nothing>"
				&& tileAt(frames, 4, 0) == "<nothing>",
			"A cell outside the 3-wide span wore a frame: "
				+ tileList(pass.frameCells()));
	}

	// The wide opening is one aperture. The glass runs straight through the
	// cell boundary between the two cells - the inset is at the ends of the
	// span, not at the ends of each cell - and the frames tile the span
	// with nothing left over and nothing doubled up, which is what makes a
	// 2-wide Window read as one window rather than as two touching.
	void theWideOpeningIsOneContinuousAperture(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = wideRoomScene(2);
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = leftToRight(pass.frameCells());
		require(frames.size() == 2, "Expected two frame cells to walk the span");

		// The glass crosses the seam between the two cells rather than
		// stopping short of it, so there is no wall, and no moulding, left
		// standing in the middle of the opening.
		auto const glass = glassRect(*scene.window);
		auto const seam = frames[0].rect.right;
		require(glass.left < seam - 0.01f && glass.right > seam + 0.01f,
			"A 2-wide Window's glass does not cross the seam between its two "
			"cells, so the opening is not continuous: glass " + glass.str()
				+ ", seam " + std::to_string(seam));

		// The frames cover the span from end to end with no gap and no
		// overlap, so the joined holes are one aperture rather than two
		// with something between them.
		require(near(frames.front().rect.left, glass.left
				- CORE_WINDOW_X_INSET * CORE_CELL_WIDTH_PIXELS
					* gUISettings.worldZoom),
			"The wide frame does not start at the span's left inset");
		require(near(frames.back().rect.right, glass.right
				+ CORE_WINDOW_X_INSET * CORE_CELL_WIDTH_PIXELS
					* gUISettings.worldZoom),
			"The wide frame does not end at the span's right inset");
		auto covered = 0.0f;
		for (auto const& frame : frames) covered += frame.rect.right - frame.rect.left;
		require(near(covered, frames.back().rect.right - frames.front().rect.left),
			"The wide span's frame quads overlap, so the span is doubled up "
			"somewhere and cannot read as one opening");
	}

	// Two wide Windows side by side each frame their own cells. Neither
	// frame reaches across into the other's opening: the seam between two
	// abutting Windows is two jambs meeting, not one shared moulding, and
	// no frame of one lies over the other's glass.
	void twoWideWindowsFrameTheirOwnCells(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		// Cells 1-2 and 3-4, abutting with no bare cell between them, so
		// "does one opening's frame leak into the other's" is asked at the
		// tightest place it can be rather than across a gap.
		auto scene = multiWindowScene(6, { { 1, 2 }, { 3, 2 } });
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 4, "Two 2-wide Windows drew "
			+ std::to_string(frames.size()) + " frame cells; each is its own "
			"two-cell opening and each frames only its own cells");
		require(tileAt(frames, 1, 0) == "windowframe-SL"
				&& tileAt(frames, 2, 0) == "windowframe-SR",
			"The first 2-wide Window did not frame its own pair: "
				+ tileList(pass.frameCells()));
		require(tileAt(frames, 3, 0) == "windowframe-SL"
				&& tileAt(frames, 4, 0) == "windowframe-SR",
			"The second 2-wide Window did not frame its own pair: "
				+ tileList(pass.frameCells()));
		require(tileAt(frames, 0, 0) == "<nothing>"
				&& tileAt(frames, 5, 0) == "<nothing>",
			"A cell belonging to no opening wore a frame: "
				+ tileList(pass.frameCells()));

		// And no frame of either Window lies over the other's glass: each
		// of the four frames belongs to exactly one of the two openings.
		for (auto const& frame : pass.frameCells())
		{
			size_t overlapped{};
			for (uint32_t ordinal = 0; ordinal < 2; ++ordinal)
				if (frame.rect.overlaps(glassRectOf(*scene.front, ordinal)))
					++overlapped;
			require(overlapped == 1, "A frame at " + frame.rect.str()
				+ " lies over " + std::to_string(overlapped)
				+ " Windows' glass; each frame belongs to exactly one opening");
		}
	}

	// The frame still lies over the wall's own tile and is still drawn
	// last, on every cell of the wider span rather than only on the first
	// one. A wide opening must not lose the ordering anywhere across it.
	void theWideFrameIsDrawnLastOnEveryCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = wideRoomScene(3);
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(pass.frameCells().size() == 3 && pass.wallCells().size() == 5,
			"The 3-wide fixture's draw was missing a layer: wall "
			+ std::to_string(pass.wallCells().size()) + ", frames "
			+ std::to_string(pass.frameCells().size()));
		auto const lastWall = *pass.lastIndexOf(pass.wallCells());
		auto const lastBehind = *pass.lastIndexOf(pass.behindCells());
		auto const lastGlass = *pass.lastIndexOf(pass.glass());
		for (uint32_t column = 1; column <= 3; ++column)
		{
			require(frames.count({ column, 0 }) == 1,
				"Column " + std::to_string(column) + " of the wide span drew no frame");
			require(wall.count({ column, 0 }) == 1,
				"Column " + std::to_string(column) + " of the wide span drew no wall tile");
			auto const& over = frames.at({ column, 0 });
			auto const& under = wall.at({ column, 0 });
			require(over.index > lastWall,
				"The frame on column " + std::to_string(column)
					+ " was drawn before the wall tile it lies over");
			require(over.index > lastBehind,
				"The frame on column " + std::to_string(column)
					+ " was drawn before the space behind");
			require(over.index > lastGlass,
				"The frame on column " + std::to_string(column)
					+ " was drawn before the glass it covers the seam of");
			require(near(over.rect.left, under.rect.left)
					&& near(over.rect.right, under.rect.right),
				"The frame on column " + std::to_string(column)
					+ " is not on the cell its wall tile is: wall "
					+ under.rect.str() + ", frame " + over.rect.str());
			require(under.tile == "room",
				"The wall under the wide span stopped drawing its own tile");
		}
	}

	// The wide row does not vary with its wall either. The same 2-wide
	// opening in a Room, a Corridor and a Facade draws the same two tiles,
	// because the frame carries the moulding and the wall carries its own
	// look, whatever wall that is.
	void everyWallFramesAWideOpeningTheSameWay(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto const drawnRoom = tileList(recordPass(wideRoomScene(2).front, atlas)
			.frameCells());
		auto const drawnCorridor = tileList(
			recordPass(wideCorridorScene(2).front, atlas).frameCells());
		auto const drawnFacade = tileList(
			recordPass(wideFacadeScene(2).front, atlas).frameCells());
		require(drawnRoom == drawnCorridor && drawnCorridor == drawnFacade,
			"A 2-wide opening drew a different frame per wall: Room "
			+ drawnRoom + ", Corridor " + drawnCorridor + ", Facade "
			+ drawnFacade);
		require(drawnRoom == "windowframe-SL, windowframe-SR",
			"A 2-wide opening did not draw the wide pair: " + drawnRoom);
	}

	// A 2-level single-column Window frames its two levels as one opening:
	// a sill under the bottom cell and a head over the top one. The bottom
	// cell wears `windowframe-BS` and the top cell `windowframe-TS`, and
	// nothing outside the span wears either.
	void aTwoLevelWindowFramesItsSpanWithAHeadAndASill(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = tallRoomScene(2);
		require(scene.window->getLevelsHigh() == 2,
			"The fixture Window is not two levels high");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 2, "A 2-level Window drew "
			+ std::to_string(frames.size()) + " frame cells; a tall opening is "
			"framed up its whole span, one tile per level");
		require(tileAt(frames, 2, 0) == "windowframe-BS",
			"The bottom cell of a 2-level Window wore " + tileAt(frames, 2, 0)
				+ "; the span's bottom end takes the sill tile");
		require(tileAt(frames, 2, 1) == "windowframe-TS",
			"The top cell of a 2-level Window wore " + tileAt(frames, 2, 1)
				+ "; the span's top end takes the head tile");
		require(tileAt(frames, 1, 0) == "<nothing>"
				&& tileAt(frames, 3, 0) == "<nothing>"
				&& tileAt(frames, 1, 1) == "<nothing>"
				&& tileAt(frames, 3, 1) == "<nothing>",
			"A cell outside the span wore a frame: " + tileList(pass.frameCells()));
	}

	// A 3-level single-column Window wears BS, MS and TS bottom to top. The
	// middle cell takes the middle tile, which carries jambs only and no
	// head or sill: a level that is neither end of the span has no end
	// moulding in it, so the opening reads as one tall window rather than
	// as a stack of identical cells.
	void aThreeLevelWindowFramesItsThreeLevelsBottomToTop(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = tallRoomScene(3);
		require(scene.window->getLevelsHigh() == 3,
			"The fixture Window is not three levels high");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 3, "A 3-level Window drew "
			+ std::to_string(frames.size()) + " frame cells");
		require(tileAt(frames, 2, 0) == "windowframe-BS",
			"The bottom cell of a 3-level Window wore " + tileAt(frames, 2, 0));
		require(tileAt(frames, 2, 1) == "windowframe-MS",
			"The middle cell of a 3-level Window wore " + tileAt(frames, 2, 1)
				+ "; a level that is neither end of the span takes the middle "
				"tile, whose hole is the whole cell height - jambs only, no head "
				"and no sill across the middle of the glass");
		require(tileAt(frames, 2, 2) == "windowframe-TS",
			"The top cell of a 3-level Window wore " + tileAt(frames, 2, 2));
		require(tileAt(frames, 1, 0) == "<nothing>"
				&& tileAt(frames, 3, 0) == "<nothing>"
				&& tileAt(frames, 1, 1) == "<nothing>"
				&& tileAt(frames, 3, 1) == "<nothing>"
				&& tileAt(frames, 1, 2) == "<nothing>"
				&& tileAt(frames, 3, 2) == "<nothing>",
			"A cell outside the tall span wore a frame: "
				+ tileList(pass.frameCells()));
	}

	// The tall opening is one aperture. The glass runs straight through the
	// boundary between each pair of levels - the 0.2 stand-off belongs to
	// the bottom of the span, not to the bottom of every cell - and the
	// frame quads tile the span from the sill to the head with no gap and
	// no overlap, so the joined holes are one opening rather than three
	// with something lying across them.
	void theTallOpeningIsOneContinuousAperture(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = tallRoomScene(3);
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.count({ 2, 0 }) == 1 && frames.count({ 2, 1 }) == 1
				&& frames.count({ 2, 2 }) == 1,
			"Expected three frame cells up the span, one per level, and the "
			"fixture did not give them: " + tileList(pass.frameCells()));

		core::Vector2 lo{}, hi{};
		scene.front->getBounds(lo, hi);
		auto const floorLine = std::max(toScreen(lo).y, toScreen(hi).y);
		auto const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;
		auto const glass = glassRect(*scene.window);

		// The glass crosses every cell boundary inside the span, so there is
		// no wall and no transom of moulding left standing in the middle of
		// the opening.
		for (uint32_t seam = 1; seam < 3; ++seam)
		{
			auto const y = floorLine - float(seam) * cellHeight;
			require(glass.top < y - 0.01f && glass.bottom > y + 0.01f,
				"A 3-level Window's glass does not cross the boundary between "
				"level " + std::to_string(seam - 1) + " and level "
					+ std::to_string(seam) + ", so the opening is not "
					"continuous: glass " + glass.str());
		}
		// The bottom cell's hole stops above the cell's floor line exactly
		// where the glass does, and the frame covers the whole cell down to
		// the floor, so that 0.2 stand-off is the sill the frame carries
		// rather than a gap the wall shows through.
		require(near(floorLine - glass.bottom,
				CORE_WINDOW_Y_OFFSET * cellHeight),
			"The glass does not stand 0.2 of a level above the span's floor "
			"line, so the sill the bottom frame cell carries is not the band "
			"the glass stops above: " + std::to_string(floorLine - glass.bottom));
		require(near(frames.at({ 2, 0 }).rect.bottom, floorLine),
			"The bottom frame cell stops short of the wall's floor line, so the "
			"sill it carries has no floor to sit on: "
				+ frames.at({ 2, 0 }).rect.str());

		// And the quads tile the span from floor to head with no gap and no
		// overlap, so the joined holes are one aperture.
		std::vector<DrawnQuad> column;
		for (uint32_t level = 0; level < 3; ++level)
			column.push_back(frames.at({ 2, level }));
		float covered{};
		for (auto const& frame : column)
			covered += frame.rect.bottom - frame.rect.top;
		// The column runs downward on the screen, so the span's height is the
		// bottom cell's bottom edge to the head cell's top edge.
		require(near(covered, column.front().rect.bottom - column.back().rect.top),
			"The tall span's frame quads overlap, so the span is doubled up "
			"somewhere and cannot read as one opening");
		for (uint32_t level = 0; level + 1 < column.size(); ++level)
			require(near(column[level].rect.top, column[level + 1].rect.bottom),
				"Two levels of one tall span do not meet on the cell boundary, "
				"which puts a bar across the glass at the join: "
				+ column[level].rect.str() + " and "
				+ column[level + 1].rect.str());
	}

	// The tall frame is trimmed at the ceiling too. A Room stands 0.9 of a
	// cell on its top level while the cell is 1.0, so three whole cells of
	// frame would end a tenth of a cell above the ceiling they are moulding.
	// The head cell is cut back to the ceiling; the two levels under it are
	// untouched, because only the top of the span meets the ceiling.
	void theTallFrameIsTrimmedAtTheCeiling(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = tallRoomScene(3, 2);
		core::Vector2 lo{}, hi{};
		scene.front->getBounds(lo, hi);
		auto const ceiling = std::min(toScreen(lo).y, toScreen(hi).y);
		auto const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		auto const pass = recordPass(scene.front, atlas);
		require(!pass.frameCells().empty(),
			"The tall fixture drew no frame to trim");
		for (auto const& frame : pass.frameCells())
			require(frame.rect.top >= ceiling - 0.01f,
				"A tall frame stood " + std::to_string(ceiling - frame.rect.top)
					+ " pixels above the ceiling it is moulding at "
					+ frame.rect.str());
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.count({ 2, 0 }) == 1 && frames.count({ 2, 1 }) == 1
				&& frames.count({ 2, 2 }) == 1,
			"The tall fixture did not frame three levels, so there is no head "
			"cell to check the trim on: " + tileList(pass.frameCells()));
		auto const& head = frames.at({ 2, 2 });
		require(near(head.rect.top, ceiling),
			"The head cell was not cut back to the ceiling: its top is at "
				+ std::to_string(head.rect.top) + ", the ceiling is at "
				+ std::to_string(ceiling));
		require(head.rect.bottom - head.rect.top < cellHeight - 0.01f,
			"The head cell is a whole cell tall, so the trim never touched it "
			"and moulding stands above the ceiling");
		// The two levels under the head keep their whole cells.
		require(near(frames.at({ 2, 0 }).rect.bottom - frames.at({ 2, 0 }).rect.top,
				cellHeight)
				&& near(frames.at({ 2, 1 }).rect.bottom
						- frames.at({ 2, 1 }).rect.top, cellHeight),
			"A level below the head was trimmed as well; only the top of the "
			"span meets the ceiling");
		// And the wall tile it lies on is trimmed the same way, so the two
		// land on the same crop rather than one above the other.
		for (auto const& wall : pass.wallCells())
			require(wall.rect.top >= ceiling - 0.01f,
				"A wall tile stood above its own ceiling; the trim cannot be "
				"compared against a wall that is not trimmed either");
	}

	// The tall row does not vary with its wall either. The same 3-level
	// opening in a Room, a Corridor and a Facade draws the same three tiles,
	// because the frame carries the moulding and the wall carries its own
	// look, whatever wall that is.
	void everyWallFramesATallOpeningTheSameWay(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto const drawnRoom = tileList(recordPass(tallRoomScene(3).front, atlas)
			.frameCells());
		auto const drawnCorridor = tileList(
			recordPass(tallCorridorScene(3).front, atlas).frameCells());
		auto const drawnFacade = tileList(
			recordPass(tallFacadeScene(3).front, atlas).frameCells());
		require(drawnRoom == drawnCorridor && drawnCorridor == drawnFacade,
			"A 3-level opening drew a different frame per wall: Room "
			+ drawnRoom + ", Corridor " + drawnCorridor + ", Facade "
			+ drawnFacade);
		require(drawnRoom == "windowframe-BS, windowframe-MS, windowframe-TS",
			"A 3-level opening did not draw the tall triple bottom to top: "
			+ drawnRoom);
	}

	// A Window on a back Layer, seen through a clear Window in the Layer in
	// front, wears its own frame. The frame arrives *through* the aperture
	// recursion - the far wall is drawn under the near Window's clip, and
	// the far Window that is part of that wall frames itself on the same
	// rule a front-wall Window does - rather than being suppressed by the
	// clip it arrives under. That is what keeps a nested aperture reading as
	// a window rather than as a hole punched in the view.
	//
	// Asserted on the far window's cells specifically: the nested frame is
	// the far Window's own one cell, wearing the tile the far Window's own
	// span role calls for, at the far wall's grid position - not merely a
	// quad that happened to draw somewhere down the recursion.
	void aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture(
		smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto const scene = nestedWindowScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const column = scene.sharedColumn;

		// The near Window frames its own three cells under its own clip, so
		// the nested frames below are not the near frames mislabelled.
		auto const nearFrames = gridOf(pass.frameCells(), *scene.front);
		require(nearFrames.size() == 3
				&& tileAt(nearFrames, 1, 0) == "windowframe-SL"
				&& tileAt(nearFrames, 2, 0) == "windowframe-SM"
				&& tileAt(nearFrames, 3, 0) == "windowframe-SR",
			"The near Window did not frame its own span: "
				+ tileList(pass.frameCells()));

		auto const farFrames = gridOf(pass.nestedFrameCells(), *scene.front);
		require(farFrames.count({ column, 0 }) == 1
				&& farFrames.size() == 1,
			"The Window on the back Layer wore "
				+ std::to_string(farFrames.size()) + " frame cells through the "
				"aperture; a 1x1 far Window frames exactly its own one cell, and "
				"nothing else should be framing down this aperture: "
				+ tileList(pass.nestedFrameCells()));
		require(tileAt(farFrames, column, 0) == "windowframe-SS",
			"The far Window's cell wore " + tileAt(farFrames, column, 0)
				+ "; a 1x1 Window frames itself as the block's single-column, "
			"single-level cell whatever Layer it stands on");
		// The column the far frame lands on is one the near opening also
		// frames, and the two tiles disagree because the two openings are
		// different sizes. That is what makes this the far Window's frame and
		// not the near opening's moulding seen from behind: the tile came
		// from the far Window's span, not the near one's.
		require(tileAt(nearFrames, column, 0) == "windowframe-SM"
				&& tileAt(farFrames, column, 0) != tileAt(nearFrames, column, 0),
			"The near and far frames agree on the shared column, so the nested "
			"tile is not distinguishable from the near opening's own: near "
				+ tileAt(nearFrames, column, 0) + ", far "
				+ tileAt(farFrames, column, 0));
		// The far opening is one cell: nothing framed from down the aperture
		// at the columns either side of it, which belong to the near opening
		// alone.
		require(tileAt(farFrames, column - 1, 0) == "<nothing>"
				&& tileAt(farFrames, column + 1, 0) == "<nothing>",
			"Something framed the near opening's end cells from down the "
			"aperture: " + tileList(pass.nestedFrameCells()));

		// The nested frame sits on the far wall's own cell: it spans that
		// cell's full width, stops at that cell's floor line, and is cut
		// back at the top to the near aperture it arrived through.
		auto const& far = farFrames.at({ column, 0 });
		auto const farCell = cellRect(*scene.middle, column, 0);
		auto const nearGlass = glassRect(*scene.nearWindow);
		require(near(far.rect.left, farCell.left)
				&& near(far.rect.right, farCell.right),
			"The nested frame does not span the far Window's own cell "
			"horizontally: frame " + far.rect.str() + ", far cell "
				+ farCell.str());
		require(near(far.rect.bottom, farCell.bottom),
			"The nested frame does not reach the far wall's floor line: frame "
			+ far.rect.str() + ", far cell " + farCell.str());
		require(near(far.rect.top, nearGlass.top) && far.rect.top > farCell.top,
			"The nested frame was not cut back to the near aperture: frame top "
				+ std::to_string(far.rect.top) + ", near glass top "
				+ std::to_string(nearGlass.top) + ", far cell top "
				+ std::to_string(farCell.top));
		// And it is not the near opening's own frame quad at that column:
		// the near frame keeps its whole cell, the nested one does not.
		require(!far.rect.sameAs(nearFrames.at({ column, 0 }).rect),
			"The nested frame has the near frame's rect, so the two cannot be "
			"told apart: nested " + far.rect.str() + ", near "
				+ nearFrames.at({ column, 0 }).rect.str());

		// And it arrived through the aperture: it is drawn under the near
		// Window's clip rather than under the Sector's own, which is the one
		// way a back Layer's artwork reaches the screen at all.
		require(!far.ownClip,
			"The nested frame was drawn under the Sector's own clip, so it did "
			"not come down the aperture recursion");
		require(far.clip.left >= nearGlass.left - 0.01f
				&& far.clip.right <= nearGlass.right + 0.01f
				&& far.clip.top >= nearGlass.top - 0.01f
				&& far.clip.bottom <= nearGlass.bottom + 0.01f,
			"The nested frame was not clipped to the near Window's aperture: "
			"frame clip " + far.clip.str() + ", near glass " + nearGlass.str());
		// The far frame really does lie inside the near aperture rather than
		// wholly behind the near glass, so there is a piece of its moulding
		// to see and the check could not have passed on a quad the clip
		// swallowed whole.
		require(far.rect.right > nearGlass.left + 0.01f
				&& far.rect.left < nearGlass.right - 0.01f,
			"The far frame lies wholly outside the near aperture, so this "
			"fixture proves nothing about a frame arriving through one");

		// It lies over the far wall's own tile, the same way a front frame
		// lies over its wall - the recursion did not put the moulding under
		// the artwork it moulds.
		auto const farWall = pass.farWallCells();
		require(!farWall.empty(),
			"The far wall drew no tile, so 'the frame lies over it' would be "
			"an assertion about nothing");
		require(*pass.firstIndexOf(pass.nestedFrameCells())
				> *pass.lastIndexOf(farWall),
			"The nested frame was drawn before the far wall tile it lies over");
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
		// The wide row and the tall column are carried on the same terms as
		// the single-column tile: overlays the render path can ask for and
		// the surface query never answers.
		for (auto const* frame : { "windowframe-SL", "windowframe-SM",
				"windowframe-SR", "windowframe-TS", "windowframe-MS",
				"windowframe-BS" })
		{
			require(hasSectorTileOverlay(frame),
				std::string{ frame }
					+ " is named by the render path but the installed atlas does "
					"not carry it");
			require(!hasSectorTileSurface(frame),
				std::string{ frame } + " answered the Sector-surface query; a "
				"windowframe is never a Sector surface");
		}
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
	checks.push_back({ "windowframeCells/aTwoWideWindowFramesItsTwoCellsAsOneOpening",
		isolated<aTwoWideWindowFramesItsTwoCellsAsOneOpening> });
	checks.push_back({ "windowframeCells/aThreeWideWindowFramesItsThreeCells",
		isolated<aThreeWideWindowFramesItsThreeCells> });
	checks.push_back({ "windowframeCells/theWideOpeningIsOneContinuousAperture",
		isolated<theWideOpeningIsOneContinuousAperture> });
	checks.push_back({ "windowframeCells/twoWideWindowsFrameTheirOwnCells",
		isolated<twoWideWindowsFrameTheirOwnCells> });
	checks.push_back({ "windowframeCells/theWideFrameIsDrawnLastOnEveryCell",
		isolated<theWideFrameIsDrawnLastOnEveryCell> });
	checks.push_back({ "windowframeCells/everyWallFramesAWideOpeningTheSameWay",
		isolated<everyWallFramesAWideOpeningTheSameWay> });
	checks.push_back({ "windowframeCells/aTwoLevelWindowFramesItsSpanWithAHeadAndASill",
		isolated<aTwoLevelWindowFramesItsSpanWithAHeadAndASill> });
	checks.push_back({ "windowframeCells/aThreeLevelWindowFramesItsThreeLevelsBottomToTop",
		isolated<aThreeLevelWindowFramesItsThreeLevelsBottomToTop> });
	checks.push_back({ "windowframeCells/theTallOpeningIsOneContinuousAperture",
		isolated<theTallOpeningIsOneContinuousAperture> });
	checks.push_back({ "windowframeCells/theTallFrameIsTrimmedAtTheCeiling",
		isolated<theTallFrameIsTrimmedAtTheCeiling> });
	checks.push_back({ "windowframeCells/everyWallFramesATallOpeningTheSameWay",
		isolated<everyWallFramesATallOpeningTheSameWay> });
	checks.push_back({ "windowframeCells/aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture",
		isolated<aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture> });
	checks.push_back({ "windowframeCells/theFrameNameIsTheOpeningAndNothingElse",
		isolated<theFrameNameIsTheOpeningAndNothingElse> });
}
