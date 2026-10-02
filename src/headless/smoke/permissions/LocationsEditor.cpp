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

	void selection(bool corridor)
	{
		permission_smoke::EditorState state;
		auto world = std::make_shared<core::World>("Selection", 12, 1);
		auto sector = corridor ? world->addCorridor(0, 0, 0, 4, 1)
			: world->addRoom("Selected Room", 0, 0, 0, 4, 1);
		auto facade = world->addFacade("Facade", 0, 0, 4, 4, 1);
		auto background = world->addBackground(0, 0, 8, 4, 1);
		world->finishBuild(); world->pauseSimulation();
		auto red = world->addAccessPermission("Red##literal");
		auto blue = world->addAccessPermission("Blue");
		gWorldDocumentHistory.clear(); gWorldDocumentHistory.markSaved(); world->markSaved();
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = ImVec2(1000, 700);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGuiID redCheckbox = 0;
		ImGuiID blueCheckbox = 0;
		ImGuiID clearButton = 0;
		std::string text;
		auto frame = [&](uint32_t selectedSector = ~0u)
		{
			if (selectedSector == ~0u) selectedSector = sector;
			ImGui::NewFrame();
			ImGui::SetNextWindowPos(ImVec2(10, 10)); ImGui::SetNextWindowSize(ImVec2(900, 650));
			ImGui::Begin("Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
			auto* window = ImGui::GetCurrentWindow();
			window->StateStorage.SetInt(window->GetID("Required Access permissions"), 1);
			ImGui::PushID("Required Access permissions");
			ImGui::PushID("location-permissions");
			ImGui::PushID(static_cast<int>(red.value)); redCheckbox = ImGui::GetID(world->getAccessPermissionName(red).c_str()); ImGui::PopID();
			ImGui::PushID(static_cast<int>(blue.value)); blueCheckbox = ImGui::GetID("Blue"); ImGui::PopID();
			clearButton = ImGui::GetID("Clear");
			ImGui::PopID(); ImGui::PopID();
			auto const disabled = GImGui->DisabledStackSize;
			auto const flags = GImGui->CurrentItemFlags;
			auto const alpha = ImGui::GetStyle().Alpha;
			ImGui::LogToBuffer();
			// This is the production Selection seam called by UI.cpp for Rooms
			// and Corridors, not a mirrored checklist or headless replacement.
			renderLocationPermissionRequirements(world, selectedSector);
			text = GImGui->LogBuffer.c_str(); ImGui::LogFinish();
			require(GImGui->DisabledStackSize == disabled && GImGui->CurrentItemFlags == flags
				&& ImGui::GetStyle().Alpha == alpha, "Location Selection leaked ImGui disabled state");
			ImGui::End(); ImGui::Render();
		};
		frame();
		require(text.find("Location permissions") != std::string::npos
			&& text.find("Required (all): None") != std::string::npos
			&& text.find("Permission set grants") != std::string::npos
			&& text.find("Control operation requirements remain independent") != std::string::npos,
			"Selection omitted requirement summary or all-of semantics");
		frame(facade); require(text.find("Location permissions") == std::string::npos, "Facade exposed requirement");
		frame(background); require(text.find("Location permissions") == std::string::npos, "Background exposed requirement");
		frame(999); require(text.find("Location permissions") == std::string::npos, "Missing Location exposed requirement");
		require(gWorldDocumentHistory.undoCount() == 0 && !world->isModified(), "Inspection/cancellation created an edit");
		// Locate actual widgets by hovered ID, then drive press/release events.
		// This avoids hard-coded checkbox layout and never uses desktop input.
		auto pointFor = [&](ImGuiID id)
		{
			for (float y = 35; y < 500; y += 2)
			{
				io.AddMousePosEvent(65, y); frame();
				if (ImGui::GetHoveredID() == id) return ImVec2(65, y);
			}
			throw std::runtime_error("Selection widget was not reachable by real ImGui input");
		};
		auto click = [&](ImVec2 point)
		{
			io.AddMousePosEvent(point.x, point.y); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
		};
		click(pointFor(redCheckbox));
		require(world->getLocationPermissionRequirement(sector) == std::vector<core::AccessPermissionId>{ red }
			&& gWorldDocumentHistory.undoCount() == 1, "Real Selection checkbox did not commit one edit");
		click(pointFor(blueCheckbox));
		auto expected = std::vector<core::AccessPermissionId>{ red, blue };
		require(world->getLocationPermissionRequirement(sector) == expected
			&& gWorldDocumentHistory.undoCount() == 2, "Second checkbox did not commit one edit");
		frame();
		require(text.find("Required (all): Red##literal, Blue") != std::string::npos,
			"Selection summary did not show literal permission names");
		std::string diagnostic;
		auto before = save(*world);
		require(!commitLocationPermissionRequirement(world, sector, red, true, diagnostic)
			&& !commitLocationPermissionRequirement(world, sector, core::AccessPermissionId{ 256 }, true, diagnostic)
			&& !commitLocationPermissionRequirement(world, facade, red, true, diagnostic)
			&& !commitLocationPermissionRequirement(world, 999, red, true, diagnostic)
			&& !commitLocationPermissionRequirement({}, sector, red, true, diagnostic)
			&& save(*world) == before && gWorldDocumentHistory.undoCount() == 2, "No-op/refusal/null World created history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto restored = std::make_shared<core::World>("undo", 1, 1);
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			require(restored->deserialize(*reader, work), "History snapshot did not load");
			world = std::move(restored); world->pauseSimulation(); return true;
		};
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Undo failed");
		require(world->getLocationPermissionRequirement(sector) == std::vector<core::AccessPermissionId>{ red }, "Undo lost logical requirement change");
		require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save(*world)), restore), "Redo failed");
		require(world->getLocationPermissionRequirement(sector) == expected, "Redo lost requirement");
		click(pointFor(clearButton));
		require(world->getLocationPermissionRequirement(sector).empty()
			&& gWorldDocumentHistory.undoCount() == 3, "Clear was not one logical edit");
		require(!commitClearLocationPermissionRequirement(world, sector, diagnostic)
			&& gWorldDocumentHistory.undoCount() == 3, "Empty Clear created history");
		frame(); require(text.find("Required (all): None") != std::string::npos, "Clear did not show None");
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Clear undo failed");
		require(world->getLocationPermissionRequirement(sector) == expected, "Clear undo lost requirement");
		before = save(*world);
		require(world->resumeSimulation(), "Selection fixture did not resume");
		frame();
		require(text.find("Required (all): Red##literal, Blue") != std::string::npos, "Running Selection omitted authored requirement");
		require(!commitLocationPermissionRequirement(world, sector, red, false, diagnostic)
			&& diagnostic.find("paused") != std::string::npos && save(*world) == before
			&& gWorldDocumentHistory.undoCount() == 2, "Running Selection edit created history");
		// Rendering and actual clicks while disabled are both non-mutating.
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
		require(save(*world) == before && gWorldDocumentHistory.undoCount() == 2, "Disabled Selection checkbox edited requirement");
	}
}

void permission_smoke::registerLocationEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "permissions/locationSelectionRoom", [](smoke::Context const&) { selection(false); } });
	checks.push_back({ "permissions/locationSelectionCorridor", [](smoke::Context const&) { selection(true); } });
}
