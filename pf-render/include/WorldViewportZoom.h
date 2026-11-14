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

	// How far the whole World picture reaches past the World's own declared
	// box, in screen pixels at a given zoom (#1109). The picture is the
	// union of the unlifted World box and that box moved by every Layer's
	// floor lift: `up` is how far above the declared top the deepest
	// positive lift reaches, `down` how far below the declared bottom the
	// deepest negative lift pushes. Both are zero or positive, and a
	// World with no depth anywhere answers {0, 0} - the same box the
	// World always drew.
	struct FloorLiftSlack
	{
		float up{};
		float down{};
	};

	// The slack of that union over every Layer in the World. Asked of the
	// World rather than of the pass in progress, so a caller sizing a clip
	// or a scrollbar reads the same staircase the drawer is about to
	// install. Layers that lift neither way contribute nothing.
	[[nodiscard]] FloorLiftSlack worldFloorLiftSlack(core::World const& world,
		float zoom);

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
	Vec2 scrollMinimum{};
	bool horizontalScrollbar{ false };
	bool verticalScrollbar{ false };
};

// Resolves the mutually-dependent scrollbar visibility for a World canvas.
// World dimensions are expressed in unscaled pixels; zoom is a scale
// multiplier, so values above one make the World larger on screen. The
// sizes cross on the shared drawing vocabulary, not on ImGui's ImVec2: the
// layout maths is the renderer's, whatever surface asks for it (#1003).
//
// `liftUp` and `liftDown` are the floor lift slack of #1109 in screen
// pixels at this zoom: how far the drawn picture reaches above and below
// the declared World box. The scrollbar decision is taken against the
// picture rather than the bare box, or shifted content would sit cut off
// with no bar to reach it. The range then grows at both ends rather than
// shifting: scrollY = 0 keeps meaning "the unlifted World bottom at the
// canvas bottom", so adding a depth never jumps the view. The lifted top
// is reached by scrolling up to worldSize.y + liftUp - canvasSize.y, and
// the pushed-down bottom by scrolling below zero to -liftDown. With both
// slacks at zero every term here is the arithmetic it always was.
inline WorldViewportLayout worldViewportLayout(Vec2 availableSize,
	Vec2 unscaledWorldSize, float zoom, float horizontalScrollbarSpace,
	float verticalScrollbarSpace, float liftUp = 0.0f, float liftDown = 0.0f)
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

	float const pictureHeight = result.worldSize.y + liftUp + liftDown;

	result.horizontalScrollbar = result.worldSize.x > result.canvasSize.x;
	// A picture pushed down past the box needs the bar even when it would
	// otherwise fit, because at the resting scroll position the pushed
	// part sits below the canvas and nothing else can bring it back.
	result.verticalScrollbar = pictureHeight > result.canvasSize.y
		|| liftDown > 0.0f;
	if (result.horizontalScrollbar
		&& pictureHeight > result.canvasSize.y - horizontalScrollbarSpace)
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
		(std::max)(result.worldSize.y + liftUp - result.canvasSize.y, 0.0f)
	};
	result.scrollMinimum = { 0.0f, -liftDown };
	return result;
}
