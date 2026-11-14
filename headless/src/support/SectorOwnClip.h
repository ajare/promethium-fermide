#pragma once

#include <algorithm>

#include "WorldDrawList.h"
#include "UISettings.h"
#include "WorldToScreen.h"
#include "core/Defines.h"
#include "core/Sector.h"

extern UISettings gUISettings;

// The box a Sector clips its own drawing to, in screen space.
//
// `renderSector` pushes `Sector::getBounds()` - the Sector's left and right
// extents, its floor, and its ceiling at the height its type defines - and
// intersects it with whatever clip it was already drawing under, so nothing a
// Sector draws reaches outside itself and a back Sector seen through a
// one-cell aperture cannot widen the clip back out to its footprint.
//
// A check that sorts a recorded pass into "the Sector's own quads" and "quads
// a Door or Window aperture showed behind it" is sorting on exactly that
// distinction, so the clip it compares against has to be this box rather than
// whatever rect the recorder opened with. The Sector's own surface, frames and
// boundary strips carry it; anything that came down an aperture carries that
// narrower clip and is not one of them.
//
// The mapping is not restated here. It is the render module's `WorldToScreen`,
// the same definition the renderer's own `transformPosition` goes through,
// so a headless helper can no longer predict a screen position the renderer
// disagrees with (#1100).
namespace headless
{
	inline pf::render::Vec2 sectorPointToScreen(core::Vector2 const& point)
	{
		return pf::render::worldToScreen(gUISettings).toScreen({point.x, point.y});
	}

	// The Sector's own box on screen, before any outer clip.
	inline WorldDrawList::ClipRectangle sectorOwnBoxOnScreen(
		core::Sector const& sector)
	{
		core::Vector2 low{}, high{};
		sector.getBounds(low, high);
		auto const a = sectorPointToScreen(low);
		auto const b = sectorPointToScreen(high);
		return {
			{ std::min(a.x, b.x), std::min(a.y, b.y) },
			{ std::max(a.x, b.x), std::max(a.y, b.y) } };
	}

	// The clip a Sector's own drawing carries: its vertical span - floor to
	// the ceiling its type defines - across whatever horizontal span the pass
	// already had, narrowed by the clip it was recorded under.
	//
	// Only the Y extents are the Sector's own. The X span is inherited: a
	// control centred on a Sector boundary keeps the half of it that
	// overhangs, which a box-wide clip would cut away.
	inline WorldDrawList::ClipRectangle sectorOwnClip(
		core::Sector const& sector, WorldDrawList::ClipRectangle const& outer)
	{
		auto const box = sectorOwnBoxOnScreen(sector);
		return {
			{ outer.minimum.x, std::max(outer.minimum.y, box.minimum.y) },
			{ outer.maximum.x, std::min(outer.maximum.y, box.maximum.y) } };
	}
}
