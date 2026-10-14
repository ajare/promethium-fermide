// Application-only World geometry, adapted from src/Render.cpp.
// Intentionally independent of the editor renderer; no shared rendering module.
#include <array>
#include <stdexcept>
#include <cassert>
#include <algorithm>
#include <optional>

#include "DrawTypes.h"
#include "PlayerTransportSelectorDrawing.h"


#include "core/Defines.h"
#include "core/Background.h"
#include "core/Facade.h"
#include "core/World.h"
#include "core/AirlockTransit.h"
#include "core/ChamberTransit.h"
#include "core/Location.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/StairwellTransit.h"
#include "core/StaircaseTransit.h"
#include "core/DoorSectorObject.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/AccessPanel.h"
#include "core/WalkwaySectorObject.h"
#include "core/Button.h"


#include "Render.h"

#include "SectorTileset.h"
#include "ObjectTileset.h"
#include "WorldDrawList.h"


#include "RenderSettings.h"
#include "WorldRenderer.h"

namespace pf::application::render {

//
// The World currently being rendered. A clear Window composites the
// Backgrounds behind it from the back Layer's cell grid (#37, and #36 made
// the Window's single back Sector non-authoritative for such a span), and
// the renderSector -> renderSectorObjects -> renderWindow chain which
// reaches renderWindowClear carries no World pointer. A RenderWorldScope
// installs it for exactly one render and restores the previous World when it
// goes out of scope. renderWorld() opens one per call; a headless caller that
// drives the Sector-level passes directly opens its own when those passes need
// the World, instead of leaving it to process-global state.
//
static std::shared_ptr<const core::World> gRenderWorld;

RenderWorldScope::RenderWorldScope(std::shared_ptr<const core::World> world)
	: mPrevious(std::move(gRenderWorld))
{
	gRenderWorld = std::move(world);
}

RenderWorldScope::~RenderWorldScope()
{
	gRenderWorld = std::move(mPrevious);
}


using namespace std;

Colour ForeLocationColour = Colour(192, 192, 255);
Colour BackLocationColour = Colour(224, 224, 255);
// The value lives in Render.h as LightsOffTint so the headless checks can read
// the same colour the viewport paints with.
Colour LightsOffColour = Colour(LightsOffTint.r, LightsOffTint.g, LightsOffTint.b);
Colour LadderColour = Colour(128, 128, 192);
Colour LiftColour = Colour(128, 128, 192);
Colour ShuttleColour = Colour(128, 128, 192);
Colour StairwellColour = Colour(128, 128, 192);

#define RENDER_SECTOR_OBJECTS_BEHIND 1
#define RENDER_SECTOR_OBJECTS_INFRONT 2

void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, Colour colour, WorldDrawList* drawList);
void renderSectorDepthContent(shared_ptr<const core::Sector> sector, WorldDrawList* drawList);

void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	std::vector<TransitAperture> const& apertures, WorldDrawList* drawList);

void renderStaircase(shared_ptr<const core::Staircase> staircase, WorldDrawList* drawList);

void transformPosition(core::Vector2& p)
{
	p.x *= CORE_CELL_WIDTH_PIXELS * gRenderSettings.worldZoom;
	p.y *= CORE_LEVEL_HEIGHT_PIXELS * gRenderSettings.worldZoom;
	p.y = gRenderSettings.worldViewportY + gRenderSettings.worldViewportHeight - p.y;

	p.x += gRenderSettings.worldViewportX + gRenderSettings.xOffset;
	p.y -= gRenderSettings.yOffset;
}


void transformPosition(float& x, float& y)
{
	core::Vector2 p{ x, y };

	transformPosition(p);

	x = p.x;
	y = p.y;
}


void renderDoorOpenUp(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	bounds1 = bounds0;
	// The leaf travels the Door's authored physical height. Door timing scales
	// with this distance so regular and tall OpenUp leaves have the same speed.
	bounds1.y += door->getOpenPercentage() * door->getSize().y;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = Colour(64, 192, 255);
		if (!drawObjectSprite("door", drawList, {bounds1.x, bounds1.y}, {bounds2.x, bounds2.y},
			White, {0, door->getOpenPercentage()}, {1, 1}))
			drawList->AddRectFilled({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// Canvas clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds1.x, bounds1.y }, { bounds2.x, bounds2.y }, Colour(0, 0, 0));
	}
}


void renderDoorOpenLeft(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	// The leaf slides toward decreasing world X: its right edge tracks the
	// open percentage, so the aperture is vacated from the right edge and the
	// back Sector is revealed right-to-left.
	bounds1 = bounds2;
	bounds1.x -= door->getOpenPercentage() * (bounds2.x - bounds0.x);

	// At 100% open the leaf has slid clean out of the aperture, so it paints
	// neither fill nor outline - otherwise the wireframe pass would emit a
	// stroked zero-width rectangle at the left jamb.
	bool const leafVisible = bounds1.x > bounds0.x;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = Colour(64, 192, 255);
		if (leafVisible && !drawObjectSprite("door", drawList,
			{bounds0.x, bounds0.y}, {bounds1.x, bounds2.y}, White,
			{door->getOpenPercentage(), 0}, {1, 1}))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// The back Sector shows only through the vacated region: from the leaf's
		// right edge to the aperture's right edge, over the full aperture height.
		// Canvas clipping expects ascending Y coordinates, but we have flipped
		// them for rendering.
		drawList->PushClipRect({ bounds1.x, bounds2.y }, { bounds2.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Only the remaining leaf is outlined; the aperture carries no solid fill.
		if (leafVisible)
			drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds2.y }, Colour(0, 0, 0));
	}
}


void renderDoorOpenRight(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1, bounds2;

	door->getFullShape(bounds0, bounds2);

	// The leaf slides toward increasing world X: its left edge tracks the open
	// percentage, so the aperture is vacated from the left edge and the back
	// Sector is revealed left-to-right.
	bounds1 = bounds0;
	bounds1.x += door->getOpenPercentage() * (bounds2.x - bounds0.x);

	// At 100% open the leaf has slid clean out of the aperture, so it paints
	// neither fill nor outline - otherwise the wireframe pass would emit a
	// stroked zero-width rectangle at the right jamb.
	bool const leafVisible = bounds1.x < bounds2.x;

	transformPosition(bounds0);
	transformPosition(bounds1);
	transformPosition(bounds2);

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = Colour(64, 192, 255);
		if (leafVisible && !drawObjectSprite("door", drawList,
			{bounds1.x, bounds0.y}, {bounds2.x, bounds2.y}, White,
			{0, 0}, {1 - door->getOpenPercentage(), 1}))
			drawList->AddRectFilled({ bounds1.x, bounds0.y }, { bounds2.x, bounds2.y }, doorColour);

		drawList->AddDrawCmd();

		// The back Sector shows only through the vacated region: from the
		// aperture's left edge to the leaf's left edge, over the full aperture
		// height. Canvas clipping expects ascending Y coordinates, but we have
		// flipped them for rendering.
		drawList->PushClipRect({ bounds0.x, bounds2.y }, { bounds1.x, bounds0.y }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Only the remaining leaf is outlined; the aperture carries no solid fill.
		if (leafVisible)
			drawList->AddRect({ bounds1.x, bounds0.y }, { bounds2.x, bounds2.y }, Colour(0, 0, 0));
	}
}


void renderDoorOpenApart(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds2;

	door->getFullShape(bounds0, bounds2);

	// Two equal leaves split at the aperture midpoint and slide outward: each
	// travels half an aperture width over the open percentage, so the gap they
	// leave is always centred and becomes the whole aperture at 100% open.
	// Fractional midpoints are valid, so any Door width is supported.
	auto const worldX0 = bounds0.x;
	auto const worldX2 = bounds2.x;
	auto const halfWidth = (worldX2 - worldX0) * 0.5f;
	auto const midX = worldX0 + halfWidth;
	auto const travel = door->getOpenPercentage() * halfWidth;

	auto const leftLeafX0 = midX - halfWidth - travel;
	auto const leftLeafX1 = midX - travel;
	auto const rightLeafX0 = midX + travel;
	auto const rightLeafX1 = midX + halfWidth + travel;

	// A leaf paints only while some part of it is still inside the aperture; at
	// 100% open both have slid clean out and the Door draws no leaf geometry.
	bool const leftLeafVisible = leftLeafX1 > worldX0;
	bool const rightLeafVisible = rightLeafX0 < worldX2;

	transformPosition(bounds0);
	transformPosition(bounds2);

	// The transform flips Y for rendering, so bounds2 carries the aperture's top
	// and bounds0 its bottom.  X stays linear in world units, which lets the leaf
	// edges be projected without re-running the whole transform.
	auto const apertureLeft = min(bounds0.x, bounds2.x);
	auto const apertureRight = max(bounds0.x, bounds2.x);
	auto const apertureTop = min(bounds0.y, bounds2.y);
	auto const apertureBottom = max(bounds0.y, bounds2.y);
	auto const screenX = [&](float worldX)
	{
		return apertureLeft + (worldX - worldX0) * CORE_CELL_WIDTH_PIXELS
			* gRenderSettings.worldZoom;
	};

	if (style == LayerRenderStyle::Solid)
	{
		auto doorColour = Colour(64, 192, 255);
		auto seamColour = Colour(0, 0, 0);

		// The leaves slide out of the Door, so their fill is clipped to the full
		// aperture and never paints over the surrounding Location.
		drawList->PushClipRect({ apertureLeft, apertureTop }, { apertureRight, apertureBottom }, true);

		if (leftLeafVisible)
		{
			if (!drawObjectSprite("door", drawList, {screenX(leftLeafX0), apertureTop},
				{screenX(leftLeafX1), apertureBottom}, White, {0, 0}, {0.5f, 1}))
				drawList->AddRectFilled({ screenX(leftLeafX0), apertureTop },
					{ screenX(leftLeafX1), apertureBottom }, doorColour);
			// The inner edge is the centre seam while closed and the facing edge of
			// the two leaves while the Door is part open.
			drawList->AddLine({ screenX(leftLeafX1), apertureTop },
				{ screenX(leftLeafX1), apertureBottom }, seamColour);
		}

		if (rightLeafVisible)
		{
			if (!drawObjectSprite("door", drawList, {screenX(rightLeafX0), apertureTop},
				{screenX(rightLeafX1), apertureBottom}, White, {0.5f, 0}, {1, 1}))
				drawList->AddRectFilled({ screenX(rightLeafX0), apertureTop },
					{ screenX(rightLeafX1), apertureBottom }, doorColour);
			drawList->AddLine({ screenX(rightLeafX0), apertureTop },
				{ screenX(rightLeafX0), apertureBottom }, seamColour);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();

		// The back Sector shows only through the centred gap between the two
		// leaves: it grows symmetrically from the midpoint and fills the whole
		// aperture at 100% open.  Canvas clipping expects ascending Y coordinates,
		// but we have flipped them for rendering.
		drawList->PushClipRect({ screenX(leftLeafX1), apertureTop },
			{ screenX(rightLeafX0), apertureBottom }, true);

		// A Door is authored on the front Layer of its pair, so index 1 is the
		// Sector on the Layer directly behind.
		auto backSector = door->getBackSector();
		if (backSector)
		{
			renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);
		}

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		// Both remaining leaves are outlined separately, clipped to the aperture,
		// and the aperture carries no solid fill.
		drawList->PushClipRect({ apertureLeft, apertureTop }, { apertureRight, apertureBottom }, true);

		if (leftLeafVisible)
			drawList->AddRect({ screenX(leftLeafX0), apertureTop },
				{ screenX(leftLeafX1), apertureBottom }, Colour(0, 0, 0));

		if (rightLeafVisible)
			drawList->AddRect({ screenX(rightLeafX0), apertureTop },
				{ screenX(rightLeafX1), apertureBottom }, Colour(0, 0, 0));

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
}


// Keep the condition badge above the threshold, leaving the frozen leaf visible.


void renderDoor(shared_ptr<const core::Door> door, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto openStyle = door->getOpenStyle();

	switch (openStyle)
	{
	case core::Door::OpenStyle::OpenUp:
		renderDoorOpenUp(door, layer, style, drawList);
		break;

	case core::Door::OpenStyle::OpenLeft:
		renderDoorOpenLeft(door, layer, style, drawList);
		break;

	case core::Door::OpenStyle::OpenRight:
		renderDoorOpenRight(door, layer, style, drawList);
		break;

	case core::Door::OpenStyle::OpenApart:
		renderDoorOpenApart(door, layer, style, drawList);
		break;
	}


	// Owned transport Doors share failure, but the warning belongs to the
	// whole transport and follows its car rather than each stationary landing.
}


void renderBulkheadDoor(shared_ptr<const core::BulkheadDoor> door, uint32_t /* layer */, LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	core::Vector2 bounds0, bounds1;

	door->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto topLeft = Vec2{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
	auto bottomRight = Vec2{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
	auto doorColour = Colour(128, 192, 182);
	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect(topLeft, bottomRight, Colour(0, 0, 0));
	}
	else
	{
		drawList->AddRectFilled(topLeft, bottomRight, doorColour);
	}
}


void renderWindowClear(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	window->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, Colour(0, 0, 0));
		return;
	}

	// A Window is authored on the front Layer of its pair, so index 1 is the
	// Sector on the Layer directly behind.
	auto backSector = window->getBackSector();
	auto const backLayer = core::layerBehind(layer);

	// The aperture's rect in world (cell) units, before the screen transform.
	core::Vector2 worldMin, worldMax;
	window->getFullShape(worldMin, worldMax);

	drawList->AddDrawCmd();

	// Canvas clipping expects ascending Y coordinates, but we have flipped them for rendering.
	drawList->PushClipRect({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, true);

	// What the glass shows is derived from the back Layer's cell grid, not from
	// the Window's single back Sector (#36 made that reference
	// non-authoritative). Each Background the aperture looks into is drawn
	// clipped to the intersection of the Window rect and that Background's own
	// rect, so the seam between two Backgrounds lands exactly on the cell
	// boundary between them - no bleed past it, no seam line across it - and
	// stays pinned to the world as the viewport scrolls.
	auto const regions = gRenderWorld
		? backgroundApertureRegions(*gRenderWorld, backLayer, worldMin, worldMax)
		: std::vector<BackgroundApertureRegion>{};

	if (!regions.empty())
	{
		// Cells the span looks through which hold no Background contribute no
		// region; black stands in behind the glass there, as it does for a
		// Window with no back Sector at all. The opaque fills below cover it
		// wherever a Background is present.
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, Colour(0, 0, 0));

		for (auto const& region : regions)
		{
			auto clip0 = region.min;
			auto clip1 = region.max;
			transformPosition(clip0);
			transformPosition(clip1);

			drawList->AddDrawCmd();
			drawList->PushClipRect(
				{ min(clip0.x, clip1.x), min(clip0.y, clip1.y) },
				{ max(clip0.x, clip1.x), max(clip0.y, clip1.y) }, true);

			// The Background fills with its own colour (apertureFillColour,
			// #34); the region clip trims that fill to this stretch of the
			// aperture.
			renderSector(region.background, backLayer, LayerRenderStyle::Aperture, false,
				BackLocationColour, drawList);

			drawList->PopClipRect();
			drawList->AddDrawCmd();
		}
	}
	else if (backSector)
	{
		// The glass shows the Background's own colour, not the generic
		// back-layer tint (#34). A back Sector that carries no colour of its
		// own keeps the tint.
		auto const ownColour = apertureFillColour(*backSector);
		auto const apertureColour = ownColour
			? Colour(ownColour->r, ownColour->g, ownColour->b, 255)
			: BackLocationColour;

		renderSector(backSector, core::layerBehind(layer), LayerRenderStyle::Aperture, false,
			apertureColour, drawList);
	}
	else
	{
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, Colour(0, 0, 0));
	}

	drawList->PopClipRect();
	drawList->AddDrawCmd();
}


void renderWindowFrosted(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	renderWindowClear(window, layer, style, drawList);
}


void renderWindowTinted(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	renderWindowClear(window, layer, style, drawList);
}


void renderBoothWindow(shared_ptr<const core::BoothWindow> booth, uint32_t layer,
	LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Wireframe)
	{
		renderWindowClear(booth, layer, style, drawList);
		return;
	}
	if (!isDrawnSolid(style)) return;
	core::Vector2 from, to;
	booth->getFullShape(from, to);
	transformPosition(from); transformPosition(to);
	Vec2 topLeft{min(from.x, to.x), min(from.y, to.y)};
	Vec2 bottomRight{max(from.x, to.x), max(from.y, to.y)};
	// These insets are the transparent centre of the supplied 52 x 48 frame.
	float const width = bottomRight.x - topLeft.x, height = bottomRight.y - topLeft.y;
	Vec2 innerMin{topLeft.x + width * 5.0f / 52.0f, topLeft.y + height * 5.0f / 48.0f};
	Vec2 innerMax{bottomRight.x - width * 5.0f / 52.0f, bottomRight.y - height * 5.0f / 48.0f};
	float const travel = (innerMax.y - innerMin.y) * booth->getProgress();
	if (booth->getProgress() > 0.0f)
	{
		drawList->PushClipRect({innerMin.x, innerMax.y - travel}, innerMax, true);
		renderWindowClear(booth, layer, style, drawList);
		drawList->PopClipRect();
	}
	// Only the shutter translates. Its clip intersects any enclosing aperture.
	drawList->PushClipRect(innerMin, innerMax, true);
	Vec2 shutterMin{innerMin.x, innerMin.y - travel};
	Vec2 shutterMax{innerMax.x, innerMax.y - travel};
	if (!drawObjectSprite("booth-window-shutter", drawList, shutterMin, shutterMax))
		drawList->AddRectFilled(shutterMin, shutterMax, Colour(90, 106, 116));
	drawList->PopClipRect();
	if (!drawObjectSprite("booth-window-open", drawList, topLeft, bottomRight))
		drawList->AddRect(topLeft, bottomRight, Colour(148, 162, 170), 0.0f, 0, 2.0f);
}

void renderWindow(shared_ptr<const core::Window> window, uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto windowStyle = window->getStyle();

	bool const booth = window->isBoothWindow();
	if (booth) renderBoothWindow(static_pointer_cast<const core::BoothWindow>(window), layer, style, drawList);
	else switch (windowStyle)
	{
	case core::Window::Style::Clear:
		renderWindowClear(window, layer, style, drawList);
		break;

	case core::Window::Style::Frosted:
		renderWindowFrosted(window, layer, style, drawList);
		break;

	case core::Window::Style::Tinted:
		renderWindowTinted(window, layer, style, drawList);
		break;
	}

	if (!booth && style == LayerRenderStyle::Solid && hasObjectTileset())
	{
		core::Vector2 from, to;
		window->getFullShape(from, to);
		transformPosition(from);
		transformPosition(to);
		auto const sprite = windowStyle == core::Window::Style::Clear ? "window-clear"
			: windowStyle == core::Window::Style::Frosted ? "window-frosted" : "window-tinted";
		drawObjectSprite(sprite, drawList, {from.x, from.y}, {to.x, to.y});
	}


}


void renderPhysicalControl(shared_ptr<const core::Button> button, uint32_t /* layer */, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;
	button->getFullShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);

	// A Button whose controlled object is Broken renders red, never the enabled
	// green, so a failed threshold is visible from its control.
	auto const broken = gRenderWorld
		&& gRenderWorld->isControlTargetBroken(button->getInteractionPointId());
	auto colour = broken ? Colour(255, 0, 0)
		: button->isEnabled() ? Colour(0, 255, 128) : Colour(192, 128, 128);
	auto point = gRenderWorld ? gRenderWorld->lookupInteractionPoint(button->getInteractionPointId()).entity : nullptr;
	auto unit = point ? gRenderWorld->lookupDumbwaiter(point->getDumbwaiterOwner()) : nullptr;
	if (unit && !broken)
	{
		auto stop = unit->getLandingButton(0) == button->getInteractionPointId() ? 0u : 1u;
		colour = unit->isBusy() ? Colour(220, 80, 80)
			: unit->getButtonState(stop) == core::DumbwaiterButtonState::Here
				? Colour(80, 200, 120) : Colour(200, 160, 80);
	}

	// A Button seen from the Layer its controlling threshold was authored on is
	// filled solid; from every other Layer it contributes the outline only,
	// exactly like the Door it stands beside.
	if (style == LayerRenderStyle::Solid)
	{
		if (unit || broken || !drawObjectSprite(button->isEnabled() ? "button-enabled" : "button-disabled",
			drawList, {bounds0.x, bounds0.y}, {bounds1.x, bounds1.y}))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}

}


void renderWalkway(shared_ptr<const core::Walkway> walkway, uint32_t /* layer */, LayerRenderStyle style,
	WorldDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid) return;
	core::Vector2 bounds0, bounds1;

	walkway->getFullShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = Colour(64, 64, 64);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, colour,
		2.0f);
}


void renderForceBridge(shared_ptr<const core::ForceBridge> forceBridge, uint32_t /* layer */,
	LayerRenderStyle style, WorldDrawList* drawList)
{
	if (style != LayerRenderStyle::Solid)
	{
		return;
	}
	core::Vector2 bounds0, bounds1;

	forceBridge->getCurrentShape(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);
	drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y },
		Colour(0, 255, 0), 2.0f);
}


void renderLadder(shared_ptr<const core::Ladder> ladder, uint32_t /* layer */, LayerRenderStyle /* style */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	ladder->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LadderColour;
	drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderLift(shared_ptr<const core::Lift> lift, uint32_t /* layer */, LayerRenderStyle /* style */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	lift->getCurrentShape(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	auto colour = LiftColour;
	if (!drawObjectSprite("lift-car", drawList,
		{ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }))
		drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
}


void renderPlatformLift(shared_ptr<const core::LiftSectorObject> const& platformLift,
	uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	// The moving platform is only a thin slab, so also show the full authored
	// shaft occupied by its stops. Keep this outline subdued so Walkways and the
	// platform itself remain the dominant geometry.
	core::Vector2 bounds0, bounds1;
	platformLift->getBounds(bounds0, bounds1);
	transformPosition(bounds0);
	transformPosition(bounds1);
	Vec2 topLeft{ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) };
	Vec2 bottomRight{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) };
	drawList->AddRect(topLeft, bottomRight, Colour(128, 128, 192, 72));

	core::Vector2 car0, car1;
	platformLift->getLift()->getCurrentShape(car0, car1);
	transformPosition(car0);
	transformPosition(car1);
	if (!drawObjectSprite("platform-lift", drawList, {car0.x, car0.y}, {car1.x, car1.y}))
		renderLift(platformLift->getLift(), layer, style, drawList);
}


void renderShuttle(shared_ptr<const core::Shuttle> shuttle, uint32_t /* layer */, LayerRenderStyle /* style */, WorldDrawList* drawList)
{
	core::Vector2 bounds0, bounds1;

	shuttle->getCurrentShape(bounds0, bounds1);

	auto const numCars = shuttle->getNumCars();
	auto const carWidth = shuttle->getCarWidth();
	auto const colour = ShuttleColour;

	auto drawPart = [&](char const* sprite, core::Vector2 part0, core::Vector2 part1)
	{
		transformPosition(part0);
		transformPosition(part1);
		if (!drawObjectSprite(sprite, drawList, { part0.x, part0.y }, { part1.x, part1.y }))
			drawList->AddRectFilled({ part0.x, part0.y }, { part1.x, part1.y }, colour);
	};

	for (uint32_t car = 0; car < numCars; ++car)
	{
		auto const carX = bounds0.x + car * (carWidth + 1);
		for (uint32_t cell = 0; cell < carWidth; ++cell)
		{
			auto const* sprite = cell == 0 ? "shuttle-car-left"
				: cell + 1 == carWidth ? "shuttle-car-right"
				: "shuttle-car-middle";
			drawPart(sprite, { carX + cell, bounds0.y },
				{ carX + cell + 1.0f, bounds1.y });
		}

		if (car + 1 < numCars)
		{
			auto const connectorX = carX + carWidth;
			drawPart("shuttle-car-connector", { connectorX, bounds0.y },
				{ connectorX + 1.0f, bounds1.y });
		}
	}
}


void renderStairwell(shared_ptr<const core::Stairwell> stairwell, uint32_t /* layer */, LayerRenderStyle style, WorldDrawList* drawList)
{
	core::Vector2 worldMin, worldMax;
	stairwell->getCurrentShape(worldMin, worldMax);

	auto screenMin = worldMin;
	auto screenMax = worldMax;
	transformPosition(screenMin);
	transformPosition(screenMax);
	Vec2 topLeft{ min(screenMin.x, screenMax.x), min(screenMin.y, screenMax.y) };
	Vec2 bottomRight{ max(screenMin.x, screenMax.x), max(screenMin.y, screenMax.y) };
	drawList->AddRectFilled(topLeft, bottomRight, Colour(255, 255, 255));

	auto toScreen = [&](core::Vector2 point)
	{
		point += worldMin;
		transformPosition(point);
		return Vec2{ point.x, point.y };
	};

	// Stairwell::getLevelPath is also used to place the Graph vertices. Build one
	// continuous polyline so adjacent flights share their exact level endpoint.
	vector<Vec2> pathPoints;
	if (stairwell->getLevelsHigh() > 1)
		pathPoints.reserve(1 + (stairwell->getLevelsHigh() - 1) * 3);
	for (uint32_t level = 0; level + 1 < stairwell->getLevelsHigh(); ++level)
	{
		auto path = stairwell->getLevelPath(level);
		if (pathPoints.empty()) pathPoints.push_back(toScreen(path[0]));
		pathPoints.push_back(toScreen(path[1]));
		pathPoints.push_back(toScreen(path[2]));
		pathPoints.push_back(toScreen(path[3]));
	}
	for (uint32_t level = 0; level < stairwell->getLevelsHigh(); ++level)
		for (auto const& platform : stairwell->getLandingPaths(level))
		{
			array<Vec2, 2> points{toScreen(platform[0]), toScreen(platform[1])};
			drawList->AddPolyline(points.data(), 2, Colour(223, 223, 223), DrawFlagsNone, 10.0f);
			drawList->AddPolyline(points.data(), 2, Colour(0, 0, 0), DrawFlagsNone, 4.0f);
		}
	if (pathPoints.size() >= 2)
	{
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			Colour(223, 223, 223), DrawFlagsNone, 10.0f);
		drawList->AddPolyline(pathPoints.data(), (int)pathPoints.size(),
			Colour(0, 0, 0), DrawFlagsNone, 4.0f);
	}
	// Outline each permanently open doorway both from inside the Stairwell
	// and through the landing aperture. The aperture pass already clips each
	// box to its landing; these frames never reveal the fore Layer recursively.
	if (isDrawnSolid(style))
		for (uint32_t level = 0; level < stairwell->getLevelsHigh(); ++level)
		{
			auto low = toScreen({1.f - CORE_STAIRWELL_PASSAGE_WIDTH * .5f, float(level)});
			auto high = toScreen({1.f + CORE_STAIRWELL_PASSAGE_WIDTH * .5f,
				float(level) + CORE_STAIRWELL_DOORWAY_HEIGHT});
			drawList->AddRect({low.x, high.y}, {high.x, low.y}, Colour(0, 0, 0));
		}
}


void renderStaircase(shared_ptr<const core::Staircase> staircase, WorldDrawList* drawList)
{
	auto const path = staircase->getPath();
	core::Vector2 origin, ignored;
	staircase->getCurrentShape(origin, ignored);
	uint32_t const count = staircase->getStepCount();
	auto toScreen = [&](float x, float y)
	{
		core::Vector2 point{ origin.x + x, origin.y + y };
		transformPosition(point);
		return Vec2{ point.x, point.y };
	};
	auto drawSteps = [&](auto const& segments, PackedColourValue colour, float thickness)
	{
		for (auto const& segment : segments)
			drawList->AddPolyline(segment.data(), (int)segment.size(), colour,
				DrawFlagsNone, thickness);
	};

	if (staircase->isEscalator())
	{
		// Each L-shaped tread/riser advances along the incline at the Escalator's
		// world speed, then wraps to the opposite endpoint.
		vector<array<Vec2, 3>> segments;
		segments.reserve(count);
		float const step = 1.0f / (float)count;
		float const phase = staircase->getAnimationPhase();
		for (uint32_t i = 0; i < count; ++i)
		{
			float t = fmod((float)i * step + phase, 1.0f);
			if (t < 0.0f) t += 1.0f;
			float const previous = max(0.0f, t - step);
			float const x0 = path[0].x + (path[1].x - path[0].x) * previous;
			float const x1 = path[0].x + (path[1].x - path[0].x) * t;
			float const y0 = path[0].y + (path[1].y - path[0].y) * previous;
			float const y1 = path[0].y + (path[1].y - path[0].y) * t;
			segments.push_back({ toScreen(x0, y0), toScreen(x1, y0), toScreen(x1, y1) });
		}
		drawSteps(segments, packColour(32, 32, 32, 255), 10.0f);
		drawSteps(segments, packColour(220, 220, 220, 255), 6.0f);
		return;
	}

	vector<Vec2> points;
	points.reserve(count * 2 + 1);
	points.push_back(toScreen(path[0].x, path[0].y));
	for (uint32_t i = 0; i < count; ++i)
	{
		float const t0 = (float)i / (float)count;
		float const t1 = (float)(i + 1) / (float)count;
		float const x1 = path[0].x + (path[1].x - path[0].x) * t1;
		float const y0 = path[0].y + (path[1].y - path[0].y) * t0;
		float const y1 = path[0].y + (path[1].y - path[0].y) * t1;
		points.push_back(toScreen(x1, y0));
		points.push_back(toScreen(x1, y1));
	}
	drawList->AddPolyline(points.data(), (int)points.size(), packColour(32, 32, 32, 255),
		DrawFlagsNone, 10.0f);
	drawList->AddPolyline(points.data(), (int)points.size(), packColour(220, 220, 220, 255),
		DrawFlagsNone, 6.0f);
}


void renderSectorObjects(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, int flags, WorldDrawList* drawList)
{
	// Sort so that Ladders and Lifts are rendered first, as these need to be behind everything else.
	auto sortedObjects = sector->getSortedObjects([](auto obj1, auto obj2)
	{
		if (obj1->getObjectType() == core::SectorObjectType::Ladder || obj1->getObjectType() == core::SectorObjectType::Lift)
		{
			if (obj2->getObjectType() == core::SectorObjectType::Ladder || obj2->getObjectType() == core::SectorObjectType::Lift)
			{
				if (obj1->getCellX() == obj1->getCellY())
				{
					return obj1->getCellY() < obj2->getCellY();
				}
				else
				{
					return obj1->getCellX() < obj2->getCellX();
				}
			}
			else
			{
				return true;
			}
		}
		else
		{
			return false;
		}
	});

	// A Door or Window is authored on the front Layer of its pair, but the same
	// object is registered on the Sector on the Layer behind as well. Its aperture
	// is only drawn while rendering from the Layer it is authored on; from the
	// Layer behind it is drawn as an outline.
	auto thresholdStyle = [&](shared_ptr<const core::Sector> authoredSector)
	{
		return isDrawnSolid(style) && sector == authoredSector
			? LayerRenderStyle::Solid
			: LayerRenderStyle::Wireframe;
	};

	for (auto object : sortedObjects)
	{
		switch (object->getObjectType())
		{
		case core::SectorObjectType::BulkheadDoor:
			// Like ordinary Door outlines, same-Layer thresholds and their
			// condition warning remain visible in the wireframe pass.
			if (flags & (style == LayerRenderStyle::Wireframe
				? RENDER_SECTOR_OBJECTS_BEHIND : RENDER_SECTOR_OBJECTS_INFRONT))
			{
				renderBulkheadDoor(static_pointer_cast<const core::BulkheadDoorSectorObject>(object)->getDoor(), layer, style, drawList);
			}
			break;

		case core::SectorObjectType::Door:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
				if (style == LayerRenderStyle::Aperture && door->getFrontSector() != sector) break;
				renderDoor(door, layer, thresholdStyle(door->getFrontSector()), drawList);
			}
			break;


		case core::SectorObjectType::ForceBridge:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderForceBridge(static_pointer_cast<const core::ForceBridgeSectorObject>(object)->getForceBridge(), layer, style, drawList);
			}
			break;

		case core::SectorObjectType::Ladder:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderLadder(static_pointer_cast<const core::LadderSectorObject>(object)->getLadder(), layer, style, drawList);
			}
			break;

		case core::SectorObjectType::Lift:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderPlatformLift(static_pointer_cast<const core::LiftSectorObject>(object),
					layer, style, drawList);
			}
			break;

		case core::SectorObjectType::InteractionPoint:
			// Physical controls draw with the thresholds, not the in-front objects:
			// a Door Button renders exactly like its Door - solid on the Layer the
			// Door was authored on, an outline from every other Layer - so the
			// Button on the far side of a Door is never solid and shows through in
			// the wireframe overlay pass. Ordinary controls carry no threshold
			// Layer and stay solid wherever their own Layer is drawn, but a
			// non-solid pass contributes outlines only, never a fill.
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto button = static_pointer_cast<const core::Button>(object->_getObject());
				auto const thresholdLayer = button->getThresholdLayer();
				auto controlStyle = thresholdLayer == ~0u || thresholdLayer == layer
					? LayerRenderStyle::Solid : LayerRenderStyle::Wireframe;
				if (style != LayerRenderStyle::Solid) controlStyle = LayerRenderStyle::Wireframe;
				renderPhysicalControl(button, layer, controlStyle, drawList);
			}
			break;

		case core::SectorObjectType::AccessPanel:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto panel = static_pointer_cast<const core::AccessPanelSectorObject>(object)->getPanel();
				core::Vector2 min, max; panel->getFullShape(min, max);
				bool degenerate = panel->getSize().x == 0 || panel->getSize().y == 0;
				if (degenerate) panel->getSelectionShape(min, max);
				transformPosition(min); transformPosition(max);
				if (!degenerate && isDrawnSolid(style))
					drawList->AddRectFilled({min.x, max.y}, {max.x, min.y}, packColour(22, 30, 40, 255));
				if (panel->getProgress() > 0)
				{
					// An inset cross distinguishes Open even in wireframe and zero-area indicators.
					drawList->AddLine({min.x, max.y}, {max.x, min.y}, packColour(100, 210, 180, 255), 2);
					drawList->AddLine({min.x, min.y}, {max.x, max.y}, packColour(100, 210, 180, 255), 2);
				}
				// OpenUp: the leaf retracts into the top edge, revealing the bottom first.
				if (!degenerate && panel->getProgress() < 1)
				{
					auto bottom = min.y + (max.y - min.y) * panel->getProgress();
					if (isDrawnSolid(style))
						drawList->AddRectFilled({min.x, max.y}, {max.x, bottom}, packColour(78, 92, 110, 255));
					if (panel->getProgress() > 0)
						drawList->AddRect({min.x, max.y}, {max.x, bottom}, packColour(165, 180, 198, 255), 0, 0, 2);
				}
				drawList->AddRect({min.x, max.y}, {max.x, min.y},
					packColour(165, 180, 198, 255), 0, 0, 2);
			}
			break;

		case core::SectorObjectType::Marker:
			break;

		case core::SectorObjectType::Walkway:
			if (flags & RENDER_SECTOR_OBJECTS_INFRONT)
			{
				renderWalkway(static_pointer_cast<const core::WalkwaySectorObject>(object)->getWalkway(), layer, style, drawList);
			}
			break;

		case core::SectorObjectType::BoothWindow:
		case core::SectorObjectType::Window:
			if (flags & RENDER_SECTOR_OBJECTS_BEHIND)
			{
				auto window = static_pointer_cast<const core::WindowSectorObject>(object)->getWindow();
				if (style == LayerRenderStyle::Aperture && window->getFrontSector() != sector) break;
				renderWindow(window, layer, thresholdStyle(window->getFrontSector()), drawList);
			}
			break;

		default:
			break;
		}
	}
}


//
// Clipped transits from the Layer behind draw over the viewed Layer's Locations.
// Redraw what the viewed Layer owns and must keep in front of them: its own
// thresholds first, so a closed Door still occludes the Transit standing behind
// it; then its physical controls; then its Agents, so no control can be painted
// in front of an Agent.
//
void renderThresholdsControlsAndAgentsAboveTransit(vector<shared_ptr<const core::Sector>> const& sectors,
	uint32_t layer, WorldDrawList* drawList)
{
	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::Door)
				continue;

			// Only a Door authored on this Layer opens into the Layer behind, and so
			// covers something that Layer draws. The same Door object registered on the
			// Sector behind is seen from the other side and is not occluder here.
			auto door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			if (door->getFrontSector() != sector)
				continue;

			renderDoor(door, layer, LayerRenderStyle::Solid, drawList);
		}
	}

	for (auto const& sector : sectors)
	{
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			auto button = static_pointer_cast<const core::Button>(object->_getObject());
			auto const thresholdLayer = button->getThresholdLayer();
			auto const controlStyle = thresholdLayer == ~0u || thresholdLayer == layer
				? LayerRenderStyle::Solid : LayerRenderStyle::Wireframe;
			renderPhysicalControl(button, layer, controlStyle, drawList);
		}
	}

	// Controls are redrawn above clipped transits. Restore the sector's Agents
	// afterwards so no physical control can be painted in front of them.
	for (auto const& sector : sectors)
	{
		// Chamber occupants were already drawn below their processing overlay.
		// This foreground restoration must not repaint them above the white quad.
		if (auto chamber = dynamic_pointer_cast<const core::ChamberTransit>(sector);
			chamber && chamber->getDecontaminationOpacity() > 0) continue;
		renderSectorDepthContent(sector, drawList);
	}
}


PackedColourValue agentRenderColour(core::Agent const& agent)
{
	auto const colour = agent.getEffectiveColour().value;
	return PackedColourValue(Colour(colour.r, colour.g, colour.b));
}

void renderAgent(core::Agent const* agent, WorldDrawList* drawList)
{
    core::Vector2 low, high;
    agent->getBounds().getCurrentShape(low, high);
    low.x += agent->getPoseRenderXOffset(); high.x += agent->getPoseRenderXOffset();
    low.y += agent->getPoseRenderYOffset(); high.y += agent->getPoseRenderYOffset();
    transformPosition(low); transformPosition(high);
    auto size = objectSpriteSize(agent->getPoseImageTile().c_str());
    auto standing = objectSpriteSize(agent->getPhysicalBaseline().poses.at(core::Pose::Standing).imageTile.c_str());
    if (!size || !standing) throw std::runtime_error("Missing Agent pose artwork");
    auto const scale = std::min(agent->getPhysicalBaseline().width * CORE_CELL_WIDTH_PIXELS / standing->x,
        agent->getPhysicalBaseline().standingHeight * CORE_LEVEL_HEIGHT_PIXELS / standing->y)
        * agent->getEffectiveHeightModifier().value * gRenderSettings.worldZoom;
    Vec2 minimum{(low.x + high.x - size->x * scale) * 0.5f, low.y - size->y * scale};
    drawObjectSprite(agent->getPoseImageTile().c_str(), drawList, minimum,
        {minimum.x + size->x * scale, minimum.y + size->y * scale}, agentRenderColour(*agent));
}

void renderSectorDepthContent(shared_ptr<const core::Sector> sector, WorldDrawList* drawList)
{
	// One ordered content pass, under the caller's existing Layer/aperture clip.
	struct Content { int depth; core::FurnitureInstance const* furniture; core::Agent const* agent; };
	vector<Content> content;
	if (gRenderWorld && gRenderWorld->furnitureCatalogue())
		for (auto const& instance : gRenderWorld->furniture())
			if (instance.sector == sector->getIndex()) content.push_back({ instance.localDepth, &instance, nullptr });
	for (auto agent : sector->getAgents()) content.push_back({ agent->getLocalDepth(), nullptr, agent });
	stable_sort(content.begin(), content.end(), [](auto const& a, auto const& b) {
		if (a.depth != b.depth) return a.depth > b.depth;
		return a.furniture && !b.furniture;
	});
	for (auto const& entry : content)
	{
		if (entry.agent) { renderAgent(entry.agent, drawList); continue; }
		auto const& instance = *entry.furniture;
		auto const& definition = *gRenderWorld->furnitureCatalogue()->definition(instance.definitionKey);
		for (auto const& tile : definition.tiles)
		{
			core::Vector2 top{ sector->getCellX() + instance.x + tile.x, sector->getCellY() + instance.y + tile.y + 1 };
			core::Vector2 bottom{ top.x + 1, top.y - 1 };
			transformPosition(top); transformPosition(bottom);
			if (!drawObjectSprite(tile.image.c_str(), drawList, { top.x, top.y }, { bottom.x, bottom.y }))
				throw std::runtime_error("Missing Furniture Image-set region: " + tile.image);
		}
	}
}


void renderSector(shared_ptr<const core::Sector> sector, uint32_t layer, LayerRenderStyle style, bool renderEdges, Colour colour, WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	// A Background is drawn with its own raw colour. The lights-off tint is
	// bypassed: a Background has no lights to switch, and tinting it would
	// silently override the colour the user picked and make the picker
	// untrustworthy. A Facade follows the same rendering rule (ADR 0003):
	// its user-picked colour is authoritative even though, unlike a
	// Background, its lights-off state still means what it means for the
	// agents and objects inside it.
	auto const sectorType = sector->getType();
	auto const ownColour = rendersAsFlatColour(sectorType);

	if (!bypassesLightsOffTint(sectorType) && !sector->areLightsOn())
	{
		colour = LightsOffColour;
	}

	// Render sector area
	core::Vector2 bounds0, bounds1;

	sector->getBounds(bounds0, bounds1);

	transformPosition(bounds0);
	transformPosition(bounds1);

	// The Sector's surface fill. A Sector that carries no colour of its own
	// takes the Layer's. A flat-colour Sector fills with its own colour here,
	// where the generic fill sits - before the BEHIND object pass - so a Door
	// or Window authored on a Facade keeps its aperture instead of being
	// painted over by the flat fill (#49). The wireframe overlay contributes
	// the outline only, never a fill (ADR 0002).
	if (ownColour)
	{
		auto const surface = flatSurfaceColour(*sector);
		assert(surface.has_value());
		auto const fill = Colour(surface->r, surface->g, surface->b, 255);

		if (shouldFillBackground(style))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
		else if (shouldOutlineBackground(style))
			drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, fill);
	}
	else if (style == LayerRenderStyle::Wireframe)
	{
		drawList->AddRect({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}
	else
	{
		std::string kind;
		switch (sectorType)
		{
		case core::SectorType::Location: kind = static_pointer_cast<const core::Location>(sector)->isCorridor() ? "corridor" : "room"; break;
		case core::SectorType::Airlock:
		case core::SectorType::Chamber: kind = "corridor"; break;
		case core::SectorType::Ladder: kind = "ladder"; break;
		case core::SectorType::Lift:
		case core::SectorType::Dumbwaiter: kind = "lift"; break;
		// The rail corridor is static architecture; carriage images are rendered
		// separately at the Shuttle's current position.
		case core::SectorType::Shuttle: kind = "lift"; break;
		case core::SectorType::Stairwell: kind = "stairwell"; break;
		// Staircase steps are rendered separately from their Sector surface. Use
		// the same plain shaft tile as a Stairwell rather than painting a second,
		// fixed staircase image underneath the procedural geometry.
		case core::SectorType::Staircase: kind = "stairwell"; break;
		default: break;
		}
		if (!drawSectorTileSurface(kind, drawList, {bounds0.x, bounds1.y},
			{bounds1.x, bounds0.y}, colour, gRenderSettings.worldZoom))
			drawList->AddRectFilled({ bounds0.x, bounds0.y }, { bounds1.x, bounds1.y }, colour);
	}

	// Thresholds are normally drawn before the sector-specific geometry so their
	// apertures are not painted over. On a Shuttle's own Layer its Doors are seen
	// from the back and contribute wireframes rather than apertures; defer those
	// outlines until after the filled carriage so the carriage cannot hide them.
	// Apertures may reveal further thresholds, but only those authored on the
	// current Layer: renderSectorObjects skips back-facing thresholds to avoid
	// re-entering the same aperture. Nested scissor rectangles keep the view local.
	auto const backObjectsFollowGeometry = sectorType == core::SectorType::Shuttle
		&& isDrawnSolid(style);
	if (!backObjectsFollowGeometry)
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	// Sector-specific
	switch (sector->getType())
	{
	case core::SectorType::Dumbwaiter:
		if (isDrawnSolid(style))
		{
			auto unit = static_pointer_cast<const core::Dumbwaiter>(sector);
			auto position = unit->getCarPosition();
			core::Vector2 low{position.x + 0.1f, position.y + 0.18f};
			core::Vector2 high{position.x + 0.9f, position.y + 0.55f};
			transformPosition(low); transformPosition(high);
			// Procedural empty car: normal sector recursion supplies depth and clipping.
			drawList->AddRectFilled({low.x, high.y}, {high.x, low.y}, packColour(180, 190, 205, 255));
			drawList->AddRect({low.x, high.y}, {high.x, low.y}, packColour(60, 70, 85, 255), 0, 0, 2);
		}
		break;
	case core::SectorType::Ladder:
		if (shouldRenderLadderGeometry(style))
			renderLadder(static_pointer_cast<const core::LadderTransit>(sector)->getLadder(),
				layer, style, drawList);
		break;

	case core::SectorType::Lift:
		// Like every other Transit, a wireframe Layer contributes the Sector outline
		// only. Painting the car filled here leaked it over the viewed Layer.
		if (isDrawnSolid(style))
			renderLift(static_pointer_cast<const core::LiftTransit>(sector)->getLift(),
				layer, style, drawList);
		break;

	case core::SectorType::Shuttle:
		// A wireframe Layer contributes a sector outline only. Its vehicle appears
		// when the viewed Layer draws the sector itself, or through an aperture.
		if (isDrawnSolid(style))
			renderShuttle(static_pointer_cast<const core::ShuttleTransit>(sector)->getShuttle(),
				layer, style, drawList);
		break;

	case core::SectorType::Stairwell:
		if (shouldRenderStairwellGeometry(style))
			renderStairwell(static_pointer_cast<const core::StairwellTransit>(sector)->getStairwell(),
				layer, style, drawList);
		break;

	case core::SectorType::Staircase:
		if (isDrawnSolid(style))
			renderStaircase(static_pointer_cast<const core::StaircaseTransit>(sector)->getStaircase(), drawList);
		break;

	// Background and Facade carry no geometry of their own: their surface is
	// the flat fill emitted above, before the thresholds, so nothing here can
	// paint over an aperture (#49).
	default:
		break;
	}

	if (backObjectsFollowGeometry)
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_BEHIND, drawList);
	}

	// Objects
	if (isDrawnSolid(style))
	{
		renderSectorObjects(sector, layer, style, RENDER_SECTOR_OBJECTS_INFRONT, drawList);
	}

	// Transit occupants obey the same aperture rule as their transit. Door and
	// Window rendering call this with a clip rectangle active from the Layer in
	// front; the wireframe overlay pass must not expose them.
	if (shouldRenderSectorAgents(sector->getType(), style))
		renderSectorDepthContent(sector, drawList);

	// Scan overlays follow occupants, and share the production scan clock. The
	// triangular wave is independent of the chamber's authored travel direction.
	if (sector->getType() == core::SectorType::Chamber && isDrawnSolid(style))
	{
		auto chamber = static_pointer_cast<const core::ChamberTransit>(sector);
		if (chamber->getSubtype() == core::ChamberSubtype::SecurityScanner
			&& chamber->getPhase() == core::SecurityScannerPhase::Scanning)
		{
			float progress = chamber->getScanProgress();
			float sweep = 1.0f - std::abs(2.0f * progress - 1.0f);
			float x = bounds0.x + (bounds1.x - bounds0.x) * sweep;
			float y = bounds1.y + (bounds0.y - bounds1.y) * sweep;
			auto colour = packColour(255, 0, 0, 128);
			drawList->AddLine({ bounds0.x, y }, { bounds1.x, y }, colour, 4.0f);
			drawList->AddLine({ x, bounds1.y }, { x, bounds0.y }, colour, 4.0f);
		}
		else if (chamber->getDecontaminationOpacity() > 0)
		{
			auto alpha = (int)std::lround(chamber->getDecontaminationOpacity() * 255.0f);
			drawList->AddRectFilled({ bounds0.x, bounds1.y }, { bounds1.x, bounds0.y }, packColour(255, 255, 255, alpha));
			// Generated Doors belong to neighbouring Sectors and may already have
			// been painted. Restore their visible leaves above the interior effect.
			for (int side = 0; side < 2; ++side)
				renderBulkheadDoor(chamber->getDoor(side), layer, style, drawList);
		}
	}

	// Render ceiling
	// A Background has no floor, ceiling or walls - its colour is the whole
	// surface, and a per-sector black border would break the seamless surface
	// adjacent Backgrounds are meant to form. A Facade skips them for the
	// same flat-surface reason: its perimeter is open, so there are no walls
	// to draw, and the flat colour carries its extent (ADR 0003).
	if (renderEdges && !ownColour)
	{
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary("ceiling", drawList,
			{bounds0.x, bounds1.y}, {bounds1.x, bounds1.y + 2.0f}))
			drawList->AddLine({ bounds0.x, bounds1.y }, { bounds1.x, bounds1.y }, Colour(0, 0, 0), 2.0f);

		// Render floor
		if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary("floor", drawList,
			{bounds0.x, bounds0.y - 2.0f}, {bounds1.x, bounds0.y}))
			drawList->AddLine({ bounds0.x, bounds0.y }, { bounds1.x, bounds0.y }, Colour(0, 0, 0), 2.0f);

		// Render walls
		//
		// An open end takes away only the stretch of wall the neighbouring
		// Location actually shares, never the whole level: a Room level standing
		// taller than the Corridor it opens into keeps the wall above the
		// Corridor's ceiling, or the two Locations read as open to the void
		// above their own connection.
		//
		// The wireframe overlay passes the viewed Layer through too, so a wall on
		// the Layer behind does not draw across an opening the viewed Layer
		// has made - otherwise the removed wall comes back as the Sector behind
		// it, and the connection reads as closed again.
		int const viewLayer{ std::max(0, gRenderSettings.visibleLayer) };
		float height{ 0.0f };

		for (uint32_t y = 0; y < sector->getLevelsHigh(); ++y)
		{
			auto levelHeight = sector->getLevelHeight(y);

			core::Vector2 wallBounds0, wallBounds1;

			sector->getBounds(wallBounds0, wallBounds1);

			wallBounds0.y += height;
			wallBounds1.y = wallBounds0.y + levelHeight;

			for (int const side : { CORE_SIDE_LEFT, CORE_SIDE_RIGHT })
			{
				auto const x = side == CORE_SIDE_LEFT ? wallBounds0.x : wallBounds1.x;

				for (auto const& span : wallSpansToDraw(gRenderWorld, *sector, y, side, viewLayer))
				{
					core::Vector2 from{ x, span.y0 }, to{ x, span.y1 };

					transformPosition(from);
					transformPosition(to);

					float const left = side == CORE_SIDE_LEFT ? from.x : from.x - 2.0f;
					if (style == LayerRenderStyle::Wireframe || !drawSectorTileBoundary(
						side == CORE_SIDE_LEFT ? "left" : "right", drawList,
						{left, std::min(from.y, to.y)}, {left + 2.0f, std::max(from.y, to.y)}))
						drawList->AddLine({ from.x, from.y }, { to.x, to.y }, Colour(0, 0, 0), 2.0f);
				}
			}

			height += levelHeight;
		}
	}


}


void renderLocationContentAboveTransit(shared_ptr<const core::Sector> const& location,
	uint32_t layer, LayerRenderStyle style, WorldDrawList* drawList)
{
	if (isDrawnSolid(style))
	{
		renderSectorObjects(location, layer, LayerRenderStyle::Solid,
			RENDER_SECTOR_OBJECTS_BEHIND | RENDER_SECTOR_OBJECTS_INFRONT, drawList);
	}
	renderSectorDepthContent(location, drawList);
}

//
// Draws one Transit inside a single aperture of the viewed Layer. Everything the
// Transit contributes - Sector fill, geometry, and its own Agents - is clipped to
// that aperture by the caller, so a Transit on the Layer behind never fills over
// the Layer in front of it.
//
void renderTransitInAperture(shared_ptr<const core::Sector> const& transit, uint32_t behindLayer,
	TransitAperture const& aperture, WorldDrawList* drawList)
{
	// An aperture always looks through to the Layer behind the selection, so it
	// always carries the back-Layer colour.
	auto const colour = BackLocationColour;

	// A Staircase is drawn as its own polyline across the Location it crosses
	// rather than as a filled Sector, so it never contributes a Sector fill
	// through an aperture.
	if (transit->getType() == core::SectorType::Staircase)
	{
		renderStaircase(
			static_pointer_cast<const core::StaircaseTransit>(transit)->getStaircase(), drawList);
		renderSectorDepthContent(transit, drawList);
		return;
	}

	renderSector(transit, behindLayer, LayerRenderStyle::Aperture, false, colour, drawList);

	// A Ladder sits behind the contents of the Location it lands in, so those
	// contents are redrawn inside the same aperture.
	if (aperture.location && shouldRenderForeContentAfterTransit(transit->getType()))
	{
		renderLocationContentAboveTransit(aperture.location, aperture.location->getLayerIndex(),
			LayerRenderStyle::Aperture, drawList);
	}
}

//
// Draws one Transit of the Layer behind the selection, solid, through each of the
// apertures the viewed Layer gives it. A Transit with no aperture is not drawn.
//
void renderTransitThroughApertures(shared_ptr<const core::Sector> const& transit,
	uint32_t behindLayer, std::vector<TransitAperture> const& apertures, WorldDrawList* drawList)
{
	static_assert(shouldClipTransitToApertures(LayerRenderStyle::Aperture),
		"this is the clipped pass, so it must only draw a style which clips to apertures");

	if (!transit)
	{
		return;
	}

	for (auto const& aperture : apertures)
	{
		auto bounds0 = aperture.min;
		auto bounds1 = aperture.max;
		transformPosition(bounds0);
		transformPosition(bounds1);

		drawList->AddDrawCmd();

		// Canvas clipping expects ascending Y coordinates, but we have flipped them for rendering
		drawList->PushClipRect({ min(bounds0.x, bounds1.x), min(bounds0.y, bounds1.y) },
			{ max(bounds0.x, bounds1.x), max(bounds0.y, bounds1.y) }, true);

		renderTransitInAperture(transit, behindLayer, aperture, drawList);

		drawList->PopClipRect();
		drawList->AddDrawCmd();
	}
}


//
// The Sectors of one Layer that the current viewport actually sees.
//
// Every render pass must cull with the same bounds (#58): the visible world
// origin is (-xOffset, -yOffset) - the scrollbars drive both - over the
// viewport's own width and height. Passing a hard-coded Y origin here is what
// made high levels vanish when scrolled into view.
//
std::vector<std::shared_ptr<const core::Sector>> viewportSectors(
	std::shared_ptr<const core::World> const& world, uint32_t layer)
{
	auto const inverseZoom = 1.0f / gRenderSettings.worldZoom;
	return world->getSectorsInBounds(layer,
		-gRenderSettings.xOffset * inverseZoom, -gRenderSettings.yOffset * inverseZoom,
		gRenderSettings.worldViewportWidth * inverseZoom,
		gRenderSettings.worldViewportHeight * inverseZoom);
}


//
// Draws every Sector one Layer contributes, in the given style.
//
void renderSectors(shared_ptr<const core::World> world, uint32_t layer, LayerRenderStyle style,
	WorldDrawList* drawList)
{
	if (style == LayerRenderStyle::Hidden)
	{
		return;
	}

	auto const sectors = viewportSectors(world, layer);

	auto const colour = style == LayerRenderStyle::Solid ? ForeLocationColour : BackLocationColour;

	for (auto sector : sectors)
	{
		renderSector(sector, layer, style, true, colour, drawList);
	}
}


//
// Draws the Transits of the Layer directly behind the selection, solid, clipped to
// the apertures the viewed Layer gives them. The viewed Layer's Locations are
// the apertures onto the Layer behind, so a Transit never fills over ground the
// Layer in front of it does not open up. A Transit with a Door or clear Window in
// front of it is drawn by that threshold's own aperture as well.
//
// This pass runs whether or not the wireframe overlay is on: the overlay adds the
// Layer behind's outlines, it is not what makes that Layer visible.
//
void renderBehindLayerTransits(shared_ptr<const core::World> world, uint32_t behindLayer,
	std::vector<std::shared_ptr<const core::Sector>> const& viewSectors, WorldDrawList* drawList)
{
	auto const viewLayer = core::layerInFront(behindLayer);

	auto const transits = viewportSectors(world, behindLayer);

	for (auto const& transit : transits)
	{
		renderTransitThroughApertures(transit, behindLayer,
			transitApertures(transit, viewLayer, viewSectors), drawList);
	}
}


void renderWorld(shared_ptr<const core::World> world, WorldDrawList* drawList)
{
	// Own the World for exactly this call. The scope restores the previous
	// World on every exit, so closing the last document releases it as soon as
	// the viewport stops rendering it and an early return cannot leave it
	// reachable by the Sector-level passes.
	RenderWorldScope renderWorldScope(world);
	if (!drawList || !world || world->getLayerCount() == 0) return;

	auto const layerCount = world->getLayerCount();
	auto const viewLayer = static_cast<uint32_t>(clamp(gRenderSettings.visibleLayer, 0,
		static_cast<int>(layerCount) - 1));

	drawList->PushClipRect({ gRenderSettings.worldViewportX, gRenderSettings.worldViewportY },
		{ gRenderSettings.worldViewportX + gRenderSettings.worldViewportWidth,
			gRenderSettings.worldViewportY + gRenderSettings.worldViewportHeight }, true);

	for (auto const& pass : renderPasses(viewLayer, layerCount))
	{
		switch (pass.style)
		{
		case LayerRenderStyle::Solid:
			// Only the viewed Layer is drawn whole. Deeper geometry enters through
			// the clipped aperture paths below or through nested thresholds.
			renderSectors(world, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Aperture:
		{
			auto const frontLayer = core::layerInFront(pass.layer);
			auto const frontSectors = viewportSectors(world, frontLayer);
			renderBehindLayerTransits(world, pass.layer, frontSectors, drawList);
			// Clipped Transits draw over the viewed Layer's Locations. Restore
			// that Layer's thresholds, controls, and Agents in front of them.
			renderThresholdsControlsAndAgentsAboveTransit(frontSectors, frontLayer, drawList);
			break;
		}

		case LayerRenderStyle::Wireframe:
			// The wireframe overlay x-rays the Layer directly behind: its whole
			// footprint is outlined over the viewed Layer. Outlines only - never a
			// fill, Transit geometry, or the Agents inside them.
			renderSectors(world, pass.layer, pass.style, drawList);
			break;

		case LayerRenderStyle::Hidden:
		default:
			break;
		}
	}

	if (auto selector = world->getPlayerTransportSelector())
	{
		auto anchor = selector->anchor; transformPosition(anchor);
		pf::drawPlayerTransportSelector(*selector, anchor.x, anchor.y,
			[&](float x0, float y0, float x1, float y1, uint32_t colour) { drawList->AddRectFilled({x0,y0}, {x1,y1}, colour); },
			[&](float x0, float y0, float x1, float y1, uint32_t colour) { drawList->AddRect({x0,y0}, {x1,y1}, colour); });
	}
	drawList->PopClipRect();
}

RenderSettings gRenderSettings;

WorldDrawList drawWorld(std::shared_ptr<const core::World> const& world, float width, float height)
{
    WorldDrawList commands(WorldDrawList::ClipRectangle{{0, 0}, {width, height}});
    if (!world || width <= 0 || height <= 0 || world->getLayerCount() == 0) return commands;
    gRenderSettings = {};
    gRenderSettings.worldViewportWidth = width;
    gRenderSettings.worldViewportHeight = height;
    auto sectors = world->getSectors(0);
    if (!sectors.empty()) {
        core::Vector2 minimum, maximum;
        sectors.front()->getBounds(minimum, maximum);
        for (auto const& sector : sectors) {
            core::Vector2 low, high; sector->getBounds(low, high);
            minimum.x = std::min(minimum.x, low.x); minimum.y = std::min(minimum.y, low.y);
            maximum.x = std::max(maximum.x, high.x); maximum.y = std::max(maximum.y, high.y);
        }
        float const contentWidth = std::max(1.0f, (maximum.x - minimum.x) * CORE_CELL_WIDTH_PIXELS);
        float const contentHeight = std::max(1.0f, (maximum.y - minimum.y) * CORE_LEVEL_HEIGHT_PIXELS);
        float const zoom = std::max(0.001f, std::min({1.0f, width * 0.9f / contentWidth,
                                                   height * 0.9f / contentHeight}));
        gRenderSettings.worldZoom = zoom;
        gRenderSettings.xOffset = (width - contentWidth * zoom) * 0.5f - minimum.x * CORE_CELL_WIDTH_PIXELS * zoom;
        gRenderSettings.yOffset = (height - contentHeight * zoom) * 0.5f - minimum.y * CORE_LEVEL_HEIGHT_PIXELS * zoom;
    }
    if (auto player = world->lookupAgent(world->getPlayerControlledAgent()); player) {
        auto const* agent = player.entity;
        auto centre = agent->getGlobalPosition();
        centre.y += agent->getHeight() * 0.5f;
        gRenderSettings.visibleLayer = static_cast<int>(agent->getSector()->getLayerIndex());
        gRenderSettings.worldZoom = 1.0f;
        gRenderSettings.xOffset = width * 0.5f - centre.x * CORE_CELL_WIDTH_PIXELS;
        gRenderSettings.yOffset = height * 0.5f - centre.y * CORE_LEVEL_HEIGHT_PIXELS;
    }
    renderWorld(world, &commands);
    return commands;
}

} // namespace pf::application::render
