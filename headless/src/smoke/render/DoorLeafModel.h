#pragma once

// The Door leaf model, restated on the test side of the render seam so the
// smoke checks predict leaf geometry instead of remembering it (#1056).
//
// A leaf is a full-cell tile drawn at its cell rect, translated by the
// opening style's animation vector and clipped to the doorway aperture.
// The aperture is the Door's own shape: inset from the cell by
// CORE_DOOR_X_INSET on each x side and standing on the cell floor. The
// cell a leaf is drawn at is therefore the aperture widened back out by
// that inset and carried up to a whole cell, which is what lets the clip
// select the band of the tile so one tile serves both widths.
//
// The travel distances are the ones the feature keeps: the aperture
// height for OpenUp, the aperture width for OpenLeft and OpenRight, and
// half the aperture width per leaf for OpenApart.
//
// The side-sliding travel is derived here from the contract rather than
// copied from the renderer (#1061, Option A). The contract pins the
// leaf's trailing edge - the edge last to leave the doorway, and so the
// edge the visible coverage is made of - on the far jamb at p = 0, so
// the reveal begins the instant the Door moves and the leaf never reads
// as frozen, and on the near jamb at p = 1, so a fully open Door leaves
// nothing painted across either jamb. The leaf's own cell puts that edge
// one inset outside the far jamb, so the animation vector starts at that
// inset rather than at zero:
//
//   travel(p) = INSET + p x apertureWidth
//
// What that costs the closed leaf on its tile: a 1-wide OpenLeft Door
// sees u[0.2,1.0] of L0 where the source crop saw u[0.1,0.9], and the
// matching OpenRight Door sees the mirror u[0.0,0.8]. L0 is symmetric
// about u = 0.5 for the 1-wide OpenApart split, so the two bands are
// mirror images of each other and each still holds the symmetry axis.
//
// Everything here is in World units with y growing up. The checks project
// to screen with their own transform helpers.

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Defines.h"
#include "core/Door.h"

namespace door_leaf
{
	// A rectangle in World units.
	struct Rect
	{
		float x0{0.0f}, y0{0.0f}, x1{0.0f}, y1{0.0f};

		float width() const { return x1 - x0; }
		float height() const { return y1 - y0; }
		bool empty() const { return x1 <= x0 || y1 <= y0; }

		Rect intersect(Rect const& o) const
		{
			return {std::max(x0, o.x0), std::max(y0, o.y0),
				std::min(x1, o.x1), std::min(y1, o.y1)};
		}
	};

	// One leaf: the cell it is drawn at, the band of its tile, and the
	// animation vector that carries it out of the doorway.
	struct Leaf
	{
		Rect cell;
		Rect uv;
		float travelX{0.0f};
		float travelY{0.0f};
		// Which of the leaf's vertical edges faces the Door's centre.
		bool innerEdgeIsLeft{false};
		// True when this leaf draws from the R0 family rather than L0.
		bool rightLeaf{false};

		Rect slid() const
		{
			return {cell.x0 + travelX, cell.y0 + travelY,
				cell.x1 + travelX, cell.y1 + travelY};
		}

		// The part of the leaf still inside the doorway.
		Rect visible(Rect const& aperture) const { return slid().intersect(aperture); }
	};

	// What one opening style contributes at one open percentage.
	struct Plan
	{
		uint32_t cellsWide{0};
		Rect aperture;
		// The x of the left edge of the cell span the leaves are drawn at.
		float cellX0{0.0f};
		std::vector<Leaf> leaves;
		// The region of the aperture the leaves have vacated.
		Rect reveal;
		// OpenApart draws each leaf's inner edge as a visible seam.
		bool seamPerLeaf{false};
		// How far each leaf travels between closed and fully open.
		float travelPerLeaf{0.0f};

		// The leaves that still have a part inside the doorway, and are
		// therefore drawn at all.
		std::vector<Leaf> drawn() const
		{
			std::vector<Leaf> stillIn;
			for (auto const& leaf : leaves)
				if (!leaf.visible(aperture).empty()) stillIn.push_back(leaf);
			return stillIn;
		}

		// The part of the doorway the leaves still cover.
		Rect covered() const
		{
			Rect covered;
			bool any{false};
			for (auto const& leaf : drawn())
			{
				auto const v = leaf.visible(aperture);
				if (v.empty()) continue;
				if (!any) { covered = v; any = true; continue; }
				covered = {std::min(covered.x0, v.x0), std::min(covered.y0, v.y0),
					std::max(covered.x1, v.x1), std::max(covered.y1, v.y1)};
			}
			return covered;
		}

		// The union of the drawn leaves' own rects, before the doorway clip
		// is applied. An outline's vertices are the whole cell, so the
		// wireframe pass is measured against this rather than against the
		// clipped part.
		Rect slidUnion() const
		{
			Rect unionRect;
			bool any{false};
			for (auto const& leaf : drawn())
			{
				auto const v = leaf.slid();
				if (!any) { unionRect = v; any = true; continue; }
				unionRect = {std::min(unionRect.x0, v.x0), std::min(unionRect.y0, v.y0),
					std::max(unionRect.x1, v.x1), std::max(unionRect.y1, v.y1)};
			}
			return unionRect;
		}
	};

	// The cell span the leaves are drawn at, for an aperture inset from
	// the cell by CORE_DOOR_X_INSET on each x side.
	inline float cellLeft(float apertureLeft)
	{
		return apertureLeft - CORE_DOOR_X_INSET;
	}

	// The direction a side-sliding style's leaves travel in: -1 for
	// OpenLeft, +1 for OpenRight.
	inline float sideSign(core::Door::OpenStyle style)
	{
		return style == core::Door::OpenStyle::OpenLeft ? -1.0f : 1.0f;
	}

	// Where the contract pins a side-sliding leaf's trailing edge - the
	// edge last to leave the doorway, and so the edge the visible coverage
	// is made of (#1061, Option A). At p = 0 it is on the far jamb, so
	// the reveal begins the instant the Door moves; at p = 1 it is on the
	// near jamb, so a fully open Door paints nothing across either jamb.
	// Between the pins it moves at a uniform speed, which is what makes
	// the visible travel exactly one aperture width.
	inline float sideTrailingEdgePinned(float openPct, Rect const& aperture,
		float sign)
	{
		auto const farJamb = sign < 0.0f ? aperture.x1 : aperture.x0;
		return farJamb + sign * openPct * aperture.width();
	}

	// The animation vector that carries the leaf's own trailing edge onto
	// its pin. The leaf's cell is the aperture widened back out by the
	// inset, so that edge starts one inset outside the far jamb and the
	// travel opens at that inset - which is the whole of #1061: spend the
	// inset at the start instead and the visible edge stalls behind the
	// far jamb for it, drop it altogether and the last inset is still
	// painted across the far jamb at 100% open.
	inline float sideTravel(float openPct, uint32_t cellsWide,
		Rect const& aperture, float sign)
	{
		auto const cellTrailingEdge = sign < 0.0f
			? cellLeft(aperture.x0) + static_cast<float>(cellsWide)
			: cellLeft(aperture.x0);
		return std::abs(sideTrailingEdgePinned(openPct, aperture, sign)
			- cellTrailingEdge);
	}

	inline Plan plan(uint32_t cellsWide, Rect aperture,
		core::Door::OpenStyle style, float openPct)
	{
		Plan out;
		out.aperture = aperture;
		out.cellsWide = cellsWide;

		auto const cellX0 = cellLeft(aperture.x0);
		out.cellX0 = cellX0;
		auto const cellY0 = aperture.y0;
		// The leaf is drawn at its whole cell - one World cell wide and one
		// Level tall - whatever the doorway's own height. The clip is what
		// keeps it inside the doorway.
		auto const cellTop = cellY0 + 1.0f;
		auto const wholeTile = Rect{0.0f, 0.0f, 1.0f, 1.0f};
		auto const cellSpan = static_cast<float>(cellsWide);

		// The whole-cell leaf one of the Door's own cells wears: L0 on the
		// left cell, R0 on every cell after it.
		auto addCell = [&](uint32_t index, float travelX, float travelY)
		{
			auto const x0 = cellX0 + static_cast<float>(index);
			out.leaves.push_back(Leaf{{x0, cellY0, x0 + 1.0f, cellTop},
				wholeTile, travelX, travelY, false, index != 0});
		};

		switch (style)
		{
		case core::Door::OpenStyle::OpenUp:
		{
			auto const travel = openPct * aperture.height();
			out.travelPerLeaf = aperture.height();
			for (uint32_t i = 0; i < cellsWide; ++i)
				addCell(i, 0.0f, travel);
			out.reveal = {aperture.x0, cellY0, aperture.x1, cellY0 + travel};
			break;
		}

		case core::Door::OpenStyle::OpenLeft:
		case core::Door::OpenStyle::OpenRight:
		{
			auto const sign = sideSign(style);
			// Derived from the pins, not read off the renderer: the travel
			// is the distance from where the leaf's cell puts its trailing
			// edge to where the contract pins that edge.
			auto const travel = sideTravel(openPct, cellsWide, aperture, sign);
			// What the visible edge covers between closed and fully open:
			// one aperture width, however far outside the jamb the leaf had
			// to start from.
			out.travelPerLeaf = sideTravel(1.0f, cellsWide, aperture, sign)
				- sideTravel(0.0f, cellsWide, aperture, sign);
			for (uint32_t i = 0; i < cellsWide; ++i)
				addCell(i, sign * travel, 0.0f);

			if (sign < 0.0f)
				// What the leaf has vacated is the strip between its pinned
				// trailing edge and the far jamb it came off.
				out.reveal = {sideTrailingEdgePinned(openPct, aperture, sign),
					aperture.y0, aperture.x1, aperture.y1};
			else
				out.reveal = {aperture.x0, aperture.y0,
					sideTrailingEdgePinned(openPct, aperture, sign),
					aperture.y1};
			break;
		}

		case core::Door::OpenStyle::OpenApart:
		{
			auto const halfWidth = aperture.width() * 0.5f;
			auto const centre = cellX0 + cellSpan * 0.5f;
			auto const travel = openPct * halfWidth;
			out.travelPerLeaf = halfWidth;
			out.seamPerLeaf = true;

			// A cell the centre cuts through becomes two half-cell leaves
			// wearing the two halves of L0 - the 1-wide case.
			for (uint32_t i = 0; i < cellsWide; ++i)
			{
				auto const x0 = cellX0 + static_cast<float>(i);
				auto const x1 = x0 + 1.0f;
				if (x1 <= centre)
					out.leaves.push_back(Leaf{{x0, cellY0, x1, cellTop},
						wholeTile, -travel, 0.0f, false, false});
				else if (x0 >= centre)
					out.leaves.push_back(Leaf{{x0, cellY0, x1, cellTop},
						wholeTile, travel, 0.0f, true, true});
				else
				{
					out.leaves.push_back(Leaf{{x0, cellY0, centre, cellTop},
						{0.0f, 0.0f, 0.5f, 1.0f}, -travel, 0.0f, false, false});
					out.leaves.push_back(Leaf{{centre, cellY0, x1, cellTop},
						{0.5f, 0.0f, 1.0f, 1.0f}, travel, 0.0f, true, false});
				}
			}

			out.reveal = {centre - travel, aperture.y0, centre + travel,
				aperture.y1};
			break;
		}
		}

		return out;
	}
} // namespace door_leaf
