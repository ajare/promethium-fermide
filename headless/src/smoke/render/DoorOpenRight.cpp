#include "Checks.h"
#include "DoorLeafInspection.h"
#include "DoorLeafModel.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
// The real renderer's OpenRight Door passes, re-pinned against the
// translate-under-a-clip leaf model (#1056).
//
// An OpenRight Door's joined leaf slides toward increasing world X. It is
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
// The Broken warning checks ride along here because a broken Door freezes
// its leaf where it stopped, and that frozen leaf is the thing the warning
// has to sit above without moving.
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
#include "core/BulkheadDoorSectorObject.h"
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

	// The colours Render.cpp paints an OpenRight Door with.
	PackedColourValue const kDoorLeafColour = PackedColourValue(ImColor(64, 192, 255));      // the leaf fill
	PackedColourValue const kApertureFillColour = PackedColourValue(ImColor(224, 224, 255)); // BackLocationColour
	PackedColourValue const kThresholdOutlineColour = PackedColourValue(ImColor(0, 0, 0));    // wireframe thresholds
	// The front Room's own surface fill. Deliberately not black, so the
	// wireframe threshold outlines stay distinguishable from it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Broken warning's own colours.
	PackedColourValue const kWarningColour = PackedColourValue(ImColor(255, 166, 26, 255));
	// The Bulkhead Door's slab, which the bulkhead variant of the warning
	// check watches instead of an ordinary leaf.
	PackedColourValue const kBulkheadColour = PackedColourValue(ImColor(128, 192, 182));

	// The Door under test: an ordinary OpenRight Door on Layer 0 at cell
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

	DoorScene buildOpenRightDoorScene(uint32_t cellsWide = 2)
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("OpenRight door render", 8, 3);
		auto& world = *scene.world;
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 7, 1);
		world.addRoom("Back", 1, 0, 0, 7, 1);

		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = cellsWide;
		doorOptions.openStyle = core::Door::OpenStyle::OpenRight;
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
		require(scene.door->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"The Door was not authored OpenRight");
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
	// AddRect's anti-aliasing spreads the outline a hair beyond the exact rect,
	// so the wireframe predictions get a slightly wider tolerance.
	constexpr float kWireframeEpsilon = 1.5f;

	void near(float actual, float expected, char const* what, float epsilon = kEpsilon)
	{
		require(std::abs(actual - expected) < epsilon,
			std::string(what) + " is not where the OpenRight leaf model predicts"
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
		auto scene = buildOpenRightDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenRight, scene.door->getOpenPercentage());
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
		auto const expectedReveal = model.reveal.intersect(scene.aperture);
		if (expectedReveal.empty())
		{
			nearBox(door_leaf::project(model.covered()), apertureBox,
				"a closed Door's leaves over its doorway");
			return;
		}

		auto const expectedBox = door_leaf::project(expectedReveal);
		auto const clip = door_leaf::clipCovering(drawList, kApertureFillColour);
		near(clip.x, expectedBox.left, "the aperture clip's left edge");
		near(clip.z, expectedBox.right, "the aperture clip's right edge");
		near(clip.y, expectedBox.top, "the aperture clip's top edge");
		near(clip.w, expectedBox.bottom, "the aperture clip's bottom edge");
	}

	// Fully open: every leaf has slid clean out of the doorway and paints no
	// fill at all; the back Sector's clip covers the whole aperture.
	void checkFullyOpenSolidPass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenRightDoorScene();
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
		auto scene = buildOpenRightDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenRight, scene.door->getOpenPercentage());

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

	// Fully open: every leaf has slid clean out of the doorway, so the
	// wireframe pass outlines nothing at all - no stroked zero-width
	// rectangle at the jamb - and nothing is filled.
	void checkFullyOpenWireframePass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenRightDoorScene();
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

	DoorScene buildBulkheadScene()
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("Bulkhead warning", 12, 3);
		scene.frontSectorIndex = scene.world->addRoom("Left", 0, 0, 0, 5, 1);
		scene.world->addRoom("Right", 0, 0, 5, 6, 1);
		auto made = scene.world->addSectorBulkheadDoor(0, 0, 5, CORE_SIDE_LEFT);
		scene.door = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(
			made.door.sector->getObject(made.door.index))->getDoor();
		scene.world->finishBuild(); scene.world->pauseSimulation();
		core::Vector2 lo, hi;
		scene.door->getFullShape(lo, hi);
		scene.aperture = {std::min(lo.x, hi.x), std::min(lo.y, hi.y),
			std::max(lo.x, hi.x), std::max(lo.y, hi.y)};
		return scene;
	}

	// A Broken Door keeps its leaf exactly where it stopped and puts the
	// warning above it: the leaf's geometry is untouched by the break, and
	// clearing the break restores both.
	void checkBrokenWarningPreservesPosition(bool bulkhead = false)
	{
		ImGuiGuard imgui;
		for (auto style : { core::Door::OpenStyle::OpenUp, core::Door::OpenStyle::OpenLeft,
			core::Door::OpenStyle::OpenRight, core::Door::OpenStyle::OpenApart })
			for (float fraction : { 0.0f, 0.5f, 1.0f })
			{
				auto scene = bulkhead ? buildBulkheadScene() : buildOpenRightDoorScene();
				scene.door->setOpenStyle(style); driveTo(scene, fraction);
				auto colour = bulkhead ? kBulkheadColour : kDoorLeafColour;
				auto drawList = testDrawList(); drawList->_ResetForNewFrame();
				renderPass(scene, LayerRenderStyle::Solid, drawList);
				auto leaf = door_leaf::extentOf(drawList, colour);
				require(scene.world->setDoorBroken(scene.door->getTraversalResourceId(), true), "Canvas fixture break failed");
				drawList->_ResetForNewFrame(); renderPass(scene, LayerRenderStyle::Solid, drawList);
				auto brokenLeaf = door_leaf::extentOf(drawList, colour);
				auto warning = door_leaf::extentOf(drawList, kWarningColour);
				require(warning.painted() && warning.maxY < door_leaf::toScreenY(scene.door->getSize().y),
					"Broken warning missing or obscuring physical leaf");
				require(leaf.vertexCount == brokenLeaf.vertexCount, "Warning changed leaf geometry");
				if (leaf.painted())
				{
					near(brokenLeaf.minX, leaf.minX, "Frozen leaf left"); near(brokenLeaf.maxX, leaf.maxX, "Frozen leaf right");
					near(brokenLeaf.minY, leaf.minY, "Frozen leaf top"); near(brokenLeaf.maxY, leaf.maxY, "Frozen leaf bottom");
				}
				drawList->_ResetForNewFrame(); renderPass(scene, LayerRenderStyle::Wireframe, drawList);
				require(door_leaf::extentOf(drawList, kWarningColour).painted(), "Wireframe omitted condition warning");
				scene.world->setDoorBroken(scene.door->getTraversalResourceId(), false);
				drawList->_ResetForNewFrame(); renderPass(scene, LayerRenderStyle::Solid, drawList);
				require(!door_leaf::extentOf(drawList, kWarningColour).painted()
					&& std::abs(scene.door->getOpenPercentage() - fraction) < 0.0001f, "Restore warning/position mismatch");
			}
	}
}

void render_smoke::registerDoorOpenRight(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "bulkhead/checkBrokenWarningPreservesPosition", [](smoke::Context const&) { checkBrokenWarningPreservesPosition(true); } });
	checks.push_back({ "doorOpenRight/checkBrokenWarningPreservesPosition", [](smoke::Context const&) { checkBrokenWarningPreservesPosition(); } });
	checks.push_back({ "doorOpenRightSolidPassClosed", isolated<[](smoke::Context const&) { checkSolidPass(0.0f); }> });
	checks.push_back({ "doorOpenRightWireframePassClosed", isolated<[](smoke::Context const&) { checkWireframePass(0.0f); }> });
	checks.push_back({ "doorOpenRightSolidPassHalfOpen", isolated<[](smoke::Context const&) { checkSolidPass(0.5f); }> });
	checks.push_back({ "doorOpenRightWireframePassHalfOpen", isolated<[](smoke::Context const&) { checkWireframePass(0.5f); }> });
	checks.push_back({ "doorOpenRightFullyOpenSolidPass", isolated<[](smoke::Context const&) { checkFullyOpenSolidPass(); }> });
	checks.push_back({ "doorOpenRightFullyOpenWireframePass", isolated<[](smoke::Context const&) { checkFullyOpenWireframePass(); }> });
}
