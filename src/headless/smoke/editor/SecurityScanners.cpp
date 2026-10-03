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
	void structuralHistory(smoke::Context const&)
	{
		using smoke::require;
		for (bool forward : { false, true })
		{
			auto world = std::make_shared<core::World>("Scanner structural history", 14, 3);
			for (uint32_t row = 0; row < 3; ++row)
			{
				world->addRoom("Left", 0, row, 0, 3 + row, 1);
				world->addCorridor(0, row, 4 + 2 * row, 3, 1);
			}
			auto index = world->addSecurityScanner(0, 0, 3, 1, forward);
			world->finishBuild(); world->pauseSimulation();
			require(world->setSecurityScannerConfiguration(index, 8, 0, 0.1f, 10), "History configuration refused");
			DocumentHistory history;
			auto restore = [&](DocumentSnapshot const& snapshot) {
				auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
				core::SerializationWorkData work; bool result = world->deserialize(*reader, work);
				world->pauseSimulation(); return result;
			};
			std::vector<std::string> states{ captureDocumentSnapshot(world, history)->yaml };
			auto stale = world->planResizeSecurityScanner(index, 4, 1, 2, !forward);
			world->resumeSimulation();
			for (auto plan : { stale, world->planRemoveSecurityScanner(index) })
			{
				bool refused = false;
				try { world->applySecurityScannerEdit(plan); } catch (core::Exception const&) { refused = true; }
				require(refused && captureDocumentSnapshot(world, history)->yaml == states[0]
					&& history.undoCount() == 0 && history.redoCount() == 0, "Running structural edit changed document/history");
			}
			world->pauseSimulation();
			for (auto plan : { world->planResizeSecurityScanner(index, 0, 0, 1, forward),
				world->planResizeSecurityScanner(index, 3, 0, 2, forward) })
			{
				require(!plan.valid, "Invalid history geometry planned");
				bool refused = false;
				try { world->applySecurityScannerEdit(plan); } catch (core::Exception const&) { refused = true; }
				require(refused && captureDocumentSnapshot(world, history)->yaml == states[0]
					&& history.undoCount() == 0, "Invalid structural edit changed document/history");
			}
			for (int step = 0; step < 4; ++step)
			{
				auto before = captureDocumentSnapshot(world, history);
				auto plan = step == 3 ? world->planRemoveSecurityScanner(index)
					: world->planResizeSecurityScanner(index, step == 0 ? 3 : 3 + step,
						step == 0 ? 0 : step, step == 0 ? 1 : 1 + step, !forward);
				require(plan.valid, "History scanner edit refused");
				index = world->applySecurityScannerEdit(plan);
				commitDocumentEdit(std::move(before), history);
				states.push_back(captureDocumentSnapshot(world, history)->yaml);
			}
			require(history.undoCount() == 4 && world->getNumSectors() == 6, "Structural commands not recorded");
			for (int step = 3; step >= 0; --step)
			{
				require(history.undo(captureDocumentSnapshot(world, history), restore), "Structural undo refused");
				require(captureDocumentSnapshot(world, history)->yaml == states[step], "Structural undo lost authored state/wall metadata");
				auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(6));
				require(chamber && chamber->getDoor(0)->isSecurityScannerOwned() && chamber->getDoor(1)->isSecurityScannerOwned()
					&& chamber->getCapacity() == 1 && chamber->getSensorDistance() == 8
					&& chamber->getScanSeconds() == 0.1f && world->isTraversalTopologyValid(), "Structural undo lost configuration/ownership");
			}
			for (int step = 1; step <= 4; ++step)
			{
				require(history.redo(captureDocumentSnapshot(world, history), restore), "Structural redo refused");
				require(captureDocumentSnapshot(world, history)->yaml == states[step], "Structural redo lost authored state");
			}
		}
	}

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
						"Post-pause: 1.0 s", "Sensor distance: 0.5 units", "Phase: Idle", "Remaining: 0.0 s",
						"Occupancy: 0 / 1", "Scan progress: 0%", "structural edits require an empty, paused chamber" })
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
					world->pauseSimulation();
					auto propertyBefore = captureDocumentSnapshot(world, history);
					require(world->setSecurityScannerConfiguration(index, 20, 10, 0.1f, 0), "Property edit refused");
					commitDocumentEdit(std::move(propertyBefore), history);
					require(history.undo(captureDocumentSnapshot(world, history), restore), "Property undo refused");
					chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
					require(chamber->getSensorDistance() == 0.5f && chamber->getScanSeconds() == 2, "Property undo lost defaults");
					require(history.redo(captureDocumentSnapshot(world, history), restore), "Property redo refused");
					chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(world->getSector(index));
					require(chamber->getSensorDistance() == 20 && chamber->getPreDelaySeconds() == 10
						&& chamber->getScanSeconds() == 0.1f && chamber->getPostPauseSeconds() == 0, "Property redo lost values");
					world->markSaved(); auto baseline = captureDocumentSnapshot(world, history)->yaml;
					for (auto invalid : { planSecurityScannerDrag(*world, 0, 2, 0, 4, 1),
						planSecurityScannerDrag(*world, 0, 2, 0, 4, 0), planSecurityScannerDrag(*world, 0, -1, 0, 4, 0) })
					{
						require(!invalid.valid && !invalid.diagnostic.empty(), "Invalid palette drag accepted");
						bool refused = false;
						try { commitSecurityScannerDraft(*world, 0, invalid); } catch (core::Exception const&) { refused = true; }
						require(refused && captureDocumentSnapshot(world, history)->yaml == baseline && !world->isModified()
							&& history.undoCount() == 2, "Invalid palette drag mutated document/history");
					}
				}
	}
}

void editor_smoke::registerSecurityScanners(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "securityScanners/editorCommandsAndHistory", commands });
	checks.push_back({ "securityScanners/structuralHistory", structuralHistory });
}
