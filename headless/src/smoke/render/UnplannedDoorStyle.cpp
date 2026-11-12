#include "Checks.h"
#include "DoorLeafInspection.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
// What the one parameterised Door draw does with an opening style it has
// no arm for (#1068).
//
// `planDoorLeaves` fills a `DoorLeafPlan` from a switch on the Door's
// opening style, and the plan's `reveal` is the rect the back Sector is
// clipped to. While the rects carried no default initialiser, a fifth
// `OpenStyle` added to core without a matching arm compiled cleanly and
// then clipped that back Sector to four indeterminate floats - a wrong
// picture driven by uninitialised memory. The rects default to the zero
// rect now, and the switch has a `default:` arm that refuses. These checks
// pin both halves, and the half that has to stay true whatever the style
// list grows to: an empty reveal draws no back Sector, and a planned style
// still draws its leaves.
//
// The unplanned style is driven as the value one past the last enumerator
// the renderer names, which is exactly how a new style arrives at a plan
// that has not learned it yet.
//
// Everything runs headless: ImGui is created without a renderer, so no
// window, dialog, or GPU is ever touched.

#include <cmath>
#include <cstdint>
#include <memory>
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
	using smoke::require;

	// The colours Render.cpp paints a Door with: the leaf fill, and the
	// tint it gives the Sector revealed through the doorway.
	PackedColourValue const kDoorLeafColour = PackedColourValue(ImColor(64, 192, 255));
	PackedColourValue const kApertureFillColour = PackedColourValue(ImColor(224, 224, 255));
	// The front Room's own surface fill. Deliberately not either of the
	// two above, so neither the leaf nor the reveal can be confused with it.
	ImColor const kFrontRoomColour(192, 192, 255);

	// The Door under test: an ordinary Door on Layer 0 in a Room, with a
	// Room directly behind it on Layer 1.
	struct DoorScene
	{
		std::unique_ptr<core::World> world;
		uint32_t frontSectorIndex{ 0 };
		std::shared_ptr<core::Door> door;
		// The Door aperture in world (cell) units.
		door_leaf::Rect aperture;
	};

	// ImGui without a renderer: contexts are CPU-side only, nothing reaches
	// a window or the GPU.
	using ImGuiGuard = headless::ScopedImGuiContext;

	// The opening style `planDoorLeaves` has no arm for: the value one
	// past the last enumerator it names. A fifth `OpenStyle` added to core
	// reaches the plan exactly this way - as a value the switch does not
	// name - so the check keeps meaning that whatever the enumerator list
	// grows to, and does not go stale the day one is added (#1068).
	int unplannedStyleValue()
	{
		return static_cast<int>(core::Door::OpenStyle::OpenApart) + 1;
	}

	core::Door::OpenStyle unplannedStyle()
	{
		return static_cast<core::Door::OpenStyle>(unplannedStyleValue());
	}

	DoorScene buildDoorScene(core::Door::OpenStyle style, uint32_t cellsWide = 2)
	{
		DoorScene scene;
		scene.world = std::make_unique<core::World>("Door leaf plan safety", 8, 3);
		auto& world = *scene.world;
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Front", 0, 0, 0, 7, 1);
		world.addRoom("Back", 1, 0, 0, 7, 1);

		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = cellsWide;
		doorOptions.openStyle = style;
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
		require(scene.door->getOpenStyle() == style,
			"The Door was not authored with the opening style the check asked for");
		require(scene.door->getCellsWide() == cellsWide,
			"The Door was not authored the width the check asked for");

		core::Vector2 lo, hi;
		scene.door->getFullShape(lo, hi);
		scene.aperture = {std::min(lo.x, hi.x), std::min(lo.y, hi.y),
			std::max(lo.x, hi.x), std::max(lo.y, hi.y)};
		return scene;
	}

	// Drives the Door to an exact open percentage through its own update
	// loop, so the reveal is the one a real frame would carry.
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
	}

	// The viewport-qualified draw list: the plain form needs a current
	// window, which headless never has.
	ImDrawList* testDrawList()
	{
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 1280.0f;
		gUISettings.worldViewportHeight = 720.0f;
		return ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
	}

	// The real Sector pass, which reaches renderDoor() and the one
	// parameterised leaf draw the same way a viewport frame does.
	void renderPass(DoorScene const& scene, LayerRenderStyle style, ImDrawList* drawList)
	{
		drawList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(1280.0f, 720.0f), false);
		renderSector(scene.world->getSector(scene.frontSectorIndex), 0, style, false,
			kFrontRoomColour, drawList);
		drawList->PopClipRect();
	}

	// Runs the Door's own draw into a recording command stream and hands
	// back the refusal, or empty when the draw was accepted. The stream is
	// read back rather than a replayed draw list because it holds what the
	// Door itself emitted and nothing else: `planDoorLeaves` is the first
	// thing the pass does, so a refusal that left no commands behind is
	// known to have arrived before the Door recorded a clip, a leaf, or a
	// reveal.
	std::string doorDrawRefusal(std::shared_ptr<core::Door> const& door,
		ImDrawList* drawList, std::size_t& commandsRecorded)
	{
		WorldDrawList commands(headless::clipUnder(drawList));
		try
		{
			renderDoor(door, 0, LayerRenderStyle::Solid, false, &commands);
		}
		catch (std::exception const& error)
		{
			commandsRecorded = commands.commands().size();
			return std::string(error.what());
		}
		commandsRecorded = commands.commands().size();
		return {};
	}

	// The plan refuses a style it has no arm for, and says which one. The
	// value is what the message carries because the arm is only ever
	// reached by a style the renderer does not name.
	void anUnplannedStyleRefusesToDraw(smoke::Context const&)
	{
		ImGuiGuard imgui;
		auto scene = buildDoorScene(core::Door::OpenStyle::OpenUp);
		scene.door->setOpenStyle(unplannedStyle());
		// The refusal has to be about a style the Door actually carries, not
		// about one the set never applied.
		require(static_cast<int>(scene.door->getOpenStyle()) == unplannedStyleValue(),
			"The Door did not take the unplanned opening style");

		auto drawList = testDrawList();
		std::size_t commandsRecorded{ 0 };
		auto const refusal = doorDrawRefusal(scene.door, drawList, commandsRecorded);

		require(!refusal.empty(),
			"an unplanned opening style was drawn instead of refused");
		require(refusal.find("has no leaf plan") != std::string::npos,
			"the refusal does not say the style has no leaf plan: " + refusal);
		require(refusal.find("#" + std::to_string(unplannedStyleValue()))
			!= std::string::npos,
			"the refusal does not name the style it refused: " + refusal);
	}

	// The refusal arrives before the Door has painted anything, so there is
	// no clip rect - and no fill under one - left in the frame from a reveal
	// nobody wrote.
	void anUnplannedStylePaintsNothingBeforeRefusing(smoke::Context const&)
	{
		ImGuiGuard imgui;
		auto scene = buildDoorScene(core::Door::OpenStyle::OpenApart);
		scene.door->setOpenStyle(unplannedStyle());
		auto drawList = testDrawList();

		std::size_t commandsRecorded{ 0 };
		auto const refusal = doorDrawRefusal(scene.door, drawList, commandsRecorded);

		require(!refusal.empty(),
			"an unplanned opening style was drawn instead of refused");
		require(commandsRecorded == 0,
			"the unplanned style recorded " + std::to_string(commandsRecorded)
			+ " draw commands before refusing; the refusal has to arrive before "
			"the Door records anything, so no clip built from an unwritten "
			"reveal can ever reach a frame");
	}

	// The half that does not depend on the refusal: a reveal that vacates
	// nothing draws no back Sector. A closed Door of every style vacates
	// nothing - its reveal is degenerate on the axis it travels - which is
	// the same empty rect an unwritten reveal defaults to.
	void aClosedDoorRevealsNothingOnEveryStyle(smoke::Context const&)
	{
		ImGuiGuard imgui;
		for (auto const style : { core::Door::OpenStyle::OpenUp,
				core::Door::OpenStyle::OpenLeft,
				core::Door::OpenStyle::OpenRight,
				core::Door::OpenStyle::OpenApart })
		{
			auto scene = buildDoorScene(style);
			auto drawList = testDrawList();
			renderPass(scene, LayerRenderStyle::Solid, drawList);

			require(door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
				"a closed Door drew no leaf, so the check could pass with the "
				"whole draw missing");
			require(!door_leaf::extentOf(drawList, kApertureFillColour).painted(),
				"a closed Door revealed the Sector behind it, so an empty "
				"reveal is not stopping the back-Sector draw");
		}
	}

	// The refusal must not reach a style that does have a plan: half open,
	// each of the four still paints its leaves and still reveals the Sector
	// behind through the strip they have vacated.
	void aPlannedStyleStillDrawsItsLeavesAndReveal(smoke::Context const&)
	{
		ImGuiGuard imgui;
		for (auto const style : { core::Door::OpenStyle::OpenUp,
				core::Door::OpenStyle::OpenLeft,
				core::Door::OpenStyle::OpenRight,
				core::Door::OpenStyle::OpenApart })
		{
			auto scene = buildDoorScene(style);
			driveTo(scene, 0.5f);
			require(scene.door->getOpenPercentage() > 0.0f,
				"The Door did not open, so the reveal it should paint was never asked for");
			auto drawList = testDrawList();
			renderPass(scene, LayerRenderStyle::Solid, drawList);

			require(door_leaf::extentOf(drawList, kDoorLeafColour).painted(),
				"a half-open planned Door drew no leaves");
			require(door_leaf::extentOf(drawList, kApertureFillColour).painted(),
				"a half-open planned Door revealed nothing, so its reveal was "
				"read as empty when it is not");
		}
	}
}


void render_smoke::registerUnplannedDoorStyle(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorLeafPlan/anUnplannedStyleRefusesToDraw",
		isolated<[](smoke::Context const& c) { anUnplannedStyleRefusesToDraw(c); }> });
	checks.push_back({ "doorLeafPlan/anUnplannedStylePaintsNothingBeforeRefusing",
		isolated<[](smoke::Context const& c) { anUnplannedStylePaintsNothingBeforeRefusing(c); }> });
	checks.push_back({ "doorLeafPlan/aClosedDoorRevealsNothingOnEveryStyle",
		isolated<[](smoke::Context const& c) { aClosedDoorRevealsNothingOnEveryStyle(c); }> });
	checks.push_back({ "doorLeafPlan/aPlannedStyleStillDrawsItsLeavesAndReveal",
		isolated<[](smoke::Context const& c) { aPlannedStyleStillDrawsItsLeavesAndReveal(c); }> });
}
