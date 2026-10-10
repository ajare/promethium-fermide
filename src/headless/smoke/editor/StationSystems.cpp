#include "Checks.h"
#include "State.h"
#include "StationSystemsWindow.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace
{
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
		world.reset(); frame(); require(text.find("No World is open.") != std::string::npos, "No active World status missing");
	}
}
void editor_smoke::registerStationSystems(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/windowWorkflow", workflow});
}
