#include "Checks.h"
#include "State.h"
// Door panel disabled-scope balance checks, for ticket #99.
//
// Selecting a Door leaked an ImGui disabled scope: the opening-style
// selector's BeginDisabled(!isSimulationPaused()) had no matching EndDisabled,
// so the Selection window ended one disabled-stack entry deep. Debug builds
// tripped ImGui's BeginDisabled/EndDisabled mismatch assertion; Release builds
// carried the global disabled flag and reduced alpha past the Door panel,
// dimming unrelated editor controls and leaking another entry every frame.
//
// The panel itself is compiled into this binary (src/DoorPanel.cpp), so these
// checks drive the real renderDoorPanel inside a CPU-side ImGui context -
// there is no mirrored copy to drift out of sync with the panel. Every Door
// ownership type (ordinary, Lift-owned, Shuttle-owned) is rendered while the
// simulation is paused and while it is running, and each render must leave:
//
//   * ImGui's disabled stack at the depth it started (checked explicitly, and
//     again by ImGui's own Debug end-window assertion when ImGui::End runs)
//   * the current item flags and global alpha untouched
//   * a control rendered after the panel free of inherited disabled state

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include "DoorPanel.h"
#include "DocumentEdit.h"
#include "core/BulkheadDoorSectorObject.h"

#include "core/YamlSerializer.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/World.h"
#include "core/Door.h"
#include "core/Button.h"
#include "core/DoorSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObject.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct ImGuiGuard
	{
		headless::ScopedImGuiContext context;
		ImGuiGuard()
		{
			auto& io = ImGui::GetIO();
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			// NewFrame() installs GetDefaultFont(), which reads Fonts[0]; a fresh
			// atlas has no fonts until one is added and built.
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
	};

	std::shared_ptr<const core::SectorObject> doorObjectAt(
		core::World::CreateObjectResult const& created)
	{
		require(created.index != ~0u && created.sector != nullptr,
			"The test World did not create its Door");
		auto const object = created.sector->getObject(created.index);
		require(object != nullptr && object->getObjectType() == core::SectorObjectType::Door,
			"The created object is not a Door");
		return object;
	}

	void requireOrdinary(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object)
	{
		uint32_t liftSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(!world.isLiftOwnedDoor(object, &liftSector, &stopIndex),
			"The ordinary test Door reads as Lift-owned");
		require(!world.isShuttleOwnedDoor(object, &liftSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The ordinary test Door reads as Shuttle-owned");
	}

	void requireLiftOwned(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object, uint32_t liftSector)
	{
		uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(world.isLiftOwnedDoor(object, &ownerSector, &stopIndex),
			"The Lift test Door does not read as Lift-owned");
		require(ownerSector == liftSector, "The Lift test Door names the wrong Lift sector");
		require(!world.isShuttleOwnedDoor(object, &ownerSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The Lift test Door also reads as Shuttle-owned");
	}

	void requireShuttleOwned(core::World const& world,
		std::shared_ptr<const core::SectorObject> const& object, uint32_t shuttleSector)
	{
		uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u }, doorIndex{ ~0u };
		require(world.isShuttleOwnedDoor(object, &ownerSector, &stopIndex,
				&carriageIndex, &doorIndex),
			"The Shuttle test Door does not read as Shuttle-owned");
		require(ownerSector == shuttleSector,
			"The Shuttle test Door names the wrong Shuttle sector");
		require(!world.isLiftOwnedDoor(object, &ownerSector, &stopIndex),
			"The Shuttle test Door also reads as Lift-owned");
	}

	// Renders the real Door panel for one selected Door and requires every
	// side effect of a disabled scope - stack depth, item flags, global alpha -
	// to be exactly as the panel found them. In Debug builds ImGui::End()
	// additionally asserts through its own end-window stack check, so an
	// imbalance fails here twice over.
	void requirePanelLeavesNoDisabledState(std::shared_ptr<core::World> const& world,
		std::shared_ptr<const core::SectorObject> object, char const* what)
	{
		ImGui::NewFrame();
		ImGui::Begin("Selection");

		auto const depthOnEntry = GImGui->DisabledStackSize;
		auto const flagsOnEntry = GImGui->CurrentItemFlags;
		auto const alphaOnEntry = GImGui->Style.Alpha;

		renderDoorPanel(world, object);

		require(GImGui->DisabledStackSize == depthOnEntry,
			std::string(what) + ": the panel left ImGui's disabled stack unbalanced");
		require(GImGui->CurrentItemFlags == flagsOnEntry,
			std::string(what) + ": the panel leaked item flags into the window");
		require(GImGui->Style.Alpha == alphaOnEntry,
			std::string(what) + ": the panel leaked a reduced global alpha");

		// A control rendered after the panel must inherit none of its disabled
		// state; this is exactly what the running-simulation leak dimmed.
		ImGui::Button("After the panel");
		require(!(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled),
			std::string(what)
				+ ": a control rendered after the panel inherited a disabled scope");
		require(GImGui->Style.Alpha == alphaOnEntry,
			std::string(what) + ": a control after the panel rendered dimmed");

		ImGui::End();
		ImGui::Render();
	}

	void checkOrdinaryDoor()
	{
		auto world = std::make_shared<core::World>("Ordinary door panel", 12, 3);
		world->addRoom("Fore", 0, 0, 0, 11, 2);
		world->addRoom("Aft", 1, 0, 0, 11, 2);
		auto const created = world->addSectorDoor(0, 0, 3, core::World::CreateDoorOptions{});
		world->finishBuild();
		auto const object = doorObjectAt(created.door);
		requireOrdinary(*world, object);

		requirePanelLeavesNoDisabledState(world, object,
			"An ordinary Door selection, simulation running");
		world->pauseSimulation();
		requirePanelLeavesNoDisabledState(world, object,
			"An ordinary Door selection, simulation paused");
	}

	void checkBrokenControlsAndHistory(bool bulkhead = false)
	{
		auto world = std::make_shared<core::World>("Broken controls", 12, 3);
		std::shared_ptr<const core::SectorObject> object;
		std::shared_ptr<core::Door> door;
		if (bulkhead)
		{
			world->addRoom("Left", 0, 0, 0, 5, 1);
			world->addRoom("Right", 0, 0, 5, 6, 1);
			auto made = world->addSectorBulkheadDoor(0, 0, 5, CORE_SIDE_LEFT);
			object = made.door.sector->getObject(made.door.index);
			door = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(object)->getDoor();
		}
		else
		{
			world->addRoom("Front", 0, 0, 0, 11, 1);
			world->addRoom("Back", 1, 0, 0, 11, 1);
			auto made = world->addSectorDoor(0, 0, 3);
			object = doorObjectAt(made.door);
			door = std::static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
		}
		world->finishBuild(); world->pauseSimulation();
		gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved(); world->markSaved();
		auto save = [&]
		{
			core::SerializationWorkData work; work.markSerializedUnmodified = false;
			auto writer = core::YamlSerializer::toString(); world->serialize(*writer, work); writer->serialize();
			return writer->getSerializedString();
		};
		auto initial = save();
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = ImVec2(1600, 1000);
		std::string text;
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		auto frame = [&]
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(10, 10)); ImGui::SetNextWindowSize(ImVec2(1500, 950));
			ImGui::Begin("Broken Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
			text.clear(); ImGui::LogToClipboard();
			if (bulkhead) renderDoorConditionPanel(world, door);
			else renderDoorPanel(world, object);
			ImGui::LogFinish();
			ImGui::End(); ImGui::Render();
		};
		auto pointFor = [&](char const* label)
		{
			frame(); auto* window = ImGui::FindWindowByName("Broken Selection");
			auto id = window->GetID(label);
			for (float y = window->Pos.y + 25; y < window->Pos.y + 400; y += 8)
				for (float x = window->Pos.x + 5; x < window->Pos.x + 180; x += 16)
				{
					io.AddMousePosEvent(x, y); frame(); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2(x, y);
				}
			throw std::runtime_error(std::string("Door control not reachable: ") + label);
		};
		auto click = [&](char const* label)
		{
			auto point = pointFor(label); io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
		};
		click("Initially Broken"); frame();
		require(door->isInitiallyBroken() && door->isBroken() && gWorldDocumentHistory.undoCount() == 1,
			"Authored Door control did not commit one history entry");
		require(text.find("Broken (position frozen)") != std::string::npos
			&& text.find("0.00%") != std::string::npos, "Selection omitted Broken status or physical percentage");
		auto authored = save(); world->markSaved();
		click("Live Broken");
		require(!door->isBroken() && door->isInitiallyBroken() && save() == authored
			&& !world->isModified() && gWorldDocumentHistory.undoCount() == 1, "Live restore changed document/history");
		require(world->resumeSimulation(), "Simulation did not resume");
		click("Live Broken");
		require(door->isBroken() && save() == authored && !world->isModified(), "Running live break control failed: broken=" + std::to_string(door->isBroken())
			+ " saved=" + std::to_string(save() == authored) + " dirty=" + std::to_string(world->isModified()));
		world->pauseSimulation();
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = std::make_shared<core::World>("History", 1, 1);
			core::SerializationWorkData work; auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			require(loaded->deserialize(*reader, work), "Door history did not load"); world = loaded; world->pauseSimulation();
			return true;
		};
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save()), restore) && save() == initial,
			"Door initial Broken undo failed");
		require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save()), restore) && save() == authored,
			"Door initial Broken redo failed");
	}

	void checkWallSafeDocumentEdits()
	{
		auto world = std::make_shared<core::World>("Wall-safe history", 16, 3);
		auto left = world->addRoom("Left", 0, 1, 0, 4, 1);
		auto front = world->addRoom("Front", 0, 1, 4, 2, 1);
		world->addRoom("Back", 1, 1, 0, 16, 1);
		auto created = world->addSectorDoor(0, 1, 4, core::World::RemoteControlledDoor1Options);
		world->addSectorLightSwitch(front, 1);
		world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		history.markSaved();
		auto placement = [&]
		{
			std::vector<core::Vector2> result;
			for (uint32_t i = 0; i < world->getSector(front)->getNumObjects(); ++i)
			{
				auto object = world->getSector(front)->getObject(i);
				auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
				if (!button) continue;
				result.push_back(button->getPosition() + button->getSize() * 0.5f);
				result.push_back(world->lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition());
			}
			std::sort(result.begin(), result.end(), [](auto const& a, auto const& b)
				{ return a.x != b.x ? a.x < b.x : a.y < b.y; });
			return result;
		};
		auto initial = captureDocumentSnapshot(world, history);
		auto positions = placement();
		require(positions.size() == 4 && positions[0].x == 5.0f && positions[2].x == 5.5f,
			"Document fixture placement is not explicit right/centred authoring");
		auto invalid = world->planResizeSectorDoor(front, created.door.index, 4, 1, 2, 1);
		require(!invalid.valid && captureDocumentSnapshot(world, history)->yaml == initial->yaml
			&& history.undoCount() == 0, "Invalid wall-safe resize dirtied document/history");
		world->removeLocationWall(left, 0, CORE_SIDE_RIGHT);
		auto valid = world->planResizeSectorDoor(front, created.door.index, 4, 1, 2, 1);
		require(valid.valid, "Removed wall did not permit authored boundary host: " + valid.diagnostic);
		world->applyObjectMove(valid);
		commitDocumentEdit(initial, history);
		auto edited = captureDocumentSnapshot(world, history);
		auto changed = placement();
		require(changed[0].x == 4.0f && changed[2].x == 5.5f,
			"Resize failed to change Door host independently of light switch");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {});
			world->pauseSimulation();
			return bool(world);
		};
		require(history.undo(*edited, restore) && placement() == positions,
			"Production document undo lost wall-safe placement/approaches");
		require(history.redo(*captureDocumentSnapshot(world, history), restore) && placement() == changed,
			"Production document redo lost wall-safe placement/approaches");
	}

	void checkCanonicalDocumentHistory()
	{
		auto world = std::make_shared<core::World>("Canonical history", 12, 2);
		world->addLayer();
		world->addRoom("Front", 0, 0, 0, 12, 1);
		auto middle = world->addRoom("Middle", 1, 0, 0, 6, 1);
		world->addRoom("Back", 2, 0, 0, 12, 1);
		auto wide = core::World::RemoteControlledDoor1Options; wide.width = 2;
		world->addSectorDoor(0, 0, 2, wide);
		world->addSectorDoor(1, 0, 3, core::World::RemoteControlledDoor1Options);
		world->addSectorDoor(0, 0, 4, core::World::RemoteControlledDoor1Options);
		world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		auto positions = [&]
		{
			std::vector<float> result;
			for (uint32_t i = 0; i < world->getSector(middle)->getNumObjects(); ++i)
			{
				auto object = world->getSector(middle)->getObject(i);
				auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
				if (button) result.push_back(world->lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition().x);
			}
			std::sort(result.begin(), result.end()); return result;
		};
		auto initial = captureDocumentSnapshot(world, history);
		// The same production option readback and creation seams used by Door
		// clipboard placement; no derived Button position is copied.
		core::World::CreateDoorOptions copied;
		require(world->getSectorDoorOptions(1, 0, 3, 1, copied), "Door clipboard readback failed");
		world->addSectorDoor(1, 0, 5, copied); world->finishBuild();
		commitDocumentEdit(initial, history);
		require(positions() == std::vector<float>{2, 3, 4, 5}, "Clipboard-style placement missed simultaneous reflow");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {});
			if (world) world->pauseSimulation();
			return bool(world);
		};
		require(history.undo(*captureDocumentSnapshot(world, history), restore)
			&& positions() == std::vector<float>{3, 4, 5}, "Undo did not restore canonical preferences");
		require(history.redo(*captureDocumentSnapshot(world, history), restore)
			&& positions() == std::vector<float>{2, 3, 4, 5}, "Redo depended on previous Button placement");
	}

	void checkStackDocumentHistory()
	{
		auto world = std::make_shared<core::World>("Stack document", 6, 1); world->addLayer();
		world->addRoom("Front", 0, 0, 0, 6, 1);
		auto middle = world->addRoom("Middle", 1, 0, 1, 2, 1);
		world->addRoom("Back", 2, 0, 0, 6, 1);
		world->addSectorDoor(0, 0, 2, core::World::RemoteControlledDoor1Options);
		world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		auto positions = [&]
		{
			std::vector<core::Vector2> result;
			std::shared_ptr<const core::Vertex> approach;
			for (uint32_t i = 0; i < world->getSector(middle)->getNumObjects(); ++i)
			{
				auto object = world->getSector(middle)->getObject(i);
				auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
				if (!button) continue;
				auto centre = button->getPosition() + button->getSize() * 0.5f;
				std::shared_ptr<const core::SectorObject> selected;
				require(world->getObjectAtPosition(1, centre.x, centre.y, &selected) == button && selected == object,
					"Editor selection chose another stack member");
				auto vertex = world->getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object));
				require(vertex && vertex->getPosition().y == 0, "History restored an elevated approach");
				if (approach && result.front().x == centre.x) require(approach == vertex, "History duplicated shared approach");
				approach = vertex;
				result.push_back(centre);
			}
			std::sort(result.begin(), result.end(), [](auto a, auto b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
			return result;
		};
		auto initial = captureDocumentSnapshot(world, history);
		core::World::CreateDoorOptions copied;
		require(world->getSectorDoorOptions(0, 0, 2, 1, copied), "Stack clipboard option readback failed");
		// Clipboard copies authored controls, not Button geometry or approaches.
		world->addSectorDoor(1, 0, 2, copied); world->finishBuild();
		commitDocumentEdit(initial, history);
		auto stacked = positions();
		require(stacked.size() == 2 && stacked[0].x == stacked[1].x && stacked[0].y < stacked[1].y, "Clipboard did not reconstruct stack");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {});
			if (world) world->pauseSimulation();
			return bool(world);
		};
		require(history.undo(*captureDocumentSnapshot(world, history), restore) && positions().size() == 1, "Stack creation undo failed");
		require(history.redo(*captureDocumentSnapshot(world, history), restore) && positions() == stacked, "Stack creation redo failed");
		auto beforeResize = captureDocumentSnapshot(world, history);
		auto plan = world->planResizeLocation(middle, 1, 0, 3, 1);
		require(plan.valid, "Available alternate side resize refused: " + plan.diagnostic);
		middle = world->applyLocationEdit(plan);
		commitDocumentEdit(beforeResize, history);
		auto separated = positions();
		require(separated.size() == 2 && separated[0].x == 2 && separated[1].x == 3
			&& separated[0].y == separated[1].y, "Newly available alternate side retained stack");
		require(history.undo(*captureDocumentSnapshot(world, history), restore) && positions() == stacked, "Resize undo did not reconstruct stack");
		require(history.redo(*captureDocumentSnapshot(world, history), restore) && positions() == separated, "Resize redo did not separate stack");
	}

	void checkExistingButtonsCanBeRemoved()
	{
		auto world = std::make_shared<core::World>("Door button checkbox", 12, 3);
		world->addRoom("Fore", 0, 0, 0, 11, 2);
		world->addRoom("Aft", 1, 0, 0, 11, 2);
		core::World::CreateDoorOptions options;
		options.controls[0] = true;
		options.controls[1] = true;
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		auto const created = world->addSectorDoor(0, 0, 7, options);
		world->finishBuild();
		world->pauseSimulation();
		auto const object = doorObjectAt(created.door);

		ImGui::NewFrame();
		ImGui::Begin("Selection");
		renderDoorPanel(world, object);
		require(GImGui->LastItemData.ID == ImGui::GetID("Buttons"),
			"The Door panel's final control is not the Buttons checkbox");
		require(!(GImGui->LastItemData.InFlags & ImGuiItemFlags_Disabled),
			"A paused ordinary Door with Buttons cannot have them unchecked");
		ImGui::End();
		ImGui::Render();
		checkWallSafeDocumentEdits();
		checkCanonicalDocumentHistory();
		checkStackDocumentHistory();
	}

	void checkLiftOwnedDoor()
	{
		auto world = std::make_shared<core::World>("Lift door panel", 16, 3);
		auto const hall = world->addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 16; ++x)
				world->addSectorWalkway(hall, level, x);
		core::World::CreateLiftOptions options;
		options.cellsWide = 1;
		options.levelsHigh = 3;
		options.stopOffsets = { 0, 1, 2 };
		auto const lift = world->addLift(1, 0, 8, options);
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		world->finishBuild();
		auto const object = doorObjectAt(lift.doors[0].door);
		requireLiftOwned(*world, object, lift.lift.sector->getIndex());

		requirePanelLeavesNoDisabledState(world, object,
			"A Lift-owned Door selection, simulation running");
		world->pauseSimulation();
		requirePanelLeavesNoDisabledState(world, object,
			"A Lift-owned Door selection, simulation paused");
	}

	void checkShuttleOwnedDoor()
	{
		auto world = std::make_shared<core::World>("Shuttle door panel", 32, 3);
		world->addCorridor(0, 0, 31);
		world->addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions options{ 2, 3, { 0, 18 }, 0 };
		options.capacity = 2;
		options.doorMask = 0b101;
		auto const shuttle = world->addShuttle(1, 0, 0, 27, options);
		world->finishBuild();
		// The grid is stop x carriage x doorMask cell, with empty cells where a
		// partial landing is unsupported; take the first Door it actually made.
		auto const made = std::find_if(shuttle.doors.begin(), shuttle.doors.end(),
			[](core::World::CreateDoorResult const& entry)
			{ return entry.door.index != ~0u && entry.door.sector != nullptr; });
		require(made != shuttle.doors.end(), "The Shuttle generated no Doors at all");
		auto const object = doorObjectAt(made->door);
		requireShuttleOwned(*world, object, shuttle.shuttle.sector->getIndex());

		requirePanelLeavesNoDisabledState(world, object,
			"A Shuttle-owned Door selection, simulation running");
		world->pauseSimulation();
		requirePanelLeavesNoDisabledState(world, object,
			"A Shuttle-owned Door selection, simulation paused");
	}
}

void editor_smoke::registerDoorPanel(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "doorpanel/checkBulkheadBrokenControlsAndHistory", [](smoke::Context const&) { State state; ImGuiGuard guard; checkBrokenControlsAndHistory(true); } });
	checks.push_back({ "doorpanel/checkBrokenControlsAndHistory", [](smoke::Context const&) { State state; ImGuiGuard guard; checkBrokenControlsAndHistory(); } });
	checks.push_back({ "doorpanel/checkOrdinaryDoor", [](smoke::Context const&) { State state; ImGuiGuard guard; checkOrdinaryDoor(); } });
	checks.push_back({ "doorpanel/checkExistingButtonsCanBeRemoved", [](smoke::Context const&) { State state; ImGuiGuard guard; checkExistingButtonsCanBeRemoved(); } });
	checks.push_back({ "doorpanel/checkLiftOwnedDoor", [](smoke::Context const&) { State state; ImGuiGuard guard; checkLiftOwnedDoor(); } });
	checks.push_back({ "doorpanel/checkShuttleOwnedDoor", [](smoke::Context const&) { State state; ImGuiGuard guard; checkShuttleOwnedDoor(); } });
}
