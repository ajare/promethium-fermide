#pragma once

#include "imgui/imgui.h"

// World-content drags auto-scroll before the pointer reaches the edge. The
// speed ramps up through this border so approaching an edge is controllable,
// while a pointer outside the canvas keeps scrolling at the maximum speed.
inline constexpr float WorldDragScrollBorder{ 48.0f };
inline constexpr float WorldDragScrollPixelsPerSecond{ 640.0f };

inline constexpr float worldDragClamp(float value, float minimum, float maximum)
{
	return value < minimum ? minimum : value > maximum ? maximum : value;
}

inline constexpr float worldDragAxisVelocity(float position, float minimum, float maximum)
{
	auto const midpoint = (minimum + maximum) * 0.5f;
	if (position < midpoint)
	{
		auto const proximity = worldDragClamp(
			(minimum + WorldDragScrollBorder - position) / WorldDragScrollBorder,
			0.0f, 1.0f);
		return -proximity * WorldDragScrollPixelsPerSecond;
	}
	auto const proximity = worldDragClamp(
		(position - (maximum - WorldDragScrollBorder)) / WorldDragScrollBorder,
		0.0f, 1.0f);
	return proximity * WorldDragScrollPixelsPerSecond;
}

// Returns the requested scrollbar movement for this frame. Horizontal scroll
// follows screen X. Vertical scrollbar coordinates are world-up, so screen Y
// is inverted: dragging near the top reveals higher Levels.
inline constexpr ImVec2 worldDragScrollDelta(ImVec2 pointer, ImVec2 canvasMin,
	ImVec2 canvasSize, float frameSeconds)
{
	auto const canvasMax = ImVec2(canvasMin.x + canvasSize.x,
		canvasMin.y + canvasSize.y);
	auto const elapsed = worldDragClamp(frameSeconds, 0.0f, 0.1f);
	return ImVec2(
		worldDragAxisVelocity(pointer.x, canvasMin.x, canvasMax.x) * elapsed,
		-worldDragAxisVelocity(pointer.y, canvasMin.y, canvasMax.y) * elapsed);
}

// Continue hit-testing a drag after the physical pointer leaves the canvas by
// pinning its effective position just inside the nearest edge. Scrolling then
// moves new World content beneath that effective pointer.
inline constexpr ImVec2 worldDragPositionInCanvas(ImVec2 pointer, ImVec2 canvasMin,
	ImVec2 canvasSize)
{
	auto const insetX = canvasSize.x > 2.0f ? 1.0f : canvasSize.x * 0.5f;
	auto const insetY = canvasSize.y > 2.0f ? 1.0f : canvasSize.y * 0.5f;
	return ImVec2(
		worldDragClamp(pointer.x, canvasMin.x + insetX,
			canvasMin.x + canvasSize.x - insetX),
		worldDragClamp(pointer.y, canvasMin.y + insetY,
			canvasMin.y + canvasSize.y - insetY));
}
