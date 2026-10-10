#include "Checks.h"
#include "State.h"
#include "StationSystemsWindow.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace
{
	void storeWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text; io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Store editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems({true, 1.0f, {
			{"o2", "kg", 1000.0, 800.0, 100.0}, {"water", "L", 500.0, 0.0, 250.0}}},
			false, &diagnostic), diagnostic.c_str());
		auto frame = [&]
		{
			ImGui::NewFrame(); text.clear(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 850});
			window.render(world, history); ImGui::LogFinish(); ImGui::Render();
		};
		auto clickPoint = [&](ImVec2 point)
		{
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
		};
		auto point = [&](char const* label)
		{
			auto id = ImGui::FindWindowByName("Station Systems")->GetID(label);
			for (float y = 45; y < 850; y += 5)
				for (float x = 18; x < 940; x += 15)
				{
					io.AddMousePosEvent(x, y); frame(); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2(x, y);
				}
			throw std::runtime_error(std::string("Missing Station control: ") + label);
		};
		auto typeValue = [&](char const* label, char const* value)
		{
			clickPoint(point(label));
			io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
			io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
			io.AddInputCharactersUTF8(value); frame();
			io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
		};
		window.visible = true; frame(); frame();
		// The separate window shows units, usable versus held inventory,
		// capacity and initial versus current state.
		require(text.find("o2 (kg): usable 800.000, held 100.000 of capacity 1000.000 | initial usable 800.000, initial held 100.000")
			!= std::string::npos, "Store live display missing identity, units, usable/held or capacity");
		require(text.find("water (L): usable 0.000, held 250.000 of capacity 500.000 | initial usable 0.000, initial held 250.000")
			!= std::string::npos, "Empty-usable store display wrong");
		require(text.find("2 resource store(s)") != std::string::npos, "Authored store count missing");
		// Author one more store through the real controls, then edit it.
		clickPoint(point("Add store"));
		require(text.find("Draft differs from the authored start conditions") != std::string::npos,
			"Added draft store not reported as a change");
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().stores.size() == 3 && history.undoCount() == 1,
			"Added store did not apply or commit history");
		typeValue("Store 2 starting usable", "30");
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().stores[2].startingQualified == 30.0
			&& world->getStationSystemsSnapshot().stores[2].currentQualified == 30.0
			&& history.undoCount() == 2, "Edited starting usable did not reach the live store");
		// Invalid values are refused before running, with a visible diagnostic
		// and no mutation of the authored document or history.
		typeValue("Store 2 capacity", "0");
		clickPoint(point("Apply settings")); frame();
		require(text.find("capacity must be finite and positive") != std::string::npos,
			"Invalid store capacity diagnostic not shown");
		require(world->getStationSystemsSettings().stores[2].capacity == 100.0
			&& history.undoCount() == 2, "Invalid store edit mutated state");
		typeValue("Store 2 capacity", "75");
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().stores[2].capacity == 75.0
			&& history.undoCount() == 3, "Recovery capacity edit did not apply");
		clickPoint(point("Remove store 2"));
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().stores.size() == 2
			&& world->getStationSystemsSnapshot().stores.size() == 2
			&& history.undoCount() == 4, "Removed store did not apply");
		// Running keeps the live display and never edits the authored document.
		world->markSaved();
		world->resumeSimulation(); require(world->advanceTicks(60), "Store editor run failed");
		frame();
		require(text.find("o2 (kg): usable 800.000") != std::string::npos
			&& !world->isModified(), "Run changed the display source or dirtied the document");
		// Undo restores the authored store set without resurrecting live state.
		world->pauseSimulation();
		auto restore = [&](DocumentSnapshot const& snapshot)
		{ world = deserializeDocumentSnapshot(snapshot, world, {}); world->pauseSimulation(); return true; };
		require(history.undo(captureDocumentSnapshot(world, history), restore)
			&& world->getStationSystemsSettings().stores.size() == 3,
			"Undo did not restore the authored store set");
		require(world->getStationSystemsSnapshot().worldTick == 0
			&& world->getStationSystemsSnapshot().stores.size() == 3,
			"History undo resurrected live station state instead of reconstructing stores");
	}
	void workflow(smoke::Context const&)
	{
		using smoke::require;
		editor_smoke::State state;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text; io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Station editor", 2, 2); world->pauseSimulation(); world->finishBuild();
		auto frame = [&]
		{
			ImGui::NewFrame(); text.clear(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 850});
			window.render(world, history); ImGui::LogFinish(); ImGui::Render();
		};
		window.visible = true; frame(); frame();
		require(ImGui::FindWindowByName("Station Systems") != nullptr && text.find("Active World: Station editor") != std::string::npos,
			"Separate top-level station window missing active World");
		require(text.find("Draft matches the authored start conditions") != std::string::npos,
			"Unchanged draft state not reported");
		auto clickPoint = [&](ImVec2 point)
		{
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
		};
		auto point = [&](char const* label)
		{
			auto id = ImGui::FindWindowByName("Station Systems")->GetID(label);
			for (float y = 45; y < 650; y += 7)
				for (float x = 18; x < 600; x += 20)
				{
					io.AddMousePosEvent(x, y); frame(); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2(x, y);
				}
			throw std::runtime_error(std::string("Missing Station control: ") + label);
		};
		clickPoint(point("Enable Station systems"));
		require(text.find("Draft differs from the authored start conditions") != std::string::npos,
			"Draft change not reported");
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().enabled && history.undoCount() == 1, "Enable did not commit document history");
		require(text.find("Document: unsaved changes") != std::string::npos, "Dirty state not shown after applying settings");
		world->markSaved(); frame();
		require(text.find("Document: saved") != std::string::npos, "Saved state not shown after markSaved");
		world->resumeSimulation(); require(world->advanceTicks(60), "Editor station did not tick"); frame();
		require(text.find("Live station state: Enabled") != std::string::npos && text.find("60 completed ticks") != std::string::npos,
			"Station live results not observable");
		auto before = world->getStationSystemsSnapshot();
		// Close via the real top-level ImGui close control, not disablement.
		auto* imguiWindow = ImGui::FindWindowByName("Station Systems");
		clickPoint({imguiWindow->Pos.x + imguiWindow->Size.x - 15, imguiWindow->Pos.y + 10});
		require(!window.visible && world->getStationSystemsSettings().enabled && world->getStationSystemsSnapshot() == before,
			"Closing window changed station simulation");
		require(world->advanceTick(), "Hidden station stopped ticking");
		window.visible = true; world->pauseSimulation(); frame();
		clickPoint(point("Enable Station systems")); clickPoint(point("Cancel edits"));
		require(world->getStationSystemsSettings().enabled && history.undoCount() == 1, "Cancel changed live/authored state");
		clickPoint(point("Enable Station systems")); clickPoint(point("Apply settings")); frame();
		require(world->getStationSystemsSettings().enabled && world->getSimulationTick() == 61
			&& text.find("Confirm?") != std::string::npos, "Apply bypassed reset confirmation");
		clickPoint(point("Cancel reset"));
		require(world->getSimulationTick() == 61 && history.undoCount() == 1, "Reset cancellation mutated state");
		clickPoint(point("Apply settings")); clickPoint(point("Confirm World reset"));
		require(!world->getStationSystemsSettings().enabled && world->getSimulationTick() == 0 && history.undoCount() == 2,
			"Confirmed setting did not reset World and commit history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{ world = deserializeDocumentSnapshot(snapshot, world, {}); world->pauseSimulation(); return true; };
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->getStationSystemsSettings().enabled,
			"Station undo lost configuration");
		require(world->getStationSystemsSnapshot().worldTick == 0, "History resurrected live station Lua");
		require(history.redo(captureDocumentSnapshot(world, history), restore) && !world->getStationSystemsSettings().enabled, "Station redo failed");
		world = std::make_shared<core::World>("Second World", 2, 2); world->pauseSimulation(); frame();
		require(text.find("Active World: Second World") != std::string::npos && text.find("Live station state: Disabled") != std::string::npos,
			"Window leaked state across Worlds");
		struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } } sourceScope;
		core::setStationSystemsSourceLoader([] {
			return "return function(name) return {advance=function() return 0/0 end} end";
		});
		world->finishBuild();
		require(world->configureStationSystems({true, 1}), "Could not configure failure fixture");
		world->resumeSimulation(); require(!world->advanceTick() && !world->getStationSystemsSnapshot().healthy,
			"Failure fixture did not reach station failure"); frame();
		require(text.find("Live station state: Failed") != std::string::npos
			&& text.find("World status: Paused") != std::string::npos
			&& text.find("Invalid output/accounting") != std::string::npos
			&& text.find("non-finite") != std::string::npos
			&& text.find("Reset the World") != std::string::npos,
			"Station window omitted failure category, pause, diagnostic or recovery");
		core::setStationSystemsSourceLoader({}); world->resetSimulation(); frame();
		require(text.find("Live station state: Enabled") != std::string::npos
			&& text.find("Software failure at tick") == std::string::npos, "Window retained reset failure");
		world.reset(); frame(); require(text.find("No World is open.") != std::string::npos, "No active World status missing");
	}
	void transferWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		std::string text; io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		DocumentHistory history;
		StationSystemsWindow window;
		// The transfer draft extends past the default display height; ImGui
		// clamps the mouse to the display, so give the headless test enough.
		io.DisplaySize = {1200, 1700};
		auto world = std::make_shared<core::World>("Transfer editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems({true, 1.0f,
			{{"src", "kg", 100.0, 100.0, 0.0, 20.0}, {"dst", "kg", 40.0, 0.0, 0.0},
			{"aux", "kg", 50.0, 0.0, 0.0}},
			{{"pump", "src", "dst", "kg", 60.0, 0}}}, false, &diagnostic), diagnostic.c_str());
		auto frame = [&]
		{
			ImGui::NewFrame(); text.clear(); ImGui::LogToClipboard();
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1600});
			window.render(world, history); ImGui::LogFinish(); ImGui::Render();
		};
		auto clickPoint = [&](ImVec2 point)
		{
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
		};
		auto point = [&](char const* label)
		{
			auto id = ImGui::FindWindowByName("Station Systems")->GetID(label);
			for (float y = 45; y < 1590; y += 5)
				for (float x = 18; x < 940; x += 15)
				{
					io.AddMousePosEvent(x, y); frame(); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2(x, y);
				}
			throw std::runtime_error(std::string("Missing Station control: ") + label);
		};
		auto typeValue = [&](char const* label, char const* value)
		{
			clickPoint(point(label));
			io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
			io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
			io.AddInputCharactersUTF8(value); frame();
			io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
		};
		window.visible = true; frame(); frame();
		require(text.find("src (kg): usable 100.000, held 0.000 of capacity 100.000 | initial usable 100.000, initial held 0.000 | protected 20.000")
			!= std::string::npos, "Protected reserve not shown in the live store table");
		require(text.find("pump: src -> dst (kg) idle | requested 0.000, completed 0.000, unmet 0.000 (rate 60.000/s, priority 0)")
			!= std::string::npos, "Authored transfer not shown in the live transfer table");
		// Run until the destination fills: the window shows the rejection and
		// the conserved boundary balance.
		world->markSaved();
		world->resumeSimulation(); require(world->advanceTicks(41), "Transfer editor run failed");
		world->pauseSimulation(); frame();
		require(text.find("pump: src -> dst (kg) rejected | requested 1.000, completed 0.000, unmet 1.000 (rate 60.000/s, priority 0)")
			!= std::string::npos, "Blocked transfer rejection not observable in the window");
		require(text.find("Balance kg: starting 100.000, current 100.000, residual 0.000000000")
			!= std::string::npos, "Boundary balance not observable in the window");
		require(!world->isModified(), "Transfer ticking dirtied the authored document");
		// Draft a second transfer through the real controls; an unknown
		// source is refused with a visible diagnostic and no mutation.
		clickPoint(point("Add transfer"));
		typeValue("Transfer 1 source", "nope");
		clickPoint(point("Apply settings"));
		clickPoint(point("Confirm World reset")); frame();
		require(text.find("references unknown source store 'nope'") != std::string::npos,
			"Invalid transfer source diagnostic not shown");
		require(world->getStationSystemsSettings().transfers.size() == 1 && history.undoCount() == 0
			&& world->getSimulationTick() == 41,
			"Invalid transfer draft mutated the authored settings");
		// Repair the drafted source (the invalid 'nope') to the aux store and
		// confirm again: the valid transfer applies with the required reset.
		typeValue("Transfer 1 source", "aux");
		clickPoint(point("Confirm World reset"));
		require(world->getStationSystemsSettings().transfers.size() == 2 && history.undoCount() == 1
			&& world->getSimulationTick() == 0,
			"Repaired transfer draft did not apply through the reset-confirmed flow");
		frame();
		require(text.find("2 transfer(s)") != std::string::npos
			&& text.find("aux -> dst (kg) idle") != std::string::npos,
			"Applied transfer not reflected in the window");
		// Remove the drafted transfer again (tick 0: no reset confirmation needed).
		clickPoint(point("Remove transfer 1"));
		clickPoint(point("Apply settings"));
		require(world->getStationSystemsSettings().transfers.size() == 1 && history.undoCount() == 2,
			"Transfer removal did not apply");
	}
}
void editor_smoke::registerStationSystems(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/windowWorkflow", workflow});
	checks.push_back({"station/storeWorkflow", storeWorkflow});
	checks.push_back({"station/transferWorkflow", transferWorkflow});
}
