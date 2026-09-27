// Vertical viewport culling, for ticket #58.
//
// The bounds query that feeds every render pass converted its upper Y bound
// with CORE_CELL_WIDTH_PIXELS instead of CORE_LEVEL_HEIGHT_PIXELS, and the
// renderer passed a hard-coded Y origin of 0 instead of the scrollbar's
// -yOffset. The over-wide initial window masked the missing origin for the
// first few rows; scroll far enough up a tall World and the Sectors that
// moved into view were culled - grid and blank space where Rooms should be.
//
// The checks pin down:
//
//   * a sub-level-height bounds query at the origin does not reach level 2 -
//     the ticket's probe, which the wrong divisor answered "2 sectors";
//   * viewportSectors() tracks a non-zero vertical offset: the Sectors above
//     the initial viewport come back, the scrolled-out ones below do not;
//   * the full scrollbar range works - the topmost level of a tall World
//     is visible when scrolled to the bottom of the scrollbar;
//   * the real render pass paints the scrolled-in Sector and paints nothing
//     of the culled Sector below the viewport.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "UISettings.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Sector.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The Solid-pass Layer fill renderSectors() passes down as ForeLocationColour.
	ImU32 const kForeLocationFill = ImU32(ImColor(192, 192, 255));

	// The scene: one Layer holding Rooms on the ground, on level 2 (just above
	// a three-level viewport's initial range), and on the topmost level of an
	// eight-level World.
	struct CullingScene
	{
		std::shared_ptr<core::World> world{ std::make_shared<core::World>(
			"Viewport culling", 12, 8) };
		uint32_t groundRoom{ 0 };
		uint32_t upperRoom{ 0 };
		uint32_t topRoom{ 0 };

		CullingScene()
		{
			while (world->getLayerCount() < 2) world->addLayer();

			groundRoom = world->addRoom("Ground", 0, 0, 0, 4, 1);
			upperRoom = world->addRoom("Upper", 0, 2, 0, 4, 1);
			topRoom = world->addRoom("Top", 0, 7, 0, 4, 1);

			world->finishBuild();
			world->pauseSimulation();
		}
	};

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	struct ImGuiGuard
	{
		ImGuiGuard() { ImGui::CreateContext(); }
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// A three-level-tall viewport at the origin, scrolled vertically by
	// scrollY pixels. The visible world band is [scrollY, scrollY + height].
	void setViewport(float scrollX, float scrollY, float width, float height)
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = width;
		gUISettings.worldViewportHeight = height;
		gUISettings.xOffset = -scrollX;
		gUISettings.yOffset = -scrollY;
	}

	// The screen-space rectangle a Sector's bounds land in under
	// transformPosition(), for the current gUISettings.
	struct ScreenRect
	{
		float minX, minY, maxX, maxY;
	};

	ScreenRect sectorScreenRect(core::World const& world, uint32_t sectorIndex)
	{
		core::Vector2 b0, b1;
		world.getSector(sectorIndex)->getBounds(b0, b1);

		auto const tx = [](float x)
		{
			return x * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset;
		};
		auto const ty = [](float y)
		{
			return gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- y * CORE_LEVEL_HEIGHT_PIXELS - gUISettings.yOffset;
		};

		return {
			std::min(tx(b0.x), tx(b1.x)), std::min(ty(b0.y), ty(b1.y)),
			std::max(tx(b0.x), tx(b1.x)), std::max(ty(b0.y), ty(b1.y))
		};
	}

	// Sample the painter-ordered command stream, respecting nested scissor
	// rectangles. These scenes use opaque untextured fills and sample away
	// from outlines and text.
	ImU32 colourAt(WorldDrawList const& draws, ImVec2 point)
	{
		ImU32 result = 0;
		for (auto const& command : draws.commands())
		{
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			auto const& clip = triangle->clip;
			if (point.x < clip.minimum.x || point.x >= clip.maximum.x
				|| point.y < clip.minimum.y || point.y >= clip.maximum.y) continue;
			auto cross = [point](ImVec2 a, ImVec2 b)
				{ return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x); };
			auto const a = cross(triangle->positions[0], triangle->positions[1]);
			auto const b = cross(triangle->positions[1], triangle->positions[2]);
			auto const c = cross(triangle->positions[2], triangle->positions[0]);
			if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
				result = triangle->colour;
		}
		return result;
	}

	// How many vertices of the given colour fall inside the rectangle.
	int verticesIn(ImDrawList const* drawList, ImU32 colour, ScreenRect const& rect)
	{
		int count = 0;
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
		{
			auto const& v = drawList->VtxBuffer[i];
			if (v.col != colour) continue;
			if (v.pos.x >= rect.minX - 0.01f && v.pos.x <= rect.maxX + 0.01f
				&& v.pos.y >= rect.minY - 0.01f && v.pos.y <= rect.maxY + 0.01f)
			{
				++count;
			}
		}
		return count;
	}
}

// The ticket's probe: a 159-pixel-high query - less than one 160-pixel
// level - at the origin must see level 0 only. With the upper bound divided by
// CORE_CELL_WIDTH_PIXELS it reached level 2 and returned the Upper Room too.
void subLevelHeightViewportExcludesLevelTwo()
{
	CullingScene scene;

	auto const sectors = scene.world->getSectorsInBounds(0, 0.0f, 0.0f,
		4 * CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS - 1.0f);

	require(sectors.size() == 1,
		std::format("a sub-level-height viewport at the origin returned {} sectors, expected 1",
			sectors.size()));
	require(sectors[0] == scene.world->getSector(scene.groundRoom),
		"the sub-level-height viewport did not return the level-0 Room it covers");
}

// With the scrollbar moved down (yOffset negative), the query must follow:
// the level-2 Room scrolled into view comes back, the level-0 Room scrolled
// out below the viewport does not.
void verticalOffsetTracksTheVisibleOrigin()
{
	CullingScene scene;

	// Three levels visible, scrolled so the band is [320, 800] world pixels:
	// level 2 is in, level 0 is out.
	setViewport(0.0f, 320.0f, 640.0f, 480.0f);

	auto const sectors = viewportSectors(scene.world, 0);

	bool upperIn = false, groundIn = false, topIn = false;
	for (auto const& sector : sectors)
	{
		if (sector == scene.world->getSector(scene.upperRoom)) upperIn = true;
		if (sector == scene.world->getSector(scene.groundRoom)) groundIn = true;
		if (sector == scene.world->getSector(scene.topRoom)) topIn = true;
	}

	require(upperIn, "the Room scrolled into view on level 2 was culled (#58 regression)");
	require(!groundIn, "the level-0 Room below the viewport was not culled");
	require(!topIn, "the level-7 Room far above the viewport was not culled");
}

// The full scrollbar range: at the bottom of an eight-level World's
// scroll, the topmost level must be visible. A World "substantially
// taller than the World panel" is exactly what the ticket reproduced with.
void fullyScrolledTopLevelIsVisible()
{
	CullingScene scene;

	// scrollMax = 8 * 160 - 480 = 800: the band is [800, 1280], levels 5-7.
	setViewport(0.0f, 8.0f * CORE_LEVEL_HEIGHT_PIXELS - 480.0f, 640.0f, 480.0f);

	auto const sectors = viewportSectors(scene.world, 0);

	bool topIn = false, groundIn = false;
	for (auto const& sector : sectors)
	{
		if (sector == scene.world->getSector(scene.topRoom)) topIn = true;
		if (sector == scene.world->getSector(scene.groundRoom)) groundIn = true;
	}

	require(topIn, "the topmost level was culled at the end of the scrollbar range");
	require(!groundIn, "the ground level was still submitted at the end of the scrollbar range");
}

// The real render pass, not just the query: with a non-zero vertical offset
// the Solid pass paints the scrolled-in Sector's fill quad on screen and
// paints nothing of the culled Sector below the viewport.
void renderPassPaintsScrolledInSectorOnly()
{
	ImGuiGuard imgui;
	ImDrawList* drawList = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());

	CullingScene scene;

	setViewport(0.0f, 320.0f, 640.0f, 480.0f);

	renderSectors(scene.world, 0, LayerRenderStyle::Solid, drawList);

	auto const upperRect = sectorScreenRect(*scene.world, scene.upperRoom);
	auto const groundRect = sectorScreenRect(*scene.world, scene.groundRoom);

	// The level-2 Room fills as one quad (4 vertices) inside its on-screen rect.
	require(verticesIn(drawList, kForeLocationFill, upperRect) == 4,
		"the Solid pass painted no fill quad for the Room scrolled into view (#58 regression)");

	// The level-0 Room sits below the viewport; none of its fill may be drawn.
	require(verticesIn(drawList, kForeLocationFill, groundRect) == 0,
		"the Solid pass painted the culled level-0 Room below the viewport");
}

void backgroundsShowThroughEmptyLayers()
{
	ImGuiGuard imgui;
	auto world = std::make_shared<core::World>("Background visibility", 6, 2);
	while (world->getLayerCount() < 4) world->addLayer();
	world->addBackground(3, 0, 0, 6, 2, { 23, 67, 109 });
	world->addRoom("Front occluder", 0, 0, 0, 1, 1);
	world->addRoom("Intermediate occluder", 2, 0, 1, 1, 1);
	world->addBackground(1, 0, 2, 1, 1, { 151, 37, 83 });
	world->finishBuild();
	for (bool overlay : { false, true })
	for (int selected = 0; selected < 4; ++selected)
	for (float zoom : { 1.0f, 2.0f })
	{
		setViewport(-17, 11, 6 * CORE_CELL_WIDTH_PIXELS * zoom,
			2 * CORE_LEVEL_HEIGHT_PIXELS * zoom);
		gUISettings.worldZoom = zoom;
		gUISettings.visibleLayer = selected;
		gUISettings.renderNextLayerWireframe = overlay;
		gUISettings.renderGrid = false;
		WorldDrawList draws(WorldDrawList::ClipRectangle{ { 0, 0 },
			{ gUISettings.worldViewportWidth, gUISettings.worldViewportHeight } });
		renderWorld(world, &draws);
		for (int y = 0; y < 2; ++y)
		for (int x = 0; x < 6; ++x)
		{
			core::Vector2 point{ x + 0.37f, y + 0.43f };
			point.x = point.x * CORE_CELL_WIDTH_PIXELS * zoom + gUISettings.xOffset;
			point.y = gUISettings.worldViewportHeight
				- point.y * CORE_LEVEL_HEIGHT_PIXELS * zoom - gUISettings.yOffset;
			auto expected = ImU32(ImColor(23, 67, 109));
			if (y == 0 && ((x == 0 && selected == 0) || (x == 1 && selected <= 2)))
				expected = kForeLocationFill;
			if (y == 0 && x == 2 && selected <= 1) expected = ImU32(ImColor(151, 37, 83));
			require(colourAt(draws, { point.x, point.y }) == expected,
				"Back-to-front composition has the wrong visible Sector at "
				+ std::to_string(x) + "," + std::to_string(y)
				+ " with selected Layer " + std::to_string(selected));
		}
		require(colourAt(draws, { -1, 10 }) == 0, "World rendering escaped its viewport scissor");
	}
}

void nestedWindowsKeepTheirScissors()
{
	ImGuiGuard imgui;
	auto world = std::make_shared<core::World>("Nested apertures", 6, 2);
	while (world->getLayerCount() < 3) world->addLayer();
	world->addRoom("Front", 0, 0, 0, 6, 1);
	world->addRoom("Middle", 1, 0, 0, 6, 1);
	world->addBackground(2, 0, 0, 6, 1, { 23, 67, 109 });
	auto front = world->addSectorWindow(0, 0, 1, 3, 1,
		{ false, core::Window::State::Closed, core::Window::Style::Clear });
	auto middle = world->addSectorWindow(1, 0, 2, 3, 1,
		{ false, core::Window::State::Closed, core::Window::Style::Clear });
	world->finishBuild();
	setViewport(0, 0, 6 * CORE_CELL_WIDTH_PIXELS, 2 * CORE_LEVEL_HEIGHT_PIXELS);
	gUISettings.visibleLayer = 0;
	gUISettings.renderGrid = false;
	gUISettings.renderNextLayerWireframe = false;
	WorldDrawList draws(WorldDrawList::ClipRectangle{ { 0, 0 },
		{ gUISettings.worldViewportWidth, gUISettings.worldViewportHeight } });
	renderWorld(world, &draws);
	core::Vector2 frontMin, frontMax, middleMin, middleMax;
	front.object->getFullShape(frontMin, frontMax);
	middle.object->getFullShape(middleMin, middleMax);
	auto sample = [&](float x)
	{
		auto const y = (frontMin.y + frontMax.y) * 0.5f;
		return colourAt(draws, { x * CORE_CELL_WIDTH_PIXELS,
			gUISettings.worldViewportHeight - y * CORE_LEVEL_HEIGHT_PIXELS });
	};
	require(sample((middleMin.x + frontMax.x) * 0.5f) == ImU32(ImColor(23, 67, 109)),
		"Aligned Windows did not reveal the third Layer");
	require(sample((frontMin.x + middleMin.x) * 0.5f) == ImU32(ImColor(224, 224, 255)),
		"Deeper Window escaped its own scissor into the middle Room");
	require(sample((frontMax.x + middleMax.x) * 0.5f) == kForeLocationFill,
		"Deeper Window escaped the front Window's scissor");
}

void layerCompositionOrderStopsAtSelection()
{
	for (uint32_t count : { 2u, 4u, 256u })
	for (uint32_t selected = 0; selected < count; ++selected)
	for (bool overlay : { false, true })
	{
		auto const passes = renderPasses(selected, count, overlay);
		auto nextSolid = count;
		uint32_t outlines = 0;
		for (auto const& pass : passes)
		{
			require(pass.layer >= selected && pass.layer < count,
				"A render pass escaped the selected-to-back Layer range");
			if (pass.style == LayerRenderStyle::Solid)
				require(pass.layer == --nextSolid, "Solid Layers are not ordered back-to-front");
			if (pass.style == LayerRenderStyle::Wireframe)
			{
				++outlines;
				require(pass.layer == selected + 1, "Overlay moved beyond the next Layer");
			}
		}
		require(nextSolid == selected, "Compositing stopped before the selected Layer");
		require(outlines == (overlay && selected + 1 < count ? 1u : 0u),
			"Overlay toggle changed the wrong passes");
	}
	require(renderPasses(4, 4, true).empty() && renderPasses(0, 0, true).empty(),
		"Invalid selection generated render passes");
}

void runViewportCullingSmokeChecks()
{
	auto const savedSettings = gUISettings;
	backgroundsShowThroughEmptyLayers();
	nestedWindowsKeepTheirScissors();
	layerCompositionOrderStopsAtSelection();
	subLevelHeightViewportExcludesLevelTwo();
	verticalOffsetTracksTheVisibleOrigin();
	fullyScrolledTopLevelIsVisible();
	renderPassPaintsScrolledInSectorOnly();
	gUISettings = savedSettings;
}
