#include "Checks.h"
#include "ImGuiContext.h"
#include "DocumentEdit.h"
#include "DocumentHistory.h"
#include "PaletteLayout.h"
#include "SecurityScannerPanel.h"
#include "UI.h"
#include "core/YamlSerializer.h"
#include "core/Exceptions.h"
#include "imgui/imgui_internal.h"
#include <yaml-cpp/yaml.h>

namespace
{
	void commands(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		ImGui::GetIO().IniFilename = nullptr; ImGui::GetIO().LogFilename = nullptr;
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build();
		for (bool direction : { false, true })
			for (uint32_t width : { 1u, 3u })
				for (bool open : { false, true })
				{
					auto world = std::make_shared<core::World>("Scanner history", 12, 3);
					world->addRoom("Left", 0, 0, 0, 2, 1); world->addCorridor(0, 0, 2 + width, 2, 1);
					if (open)
					{
						auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
						world->serialize(*writer, work); writer->serialize(); auto node = YAML::Load(writer->getSerializedString());
						YAML::Node wall; wall["type"] = "removeWall"; wall["sectorIndex"] = 0;
						wall["levelIndex"] = 0; wall["side"] = "right"; wall["airlockWallRestoration"] = true;
						node["construction"].push_back(wall);
						auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
						require(world->deserialize(*reader, work), "Open-end history fixture refused"); world->pauseSimulation();
					}
					world->finishBuild(); world->pauseSimulation(); world->markSaved();
					DocumentHistory history;
					float start = direction ? 2.25f : width + 1.75f, end = direction ? width + 1.75f : 2.25f;
					auto draft = planSecurityScannerDrag(*world, 0, start, 0, end, 0);
					require(draft.valid && draft.x == 2 && draft.width == width
						&& draft.leftToRight == direction, "Directional palette preview incorrect");
					auto before = captureDocumentSnapshot(world, history);
					auto index = commitSecurityScannerDraft(*world, 0, draft); world->finishBuild();
					commitDocumentEdit(std::move(before), history);
					require(history.undoCount() == 1 && isCanvasSelectableSectorType(core::SectorType::SecurityScanner)
						&& paletteSlotRow(PaletteSlot::SecurityScanner) == 0, "Scanner not palette-created/selectable/undoable");
					auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
					ImGui::GetIO().DisplaySize = { 1200, 720 }; ImGui::NewFrame();
					ImGui::Begin("Scanner Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
					ImGui::LogToBuffer(); drawSecurityScannerSelectionPanel(*chamber);
					std::string text = GImGui->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::Render();
					for (auto readout : { "Security scanner", "Capacity: 1", "Pre-delay: 1.0 s", "Complete scan: 2.0 s",
						"Post-pause: 1.0 s", "Sensor distance: 0.5 units", "structural editing unavailable" })
						require(text.find(readout) != std::string::npos, "Production Selection missing scanner readout");
					require(text.find(draft.leftToRight ? "Left to right" : "Right to left") != std::string::npos, "Selection direction wrong");
					auto restore = [&](DocumentSnapshot const& snapshot) {
						auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
						core::SerializationWorkData work; return world->deserialize(*reader, work);
					};
					require(history.undo(captureDocumentSnapshot(world, history), restore) && world->getNumSectors() == 2
						&& world->getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
						&& world->getSector(1)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::Wall
						&& world->getSector(0)->getNumObjects() == 0 && world->getSector(1)->getNumObjects() == 0,
						"Scanner undo left orphan Doors or lost adjoining walls");
					require(history.redo(captureDocumentSnapshot(world, history), restore), "Scanner redo refused");
					chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
					require(chamber && chamber->getNumObjects() == 2 && chamber->getDoor(0)->isClosed()
						&& chamber->getDoor(1)->isSecurityScannerOwned() && chamber->isLeftToRight() == draft.leftToRight
						&& chamber->getCapacity() == 1 && chamber->getScanSeconds() == 2, "Scanner redo lost configuration/devices");
					world->markSaved(); auto baseline = captureDocumentSnapshot(world, history)->yaml;
					for (auto invalid : { planSecurityScannerDrag(*world, 0, 2, 0, 4, 1),
						planSecurityScannerDrag(*world, 0, 2, 0, 4, 0), planSecurityScannerDrag(*world, 0, -1, 0, 4, 0) })
					{
						require(!invalid.valid && !invalid.diagnostic.empty(), "Invalid palette drag accepted");
						bool refused = false;
						try { commitSecurityScannerDraft(*world, 0, invalid); } catch (core::Exception const&) { refused = true; }
						require(refused && captureDocumentSnapshot(world, history)->yaml == baseline && !world->isModified()
							&& history.undoCount() == 1, "Invalid palette drag mutated document/history");
					}
				}
	}
}

void editor_smoke::registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/editorCommandsAndHistory", commands });
}
