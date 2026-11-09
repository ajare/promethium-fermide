#pragma once

#include <algorithm>

#include "DrawTypes.h"
#include "core/Vector2.h"

using pf::render::Vec2;

// Converts a screen-space cursor point into World coordinates under the
// viewport the caller is looking at. The cursor arrives as a parameter:
// the shared renderer no longer asks ImGui where the mouse is, so zooming
// and picking keep working whatever surface the caller has the pointer on
// (#1000). Screen position is in the same pixels ImGui reports.
core::Vector2 worldPositionAtCursor(core::Vector2 screenPosition);

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
