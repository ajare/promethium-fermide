// The clip's bottom edge cuts a row's interior, and that row still draws
// (#1048).
//
// Rows are counted up from the Sector's floor line, which is what keeps the
// floor-anchored crop (#1037) out of reach of the viewport. The cost of
// counting from the far end is that the first row worth emitting is a
// subtraction rather than a lookup, and this path subtracted with `ceil`.
// Rounding the distance from the clip's bottom edge to the floor line up
// rounds past the row that straddles that edge - the row whose top is above
// the clip's bottom and whose bottom is below it - and a row that is 319 of
// its 320 pixels visible went undrawn. Only an interior cut breaks: a clip
// bottom landing exactly on a row boundary answers the same either way,
// which is how this survived a rewrite (#1039), a contract (#1037) and
// every scroll of the editor that was ever eyeballed.
//
// The fix is truncation, the shape `firstColumn` already used two lines
// above it. What this file pins is what that buys:
//
//   * a row the clip cuts through is emitted, at its full World cell rect
//     and sampling its whole tile, with the clip carried on the command so
//     the scissor takes the rest. The row is cropped, not skipped;
//   * it is cropped and not stretched. The emitted cell is one cell tall
//     and samples one whole tile, so the pixels that reach the screen are
//     artwork pixels at one to one, exactly as they were above the cut. The
//     crop lands on the tile's floor end, which is the end the cell keeps
//     its own floor on;
//   * a clip bottom landing exactly on a row boundary is unchanged: the row
//     whose bottom edge *is* the cut lies under the clip, not above it, and
//     is still not emitted;
//   * a clip bottom below the whole drawn area is unchanged: every row is
//     emitted whole;
//   * a viewport that cuts the Sector at both ends at once crops each end
//     its own way - the bottom row by the scissor, the top row in the
//     sample - and neither crop stretches the other;
//   * a variant on the straddling row reaches the screen. This is the part
//     that turned the bug from a strip of missing background into lost
//     artwork: once a cell resolves to a tile of the author's choosing
//     (#1039, #1040), a row the loop never visits is a picture nobody
//     painted.
//
// The coverage that existed clipped the left and top edges only (#1039), the
// two edges the counting starts at, so nothing in the suite was ever aimed
// at the one edge where the arithmetic is asymmetric and the only edge that
// breaks. Every assertion here is on the recorded WorldDrawList command
// vector - the stable CPU seam ADR 0010 names - and every clip is a recorded
// clip rectangle rather than a scissor, so nothing needs a GPU or a window.

#include "Checks.h"

#include "DrawTypes.h"
#include "SectorTileset.h"
#include "TileArtwork.h"
#include "WorldDrawList.h"

#include "core/Defines.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	bool near(float a, float b) { return std::abs(a - b) < 0.01f; }

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// The colour a tile is drawn with when nothing is multiplied into it.
	// Every image command the surface loop emits carries exactly this.
	pf::render::PackedColourValue const kRaw = White;

	// The tile standing for cell (column, level) of the synthetic sheet, and
	// the variant of that same cell, named the way a recorded variant names
	// it: the base name with a `-Vn` suffix.
	std::string cellTile(int column, int level)
	{
		return "cell-" + std::to_string(column) + "-" + std::to_string(level);
	}

	std::string cellVariantTile(int column, int level)
	{
		return cellTile(column, level) + "-V1";
	}

	// Installs the synthetic sheet: one full-cell tile per cell, level 0 on
	// the sheet's bottom row the way level 0 is a Sector's bottom row, and -
	// with `variants` - a `-V1` tile per cell as well, parked in the columns
	// to the right so no two tiles share a texel column or row.
	SectorTileset installCellAtlas(int columns, int levels, bool variants = false)
	{
		using smoke::require;
		SectorTileset atlas;
		atlas.width = columns * (variants ? 2 : 1) * kCellWidth;
		atlas.height = levels * kCellHeight;
		atlas.family = SectorTilesetFamily::New;
		for (int level = 0; level < levels; ++level)
		{
			auto const row = (levels - 1 - level) * kCellHeight;
			for (int column = 0; column < columns; ++column)
			{
				atlas.surfaces.emplace(cellTile(column, level), SectorTileRegion{
					column * kCellWidth, row, kCellWidth, kCellHeight });
				if (!variants) continue;
				atlas.surfaces.emplace(cellVariantTile(column, level),
					SectorTileRegion{ (columns + column) * kCellWidth, row,
						kCellWidth, kCellHeight });
			}
		}
		auto const expected = size_t(columns) * size_t(levels)
			* (variants ? 2u : 1u);
		require(atlas.surfaces.size() == expected,
			"The synthetic atlas did not carry one tile per cell"
				+ std::string(variants ? " and one variant per cell" : ""));
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The synthetic atlas did not come up as the New family");
		return atlas;
	}

	using Cell = std::pair<std::uint32_t, std::uint32_t>;

	// A resolver that records every cell it is asked about and answers the
	// tile named for that cell, so the asking and the drawing can be set
	// against each other.
	struct Recorder
	{
		std::vector<Cell> asked;

		SectorSurfaceTileResolver resolver()
		{
			return [this](std::uint32_t column, std::uint32_t level) {
				asked.emplace_back(column, level);
				return cellTile(int(column), int(level));
			};
		}
	};

	// One surface quad as it came out of the command stream: the rectangle
	// it covers, the tile its UVs sample, the fractions of that tile it
	// samples, and the clip it was recorded under.
	struct Quad
	{
		std::string tile;
		float left{}, top{}, right{}, bottom{};
		float u0{}, u1{}, v0{}, v1{};
		float clipLeft{}, clipTop{}, clipRight{}, clipBottom{};
		pf::render::PackedColourValue tint{};

		float height() const { return bottom - top; }
	};

	std::string describe(Quad const& quad)
	{
		std::ostringstream out;
		out << "'" << quad.tile << "' at (" << quad.left << "," << quad.top
			<< ")-(" << quad.right << "," << quad.bottom << ") sampling u "
			<< quad.u0 << ".." << quad.u1 << ", v " << quad.v0 << ".."
			<< quad.v1 << " under a clip bottom of " << quad.clipBottom;
		return out.str();
	}

	// The tile a sampled UV centre falls on. A crop moves the corner of the
	// sampled range but can never move its centre off the tile it came
	// from, and the sheet's regions are disjoint, so the centre lands on
	// exactly one of them.
	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height)
				/ float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
					&& centreV < bottom)
				return name;
		}
		return {};
	}

	// Reads the recorded SectorAtlas triangles back as quads: the pair of
	// triangles one `AddImage` call makes, turned into the rectangle they
	// cover, the fraction of one tile they sample and the clip they carry.
	std::vector<Quad> quadsOf(WorldDrawList const& commands,
		SectorTileset const& atlas)
	{
		using smoke::require;
		std::vector<WorldDrawList::Triangle const*> triangles;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle
				&& triangle->texture == WorldDrawList::Texture::SectorAtlas)
				triangles.push_back(triangle);
		}
		require(triangles.size() % 2 == 0,
			"An odd number of SectorAtlas triangles came back, so they are not "
			"the pairs one surface quad makes");

		std::vector<Quad> quads;
		for (size_t pair = 0; pair < triangles.size(); pair += 2)
		{
			Quad quad;
			quad.left = quad.top = 1e9f;
			quad.right = quad.bottom = -1e9f;
			float uvLeft = 1e9f, uvRight = -1e9f, uvTop = 1e9f,
				uvBottom = -1e9f;
			for (auto const* triangle : { triangles[pair], triangles[pair + 1] })
			{
				for (int i = 0; i < 3; ++i)
				{
					quad.left = std::min(quad.left, triangle->positions[i].x);
					quad.right = std::max(quad.right, triangle->positions[i].x);
					quad.top = std::min(quad.top, triangle->positions[i].y);
					quad.bottom = std::max(quad.bottom, triangle->positions[i].y);
					uvLeft = std::min(uvLeft, triangle->texcoords[i].x);
					uvRight = std::max(uvRight, triangle->texcoords[i].x);
					uvTop = std::min(uvTop, triangle->texcoords[i].y);
					uvBottom = std::max(uvBottom, triangle->texcoords[i].y);
				}
				quad.clipLeft = triangle->clip.minimum.x;
				quad.clipTop = triangle->clip.minimum.y;
				quad.clipRight = triangle->clip.maximum.x;
				quad.clipBottom = triangle->clip.maximum.y;
				quad.tint = triangle->colour;
			}
			quad.tile = tileAt((uvLeft + uvRight) * 0.5f,
				(uvTop + uvBottom) * 0.5f, atlas);
			require(!quad.tile.empty(),
				"A surface quad sampled UVs that fall on no tile of the "
				"installed atlas");
			// The sampled fraction of the tile, undoing the half-texel inset
			// the renderer uses so linear filtering never reads a
			// neighbouring tile's edge texel. Each axis undoes itself against
			// its own sheet extent: the sheet is not square here, so reading
			// a v back against the sheet's width would say something false.
			auto const region = atlas.surfaces.at(quad.tile);
			auto const fraction = [&](float uv, int origin, int extent,
				int sheetExtent) {
				return (uv * float(sheetExtent) - (float(origin) + 0.5f))
					/ float(extent - 1);
			};
			quad.u0 = fraction(uvLeft, region.x, region.width, atlas.width);
			quad.u1 = fraction(uvRight, region.x, region.width, atlas.width);
			quad.v0 = fraction(uvTop, region.y, region.height, atlas.height);
			quad.v1 = fraction(uvBottom, region.y, region.height, atlas.height);
			quads.push_back(quad);
		}
		return quads;
	}

	// The cell a quad's rectangle belongs to, read off the geometry rather
	// than out of the order it was drawn in. The rows are anchored on the
	// floor line, so a quad's own bottom edge says which level it is.
	Cell cellOf(Quad const& quad, float floorLine)
	{
		return { std::uint32_t(std::lround(quad.left / float(kCellWidth))),
			std::uint32_t(std::lround((floorLine - quad.bottom)
				/ float(kCellHeight))) };
	}

	// The levels that came back at all, read off the geometry.
	std::set<int> levelsOf(std::vector<Quad> const& quads, float floorLine)
	{
		std::set<int> levels;
		for (auto const& quad : quads)
			levels.insert(int(cellOf(quad, floorLine).second));
		return levels;
	}

	std::string textOf(std::set<int> const& levels)
	{
		std::ostringstream out;
		bool first = true;
		for (auto const level : levels)
		{
			if (!first) out << ", ";
			first = false;
			out << level;
		}
		return out.str();
	}

	std::string expectedText(int first, int last)
	{
		std::set<int> levels;
		for (int level = first; level <= last; ++level) levels.insert(level);
		return textOf(levels);
	}

	// The quad that landed on the given cell, by its geometry.
	Quad const& onCell(std::vector<Quad> const& quads, float floorLine,
		int column, int level)
	{
		for (auto const& quad : quads)
			if (cellOf(quad, floorLine) == Cell{ std::uint32_t(column),
					std::uint32_t(level) })
				return quad;
		throw smoke::Failure("Nothing was drawn at column "
			+ std::to_string(column) + ", level " + std::to_string(level));
	}

	bool wasAsked(std::vector<Cell> const& asked, int column, int level)
	{
		for (auto const& cell : asked)
			if (cell == Cell{ std::uint32_t(column), std::uint32_t(level) })
				return true;
		return false;
	}

	// Every quad carries the clip it was drawn under, which is what lets the
	// scissor crop the geometry the row selection left whole.
	void requireCarriesClip(std::vector<Quad> const& quads, float clipLeft,
		float clipTop, float clipRight, float clipBottom)
	{
		using smoke::require;
		for (auto const& quad : quads)
			require(near(quad.clipLeft, clipLeft)
					&& near(quad.clipTop, clipTop)
					&& near(quad.clipRight, clipRight)
					&& near(quad.clipBottom, clipBottom),
				"A surface quad did not carry the clip it was drawn under: "
					+ describe(quad));
	}

	std::string rectText(float top, float bottom)
	{
		return "(" + std::to_string(top) + "," + std::to_string(bottom) + ")";
	}

	// The row the clip cuts through, asserted as the rule states it: emitted
	// at its full World cell, sampling its whole tile, under the clip that
	// crops it. `visible` is how much of that cell is above the clip's
	// bottom edge, and is what the scissor leaves rather than what the loop
	// chose to draw.
	void requireStraddlingRowDrawnWhole(Quad const& quad, float floorLine,
		int level, float clipBottom)
	{
		using smoke::require;
		auto const cellBottom = floorLine - float(level) * float(kCellHeight);
		auto const cellTop = cellBottom - float(kCellHeight);
		require(near(quad.bottom, cellBottom) && near(quad.top, cellTop),
			"The row the clip cuts through was not emitted at its full World "
			"cell: " + describe(quad) + "; the cell runs " + rectText(cellTop,
					cellBottom) + " and the scissor takes whatever of that is "
					"below " + std::to_string(clipBottom));
		// One cell tall, one whole tile sampled: the pixels per artwork pixel
		// the visible band gets is the ratio the rest of the row already had.
		require(near(quad.height(), float(kCellHeight)),
			"The straddling row drew " + std::to_string(quad.height())
				+ "px for a " + std::to_string(kCellHeight)
				+ "px cell; a row cut by the clip is cropped, not shrunk");
		require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f),
			"The straddling row samples v " + std::to_string(quad.v0) + ".."
				+ std::to_string(quad.v1) + "; it is emitted sampling its whole "
				"tile and cropped by the clip, so a shrunk sample here means the "
				"crop was folded into the sample and the tile is being stretched");
		require(near(quad.clipBottom, clipBottom),
			"The straddling row did not carry the clip that crops it: "
				+ describe(quad));
		require(quad.tint == kRaw,
			"The straddling row was drawn with a colour other than its raw "
				"artwork: " + describe(quad));
		// One artwork pixel per screen pixel, stated on the sample as well as
		// on the rect: the emitted quad covers as many artwork rows as it
		// covers screen pixels. Fold the crop into the sample and shrink the
		// rect to the clip and this balance can still hold while the picture
		// is stretched by a different amount everywhere else; what it says
		// here is that the scissor, not the renderer, is doing the cropping.
		require(near((quad.v1 - quad.v0) * float(kCellHeight), quad.height()),
			"The straddling row does not hold one artwork pixel to one screen "
				"pixel: " + describe(quad));
		// And the cut really is interior: part of the cell is above the
		// clip's bottom edge and part of it below, which is the case under
		// test. If this ever fails, the fixture stopped exercising the cut.
		auto const visible = clipBottom - cellTop;
		require(visible > 0.0f && visible < float(kCellHeight),
			"The clip does not cut this row's interior, so the case is not the "
			"one under test");
	}

	// ------------------------------------------------------------------
	// The checks.
	// ------------------------------------------------------------------

	// A clip bottom one pixel above the Sector's floor line leaves the whole
	// bottom row on screen but for that pixel, and the whole row draws.
	//
	// This is the case the old `ceil` lost outright: the distance from the
	// clip's bottom edge to the floor line was 1px, `ceil` rounded it up to
	// one cell, and the loop started one row above the floor - leaving 319
	// visible pixels of a Sector's bottom row undrawn.
	void anInteriorBottomCutDrawsTheRowThatStraddlesTheClip(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels);
		float const floorLine = float(levels) * float(kCellHeight);
		float const clipBottom = floorLine - 1.0f;
		Recorder recorder;
		WorldDrawList commands({ { -1000.0f, -1000.0f },
			{ 1000.0f, clipBottom } });
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				{ float(columns) * float(kCellWidth), floorLine }, 1.0f,
				recorder.resolver()),
			"A Sector surface with an atlas installed must draw");

		// Every level of the Sector is drawn, the bottom one included: the
		// clip's bottom edge sits inside it, not under it.
		auto const quads = quadsOf(commands, atlas);
		require(levelsOf(quads, floorLine)
				== std::set<int>{ 0, 1, 2, 3 },
			"The bottom cut drew levels " + textOf(levelsOf(quads, floorLine))
				+ "; a clip one pixel above the floor line leaves all four levels "
				"of a four-level Sector visible and all four must draw");
		require(quads.size() == size_t(columns) * size_t(levels),
			"The bottom cut drew " + std::to_string(quads.size())
				+ " quads; the whole Sector is "
				+ std::to_string(size_t(columns) * size_t(levels))
				+ " cells and every one of them is at least partly on screen");
		require(wasAsked(recorder.asked, 0, 0),
			"The loop never resolved the cell at column 0, level 0, which the "
			"clip leaves 319 of its 320 pixels visible");

		requireStraddlingRowDrawnWhole(onCell(quads, floorLine, 0, 0),
			floorLine, 0, clipBottom);
		requireCarriesClip(quads, -1000.0f, -1000.0f, 1000.0f, clipBottom);

		// The rows above the cut are untouched by it: whole cells, whole
		// tiles, exactly as an unclipped draw would have laid them.
		for (int level = 1; level < levels; ++level)
		{
			auto const& quad = onCell(quads, floorLine, 0, level);
			require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f)
					&& near(quad.height(), float(kCellHeight)),
				"A row wholly above the cut was altered by it: "
					+ describe(quad));
		}
	}

	// A clip bottom part-way up a row that is not the bottom one: that row
	// draws, and only the rows wholly under the clip are left out.
	//
	// The cut here lands half way up level 1 of a four-level Sector. Level 0
	// is entirely below the clip and is neither resolved nor drawn; level 1
	// straddles the clip and draws whole for the scissor to crop at its
	// middle.
	void aBottomCutPartWayUpARowDrawsThatRowAndSkipsOnlyWhatIsBelow(
		smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels);
		float const floorLine = float(levels) * float(kCellHeight);
		float const clipBottom = floorLine - 1.5f * float(kCellHeight);
		Recorder recorder;
		WorldDrawList commands({ { -1000.0f, -1000.0f },
			{ 1000.0f, clipBottom } });
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				{ float(columns) * float(kCellWidth), floorLine }, 1.0f,
				recorder.resolver()),
			"A Sector surface with an atlas installed must draw");

		auto const quads = quadsOf(commands, atlas);
		require(levelsOf(quads, floorLine) == std::set<int>{ 1, 2, 3 },
			"The cut drew levels " + textOf(levelsOf(quads, floorLine))
				+ "; the clip cuts level 1's interior and leaves levels 2 and 3 "
				"whole above it, while level 0 is entirely under the clip");
		require(quads.size() == size_t(columns) * size_t(levels - 1),
			"The cut drew " + std::to_string(quads.size()) + " quads; three "
				"levels of " + std::to_string(columns) + " cells each were asked "
				"for");
		for (int column = 0; column < columns; ++column)
			require(!wasAsked(recorder.asked, column, 0),
				"The loop resolved column " + std::to_string(column)
					+ ", level 0, which lies entirely below the clip's bottom "
					"edge");

		for (int column = 0; column < columns; ++column)
			requireStraddlingRowDrawnWhole(onCell(quads, floorLine, column, 1),
				floorLine, 1, clipBottom);
		requireCarriesClip(quads, -1000.0f, -1000.0f, 1000.0f, clipBottom);
	}

	// A clip bottom landing exactly on a row boundary is unchanged by the
	// fix, and the boundary is not a straddle: the row whose bottom edge *is*
	// the cut lies under the clip, so it is not emitted, and the row whose
	// top edge is the cut draws whole.
	//
	// Both boundary distances below are whole cells, which is the only case
	// `ceil` and truncation ever agreed on. That agreement is why an interior
	// cut could go unnoticed for two tickets.
	void aClipBottomExactlyOnARowBoundaryIsUnchanged(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels);
		float const floorLine = float(levels) * float(kCellHeight);
		for (int cellsBelow : { 1, 2, 3 })
		{
			float const clipBottom = floorLine
				- float(cellsBelow) * float(kCellHeight);
			Recorder recorder;
			WorldDrawList commands({ { -1000.0f, -1000.0f },
				{ 1000.0f, clipBottom } });
			require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
					{ float(columns) * float(kCellWidth), floorLine },
					1.0f, recorder.resolver()),
				"A Sector surface with an atlas installed must draw");
			auto const quads = quadsOf(commands, atlas);
			auto const drawn = levelsOf(quads, floorLine);
			std::set<int> want;
			for (int level = cellsBelow; level < levels; ++level)
				want.insert(level);
			require(drawn == want,
				"A clip bottom exactly " + std::to_string(cellsBelow)
					+ " cells above the floor line drew levels "
					+ textOf(drawn) + "; it must draw "
					+ expectedText(cellsBelow, levels - 1)
					+ " and leave the cells below the boundary undrawn");
			require(quads.size() == size_t(columns)
					* size_t(levels - cellsBelow),
				"A clip bottom on a row boundary drew "
					+ std::to_string(quads.size()) + " quads; "
					+ std::to_string(size_t(columns)
						* size_t(levels - cellsBelow)) + " were asked for");
			// The row that starts at the boundary is whole: full cell, full
			// tile, no crop of its own.
			for (int column = 0; column < columns; ++column)
			{
				auto const& quad = onCell(quads, floorLine, column, cellsBelow);
				require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f)
						&& near(quad.height(), float(kCellHeight))
						&& near(quad.bottom, clipBottom),
					"The row whose bottom edge is the boundary was not whole: "
						+ describe(quad));
			}
			for (int column = 0; column < columns; ++column)
				require(!wasAsked(recorder.asked, column, cellsBelow - 1),
					"The loop resolved the cell directly under a boundary clip, "
					"which has no pixel above the clip");
		}
	}

	// A clip bottom below the whole drawn area is unchanged: every row of
	// the Sector draws whole, including the one whose bottom edge is the
	// floor line itself, which is the boundary case with nothing below it.
	void aClipBottomBelowTheWholeAreaIsUnchanged(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels);
		float const floorLine = float(levels) * float(kCellHeight);
		for (float below : { 0.0f, 1.0f, 400.0f })
		{
			float const clipBottom = floorLine + below;
			Recorder recorder;
			WorldDrawList commands({ { -1000.0f, -1000.0f },
				{ 1000.0f, clipBottom } });
			require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
					{ float(columns) * float(kCellWidth), floorLine },
					1.0f, recorder.resolver()),
				"A Sector surface with an atlas installed must draw");
			auto const quads = quadsOf(commands, atlas);
			require(levelsOf(quads, floorLine)
					== std::set<int>{ 0, 1, 2, 3 },
				"A clip bottom " + std::to_string(below) + "px below the floor "
					"line drew levels " + textOf(levelsOf(quads, floorLine))
					+ "; the whole Sector is above the clip and must draw");
			require(quads.size() == size_t(columns) * size_t(levels),
				"A clip below the area drew " + std::to_string(quads.size())
					+ " quads for a " + std::to_string(size_t(columns)
						* size_t(levels)) + "-cell Sector");
			for (auto const& quad : quads)
				require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f)
						&& near(quad.height(), float(kCellHeight)),
					"A cell that is wholly above the clip was cropped: "
						+ describe(quad));
			requireCarriesClip(quads, -1000.0f, -1000.0f, 1000.0f, clipBottom);
		}
	}

	// A viewport shorter than the Sector cuts it at both ends at once, and
	// both crops do their own job. The bottom edge crops by the scissor and
	// leaves the row it cuts emitted whole; the top edge crops by the sample
	// and drops the band of tile that would have stood above the ceiling.
	// This is the ordinary scroll case for a tall Room in a short window, and
	// the two crops have to coexist: fixing the bottom by clamping geometry
	// to the clip would show up here as a stretched top row.
	void aClipCuttingTopAndBottomAtOnceCropsEachEndItsOwnWay(
		smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels);
		float const floorLine = float(levels) * float(kCellHeight);
		float const clipTop = 200.0f;
		float const clipBottom = 1000.0f;
		Recorder recorder;
		WorldDrawList commands({ { -1000.0f, clipTop },
			{ 1000.0f, clipBottom } });
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				{ float(columns) * float(kCellWidth), floorLine }, 1.0f,
				recorder.resolver()),
			"A Sector surface with an atlas installed must draw");

		auto const quads = quadsOf(commands, atlas);
		require(levelsOf(quads, floorLine) == std::set<int>{ 0, 1, 2, 3 },
			"A viewport spanning the middle of a four-level Sector drew levels "
				+ textOf(levelsOf(quads, floorLine)) + "; every level is at least "
				"partly between the two edges and every one must draw");
		require(quads.size() == size_t(columns) * size_t(levels),
			"The spanning viewport drew " + std::to_string(quads.size())
				+ " quads for a " + std::to_string(size_t(columns)
					* size_t(levels)) + "-cell Sector");

		// Bottom edge: emitted whole, cropped by the scissor.
		for (int column = 0; column < columns; ++column)
			requireStraddlingRowDrawnWhole(onCell(quads, floorLine, column, 0),
				floorLine, 0, clipBottom);

		// Top edge: cropped in the sample, the tile's floor end still on the
		// row's own bottom edge, and the row shortened to what is left.
		float const visibleTopRow = float(kCellHeight) - clipTop;
		for (int column = 0; column < columns; ++column)
		{
			auto const& quad = onCell(quads, floorLine, column, levels - 1);
			require(near(quad.top, clipTop)
					&& near(quad.height(), visibleTopRow),
				"The top row clipped at " + std::to_string(clipTop) + " drew "
					+ std::to_string(quad.height()) + "px from "
					+ std::to_string(quad.top) + "; it is cropped at the clip, "
					"not shifted to it: " + describe(quad));
			require(near(quad.v1, 1.0f)
					&& near(quad.v0, clipTop / float(kCellHeight)),
				"The top row samples v " + std::to_string(quad.v0) + ".."
					+ std::to_string(quad.v1) + "; the band above the clip is what "
					"gets dropped and the tile's floor end is kept: "
					+ describe(quad));
			// Cropped, not stretched: the sampled artwork rows still equal
			// the drawn pixels.
			require(near((quad.v1 - quad.v0) * float(kCellHeight),
					quad.height()),
				"The top row lost its one artwork pixel to one screen pixel "
				"ratio: " + describe(quad));
		}

		// The rows wholly between the two edges are untouched by either.
		for (int level = 1; level <= 2; ++level)
			for (int column = 0; column < columns; ++column)
			{
				auto const& quad = onCell(quads, floorLine, column, level);
				require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f)
						&& near(quad.height(), float(kCellHeight)),
					"A row wholly inside the viewport was cropped by one of its "
					"edges: " + describe(quad));
			}
		requireCarriesClip(quads, -1000.0f, clipTop, 1000.0f, clipBottom);
	}

	// The variant case, which is the one an author notices.
	//
	// A cell recorded as wearing a variant (#1040) resolves to that variant
	// tile and must draw it wherever its cell lands. Under the old row
	// selection a variant on the straddling row was not drawn cropped and
	// partly missing: it was not drawn at all, while the identical cell one
	// row up drew fine. The ticket's own words: this is where a strip of
	// missing background became "the artwork I painted is not showing".
	void aVariantOnTheStraddlingRowIsDrawnAndNotSkipped(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 4;
		auto const atlas = installCellAtlas(columns, levels, true);
		require(hasSectorTileSurface(cellVariantTile(0, 0)),
			"The test atlas was supposed to carry a variant for the straddling "
			"cell");
		float const floorLine = float(levels) * float(kCellHeight);
		float const clipBottom = floorLine - 1.0f;
		// The resolver answers the way a recorded variant does: the base
		// name with a `-Vn` suffix, everywhere a variant was recorded and the
		// base elsewhere.
		std::vector<Cell> asked;
		SectorSurfaceTileResolver resolver = [&](std::uint32_t column,
			std::uint32_t level) {
			asked.emplace_back(column, level);
			return level == 0u ? cellVariantTile(int(column), int(level))
				: cellTile(int(column), int(level));
		};
		WorldDrawList commands({ { -1000.0f, -1000.0f },
			{ 1000.0f, clipBottom } });
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				{ float(columns) * float(kCellWidth), floorLine }, 1.0f,
				resolver),
			"A Sector surface with an atlas installed must draw");

		auto const quads = quadsOf(commands, atlas);
		require(wasAsked(asked, 0, 0) && wasAsked(asked, 1, 0),
			"The loop never resolved the variant cells on the straddling row, "
			"so their artwork never had a chance to reach the screen");
		for (int column = 0; column < columns; ++column)
		{
			auto const& quad = onCell(quads, floorLine, column, 0);
			require(quad.tile == cellVariantTile(column, 0),
				"The variant cell at column " + std::to_string(column)
					+ ", level 0 drew '" + quad.tile + "'; a recorded variant "
					"draws its own tile even on a row the clip cuts through");
			requireStraddlingRowDrawnWhole(quad, floorLine, 0, clipBottom);
		}
		for (int column = 0; column < columns; ++column)
			require(onCell(quads, floorLine, column, 1).tile
					== cellTile(column, 1),
				"A cell with no variant recorded drew a variant tile: "
					+ describe(onCell(quads, floorLine, column, 1)));
		requireCarriesClip(quads, -1000.0f, -1000.0f, 1000.0f, clipBottom);
	}
}

void render_smoke::registerSectorSurfaceBottomClip(std::vector<smoke::Check>& checks)
{
	checks.push_back({
		"sectorSurfaceBottomClip/anInteriorBottomCutDrawsTheRowThatStraddlesTheClip",
		isolated<anInteriorBottomCutDrawsTheRowThatStraddlesTheClip> });
	checks.push_back({
		"sectorSurfaceBottomClip/aBottomCutPartWayUpARowDrawsThatRowAndSkipsOnlyWhatIsBelow",
		isolated<aBottomCutPartWayUpARowDrawsThatRowAndSkipsOnlyWhatIsBelow> });
	checks.push_back({
		"sectorSurfaceBottomClip/aClipBottomExactlyOnARowBoundaryIsUnchanged",
		isolated<aClipBottomExactlyOnARowBoundaryIsUnchanged> });
	checks.push_back({
		"sectorSurfaceBottomClip/aClipBottomBelowTheWholeAreaIsUnchanged",
		isolated<aClipBottomBelowTheWholeAreaIsUnchanged> });
	checks.push_back({
		"sectorSurfaceBottomClip/aClipCuttingTopAndBottomAtOnceCropsEachEndItsOwnWay",
		isolated<aClipCuttingTopAndBottomAtOnceCropsEachEndItsOwnWay> });
	checks.push_back({
		"sectorSurfaceBottomClip/aVariantOnTheStraddlingRowIsDrawnAndNotSkipped",
		isolated<aVariantOnTheStraddlingRowIsDrawnAndNotSkipped> });
}
