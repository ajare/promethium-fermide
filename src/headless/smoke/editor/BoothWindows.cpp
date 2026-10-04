#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "core/Agent.h"
#include "BoothWindowEditor.h"
#include "PermissionsPanel.h"
#include "PaletteLayout.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui_internal.h"
#include <cmath>
#include "../support/DumbwaiterFixture.h"

namespace
{
	void dumbwaiterHistory(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world = dumbwaiter_fixture::make(); gWorldDocumentHistory.clear();
		auto before = captureDocumentSnapshot(world);
		auto id = world->addDumbwaiter(1, 0, 2); world->finishBuild();
		commitDocumentEdit(std::move(before));
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto input = core::YamlSerializer::fromString(snapshot.yaml); input->deserialize(); core::SerializationWorkData work;
			bool result = world->deserialize(*input, work); world->pauseSimulation(); return result;
		};
		auto undo = [&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore), "Dumbwaiter undo failed"); };
		auto redo = [&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Dumbwaiter redo failed"); };
		undo(); require(!world->lookupDumbwaiter(id), "Creation undo retained unit");
		redo(); require(bool(world->lookupDumbwaiter(id)), "Creation redo lost unit identity");
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
			for (float y = 35; y < 400 && !found; y += 7) for (float x = 15; x < 400 && !found; x += 15)
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
		click("Delete Dumbwaiter", false);
		require(!world->lookupDumbwaiter(id) && gWorldDocumentHistory.undoCount() == 3, "Selection whole-unit deletion failed");
		undo(); require(world->lookupDumbwaiter(id)->getAperture(0)->getDumbwaiterOwner() == id, "Delete undo lost owned children");
		redo(); require(!world->lookupDumbwaiter(id), "Delete redo failed");
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
		auto actor = world->createAgent("Manual operator", 0, 0, 0.5f);
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
	checks.push_back({"dumbwaiters/landingPermissionHistoryAndAgentSelection", dumbwaiterPermissionHistory});
	checks.push_back({"boothWindows/historyAndClipboard",historyAndClipboard});
}
