// Viewport edge-scrolling checks. These exercise the pure geometry used by
// World-content drags without requiring a graphical window or synthetic input.

#include <stdexcept>

#include "WorldViewportDrag.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	constexpr ImVec2 CanvasMin{ 100.0f, 50.0f };
	constexpr ImVec2 CanvasSize{ 800.0f, 600.0f };

	void theMiddleOfTheCanvasDoesNotScroll()
	{
		auto const delta = worldDragScrollDelta({ 500.0f, 350.0f },
			CanvasMin, CanvasSize, 1.0f / 60.0f);
		require(delta.x == 0.0f && delta.y == 0.0f,
			"a drag away from the canvas border scrolled the viewport");
	}

	void approachingEachBorderScrollsInTheExpectedDirection()
	{
		auto const left = worldDragScrollDelta({ 110.0f, 350.0f },
			CanvasMin, CanvasSize, 0.01f);
		auto const right = worldDragScrollDelta({ 890.0f, 350.0f },
			CanvasMin, CanvasSize, 0.01f);
		auto const top = worldDragScrollDelta({ 500.0f, 60.0f },
			CanvasMin, CanvasSize, 0.01f);
		auto const bottom = worldDragScrollDelta({ 500.0f, 640.0f },
			CanvasMin, CanvasSize, 0.01f);
		require(left.x < 0.0f && left.y == 0.0f,
			"the left drag border did not request leftward scroll");
		require(right.x > 0.0f && right.y == 0.0f,
			"the right drag border did not request rightward scroll");
		require(top.y > 0.0f && top.x == 0.0f,
			"the top drag border did not reveal higher Levels");
		require(bottom.y < 0.0f && bottom.x == 0.0f,
			"the bottom drag border did not reveal lower Levels");
	}

	void scrollingAcceleratesTowardTheBorderAndStaysBoundedOutside()
	{
		auto const entering = worldDragScrollDelta({ 865.0f, 350.0f },
			CanvasMin, CanvasSize, 0.1f);
		auto const edge = worldDragScrollDelta({ 900.0f, 350.0f },
			CanvasMin, CanvasSize, 0.1f);
		auto const outside = worldDragScrollDelta({ 1900.0f, 350.0f },
			CanvasMin, CanvasSize, 1.0f);
		require(entering.x > 0.0f && entering.x < edge.x,
			"drag scrolling does not ramp up toward the border");
		require(edge.x == WorldDragScrollPixelsPerSecond * 0.1f,
			"the border did not reach the configured drag-scroll speed");
		require(outside.x == edge.x,
			"an outside pointer exceeded the bounded drag-scroll speed or frame step");
	}

	void anOutsidePointerContinuesAtTheNearestCanvasEdge()
	{
		auto const position = worldDragPositionInCanvas({ 10.0f, 900.0f },
			CanvasMin, CanvasSize);
		require(position.x == CanvasMin.x + 1.0f,
			"an outside pointer was not pinned inside the left canvas edge");
		require(position.y == CanvasMin.y + CanvasSize.y - 1.0f,
			"an outside pointer was not pinned inside the bottom canvas edge");
	}
}

void runViewportDragScrollSmokeChecks()
{
	theMiddleOfTheCanvasDoesNotScroll();
	approachingEachBorderScrollsInTheExpectedDirection();
	scrollingAcceleratesTowardTheBorderAndStaysBoundedOutside();
	anOutsidePointerContinuesAtTheNearestCanvasEdge();
}
