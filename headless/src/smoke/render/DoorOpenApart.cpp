#include "Checks.h"
#include "DoorLeafInspection.h"
#include "DoorLeafModel.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
// The real renderer's OpenApart Door passes, re-pinned against the
// translate-under-a-clip leaf model (#1056).
//
// An OpenApart Door's two leaves slide apart horizontally, each by half
// the aperture width, so the doorway opens from its centre outward. Each
// leaf is a full-cell tile drawn at its cell rect, translated by the
// style's animation vector and clipped to the doorway aperture - the
// same parameterised draw the other three styles use. This check drives
// the real renderSector() against a live ImDrawList at closed, partial
// and fully open percentages, and reads the vertex buffer and the
// draw-command clip rects back:
//
//   * both leaf quads are whole World cells, one cell wide and one Level
//     tall, wherever they have slid to;
//   * the clip the leaves are painted under is the doorway itself, so a
//     leaf cannot paint over the wall its collision stops in;
//   * the back Sector's fill is clipped to the middle region the leaves
//     have vacated, and at 0% open nothing is vacated because the leaves
//     cover the whole doorway;
//   * the seam where the two leaves meet stays a drawn line rather than
//     opening into a gap or doubling into a band, and it sits at the
//     aperture centre for a 1-wide Door and at the cell boundary for a
//     2-wide one, tracking outward symmetrically as the Door opens;
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

	// The colours Render.cpp paints an OpenApart Door with.
	PackedColourValue const kDoorLeafColour = PackedColourValue(ImColor(64, 192, 255));      // the leaf fill
	PackedColourValue const kApertureFillColour = PackedColourValue(ImColor(224, 224, 255)); // BackLocationColour
	PackedColourValue const kThresholdOutlineColour = PackedColourValue(ImColor(0, 0, 0));    // wireframe thresholds
	// The front Room's own surface fill. Deliberately not black, so the
	// wireframe threshold outlines stay distinguishable from it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Door under test: an ordinary OpenApart Door on Layer 0 at cell
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

	DoorScene buildOpenApartDoorScene(uint32_t cellsWide = 2)
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("OpenApart door render", 8, 3);
		auto& world = *scene.world;
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 7, 1);
		world.addRoom("Back", 1, 0, 0, 7, 1);

		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = cellsWide;
		doorOptions.openStyle = core::Door::OpenStyle::OpenApart;
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
		require(scene.door->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"The Door was not authored OpenApart");
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
			std::string(what) + " is not where the OpenApart leaf model predicts"
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
	// to the middle region they have vacated.
	void checkSolidPass(float openPct)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenApart, scene.door->getOpenPercentage());
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
		// middle region only, which grows from nothing at closed to the whole
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

	// Fully open: both leaves have slid clean out of the doorway and paint
	// no fill at all; the back Sector's clip covers the whole aperture.
	void checkFullyOpenSolidPass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene();
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
		auto scene = buildOpenApartDoorScene();
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenApart, scene.door->getOpenPercentage());

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

	// Fully open: both leaves have slid clean out of the doorway, so the
	// wireframe pass outlines nothing at all - no stroked zero-width
	// rectangle at the centre line - and nothing is filled.
	void checkFullyOpenWireframePass()
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene();
		driveTo(scene, 1.0f);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Wireframe, drawList);

		require(!door_leaf::extentOf(drawList, kThresholdOutlineColour).painted(),
			"the fully-open wireframe pass drew a leaf outline at the centre line");
		require(!door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
			"the fully-open wireframe pass filled the leaf solid");
		require(!door_leaf::extentOf(drawList, kApertureFillColour).painted(),
			"the fully-open wireframe pass filled the aperture");
	}

	// The X the two leaves' inner edges meet at, before they slide: the
	// aperture centre for a 1-wide Door, where the single cell is split
	// between the two leaves, and the shared cell boundary for a 2-wide
	// one, where each leaf owns a whole cell.
	float closedSeamWorldX(door_leaf::Plan const& model)
	{
		if (model.cellsWide == 1)
			return (model.aperture.x0 + model.aperture.x1) * 0.5f;
		return model.cellX0 + static_cast<float>(model.cellsWide - 1);
	}

	// How far the right-hand leaf has travelled so far - the left-hand
	// leaf travels the same distance the other way, so this and the closed
	// seam position fix where the join sits now.
	float rightLeafTravel(door_leaf::Plan const& model)
	{
		for (auto const& leaf : model.leaves)
			if (leaf.innerEdgeIsLeft) return leaf.travelX;
		return 0.0f;
	}

	float seamWorldX(door_leaf::Plan const& model)
	{
		return closedSeamWorldX(model) + rightLeafTravel(model);
	}

	// The seam where the two leaves meet stays a drawn line: the leaves'
	// inner edges are both present at the same X, so the join neither
	// opens into a gap nor doubles into a band. Both passes are checked -
	// the Solid pass where the leaf fills meet, and the wireframe pass
	// where their outlines are stroked.
	void checkSeamIsDrawnLine(float openPct, uint32_t cellsWide)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene(cellsWide);
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenApart, scene.door->getOpenPercentage());
		auto const seam = door_leaf::toScreenX(seamWorldX(model));

		auto const seamFilled = [&](PackedColourValue colour)
		{
			auto const xs = door_leaf::xsOf(drawList, colour);
			require(door_leaf::anyNear(xs, seam, kEpsilon),
				"the seam is not a drawn line at " + std::to_string(seam)
				+ " - the leaves' inner edges do not meet there");
		};
		seamFilled(kDoorLeafColour);

		// A 1-wide Door splits one cell between two leaves, so the art on
		// each half has to mirror the other about the cell centre or the
		// closed Door would read as two mismatched halves.
		if (model.cellsWide == 1)
		{
			auto const centre = door_leaf::toScreenX(model.cellX0 + 0.5f);
			auto const quads = door_leaf::filledRectsOf(drawList, kDoorLeafColour);
			require(quads.size() == 2,
				"a 1-wide OpenApart Door should paint two leaf quads, got "
				+ std::to_string(quads.size()));
			for (auto const& q : quads)
			{
				auto const mirrored = door_leaf::ScreenBox{
					2.0f * centre - q.right, q.top, 2.0f * centre - q.left, q.bottom};
				auto const partner = std::find_if(quads.begin(), quads.end(),
					[&](door_leaf::ScreenBox const& p)
					{
						return std::abs(p.left - mirrored.left) < kEpsilon
							&& std::abs(p.right - mirrored.right) < kEpsilon
							&& std::abs(p.top - mirrored.top) < kEpsilon
							&& std::abs(p.bottom - mirrored.bottom) < kEpsilon;
					});
				require(partner != quads.end(),
					"a 1-wide OpenApart leaf has no mirror partner about the cell centre");
			}
		}

		drawList->_ResetForNewFrame();
		renderPass(scene, LayerRenderStyle::Wireframe, drawList);
		seamFilled(kThresholdOutlineColour);
	}

	// The seam tracks outward symmetrically: the two leaves' inner edges
	// stay the same distance either side of the aperture centre, so the
	// doorway opens evenly from its middle.
	void checkSeamSymmetry(float openPct, uint32_t cellsWide)
	{
		ImGuiGuard imgui;
		auto scene = buildOpenApartDoorScene(cellsWide);
		driveTo(scene, openPct);
		auto drawList = testDrawList();

		renderPass(scene, LayerRenderStyle::Solid, drawList);

		auto const model = door_leaf::plan(scene.door->getCellsWide(), scene.aperture,
			core::Door::OpenStyle::OpenApart, scene.door->getOpenPercentage());
		auto const centre = door_leaf::toScreenX((scene.aperture.x0 + scene.aperture.x1) * 0.5f);
		auto const seam = door_leaf::toScreenX(seamWorldX(model));
		auto const other = 2.0f * centre - seam;
		auto const xs = door_leaf::xsOf(drawList, kDoorLeafColour);
		require(door_leaf::anyNear(xs, seam, kEpsilon) && door_leaf::anyNear(xs, other, kEpsilon),
			"the leaves' inner edges are not symmetric about the aperture centre");
	}
}


void render_smoke::registerDoorOpenApart(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorOpenApartSolidPassClosed", isolated<[](smoke::Context const&) { checkSolidPass(0.0f); }> });
	checks.push_back({ "doorOpenApartWireframePassClosed", isolated<[](smoke::Context const&) { checkWireframePass(0.0f); }> });
	checks.push_back({ "doorOpenApartSolidPassHalfOpen", isolated<[](smoke::Context const&) { checkSolidPass(0.5f); }> });
	checks.push_back({ "doorOpenApartWireframePassHalfOpen", isolated<[](smoke::Context const&) { checkWireframePass(0.5f); }> });
	checks.push_back({ "doorOpenApartFullyOpenSolidPass", isolated<[](smoke::Context const&) { checkFullyOpenSolidPass(); }> });
	checks.push_back({ "doorOpenApartFullyOpenWireframePass", isolated<[](smoke::Context const&) { checkFullyOpenWireframePass(); }> });
	checks.push_back({ "doorOpenApartSeamIsDrawnLineClosed", isolated<[](smoke::Context const&) { checkSeamIsDrawnLine(0.0f, 2); }> });
	checks.push_back({ "doorOpenApartSeamIsDrawnLineHalfOpen", isolated<[](smoke::Context const&) { checkSeamIsDrawnLine(0.5f, 2); }> });
	checks.push_back({ "doorOpenApartSeamIsDrawnLineClosedOneWide", isolated<[](smoke::Context const&) { checkSeamIsDrawnLine(0.0f, 1); }> });
	checks.push_back({ "doorOpenApartSeamSymmetryHalfOpen", isolated<[](smoke::Context const&) { checkSeamSymmetry(0.5f, 2); }> });
	checks.push_back({ "doorOpenApartSeamSymmetryHalfOpenOneWide", isolated<[](smoke::Context const&) { checkSeamSymmetry(0.5f, 1); }> });
}
