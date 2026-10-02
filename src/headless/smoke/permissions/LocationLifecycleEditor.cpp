#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"
#include "imgui_internal.h"
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	void require(bool value, std::string const& message)
	{ if (!value) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	void lifecycle(bool corridor)
	{
		permission_smoke::EditorState state;
		auto world = std::make_shared<core::World>("History", 24, 4);
		auto selected = corridor ? world->addCorridor(0, 0, 0, 4, 2)
			: world->addRoom("Room", 0, 0, 0, 4, 2);
		auto unrelated = world->addRoom("Unrelated", 0, 0, 18, 4, 2);
		world->finishBuild(); world->pauseSimulation();
		auto red = world->addAccessPermission("Red");
		auto blue = world->addAccessPermission("Blue");
		require(world->setLocationPermissionRequirement(selected, { red, blue })
			&& world->setLocationPermissionRequirement(unrelated, { red }), "Requirements refused");
		gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved(); world->markSaved();
		auto original = save(*world);
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto restored = std::make_shared<core::World>("undo", 1, 1);
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			require(restored->deserialize(*reader, work), "History snapshot did not load");
			world = std::move(restored); world->pauseSimulation(); return true;
		};
		auto undo = [&] { require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Undo failed"); };
		auto redo = [&] { require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save(*world)), restore), "Redo failed"); };
		auto edit = [&](core::World::LocationEditPlan const& plan)
		{
			require(plan.valid, plan.diagnostic);
			// The same capture/apply/commit seam used by UI.cpp's Location edits.
			auto snapshot = captureDocumentSnapshot(world);
			world->applyLocationEdit(plan); commitDocumentEdit(std::move(snapshot));
		};
		edit(world->planResizeLocation(selected, 6, 1, 4, 2));
		auto moved = save(*world);
		require(world->getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ red, blue }, "Move lost requirement");
		undo(); require(save(*world) == original, "Move undo did not restore complete document");
		redo(); require(save(*world) == moved, "Move redo did not restore complete document");
		edit(world->planResizeLocation(selected, 6, 1, 6, corridor ? 2 : 3));
		auto resized = save(*world);
		undo(); require(save(*world) == moved, "Resize undo lost document");
		redo(); require(save(*world) == resized, "Resize redo lost document");
		std::string diagnostic;
		require(commitAccessPermissionRename(world, red, "Renamed", diagnostic), diagnostic);
		auto renamed = save(*world);
		require(world->getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ red, blue }
			&& world->getLocationPermissionRequirement(unrelated) == std::vector<core::AccessPermissionId>{ red }, "Rename lost stable references");
		undo(); require(save(*world) == resized, "Rename undo failed");
		redo(); require(save(*world) == renamed, "Rename redo failed");

		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = ImVec2(1600, 900); io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGuiID rowDelete = 0;
		std::string text;
		// Redirect ImGui logging to memory only. EndPopup finishes logging before
		// the panel returns, so LogToBuffer cannot capture its impact text.
		io.ClipboardUserData = &text;
		io.SetClipboardTextFn = [](void* data, char const* value)
		{ *static_cast<std::string*>(data) = value; };
		auto frame = [&]
		{
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(10, 10)); ImGui::SetNextWindowSize(ImVec2(1500, 850));
			ImGui::Begin("Permissions lifecycle", nullptr, ImGuiWindowFlags_NoSavedSettings);
			ImGui::PushID("AccessPermissions");
			ImGui::PushID(static_cast<int>(red.value)); rowDelete = ImGui::GetID("Delete"); ImGui::PopID(); ImGui::PopID();
			text.clear(); ImGui::LogToClipboard(); renderPermissionsPanel(world);
			ImGui::LogFinish();
			ImGui::End(); ImGui::Render();
		};
		auto pointFor = [&](char const* windowName, ImGuiID id)
		{
			frame();
			auto* window = ImGui::FindWindowByName(windowName);
			require(window != nullptr, "Production permission window missing");
			auto pos = window->Pos; auto size = window->Size;
			// CPU-only mouse events, bounded search, no native windows or dialogs.
			for (float y = pos.y + 25; y < pos.y + size.y; y += 8)
				for (float x = pos.x + 5; x < pos.x + size.x; x += 16)
				{
					io.AddMousePosEvent(x, y); frame();
					if (ImGui::GetHoveredID() == id) return ImVec2(x, y);
				}
			throw std::runtime_error(std::string("Production permission button not reachable in ") + windowName);
		};
		auto click = [&](ImVec2 point)
		{
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
		};
		frame();
		require(text.find("0 / 0 / 0 / 2") != std::string::npos, "Usage table omitted both Location references");
		auto count = gWorldDocumentHistory.undoCount();
		click(pointFor("Permissions lifecycle", rowDelete)); frame();
		require(text.find("2 Location requirements") != std::string::npos, "Real deletion impact omitted Locations: " + text);
		auto* popup = ImGui::FindWindowByName("Delete Access permission?");
		require(popup != nullptr, "Deletion confirmation not rendered");
		click(pointFor("Delete Access permission?", popup->GetID("Cancel")));
		require(save(*world) == renamed && gWorldDocumentHistory.undoCount() == count, "Cancel changed document/history");
		click(pointFor("Permissions lifecycle", rowDelete)); frame();
		popup = ImGui::FindWindowByName("Delete Access permission?");
		click(pointFor("Delete Access permission?", popup->GetID("Delete")));
		auto deleted = save(*world);
		require(gWorldDocumentHistory.undoCount() == count + 1
			&& world->getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ blue }
			&& world->getLocationPermissionRequirement(unrelated).empty(), "Confirmed deletion did not clear every Location in one edit");
		undo(); require(save(*world) == renamed && world->getAccessPermissionUsage(red).locationRequirements == 2, "Deletion undo lost references");
		redo(); require(save(*world) == deleted, "Deletion redo lost cleanup");
		auto replacement = commitAccessPermissionAdd(world, "Replacement", diagnostic);
		require(replacement == red && world->getAccessPermissionUsage(replacement).locationRequirements == 0, "UI slot reuse aliased stale references");
		auto reused = save(*world);
		undo(); require(save(*world) == deleted, "Reuse undo changed cleanup");
		undo(); require(save(*world) == renamed, "Undo across reuse lost protection");
		redo(); redo(); require(save(*world) == reused, "Redo across reuse inherited stale protection");
		edit(world->planRemoveLocation(selected));
		auto removed = save(*world);
		require(world->getAccessPermissionUsage(blue).locationRequirements == 0, "Location deletion retained usage");
		undo(); require(save(*world) == reused && world->getLocationPermissionRequirement(selected) == std::vector<core::AccessPermissionId>{ blue }, "Location deletion undo lost requirement");
		redo(); require(save(*world) == removed, "Location deletion redo failed");
	}
}

void permission_smoke::registerLocationLifecycleEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "permissions/locationLifecycleRoom", [](smoke::Context const&) { lifecycle(false); } });
	checks.push_back({ "permissions/locationLifecycleCorridor", [](smoke::Context const&) { lifecycle(true); } });
}
