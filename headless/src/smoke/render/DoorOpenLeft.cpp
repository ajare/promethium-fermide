#include "Checks.h"
#include "DoorLeafInspection.h"
#include "DoorLeafModel.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
// The real renderer's OpenLeft Door passes, re-pinned against the
// translate-under-a-clip leaf model (#1056).
//
// An OpenLeft Door's joined leaf slides toward decreasing world X. It is
// no longer a strip cropped from the leaf tile's own edge: each of the
// Door's cells wears a full-cell tile, drawn at the cell rect, translated
// by the style's animation vector and clipped to the doorway aperture.
// The aperture is inset from the cell by CORE_DOOR_X_INSET on each side,
// so the leaf's inner edge starts one inset outside the aperture and
// clearing the far jamb takes the aperture width plus that inset. This
// check drives the real renderSector() - which reaches renderDoor() and
// the one parameterised leaf draw - against a live ImDrawList at closed,
// partial and fully open percentages, and reads the vertex buffer and the
// draw-command clip rects back:
//
//   * every leaf quad is a whole World cell, one cell wide and one Level
//     tall, wherever it has slid to - the tile is never squashed into the
//     strip of aperture it still covers;
//   * the clip the leaves are painted under is the doorway itself, so a
//     leaf cannot paint over the wall its collision stops in;
//   * the back Sector's fill is clipped to the region the leaves have
//     vacated, and at 0% open nothing is vacated because the leaves cover
//     the whole doorway;
//   * a leaf that has slid clean out of the doorway emits no geometry at
//     all, so a fully open Door paints neither leaf nor outline.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/Vector2.h"
#include "UISettings.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// The colours Render.cpp paints an OpenLeft Door with.
	PackedColourValue const kDoorLeafColour = PackedColourValue(ImColor(64, 192, 255));      // the leaf fill
	PackedColourValue const kApertureFillColour = PackedColourValue(ImColor(224, 224, 255)); // BackLocationColour
	PackedColourValue const kThresholdOutlineColour = PackedColourValue(ImColor(0, 0, 0));    // wireframe thresholds
	// The front Room's own surface fill. Deliberately not black, so the
	// wireframe threshold outlines stay distinguishable from it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Door under test: an ordinary OpenLeft Door on Layer 0 at cell
	// (3, 0), with a Room directly behind it on Layer 1.
	struct DoorScene
	{
		std::unique_ptr<core::World> world;
		uint32_t frontSectorIndex{ 0 };
		std::shared_ptr<core::Door> door;
		// The Door aperture in world (cell) units.
		door_leaf::Rect aperture;
	};

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches a
	// window or the GPU.
	using ImGuiGuard = headless::ScopedImGuiContext;

	DoorScene buildOpenLeftDoorScene(uint32_t cellsWide = 2)
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("OpenLeft door render", 8, 3);
		auto& world = *scene.world;
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 7, 1);
		world.addRoom("Back", 1, 0, 0, 7, 1);

		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = cellsWide;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const created = world.addSectorDoor(0, 0, 3, doorOptions);
		world.finishBuild();
		world.pauseSimulation();

		scene.frontSectorIndex = created.door.sector->getIndex();
		auto sector = world.getSector(scene.frontSectorIndex);
		require(sector != nullptr, "The Door's front Sector vanished");
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (object && object->getObjectType() == core::SectorObjectType::Door)
			{
				scene.door = std::const_pointer_cast<core::Door>(
					std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor());
				break;
			}
		}
		require(scene.door != nullptr, "The ordinary Door could not be found");
		require(scene.door->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"The Door was not authored OpenLeft");
		require(scene.door->getCellsWide() == cellsWide,
			"The Door was not authored the width the check asked for");

		core::Vector2 lo, hi;
		scene.door->getFullShape(lo, hi);
		scene.aperture = {std::min(lo.x, hi.x), std::min(lo.y, hi.y),
			std::max(lo.x, hi.x), std::max(lo.y, hi.y)};
		return scene;
	}

	// Drive the Door to an exact open percentage through its own update loop.
	void driveTo(DoorScene const& scene, float targetPct)
	{
		auto& door = *scene.door;
		if (targetPct <= 0.0f)
		{
			require(door.getOpenPercentage() <= 0.0f,
				"A freshly built Door should start closed");
			return;
		}
		door.open();
		auto const step = door.getOpenCloseTime() * 0.25f;
		while (door.getOpenPercentage() < targetPct)
			door.update(step);
		require(door.getOpenPercentage() <= 1.0f, "The Door opened past fully open");
		if (targetPct >= 1.0f)
			require(door.isOpen(), "The Door did not reach the Open state");
	}

	// The viewport-qualified draw list: the plain form needs a current window,
	// which headless never has.
	ImDrawList* testDrawList()
	{
		// Give the transform a non-degenerate viewport so pushed clip rects
		// survive intersection with the on-screen rect.
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
	}

	void renderPass(DoorScene const& scene, LayerRenderStyle style, ImDrawList* drawList)
	{
		// Outside a framed ImGui::Render() the background draw list carries an
		// empty clip; give the pass the viewport rect a real frame would, so the
		// renderer's own PushClipRect intersects against something sane.
		drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
		renderSector(scene.world->getSector(scene.frontSectorIndex), 0, style, false,
			kFrontRoomColour, drawList);
		drawList->PopClipRect();
	}

	constexpr float kEpsilon = 0.01f;
	// The stall checks measure strips a couple of pixels wide off the back
	// Sector's clip, so they get a looser tolerance than the box checks.
	constexpr float kStallEpsilon = 0.25f;
	// AddRect's anti-aliasing spreads the outline a hair beyond the exact rect,
	// so the wireframe predictions get a slightly wider tolerance.
	constexpr float kWireframeEpsilon = 1.5f;

	void near(float actual, float expected, char const* what, float epsilon = kEpsilon)
	{
		require(std::abs(actual - expected) < epsilon,
			std::string(what) + " is not where the OpenLeft leaf model predicts"
			+ " (expected " + std::to_string(expected) + ", got " + std::to_string(actual) + ")");
	}

	void nearBox(door_leaf::ScreenBox const& actual, door_leaf::ScreenBox const& expected,
		std::string const& what, float epsilon = kEpsilon)
	{
		near(actual.left, expected.left, (what + " left edge").c_str(), epsilon);
		near(actual.top, expected.top, (what + " top edge").c_str(), epsilon);
		near(actual.right, expected.right, (what + " right edge").c_str(), epsilon);
		near(actual.bottom, expected.bottom, (what + " bottom edge").c_str(), epsilon);
	}

	// The leaf quads the renderer emitted, matched one-for-one against the
	// leaves the model says are still inside the doorway. Each one is a whole
	// World cell, and each one was painted under the doorway's own clip.
	void checkLeafQuads(std::vector<door_leaf::ScreenBox> const& quads,
		door_leaf::Plan const& model, std::string const& pass)
	{
		auto const drawn = model.drawn();
		require(quads.size() == drawn.size(),
			pass + " painted " + std::to_string(quads.size())
			+ " leaf quads, the model says " + std::to_string(drawn.size())
			+ " leaves are still inside the doorway");

		for (auto const& leaf : drawn)
		{
			auto const expected = door_leaf::project(leaf.slid());
			auto const found = std::find_if(quads.begin(), quads.end(),
				[&](door_leaf::ScreenBox const& q)
				{
					return std::abs(q.left - expected.left) < kEpsilon
						&& std::abs(q.right - expected.right) < kEpsilon;
				});
			require(found != quads.end(),
				pass + " painted no leaf quad where the model puts one at "
				+ std::to_string(expected.left) + " -> " + std::to_string(expected.right));
			// A full-cell tile drawn at its cell rect: one World cell wide
			// and one Level tall, however far it has slid.
			near(found->width(), CORE_CELL_WIDTH_PIXELS,
				(pass + " leaf quad is not one cell wide").c_str());
			near(found->height(), CORE_LEVEL_HEIGHT_PIXELS,
				(pass + " leaf quad is not one Level tall").c_str());
		}
	}

	// Solid pass at a given percentage: the leaf cells are whole cells
	// painted under the doorway clip, and the back Sector's fill is clipped
	// to the region they have vacated.
	void checkSolidPass(float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenLeft, scene.door->getOpenPercentage());
		auto const apertureBox = door_leaf::project(scene.aperture);

		auto const quads = door_leaf::filledRectsOf(drawList, kDoorLeafColour);
		require(quads.empty() == model.drawn().empty(),
			"the Solid pass and the leaf model disagree about whether any leaf is drawn");
		checkLeafQuads(quads, model, "the Solid pass");

		// The clip the leaves were painted under is the doorway itself, so a
		// leaf cannot paint over the wall its collision stops in.
		if (!quads.empty())
		{
			auto const leafClip = door_leaf::clipCovering(drawList, kDoorLeafColour);
			near(leafClip.x, apertureBox.left, "the leaf clip's left edge");
			near(leafClip.z, apertureBox.right, "the leaf clip's right edge");
			near(leafClip.y, apertureBox.top, "the leaf clip's top edge");
			near(leafClip.w, apertureBox.bottom, "the leaf clip's bottom edge");
		}

		// The back Sector's aperture fill: the Door clips it to the vacated
		// region only, which grows from nothing at closed to the whole
		// aperture at fully open. With nothing vacated the Door issues no
		// reveal draw at all, and what is asserted instead is that the leaves
		// cover the doorway completely - which is why there is nothing to
		// reveal.
		// The back Sector is clipped to the whole doorway rather than to
		// the strip the leaves have vacated: the leaves are drawn over it
		// and cover what they cover, so the draw computes no strip at all
		// (#1073). A closed Door draws no back Sector - its leaves stand
		// across the whole doorway, so that draw would paint over itself -
		// which is the same thing the empty reveal used to buy by
		// construction, now asked for directly.
		auto const expectedReveal = model.reveal.intersect(scene.aperture);
		if (expectedReveal.empty())
		{
			require(!door_leaf::extentOf(drawList, kApertureFillColour).painted(),
				"a closed Door drew the Sector behind a doorway its own leaves "
				"already cover");
			nearBox(door_leaf::project(model.covered()), apertureBox,
				"a closed Door's leaves over its doorway");
			return;
		}

		auto const clip = door_leaf::clipCovering(drawList, kApertureFillColour);
		near(clip.x, apertureBox.left, "the back Sector clip's left edge");
		near(clip.z, apertureBox.right, "the back Sector clip's right edge");
		near(clip.y, apertureBox.top, "the back Sector clip's top edge");
		near(clip.w, apertureBox.bottom, "the back Sector clip's bottom edge");
	}

	// Fully open: every leaf has slid clean out of the doorway and paints no
	// fill at all; the back Sector's clip covers the whole aperture.
	void checkFullyOpenSolidPass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		require(!door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
			"the fully-open Solid pass painted a leaf that has slid clean out of the doorway");

		auto const apertureBox = door_leaf::project(scene.aperture);
		auto const back = door_leaf::extentOf(drawList, kApertureFillColour);
		require(back.painted(), "no aperture fill reached the Sector behind");
		auto const clip = door_leaf::clipCovering(drawList, kApertureFillColour);
		near(clip.x, apertureBox.left, "the aperture clip's left edge");
		near(clip.z, apertureBox.right, "the aperture clip's right edge");
		near(clip.y, apertureBox.top, "the aperture clip's top edge");
		near(clip.w, apertureBox.bottom, "the aperture clip's bottom edge");
	}

	// Wireframe pass at a given percentage: the leaves still inside the
	// doorway are outlined as whole cells and nothing is filled - no leaf
	// colour, no back Sector colour.
	void checkWireframePass(float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenLeft, scene.door->getOpenPercentage());

		// An outlined rect carries more than four vertices once its
		// anti-aliasing is counted, so the wireframe pass is asserted as the
		// extent the outlines cover rather than as separate quads.
		auto const outline = door_leaf::extentOf(drawList, kThresholdOutlineColour);
		require(outline.painted() == !model.drawn().empty(),
			"the wireframe pass and the leaf model disagree about whether any leaf is outlined");
		if (!model.drawn().empty())
		{
			auto const expected = door_leaf::project(model.slidUnion());
			near(outline.minX, expected.left, "the outlined leaves' left edge",
				kWireframeEpsilon);
			near(outline.maxX, expected.right, "the outlined leaves' right edge",
				kWireframeEpsilon);
			near(outline.minY, expected.top, "the outlined leaves' top edge",
				kWireframeEpsilon);
			near(outline.maxY, expected.bottom, "the outlined leaves' bottom edge",
				kWireframeEpsilon);
		}

		require(!door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
			"the wireframe pass filled the leaf solid");
		require(!door_leaf::extentOf(drawList, kApertureFillColour).painted(),
			"the wireframe pass filled the aperture");
	}

	// A paused simulation freezes the leaf exactly where it stopped
	// mid-travel: the artwork is a pure function of the open percentage,
	// so ticking a paused World moves nothing. (#1056)
	void checkPausedSimulationFreezesTheLeafMidTravel()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		// Half way through the first half of the travel, where both leaf
		// cells are still inside the doorway and neither has been culled.
		driveTo(scene, 0.25f);
		require(scene.world->isSimulationPaused(),
			"The fixture should be paused for a freeze check");

		auto drawList = testDrawList();
		renderPass(scene, LayerRenderStyle::Solid, drawList);
		auto const before = door_leaf::filledRectsOf(drawList, kDoorLeafColour);
		auto const pctBefore = scene.door->getOpenPercentage();
		require(before.size() == 2,
			"a mid-travel 2-wide Door should paint two leaf quads, got "
			+ std::to_string(before.size()));

		for (int tick = 0; tick < 5; ++tick) scene.world->update(0.1f);

		drawList->_ResetForNewFrame();
		renderPass(scene, LayerRenderStyle::Solid, drawList);
		auto const after = door_leaf::filledRectsOf(drawList, kDoorLeafColour);

		require(scene.door->getOpenPercentage() == pctBefore,
			"a paused World advanced the Door's open percentage from "
			+ std::to_string(pctBefore) + " to "
			+ std::to_string(scene.door->getOpenPercentage()));
		require(after.size() == before.size(),
			"the leaf geometry changed across a paused World tick");
		for (std::size_t i = 0; i < before.size(); ++i)
			nearBox(after[i], before[i], "a frozen leaf quad");
	}

	// A 2-wide Door's two leaf cells join seamlessly: they meet on their
	// shared cell edge with no gap between them and no overlap across it,
	// so the joined leaf reads as one leaf rather than two tiles that
	// happen to be nearby. (#1056)
	void checkTwoLeafCellsJoinSeamlessly()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene(2);
		driveTo(scene, 0.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const quads = door_leaf::filledRectsOf(drawList, kDoorLeafColour);
		require(quads.size() == 2,
			"a closed 2-wide Door should paint two leaf quads, got "
			+ std::to_string(quads.size()));

		auto const left = std::min_element(quads.begin(), quads.end(),
			[](door_leaf::ScreenBox const& a, door_leaf::ScreenBox const& b)
			{ return a.left < b.left; });
		auto const right = std::max_element(quads.begin(), quads.end(),
			[](door_leaf::ScreenBox const& a, door_leaf::ScreenBox const& b)
			{ return a.left < b.left; });

		// The shared cell edge is where the two leaves meet: the left
		// cell's right edge is the right cell's left edge, exactly.
		near(left->right, right->left,
			"the two leaf cells do not meet on their shared cell edge");
		// Same height, same floor line: the join is edge to edge, not
		// corner to corner.
		near(left->top, right->top, "the two leaf cells sit at different heights");
		near(left->bottom, right->bottom,
			"the two leaf cells stand on different floor lines");

		// Together they cover the doorway with nothing left over on either
		// side of the join.
		// The cells are whole cells, wider than the doorway they are clipped
		// to, so what the join has to guarantee is that between them they
		// cover the aperture from jamb to jamb with nothing missing at the
		// seam.
		auto const unionBox = door_leaf::unionOf(quads);
		auto const apertureBox = door_leaf::project(scene.aperture);
		require(unionBox.minX <= apertureBox.left + kEpsilon
				&& unionBox.maxX >= apertureBox.right - kEpsilon,
			"the joined leaves do not cover the doorway from jamb to jamb");
	}

	// Drives the Door to an open percentage fine enough to stop inside
	// the first few percent of its travel. The coarse quarter-cycle step
	// the other checks use cannot: the stall #1061 is about swallowed a
	// tenth of the travel, so a check that wants to catch the reveal
	// already under way inside it needs thousandths of a cycle.
	void driveToExact(DoorScene const& scene, float targetPct)
	{
		auto& door = *scene.door;
		require(door.getOpenPercentage() <= 0.0f,
			"The stall fixture should start from a closed Door");
		door.open();
		auto const step = door.getOpenCloseTime() / 1000.0f;
		while (door.getOpenPercentage() < targetPct) door.update(step);
		require(door.getOpenPercentage() <= targetPct + 0.001f,
			"The stall fixture overshot " + std::to_string(targetPct)
			+ " open, reaching " + std::to_string(door.getOpenPercentage()));
	}

	// One percentage of the travel, measured off the draw list in
	// absolute pixels rather than against the model that predicted it.
	//
	// #1061: a side-sliding leaf's visible edge starts on the far jamb
	// the instant the Door opens, moves at a uniform speed, and has
	// covered exactly one aperture width by the time the Door is fully
	// open. The stall the old travel produced was invisible to a check
	// that only looked at closed and fully open, and the leaf quads say
	// nothing either way because they are whole cells wherever they sit.
	// What carries the animation is the strip the leaves have vacated,
	// which the back Sector's fill clip carries, and what is left of the
	// leaves inside the doorway. Those are the two numbers read here.
	void checkVisibleEdgeAt(uint32_t cellsWide, float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene(cellsWide);
		driveToExact(scene, openPct);
		auto const pct = scene.door->getOpenPercentage();
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const apertureBox = door_leaf::project(scene.aperture);
		auto const widthPx = apertureBox.width();
		auto const vacatedPx = pct * widthPx;
		require(vacatedPx > 1.0f,
			"the fixture is so far closed that the vacated strip is below the "
			"measurement floor, so this percentage proves nothing");

		// The vacated strip runs back from the far jamb: an OpenLeft leaf
		// slides away from the right-hand jamb, so that is the side that
		// uncovers first, and it uncovers openPct of the doorway. Asked
		// first whether the strip stays inside the doorway at all, because
		// that says more than a mis-measured edge does: a reveal that has
		// overshot its jamb inverts in World units, and a projection that
		// normalises it would show "nothing vacated" as a strip of back
		// Sector painted over the frame.
		// The back Sector's clip is the whole doorway now, so it says
		// nothing about where the leaf has got to (#1073): the vacated
		// strip is not a clip any more, it is whatever the leaves have not
		// painted over. What the strip is bounded by is the doorway the
		// whole draw sits inside, and that is asserted here; the travel is
		// read off the leaves themselves below, which is where it always
		// actually was.
		auto const clip = door_leaf::clipCovering(drawList, kApertureFillColour);
		near(clip.x, apertureBox.left, "the back Sector clip's left edge");
		near(clip.z, apertureBox.right, "the back Sector clip's right edge");

		// What the leaves still cover inside the doorway, measured off the
		// quads they painted and clipped to the aperture. This is the
		// number a frozen leaf keeps at the full aperture width, which is
		// exactly what the stall looked like - and for an OpenLeft leaf it
		// is the vacated strip too, read from the other end: the leaves'
		// visible right edge is the near edge of what the leaf has uncover-
		// ed, and it has to sit openPct of the doorway in from the right
		// jamb.
		auto const quads = door_leaf::filledRectsOf(drawList, kDoorLeafColour);
		require(!quads.empty(),
			"a barely-open Door painted no leaf at all, so there is nothing "
			"left covering the doorway");
		auto const covered = door_leaf::unionOf(quads);
		auto const coveredPx = std::min(covered.maxX, apertureBox.right)
			- std::max(covered.minX, apertureBox.left);
		near(coveredPx, widthPx - vacatedPx,
			"the leaves do not cover the complement of the reveal, so the "
			"visible coverage is not what the travel promises", kStallEpsilon);
		near(std::min(covered.maxX, apertureBox.right),
			apertureBox.right - vacatedPx,
			"the leaves' visible edge is not openPct of the aperture in from "
			"the far jamb, which is a stalled start, a short travel, or a "
			"visible edge that does not move at a uniform speed",
			kStallEpsilon);
	}

	// The whole side-sliding travel. 0.02 is a fifth of the 11.1% a
	// 1-wide Door used to spend entirely behind the far jamb; 0.05 and
	// 0.1 straddle what was left of that stall; the rest pin the uniform
	// speed through the middle of the travel.
	void checkVisibleEdgeStartsAtTheJamb(uint32_t cellsWide)
	{
		for (float const openPct : { 0.02f, 0.05f, 0.1f, 0.25f, 0.5f, 0.75f })
			checkVisibleEdgeAt(cellsWide, openPct);
	}

	// Fully open, nothing of the leaf is left inside the doorway to paint
	// across either jamb: no leaf fill reaches the doorway, and the strip
	// the leaves have vacated is the whole of it.
	void checkFullyOpenClearsBothJambs(uint32_t cellsWide)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene(cellsWide);
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		require(!door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
			"a fully open " + std::to_string(cellsWide)
			+ "-wide OpenLeft Door still painted leaf artwork across a jamb");

		auto const apertureBox = door_leaf::project(scene.aperture);
		auto const reveal = door_leaf::clipCovering(drawList, kApertureFillColour);
		near(reveal.x, apertureBox.left, "the fully open reveal's left edge");
		near(reveal.z, apertureBox.right, "the fully open reveal's right edge");
	}

	// Fully open: every leaf has slid clean out of the doorway, so the
	// wireframe pass outlines nothing at all - no stroked zero-width
	// rectangle at the jamb - and nothing is filled.
	void checkFullyOpenWireframePass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenLeftDoorScene();
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		require(!door_leaf::extentOf(drawList, kThresholdOutlineColour).painted(),
			"the fully-open wireframe pass drew a leaf outline at the jamb");
		require(!door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
			"the fully-open wireframe pass filled the leaf solid");
		require(!door_leaf::extentOf(drawList, kApertureFillColour).painted(),
			"the fully-open wireframe pass filled the aperture");
	}
}


void render_smoke::registerDoorOpenLeft(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorOpenLeftSolidPassClosed", isolated<[](smoke::Context const&) { checkSolidPass(0.0f); }> });
	checks.push_back({ "doorOpenLeftWireframePassClosed", isolated<[](smoke::Context const&) { checkWireframePass(0.0f); }> });
	checks.push_back({ "doorOpenLeftSolidPassHalfOpen", isolated<[](smoke::Context const&) { checkSolidPass(0.5f); }> });
	checks.push_back({ "doorOpenLeftWireframePassHalfOpen", isolated<[](smoke::Context const&) { checkWireframePass(0.5f); }> });
	checks.push_back({ "doorOpenLeftFullyOpenSolidPass", isolated<[](smoke::Context const&) { checkFullyOpenSolidPass(); }> });
	checks.push_back({ "doorOpenLeftFullyOpenWireframePass", isolated<[](smoke::Context const&) { checkFullyOpenWireframePass(); }> });
	checks.push_back({ "doorOpenLeftPausedSimulationFreezesTheLeafMidTravel", isolated<[](smoke::Context const&) { checkPausedSimulationFreezesTheLeafMidTravel(); }> });
	checks.push_back({ "doorOpenLeftTwoLeafCellsJoinSeamlessly", isolated<[](smoke::Context const&) { checkTwoLeafCellsJoinSeamlessly(); }> });
	checks.push_back({ "doorOpenLeftVisibleEdgeStartsAtTheJambOneWide", isolated<[](smoke::Context const&) { checkVisibleEdgeStartsAtTheJamb(1u); }> });
	checks.push_back({ "doorOpenLeftVisibleEdgeStartsAtTheJambTwoWide", isolated<[](smoke::Context const&) { checkVisibleEdgeStartsAtTheJamb(2u); }> });
	checks.push_back({ "doorOpenLeftFullyOpenClearsBothJambsOneWide", isolated<[](smoke::Context const&) { checkFullyOpenClearsBothJambs(1u); }> });
}
