#include "WorldDrawReplay.h"

#include <algorithm>
#include <array>

namespace
{
	ImVec2 toIm(Vec2 value)
	{
		return ImVec2{ value.x, value.y };
	}

	bool samePoint(Vec2 left, Vec2 right)
	{
		return left.x == right.x && left.y == right.y;
	}

	ImVec4 toIm(WorldDrawList::ClipRectangle const& clip)
	{
		return ImVec4{ clip.minimum.x, clip.minimum.y, clip.maximum.x, clip.maximum.y };
	}

	// The four corners of the axis-aligned box the points span, in the order
	// a filled rectangle is written: top-left, top-right, bottom-right,
	// bottom-left.
	std::array<Vec2, 4> boxCorners(std::array<Vec2, 4> const& points)
	{
		float minX = points[0].x, maxX = points[0].x;
		float minY = points[0].y, maxY = points[0].y;
		for (auto const& point : points)
		{
			minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
			minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
		}
		return { Vec2{ minX, minY }, Vec2{ maxX, minY },
			Vec2{ maxX, maxY }, Vec2{ minX, maxY } };
	}

	// AddRectFilled() and AddImage() both record a filled rectangle as the
	// pair of triangles it is cut from: they share the first->third diagonal
	// and carry one colour, one texture and one clip. Rejoined they are the
	// four-corner quad ImGui's own filled-quad path emits - four vertices,
	// not the six two separately drawn triangles would leave, which is what
	// lets a check group a draw list's vertices back into rectangles.
	//
	// The axis-aligned test is what keeps this honest: a fan of triangles
	// around a shared centre (a filled circle) shares a vertex the same way
	// and must not be rejoined into something it never was.
	bool isQuadPair(WorldDrawList::Triangle const& first,
		WorldDrawList::Triangle const& second)
	{
		if (first.colour != second.colour || first.texture != second.texture) return false;
		// The same family off two different sheets is not one quad: the two
		// triangles would have been recorded by two `AddSectorImage()` calls
		// and belong to two batches, and rejoining them here would let a check
		// read one rectangle where two sheets were drawn (#1089).
		if (first.sectorSheet != second.sectorSheet) return false;
		if (!samePoint(first.clip.minimum, second.clip.minimum)
			|| !samePoint(first.clip.maximum, second.clip.maximum)) return false;

		std::array<Vec2, 4> const points{ first.positions[0], first.positions[1],
			first.positions[2], second.positions[2] };
		// The shared diagonal must actually be a diagonal, and the two free
		// corners must be distinct from each other.
		if (samePoint(points[0], points[2]) || samePoint(points[1], points[3])) return false;

		auto const corners = boxCorners(points);
		auto const countIs = [](auto const& values, Vec2 point, int wanted)
		{
			return std::count_if(values.begin(), values.end(),
				[&](Vec2 other) { return samePoint(point, other); }) == wanted;
		};
		for (auto const& point : points)
		{
			// Every point is a corner of the box, and no corner is used twice:
			// the four of them are the box, not a subset or a repeat of it.
			if (!countIs(corners, point, 1)) return false;
			if (!countIs(points, point, 1)) return false;
		}
		return true;
	}

	// Writes the rejoined quad: four vertices, six indices, the two source
	// triangles tiled over the shared first->third diagonal exactly as they
	// were recorded.
	void replayQuad(ImDrawList* drawList, WorldDrawList::Triangle const& first,
		WorldDrawList::Triangle const& second)
	{
		if ((first.colour & IM_COL32_A_MASK) == 0) return;
		drawList->PrimReserve(6, 4);
		drawList->PrimWriteVtx(toIm(first.positions[0]), toIm(first.texcoords[0]), first.colour);
		drawList->PrimWriteVtx(toIm(first.positions[1]), toIm(first.texcoords[1]), first.colour);
		drawList->PrimWriteVtx(toIm(first.positions[2]), toIm(first.texcoords[2]), first.colour);
		drawList->PrimWriteVtx(toIm(second.positions[2]), toIm(second.texcoords[2]), second.colour);
		auto const base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx - 4);
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 0));
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 0));
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
		drawList->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
	}
}

WorldDrawList::ClipRectangle headless::clipUnder(ImDrawList const* drawList)
{
	// The draw list's live clip, not the top of a stack that may be empty:
	// outside a frame it carries the fullscreen rect, which is exactly the
	// nothing-is-clipped-away starting point a recording needs.
	auto const& clip = drawList->_CmdHeader.ClipRect;
	return { { clip.x, clip.y }, { clip.z, clip.w } };
}

void headless::replay(WorldDrawList const& recorded, ImDrawList* drawList)
{
	auto const& commands = recorded.commands();

	// The clip the primitives just painted under. A recorded clip is already
	// the intersection the renderer computed, so replaying it replaces the
	// draw list's clip rather than re-intersecting it - and only when it
	// actually changed, so the draw-command splitting comes out the same.
	auto painted = drawList->_CmdHeader.ClipRect;
	auto pushed = false;
	auto paintUnder = [&](WorldDrawList::ClipRectangle const& clip)
	{
		auto const wanted = toIm(clip);
		if (pushed && wanted.x == painted.x && wanted.y == painted.y
			&& wanted.z == painted.z && wanted.w == painted.w)
			return;
		if (pushed) drawList->PopClipRect();
		drawList->PushClipRect(ImVec2{ wanted.x, wanted.y },
			ImVec2{ wanted.z, wanted.w }, false);
		painted = wanted;
		pushed = true;
	};

	for (size_t index = 0; index < commands.size(); ++index)
	{
		if (auto const* line = std::get_if<WorldDrawList::Line>(&commands[index]))
		{
			paintUnder(line->clip);
			drawList->AddLine(toIm(line->from), toIm(line->to), line->colour,
				line->thickness);
			continue;
		}
		if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&commands[index]))
		{
			WorldDrawList::Triangle const* pair = nullptr;
			if (index + 1 < commands.size())
				pair = std::get_if<WorldDrawList::Triangle>(&commands[index + 1]);
			paintUnder(triangle->clip);
			if (pair && isQuadPair(*triangle, *pair))
			{
				replayQuad(drawList, *triangle, *pair);
				++index;
			}
			else
			{
				drawList->AddTriangleFilled(toIm(triangle->positions[0]),
					toIm(triangle->positions[1]), toIm(triangle->positions[2]),
					triangle->colour);
			}
			continue;
		}
		if (auto const* text = std::get_if<WorldDrawList::Text>(&commands[index]))
		{
			paintUnder(text->clip);
			if (text->font.size <= 0.0f)
			{
				// The implicit run: the backend's native text path lays it
				// out with its own face and size, as it always did.
				drawList->AddText(toIm(text->position), text->colour,
					text->value.c_str());
				continue;
			}
			// A sized run has no native form. Its glyph triangles come from
			// the installed metrics, positioned relative to the run and
			// textured from the font atlas.
			auto const glyphs = WorldDrawList::fontMetrics()
				.tessellate(text->font, text->value.c_str());
			if (glyphs.empty()) continue;
			auto const count = static_cast<int>(glyphs.size());
			drawList->PrimReserve(count, count);
			for (auto const& vertex : glyphs)
				drawList->PrimVtx(toIm(Vec2{ text->position.x + vertex.position.x,
					text->position.y + vertex.position.y }),
					toIm(vertex.texcoord), text->colour);
			continue;
		}
		// A Barrier is an explicit draw-command break: nothing to paint, but
		// whatever follows must not merge into what came before.
		drawList->AddDrawCmd();
	}

	if (pushed) drawList->PopClipRect();
}
