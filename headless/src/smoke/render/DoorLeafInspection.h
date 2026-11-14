#pragma once

// The draw-list inspection helpers the Door leaf smoke files share: the
// screen projection, the individual quads a pass emitted, and the clip a
// fill was painted under (#1056).
//
// A leaf is drawn as a whole cell and clipped to the doorway, so the
// vertex buffer carries quads that reach outside what actually paints.
// Recovering them one by one - rather than folding them into one extent -
// is what lets a check say a leaf is a whole cell rather than a strip,
// and reading the draw command's clip back is what lets a check say the
// clip is the doorway.

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "DrawTypes.h"
#include "DoorLeafModel.h"
#include "UISettings.h"
#include "WorldToScreen.h"

using pf::render::PackedColourValue;

extern UISettings gUISettings;

namespace door_leaf
{
	// The projection the checks predict screen coordinates with, taken from
	// the render module's shared mapping rather than restated here, so it is
	// the transform transformPosition() actually applies (#1100). The Door
	// leaf checks pin the zoom, which the mapping reads like everything else.
	inline float toScreenX(float worldX)
	{
		return pf::render::worldToScreen(gUISettings).toScreenX(worldX);
	}

	inline float toScreenY(float worldY)
	{
		return pf::render::worldToScreen(gUISettings).toScreenY(worldY);
	}

	// A rectangle in screen pixels.
	struct ScreenBox
	{
		float left{0.0f}, top{0.0f}, right{0.0f}, bottom{0.0f};

		float width() const { return right - left; }
		float height() const { return bottom - top; }
	};

	// A World-unit rect projected to screen. World y grows up and screen y
	// grows down, so the rect's top edge is its far y edge on screen.
	inline ScreenBox project(Rect const& r)
	{
		return {toScreenX(r.x0), toScreenY(r.y1), toScreenX(r.x1), toScreenY(r.y0)};
	}

	struct ColourExtent
	{
		float minX{ FLT_MAX }, maxX{ -FLT_MAX };
		float minY{ FLT_MAX }, maxY{ -FLT_MAX };
		int vertexCount{ 0 };

		bool painted() const { return vertexCount > 0; }
	};

	inline ColourExtent extentOf(ImDrawList const* drawList, PackedColourValue colour)
	{
		ColourExtent extent;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col != colour) continue;
			extent.minX = std::min(extent.minX, drawList->VtxBuffer[i].pos.x);
			extent.maxX = std::max(extent.maxX, drawList->VtxBuffer[i].pos.x);
			extent.minY = std::min(extent.minY, drawList->VtxBuffer[i].pos.y);
			extent.maxY = std::max(extent.maxY, drawList->VtxBuffer[i].pos.y);
			++extent.vertexCount;
		}
		return extent;
	}

	// The filled rectangles of one colour: each AddRectFilled contributes
	// four consecutive vertices, so grouping them in fours recovers the
	// individual quads the renderer emitted - which is how two leaf cells
	// are told apart instead of folding into one extent.
	inline std::vector<ScreenBox> filledRectsOf(ImDrawList const* drawList,
		PackedColourValue colour)
	{
		std::vector<int> indices;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) indices.push_back(i);
		if (indices.size() % 4 != 0) return {};
		std::vector<ScreenBox> rects;
		for (size_t start = 0; start < indices.size(); start += 4)
		{
			ScreenBox rect;
			rect.left = rect.right = drawList->VtxBuffer[indices[start]].pos.x;
			rect.top = rect.bottom = drawList->VtxBuffer[indices[start]].pos.y;
			for (size_t i = start; i < start + 4; ++i)
			{
				auto const& v = drawList->VtxBuffer[indices[i]];
				rect.left = std::min(rect.left, v.pos.x);
				rect.right = std::max(rect.right, v.pos.x);
				rect.top = std::min(rect.top, v.pos.y);
				rect.bottom = std::max(rect.bottom, v.pos.y);
			}
			rects.push_back(rect);
		}
		return rects;
	}

	// The X of every vertex painted in the given colour - a seam is a pair
	// of vertical lines, so its position is fully described by the X of
	// the vertices that carry it.
	inline std::vector<float> xsOf(ImDrawList const* drawList, PackedColourValue colour)
	{
		std::vector<float> xs;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) xs.push_back(drawList->VtxBuffer[i].pos.x);
		return xs;
	}

	inline bool anyNear(std::vector<float> const& values, float target, float epsilon)
	{
		return std::any_of(values.begin(), values.end(),
			[&](float value) { return std::abs(value - target) < epsilon; });
	}

	// The ClipRect of the draw command that first consumes a vertex of the
	// given colour - the clip the renderer painted that fill under.
	inline ImVec4 clipCovering(ImDrawList const* drawList, PackedColourValue colour)
	{
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			if (drawList->VtxBuffer[i].col != colour) continue;
			for (int p = 0; p < drawList->IdxBuffer.Size; ++p)
			{
				if (drawList->IdxBuffer[p] != static_cast<ImDrawIdx>(i)) continue;
				for (int c = 0; c < drawList->CmdBuffer.Size; ++c)
				{
					auto const& cmd = drawList->CmdBuffer[c];
					if (p >= static_cast<int>(cmd.IdxOffset)
						&& p < static_cast<int>(cmd.IdxOffset) + static_cast<int>(cmd.ElemCount))
						return cmd.ClipRect;
				}
				return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
			}
			return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
		}
		return ImVec4(0.0f, 0.0f, -1.0f, -1.0f);
	}

	// The union of a set of screen boxes, as an extent.
	inline ColourExtent unionOf(std::vector<ScreenBox> const& boxes)
	{
		ColourExtent extent;
		for (auto const& b : boxes)
		{
			extent.minX = std::min(extent.minX, b.left);
			extent.maxX = std::max(extent.maxX, b.right);
			extent.minY = std::min(extent.minY, b.top);
			extent.maxY = std::max(extent.maxY, b.bottom);
			extent.vertexCount += 4;
		}
		return extent;
	}
} // namespace door_leaf
