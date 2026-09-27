#pragma once

#include <algorithm>

#include "imgui/imgui.h"

struct WorldViewportLayout
{
	ImVec2 canvasSize{};
	ImVec2 worldSize{};
	ImVec2 scrollMaximum{};
	bool horizontalScrollbar{ false };
	bool verticalScrollbar{ false };
};

// Resolves the mutually-dependent scrollbar visibility for a World canvas.
// World dimensions are expressed in unscaled pixels; zoom is a scale
// multiplier, so values above one make the World larger on screen.
inline WorldViewportLayout worldViewportLayout(ImVec2 availableSize,
	ImVec2 unscaledWorldSize, float zoom, float horizontalScrollbarSpace,
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
