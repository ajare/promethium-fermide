// A 1x1 Window wears a windowframe over its wall (#1075), a wide Window
// frames its whole span as one opening (#1076), a tall Window frames its
// span with a head across the top and a sill along the bottom (#1077),
// and a Window that is wide *and* tall frames its corners and interior
// edges so the role grid closes at fifteen tiles (#1078).
//
// The tracer bullet for the windowframe series pinned everything the wide,
// tall and corner tickets hang off, in the smallest complete shape the
// feature can take: the atlas carries the frame, the renderer derives it
// from the live Window rather than from anything painted, and it lands
// over the wall's own tile without taking anything away. The wide work
// added the horizontal axis of the role grid; the tall work added the
// vertical one; the corner work fills the wide columns of the tall rows,
// where a cell is an end of the span in both directions at once and
// carries its jamb and its head or sill together. The frame a cell wears
// depends on where that cell sits in the opening's span on both axes, and
// every one of those positions now has a tile - except the all-middle
// cell, which asks for nothing because the middle of a big opening is all
// glass with no moulding on it.
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
//     boundary. The glass tops out 0.5 of a level above the top cell's
//     floor - the same row the single-level band holes at - and never
//     0.3, which is the glass's *height* for one level and not its top;
//   * the hole a frame tile declares covers the glass it frames wherever
//     the two overlap, so no opaque moulding lies over the pane anywhere
//     in the span. That is checked off the recorded quads and the glass
//     rect rather than off a tile name, which is what keeps a tileset edit
//     from putting the head back over the glass unnoticed (#1080);
//   * a span that is wide and tall has corners, and a corner is joined and
//     not butted: the cell that is the span's left end *and* its top
//     carries the left jamb and the head on one tile, at neither the
//     jamb's position alone nor the head's but the corner's, so no
//     moulding stops short where the two meet and no second tile is laid
//     over the first. The interior edges carry one moulding across a
//     whole cell - head, sill, or jamb - and the all-middle cell asks for
//     no tile at all;
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
#include "SectorDrawSheet.h"
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

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

	// The frames the shipped set authors: the whole role grid, fifteen
	// tiles - sixteen cells less the all-middle one, which asks for no
	// tile at all. The single-level row, the single-column column, and the
	// wide columns of the three tall rows where a span has corners and
	// interior edges.
	std::vector<std::string> const kWindowframes{ "windowframe-SS",
		"windowframe-SL", "windowframe-SM", "windowframe-SR",
		"windowframe-TS", "windowframe-MS", "windowframe-BS",
		"windowframe-TL", "windowframe-TM", "windowframe-TR",
		"windowframe-ML", "windowframe-MR",
		"windowframe-BL", "windowframe-BM", "windowframe-BR" };

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
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic windowframe atlas did not come up as the New family");
		return atlas;
	}

	// The Object-side art this file installs, kept beside the install so a
	// quad's tile is read back out of the UVs it sampled rather than taken
	// on trust from a name the recorder kept to itself - the same reason the
	// Sector side resolves a quad by the region it sampled rather than by
	// what the draw asked for.
	ObjectTileset gObjectArt;

	// The Object-side art, each sprite in its own column of its own sheet,
	// so an Object quad is distinguishable from a frame quad by which atlas
	// it sampled and the frame's turn in the order is checkable against it.
	//
	// The three glass tiles are installed at the size the authored set
	// declares them - 32 x 80 px drawn at 0.25 x 0.25 World units - because
	// the scale a quad samples is the whole of what the glass checks read,
	// and it has to come from the tile rather than from a size this fixture
	// made up (#1079).
	//
	// The BoothWindow's own frame and shutter are in here for the same
	// reason the glass is: the check that refuses a windowframe over a
	// hatch has to be able to see that the hatch itself drew, rather than
	// reading an empty pass as a successful exclusion.
	void installObjectArt()
	{
		ObjectTileset art;
		art.height = kCellHeight;
		int column = 0;
		auto const add = [&](char const* name, int width, int height,
			float drawnWidth, float drawnHeight)
		{
			art.sprites.emplace(name, TileArtwork{
				{ column++ * kCellWidth, 0, width, height },
				{ 0.0f, 0.0f }, { drawnWidth, drawnHeight } });
		};
		for (auto const* name : { "glass-clear", "glass-frosted",
			"glass-tinted" })
			add(name, 32, 80, 0.25f, 0.25f);
		add("booth-window-shutter", kCellWidth, kCellHeight, 1.0f, 1.0f);
		add("booth-window-open", kCellWidth, kCellHeight, 1.0f, 1.0f);
		art.width = column * kCellWidth;
		gObjectArt = art;
		setObjectTileset(std::move(art), static_cast<TextureId>(2));
		require(hasObjectTileset(),
			"The synthetic Object atlas did not install, so the glass layer "
			"the frame is supposed to draw over would not exist to check");
	}

	// The Object atlas with one glass Style's tile taken out of it, which is
	// the state a sheet that has not drawn that glass yet leaves the renderer
	// in. Everything else the fixture installed stays.
	void installObjectArtWithout(char const* tile)
	{
		auto art = gObjectArt;
		require(art.sprites.erase(tile) != 0,
			std::string{ "The fixture's Object atlas carries no '" } + tile
			+ "', so taking it out would not be testing a miss");
		gObjectArt = art;
		setObjectTileset(std::move(art), static_cast<TextureId>(2));
		require(!objectTileArtwork(tile),
			std::string{ "'" } + tile + "' still resolved after being erased, so "
			"this is not the missing-tile state the check means to be in");
	}

	// The fixture's Object atlas with the version 1 pane-sized names added
	// beside the repeating tiles - the state a set caught mid-transition is
	// in. Each new tile takes its own column so it stays readable back out
	// of the UVs a quad sampled, and each is drawn at the pane-sized extent
	// the real version 1 art declares (#1083).
	void installObjectArtWithLegacyPaneTiles()
	{
		auto art = gObjectArt;
		int column = 0;
		for (auto const& [name, sprite] : art.sprites)
			column = std::max(column, sprite.region.x + sprite.region.width);
		for (auto const* name : { "window-clear", "window-frosted",
			"window-tinted" })
		{
			art.sprites.emplace(name, TileArtwork{
				{ column, 0, kCellWidth, kCellHeight },
				{ 0.0f, 0.0f }, { 0.8f, 0.3f } });
			column += kCellWidth;
		}
		art.width = column;
		gObjectArt = art;
		setObjectTileset(std::move(art), static_cast<TextureId>(2));
		require(objectTileArtwork("glass-clear")
				&& objectTileArtwork("window-clear"),
			"The fixture's Object atlas does not carry both glass vocabularies, "
			"so the preference checked against it has nothing to prefer");
	}

	// The real version 1 object tileset installed as the Object atlas. This
	// is the state a World selecting one of the custom theme sets -
	// `StationFurnitureAtlas` or `DoctorFemale1Atlas`, both authored on
	// this very file - leaves the renderer in: no `glass-*` tile by any
	// name, and the pane-sized `window-clear` / `-frosted` / `-tinted` art
	// the version 1 required-tile contract still names.
	void installVersionOneObjectArt(smoke::Context const& context)
	{
		auto art = ObjectTileset::load(
			context.fixture("resources/textures/objects.tileset.yaml"));
		require(!art.declaresRenderSemantics,
			"The version 1 object tileset reports declared render semantics, so "
			"installing it would not be testing the version 1 fallback path");
		for (auto const* name : { "glass-clear", "glass-frosted",
			"glass-tinted" })
			require(art.sprites.count(name) == 0, std::string{ "The version 1 "
				"set now carries '" } + name + "', so the pane below would not "
				"be resolving a name the installed set does not have");
		for (auto const* name : { "window-clear", "window-frosted",
			"window-tinted" })
			require(art.sprites.count(name) != 0, std::string{ "The version 1 "
				"set carries no '" } + name + "', so there is no pane-sized "
				"glass art left for the fallback to draw");
		gObjectArt = art;
		setObjectTileset(std::move(art), static_cast<TextureId>(2));
		require(hasObjectTileset(),
			"The version 1 Object atlas did not install, so the pane below has "
			"no Objects artwork to resolve at all");
	}

	// The glass tile one Object UV falls on, by the centre of the range the
	// quad sampled. Each tile owns its own column, so the centre lands on
	// exactly one of them.
	std::string objectTileAt(float centreU, float centreV)
	{
		if (gObjectArt.width <= 0 || gObjectArt.height <= 0) return {};
		auto const w = float(gObjectArt.width), h = float(gObjectArt.height);
		for (auto const& [name, sprite] : gObjectArt.sprites)
		{
			auto const& r = sprite.region;
			if (centreU >= float(r.x) / w && centreU < float(r.x + r.width) / w
					&& centreV >= float(r.y) / h && centreV < float(r.y + r.height) / h)
				return name;
		}
		return {};
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

	// The glass is keyed by the Style alone, and two vocabularies carry it:
	// the version 2 `glass-<style>` tile that repeats across the pane
	// (#1079) and the version 1 `window-<style>` pane-sized art a set with
	// no repeating tile falls back to (#1083). Neither prefix is one any
	// other Object artwork uses - the BoothWindow's frame and shutter are
	// `booth-window-*`, and they draw from the same atlas without being
	// glass - so the prefix still separates a pane's glass from everything
	// else a Window's pass can carry.
	bool isGlassTile(std::string const& name)
	{
		return name.rfind("glass-", 0) == 0 || name.rfind("window-", 0) == 0;
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

		// The glass: the pane's own tiles, drawn from the Objects atlas over
		// the space behind. It is not a Sector tile, and it is drawn inside
		// the aperture rather than under the Sector's own clip. Filtered by
		// name rather than by atlas so the BoothWindow's frame and shutter -
		// which draw from the same atlas and are not glass - cannot be read
		// as a pane that repeated.
		std::vector<DrawnQuad> glass() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.texture == WorldDrawList::Texture::ObjectAtlas
						&& isGlassTile(q.tile))
					out.push_back(q);
			return out;
		}

		// Every ObjectAtlas quad the pass emitted, glass or otherwise.
		std::vector<DrawnQuad> objectQuads() const
		{
			std::vector<DrawnQuad> out;
			for (auto const& q : quads)
				if (q.texture == WorldDrawList::Texture::ObjectAtlas) out.push_back(q);
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
				// Read back out of the UVs rather than taken on trust, so a
				// check can tell clear glass from frosted glass from the
				// BoothWindow's shutter and know that a cut quad still landed
				// inside the tile it means to be sampling.
				q.tile = objectTileAt((q.u0 + q.u1) * 0.5f, (q.v0 + q.v1) * 0.5f);
				require(!q.tile.empty(),
					"An ObjectAtlas quad sampled UVs that fall on no tile in the "
					"installed Object atlas");
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

	// A Room `wide` cells by `tall` levels with a Window of the same size
	// set into it, so the span has corners on both axes and - once it is
	// three or more in each direction - an interior cell that is middle in
	// both. The wall is two cells wider than the opening so a check can
	// see the cells outside the span wearing no frame; the height is the
	// span's own, because a framed cell that stood above the ceiling would
	// be trimmed rather than absent and the two need telling apart.
	Scene squareRoomScene(uint32_t wide, uint32_t tall, uint32_t windowX = 1)
	{
		return buildScene("room", wide + 2, tall, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, wide, tall);
	}

	Scene squareCorridorScene(uint32_t wide, uint32_t tall, uint32_t windowX = 1)
	{
		return buildScene("corridor", wide + 2, tall, windowX, 0,
			CORE_CORRIDOR_HEIGHT, false, wide, tall);
	}

	Scene squareFacadeScene(uint32_t wide, uint32_t tall, uint32_t windowX = 1)
	{
		return buildScene("facade", wide + 2, tall, windowX, 0,
			CORE_ROOM_MAX_HEIGHT, false, wide, tall);
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

	// ------------------------------------------------------------------
	// The frame's hole against the glass it frames.
	//
	// A frame's hole is the only part of it that is not moulding, so the
	// one thing its artwork cannot get wrong is being smaller than the pane
	// it is cut for: every pixel of glass the hole misses is a pixel of
	// opaque moulding laid over the pane. #1080 is exactly that defect -
	// the four `T` tiles were holed at the glass *height* (0.3 of a level)
	// rather than at the glass *top* (0.5 of a level above the top cell's
	// floor), so the head lay over the top 0.2 of a level of glass and read
	// 0.7 of a cell tall instead of 0.5.
	//
	// What pins it is therefore not the tile's name and not a hole value a
	// test restates: it is the quads the draw actually emitted, each one's
	// authored hole read out of the shipped tileset and mapped through the
	// UVs that quad sampled, against the glass rect the Window is drawn to.
	// The rounding rule lets the hole fall short of the glass rect by under
	// one artwork pixel at each edge - it rounds inward on purpose, so the
	// moulding never stands inside the pane - and nothing more, which is
	// what makes a tenth of a pixel of inward rounding pass while a 64px
	// shortfall cannot.
	// ------------------------------------------------------------------

	// The authored hole of every overlay in the shipped set, in artwork
	// pixels from the tile's own top-left. Read off the shipped tileset
	// rather than restated here, so a tileset edit moves this check with it
	// instead of being blessed by it.
	std::map<std::string, std::array<int, 4>> authoredHoles(
		smoke::Context const& context)
	{
		auto const doc = YAML::LoadFile(context.fixture(
			"resources/textures/sectors-new.tileset.yaml").string());
		require(doc["overlays"] && doc["overlays"].IsMap(),
			"The shipped sector tileset carries no `overlays` map to read a "
			"frame hole out of");
		std::map<std::string, std::array<int, 4>> holes;
		for (auto const& entry : doc["overlays"])
		{
			auto const hole = entry.second["hole"];
			if (!hole || !hole.IsSequence() || hole.size() != 4) continue;
			holes[entry.first.as<std::string>()] = std::array<int, 4>{
				hole[0].as<int>(), hole[1].as<int>(),
				hole[2].as<int>(), hole[3].as<int>() };
		}
		require(!holes.empty(), "No shipped overlay declares a `hole`, so a "
			"drawn frame would have nothing to be checked against");
		return holes;
	}

	// The screen rect one frame quad's authored hole lands on.
	//
	// Mapped through the UVs the quad actually sampled rather than through
	// the cell it was asked to fill. The draw insets a tile's sampled range
	// by half a texel at each edge and crops a trimmed top row by starting
	// the sample partway down the tile, and both of those land in these UVs;
	// mapping through them puts the hole where the drawn artwork puts it,
	// which is the only place worth checking.
	ScreenRect holeScreenRect(DrawnQuad const& q, SectorTileset const& atlas,
		std::array<int, 4> const& hole)
	{
		auto const found = atlas.overlays.find(q.tile);
		require(found != atlas.overlays.end(), "A frame quad names '" + q.tile
			+ "', which the installed atlas does not carry as an overlay");
		auto const& region = found->second;
		require(atlas.width > 0 && atlas.height > 0 && region.width > 1
				&& region.height > 1,
			"The installed frame region is too small to map a hole through");
		auto const w = float(atlas.width), h = float(atlas.height);
		// Undo the half-texel inset to get back to the 0..1 fraction of
		// *this tile* each recorded UV sits at.
		auto const tileU = [&](float uv)
		{
			return (uv * w - (float(region.x) + 0.5f)) / float(region.width - 1);
		};
		auto const tileV = [&](float vv)
		{
			return (vv * h - (float(region.y) + 0.5f)) / float(region.height - 1);
		};
		auto const u0 = tileU(q.u0), u1 = tileU(q.u1);
		auto const v0 = tileV(q.v0), v1 = tileV(q.v1);
		require(u1 > u0 && v1 > v0, "A '" + q.tile
			+ "' frame quad sampled no range of its tile, so its hole cannot be "
			"placed anywhere");
		auto const sx = (q.rect.right - q.rect.left) / (u1 - u0);
		auto const sy = (q.rect.bottom - q.rect.top) / (v1 - v0);
		auto const map = [&](float fu, float fv)
		{
			return pf::render::Vec2{ q.rect.left + (fu - u0) * sx,
				q.rect.top + (fv - v0) * sy };
		};
		ScreenRect rect;
		rect.include(map(float(hole[0]) / float(region.width),
			float(hole[1]) / float(region.height)));
		rect.include(map(float(hole[0] + hole[2]) / float(region.width),
			float(hole[1] + hole[3]) / float(region.height)));
		return rect;
	}

	// Requires that no moulding of any frame quad in the pass lies over
	// glass. Each quad is checked against the part of the glass rect it
	// covers at all: glass outside that quad's own rect belongs to whichever
	// quad covers it, or to no frame, and is that quad's business.
	void requireNoMouldingOverGlass(Pass const& pass, SectorTileset const& atlas,
		std::map<std::string, std::array<int, 4>> const& holes,
		ScreenRect const& glass, std::string const& label)
	{
		auto const frames = pass.frameCells();
		require(!frames.empty(), label + " drew no frame to check");
		// The hole is rounded inward on every edge, so it may fall short of
		// the glass by under one artwork pixel per edge. Every fixture here
		// pins the zoom at 1, where one artwork pixel is one screen pixel.
		float const roundingSlack = 1.0f;
		size_t covering{};
		for (auto const& q : frames)
		{
			auto const found = holes.find(q.tile);
			require(found != holes.end(), "A frame quad names '" + q.tile
				+ "', which the shipped tileset declares no hole for");
			ScreenRect covered;
			covered.left = std::max(glass.left, q.rect.left);
			covered.right = std::min(glass.right, q.rect.right);
			covered.top = std::max(glass.top, q.rect.top);
			covered.bottom = std::min(glass.bottom, q.rect.bottom);
			if (covered.left >= covered.right || covered.top >= covered.bottom)
				continue;
			++covering;
			auto const hole = holeScreenRect(q, atlas, found->second);
			require(hole.left <= covered.left + roundingSlack
					&& hole.right >= covered.right - roundingSlack
					&& hole.top <= covered.top + roundingSlack
					&& hole.bottom >= covered.bottom - roundingSlack,
				label + ": the '" + q.tile + "' frame lays moulding over glass - "
					"the glass it covers spans " + covered.str()
					+ " and its hole only " + hole.str());
		}
		require(covering > 0, label + " put no frame quad over any glass, so "
			"there was nothing here to check");
	}

	// The frame's hole covers the glass it frames in every cell of the
	// span, for every shape of span the role grid answers: the 1x1, the wide
	// row, the tall column and the cornered span. The tall column is where
	// the defect lived, and the cornered span is where the same dropped 0.2
	// would hide a second time on the `TL`/`TM`/`TR` row.
	void theFrameHoleCoversTheGlassItFrames(smoke::Context const& context)
	{
		auto const holes = authoredHoles(context);
		auto atlas = installAtlas(true);
		struct Span
		{
			std::string label;
			Scene scene;
		};
		std::vector<Span> spans{
			{ "a 1x1 opening", roomScene() },
			{ "a 2-cell wide opening", wideRoomScene(2) },
			{ "a 2-level tall opening", tallRoomScene(2) },
			{ "a 3-level tall opening", tallRoomScene(3) },
			{ "a 3x3 cornered opening", squareRoomScene(3, 3) } };
		for (auto& span : spans)
		{
			span.scene.refresh();
			require(span.scene.window != nullptr, span.label
				+ " has no Window to frame");
			requireNoMouldingOverGlass(recordPass(span.scene.front, atlas),
				atlas, holes, glassRect(*span.scene.window), span.label);
		}
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

	// A 2x2 Window frames its four cells as one opening with corners: the
	// bottom row wears `windowframe-BL` and `windowframe-BR`, the top row
	// `windowframe-TL` and `windowframe-TR`. Each corner carries both its
	// jamb and its head or sill, and carries them as one tile rather than
	// as the edge tile of one axis with the other axis's tile laid over it:
	// the cell that is the span's left end and its top wears a tile that
	// is neither `BM` nor `ML` but the corner between them.
	void aTwoByTwoWindowFramesItsCornersTogether(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = squareRoomScene(2, 2);
		require(scene.window->getCellsWide() == 2
				&& scene.window->getLevelsHigh() == 2,
			"The fixture Window is not 2x2");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 4, "A 2x2 Window drew "
			+ std::to_string(frames.size()) + " frame cells; a span with "
			"corners is framed on all four of its cells");
		require(tileAt(frames, 1, 0) == "windowframe-BL",
			"The bottom-left cell of a 2x2 Window wore " + tileAt(frames, 1, 0)
				+ "; a cell that is the span's left end and its bottom carries "
				"the jamb and the sill together, on one tile");
		require(tileAt(frames, 2, 0) == "windowframe-BR",
			"The bottom-right cell of a 2x2 Window wore " + tileAt(frames, 2, 0));
		require(tileAt(frames, 1, 1) == "windowframe-TL",
			"The top-left cell of a 2x2 Window wore " + tileAt(frames, 1, 1)
				+ "; a cell that is the span's left end and its top carries the "
				"jamb and the head together, on one tile");
		require(tileAt(frames, 2, 1) == "windowframe-TR",
			"The top-right cell of a 2x2 Window wore " + tileAt(frames, 2, 1));
		require(tileAt(frames, 0, 0) == "<nothing>"
				&& tileAt(frames, 3, 0) == "<nothing>"
				&& tileAt(frames, 0, 1) == "<nothing>"
				&& tileAt(frames, 3, 1) == "<nothing>",
			"A cell outside the 2x2 span wore a frame: "
				+ tileList(pass.frameCells()));
		// The corner is its own tile and not one of the two edge roles it
		// combines. Had the derivation asked for the horizontal role alone
		// the corner would have worn `BM`, and the vertical alone `ML`; both
		// would put moulding where the corner's is not, and neither says
		// what the cell is.
		require(tileAt(frames, 1, 0) != "windowframe-BM"
				&& tileAt(frames, 1, 0) != "windowframe-ML",
			"The bottom-left corner wore an edge tile rather than the corner "
			"tile: " + tileAt(frames, 1, 0));
		require(tileAt(frames, 1, 1) != "windowframe-TM"
				&& tileAt(frames, 1, 1) != "windowframe-ML",
			"The top-left corner wore an edge tile rather than the corner "
			"tile: " + tileAt(frames, 1, 1));
	}

	// A 3x3 Window frames its whole edge with the correct corner and edge
	// roles on every cell: corners at the four ends of both axes, head and
	// sill across the top and bottom middles, jambs down the left and right
	// middles, and nothing at all in the middle.
	void aThreeByThreeWindowFramesItsWholeEdge(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = squareRoomScene(3, 3);
		require(scene.window->getCellsWide() == 3
				&& scene.window->getLevelsHigh() == 3,
			"The fixture Window is not 3x3");
		auto const pass = recordPass(scene.front, atlas);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.size() == 8, "A 3x3 Window drew "
			+ std::to_string(frames.size()) + " frame cells; the whole edge of "
			"a 3x3 span is framed - eight cells - and the ninth asks for "
			"nothing");

		struct Expectation
		{
			uint32_t column{};
			uint32_t level{};
			std::string tile;
		};
		std::vector<Expectation> const edge{
			{ 1, 0, "windowframe-BL" }, { 2, 0, "windowframe-BM" },
			{ 3, 0, "windowframe-BR" },
			{ 1, 1, "windowframe-ML" }, { 3, 1, "windowframe-MR" },
			{ 1, 2, "windowframe-TL" }, { 2, 2, "windowframe-TM" },
			{ 3, 2, "windowframe-TR" } };
		for (auto const& want : edge)
			require(tileAt(frames, want.column, want.level) == want.tile,
				"The cell at column " + std::to_string(want.column) + ", level "
					+ std::to_string(want.level) + " of a 3x3 Window wore "
					+ tileAt(frames, want.column, want.level) + ", expected "
					+ want.tile);
		// Nothing outside the span framed.
		for (uint32_t column = 0; column < 5; ++column)
			for (uint32_t level = 0; level < 3; ++level)
				if (column < 1 || column > 3)
					require(tileAt(frames, column, level) == "<nothing>",
						"A cell outside the 3x3 span wore a frame at column "
							+ std::to_string(column) + ", level "
							+ std::to_string(level) + ": "
							+ tileAt(frames, column, level));
	}

	// The all-middle cell of a 3x3 Window asks for no tile at all and
	// emits nothing. It is not answered by a transparent tile: there is no
	// quad at that cell in the frame pass, so nothing is sampled, nothing
	// is blended, and the wall's own tile underneath is all that lies
	// there. A transparent tile would have been a tile - authored,
	// installed, sampled, drawn - standing in for the absence of one, and
	// the absence is the point: the middle of a big opening is all glass
	// with no moulding on it.
	void theAllMiddleCellOfA3x3AsksForNoTileAndEmitsNothing(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = squareRoomScene(3, 3);
		auto const pass = recordPass(scene.front, atlas);

		// The name the derivation gives the middle cell is empty, and the
		// atlas is never even asked to resolve it into a tile.
		require(windowframeTileName(3, 3, 1, 1).empty(),
			"The all-middle cell of a 3x3 asked for a tile: "
				+ windowframeTileName(3, 3, 1, 1));
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(frames.count({ 2, 1 }) == 0,
			"The all-middle cell of a 3x3 drew a frame: "
				+ tileAt(frames, 2, 1));
		// And nothing but the wall's own tile drew that whole cell. The
		// wall is there because the wall keeps its tile whatever stands in
		// it, and the glass is there over the space behind; what is not
		// there is any moulding tile - not a transparent one, not a
		// placeholder, nothing.
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		require(wall.count({ 2, 1 }) == 1
				&& wall.at({ 2, 1 }).tile == "room",
			"The wall under the all-middle cell stopped drawing its own tile, "
			"so the absence of a frame there would be an absence of the wall "
			"rather than an absence of moulding");
		require(!pass.glass().empty(),
			"The 3x3 fixture drew no glass, so 'the middle is all glass' "
			"would be an assertion about nothing");
		auto const middleCell = cellRect(*scene.front, 2, 1);
		for (auto const& q : pass.quads)
			require(!(q.ownClip && q.texture == WorldDrawList::Texture::SectorAtlas
					&& q.rect.sameAs(middleCell) && q.tile != "room"),
				"Something other than the wall's own tile drew a whole Sector "
				"cell at the all-middle position: " + q.tile);
	}

	// The interior cell's empty answer is the same answer a missing tile
	// gets, and neither produces the missing-art colour. The all-middle cell
	// and a cell whose frame the atlas does not carry are two different
	// facts - one is "there is no moulding here", the other is "the
	// moulding for this cell has not been drawn yet" - and they arrive at
	// the same place in the draw: no tile asked for, nothing emitted, no
	// magenta. That is what lets the grid be filled one tile at a time and
	// what keeps a World drawn against an older atlas drawing the wall it
	// always drew rather than a hole.
	void theInteriorCellsEmptyAnswerIsTheMissingTilesAnswer(smoke::Context const&)
	{
		// With the whole grid installed, the middle cell asks for nothing.
		auto atlas = installAtlas(true);
		auto scene = squareRoomScene(3, 3);
		auto const pass = recordPass(scene.front, atlas);
		require(pass.frameCells().size() == 8,
			"A 3x3 over the full grid drew "
			+ std::to_string(pass.frameCells().size()) + " frames, not 8");
		require(gridOf(pass.frameCells(), *scene.front).count({ 2, 1 }) == 0,
			"The all-middle cell drew a tile over the full grid");
		for (auto const& q : pass.quads)
			require(q.colour != kMagenta,
				"A cell drew the missing-art colour with the whole role grid "
				"installed; the all-middle cell asks for no tile and that is "
				"not a missing tile");

		// With an atlas that carries no frames at all, every cell of the
		// same span gets the same answer - nothing drawn, no magenta - and
		// the wall stays whole. The two answers are indistinguishable on
		// the wire, which is the point.
		auto bare = installAtlas(false);
		auto const barePass = recordPass(scene.front, bare);
		require(barePass.frameCells().empty(),
			"A frame drew from an atlas that authors none: "
			+ tileList(barePass.frameCells()));
		for (auto const& q : barePass.quads)
			require(q.colour != kMagenta,
				"A missing frame drew the missing-art colour; a frame that was "
				"never authored is no frame, not a hole");
		require(gridOf(barePass.wallCells(), *scene.front).size()
				== gridOf(pass.wallCells(), *scene.front).size(),
			"Taking the frames away took a wall cell with it; the wall is "
			"drawn the same whether or not a frame lies over it");
		// And the empty answer is the answer the resolver gives, not an
		// answer the loop happens to skip: asked for the all-middle cell
		// and for a name the atlas does not carry, it gives the same empty
		// string and the atlas answers neither.
		require(windowframeTileName(3, 3, 1, 1).empty()
				&& !hasSectorTileOverlay(drawingSectorSheet(), windowframeTileName(3, 3, 1, 1)),
			"The all-middle cell's empty name resolved to a tile the loop "
			"could have drawn");
		require(!hasSectorTileOverlay(drawingSectorSheet(), "windowframe-ZZ"),
			"A name the atlas does not carry resolved as though it did");
	}

	// Every frame of a span with corners still lies over the wall's own
	// tile and is still drawn last - on every cell of the span, not only
	// on the ones a single-level window would have had. A 3x3 puts its
	// wall's tile down under all nine cells, then the space behind, then
	// the glass, then the eight frame tiles, so the moulding reads as the
	// edge of the opening at every point of the edge and the corner cells
	// are covered by the same pass that covers the middles.
	void theCornerFramesAreDrawnLastOverTheWallTileOnEveryCell(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = squareRoomScene(3, 3);
		auto const pass = recordPass(scene.front, atlas);
		auto const wall = gridOf(pass.wallCells(), *scene.front);
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		require(pass.frameCells().size() == 8 && pass.wallCells().size() == 15,
			"The 3x3 fixture's draw was missing a layer: wall "
			+ std::to_string(pass.wallCells().size()) + ", frames "
			+ std::to_string(pass.frameCells().size()));
		auto const lastWall = *pass.lastIndexOf(pass.wallCells());
		auto const lastBehind = *pass.lastIndexOf(pass.behindCells());
		auto const lastGlass = *pass.lastIndexOf(pass.glass());
		for (uint32_t column = 1; column <= 3; ++column)
			for (uint32_t level = 0; level < 3; ++level)
			{
				if (column == 2 && level == 1) continue;	// asks for nothing
				require(frames.count({ column, level }) == 1,
					"The cell at column " + std::to_string(column) + ", level "
						+ std::to_string(level) + " of the span drew no frame");
				require(wall.count({ column, level }) == 1,
					"The cell at column " + std::to_string(column) + ", level "
						+ std::to_string(level) + " drew no wall tile");
				auto const& over = frames.at({ column, level });
				auto const& under = wall.at({ column, level });
				require(over.index > lastWall,
					"The frame at column " + std::to_string(column) + ", level "
						+ std::to_string(level)
						+ " was drawn before the wall tile it lies over");
				require(over.index > lastBehind,
					"The frame at column " + std::to_string(column) + ", level "
						+ std::to_string(level)
						+ " was drawn before the space behind");
				require(over.index > lastGlass,
					"The frame at column " + std::to_string(column) + ", level "
						+ std::to_string(level)
						+ " was drawn before the glass it covers the seam of");
				require(near(over.rect.left, under.rect.left)
						&& near(over.rect.right, under.rect.right),
					"The frame at column " + std::to_string(column) + ", level "
						+ std::to_string(level) + " is not on the cell its wall "
						"tile is: wall " + under.rect.str() + ", frame "
						+ over.rect.str());
				require(under.tile == "room",
					"The wall under a cornered span stopped drawing its own tile");
			}
	}

	// The corner frame is trimmed at the ceiling too. A Room standing 0.9
	// of a cell per level has a ceiling that cuts across the cell's top,
	// and the trim lands on the whole top *row* of a wide tall span - both
	// corners and the head between them - while the levels under them keep
	// their whole cells. Only the top of the span meets the ceiling, but
	// the top of the span is a row once the span is wide.
	void theCornerFrameIsTrimmedAtTheCeilingOnTheTopRow(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto scene = squareRoomScene(3, 3, 1);
		core::Vector2 lo{}, hi{};
		scene.front->getBounds(lo, hi);
		auto const ceiling = std::min(toScreen(lo).y, toScreen(hi).y);
		auto const cellHeight = CORE_LEVEL_HEIGHT_PIXELS * gUISettings.worldZoom;

		auto const pass = recordPass(scene.front, atlas);
		require(!pass.frameCells().empty(),
			"The cornered fixture drew no frame to trim");
		for (auto const& frame : pass.frameCells())
			require(frame.rect.top >= ceiling - 0.01f,
				"A frame stood " + std::to_string(ceiling - frame.rect.top)
					+ " pixels above the ceiling it is moulding at "
					+ frame.rect.str());
		auto const frames = gridOf(pass.frameCells(), *scene.front);
		for (uint32_t column = 1; column <= 3; ++column)
		{
			require(frames.count({ column, 2 }) == 1,
				"The top row of the span lost a cell, so there is no frame to "
				"check the trim on at column " + std::to_string(column));
			auto const& top = frames.at({ column, 2 });
			require(near(top.rect.top, ceiling),
				"The top-row frame at column " + std::to_string(column)
					+ " was not cut back to the ceiling: its top is at "
					+ std::to_string(top.rect.top) + ", the ceiling is at "
					+ std::to_string(ceiling));
			require(top.rect.bottom - top.rect.top < cellHeight - 0.01f,
				"The top-row frame at column " + std::to_string(column)
					+ " is a whole cell tall, so the trim never touched it and "
					"moulding stands above the ceiling");
		}
		// The two levels under the top row keep their whole cells.
		for (uint32_t level = 0; level < 2; ++level)
			for (uint32_t column = 1; column <= 3; ++column)
			{
				if (column == 2 && level == 1) continue;
				require(near(frames.at({ column, level }).rect.bottom
						- frames.at({ column, level }).rect.top, cellHeight),
					"A level under the top row was trimmed as well; only the top "
					"of the span meets the ceiling");
			}
		// And the wall tiles it lies on are trimmed the same way.
		for (auto const& wall : pass.wallCells())
			require(wall.rect.top >= ceiling - 0.01f,
				"A wall tile stood above its own ceiling; the trim cannot be "
				"compared against a wall that is not trimmed either");
	}

	// The same frame names draw in a Room, a Corridor and a Facade alike,
	// for a span with corners as for every other span. The corner tiles
	// carry moulding and nothing else, and the moulding does not know a
	// Room from a Corridor from a Facade - so a 2x2 opening draws the same
	// four names in all three, and no wall-keyed frame name draws anywhere.
	void everyWallFramesACorneredOpeningTheSameWay(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		auto const drawnRoom = tileList(recordPass(squareRoomScene(2, 2).front,
			atlas).frameCells());
		auto const drawnCorridor = tileList(recordPass(
			squareCorridorScene(2, 2).front, atlas).frameCells());
		auto const drawnFacade = tileList(recordPass(
			squareFacadeScene(2, 2).front, atlas).frameCells());
		require(drawnRoom == drawnCorridor && drawnCorridor == drawnFacade,
			"A 2x2 opening drew a different frame per wall: Room " + drawnRoom
				+ ", Corridor " + drawnCorridor + ", Facade " + drawnFacade);
		require(drawnRoom == "windowframe-BL, windowframe-BR, windowframe-TL, "
				"windowframe-TR",
			"A 2x2 opening did not draw its four corners bottom-up, left to "
			"right: " + drawnRoom);
		// No wall-keyed frame name draws anywhere across all three walls.
		for (auto const* drawn : { &drawnRoom, &drawnCorridor, &drawnFacade })
			for (auto const* wall : { "room", "corridor", "facade" })
				require(drawn->find(wall) == std::string::npos,
					"A drawn frame name carries the wall '" + std::string{ wall }
						+ "'; a frame laid over the wall's own tile does not "
						"vary with the wall: " + *drawn);
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
		auto const drawn = tileList(pass.objectQuads());
		require(pass.objectQuads().size() >= 2
				&& drawn.find("booth-window-shutter") != std::string::npos
				&& drawn.find("booth-window-open") != std::string::npos,
			"The BoothWindow drew neither its own frame nor its shutter - it drew "
			+ drawn
			+ ", so the absence of a windowframe here would prove nothing");
		// And the glass repeat stays off the hatch: the pane the tile repeats
		// across is the ordinary Window's, and a shutter that also wore
		// repeated glass would be drawn over by its own travelling panel.
		require(pass.glass().empty(),
			"A BoothWindow wore repeated glass behind the shutter that slides "
			"over it: " + tileList(pass.glass()));
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
		require(windowframeTileName(3, 3, 2, 1) == "windowframe-MR",
			"3x3 middle-right: rightmost column, middle level");
		require(windowframeTileName(2, 2, 0, 0) == "windowframe-BL",
			"2x2 bottom-left corner: leftmost column, bottom of the span");
		require(windowframeTileName(2, 2, 1, 0) == "windowframe-BR",
			"2x2 bottom-right corner: rightmost column, bottom of the span");
		require(windowframeTileName(2, 2, 0, 1) == "windowframe-TL",
			"2x2 top-left corner: leftmost column, top of the span");
		require(windowframeTileName(2, 2, 1, 1) == "windowframe-TR",
			"2x2 top-right corner: rightmost column, top of the span");
		require(windowframeTileName(3, 2, 1, 1) == "windowframe-TM",
			"3x2 top-middle: middle column, top of the span - the head alone");
		require(windowframeTileName(3, 3, 1, 0) == "windowframe-BM",
			"3x3 bottom-middle: middle column, bottom of the span - the sill "
			"alone");
		// The all-middle cell asks for no tile rather than wearing a
		// transparent one: a span that is middle in both directions is all
		// glass with no moulding on it, and the empty name is the same
		// answer a missing tile gives.
		require(windowframeTileName(3, 3, 1, 1).empty(),
			"The all-middle cell of a 3x3 asked for a tile; it is all glass "
			"with no moulding and asks for nothing");
		require(windowframeTileName(4, 4, 1, 1).empty()
				&& windowframeTileName(4, 4, 2, 2).empty(),
			"An all-middle cell of a bigger span asked for a tile; every span "
			"has exactly one cell per axis that is middle in both, and it asks "
			"for nothing whatever the span's size");

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

		require(hasSectorTileOverlay(drawingSectorSheet(), "windowframe-SS"),
			"windowframe-SS is named by the render path but the installed "
			"atlas does not carry it");
		require(!hasSectorTileSurface(drawingSectorSheet(), "windowframe-SS"),
			"A windowframe answered the Sector-surface query; it is a tile of "
			"the atlas but not a surface a Sector is authored with");
		// The whole role grid is carried on the same terms as the
		// single-column tile: overlays the render path can ask for and the
		// surface query never answers. All fifteen, because the grid is
		// filled - every cell of every span has its tile, and the only cell
		// with none is the all-middle one, which asks for nothing.
		for (auto const* frame : { "windowframe-SS", "windowframe-SL",
				"windowframe-SM", "windowframe-SR", "windowframe-TS",
				"windowframe-MS", "windowframe-BS", "windowframe-TL",
				"windowframe-TM", "windowframe-TR", "windowframe-ML",
				"windowframe-MR", "windowframe-BL", "windowframe-BM",
				"windowframe-BR" })
		{
			require(hasSectorTileOverlay(drawingSectorSheet(), frame),
				std::string{ frame }
					+ " is named by the render path but the installed atlas does "
					"not carry it");
			require(!hasSectorTileSurface(drawingSectorSheet(), frame),
				std::string{ frame } + " answered the Sector-surface query; a "
				"windowframe is never a Sector surface");
		}
		// And the derivation never produces a name the grid does not have.
		// Every cell of every span up to 7x4 - wider and taller than any
		// opening the grid was sized for - answers one of the fifteen names
		// or the empty string, and nothing else. That is what "the grid
		// closes here" means on the derivation side: no bigger opening
		// needs a tile that is not already drawn.
		std::set<std::string> const grid{ "windowframe-SS", "windowframe-SL",
			"windowframe-SM", "windowframe-SR", "windowframe-TS",
			"windowframe-TL", "windowframe-TM", "windowframe-TR",
			"windowframe-MS", "windowframe-ML", "windowframe-MR",
			"windowframe-BS", "windowframe-BL", "windowframe-BM",
			"windowframe-BR" };
		require(grid.size() == 15,
			"The role grid states " + std::to_string(grid.size())
				+ " tiles; it closes at fifteen, sixteen cells less the "
				"all-middle one");
		for (uint32_t wide = 1; wide <= 7; ++wide)
			for (uint32_t high = 1; high <= 4; ++high)
				for (uint32_t column = 0; column < wide; ++column)
					for (uint32_t level = 0; level < high; ++level)
					{
						auto const name = windowframeTileName(wide, high, column,
							level);
						if (name.empty()) continue;
						require(grid.count(name) == 1,
							"A span " + std::to_string(wide) + "x"
								+ std::to_string(high) + " asked for '" + name
								+ "', which is not one of the fifteen tiles the "
								"role grid closes at");
						require(hasSectorTileOverlay(drawingSectorSheet(), name),
							"A span asked for '" + name
								+ "' and the installed atlas does not carry it");
					}
	}

	// ------------------------------------------------------------------
	// The glass: one tile per glass Style, repeated across the pane at the
	// scale it was authored instead of one pane-sized image stretched over
	// whatever rectangle the Window happens to be (#1079).
	//
	// The thing worth pinning is that the *scale* a tile samples does not
	// change when the pane changes size. That is the entire defect, and it is
	// directly readable off the recorded quads; pane size, repeat count and
	// cut fractions all follow from it.
	// ------------------------------------------------------------------

	// The glass tile's screen extent at the current zoom. 0.25 World units
	// is 32 px across and 80 px up on the 128 x 320 cell, which is what
	// makes the tile square in World space while keeping one artwork pixel
	// to one screen pixel on both axes at once.
	float glassTileScreenWidth()
	{
		return 0.25f * float(kCellWidth) * gUISettings.worldZoom;
	}

	float glassTileScreenHeight()
	{
		return 0.25f * float(kCellHeight) * gUISettings.worldZoom;
	}

	bool close(float a, float b, float tolerance)
	{
		return std::abs(a - b) <= tolerance;
	}

	// The artwork pixels a quad sampled out of the tile it drew, across and
	// down. The sprite helper insets a tile's sampled range by half a pixel
	// at each edge so a quad never samples its neighbour's border, so a
	// whole tile measures one pixel short of its region on each axis. Every
	// tolerance used with these is that inset plus slack, and it stays an
	// order of magnitude tighter than the stretch this ticket replaces: a
	// pane-sized tile stretched over a two-level pane spread its whole 102
	// px of artwork across 1.3 World units, 78 px per unit against the 128
	// the glass is authored at.
	void sampledArtworkPixels(DrawnQuad const& q, float& across, float& down)
	{
		auto const art = objectTileArtwork(q.tile);
		require(!!art, "A glass quad names '" + q.tile
			+ "', which the installed Object atlas does not carry");
		across = (q.u1 - q.u0) * float(gObjectArt.width);
		down = (q.v1 - q.v0) * float(gObjectArt.height);
	}

	// The artwork pixels per World unit a quad samples, on each axis. This
	// is the number the whole ticket turns on: it is a fact about the tile
	// and the transform, and it cannot move when the pane it happens to be
	// filling changes size.
	void sampledScale(DrawnQuad const& q, float& perUnitX, float& perUnitY)
	{
		float across{}, down{};
		sampledArtworkPixels(q, across, down);
		auto const worldX = (q.rect.right - q.rect.left)
			/ (float(kCellWidth) * gUISettings.worldZoom);
		auto const worldY = (q.rect.bottom - q.rect.top)
			/ (float(kCellHeight) * gUISettings.worldZoom);
		require(worldX > 0.0f && worldY > 0.0f,
			"A glass quad was drawn with no extent, so the scale it samples "
			"says nothing about anything");
		perUnitX = across / worldX;
		perUnitY = down / worldY;
	}

	// Lays the recorded glass into the column/row grid the repeat built,
	// anchored at the pane's bottom-left, and requires it to cover the pane
	// exactly: every cell filled, every edge landing either on the tile
	// boundary or on the pane's own edge, and nothing outside the pane at
	// all. A gap would be an unglazed strip; an overshoot would be glass on
	// the wall.
	std::map<std::pair<int, int>, DrawnQuad> layoutGlass(
		std::vector<DrawnQuad> const& glass, ScreenRect const& pane,
		int& columns, int& rows)
	{
		auto const tileW = glassTileScreenWidth();
		auto const tileH = glassTileScreenHeight();
		columns = int(std::ceil((pane.right - pane.left) / tileW - 1e-4f));
		rows = int(std::ceil((pane.bottom - pane.top) / tileH - 1e-4f));
		std::map<std::pair<int, int>, DrawnQuad> grid;
		for (auto const& q : glass)
		{
			auto const column = int(std::floor(
				(q.rect.left - pane.left) / tileW + 1e-4f));
			auto const row = int(std::floor(
				(pane.bottom - q.rect.bottom) / tileH + 1e-4f));
			require(column >= 0 && column < columns, "A glass quad sat outside "
				"the column range the pane's width describes at " + q.rect.str());
			require(row >= 0 && row < rows, "A glass quad sat outside the row "
				"range the pane's height describes at " + q.rect.str());
			require(grid.find(std::make_pair(column, row)) == grid.end(),
				"Two glass quads landed on column " + std::to_string(column)
					+ ", row " + std::to_string(row) + ", so the repeat overlaps "
					"itself");
			auto const left = pane.left + float(column) * tileW;
			auto const bottom = pane.bottom - float(row) * tileH;
			require(near(q.rect.left, left) && near(q.rect.bottom, bottom),
				"A glass quad is off the grid the repeat builds from the pane's "
				"bottom-left: quad " + q.rect.str() + " expected at "
				+ std::to_string(left) + ", " + std::to_string(bottom));
			require(near(q.rect.right, std::min(left + tileW, pane.right)),
				"A glass quad's right edge is neither a tile boundary nor the "
				"pane's edge: " + q.rect.str());
			require(near(q.rect.top, std::max(bottom - tileH, pane.top)),
				"A glass quad's top edge is neither a tile boundary nor the "
				"pane's edge: " + q.rect.str());
			grid.emplace(std::make_pair(column, row), q);
		}
		require(int(grid.size()) == columns * rows, "The repeat covered "
			+ std::to_string(grid.size()) + " of the " + std::to_string(columns)
			+ " x " + std::to_string(rows) + " cells the pane needs, so a strip "
			"of the aperture is left unglazed");
		return grid;
	}

	// The fraction of its tile a quad sampled, on each axis.
	void sampledTileFraction(DrawnQuad const& q, float& across, float& down)
	{
		float pixelsX{}, pixelsY{};
		sampledArtworkPixels(q, pixelsX, pixelsY);
		auto const art = objectTileArtwork(q.tile);
		require(!!art, "The glass tile vanished from the atlas");
		across = pixelsX / float(art->region.width);
		down = pixelsY / float(art->region.height);
	}

	// A 1x1 pane is 0.8 x 0.3 World units against a 0.25 x 0.25 tile: four
	// columns and two rows, the last of each cut. The cut tile is drawn at
	// its remaining size sampling only the matching fraction of the tile,
	// so the pattern never squashes and the cut reads as a cut rather than
	// as a whole tile squeezed into the sliver that was left.
	void aOneByOnePaneRepeatsItsTileWithTheLastColumnAndRowCut(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = roomScene();
		auto const pane = glassRect(*scene.window);
		auto const pass = recordPass(scene.front, atlas);
		auto const glass = pass.glass();
		require(!glass.empty(), "The 1x1 pane drew no glass at all");

		int columns{}, rows{};
		auto const grid = layoutGlass(glass, pane, columns, rows);
		require(columns == 4 && rows == 2, "A 1x1 pane repeated its tile "
			+ std::to_string(columns) + " x " + std::to_string(rows)
			+ "; a 0.8 x 0.3 World-unit pane against a 0.25 x 0.25 tile is "
			"four columns and two rows");

		auto const tileW = glassTileScreenWidth();
		auto const tileH = glassTileScreenHeight();
		for (auto const& [where, q] : grid)
		{
			bool const cutColumn = where.first + 1 == columns;
			bool const cutRow = where.second + 1 == rows;
			auto const widthFraction = (q.rect.right - q.rect.left) / tileW;
			auto const heightFraction = (q.rect.bottom - q.rect.top) / tileH;
			float sampledX{}, sampledY{};
			sampledTileFraction(q, sampledX, sampledY);

			// What the quad drew and what it sampled are the same fraction of
			// the tile. A quad that filled the same rect while sampling the
			// whole tile would be stretching, which is the defect.
			require(close(sampledX, widthFraction, 0.05f)
					&& close(sampledY, heightFraction, 0.05f),
				"A glass quad fills " + std::to_string(widthFraction)
					+ " x " + std::to_string(heightFraction)
					+ " of the tile's extent while sampling " + std::to_string(sampledX)
					+ " x " + std::to_string(sampledY)
					+ " of its artwork, so the tile is being stretched rather "
					"than cut: " + q.rect.str());

			if (cutColumn)
				require(widthFraction < 0.9f && sampledX < 0.9f,
					"The pane's last column is not cut - it fills "
					+ std::to_string(widthFraction)
					+ " of the tile's width and samples "
					+ std::to_string(sampledX)
					+ ", so a partial tile is being drawn as a whole one");
			else
				require(widthFraction > 0.99f,
					"A column that is not the pane's last was drawn short: "
					+ std::to_string(widthFraction));

			if (cutRow)
				require(heightFraction < 0.9f && sampledY < 0.9f,
					"The pane's last row is not cut - it fills "
					+ std::to_string(heightFraction)
					+ " of the tile's height and samples "
					+ std::to_string(sampledY)
					+ ", so a partial tile is being drawn as a whole one");
			else
				require(heightFraction > 0.99f,
					"A row that is not the pane's last was drawn short: "
					+ std::to_string(heightFraction));
		}
	}

	// The whole ticket in one number. A 2x1 pane and a 1x2 pane repeat more
	// tiles at the same scale rather than stretching the same tiles further,
	// and the pixels-per-World-unit a quad samples is where that is visible:
	// it is a fact about the tile and the transform, and it does not move
	// with the pane.
	void theRepeatScaleDoesNotChangeWithPaneSize(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();

		float referenceX{};
		float referenceY{};
		std::string referenceName;
		int referenceTiles{};
		std::vector<std::pair<std::string, int>> tileCounts;

		auto const run = [&](char const* label, uint32_t wide, uint32_t high)
		{
			auto scene = buildScene("room", wide + 2, high, 1, 0,
				CORE_ROOM_MAX_HEIGHT, false, wide, high);
			auto const pane = glassRect(*scene.window);
			auto const pass = recordPass(scene.front, atlas);
			auto const glass = pass.glass();
			require(!glass.empty(),
				std::string{ "The " } + label + " pane drew no glass at all");

			int columns{}, rows{};
			layoutGlass(glass, pane, columns, rows);
			auto const tiles = int(glass.size());
			require(tiles == columns * rows,
				std::string{ "The " } + label + " pane's tile count disagrees "
					"with the grid it laid out");

			for (auto const& q : glass)
			{
				float perUnitX{}, perUnitY{};
				sampledScale(q, perUnitX, perUnitY);
				if (referenceName.empty())
				{
					referenceX = perUnitX;
					referenceY = perUnitY;
					referenceName = label;
					referenceTiles = tiles;
					continue;
				}
				require(close(perUnitX, referenceX, referenceX * 0.02f)
						&& close(perUnitY, referenceY, referenceY * 0.02f),
					std::string{ "The " } + label + " pane samples "
						+ std::to_string(perUnitX) + " x "
						+ std::to_string(perUnitY)
						+ " artwork pixels per World unit where the " + referenceName
						+ " pane samples " + std::to_string(referenceX) + " x "
						+ std::to_string(referenceY)
						+ ", so a bigger pane is stretching the tile rather than "
						"repeating it more times: " + q.rect.str());
			}

			// And the scale is the one the tile was authored at, so "the same
			// scale" is not merely "a scale that happens to be constant".
			require(close(referenceX, float(kCellWidth), float(kCellWidth) * 0.05f)
					&& close(referenceY, float(kCellHeight),
						float(kCellHeight) * 0.05f),
				"The glass samples " + std::to_string(referenceX) + " x "
					+ std::to_string(referenceY)
					+ " artwork pixels per World unit, not the sheet's "
					+ std::to_string(kCellWidth) + " x "
					+ std::to_string(kCellHeight) + ", so the pattern lands "
					"distorted even where it is consistent (first seen on "
				+ referenceName + ")");

			tileCounts.emplace_back(label, tiles);
		};

		run("1x1", 1, 1);
		run("2x1", 2, 1);
		run("1x2", 1, 2);

		// A bigger pane repeats more tiles rather than drawing the same
		// tiles bigger. The scale check above is what makes "more tiles" the
		// only way a pane can get bigger; this is what makes sure it actually
		// did get more of them, so a repeat that quietly drew one tile per
		// pane cannot pass on the scale assertion alone.
		require(tileCounts.size() == 3, "The pane sweep did not reach all three panes");
		for (size_t index = 1; index < tileCounts.size(); ++index)
			require(tileCounts[index].second > tileCounts[0].second,
				"A " + tileCounts[index].first + " pane repeated "
					+ std::to_string(tileCounts[index].second)
					+ " tiles where the 1x1 pane repeated "
					+ std::to_string(tileCounts[0].second)
					+ ", so the bigger pane is not repeating more of them");
		require(tileCounts[1].second != tileCounts[2].second,
			"A 2x1 pane and a 1x2 pane repeated the same number of tiles, so "
			"the sweep cannot tell the two axes apart");
	}

	// The Style picks the tile and nothing else. The pane rect, the aperture
	// and the clip are identical between clear, frosted and tinted, exactly
	// as they were when one pane-sized tile carried the whole opening; the
	// only thing that differs is which tile repeats.
	void eachGlassStyleEmitsItsOwnTileWithIdenticalGeometry(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();

		struct Shot
		{
			std::string tile;
			std::vector<DrawnQuad> quads;
			ScreenRect unionRect{};
		};
		std::vector<Shot> shots;

		std::vector<std::pair<core::Window::Style, std::string>> const styles{
			{ core::Window::Style::Clear, "glass-clear" },
			{ core::Window::Style::Frosted, "glass-frosted" },
			{ core::Window::Style::Tinted, "glass-tinted" },
		};
		for (auto const& [style, expected] : styles)
		{
			auto scene = roomScene();
			std::const_pointer_cast<core::Window>(scene.window)->setState(
				core::Window::State::Closed, style);
			require(scene.window->getStyle() == style,
				"The fixture Window did not take the glass Style it was set to");
			auto const pass = recordPass(scene.front, atlas);
			auto const glass = pass.glass();
			require(!glass.empty(), "A '" + expected + "' pane drew no glass");
			Shot shot;
			shot.tile = expected;
			for (auto const& q : glass)
			{
				require(q.tile == expected, "A pane set to '" + expected
					+ "' drew '" + q.tile + "'; the glass Style picks the tile and "
					"nothing else, so one Style must never wear another's art");
				shot.unionRect.include({ q.rect.left, q.rect.top });
				shot.unionRect.include({ q.rect.right, q.rect.bottom });
				shot.quads.push_back(q);
			}
			require(shot.unionRect.sameAs(glassRect(*scene.window)),
				"A '" + expected + "' pane's glass does not cover its own pane "
				"rect: glass " + shot.unionRect.str() + ", pane "
				+ glassRect(*scene.window).str());
			shots.push_back(std::move(shot));
		}

		auto const& first = shots.front();
		for (auto const& shot : shots)
		{
			require(shot.quads.size() == first.quads.size(), "A '" + shot.tile
				+ "' pane repeated " + std::to_string(shot.quads.size())
				+ " tiles where a '" + first.tile + "' pane repeated "
				+ std::to_string(first.quads.size()) + ", so the Style changed "
				"the geometry and not just the artwork");
			for (size_t index = 0; index < shot.quads.size(); ++index)
			{
				require(shot.quads[index].rect.sameAs(first.quads[index].rect),
					"A '" + shot.tile + "' pane's quad " + std::to_string(index)
						+ " is at " + shot.quads[index].rect.str()
						+ " where the same tile position on a '"
						+ first.tile + "' pane is at "
						+ first.quads[index].rect.str());
				require(shot.quads[index].clip.sameAs(first.quads[index].clip),
					"A '" + shot.tile + "' pane's quad " + std::to_string(index)
						+ " is clipped to " + shot.quads[index].clip.str()
						+ " where a '" + first.tile + "' pane's is clipped to "
						+ first.quads[index].clip.str() + "; the aperture and the "
						"clip are the same for every glass Style");
			}
			require(shot.unionRect.sameAs(first.unionRect),
				"A '" + shot.tile + "' pane fills " + shot.unionRect.str()
					+ " where a '" + first.tile + "' pane fills "
					+ first.unionRect.str());
		}
	}

	// The glass fills the whole aperture - including the strip the frame's
	// moulding will cover - and goes down over the space behind and under the
	// frame, so the moulding always lies on glass rather than on the gap
	// where glass stopped short.
	void theGlassFillsTheWholeApertureUnderTheFrame(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = squareRoomScene(3, 3);
		auto const pane = glassRect(*scene.window);
		auto const pass = recordPass(scene.front, atlas);
		auto const glass = pass.glass();
		require(!glass.empty(), "The 3x3 pane drew no glass");

		ScreenRect covered{};
		for (auto const& q : glass)
		{
			covered.include({ q.rect.left, q.rect.top });
			covered.include({ q.rect.right, q.rect.bottom });
		}
		require(covered.sameAs(pane), "The glass does not fill the whole "
			"aperture: glass " + covered.str() + ", aperture " + pane.str());

		auto const behind = pass.behindCells();
		auto const frames = pass.frameCells();
		require(!behind.empty(), "The fixture drew no space behind the glass, "
			"so the ordering below would prove nothing");
		require(!frames.empty(), "The fixture drew no frame over the glass, so "
			"the ordering below would prove nothing");
		require(*pass.lastIndexOf(behind) < *pass.firstIndexOf(glass),
			"The glass is not drawn over the space behind it");
		require(*pass.lastIndexOf(glass) < *pass.firstIndexOf(frames),
			"The glass is not drawn under the frame that lies over it");

		// And the moulding really does lie over glass: the frame's hole is
		// inset from the pane, so some frame quad overlaps the pane rather
		// than sitting wholly outside it.
		bool mouldingOverGlass = false;
		for (auto const& frame : frames)
			if (frame.rect.overlaps(pane)) mouldingOverGlass = true;
		require(mouldingOverGlass,
			"No frame quad overlaps the pane, so the strip the moulding covers "
			"is not being checked against the glass that fills it");
	}

	// Solid pass only. The glass draws in the solid pass as it does today;
	// the wireframe overlay emits none.
	void theGlassDrawsInTheSolidPassAndNotInTheWireframeOverlay(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = roomScene();
		auto const solid = recordPass(scene.front, atlas, kNoClip,
			LayerRenderStyle::Solid);
		require(!solid.glass().empty(),
			"The solid pass drew no glass over its aperture");

		auto const wire = recordPass(scene.front, atlas, kNoClip,
			LayerRenderStyle::Wireframe);
		require(wire.glass().empty(),
			"The wireframe overlay drew glass: " + tileList(wire.glass()));
		require(wire.objectQuads().empty(),
			"The wireframe overlay drew Objects artwork at all: "
			+ tileList(wire.objectQuads()));
	}

	// A glass tile the installed atlas does not carry is no glass at all: no
	// quad, never the missing-art colour, and the space behind the aperture
	// keeps showing through the pane instead of a hole being punched in it.
	// That is the missing-tile answer every tile gives, and it is what lets
	// a sheet be filled one tile at a time.
	void aMissingGlassTileFallsBackWithoutPunchingAHole(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();

		// Establish that the pass is capable of drawing glass before one is
		// taken away, so the absence below is the tile's absence and not the
		// whole glass layer being skipped.
		auto glazed = roomScene();
		std::const_pointer_cast<core::Window>(glazed.window)->setState(
			core::Window::State::Closed, core::Window::Style::Clear);
		require(!recordPass(glazed.front, atlas).glass().empty(),
			"The Clear pane drew no glass before the missing-tile test, so the "
			"absence checked below would prove nothing");

		installObjectArtWithout("glass-frosted");
		auto scene = roomScene();
		std::const_pointer_cast<core::Window>(scene.window)->setState(
			core::Window::State::Closed, core::Window::Style::Frosted);
		require(scene.window->getStyle() == core::Window::Style::Frosted,
			"The fixture Window did not take the Frosted Style");
		auto const pass = recordPass(scene.front, atlas);
		require(pass.glass().empty(), "A pane whose glass tile the atlas does "
			"not carry still drew glass: " + tileList(pass.glass()));
		for (auto const& q : pass.quads)
			require(q.colour != kMagenta,
				"A missing glass tile drew the missing-art colour over the "
				"aperture; a missing tile is no tile, not a hole");
		// The aperture is not punched: the space behind still drew, the wall
		// underneath is whole, and the frame still lies over the opening.
		require(!pass.behindCells().empty(),
			"The space behind stopped drawing when the glass went missing, so "
			"the aperture is a hole rather than an unglazed opening");
		require(!pass.frameCells().empty(),
			"The frame stopped drawing when the glass went missing");
		require(gridOf(pass.wallCells(), *scene.front).size() == 4,
			"The wall under an unglazed pane stopped being whole");
	}

	// The repeat belongs to the pane, not to the World grid: two windows
	// side by side each start their pattern at their own edge rather than
	// forming one sheet across the wall. The pane's left edge sits 0.1 of a
	// cell into the World grid, which is not a multiple of the tile's 0.25,
	// so a World-anchored repeat would have to cut that first column to
	// 0.15 wide. A full first column is what says the anchor is the pane's.
	void theRepeatIsAnchoredToThePaneAndNotToTheWorldGrid(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		auto scene = multiWindowScene(6, { { 1, 1 }, { 3, 1 } });
		auto const pass = recordPass(scene.front, atlas);
		auto const glass = pass.glass();
		require(!glass.empty(), "The fixture drew no glass");

		int checked{};
		for (uint32_t ordinal = 0; ordinal < 2; ++ordinal)
		{
			auto const window = windowOf(*scene.front, ordinal);
			require(window != nullptr,
				"The fixture lost Window " + std::to_string(ordinal));
			auto const pane = glassRect(*window);
			std::vector<DrawnQuad> mine;
			for (auto const& q : glass)
				if (q.rect.overlaps(pane)) mine.push_back(q);
			require(!mine.empty(), "No glass landed inside Window "
				+ std::to_string(ordinal) + "'s pane at " + pane.str());

			int columns{}, rows{};
			auto const grid = layoutGlass(mine, pane, columns, rows);
			// The first column starts on the pane's own left edge and is a
			// whole tile, and it samples from the tile's own left edge.
			auto const& firstColumn = grid.at(std::make_pair(0, 0));
			auto const tileW = glassTileScreenWidth();
			require(near(firstColumn.rect.left, pane.left),
				"Window " + std::to_string(ordinal) + "'s glass does not start "
				"at its own pane's left edge: quad "
				+ std::to_string(firstColumn.rect.left) + ", pane "
				+ std::to_string(pane.left));
			require(near(firstColumn.rect.right - firstColumn.rect.left, tileW),
				"Window " + std::to_string(ordinal) + "'s first glass column is "
				"cut rather than whole, so the repeat is anchored on the World "
				"grid and not on the pane");
			auto const art = objectTileArtwork(firstColumn.tile);
			require(!!art, "The glass tile vanished from the atlas");
			require(close(firstColumn.u0 * float(gObjectArt.width),
					float(art->region.x) + 0.5f, 0.6f),
				"Window " + std::to_string(ordinal) + "'s first glass column "
				"does not sample from the tile's own left edge, so the pattern "
				"is offset off the pane it belongs to");
			++checked;
		}
		require(checked == 2, "The anchor check did not reach both Windows");
	}

	// ------------------------------------------------------------------
	// The glass under a version 1 Object atlas (#1083).
	//
	// #1079 moved the shipped set onto the repeating `glass-<style>` tiles
	// and left the version 1 sets - the ones the custom theme atlases are
	// authored against - holding only the pane-sized `window-<style>` art
	// they have always held. Nothing asked for those names any more, so every
	// Window under those themes drew no glass at all: framed, showing the
	// space behind, and bare where the pane should have been. That is a
	// finished sheet's artwork turned into dead pixels rather than an
	// unfinished sheet being filled one tile at a time, and the ticket that
	// noticed it settled the case instead of leaving it as a side effect.
	//
	// The settled answer is that the fallback stays reachable. A pane
	// resolves to `glass-<style>` and, when the installed set carries no
	// such tile, wears the pane-sized `window-<style>` art as one quad
	// stretched over the pane at the extent that art was authored for - the
	// picture those themes drew before the repeating tile existed. The
	// checks below pin it so the next rename cannot repeat this one
	// unnoticed.
	// ------------------------------------------------------------------

	// Nothing in the pass painted the missing-art colour. A pane's glass is
	// a tile or it is nothing; it is never a magenta rectangle where the
	// pane should be.
	void requireNoMagenta(Pass const& pass, std::string const& label)
	{
		for (auto const& q : pass.quads)
			require(q.colour != kMagenta, label
				+ " painted the missing-art colour"
				+ (q.tile.empty() ? std::string{ " on a flat quad" }
					: " for tile '" + q.tile + "'")
				+ "; a tile the set does not carry is no tile, not a hole");
	}

	// A 1x1 pane under a version 1 object atlas is not left bare. The
	// repeating tile is not in the set by name, so the pane wears
	// `window-clear`: one quad over the whole pane, sampling the whole
	// tile, at the extent that art was authored for - which for a 1x1 pane
	// is the pane's own 0.8 x 0.3 World units to within the tile's own
	// pixel rounding, so the picture is undistorted exactly where the art
	// was drawn to sit.
	void aVersionOneObjectAtlasDrawsItsPaneSizedGlass(smoke::Context const& context)
	{
		auto atlas = installAtlas(true);
		installVersionOneObjectArt(context);
		auto scene = roomScene();
		auto const pane = glassRect(*scene.window);
		auto const pass = recordPass(scene.front, atlas);
		auto const glass = pass.glass();
		require(glass.size() == 1, "A Clear pane under a version 1 object "
			"atlas drew " + std::to_string(glass.size()) + " glass quads; the "
			"fallback is one pane-sized quad stretched over the pane, which is "
			"what that art was authored to be");
		auto const& quad = glass.front();
		require(quad.tile == "window-clear", "A Clear pane under a version 1 "
			"object atlas drew '" + quad.tile + "'; the fallback names the "
			"pane-sized Clear art the set carries, which is `window-clear`");
		require(quad.rect.sameAs(pane), "The fallback quad does not fill the "
			"pane it belongs to: quad " + quad.rect.str() + ", pane "
			+ pane.str());

		// The whole tile is sampled: the pane-sized art draws whole, not cut
		// and not repeated. The sprite helper insets a tile's sampled range
		// by half a pixel at each edge so a quad never samples its
		// neighbour's border, so a whole 48 px tile measures 47 px of the
		// 48 it owns and the fraction lands just short of 1. The slack below
		// is that inset and room; a real cut is a fraction of the tile, not
		// a pixel of it.
		float across{}, down{};
		sampledTileFraction(quad, across, down);
		require(close(across, 1.0f, 0.05f) && close(down, 1.0f, 0.05f),
			"The fallback quad samples " + std::to_string(across) + " x "
				+ std::to_string(down) + " of its tile's artwork rather than the "
				"whole of it, so the pane-sized art is being cut rather than "
				"stretched across the pane");

		// And the pane's World extent is the tile's authored drawn size to
		// within that rounding, so the fallback draws the legacy art at the
		// extent its author sized it for rather than at a size this fixture
		// made up.
		auto const art = objectTileArtwork(quad.tile);
		require(!!art, "The fallback tile vanished from the installed atlas");
		auto const worldWide = (pane.right - pane.left)
			/ (float(kCellWidth) * gUISettings.worldZoom);
		auto const worldHigh = (pane.bottom - pane.top)
			/ (float(kCellHeight) * gUISettings.worldZoom);
		require(close(worldWide / art->drawnSize.x, 1.0f, 0.05f)
				&& close(worldHigh / art->drawnSize.y, 1.0f, 0.05f),
			"The pane is " + std::to_string(worldWide) + " x "
				+ std::to_string(worldHigh) + " World units against the tile's "
				"authored " + std::to_string(art->drawnSize.x) + " x "
				+ std::to_string(art->drawnSize.y) + ", so the fallback is not "
				"laying the legacy art on the extent it was authored at");

		requireNoMagenta(pass, "A Clear pane under a version 1 object atlas");

		// The glass lies over the opening rather than punching a hole in it:
		// the space behind still drew, the frame still lies over the pane,
		// and the glass sits between them as it does under the shipped set.
		require(!pass.behindCells().empty(),
			"The space behind stopped drawing under the version 1 fallback, so "
			"the aperture is a hole rather than a glazed opening");
		require(!pass.frameCells().empty(),
			"The frame stopped drawing over the version 1 fallback");
		require(*pass.lastIndexOf(pass.behindCells()) < *pass.firstIndexOf(glass),
			"The version 1 glass is not drawn over the space behind it");
		require(*pass.lastIndexOf(glass) < *pass.firstIndexOf(pass.frameCells()),
			"The version 1 glass is not drawn under the frame that lies over it");
	}

	// The Style picks the fallback tile exactly as it picks the repeating
	// one. Clear wears `window-clear`, Frosted `window-frosted` and Tinted
	// `window-tinted`, each filling its own pane with one quad; no Style
	// wears another's art, and none of the three is left unglazed because
	// the name it resolves to is a version 1 name.
	void eachGlassStyleFallsBackToItsOwnVersionOneTile(smoke::Context const& context)
	{
		auto atlas = installAtlas(true);
		installVersionOneObjectArt(context);
		std::vector<std::pair<core::Window::Style, std::string>> const styles{
			{ core::Window::Style::Clear, "window-clear" },
			{ core::Window::Style::Frosted, "window-frosted" },
			{ core::Window::Style::Tinted, "window-tinted" },
		};
		for (auto const& [style, expected] : styles)
		{
			auto scene = roomScene();
			std::const_pointer_cast<core::Window>(scene.window)->setState(
				core::Window::State::Closed, style);
			require(scene.window->getStyle() == style,
				"The fixture Window did not take the glass Style it was set to");
			auto const pane = glassRect(*scene.window);
			auto const pass = recordPass(scene.front, atlas);
			auto const glass = pass.glass();
			require(glass.size() == 1, "A '" + expected + "' pane under a "
				"version 1 object atlas drew " + std::to_string(glass.size())
				+ " glass quads; one pane-sized tile fills one pane");
			require(glass.front().tile == expected, "A pane set to the '"
				+ expected + "' Style drew '" + glass.front().tile
				+ "'; the glass Style picks the tile and nothing else, so one "
				"Style must never wear another's art");
			require(glass.front().rect.sameAs(pane), "A '" + expected
				+ "' pane's fallback quad does not fill its own pane: quad "
				+ glass.front().rect.str() + ", pane " + pane.str());
			requireNoMagenta(pass, "A '" + expected + "' pane under a version "
				"1 object atlas");
		}
	}

	// The version 1 picture is one quad per pane whatever the pane's size,
	// because that is what pane-sized art means: a wider pane stretches the
	// same whole tile across more World units rather than repeating it. That
	// is the pre-#1079 picture kept whole under these sets, and it is what
	// tells the fallback apart from the repeat - which is what a set that
	// carries `glass-` art does instead.
	void theVersionOneGlassStaysOneQuadWhenThePaneGrows(smoke::Context const& context)
	{
		auto atlas = installAtlas(true);
		installVersionOneObjectArt(context);
		std::vector<std::pair<uint32_t, uint32_t>> const spans{
			{ 1, 1 }, { 2, 1 }, { 1, 2 } };
		std::vector<ScreenRect> panes;
		for (auto const& [wide, high] : spans)
		{
			auto const label = std::to_string(wide) + "x" + std::to_string(high);
			auto scene = squareRoomScene(wide, high);
			auto const pane = glassRect(*scene.window);
			auto const pass = recordPass(scene.front, atlas);
			auto const glass = pass.glass();
			require(glass.size() == 1, "A " + label + " pane under a version 1 "
				"object atlas drew " + std::to_string(glass.size())
				+ " glass quads; the pane-sized fallback is one quad per pane, "
				"not one quad per tile");
			require(glass.front().tile == "window-clear", "A " + label
				+ " pane under a version 1 object atlas drew '" + glass.front().tile
				+ "'; the Clear Style names `window-clear` whatever the pane's "
				"size");
			require(glass.front().rect.sameAs(pane), "A " + label
				+ " pane's fallback quad does not fill its own pane: quad "
				+ glass.front().rect.str() + ", pane " + pane.str());
			float across{}, down{};
			sampledTileFraction(glass.front(), across, down);
			require(close(across, 1.0f, 0.05f) && close(down, 1.0f, 0.05f),
				"A " + label + " pane's fallback quad samples only "
				+ std::to_string(across) + " x " + std::to_string(down)
				+ " of its tile, so the pane-sized art is being cut rather than "
				"stretched to the pane");
			requireNoMagenta(pass, "A " + label + " pane under a version 1 "
				"object atlas");
			panes.push_back(pane);
		}
		// The panes really did grow while the tile stayed one quad, so what
		// the checks above read is a tile stretched over a bigger pane and
		// not three copies of one rectangle: the wide span is wider than
		// the 1x1 pane by more than the tile's own width, and the tall span
		// taller by more than the tile's own height.
		require(panes.size() == 3, "The pane sweep did not reach all three spans");
		require(panes[1].right - panes[1].left
				> (panes[0].right - panes[0].left) * 1.5f,
			"The 2x1 span's pane is no wider than the 1x1 pane's, so the sweep "
			"cannot see the tile being stretched across it");
		require(panes[2].bottom - panes[2].top
				> (panes[0].bottom - panes[0].top) * 1.5f,
			"The 1x2 span's pane is no taller than the 1x1 pane's, so the "
			"sweep cannot see the tile being stretched up it");
	}

	// The repeating tile wins wherever the installed set carries it. The
	// fallback exists for a set that has nothing else, not as a second
	// answer that competes with the first: a set caught mid-transition,
	// carrying both the repeating `glass-` tiles and the old pane-sized
	// ones, draws the repeat rather than the stretch.
	void theRepeatingGlassTileIsPreferredOverTheLegacyPaneTile(smoke::Context const&)
	{
		auto atlas = installAtlas(true);
		installObjectArt();
		installObjectArtWithLegacyPaneTiles();
		auto scene = roomScene();
		auto const pass = recordPass(scene.front, atlas);
		auto const glass = pass.glass();
		require(glass.size() > 1, "A pane whose set carries both the repeating "
			"tile and the legacy pane-sized tile drew "
			+ std::to_string(glass.size()) + " glass quads; the repeating tile "
			"is what a pane wears when the installed set has it");
		for (auto const& quad : glass)
			require(quad.tile.rfind("glass-", 0) == 0, "A set carrying both "
				"glass vocabularies drew '" + quad.tile + "' for its pane; the "
				"repeating `glass-` tile is preferred over the pane-sized "
				"`window-` art");
		for (auto const& quad : pass.objectQuads())
			require(quad.tile.rfind("window-", 0) != 0, "The legacy pane-sized "
				"tile '" + quad.tile + "' drew even though the installed set "
				"carries the repeating glass tile the pane prefers");
		requireNoMagenta(pass, "A set carrying both glass vocabularies");
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
	checks.push_back({ "windowframeCells/theFrameHoleCoversTheGlassItFrames",
		isolated<theFrameHoleCoversTheGlassItFrames> });
	checks.push_back({ "windowframeCells/theTallFrameIsTrimmedAtTheCeiling",
		isolated<theTallFrameIsTrimmedAtTheCeiling> });
	checks.push_back({ "windowframeCells/everyWallFramesATallOpeningTheSameWay",
		isolated<everyWallFramesATallOpeningTheSameWay> });
	checks.push_back({ "windowframeCells/aTwoByTwoWindowFramesItsCornersTogether",
		isolated<aTwoByTwoWindowFramesItsCornersTogether> });
	checks.push_back({ "windowframeCells/aThreeByThreeWindowFramesItsWholeEdge",
		isolated<aThreeByThreeWindowFramesItsWholeEdge> });
	checks.push_back({ "windowframeCells/theAllMiddleCellOfA3x3AsksForNoTileAndEmitsNothing",
		isolated<theAllMiddleCellOfA3x3AsksForNoTileAndEmitsNothing> });
	checks.push_back({ "windowframeCells/theInteriorCellsEmptyAnswerIsTheMissingTilesAnswer",
		isolated<theInteriorCellsEmptyAnswerIsTheMissingTilesAnswer> });
	checks.push_back({ "windowframeCells/theCornerFramesAreDrawnLastOverTheWallTileOnEveryCell",
		isolated<theCornerFramesAreDrawnLastOverTheWallTileOnEveryCell> });
	checks.push_back({ "windowframeCells/theCornerFrameIsTrimmedAtTheCeilingOnTheTopRow",
		isolated<theCornerFrameIsTrimmedAtTheCeilingOnTheTopRow> });
	checks.push_back({ "windowframeCells/everyWallFramesACorneredOpeningTheSameWay",
		isolated<everyWallFramesACorneredOpeningTheSameWay> });
	checks.push_back({ "windowframeCells/aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture",
		isolated<aWindowOnABackLayerWearsItsOwnFrameThroughTheAperture> });
	checks.push_back({ "windowframeCells/theFrameNameIsTheOpeningAndNothingElse",
		isolated<theFrameNameIsTheOpeningAndNothingElse> });
	checks.push_back({ "windowframeCells/aOneByOnePaneRepeatsItsTileWithTheLastColumnAndRowCut",
		isolated<aOneByOnePaneRepeatsItsTileWithTheLastColumnAndRowCut> });
	checks.push_back({ "windowframeCells/theRepeatScaleDoesNotChangeWithPaneSize",
		isolated<theRepeatScaleDoesNotChangeWithPaneSize> });
	checks.push_back({ "windowframeCells/eachGlassStyleEmitsItsOwnTileWithIdenticalGeometry",
		isolated<eachGlassStyleEmitsItsOwnTileWithIdenticalGeometry> });
	checks.push_back({ "windowframeCells/theGlassFillsTheWholeApertureUnderTheFrame",
		isolated<theGlassFillsTheWholeApertureUnderTheFrame> });
	checks.push_back({ "windowframeCells/theGlassDrawsInTheSolidPassAndNotInTheWireframeOverlay",
		isolated<theGlassDrawsInTheSolidPassAndNotInTheWireframeOverlay> });
	checks.push_back({ "windowframeCells/aMissingGlassTileFallsBackWithoutPunchingAHole",
		isolated<aMissingGlassTileFallsBackWithoutPunchingAHole> });
	checks.push_back({ "windowframeCells/theRepeatIsAnchoredToThePaneAndNotToTheWorldGrid",
		isolated<theRepeatIsAnchoredToThePaneAndNotToTheWorldGrid> });
	checks.push_back({ "windowframeCells/aVersionOneObjectAtlasDrawsItsPaneSizedGlass",
		isolated<aVersionOneObjectAtlasDrawsItsPaneSizedGlass> });
	checks.push_back({ "windowframeCells/eachGlassStyleFallsBackToItsOwnVersionOneTile",
		isolated<eachGlassStyleFallsBackToItsOwnVersionOneTile> });
	checks.push_back({ "windowframeCells/theVersionOneGlassStaysOneQuadWhenThePaneGrows",
		isolated<theVersionOneGlassStaysOneQuadWhenThePaneGrows> });
	checks.push_back({ "windowframeCells/theRepeatingGlassTileIsPreferredOverTheLegacyPaneTile",
		isolated<theRepeatingGlassTileIsPreferredOverTheLegacyPaneTile> });
}
