#include "Checks.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
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
// The floor lift belongs to the same query (#1108). The lift is a term of the
// mapping the cull inverts, so a Layer lifted up into the bottom of the
// viewport was culled: the shifted Rooms were plainly on screen and nothing
// was drawn for them. The checks added for that pin down:
//
//   * a Layer lifted into the bottom of the editor viewport is fetched and
//     painted, where flat it was out of the band and off the canvas;
//   * the same Layer under the shipped game's follow camera at zoom 1, which
//     reaches the same blank band by the opposite arithmetic - it carries the
//     lift into yOffset so the picture stands still while the band moves;
//   * a negative lift, a floor pushed down, is not missed at the top edge:
//     the band is widened to the union of the viewport's band and the lifted
//     one, not shifted from one edge to the other;
//   * the widening keeps what it widened over - nothing the flat band fetched
//     is lost under a lift, in either direction of the lift, which is the one
//     thing a band that shifted instead of widening cannot do;
//   * a flat World culls the identical Sector set the pre-lift expression
//     gave, across zooms and scroll positions;
//   * the aperture pass culls the front Layer's Sectors by the front Layer's
//     lift, where the back Layer's number would drop a visible hole.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldToScreen.h"
#include "WorldViewportZoom.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The Solid-pass Layer fill renderSectors() passes down as ForeLocationColour.
	PackedColourValue const kForeLocationFill = PackedColourValue(ImColor(192, 192, 255));

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
	using ImGuiGuard = headless::ScopedImGuiContext;

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

		auto const mapping = pf::render::worldToScreen(gUISettings);

		return {
			std::min(mapping.toScreenX(b0.x), mapping.toScreenX(b1.x)),
			std::min(mapping.toScreenY(b0.y), mapping.toScreenY(b1.y)),
			std::max(mapping.toScreenX(b0.x), mapping.toScreenX(b1.x)),
			std::max(mapping.toScreenY(b0.y), mapping.toScreenY(b1.y))
		};
	}

	// Sample the painter-ordered command stream, respecting nested scissor
	// rectangles. These scenes use opaque untextured fills and sample away
	// from outlines and text.
	PackedColourValue colourAt(WorldDrawList const& draws, Vec2 point)
	{
		PackedColourValue result = 0;
		for (auto const& command : draws.commands())
		{
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			auto const& clip = triangle->clip;
			if (point.x < clip.minimum.x || point.x >= clip.maximum.x
				|| point.y < clip.minimum.y || point.y >= clip.maximum.y) continue;
			auto cross = [point](Vec2 a, Vec2 b)
				{ return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x); };
			auto const a = cross(triangle->positions[0], triangle->positions[1]);
			auto const b = cross(triangle->positions[1], triangle->positions[2]);
			auto const c = cross(triangle->positions[2], triangle->positions[0]);
			if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
				result = triangle->colour;
		}
		return result;
	}

	// The topmost triangle painted over a point, respecting nested scissor
	// rectangles. Nothing these checks read is a flat colour any more, so
	// the answer is the triangle itself.
	WorldDrawList::Triangle const* topAt(WorldDrawList const& draws, Vec2 point)
	{
		WorldDrawList::Triangle const* result = nullptr;
		for (auto const& command : draws.commands())
		{
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			auto const& clip = triangle->clip;
			if (point.x < clip.minimum.x || point.x >= clip.maximum.x
				|| point.y < clip.minimum.y || point.y >= clip.maximum.y) continue;
			auto cross = [point](Vec2 a, Vec2 b)
				{ return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x); };
			auto const a = cross(triangle->positions[0], triangle->positions[1]);
			auto const b = cross(triangle->positions[1], triangle->positions[2]);
			auto const c = cross(triangle->positions[2], triangle->positions[0]);
			if ((a >= 0 && b >= 0 && c >= 0) || (a <= 0 && b <= 0 && c <= 0))
				result = triangle;
		}
		return result;
	}

	// A sheet with one full-cell tile per surface these nested-aperture
	// scenes use, each in its own column, so a quad's tile is readable
	// back out of the UVs it sampled. Which tile the aperture drew says
	// which Layer it looked through to.
	SectorTileset installNestedSurfaceAtlas()
	{
		SectorTileset atlas;
		atlas.family = SectorTilesetFamily::New;
		int column = 0;
		for (auto const* name : { "room", "background", "facade" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * CORE_CELL_WIDTH_PIXELS, 0,
				CORE_CELL_WIDTH_PIXELS, CORE_LEVEL_HEIGHT_PIXELS });
		atlas.width = column * CORE_CELL_WIDTH_PIXELS;
		atlas.height = CORE_LEVEL_HEIGHT_PIXELS;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic nested-aperture atlas is not the New family");
		return atlas;
	}

	std::string tileOf(WorldDrawList::Triangle const* triangle,
		SectorTileset const& atlas)
	{
		if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
			return {};
		float u0{ 1e9f }, u1{ -1e9f }, v0{ 1e9f }, v1{ -1e9f };
		for (auto const& uv : triangle->texcoords)
		{
			u0 = std::min(u0, uv.x); u1 = std::max(u1, uv.x);
			v0 = std::min(v0, uv.y); v1 = std::max(v1, uv.y);
		}
		auto const cu = (u0 + u1) * 0.5f;
		auto const cv = (v0 + v1) * 0.5f;
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height) / float(atlas.height);
			if (cu >= left && cu < right && cv >= top && cv < bottom)
				return name;
		}
		return {};
	}

	// How many vertices of the given colour fall inside the rectangle.
	int verticesIn(ImDrawList const* drawList, PackedColourValue colour, ScreenRect const& rect)
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

	// ---------------------------------------------------------------------
	// The lift-aware band (#1108).
	//
	// Everything below drives the real cull with a Layer that draws lifted by
	// a floor depth (#1101) and asks whether the viewport still finds the
	// Sectors it can see. The screen box each check reads is the mapping's own
	// answer for that Layer - `worldToScreenForLayer`, the same query the
	// editor's cursor inverts (#1105) - so the box being looked for is the box
	// the renderer paints rather than a second piece of arithmetic that could
	// agree with a broken cull by being wrong in the same way.
	// ---------------------------------------------------------------------

	// The screen box a Sector lands in when its Layer draws at its lift.
	ScreenRect liftedScreenRect(core::World const& world, uint32_t sectorIndex,
		uint32_t layer)
	{
		core::Vector2 b0, b1;
		world.getSector(sectorIndex)->getBounds(b0, b1);
		auto const mapping = pf::render::worldToScreenForLayer(gUISettings, world,
			layer, gUISettings.worldZoom);
		return {
			std::min(mapping.toScreenX(b0.x), mapping.toScreenX(b1.x)),
			std::min(mapping.toScreenY(b0.y), mapping.toScreenY(b1.y)),
			std::max(mapping.toScreenX(b0.x), mapping.toScreenX(b1.x)),
			std::max(mapping.toScreenY(b0.y), mapping.toScreenY(b1.y))
		};
	}

	std::string describe(ScreenRect const& rect)
	{
		return "x " + std::to_string(rect.minX) + ".." + std::to_string(rect.maxX)
			+ ", y " + std::to_string(rect.minY) + ".." + std::to_string(rect.maxY);
	}

	// Does any part of the box fall inside the viewport vertically? A culling
	// check whose subject is never on screen passes on nothing, so every check
	// below asks this before it asks whether the Sector was drawn.
	bool onScreen(ScreenRect const& rect)
	{
		return rect.maxY > gUISettings.worldViewportY
			&& rect.minY < gUISettings.worldViewportY + gUISettings.worldViewportHeight;
	}

	bool sees(std::vector<std::shared_ptr<const core::Sector>> const& sectors,
		core::World const& world, uint32_t sectorIndex)
	{
		auto const wanted = world.getSector(sectorIndex);
		return std::find(sectors.begin(), sectors.end(), wanted) != sectors.end();
	}

	// The Solid pass, recorded into a fresh command vector and counted by how
	// many distinct corners of the Layer's own fill landed inside a box.
	//
	// Recorded rather than replayed into a live draw list because these checks
	// each have a flat phase and a lifted phase: two phases sharing one draw
	// list would let the second read the first one's vertices and pass on the
	// picture the earlier phase left behind.
	int fillCornersIn(std::shared_ptr<const core::World> const& world,
		uint32_t layer, ScreenRect const& rect)
	{
		WorldDrawList draws(WorldDrawList::ClipRectangle{ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderSectors(world, layer, LayerRenderStyle::Solid, &draws);

		std::vector<Vec2> corners;
		for (auto const& command : draws.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->colour != kForeLocationFill) continue;
			for (auto const& p : triangle->positions)
			{
				if (p.x < rect.minX - 0.01f || p.x > rect.maxX + 0.01f
					|| p.y < rect.minY - 0.01f || p.y > rect.maxY + 0.01f)
					continue;
				auto const already = std::find_if(corners.begin(), corners.end(),
						[&](Vec2 const& q)
						{
							return std::abs(q.x - p.x) < 0.01f
								&& std::abs(q.y - p.y) < 0.01f;
						});
				if (already == corners.end()) corners.push_back(p);
			}
		}
		return static_cast<int>(corners.size());
	}

	// Two Layers: the front one carries a floor depth, the back one draws
	// lifted by it. The back Room is the thing the viewport has to follow.
	struct LiftScene
	{
		std::shared_ptr<core::World> world{ std::make_shared<core::World>(
			"Viewport culling under a lift", 12, 12) };
		uint32_t lifter{ 0 };   // Layer 0, the Sector that carries the depth
		uint32_t shifted{ 0 };  // Layer 1, the Sector that moves
		uint32_t far{ 0 };      // Layer 1, a Sector the widened band must not reach

		explicit LiftScene(uint32_t shiftedLevel)
		{
			while (world->getLayerCount() < 2) world->addLayer();
			lifter = world->addRoom("Lifter", 0, 0, 0, 4, 1);
			shifted = world->addRoom("Shifted", 1, shiftedLevel, 0, 4, 1);
			far = world->addRoom("Far", 1, 5, 0, 4, 1);
			world->finishBuild();
			world->pauseSimulation();
		}

		void setDepth(int32_t depth)
		{
			require(world->setSectorFloorDepth(lifter, depth).changed,
				"the fixture's floor depth write was refused");
			require(world->layerFloorOffsetPx(1) == depth,
				"the staircase did not put the Layer behind at the authored depth");
		}
	};

	// The shipped game's follow camera, restated from
	// `pf::application::render::applyCamera` (#1106): the canvas is placed so
	// the followed point of the *lifted* picture sits at the canvas centre,
	// which means the Layer's lift is carried into `yOffset` on the way in.
	// The renderer adds the same term on the way out, so the picture does not
	// move - but the cull band, which is derived from `yOffset`, moves by the
	// whole lift with it. That is the second way into #1108, and it is why the
	// editor case is not enough on its own: the two reach the same blank band
	// by opposite arithmetic.
	void applyGameCamera(core::World const& world, float width, float height,
		uint32_t viewLayer, core::Vector2 followed)
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = width;
		gUISettings.worldViewportHeight = height;
		gUISettings.worldZoom = 1.0f;
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		gUISettings.renderGrid = false;
		gUISettings.xOffset = width * 0.5f - followed.x * CORE_CELL_WIDTH_PIXELS;
		gUISettings.yOffset = height * 0.5f - followed.y * CORE_LEVEL_HEIGHT_PIXELS
			- pf::render::layerFloorLiftPx(world, viewLayer,
				gUISettings.worldZoom);
	}
}

// The ticket's probe: a query one pixel short of a whole Level, at the
// origin, must see Level 0 only. With the upper bound divided by
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

	// Three Levels visible, scrolled so the band is [Level 1, Level 4]: the
	// level-2 Room is in, the level-0 Room is out.
	setViewport(0.0f, 1.0f * CORE_LEVEL_HEIGHT_PIXELS,
		4.0f * CORE_CELL_WIDTH_PIXELS, 3.0f * CORE_LEVEL_HEIGHT_PIXELS);

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

	// scrollMax = 8 Levels - a 3-Level viewport = 5 Levels: the band is
	// [Level 5, Level 8], so the topmost Level 7 sits at the end of the scroll.
	setViewport(0.0f, 5.0f * CORE_LEVEL_HEIGHT_PIXELS,
		4.0f * CORE_CELL_WIDTH_PIXELS, 3.0f * CORE_LEVEL_HEIGHT_PIXELS);

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

	setViewport(0.0f, 1.0f * CORE_LEVEL_HEIGHT_PIXELS,
		4.0f * CORE_CELL_WIDTH_PIXELS, 3.0f * CORE_LEVEL_HEIGHT_PIXELS);

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

void emptyCellsDoNotRevealDeeperLayers()
{
	ImGuiGuard imgui;
	auto world = std::make_shared<core::World>("Selected Layer only", 6, 2);
	while (world->getLayerCount() < 4) world->addLayer();
	world->addRoom("Selected", 0, 0, 0, 1, 1);
	world->addBackground(3, 0, 0, 6, 2, { 23, 67, 109 });
	world->finishBuild();

	for (bool overlay : { false, true })
	for (float zoom : { 1.0f, 2.0f })
	{
		setViewport(-17, 11, 6 * CORE_CELL_WIDTH_PIXELS * zoom,
			2 * CORE_LEVEL_HEIGHT_PIXELS * zoom);
		gUISettings.worldZoom = zoom;
		gUISettings.visibleLayer = 0;
		gUISettings.renderNextLayerWireframe = overlay;
		gUISettings.renderGrid = false;
		WorldDrawList draws(WorldDrawList::ClipRectangle{ { 0, 0 },
			{ gUISettings.worldViewportWidth, gUISettings.worldViewportHeight } });
		renderWorld(world, &draws);

		auto sample = [&](float x, float y)
		{
			return colourAt(draws, pf::render::worldToScreen(gUISettings)
				.toScreen({x, y}));
		};
		require(sample(0.37f, 0.43f) == kForeLocationFill,
			"The selected Layer's Room was not rendered");
		require(sample(2.37f, 0.43f) == 0,
			"An empty selected-Layer cell revealed a deeper Layer");
		require(colourAt(draws, { -1, 10 }) == 0,
			"World rendering escaped its viewport scissor");
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
	auto const atlas = installNestedSurfaceAtlas();
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
	// Every sample point below is a World point put on screen by the render
	// module's shared mapping, not by arithmetic restated here (#1100).
	auto const mapping = pf::render::worldToScreen(gUISettings);
	auto const screenOf = [&mapping](float worldX, float worldY)
	{
		return mapping.toScreen({worldX, worldY});
	};
	auto const midY = (frontMin.y + frontMax.y) * 0.5f;
	auto sample = [&](float x)
	{
		return colourAt(draws, screenOf(x, midY));
	};
	// Through both aligned Windows the eye lands on the third Layer's own
	// tile; through the front Window alone it lands on the middle Room's;
	// past the front Window's edge it never leaves the front Room.
	require(tileOf(topAt(draws, screenOf((middleMin.x + frontMax.x) * 0.5f, midY)), atlas)
			== "background",
		"Aligned Windows did not reveal the third Layer");

	auto const throughFront = topAt(draws,
		screenOf((frontMin.x + middleMin.x) * 0.5f, midY));
	require(tileOf(throughFront, atlas) == "room",
		"Deeper Window escaped its own scissor into the middle Room");
	auto const pastFront = topAt(draws, screenOf((frontMax.x + middleMax.x) * 0.5f, midY));
	require(tileOf(pastFront, atlas) == "room",
		"Deeper Window escaped the front Window's scissor");
	// Both are Rooms, so the tile cannot tell them apart - the scissor can.
	// What the front Window reveals is clipped to the Window; the front
	// Room's own surface is not.
	require(throughFront->clip.maximum.x - throughFront->clip.minimum.x
			< pastFront->clip.maximum.x - pastFront->clip.minimum.x,
		"the front Window drew no scissor narrower than the front Room's own surface");
}

void nestedOpenDoorsRevealTheBackLayer()
{
	ImGuiGuard imgui;
	auto world = std::make_shared<core::World>("Nested Door apertures", 6, 2);
	while (world->getLayerCount() < 3) world->addLayer();
	world->addRoom("Front", 0, 0, 0, 6, 1);
	world->addRoom("Middle", 1, 0, 0, 6, 1);
	world->addFacade("Back", 2, 0, 0, 6, 1, CORE_ROOM_MAX_HEIGHT, { 23, 67, 109 });
	auto const front = world->addSectorDoor(0, 0, 2);
	auto const middle = world->addSectorDoor(1, 0, 2);
	world->finishBuild();
	auto const atlas = installNestedSurfaceAtlas();
	world->pauseSimulation();

	for (auto const& created : { front, middle })
	{
		auto const object = created.door.sector->getObject(created.door.index);
		auto const door = std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
		door->open();
		while (!door->isOpen()) door->update(door->getOpenCloseTime() * 0.25f);
	}

	setViewport(0, 0, 6 * CORE_CELL_WIDTH_PIXELS, 2 * CORE_LEVEL_HEIGHT_PIXELS);
	gUISettings.visibleLayer = 0;
	gUISettings.renderGrid = false;
	gUISettings.renderNextLayerWireframe = false;
	WorldDrawList draws(WorldDrawList::ClipRectangle{ { 0, 0 },
		{ gUISettings.worldViewportWidth, gUISettings.worldViewportHeight } });
	renderWorld(world, &draws);

	require(tileOf(topAt(draws,
		pf::render::worldToScreen(gUISettings).toScreen({2.5f, 0.5f})), atlas)
		== "facade",
		"Aligned open Doors did not reveal the back Layer");
}

void renderPassesDrawOnlyTheSelectionWhole()
{
	for (uint32_t count : { 2u, 4u, 256u })
	for (uint32_t selected = 0; selected < count; ++selected)
	for (bool overlay : { false, true })
	{
		auto const passes = renderPasses(selected, count, overlay);
		uint32_t solids = 0;
		uint32_t outlines = 0;
		for (auto const& pass : passes)
		{
			if (pass.style == LayerRenderStyle::Solid)
			{
				++solids;
				require(pass.layer == selected,
					"A Layer other than the selection received a whole-Layer pass");
			}
			if (pass.style == LayerRenderStyle::Wireframe)
			{
				++outlines;
				require(pass.layer == selected + 1, "Overlay moved beyond the next Layer");
			}
		}
		require(solids == 1, "The selection did not receive exactly one solid pass");
		require(outlines == (overlay && selected + 1 < count ? 1u : 0u),
			"Overlay toggle changed the wrong passes");
	}
	require(renderPasses(4, 4, true).empty() && renderPasses(0, 0, true).empty(),
		"Invalid selection generated render passes");
}


// A Layer lifted into view at the bottom of the viewport draws (#1108).
//
// The viewport sees Levels 1..4; the shifted Room sits on Level 0. Flat, it
// is below the band and off the bottom of the canvas. Give the front Room a
// one-Level depth and the Room rises into the bottom of the viewport - but the
// cull band, computed from the viewport alone, stayed where it was, so the
// picture showed the shifted Room's ground and drew nothing on it.
void liftedSectorAtTheViewportBottomIsDrawn()
{
	LiftScene scene{ 0 };
	setViewport(0.0f, 500.0f, 4.0f * CORE_CELL_WIDTH_PIXELS,
		2.5f * CORE_LEVEL_HEIGHT_PIXELS);

	// The negative control first: flat, the Room really is out of the band
	// and really is off the bottom of the canvas. Without this the check
	// below would pass on a Room that was visible anyway.
	{
		RenderWorldScope scope{ scene.world };
		require(!sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"the Level-0 Room is already inside the flat viewport band, so this "
			"check has nothing to lift into view");
		require(!onScreen(sectorScreenRect(*scene.world, scene.shifted)),
			"the flat Room is on screen, so the cull is not what is under test");
		require(fillCornersIn(scene.world, 1,
			sectorScreenRect(*scene.world, scene.shifted)) == 0,
			"the Solid pass drew the Room that sits below the viewport");
	}

	scene.setDepth(CORE_LEVEL_HEIGHT_PIXELS);
	{
		RenderWorldScope scope{ scene.world };
		auto const rect = liftedScreenRect(*scene.world, scene.shifted, 1);
		require(onScreen(rect),
			"the lifted Room never reached the viewport, so nothing here tests the "
			"cull following it: " + describe(rect));
		require(rect.minY > gUISettings.worldViewportY
				+ gUISettings.worldViewportHeight * 0.5f,
			"the lifted Room is not at the bottom of the viewport the ticket "
			"describes: " + describe(rect));
		require(sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"the lift carried the Room into the viewport at " + describe(rect)
			+ " but the cull band did not follow it: the band is computed from the "
			"viewport alone and misses the whole lift (#1108)");
		require(fillCornersIn(scene.world, 1, rect) == 4,
			"the Solid pass drew no fill quad for the Room lifted into the bottom "
			"of the viewport at " + describe(rect) + " (#1108)");
		// Widened by the lift and not by more than it. The band asked for is
		// the union of the viewport's own band and the lifted one, which
		// reaches Level 4 here and no further; a Room one Level past that is
		// off screen and must stay unfetched. A fix that widened by a generous
		// constant instead of by the lift would pass every assertion above and
		// draw the whole World every frame.
		require(!sees(viewportSectors(scene.world, 1), *scene.world, scene.far),
			"the widened band reached past the lifted band it was widened by: the "
			"Room at Level 5 is off screen at "
			+ describe(liftedScreenRect(*scene.world, scene.far, 1))
			+ " and was fetched anyway");
	}
}

// The same Layer, seen under the shipped game's follow camera at zoom 1.
//
// The camera carries the lift into `yOffset` so the followed point stays
// centred, and the renderer adds the same lift on the way out: the picture
// lands on the exact screen box it had flat. What moved is the cull band,
// which follows `yOffset` and not the picture - so the Room that drew flat
// stops drawing once the Layer is lifted, and the bottom of the game window
// goes blank around the Player.
void theGameFollowCameraKeepsLiftedSectorsInView()
{
	LiftScene scene{ 3 };
	core::Vector2 const followed{ 2.0f, 5.0f };
	float const canvasW = 1280.0f;
	float const canvasH = 800.0f;

	// Flat baseline: the camera centres the followed point with no lift to
	// carry, and the Level-3 Room is in the band and drawn.
	applyGameCamera(*scene.world, canvasW, canvasH, 1u, followed);
	ScreenRect flatRect{};
	{
		RenderWorldScope scope{ scene.world };
		flatRect = liftedScreenRect(*scene.world, scene.shifted, 1);
		require(onScreen(flatRect),
			"the flat Room is not on screen, so the baseline this check moves off "
			"is not the Room's box: " + describe(flatRect));
		require(sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"the flat follow camera culled a Room it should see: " + describe(flatRect));
		require(fillCornersIn(scene.world, 1, flatRect) == 4,
			"the flat follow camera drew no fill quad for the Room at "
			+ describe(flatRect));
	}

	scene.setDepth(CORE_LEVEL_HEIGHT_PIXELS);
	applyGameCamera(*scene.world, canvasW, canvasH, 1u, followed);
	{
		RenderWorldScope scope{ scene.world };
		auto const lifted = liftedScreenRect(*scene.world, scene.shifted, 1);
		// The camera gave back exactly what the renderer took, so the picture
		// is where it always was. If this ever stops holding, the two halves of
		// the compensation drifted and the comparison below is meaningless.
		require(std::abs(lifted.minY - flatRect.minY) < 0.01f
				&& std::abs(lifted.maxY - flatRect.maxY) < 0.01f,
			"the follow camera no longer keeps the picture still under a lift: "
			+ describe(flatRect) + " became " + describe(lifted));
		require(onScreen(lifted),
			"the lifted Room is off screen, so nothing here tests the cull: "
			+ describe(lifted));
		require(sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"the follow camera moved the cull band by the whole lift and left the "
			"Room it still draws at " + describe(lifted) + " outside it (#1108)");
		require(fillCornersIn(scene.world, 1, lifted) == 4,
			"the shipped game's follow camera drew nothing for the Room at "
			+ describe(lifted) + " where it drew the same quad flat (#1108)");
	}
}

// A negative lift - a floor pushed down - does not miss at the top edge.
//
// A lift moves the visible World band down the World; a negative lift moves it
// up. Widening the band rather than shifting it is what keeps both edges: the
// band asked for is the union of the viewport's own band and the band the lift
// moves it to, so the slack that was already there at the far edge - a Room
// draws above its top cell by its `topLevelHeight` - is not traded for a new
// miss on the way back.
void aPushedDownLayerIsNotMissedAtTheTopEdge()
{
	LiftScene scene{ 7 };
	setViewport(0.0f, 1300.0f, 4.0f * CORE_CELL_WIDTH_PIXELS,
		2.5f * CORE_LEVEL_HEIGHT_PIXELS);

	{
		RenderWorldScope scope{ scene.world };
		require(!sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"the Level-7 Room is already inside the flat viewport band, so this "
			"check has nothing to push into view");
		require(!onScreen(sectorScreenRect(*scene.world, scene.shifted)),
			"the flat Room is on screen, so the cull is not what is under test");
		require(fillCornersIn(scene.world, 1,
			sectorScreenRect(*scene.world, scene.shifted)) == 0,
			"the Solid pass drew the Room that sits above the viewport");
	}

	scene.setDepth(-CORE_LEVEL_HEIGHT_PIXELS);
	{
		RenderWorldScope scope{ scene.world };
		auto const rect = liftedScreenRect(*scene.world, scene.shifted, 1);
		require(onScreen(rect),
			"the pushed-down Room never reached the viewport, so nothing here tests "
			"the top edge: " + describe(rect));
		require(rect.maxY < gUISettings.worldViewportY
				+ gUISettings.worldViewportHeight * 0.5f,
			"the pushed-down Room is not at the top of the viewport: "
			+ describe(rect));
		require(sees(viewportSectors(scene.world, 1), *scene.world, scene.shifted),
			"a negative lift pushed the Room down into the top of the viewport at "
			+ describe(rect) + " and the cull band missed it (#1108)");
		require(fillCornersIn(scene.world, 1, rect) == 4,
			"the Solid pass drew no fill quad for the Room pushed into the top of "
			"the viewport at " + describe(rect) + " (#1108)");
	}
}

// Widened, not shifted (#1108).
//
// The ticket asks for the union of the viewport's band and the lifted band
// rather than a shift, so the slack already sitting at the far edge - a Room
// draws above its top cell by its `topLevelHeight` - is not traded for a new
// miss on the way back. That property is not the same thing as the two edge
// checks above: a band that shifted instead of widening still lifts the near
// edge and still catches a pushed-down Room at the top, so it passes both of
// those and quietly drops rows off the other end. This says the one thing a
// shift cannot do - nothing the flat band fetched is lost.
void theLiftedBandKeepsEverySectorTheFlatBandFetched()
{
	// Sectors on every Level of the lifted Layer, so the flat band always
	// holds something to lose. A scene with two Rooms in it would answer this
	// vacuously at most of the scrolls below.
	auto world = std::make_shared<core::World>("Widened not shifted", 12, 8);
	while (world->getLayerCount() < 2) world->addLayer();
	auto const lifter = world->addRoom("Lifter", 0, 0, 0, 1, 1);
	for (uint32_t level = 0; level < 8; ++level)
		world->addRoom("Row " + std::to_string(level), 1, level, 0, 1, 1);
	world->finishBuild();
	world->pauseSimulation();
	RenderWorldScope scope{ world };

	// The band as it was before the lift term entered the query.
	auto const flatBand = [&](uint32_t layer)
	{
		auto const invZ = 1.0f / gUISettings.worldZoom;
		return world->getSectorsInBounds(layer,
			-gUISettings.xOffset * invZ, -gUISettings.yOffset * invZ,
			gUISettings.worldViewportWidth * invZ,
			gUISettings.worldViewportHeight * invZ);
	};

	for (int32_t const depth : { CORE_LEVEL_HEIGHT_PIXELS, -CORE_LEVEL_HEIGHT_PIXELS })
	{
		require(world->setSectorFloorDepth(lifter, depth).changed,
			"the fixture's floor depth write was refused");
		for (float const zoom : { 0.5f, 1.0f, 2.0f })
		for (float const scrollY : { 0.0f, 300.0f, 600.0f, 900.0f })
		{
			setViewport(0.0f, scrollY, 4.0f * CORE_CELL_WIDTH_PIXELS,
				2.5f * CORE_LEVEL_HEIGHT_PIXELS);
			gUISettings.worldZoom = zoom;

			auto const before = flatBand(1);
			auto const after = viewportSectors(world, 1);
			require(!before.empty(),
				"the flat band fetched nothing at scroll " + std::to_string(scrollY)
					+ " and zoom " + std::to_string(zoom)
					+ ", so keeping it would prove nothing");
			for (auto const& sector : before)
				require(std::find(after.begin(), after.end(), sector) != after.end(),
					"a lift of " + std::to_string(depth) + " at zoom "
						+ std::to_string(zoom) + " and scroll " + std::to_string(scrollY)
						+ " dropped a Sector the flat band fetched: the band was shifted off"
						" its far edge instead of widened to the union of the two, trading"
						" the existing slack for a new miss (#1108)");
		}
	}
}

// A flat World culls the identical band it culled before the lift term
// entered the query (#1108, acceptance: "a flat World draws the identical
// command stream it draws today").
//
// The pre-fix expression is restated here as the thing to hold the query to,
// across zooms and scroll positions, with the lift path live: with no depth
// anywhere the widened band must be the same four numbers, added rather than
// reordered, so no coordinate in any pass moves by a unit in the last place.
void aFlatWorldCullsTheBandItAlwaysCulled()
{
	LiftScene scene{ 2 };
	require(scene.world->layerFloorOffsetPx(1) == 0,
		"the fixture World is not flat, so this check has no baseline");
	RenderWorldScope scope{ scene.world };

	for (float const zoom : { 0.5f, 1.0f, 2.0f })
	for (float const scrollY : { -640.0f, 0.0f, 500.0f, 1777.0f })
	for (float const scrollX : { 0.0f, -333.0f })
	for (uint32_t const layer : { 0u, 1u })
	{
		setViewport(scrollX, scrollY, 4.0f * CORE_CELL_WIDTH_PIXELS,
			2.5f * CORE_LEVEL_HEIGHT_PIXELS);
		gUISettings.worldZoom = zoom;
		auto const inverseZoom = 1.0f / gUISettings.worldZoom;
		auto const expected = scene.world->getSectorsInBounds(layer,
			-gUISettings.xOffset * inverseZoom, -gUISettings.yOffset * inverseZoom,
			gUISettings.worldViewportWidth * inverseZoom,
			gUISettings.worldViewportHeight * inverseZoom);
		auto const actual = viewportSectors(scene.world, layer);
		require(actual.size() == expected.size(),
			"a flat World culled " + std::to_string(actual.size())
				+ " Sectors on Layer " + std::to_string(layer) + " at zoom "
				+ std::to_string(zoom) + " and scroll " + std::to_string(scrollY)
				+ " where the band before the lift term gave "
				+ std::to_string(expected.size())
				+ "; a zero lift must widen by nothing, not by a row");
		for (size_t i = 0; i < expected.size(); ++i)
			require(actual[i] == expected[i],
				"a flat World culled a different Sector set than the band before "
				"the lift term on Layer " + std::to_string(layer));
	}
}

// The aperture pass culls by the front Layer's lift, not the back Layer's.
//
// The aperture pass asks for the front Layer's Sectors because they are the
// holes, and the holes are measured in the front Layer's frame (#1102). Here
// the front Layer draws lifted 320 and the Layer behind it draws at zero -
// the front Layer's own negative depth cancels what it inherited - so the two
// frames disagree about the row the front Room sits on: on screen in the
// front frame, below the canvas in the back frame. A cull that fetched the
// front Layer's Sectors by the back Layer's number would drop a hole that is
// plainly visible and leave the Layer behind painted solid over it.
void theAperturePassCullsByTheFrontLayersLift()
{
	auto world = std::make_shared<core::World>("Aperture cull", 12, 12);
	while (world->getLayerCount() < 3) world->addLayer();
	auto const deep = world->addRoom("Deep", 0, 0, 0, 4, 1);
	auto const front = world->addRoom("Front", 1, 3, 0, 4, 1);
	auto const shallow = world->addRoom("Shallow", 1, 5, 0, 4, 1);
	auto const back = world->addRoom("Back", 2, 3, 0, 4, 1);
	world->finishBuild();
	world->pauseSimulation();
	require(world->setSectorFloorDepth(deep, CORE_LEVEL_HEIGHT_PIXELS).changed,
		"the Layer-0 depth write was refused");
	// Every Sector of the front Layer carries the negative depth: the Layer's
	// step down is the deepest floor on it, so one shallow Room would hold the
	// Layer behind at the number this check needs to cancel.
	require(world->setSectorFloorDepth(front, -CORE_LEVEL_HEIGHT_PIXELS).changed,
		"the front Layer's negative depth write was refused");
	require(world->setSectorFloorDepth(shallow, -CORE_LEVEL_HEIGHT_PIXELS).changed,
		"the front Layer's second negative depth write was refused");
	require(world->layerFloorOffsetPx(1) == CORE_LEVEL_HEIGHT_PIXELS,
		"the front Layer does not draw lifted one Level");
	require(world->layerFloorOffsetPx(2) == 0,
		"the back Layer does not draw flat, so the two frames do not disagree");

	setViewport(0.0f, 1500.0f, 4.0f * CORE_CELL_WIDTH_PIXELS,
		2.5f * CORE_LEVEL_HEIGHT_PIXELS);
	RenderWorldScope scope{ world };

	// The disagreement, measured by the mapping rather than by the cull, so
	// it holds whatever the cull does. The second line is the hypothetical:
	// the same document row drawn at the back Layer's lift instead of the
	// front Layer's.
	require(onScreen(liftedScreenRect(*world, front, 1)),
		"the front Room is not on screen in its own frame, so this check has no "
		"hole to lose: " + describe(liftedScreenRect(*world, front, 1)));
	require(!onScreen(liftedScreenRect(*world, front, 2)),
		"the two frames agree about the front Room's row, so the assertion below "
		"cannot tell one lift from the other: "
		+ describe(liftedScreenRect(*world, front, 2)));

	// The front Layer's cull is its own band, and so fetches the hole.
	require(sees(viewportSectors(world, 1), *world, front),
		"the front Layer's visible Sector was culled out of its own band at "
		+ describe(liftedScreenRect(*world, front, 1)) + "; the back Layer draws "
		"at " + std::to_string(world->layerFloorOffsetPx(2)) + " and culling the "
		"front Layer by that number drops this Sector (#1108)");

	// And the back Layer really does not reach the row, which is what makes
	// the assertion above a statement about the lift rather than the row.
	require(!sees(viewportSectors(world, 2), *world, back),
		"the back Layer's band reaches the row this check is built on, so the "
		"front-Layer assertion above proves nothing about lifts");
}

void render_smoke::registerViewportCulling(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "emptyCellsDoNotRevealDeeperLayers", isolated<[](smoke::Context const&) { emptyCellsDoNotRevealDeeperLayers(); }> });
	checks.push_back({ "nestedWindowsKeepTheirScissors", isolated<[](smoke::Context const&) { nestedWindowsKeepTheirScissors(); }> });
	checks.push_back({ "nestedOpenDoorsRevealTheBackLayer", isolated<[](smoke::Context const&) { nestedOpenDoorsRevealTheBackLayer(); }> });
	checks.push_back({ "renderPassesDrawOnlyTheSelectionWhole", isolated<[](smoke::Context const&) { renderPassesDrawOnlyTheSelectionWhole(); }> });
	checks.push_back({ "subLevelHeightViewportExcludesLevelTwo", isolated<[](smoke::Context const&) { subLevelHeightViewportExcludesLevelTwo(); }> });
	checks.push_back({ "verticalOffsetTracksTheVisibleOrigin", isolated<[](smoke::Context const&) { verticalOffsetTracksTheVisibleOrigin(); }> });
	checks.push_back({ "fullyScrolledTopLevelIsVisible", isolated<[](smoke::Context const&) { fullyScrolledTopLevelIsVisible(); }> });
	checks.push_back({ "renderPassPaintsScrolledInSectorOnly", isolated<[](smoke::Context const&) { renderPassPaintsScrolledInSectorOnly(); }> });
	checks.push_back({ "liftedSectorAtTheViewportBottomIsDrawn", isolated<[](smoke::Context const&) { liftedSectorAtTheViewportBottomIsDrawn(); }> });
	checks.push_back({ "theGameFollowCameraKeepsLiftedSectorsInView", isolated<[](smoke::Context const&) { theGameFollowCameraKeepsLiftedSectorsInView(); }> });
	checks.push_back({ "aPushedDownLayerIsNotMissedAtTheTopEdge", isolated<[](smoke::Context const&) { aPushedDownLayerIsNotMissedAtTheTopEdge(); }> });
	checks.push_back({ "theLiftedBandKeepsEverySectorTheFlatBandFetched", isolated<[](smoke::Context const&) { theLiftedBandKeepsEverySectorTheFlatBandFetched(); }> });
	checks.push_back({ "aFlatWorldCullsTheBandItAlwaysCulled", isolated<[](smoke::Context const&) { aFlatWorldCullsTheBandItAlwaysCulled(); }> });
	checks.push_back({ "theAperturePassCullsByTheFrontLayersLift", isolated<[](smoke::Context const&) { theAperturePassCullsByTheFrontLayersLift(); }> });
}
