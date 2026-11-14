#pragma once

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
	// It is the mapping only. It carries no term for what a Sector is, how
	// deep it sits, or anything else that may one day shift the picture:
	// such a term joins the mapping here, in the one expression, and every
	// user of it picks the change up in the same build.
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

		[[nodiscard]] constexpr float toScreenX(float worldX) const
		{
			return worldX * cellWidth + originX;
		}

		[[nodiscard]] constexpr float toScreenY(float worldY) const
		{
			return flipY - worldY * levelHeight - scrollY;
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
			return (flipY - screenY - scrollY) / levelHeight;
		}

		// core::Vector2 is not a literal type, so the point-shaped inverse is the
		// one part of the mapping that cannot be constexpr.
		[[nodiscard]] core::Vector2 toWorld(Vec2 const& screen) const
		{
			return {toWorldX(screen.x), toWorldY(screen.y)};
		}
	};

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
