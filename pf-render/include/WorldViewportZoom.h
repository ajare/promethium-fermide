#pragma once

#include <algorithm>

#include "DrawTypes.h"
#include "UISettings.h"
#include "WorldToScreen.h"
#include "core/Vector2.h"

namespace core { class World; }

using pf::render::Vec2;

// Converts a screen-space cursor point into World coordinates under the
// viewport the caller is looking at. The cursor arrives as a parameter:
// the shared renderer no longer asks ImGui where the mouse is, so zooming
// and picking keep working whatever surface the caller has the pointer on
// (#1000). Screen position is in the same pixels ImGui reports.
//
// This is the viewport's own mapping with no floor lift in it - the
// picture of a Layer that nothing shifts. Anything aimed at a Layer the
// floor depth has moved wants `pf::render::worldToScreenForLayer` read
// backward, or it lands on the Layer that used to be there (#1105).
core::Vector2 worldPositionAtCursor(core::Vector2 screenPosition);

namespace pf::render
{
	// The Layer the viewport is showing, clamped into the World it is
	// showing. The setting is an `int` that can name no Layer at all, and
	// the render pass clamps it before drawing; picking has to clamp it the
	// same way or the two read the offset of different Layers at the ends
	// of the range.
	[[nodiscard]] uint32_t viewLayerFor(core::World const& world, int viewLayer);

	// How far one Layer's picture is lifted, in screen pixels at a given
	// zoom (#1101, #1105). The World holds the depth as document-space
	// pixels at 1x and the lift is a screen-space term, so it is scaled
	// here and nowhere else - the same rule the renderer's own lift follows.
	[[nodiscard]] float layerFloorLiftPx(core::World const& world,
		uint32_t layerIndex, float zoom);

	// The mapping one Layer's picture is drawn by: the viewport's mapping
	// at this zoom, lifted by that Layer's floor offset (#1105).
	//
	// The renderer adds the lift on the way out; the editor takes it off on
	// the way in. Both directions are this one value, so the number the
	// cursor subtracts is the number the picture was drawn with, and
	// neither side can be updated without the other. Read forward it names
	// where a World point lands; read backward through `toWorld` it says
	// what the cursor is standing on.
	//
	// Content that is world-space rather than Sector-space - the grid, the
	// player transport selector - does not move with the Layer, and is
	// mapped by `worldToScreen`/`worldToScreenAt` with no lift in it.
	[[nodiscard]] WorldToScreen worldToScreenForLayer(UISettings const& settings,
		core::World const& world, uint32_t layerIndex, float zoom);
}

struct WorldViewportLayout
{
	Vec2 canvasSize{};
	Vec2 worldSize{};
	Vec2 scrollMaximum{};
	bool horizontalScrollbar{ false };
	bool verticalScrollbar{ false };
};

// Resolves the mutually-dependent scrollbar visibility for a World canvas.
// World dimensions are expressed in unscaled pixels; zoom is a scale
// multiplier, so values above one make the World larger on screen. The
// sizes cross on the shared drawing vocabulary, not on ImGui's ImVec2: the
// layout maths is the renderer's, whatever surface asks for it (#1003).
inline WorldViewportLayout worldViewportLayout(Vec2 availableSize,
	Vec2 unscaledWorldSize, float zoom, float horizontalScrollbarSpace,
	float verticalScrollbarSpace)
{
	WorldViewportLayout result;
	result.canvasSize = {
		(std::max)(availableSize.x, 1.0f),
		(std::max)(availableSize.y, 1.0f)
	};
	result.worldSize = {
		unscaledWorldSize.x * zoom,
		unscaledWorldSize.y * zoom
	};

	result.horizontalScrollbar = result.worldSize.x > result.canvasSize.x;
	result.verticalScrollbar = result.worldSize.y > result.canvasSize.y;
	if (result.horizontalScrollbar
		&& result.worldSize.y > result.canvasSize.y - horizontalScrollbarSpace)
		result.verticalScrollbar = true;
	if (result.verticalScrollbar
		&& result.worldSize.x > result.canvasSize.x - verticalScrollbarSpace)
		result.horizontalScrollbar = true;

	if (result.horizontalScrollbar)
		result.canvasSize.y = (std::max)(
			result.canvasSize.y - horizontalScrollbarSpace, 1.0f);
	if (result.verticalScrollbar)
		result.canvasSize.x = (std::max)(
			result.canvasSize.x - verticalScrollbarSpace, 1.0f);
	result.scrollMaximum = {
		(std::max)(result.worldSize.x - result.canvasSize.x, 0.0f),
		(std::max)(result.worldSize.y - result.canvasSize.y, 0.0f)
	};
	return result;
}
