#include "Checks.h"
#include "State.h"
#include "BrokenPlatformLiftFixture.h"
#include "DoorPanel.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace
{
	void controls()
	{
		using smoke::require;
		broken_platform_lift::Scene scene;
		auto world = scene.world;
		world->pauseSimulation(); world->markSaved();
		gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved();
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = ImVec2(1600, 1000);
		std::string text; io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value) { *static_cast<std::string*>(data) = value; };
		auto lift = [&] { return std::static_pointer_cast<const core::LiftSectorObject>(world->getSector(scene.owner)->getObject(scene.made.lift.index))->getLift(); };
		auto save = [&]
		{ auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work; work.markSerializedUnmodified = false;
			world->serialize(*writer, work); writer->serialize(); return writer->getSerializedString(); };
		auto original = save();
		auto frame = [&]
		{
			ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(10, 10)); ImGui::SetNextWindowSize(ImVec2(1500, 950));
			ImGui::Begin("Lift Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
			auto depth = GImGui->DisabledStackSize; auto flags = GImGui->CurrentItemFlags; auto alpha = GImGui->Style.Alpha;
			text.clear(); ImGui::LogToClipboard(); renderLiftConditionPanel(world, lift()); ImGui::LogFinish();
			require(depth == GImGui->DisabledStackSize && flags == GImGui->CurrentItemFlags && alpha == GImGui->Style.Alpha, "Lift controls leaked disabled scope");
			ImGui::End(); ImGui::Render();
		};
		auto click = [&](char const* label)
		{
			frame(); auto* window = ImGui::FindWindowByName("Lift Selection"); auto control = window->GetID(label);
			bool found = false; ImVec2 point;
			for (float y = 35; y < 200 && !found; y += 8)
				for (float x = 15; x < 200 && !found; x += 16)
				{ io.AddMousePosEvent(x, y); frame(); frame(); if (ImGui::GetHoveredID() == control) { found = true; point = ImVec2(x, y); } }
			require(found, std::string("Missing control ") + label);
			io.AddMousePosEvent(point.x, point.y); frame(); io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
		};
		click("Initially Broken"); frame();
		require(lift()->isInitiallyBroken() && lift()->isBroken() && gWorldDocumentHistory.undoCount() == 1
			&& text.find("Broken: whole Platform lift frozen") != std::string::npos && text.find("platform y:") != std::string::npos, "Initial control/status/history failed");
		auto authored = save(); world->markSaved();
		click("Live Broken");
		require(!lift()->isBroken() && lift()->isInitiallyBroken() && save() == authored && !world->isModified()
			&& gWorldDocumentHistory.undoCount() == 1, "Live control changed document/history");
		world->resumeSimulation(); click("Live Broken");
		require(lift()->isBroken() && save() == authored && !world->isModified(), "Running live control failed");
		world->pauseSimulation();
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = std::make_shared<core::World>("History", 1, 1); core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			require(loaded->deserialize(*reader, work), "History load failed"); world = loaded; world->pauseSimulation(); return true;
		};
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save()), restore) && save() == original && !lift()->isInitiallyBroken(), "Authored undo failed");
		require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save()), restore) && save() == authored && lift()->isInitiallyBroken(), "Authored redo failed");
	}
}

void editor_smoke::registerBrokenPlatformLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "platformLifts/brokenControlsAndHistory", [](smoke::Context const&)
		{ State state; headless::ScopedImGuiContext context;
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); controls(); } });
}
