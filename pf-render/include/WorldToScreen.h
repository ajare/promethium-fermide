#pragma once

#include <cstdint>

#include "DrawTypes.h"
#include "UISettings.h"
#include "core/Defines.h"
#include "core/Vector2.h"

namespace pf::render
{
	// The world-to-screen mapping, stated once (#1100).
	//
	// A World point becomes a screen point by three moves: cell width and
	// Level height in pixels, scaled by the zoom; a flip about the viewport,
	// because the World's y grows up and the screen's grows down; and the
	// pan the scroll carries, in x and in y.
	//
	// The renderer's own transform and every headless helper that has to
	// predict where a World point lands go through this one definition, so
	// the mapping is never stated a second time and cannot drift away from
	// the picture it describes.
	//
	// It is the mapping only. It knows nothing about Sectors, Layers or what
	// any of them means; the one term it carries for a shift that is not the
	// viewport's own - the floor lift - is a count of screen pixels and is
	// derived elsewhere. Every user of the mapping picks up that term in the
	// same build, which is the point of it being here rather than in the
	// handful of call sites that happen to draw something affected.
	struct WorldToScreen
	{
		// Pixels per World unit of x, and per Level of y, at this mapping's zoom.
		float cellWidth{};
		float levelHeight{};
		// The screen x of World x = 0: the viewport's own placement plus the pan.
		float originX{};
		// The screen y the World's y axis is flipped about: the viewport's bottom edge.
		float flipY{};
		// The vertical scroll, taken off the flipped y.
		float scrollY{};
		// How far the whole picture is lifted, in screen pixels at this zoom
		// (#1101). Positive is up the screen: a Layer behind a Sector whose
		// floor runs deep draws higher than it would have, and the lift is
		// already scaled by the zoom here rather than being a World-space
		// number each drawer has to remember to scale. Zero leaves both
		// directions of the mapping exactly as they were before the term
		// existed, which is what lets a flat World record the same command
		// stream it recorded before.
		float floorLiftPx{};

		[[nodiscard]] constexpr float toScreenX(float worldX) const
		{
			return worldX * cellWidth + originX;
		}

		[[nodiscard]] constexpr float toScreenY(float worldY) const
		{
			return flipY - worldY * levelHeight - scrollY - floorLiftPx;
		}

		[[nodiscard]] constexpr Vec2 toScreen(Vec2 const& world) const
		{
			return {toScreenX(world.x), toScreenY(world.y)};
		}

		// The same mapping read backwards: the World point a screen point sits on.
		[[nodiscard]] constexpr float toWorldX(float screenX) const
		{
			return (screenX - originX) / cellWidth;
		}

		[[nodiscard]] constexpr float toWorldY(float screenY) const
		{
			return (flipY - screenY - scrollY - floorLiftPx) / levelHeight;
		}

		// core::Vector2 is not a literal type, so the point-shaped inverse is the
		// one part of the mapping that cannot be constexpr.
		[[nodiscard]] core::Vector2 toWorld(Vec2 const& screen) const
		{
			return {toWorldX(screen.x), toWorldY(screen.y)};
		}

		// The same mapping with its floor lift set to this many screen pixels
		// (#1101). It sets the term rather than adding to it: a mapping
		// carries one lift, which is the lift of the one Layer whose picture
		// the caller is drawing.
		//
		// Returns a copy rather than mutating so a caller can hold the
		// unlifted mapping beside the lifted one and say which of the two
		// each of its draws goes through. Two that matter: the grid stays on
		// the unlifted picture while a lifted Layer's Sectors draw by the
		// lifted one (#1105), and the slack that widens the editor's World
		// clip is measured from the unlifted picture to where each Layer's
		// lift carries it (#1109).
		//
		// What this is not is a split between a Sector's tiles and its clip.
		// The two travel together: `renderSector` installs its own Layer's
		// lift before it transforms its own bounds, so a shifted Sector's
		// clip frames the tiles it was framing instead of cutting their top
		// off (#1102). The one thing drawn inside a lifted Sector that stays
		// where it was authored is an aperture, and it stays there by being
		// measured in the Layer in front and never re-transformed on the way
		// down - not by being cut from an unlifted mapping.
		[[nodiscard]] constexpr WorldToScreen withFloorLift(float liftPx) const
		{
			auto lifted = *this;
			lifted.floorLiftPx = liftPx;
			return lifted;
		}
	};

	// A floor depth is stored in the World as a count of document-space
	// pixels at 1x zoom; this mapping's `floorLiftPx` is a count of screen
	// pixels at this mapping's zoom. One statement of the crossing between
	// the two, so the drawer that adds the lift and the picker that takes it
	// off again cannot each scale it a different way (#1101, #1105).
	[[nodiscard]] constexpr float floorLiftScreenPx(int32_t documentPx, float zoom)
	{
		return static_cast<float>(documentPx) * zoom;
	}

	// The mapping the viewport describes at a zoom given outright rather than
	// read from the settings, for a caller that pins its own zoom and states
	// its checks against that.
	[[nodiscard]] constexpr WorldToScreen worldToScreenAt(UISettings const& settings,
		float zoom)
	{
		return {
			CORE_CELL_WIDTH_PIXELS * zoom,
			CORE_LEVEL_HEIGHT_PIXELS * zoom,
			settings.worldViewportX + settings.xOffset,
			settings.worldViewportY + settings.worldViewportHeight,
			settings.yOffset };
	}

	// The mapping the viewport currently describes.
	[[nodiscard]] constexpr WorldToScreen worldToScreen(UISettings const& settings)
	{
		return worldToScreenAt(settings, settings.worldZoom);
	}
} // namespace pf::render
