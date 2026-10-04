#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "core/Agent.h"
#include "core/Button.h"
#include "BoothWindowEditor.h"
#include "PermissionsPanel.h"
#include "PaletteLayout.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui_internal.h"
#include <cmath>
#include "../support/DumbwaiterFixture.h"

namespace
{
	void surroundingEditHistory()
	{
		using smoke::require;
		for (unsigned edit = 0; edit < 8; ++edit)
		{
			auto world = dumbwaiter_fixture::make(0, edit != 1 && edit != 2, 2); world->addLayer();
			auto id = world->addDumbwaiter(2, 0, 2, {1, 0.37f}); world->finishBuild();
			auto lower = world->addAccessPermission("Lower"), upper = world->addAccessPermission("Upper");
			auto unit = world->lookupDumbwaiter(id);
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(0), {lower});
			world->setInteractionPointPermissionRequirement(unit->getLandingButton(1), {upper});
			world->pressDumbwaiterLanding(id, 0); world->resumeSimulation(); require(world->advanceTicks(60), "History cycle fixture failed");
			world->pauseSimulation(); gWorldDocumentHistory.clear();
			auto button = unit->getLandingButton(0);
			auto before = captureDocumentSnapshot(world);
			if (edit == 0) world->applyWalkwayEdit(world->planRemoveSectorWalkway(0, 0));
			if (edit == 1 || edit == 2) world->applyLocationEdit(world->planRemoveLocation(edit - 1));
			if (edit == 3) world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 1, 1));
			if (edit == 4) world->applyDeleteLayer(world->planDeleteLayer(1));
			if (edit == 5) world->applyDeleteLevel(world->planDeleteLevel(0));
			if (edit == 6) world->applyDeleteLayer(world->planDeleteLayer(0));
			if (edit == 7) world->applyLocationEdit(world->planResizeLocation(0, 2, 0, 2, 2));
			commitDocumentEdit(std::move(before));
			auto restore = [&](DocumentSnapshot const& snapshot) {
				auto input = core::YamlSerializer::fromString(snapshot.yaml); input->deserialize(); core::SerializationWorkData work;
				bool result = world->deserialize(*input, work); world->pauseSimulation(); return result;
			};
			require(gWorldDocumentHistory.undoCount() == 1 && !world->lookupInteractionPoint(button), "Dependent edit not undoable/stale handle retained");
			require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Dependent deletion undo failed");
			unit = world->lookupDumbwaiter(id);
			require(unit && unit->getLayerIndex() == 2 && unit->getInitialStop() == 1 && unit->getTravelSeconds() == 0.37f
				&& !unit->isBusy() && unit->getAperture(1)->getProgress() == 1 && unit->getAperture(0)->getProgress() == 0
				&& !world->lookupInteractionPoint(button), "Undo restored stale runtime ownership or incomplete unit");
			for (uint32_t stop = 0; stop < 2; ++stop)
				require(world->getInteractionPointPermissionRequirement(unit->getLandingButton(stop)) == std::vector<core::AccessPermissionId>{stop == 0 ? lower : upper},
					"Dependent deletion undo lost button requirements");
			auto actor = world->createAgent("Restored", unit->getStop(1).sector->getIndex(),
				float(unit->getCellY() + 1 - unit->getStop(1).sector->getCellY()), 0.0f);
			world->grantAgentAccessPermission(actor, upper);
			require(bool(world->requestDumbwaiterLanding(id, 1, actor)), "Undo-restored Agent control refused");
			world->resumeSimulation(); require(world->advanceTicks(130) && !unit->isBusy() && unit->getCarPosition().y == 0, "Undo-restored Agent operation failed");
			world->pauseSimulation();
			require(bool(world->pressDumbwaiterLanding(id, 0)), "Undo-restored user control refused");
			require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Dependent deletion redo failed");
			unit = world->lookupDumbwaiter(id);
			require(bool(unit) == (edit >= 6) && world->getSimulationSnapshot().interactionPoints.size() == (edit >= 6 ? 2u : 0u),
				"Redo restored orphan apertures/buttons");
			world->resetSimulation(); world->pauseSimulation();
			require(bool(world->lookupDumbwaiter(id)) == (edit >= 6), "History canonical replay lost dependency edits");
		}
	}

	void dumbwaiterHistory(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world = dumbwaiter_fixture::make(); gWorldDocumentHistory.clear();
		dumbwaiter_fixture::addLandings(*world, 1, 4);
		world->addRoom("Button history front", 0, 0, 0, 1, 1);
		world->addRoom("Button history back", 1, 0, 0, 1, 1);
		world->addSectorDoor(0, 0, 0, core::World::RemoteControlledDoor1Options);
		world->finishBuild();
		auto before = captureDocumentSnapshot(world);
		auto id = world->addDumbwaiter(1, 0, 2); world->finishBuild();
		commitDocumentEdit(std::move(before));
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto input = core::YamlSerializer::fromString(snapshot.yaml); input->deserialize(); core::SerializationWorkData work;
			bool result = world->deserialize(*input, work); world->pauseSimulation(); return result;
		};
		auto undo = [&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Dumbwaiter undo failed"); };
		auto redo = [&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Dumbwaiter redo failed"); };
		auto placement = [&] {
			std::vector<core::Vector2> positions;
			for (uint32_t s = 0; s < world->getNumSectors(); ++s)
				for (uint32_t o = 0; o < world->getSector(s)->getNumObjects(); ++o)
				{
					auto object = world->getSector(s)->getObject(o);
					auto button = object ? std::dynamic_pointer_cast<const core::Button>(object->_getObject()) : nullptr;
					if (!button) continue;
					positions.push_back(button->getPosition());
					positions.push_back(world->lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition());
				}
			return positions;
		};
		auto originalPlacement = placement();
		require(originalPlacement.size() == 8, "History fixture must contain Door and landing Buttons/approaches");
		undo(); require(!world->lookupDumbwaiter(id), "Creation undo retained unit");
		redo(); require(bool(world->lookupDumbwaiter(id)), "Creation redo lost unit identity");
		require(placement() == originalPlacement, "Undo/redo reconstruction changed Button or approach placement");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1400, 900}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		auto frame = [&] {
			ImGui::NewFrame(); ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1000, 600});
			ImGui::Begin("Dumbwaiter Selection");
			if (auto unit = world->lookupDumbwaiter(id)) renderDumbwaiterPanel(world, unit);
			ImGui::End(); ImGui::Render();
		};
		frame();
		auto click = [&](char const* label, bool slider = true) {
			auto window = ImGui::FindWindowByName("Dumbwaiter Selection"); auto control = window->GetID(label);
			bool found = false; ImVec2 point;
			for (float y = 35; y < 575 && !found; y += 7) for (float x = 15; x < 900 && !found; x += 15)
			{
				io.AddMousePosEvent(x, y); frame(); frame();
				if (ImGui::GetHoveredID() == control) { found = true; point = {x, y}; }
			}
			require(found, std::string("Missing Dumbwaiter Selection control: ") + label);
			io.AddMousePosEvent(point.x + (slider ? 100 : 0), point.y); frame();
			io.AddMouseButtonEvent(0, true); frame(); io.AddMouseButtonEvent(0, false); frame();
		};
		click("Travel time (seconds)");
		require(world->lookupDumbwaiter(id)->getTravelSeconds() != 2 && gWorldDocumentHistory.undoCount() == 2,
			"Actual Selection slider did not author history");
		undo(); require(world->lookupDumbwaiter(id)->getTravelSeconds() == 2, "Timing undo failed");
		redo(); require(world->lookupDumbwaiter(id)->getTravelSeconds() != 2, "Timing redo failed");
		auto unit = world->lookupDumbwaiter(id);
		auto owner = unit->getStop(0).sector;
		std::shared_ptr<const core::WindowSectorObject> aperture; uint32_t index = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (auto object = std::dynamic_pointer_cast<const core::WindowSectorObject>(owner->getObject(i)); object && object->getCellY() == 0)
			{ aperture = object; index = i; }
		require(aperture && !world->removeSectorWindow(owner->getIndex(), index)
			&& !world->planMoveSectorObject(owner->getIndex(), index, 3, 0).valid
			&& !world->planResizeSectorWindow(owner->getIndex(), index, 2, 0, 2, 1).valid,
			"Owned child exposed independent editing");
		bool refused = false; try { makeBoothWindowClipboardObject(*world, *aperture); }
		catch (std::exception const&) { refused = true; }
		require(refused, "Owned aperture can be copied independently");
		auto authored = captureDocumentSnapshot(world)->yaml;
		auto historyCount = gWorldDocumentHistory.undoCount();
		click("Press lower landing", false);
		require(world->lookupDumbwaiter(id)->isBusy() && world->getSimulationSnapshot().deviceOperations.size() == 1,
			"Actual runtime Selection did not submit typed landing press");
		require(world->resumeSimulation() && world->advanceTicks(12), "Runtime Selection cycle setup failed");
		world->pauseSimulation(); frame();
		require(world->lookupDumbwaiter(id)->getPhase() == core::DumbwaiterPhase::Closing
			&& captureDocumentSnapshot(world)->yaml == authored && gWorldDocumentHistory.undoCount() == historyCount,
			"Runtime Selection press authored progress/history");
		world->configureDumbwaiter(id, {1, 0.1f});
		click("Press upper landing", false);
		require(world->resumeSimulation() && world->advanceTicks(102), "Runtime Selection upper send failed");
		world->pauseSimulation(); frame();
		require(world->lookupDumbwaiter(id)->getCarPosition().y == 0 && !world->lookupDumbwaiter(id)->isBusy(),
			"Selection upper control did not send to lower landing and open arrival");
		click("Destination x", false);
		io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true); frame();
		io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
		io.AddInputCharactersUTF8("4"); frame();
		io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame();
		click("Move Dumbwaiter", false);
		require(world->lookupDumbwaiter(id)->getCellX() == 4 && gWorldDocumentHistory.undoCount() == 3,
			"Actual Selection whole-unit movement did not commit history");
		undo(); require(world->lookupDumbwaiter(id)->getCellX() == 2, "Selection move undo failed");
		redo(); require(world->lookupDumbwaiter(id)->getCellX() == 4, "Selection move redo failed");
		click("Delete Dumbwaiter", false);
		require(!world->lookupDumbwaiter(id) && gWorldDocumentHistory.undoCount() == 4, "Selection whole-unit deletion failed");
		undo(); require(world->lookupDumbwaiter(id)->getAperture(0)->getDumbwaiterOwner() == id, "Delete undo lost owned children");
		redo(); require(!world->lookupDumbwaiter(id), "Delete redo failed");
		surroundingEditHistory();
	}
	void dumbwaiterPermissionHistory(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world = dumbwaiter_fixture::make(); auto id = world->addDumbwaiter(1, 0, 2);
		world->finishBuild(); world->pauseSimulation(); gWorldDocumentHistory.clear();
		std::string diagnostic;
		auto a = commitAccessPermissionAdd(world, "Lower", diagnostic);
		auto b = commitAccessPermissionAdd(world, "Upper", diagnostic);
		auto requirements = [&](uint32_t stop) { return world->getInteractionPointPermissionRequirement(world->lookupDumbwaiter(id)->getLandingButton(stop)); };
		require(commitInteractionPermissionRequirement(world, world->lookupDumbwaiter(id)->getLandingButton(0), a, true, diagnostic)
			&& commitInteractionPermissionRequirement(world, world->lookupDumbwaiter(id)->getLandingButton(1), b, true, diagnostic), diagnostic);
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto input = core::YamlSerializer::fromString(snapshot.yaml); input->deserialize(); core::SerializationWorkData data;
			bool result = world->deserialize(*input, data); world->pauseSimulation(); return result;
		};
		auto undo = [&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Requirement undo failed"); };
		auto redo = [&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Requirement redo failed"); };
		undo(); require(requirements(0) == std::vector<core::AccessPermissionId>{a} && requirements(1).empty(), "Independent requirement undo failed");
		redo(); require(requirements(1) == std::vector<core::AccessPermissionId>{b}, "Requirement redo failed");
		auto snapshot = captureDocumentSnapshot(world)->yaml; auto count = gWorldDocumentHistory.undoCount();
		require(!commitInteractionPermissionRequirement(world, world->lookupDumbwaiter(id)->getLandingButton(0), core::AccessPermissionId{255}, true, diagnostic)
			&& captureDocumentSnapshot(world)->yaml == snapshot && gWorldDocumentHistory.undoCount() == count,
			"Invalid editor reference mutated document/history");
		auto actor = world->createAgent("Manual operator", 0, 0, 0.0f);
		require(world->grantAgentAccessPermission(actor, a), "Manual operator grant failed");
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
		io.DisplaySize = {1400,900}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGuiID control = 0;
		auto frame = [&] {
			ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({900,600});
			ImGui::Begin("Agent Selection Dumbwaiter");
			ImGui::PushID(static_cast<int>(world->lookupDumbwaiter(id)->getIndex())); ImGui::PushID(0);
			control = ImGui::GetID("Agent: press lower Dumbwaiter landing"); ImGui::PopID(); ImGui::PopID();
			renderDumbwaiterAgentActions(world, actor); ImGui::End(); ImGui::Render();
		};
		frame(); bool found = false; ImVec2 point;
		for (float y = 35; y < 180 && !found; y += 7) for (float x = 15; x < 500 && !found; x += 15)
		{
			io.AddMousePosEvent(x,y); frame(); frame();
			if (ImGui::GetHoveredID() == control) { found = true; point = {x,y}; }
		}
		require(found, "Manual Agent Selection action inaccessible");
		snapshot = captureDocumentSnapshot(world)->yaml; count = gWorldDocumentHistory.undoCount();
		io.AddMousePosEvent(point.x,point.y); frame(); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
		require(world->getSimulationSnapshot().interactionRequests.size() == 1 && !world->lookupDumbwaiter(id)->isBusy(), "Manual action bypassed typed interaction activation");
		world->resumeSimulation(); require(world->advanceTicks(80), "Manual Agent cycle failed"); world->pauseSimulation();
		auto unit = world->lookupDumbwaiter(id); auto position = unit->getCarPosition(); auto operation = unit->getOperation();
		require(captureDocumentSnapshot(world)->yaml == snapshot && gWorldDocumentHistory.undoCount() == count, "Manual action authored history");
		require(commitInteractionPermissionRequirement(world, unit->getLandingButton(0), b, true, diagnostic), diagnostic);
		require(unit->getCarPosition() == position && unit->getOperation() == operation && unit->isBusy(), "Editor permission edit reset active cycle");
		world->resumeSimulation(); while (unit->isBusy()) require(world->advanceTick(), "Cycle continuation failed"); world->pauseSimulation();
		require(unit->getCarPosition().y == 1 && unit->getAperture(1)->getProgress() == 1, "Permission edit interrupted accepted cycle");
		undo(); require(requirements(0) == std::vector<core::AccessPermissionId>{a}, "Permission-only undo lost original requirement");
		redo(); require(requirements(0) == std::vector<core::AccessPermissionId>{a,b}, "Permission-only redo failed");
		// Both independent landing requirements are removed by normal registry deletion, with undo/redo.
		require(commitAccessPermissionDelete(world, b, diagnostic), diagnostic);
		require(requirements(0) == std::vector<core::AccessPermissionId>{a} && requirements(1).empty(), "Deletion left dangling landing requirements");
		undo(); require(requirements(0) == std::vector<core::AccessPermissionId>{a,b} && requirements(1) == std::vector<core::AccessPermissionId>{b}, "Deletion undo lost landing requirements");
		redo(); world->resetSimulation(); world->pauseSimulation();
		require(requirements(0) == std::vector<core::AccessPermissionId>{a} && requirements(1).empty(), "Reset replay restored deleted requirement");
	}

	void dumbwaiterMoveClipboard(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world = dumbwaiter_fixture::oppositeLandings();
		dumbwaiter_fixture::addLandings(*world, 1, 5);
		dumbwaiter_fixture::addLandings(*world, 3, 0, 2);
		auto id = world->addDumbwaiter(1, 0, 2, {1, 0.5f}); world->finishBuild();
		auto a = world->addAccessPermission("Lower"), b = world->addAccessPermission("Shared");
		auto unit = world->lookupDumbwaiter(id);
		world->setInteractionPointPermissionRequirement(unit->getLandingButton(0), {a,b});
		world->setInteractionPointPermissionRequirement(unit->getLandingButton(1), {b});
		world->pressDumbwaiterLanding(id, 0); world->resumeSimulation(); require(world->advanceTicks(60), "Copy cycle setup failed");
		world->pauseSimulation(); gWorldDocumentHistory.clear();
		auto envelope = YAML::Load(makeDumbwaiterClipboardText(*world, *unit))["prometheumClipboard"];
		require(envelope["version"].as<unsigned>() == 1 && envelope["type"].as<std::string>() == "Dumbwaiter"
			&& envelope["operation"].as<std::string>() == "copy", "Editor clipboard envelope is not parseable by the production workflow");
		auto node = envelope["object"];
		auto payload = readDumbwaiterClipboardObject(node);
		require(node["initialStop"].as<unsigned>() == 1 && node["travelSeconds"].as<float>() == 0.5f
			&& !node["id"] && !node["operation"] && !node["phase"], "Clipboard leaked runtime state/device identity");
		world->renameAccessPermission(a, "Renamed Lower");
		auto originalOperation = unit->getOperation(); auto position = unit->getCarPosition();
		auto before = captureDocumentSnapshot(world);
		auto copy = pasteDumbwaiter(world, 1, 0, 5, payload); commitDocumentEdit(std::move(before));
		auto pasted = world->lookupDumbwaiter(copy);
		require(copy != id && !pasted->isBusy() && pasted->getCarPosition() == core::Vector2{5,1}
			&& pasted->getAperture(1)->getProgress() == 1 && unit->getCarPosition() == position
			&& unit->getOperation() == originalOperation && unit->isBusy(), "Mid-cycle paste cloned/reset live source progress");
		require(world->getInteractionPointPermissionRequirement(pasted->getLandingButton(0)) == std::vector<core::AccessPermissionId>{a,b}
			&& world->getInteractionPointPermissionRequirement(pasted->getLandingButton(1)) == std::vector<core::AccessPermissionId>{b},
			"Same-World copy failed rename-stable identity resolution");
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto input = core::YamlSerializer::fromString(snapshot.yaml); input->deserialize(); core::SerializationWorkData data;
			bool result = world->deserialize(*input, data); world->pauseSimulation(); return result;
		};
		auto undo = [&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Unit clipboard/move undo failed"); };
		auto redo = [&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Unit clipboard/move redo failed"); };
		undo(); require(!world->lookupDumbwaiter(copy), "Paste undo retained unit");
		redo(); require(world->lookupDumbwaiter(copy) && world->getSimulationSnapshot().interactionPoints.size() == 4, "Paste redo lost/duplicated child ownership");
		require(dumbwaiter_fixture::control(*world, id, 0)->getCellX() == 3
			&& dumbwaiter_fixture::control(*world, id, 1)->getCellX() == 2
			&& dumbwaiter_fixture::control(*world, copy, 0)->getCellX() == 5,
			"Clipboard/history retained source sides instead of independently allocating each unit");
		before = captureDocumentSnapshot(world);
		require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(copy, 3, 2, 0)), "Editor move refused");
		commitDocumentEdit(std::move(before));
		undo(); require(world->lookupDumbwaiter(copy)->getLayerIndex() == 1, "Move undo lost placement");
		redo(); require(world->lookupDumbwaiter(copy)->getLayerIndex() == 3
			&& world->lookupDumbwaiter(copy)->getCarPosition() == core::Vector2{0,3}, "Move redo lost placement/initial state");
		world->resetSimulation(); world->pauseSimulation(); pasted = world->lookupDumbwaiter(copy);
		require(world->getInteractionPointPermissionRequirement(pasted->getLandingButton(0)) == std::vector<core::AccessPermissionId>{a,b},
			"Move history/replay lost requirements");
		auto actor = world->createAgent("Restored operator", pasted->getStop(1).sector->getIndex(), 0, 0.0f);
		world->grantAgentAccessPermission(actor, b); auto request = world->requestDumbwaiterLanding(copy, 1, actor);
		require(bool(request), "Restored landing Agent request refused"); world->resumeSimulation(); require(world->advanceTicks(128), "Restored Agent cycle failed"); world->pauseSimulation();
		require(world->lookupDumbwaiter(copy)->getCarPosition().y == 2 && !world->lookupDumbwaiter(copy)->isBusy(), "Restored Agent button did not operate unit");

		auto foreign = dumbwaiter_fixture::make(2, false, 3);
		foreign->addAccessPermission("Unrelated"); auto mappedB = foreign->addAccessPermission("Shared"), mappedA = foreign->addAccessPermission("Lower");
		auto foreignId = pasteDumbwaiter(foreign, 3, 0, 2, payload);
		auto foreignUnit = foreign->lookupDumbwaiter(foreignId);
		auto lower = foreign->getInteractionPointPermissionRequirement(foreignUnit->getLandingButton(0));
		require(lower == std::vector<core::AccessPermissionId>{mappedB,mappedA}
			&& foreign->getInteractionPointPermissionRequirement(foreignUnit->getLandingButton(1)) == std::vector<core::AccessPermissionId>{mappedB},
			"Cross-World paste reused unrelated grants instead of resolving both landing names");
		dumbwaiter_fixture::addLandings(*foreign, 3, 4); foreign->finishBuild();
		auto foreignBefore = captureDocumentSnapshot(foreign)->yaml; bool refused = false;
		auto unresolvedUpper = payload; unresolvedUpper.landingPermissions[1].permissionNames = {"Missing upper"};
		try { pasteDumbwaiter(foreign, 3, 0, 4, unresolvedUpper); } catch (std::exception const&) { refused = true; }
		require(refused && captureDocumentSnapshot(foreign)->yaml == foreignBefore, "Upper resolution failure partially placed lower/configuration");
		foreign->deleteAccessPermission(mappedB);
		foreignBefore = captureDocumentSnapshot(foreign)->yaml; refused = false;
		try { pasteDumbwaiter(foreign, 3, 0, 4, payload); } catch (std::exception const&) { refused = true; }
		require(refused && captureDocumentSnapshot(foreign)->yaml == foreignBefore, "Unresolved upper requirement partially pasted/granted");
		for (auto field : {"width", "height", "initialStop", "travelSeconds", "authorizationWorldIdentity",
			"lowerLandingPermissionRequirement", "upperLandingPermissionRequirement", "operation"})
		{
			auto invalid = YAML::Clone(node); invalid[field] = "malformed";
			auto snapshot = captureDocumentSnapshot(world)->yaml; auto history = gWorldDocumentHistory.undoCount(); refused = false;
			try { pasteDumbwaiter(world, 1, 0, 4, readDumbwaiterClipboardObject(invalid)); } catch (std::exception const&) { refused = true; }
			require(refused && captureDocumentSnapshot(world)->yaml == snapshot && gWorldDocumentHistory.undoCount() == history,
				"Malformed unit clipboard mutated document/history");
		}
		for (uint32_t stop = 0; stop < 2; ++stop)
		{
			auto invalid = YAML::Clone(node); auto field = stop == 0 ? "lowerLandingPermissionRequirement" : "upperLandingPermissionRequirement";
			invalid[field].push_back(YAML::Clone(invalid[field][0])); refused = false;
			try { readDumbwaiterClipboardObject(invalid); } catch (std::exception const&) { refused = true; }
			require(refused, "Duplicate clipboard requirement accepted");
		}
		// Destination refusal must preserve an existing accepted cycle, document and history.
		unit = world->lookupDumbwaiter(id); auto operation = world->pressDumbwaiterLanding(id, 0);
		auto snapshot = captureDocumentSnapshot(world)->yaml; auto count = gWorldDocumentHistory.undoCount(); refused = false;
		try { pasteDumbwaiter(world, 1, 0, 2, payload); } catch (std::exception const&) { refused = true; }
		require(refused && captureDocumentSnapshot(world)->yaml == snapshot && gWorldDocumentHistory.undoCount() == count
			&& unit->getOperation() == operation && unit->isBusy(), "Invalid paste reset existing cycle/history");
	}

	void historyAndClipboard(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("BoothWindow editor",10,3);
		world->addRoom("Front",0,0,0,9,2); world->addRoom("Back",1,0,0,9,2);
		world->finishBuild(); world->pauseSimulation(); gWorldDocumentHistory.clear();
		auto panels=[&] {
			unsigned count=0;
			for (uint32_t sectorIndex=0;sectorIndex<world->getNumSectors();++sectorIndex)
			{
				auto sector=world->getSector(sectorIndex);
				for (uint32_t i=0;i<sector->getNumObjects();++i)
					if (auto object=std::dynamic_pointer_cast<const core::WindowSectorObject>(sector->getObject(i));
						object && object->getWindow()->isBoothWindow() && object->getWindow()->getFrontSector()==sector)
					{
						++count; auto booth=std::static_pointer_cast<const core::BoothWindow>(object->getWindow());
						auto panel=world->lookupInteractionPoint(booth->getPanel()).entity;
						require(panel && panel->getPosition().x==float(object->getCellX())+0.5f
							&& panel->getPosition().y==float(object->getCellY())
							&& panel->getSector().value==booth->getBackSector()->getIndex()+1,
							"History/clipboard/move reconstructed panel on wrong side/position");
					}
			}
			require(world->getSimulationSnapshot().interactionPoints.size()==count,"History duplicated or orphaned panel");
		};
		auto edit=[&](auto action) { auto before=captureDocumentSnapshot(world); action(); commitDocumentEdit(std::move(before)); panels(); };
		edit([&] { world->addBoothWindow(0,0,2); world->finishBuild(); });
		require(paletteSlotRow(PaletteSlot::BoothWindow)==1 && gWorldDocumentHistory.undoCount()==1,"Palette creation not distinct/undoable");
		auto get=[&](uint32_t x) -> std::shared_ptr<const core::WindowSectorObject> {
			auto owner=world->getSector(0);
			for (uint32_t i=0;i<owner->getNumObjects();++i)
				if (auto object=std::dynamic_pointer_cast<const core::WindowSectorObject>(owner->getObject(i));
					object && object->getObjectType()==core::SectorObjectType::BoothWindow && object->getCellX()==x) return object;
			return {};
		};
		auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr; io.DisplaySize={1200,800};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		auto frame=[&] {
			ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({800,500});
			ImGui::Begin("BoothWindow Selection"); renderBoothWindowPanel(world,get(2)); ImGui::End(); ImGui::Render();
		};
		frame(); auto* window=ImGui::FindWindowByName("BoothWindow Selection"); auto control=window->GetID("Initially Open");
		bool found=false; ImVec2 point;
		for (float y=35;y<220 && !found;y+=8) for (float x=15;x<220 && !found;x+=16)
		{
			io.AddMousePosEvent(x,y); frame(); frame();
			if (ImGui::GetHoveredID()==control) { found=true; point={x,y}; }
		}
		require(found,"Initial shutter checkbox inaccessible");
		io.AddMousePosEvent(point.x,point.y); frame(); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
		require(get(2)->getWindow()->getState()==core::Window::State::Open && gWorldDocumentHistory.undoCount()==2,"Panel did not edit authored state/history");
		// Operate the actual runtime panel button; it must not author a history entry.
		auto authoredSnapshot = captureDocumentSnapshot(world)->yaml;
		auto historyCount = gWorldDocumentHistory.undoCount();
		auto toggleControl = window->GetID("Toggle shutter"); found = false;
		for (float y=35;y<220 && !found;y+=8) for (float x=15;x<220 && !found;x+=16)
		{
			io.AddMousePosEvent(x,y); frame(); frame();
			if (ImGui::GetHoveredID()==toggleControl) { found=true; point={x,y}; }
		}
		require(found, "Runtime shutter control inaccessible");
		io.AddMousePosEvent(point.x,point.y); frame(); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
		require(world->getSimulationSnapshot().deviceOperations.size()==1, "Runtime panel bypassed typed device commands");
		world->resumeSimulation(); require(world->advanceTicks(12), "Runtime panel motion failed"); world->pauseSimulation();
		auto device = std::static_pointer_cast<const core::BoothWindow>(get(2)->getWindow());
		require(device->getState()==core::Window::State::Closing && std::abs(device->getProgress()-0.75f)<0.00001f,
			"Runtime panel did not animate shutter");
		require(gWorldDocumentHistory.undoCount()==historyCount && captureDocumentSnapshot(world)->yaml==authoredSnapshot,
			"Runtime panel mutated authored history/snapshot");
		std::string diagnostic;
		auto permission = commitAccessPermissionAdd(world, "Operator", diagnostic);
		require(bool(permission), "Permission registry authoring failed");
		require(commitInteractionPermissionRequirement(world, device->getPanel(), permission, true, diagnostic),
			"BoothWindow selection requirement editing failed");
		auto requirement = [&](uint32_t x) {
			auto booth = std::static_pointer_cast<const core::BoothWindow>(get(x)->getWindow());
			return world->getInteractionPointPermissionRequirement(booth->getPanel());
		};
		require(requirement(2) == std::vector<core::AccessPermissionId>{permission}, "Authored panel requirement missing");
		auto payload=makeBoothWindowClipboardObject(*world,*get(2));
		require(payload["initialState"].as<std::string>()=="Open" && !payload["traversable"] && !payload["style"],"Clipboard leaked ordinary Window capabilities");
		edit([&] { pasteBoothWindow(world,0,0,4,readBoothWindowClipboardObject(payload)); });
		require(bool(get(4)),"Production paste lost BoothWindow");
		auto restore=[&](DocumentSnapshot const& snapshot) {
			auto reader=core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize(); core::SerializationWorkData work;
			bool result=world->deserialize(*reader,work); world->pauseSimulation(); panels(); return result;
		};
		auto undo=[&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore),"Undo failed"); };
		auto redo=[&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore),"Redo failed"); };
		undo(); require(!get(4),"Paste undo failed"); redo(); require(bool(get(4)),"Paste redo failed");
		require(requirement(4) == requirement(2), "Clipboard/history lost protection");
		undo(); undo(); require(requirement(2).empty(), "Requirement undo failed");
		redo(); require(requirement(2) == std::vector<core::AccessPermissionId>{permission}, "Requirement redo failed");
		redo();
		// Foreign Worlds remap by name, never by coincident local slot; same-World
		// rename retains identity. Missing foreign references refuse before placement.
		auto foreign = std::make_shared<core::World>("Other", 10, 3);
		foreign->addRoom("Front",0,0,0,9,2); foreign->addRoom("Back",1,0,0,9,2);
		foreign->finishBuild(); foreign->pauseSimulation();
		foreign->addAccessPermission("Unrelated"); auto mapped = foreign->addAccessPermission("Operator");
		auto copied = pasteBoothWindow(foreign,0,0,2,readBoothWindowClipboardObject(payload));
		auto copiedBooth = std::static_pointer_cast<const core::BoothWindow>(std::static_pointer_cast<const core::WindowSectorObject>(copied)->getWindow());
		require(foreign->getInteractionPointPermissionRequirement(copiedBooth->getPanel()) == std::vector<core::AccessPermissionId>{mapped},
			"Foreign clipboard reused unrelated permission identity");
		require(foreign->deleteAccessPermission(mapped), "Foreign permission deletion failed");
		auto foreignBefore = captureDocumentSnapshot(foreign)->yaml; bool refused = false;
		try { pasteBoothWindow(foreign,0,0,6,readBoothWindowClipboardObject(payload)); }
		catch (std::exception const&) { refused = true; }
		require(refused && captureDocumentSnapshot(foreign)->yaml == foreignBefore, "Unknown clipboard permission mutated destination");
		require(world->renameAccessPermission(permission, "Renamed Operator"), "Permission rename failed");
		auto indexOf=[&](auto object) { auto owner=world->getSector(0); for (uint32_t i=0;i<owner->getNumObjects();++i) if (owner->getObject(i)==object) return i; return ~0u; };
		// Cut writes the payload before using normal deletion; paste retains initial state.
		payload=makeBoothWindowClipboardObject(*world,*get(4));
		edit([&] { require(world->removeSectorWindow(0,indexOf(get(4))),"Cut deletion refused"); });
		require(!get(4),"Cut retained object"); undo(); require(bool(get(4)),"Cut undo failed"); redo(); require(!get(4),"Cut redo failed");
		edit([&] { pasteBoothWindow(world,0,0,5,readBoothWindowClipboardObject(payload)); });
		require(get(5)->getWindow()->getState()==core::Window::State::Open,"Cut/paste lost initial state");
		for (auto field : {"width","height","initialState","style","traversable","initiallyBroken","panelPermissionRequirement","authorizationWorldIdentity"})
		{
			auto malformed=YAML::Clone(payload);
			if (std::string(field)=="initialState") malformed[field]="Broken"; else malformed[field]=2;
			auto counts=gWorldDocumentHistory.undoCount(); bool refused=false;
			try { auto invalid=readBoothWindowClipboardObject(malformed); pasteBoothWindow(world,0,0,6,invalid); }
			catch (std::exception const&) { refused=true; }
			require(refused && !get(6) && gWorldDocumentHistory.undoCount()==counts,"Malformed clipboard mutated World/history");
		}
		auto index=indexOf(get(2));
		require(!world->planResizeSectorWindow(0,index,2,0,2,1).valid,"BoothWindow exposed resizing");
		auto plan=world->planMoveSectorObject(0,index,3,0); require(plan.valid,plan.diagnostic);
		edit([&] { world->applyObjectMove(plan); }); require(bool(get(3)) && !get(2),"Move lost BoothWindow");
		undo(); require(bool(get(2)) && !get(3),"Move undo failed"); redo(); require(bool(get(3)) && !get(2),"Move redo failed");
		world->resetSimulation(); panels(); require(get(3)->getWindow()->getState()==core::Window::State::Open,"Reset lost authored state");
		require(requirement(3) == std::vector<core::AccessPermissionId>{permission} && requirement(5) == requirement(3),
			"Move/reset/replay lost panel requirements");
		world->pauseSimulation();
		require(commitAccessPermissionDelete(world, permission, diagnostic), "Permission deletion failed");
		require(requirement(3).empty() && requirement(5).empty(), "Deletion left dangling panel references");
		undo(); require(requirement(3) == std::vector<core::AccessPermissionId>{permission}, "Deletion undo lost protection");
		redo(); require(requirement(3).empty(), "Deletion redo failed");
		world->resetSimulation(); panels(); require(requirement(3).empty(), "Replay restored deleted reference");
	}
}
void editor_smoke::registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"dumbwaiters/selectionAndHistory", dumbwaiterHistory});
	checks.push_back({"dumbwaiters/wholeUnitMoveAndClipboard", dumbwaiterMoveClipboard});
	checks.push_back({"dumbwaiters/landingPermissionHistoryAndAgentSelection", dumbwaiterPermissionHistory});
	checks.push_back({"boothWindows/historyAndClipboard",historyAndClipboard});
}
