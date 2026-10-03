#include "Checks.h"
#include "ImGuiContext.h"
#include "DocumentEdit.h"
#include "DocumentHistory.h"
#include "PaletteLayout.h"
#include "ChamberPanel.h"
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
			auto index = world->addChamber(0, 0, 3, 1, forward);
			world->finishBuild(); world->pauseSimulation();
			require(world->setChamberConfiguration(index, 8, 0, 0.1f, 10), "History configuration refused");
			DocumentHistory history;
			auto restore = [&](DocumentSnapshot const& snapshot) {
				auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
				core::SerializationWorkData work; bool result = world->deserialize(*reader, work);
				world->pauseSimulation(); return result;
			};
			std::vector<std::string> states{ captureDocumentSnapshot(world, history)->yaml };
			auto stale = world->planResizeChamber(index, 4, 1, 2, !forward);
			world->resumeSimulation();
			for (auto plan : { stale, world->planRemoveChamber(index) })
			{
				bool refused = false;
				try { world->applyChamberEdit(plan); } catch (core::Exception const&) { refused = true; }
				require(refused && captureDocumentSnapshot(world, history)->yaml == states[0]
					&& history.undoCount() == 0 && history.redoCount() == 0, "Running structural edit changed document/history");
			}
			world->pauseSimulation();
			for (auto plan : { world->planResizeChamber(index, 0, 0, 1, forward),
				world->planResizeChamber(index, 3, 0, 2, forward) })
			{
				require(!plan.valid, "Invalid history geometry planned");
				bool refused = false;
				try { world->applyChamberEdit(plan); } catch (core::Exception const&) { refused = true; }
				require(refused && captureDocumentSnapshot(world, history)->yaml == states[0]
					&& history.undoCount() == 0, "Invalid structural edit changed document/history");
			}
			for (int step = 0; step < 4; ++step)
			{
				auto before = captureDocumentSnapshot(world, history);
				auto plan = step == 3 ? world->planRemoveChamber(index)
					: world->planResizeChamber(index, step == 0 ? 3 : 3 + step,
						step == 0 ? 0 : step, step == 0 ? 1 : 1 + step, !forward);
				require(plan.valid, "History scanner edit refused");
				index = world->applyChamberEdit(plan);
				commitDocumentEdit(std::move(before), history);
				states.push_back(captureDocumentSnapshot(world, history)->yaml);
			}
			require(history.undoCount() == 4 && world->getNumSectors() == 6, "Structural commands not recorded");
			for (int step = 3; step >= 0; --step)
			{
				require(history.undo(captureDocumentSnapshot(world, history), restore), "Structural undo refused");
				require(captureDocumentSnapshot(world, history)->yaml == states[step], "Structural undo lost authored state/wall metadata");
				auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(6));
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

	void selectionWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr; io.LogFilename = nullptr; io.DisplaySize = {1200, 900};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		for (bool legacy : { false, true })
		{
			auto world = std::make_shared<core::World>("Chamber Selection", 14, 3);
			for (uint32_t row = 0; row < 2; ++row)
			{
				world->addRoom("Left", 0, row, 0, 4, 1);
				world->addCorridor(0, row, 6, 4, 1);
			}
			auto index = commitChamberDraft(*world, 0, planChamberDrag(*world, 0, 4.2f, 0, 5.8f, 0));
			world->finishBuild(); world->pauseSimulation();
			require(world->setChamberConfiguration(index, 8, 3, 4, 5), "Selection fixture configuration refused");
			DocumentHistory history;
			auto restore = [&](DocumentSnapshot const& snapshot) {
				auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
				core::SerializationWorkData work; bool result = world->deserialize(*reader, work);
				world->pauseSimulation(); return result;
			};
			if (legacy)
			{
				auto node = YAML::Load(captureDocumentSnapshot(world, history)->yaml);
				node["version"] = 44;
				for (auto record : node["construction"])
					if (record["type"].as<std::string>() == "chamber")
					{ record["type"] = "securityScanner"; record.remove("subtype"); }
				auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
				core::SerializationWorkData work;
				require(world->deserialize(*reader, work), "Legacy Selection fixture failed to load");
				world->pauseSimulation();
			}
			auto chamber = [&] { return std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(index)); };
			require(chamber()->getSubtype() == core::ChamberSubtype::SecurityScanner
				&& isCanvasSelectableSectorType(chamber()->getType()), "Default/legacy Chamber not selectable");
			std::string text;
			auto frame = [&] {
				ImGui::NewFrame(); ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 850});
				ImGui::Begin("Chamber Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
				ImGui::LogToBuffer();
				auto plan = index < world->getNumSectors()
					? drawChamberSelectionPanel(world, *chamber(), history)
					: std::optional<core::World::ChamberEditPlan>{};
				text = GImGui->LogBuffer.c_str(); ImGui::LogFinish();
				ImGui::End(); ImGui::Render();
				if (plan && plan->valid)
				{
					auto before = captureDocumentSnapshot(world, history);
					auto updated = world->applyChamberEdit(*plan);
					if (!plan->remove) index = updated;
					commitDocumentEdit(std::move(before), history);
				}
			};
			frame(); frame();
			auto pointFor = [&](char const* label) {
				auto id = ImGui::FindWindowByName("Chamber Selection")->GetID(label);
				for (float y = 35; y < 700; y += 7)
					for (float x : {30.0f, 100.0f, 300.0f})
					{
						io.AddMousePosEvent(x, y); frame(); frame();
						if (ImGui::GetHoveredID() == id) return ImVec2{x, y};
					}
				throw std::runtime_error(std::string("Selection control inaccessible: ") + label);
			};
			auto click = [&](ImVec2 point) {
				io.AddMousePosEvent(point.x, point.y); frame();
				io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
			};
			auto baseline = captureDocumentSnapshot(world, history)->yaml;
			world->markSaved();
			click(pointFor("Subtype")); frame();
			require(!GImGui->OpenPopupStack.empty(), "Subtype dropdown did not open");
			auto popup = GImGui->OpenPopupStack.back().Window;
			require(popup && popup->Active, "Subtype options not visible");
			// One row, with no None/future rows. The sole option uses the subtype's literal label.
			require(popup->ContentSize.y > 0 && popup->ContentSize.y <= ImGui::GetFrameHeightWithSpacing(),
				"Subtype dropdown must offer exactly one option");
			io.AddMousePosEvent(popup->Pos.x + 20, popup->Pos.y + 10); frame(); frame();
			require(ImGui::GetHoveredID() == popup->GetID("Security Scanner"),
				"Subtype dropdown sole option is not Security Scanner");
			click({popup->Pos.x + 20, popup->Pos.y + 10});
			require(GImGui->OpenPopupStack.empty() && captureDocumentSnapshot(world, history)->yaml == baseline
				&& !world->isModified() && history.undoCount() == 0 && history.redoCount() == 0,
				"Selecting active subtype changed configuration/history/dirty state");
			click(pointFor("Left to right"));
			require(!chamber()->isLeftToRight() && history.undoCount() == 1, "Scanner direction panel edit failed");
			require(history.undo(captureDocumentSnapshot(world, history), restore)
				&& captureDocumentSnapshot(world, history)->yaml == baseline, "Panel direction undo lost configuration");
			require(history.redo(captureDocumentSnapshot(world, history), restore)
				&& !chamber()->isLeftToRight(), "Panel direction redo failed");
			auto input = [&](char const* label, char const* value) {
				click(pointFor(label));
				io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
				io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
				io.AddInputCharactersUTF8(value); frame();
				io.AddKeyEvent(ImGuiKey_Enter, true); frame();
				io.AddKeyEvent(ImGuiKey_Enter, false); frame();
			};
			input("Sensor distance (World units)", "12");
			require(chamber()->getSensorDistance() == 12 && history.undoCount() == 2,
				"Production scanner configuration control did not commit history");
			require(history.undo(captureDocumentSnapshot(world, history), restore)
				&& chamber()->getSensorDistance() == 8, "Configuration control undo failed");
			require(history.redo(captureDocumentSnapshot(world, history), restore)
				&& chamber()->getSensorDistance() == 12, "Configuration control redo failed");
			auto unchanged = captureDocumentSnapshot(world, history)->yaml;
			input("Sensor distance (World units)", "-1");
			input("Chamber width", "3");
			input("Chamber x", "0");
			require(captureDocumentSnapshot(world, history)->yaml == unchanged && history.undoCount() == 2,
				"Invalid configuration/geometry partially mutated document/history");
			input("Chamber Level", "1");
			require(chamber()->getCellY() == 1 && history.undoCount() == 3, "General Chamber Level control failed");
			require(history.undo(captureDocumentSnapshot(world, history), restore)
				&& captureDocumentSnapshot(world, history)->yaml == unchanged, "Geometry control undo failed");
			require(history.redo(captureDocumentSnapshot(world, history), restore)
				&& chamber()->getCellY() == 1, "Geometry control redo failed");
			auto directionPoint = pointFor("Left to right"), deletePoint = pointFor("Delete Chamber");
			world->resumeSimulation(); auto count = history.undoCount();
			unchanged = captureDocumentSnapshot(world, history)->yaml;
			click(directionPoint); click(deletePoint);
			require(history.undoCount() == count && captureDocumentSnapshot(world, history)->yaml == unchanged,
				"Running panel authored history");
			world->pauseSimulation();
			click(pointFor("Delete Chamber"));
			require(world->getNumSectors() == 4 && history.undoCount() == count + 1, "Chamber deletion panel failed");
			require(history.undo(captureDocumentSnapshot(world, history), restore) && chamber()
				&& chamber()->getSensorDistance() == 12 && chamber()->getPreDelaySeconds() == 3
				&& chamber()->getScanSeconds() == 4 && chamber()->getPostPauseSeconds() == 5,
				"Panel deletion undo lost subtype configuration");
			require(history.redo(captureDocumentSnapshot(world, history), restore) && world->getNumSectors() == 4,
				"Panel deletion redo failed");
			require(history.undo(captureDocumentSnapshot(world, history), restore), "Occupied fixture restore failed");
			auto marker = world->addSectorMarker(2, 0, 2.0f); world->finishBuild();
			auto agent = world->lookupAgent(world->createAgent("Traveller", 3, 0, 2.0f)).entity;
			agent->setPath(world->getGraph()->calculatePath(agent,
				world->getGraph()->getVertexForObject(marker.sector->getObject(marker.index))), true);
			require(world->resumeSimulation(), "Occupied panel fixture could not resume");
			bool scanning = false;
			for (unsigned tick = 0; tick < 2000 && !scanning; ++tick)
			{
				world->advanceTick();
				scanning = chamber()->getPhase() == core::SecurityScannerPhase::Scanning;
			}
			require(scanning, "Occupied panel fixture never reached scan phase");
			world->pauseSimulation(); frame();
			for (auto status : { "Capacity: 1", "Phase: Scanning", "Remaining:", "Occupancy: 1 / 1", "Scan progress:" })
				require(text.find(status) != std::string::npos, "Occupied Selection lost subtype status");
			unchanged = captureDocumentSnapshot(world, history)->yaml; count = history.undoCount();
			auto state = world->getSimulationSnapshot().securityScanners.at(0);
			click(pointFor("Left to right")); input("Chamber Level", "0"); click(pointFor("Delete Chamber"));
			auto after = world->getSimulationSnapshot().securityScanners.at(0);
			require(captureDocumentSnapshot(world, history)->yaml == unchanged && history.undoCount() == count
				&& after.occupant == state.occupant && after.phase == state.phase
				&& after.remainingSeconds == state.remainingSeconds && after.scanProgress == state.scanProgress,
				"Occupied panel structural edits mutated World/history/journey");
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
					auto draft = planChamberDrag(*world, 0, start, 0, end, 0);
					require(draft.valid && draft.x == 2 && draft.width == width
						&& draft.leftToRight == direction, "Directional palette preview incorrect");
					auto before = captureDocumentSnapshot(world, history);
					auto index = commitChamberDraft(*world, 0, draft); world->finishBuild();
					commitDocumentEdit(std::move(before), history);
					require(history.undoCount() == 1 && isCanvasSelectableSectorType(core::SectorType::Chamber)
						&& paletteSlotRow(PaletteSlot::Chamber) == 0, "Scanner not palette-created/selectable/undoable");
					auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(index));
					require(chamber && chamber->getSubtype() == core::ChamberSubtype::SecurityScanner,
						"Production painting did not create the default Chamber subtype");
					ImGui::GetIO().DisplaySize = { 1200, 720 }; ImGui::NewFrame();
					ImGui::Begin("Scanner Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
					ImGui::LogToBuffer(); drawChamberSelectionPanel(world, *chamber, history);
					std::string text = GImGui->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::Render();
					for (auto readout : { "Chamber", "Subtype", "Security Scanner", "Chamber x", "Chamber Level", "Chamber width", "Delete Chamber",
						"Left to right", "Sensor distance (World units)", "Pre-scan delay (seconds)",
						"Complete scan duration (seconds)", "Post-scan pause (seconds)", "Capacity: 1", "Pre-delay: 1.0 s", "Complete scan: 2.0 s",
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
					chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(index));
					require(chamber && chamber->getNumObjects() == 2 && chamber->getDoor(0)->isClosed()
						&& chamber->getDoor(1)->isSecurityScannerOwned() && chamber->isLeftToRight() == draft.leftToRight
						&& chamber->getCapacity() == 1 && chamber->getScanSeconds() == 2, "Scanner redo lost configuration/devices");
					world->pauseSimulation();
					auto propertyBefore = captureDocumentSnapshot(world, history);
					require(world->setChamberConfiguration(index, 20, 10, 0.1f, 0), "Property edit refused");
					commitDocumentEdit(std::move(propertyBefore), history);
					require(history.undo(captureDocumentSnapshot(world, history), restore), "Property undo refused");
					chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(index));
					require(chamber->getSensorDistance() == 0.5f && chamber->getScanSeconds() == 2, "Property undo lost defaults");
					require(history.redo(captureDocumentSnapshot(world, history), restore), "Property redo refused");
					chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(world->getSector(index));
					require(chamber->getSensorDistance() == 20 && chamber->getPreDelaySeconds() == 10
						&& chamber->getScanSeconds() == 0.1f && chamber->getPostPauseSeconds() == 0, "Property redo lost values");
					world->markSaved(); auto baseline = captureDocumentSnapshot(world, history)->yaml;
					for (auto invalid : { planChamberDrag(*world, 0, 2, 0, 4, 1),
						planChamberDrag(*world, 0, 2, 0, 4, 0), planChamberDrag(*world, 0, -1, 0, 4, 0) })
					{
						require(!invalid.valid && !invalid.diagnostic.empty(), "Invalid palette drag accepted");
						bool refused = false;
						try { commitChamberDraft(*world, 0, invalid); } catch (core::Exception const&) { refused = true; }
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
	checks.push_back({ "securityScanners/selectionWorkflow", selectionWorkflow });
}
