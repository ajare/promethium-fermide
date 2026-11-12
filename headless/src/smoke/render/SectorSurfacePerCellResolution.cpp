// The per-cell Sector surface resolution (#1039).
//
// #1037 pinned what a Sector surface emits: one tile per World cell,
// anchored on the Sector's floor line, a partial top cell cropped at its
// top rather than stretched. What that contract cannot say is how the loop
// decides which tile a cell wears. It would be satisfied just as well by a
// loop that asked once for the whole area and repeated the answer - and a
// loop like that would leave nowhere for a per-cell choice to live, which
// is the failure this whole change is arranged to avoid.
//
// So this file pins the other half of the same path:
//
//   * every cell of the grid is resolved exactly once, in the frame the
//     resolver promises - column counted from the drawn area's left edge,
//     level from its floor line - and each cell draws the tile it was
//     resolved to, on the rectangle its own World cell occupies;
//   * resolving a cell to a different tile changes which tile it samples
//     and nothing else. The rectangle, the sampled fractions and the pixels
//     per artwork texel are what they were. A variant is a different
//     picture, never a different geometry;
//   * a cell the clip hides entirely is never resolved at all, and a cell
//     the clip cuts is resolved and drawn cropped at the clip rather than
//     shifted, stretched or dropped.
//
// The atlas is a synthetic one: `columns` x `levels` full-cell tiles, each
// named for the cell it stands for and laid out with level 0 on the sheet's
// bottom row the way level 0 is a Sector's bottom row. Naming the tiles
// after cells means a quad's identity comes back out of the command stream,
// so the check can set the tile a quad sampled against the rectangle that
// quad landed on. Nothing here depends on the order commands were emitted
// in.
//
// Every assertion is on the recorded WorldDrawList command vector, the
// stable CPU seam ADR 0010 names, and every clip is a recorded clip
// rectangle rather than a scissor, so nothing needs a GPU or a window.

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

	// The tile standing for cell (column, level) of the synthetic sheet.
	std::string cellTile(int column, int level)
	{
		return "cell-" + std::to_string(column) + "-" + std::to_string(level);
	}

	// Installs the synthetic sheet: one full-cell tile per cell, level 0 on
	// the bottom row. The renderer reads these names for lookup and nothing
	// else, which is what lets a quad's tile be read back out of its UVs.
	SectorTileset installCellAtlas(int columns, int levels)
	{
		using smoke::require;
		SectorTileset atlas;
		atlas.width = columns * kCellWidth;
		atlas.height = levels * kCellHeight;
		atlas.family = SectorTilesetFamily::New;
		for (int level = 0; level < levels; ++level)
			for (int column = 0; column < columns; ++column)
				atlas.surfaces.emplace(cellTile(column, level),
					SectorTileRegion{ column * kCellWidth,
						(levels - 1 - level) * kCellHeight, kCellWidth,
						kCellHeight });
		require(atlas.surfaces.size() == size_t(columns) * size_t(levels),
			"The synthetic per-cell atlas did not carry one tile per cell");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(),
			"The synthetic per-cell atlas did not come up as the New family");
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
			<< quad.v1;
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
	// cover and the fraction of one tile they sample.
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
	// than out of the order it was drawn in.
	Cell cellOf(Quad const& quad, float floorLine)
	{
		return { std::uint32_t(std::lround(quad.left / float(kCellWidth))),
			std::uint32_t(std::lround((floorLine - quad.bottom)
				/ float(kCellHeight))) };
	}

	// The quad in `pool` that landed on the same rectangle as `quad`.
	Quad onSameRect(std::vector<Quad> const& pool, Quad const& quad)
	{
		for (auto const& other : pool)
			if (near(other.left, quad.left) && near(other.right, quad.right)
					&& near(other.top, quad.top)
					&& near(other.bottom, quad.bottom))
				return other;
		throw smoke::Failure("Nothing was drawn at " + describe(quad)
				+ " in the comparison draw");
	}

	std::string whereIs(Cell const& cell)
	{
		return " at column " + std::to_string(cell.first) + ", level "
			+ std::to_string(cell.second);
	}

	// ------------------------------------------------------------------
	// The checks.
	// ------------------------------------------------------------------

	// Every cell of the grid is resolved exactly once, in the frame the
	// resolver promises, and each one draws the tile it was resolved to on
	// the rectangle its own World cell occupies.
	void everyCellIsResolvedOnceAndDrawnAtItsOwnRect(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 3, levels = 2;
		auto const atlas = installCellAtlas(columns, levels);
		Recorder recorder;
		WorldDrawList commands({ { -1000.0f, -1000.0f },
			{ 1000.0f, 1000.0f } });
		Vec2 const maximum{ float(columns) * kCellWidth,
			float(levels) * kCellHeight };
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				maximum, 1.0f, recorder.resolver()),
			"A Sector surface with an atlas installed must draw");

		// Asked: the whole grid, each cell once, nothing else. A loop that
		// resolved the area rather than its cells fails here, and so does one
		// that asked a cell twice.
		std::set<Cell> asked(recorder.asked.begin(), recorder.asked.end());
		require(recorder.asked.size() == size_t(columns) * size_t(levels),
			"the loop resolved " + std::to_string(recorder.asked.size())
				+ " cells; the area is "
				+ std::to_string(size_t(columns) * size_t(levels))
				+ " cells and every one of them must be asked about on its own");
		require(asked.size() == recorder.asked.size(),
			"the loop resolved some cell more than once");
		for (int level = 0; level < levels; ++level)
			for (int column = 0; column < columns; ++column)
				require(asked.count(Cell{ std::uint32_t(column),
						std::uint32_t(level) }) == 1,
					"the loop never resolved the cell at column "
						+ std::to_string(column) + ", level "
						+ std::to_string(level));

		auto const quads = quadsOf(commands, atlas);
		require(quads.size() == recorder.asked.size(),
			"the loop drew " + std::to_string(quads.size())
				+ " surface quads for "
				+ std::to_string(recorder.asked.size()) + " resolved cells");
		for (auto const& quad : quads)
		{
			auto const cell = cellOf(quad, maximum.y);
			require(cell.first < std::uint32_t(columns)
					&& cell.second < std::uint32_t(levels),
				"A surface quad landed off the grid: " + describe(quad));
			// The cell fills the World cell it belongs to, edge for edge.
			require(near(quad.left, float(cell.first) * kCellWidth)
					&& near(quad.right, float(cell.first + 1) * kCellWidth)
					&& near(quad.bottom,
						maximum.y - float(cell.second) * kCellHeight)
					&& near(quad.top,
						maximum.y - float(cell.second + 1) * kCellHeight),
				"The cell" + whereIs(cell) + " did not fill its own World cell: "
					+ describe(quad));
			// And it drew the tile that cell resolved to, which for this
			// atlas is the tile named for that same cell.
			require(quad.tile == cellTile(int(cell.first), int(cell.second)),
				"The cell" + whereIs(cell) + " drew '" + quad.tile
					+ "'; a cell must draw the tile it resolved to");
			// A whole cell samples a whole tile and carries no colour of its
			// own: the artwork is the picture.
			require(near(quad.u0, 0.0f) && near(quad.u1, 1.0f)
					&& near(quad.v0, 0.0f) && near(quad.v1, 1.0f),
				"The cell" + whereIs(cell) + " did not sample its whole tile: "
					+ describe(quad));
			require(quad.tint == kRaw,
				"The cell" + whereIs(cell) + " was drawn with a colour other "
				"than its raw artwork");
		}
	}

	// The same area drawn twice: once with every cell resolving to one tile,
	// once with every cell resolving to its own. The rectangles and the
	// sampled fractions come out the same; only the tile each cell sampled
	// differs. This is the promise a variant lives or dies by - a different
	// picture, never a different geometry - and the area is deliberately one
	// whose top level is a fraction of a cell, so the crop is in the frame
	// for both draws.
	void aDifferentTilePerCellChangesThePictureNotTheGeometry(
		smoke::Context const&)
	{
		using smoke::require;
		int const columns = 2, levels = 2;
		float const topFraction = 0.6f;
		auto const atlas = installCellAtlas(columns, levels);
		Vec2 const maximum{ float(columns) * kCellWidth,
			float(levels - 1) * kCellHeight + topFraction * kCellHeight };

		WorldDrawList oneTile({ { -1000.0f, -1000.0f },
			{ 1000.0f, 1000.0f } });
		require(drawSectorTileSurfacePerCell(&oneTile, { 0.0f, 0.0f }, maximum,
				1.0f,
				[](std::uint32_t, std::uint32_t) { return cellTile(0, 0); }),
			"The one-tile draw must draw");
		Recorder eachCell;
		WorldDrawList perCell({ { -1000.0f, -1000.0f },
			{ 1000.0f, 1000.0f } });
		require(drawSectorTileSurfacePerCell(&perCell, { 0.0f, 0.0f }, maximum,
				1.0f, eachCell.resolver()),
			"The per-cell draw must draw");

		auto const same = quadsOf(oneTile, atlas);
		auto const varied = quadsOf(perCell, atlas);
		require(same.size() == size_t(columns) * size_t(levels),
			"The one-tile draw made " + std::to_string(same.size())
				+ " quads; the area is "
				+ std::to_string(size_t(columns) * size_t(levels)) + " cells");
		require(varied.size() == same.size(),
			"The per-cell draw made " + std::to_string(varied.size())
				+ " quads against " + std::to_string(same.size())
				+ " for the same area resolved to one tile");

		for (auto const& quad : varied)
		{
			auto const& alike = onSameRect(same, quad);
			// Same rectangle, same sampled fractions: the tile got bigger,
			// smaller, shifted or stretched under a variant if this fails.
			require(near(quad.u0, alike.u0) && near(quad.u1, alike.u1)
					&& near(quad.v0, alike.v0) && near(quad.v1, alike.v1),
				"Resolving a different tile changed the sampled fractions at "
					+ describe(quad) + ", which sampled " + std::to_string(alike.u0)
					+ ".." + std::to_string(alike.u1) + ", "
					+ std::to_string(alike.v0) + ".."
					+ std::to_string(alike.v1) + " before");
			auto const cell = cellOf(quad, maximum.y);
			// And it really did resolve a different tile, except on the one
			// cell that answers the same name in both draws.
			if (cell != Cell{ 0u, 0u })
				require(quad.tile != alike.tile,
					"The cell" + whereIs(cell) + " resolved to '" + quad.tile
						+ "' but sampled the same tile as the one-tile draw: "
						+ describe(quad));
		}

		// The partial top row, in both draws: anchored on its own bottom
		// edge one whole cell above the row below, cropped at the top, at
		// one artwork pixel to one screen pixel.
		int shortRows = 0;
		for (auto const& quads : { varied, same })
			for (auto const& quad : quads)
			{
				if (cellOf(quad, maximum.y).second != std::uint32_t(levels - 1))
					continue;
				++shortRows;
				require(near(quad.bottom, maximum.y - float(levels - 1)
						* kCellHeight),
					"The partial top cell bottoms at " + std::to_string(quad.bottom)
						+ "; it is anchored on its own bottom edge, one whole cell "
						"above the row below");
				require(near(quad.top, 0.0f)
						&& near(quad.height(), topFraction * kCellHeight),
					"The partial top cell draws " + std::to_string(quad.height())
						+ "px; its top level is " + std::to_string(topFraction)
						+ " of a " + std::to_string(kCellHeight) + "px cell");
				require(near(quad.v1, 1.0f)
						&& near(quad.v0, 1.0f - topFraction),
					"The partial top cell samples v " + std::to_string(quad.v0)
						+ ".." + std::to_string(quad.v1) + "; the top "
						+ std::to_string(1.0f - topFraction)
						+ " of the tile is what gets cropped");
			}
		require(shortRows == 2 * columns,
			"Only the top level was expected to be partial in both draws; found "
				+ std::to_string(shortRows) + " short cells");
	}

	// A cell the clip hides entirely is never resolved: the loop does not
	// ask a question whose answer cannot reach the screen. A cell the clip
	// cuts is resolved and drawn cropped at the clip, at the same scale,
	// and carries the clip with it so the scissor takes the rest.
	//
	// The clip here cuts the left edge part-way through column 1 and the
	// top part-way down the top row, and leaves the floor line and the right
	// edge inside it.
	void aCellOutsideTheClipIsNeverResolved(smoke::Context const&)
	{
		using smoke::require;
		int const columns = 3, levels = 2;
		auto const atlas = installCellAtlas(columns, levels);
		float const clipLeft = 1.5f * float(kCellWidth);
		float const clipTop = 0.25f * float(kCellHeight);
		Vec2 const maximum{ float(columns) * kCellWidth,
			float(levels) * kCellHeight };
		Recorder recorder;
		WorldDrawList commands({ { clipLeft, clipTop },
			{ maximum.x + 100.0f, maximum.y + 100.0f } });
		require(drawSectorTileSurfacePerCell(&commands, { 0.0f, 0.0f },
				maximum, 1.0f, recorder.resolver()),
			"A Sector surface with an atlas installed must draw");

		// Column 0 is entirely left of the clip. Asking about it would be
		// asking for a picture no pixel can reach.
		for (auto const& cell : recorder.asked)
			require(cell.first != 0u,
				"The loop resolved column 0, which the clip hides entirely");
		std::set<Cell> asked(recorder.asked.begin(), recorder.asked.end());
		require(recorder.asked.size() == size_t(columns - 1) * size_t(levels)
				&& asked.size() == recorder.asked.size(),
			"The loop resolved " + std::to_string(recorder.asked.size())
				+ " cells; the clip leaves "
				+ std::to_string(size_t(columns - 1) * size_t(levels))
				+ " to draw, each asked once");
		for (int level = 0; level < levels; ++level)
			for (int column = 1; column < columns; ++column)
				require(asked.count(Cell{ std::uint32_t(column),
						std::uint32_t(level) }) == 1,
					"The loop never resolved the cell at column "
						+ std::to_string(column) + ", level "
						+ std::to_string(level)
						+ ", which the clip leaves visible");

		auto const quads = quadsOf(commands, atlas);
		require(quads.size() == recorder.asked.size(),
			"The clipped area drew " + std::to_string(quads.size())
				+ " quads for " + std::to_string(recorder.asked.size())
				+ " resolved cells");
		for (auto const& quad : quads)
		{
			// Every quad carries the clip it was drawn under, so the scissor
			// takes what the geometry still covers.
			require(near(quad.clipLeft, clipLeft)
					&& near(quad.clipTop, clipTop)
					&& near(quad.clipRight, maximum.x + 100.0f)
					&& near(quad.clipBottom, maximum.y + 100.0f),
				"A clipped surface quad did not carry the clip it was drawn "
					"under: " + describe(quad));
			// The top row is cropped at the clip rather than shifted or
			// stretched: it starts part-way down its tile and keeps the
			// tile's floor end on its own bottom edge.
			if (cellOf(quad, maximum.y).second == std::uint32_t(levels - 1))
			{
				require(near(quad.top, clipTop)
						&& near(quad.height(),
							float(kCellHeight) - clipTop),
					"The top row clipped to " + std::to_string(clipTop)
						+ " drew " + std::to_string(quad.height())
						+ "px from " + std::to_string(quad.top)
						+ "; it is cropped at the clip, not shifted to it");
				require(near(quad.v1, 1.0f)
						&& near(quad.v0, clipTop / float(kCellHeight)),
					"The clipped top row samples v " + std::to_string(quad.v0)
						+ ".." + std::to_string(quad.v1) + "; cropping at the clip "
						"must drop the band above it and keep the tile's floor end");
			}
			else
				require(near(quad.v0, 0.0f) && near(quad.v1, 1.0f)
						&& near(quad.height(), float(kCellHeight)),
					"A fully visible row below the clip was cropped: "
						+ describe(quad));
		}
	}
}

void render_smoke::registerSectorSurfacePerCellResolution(
	std::vector<smoke::Check>& checks)
{
	checks.push_back({
		"sectorSurfacePerCell/everyCellIsResolvedOnceAndDrawnAtItsOwnRect",
		isolated<everyCellIsResolvedOnceAndDrawnAtItsOwnRect> });
	checks.push_back({
		"sectorSurfacePerCell/aDifferentTilePerCellChangesThePictureNotTheGeometry",
		isolated<aDifferentTilePerCellChangesThePictureNotTheGeometry> });
	checks.push_back({
		"sectorSurfacePerCell/aCellOutsideTheClipIsNeverResolved",
		isolated<aCellOutsideTheClipIsNeverResolved> });
}
