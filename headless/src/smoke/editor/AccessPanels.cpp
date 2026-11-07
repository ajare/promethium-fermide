#include "Checks.h"
#include "State.h"
#include "AccessPanelEditor.h"
#include "DocumentEdit.h"
#include "PaletteLayout.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui_internal.h"
#include <limits>

namespace
{
	void clipboardAndMovement()
	{
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("Panel clipboard",16,3);
		auto room=world->addRoom("Room",0,0,0,5,2);
		auto corridor=world->addCorridor(0,0,6,4,1); auto facade=world->addFacade(0,0,11,4,2);
		world->addSectorWalkway(facade,1,2); world->finishBuild(); world->pauseSimulation(); gWorldDocumentHistory.clear();
		std::string diagnostic;
		auto source=placeAccessPanel(world,room,0,2,{0,0.25f,0.5f},diagnostic,0.125f);
		auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(source)->getPanel();
		auto actor=world->createAgent("Operator",room,0,2.5f);
		require(bool(world->requestAccessPanel(panel->getId(),core::AccessPanel::Action::Open,actor)), "Clipboard Open refused");
		world->resumeSimulation(); world->advanceTicks(180); world->pauseSimulation();
		auto payload=makeAccessPanelClipboardObject(*world,source);
		require(payload.size()==5 && payload["speed"].as<float>()==0.125f && payload["panelType"].as<std::string>()=="Empty", "Clipboard copied live ownership/state");
		world->markSaved(); auto before=captureDocumentSnapshot(world)->yaml; auto graph=world->getGraph();
		for (auto bad : {YAML::Load("[]"), YAML::Load("{panelType: Empty}"), YAML::Load("{panelType: Equipment, width: 0, height: 0, yOffset: 0}"),
			YAML::Load("{panelType: Empty, width: .nan, height: 0, yOffset: 0}"), YAML::Load("{panelType: Empty, width: 0, height: 1, yOffset: 1}"),
			YAML::Load("{panelType: Empty, width: 0, height: 0, yOffset: 0, state: Open}"),
			YAML::Load("{panelType: Empty, width: 0, height: 0, yOffset: 0, speed: 0}"),
			YAML::Load("{panelType: Empty, width: 0, height: 0, yOffset: 0, speed: .inf}")})
			require(!pasteAccessPanel(world,corridor,0,8,bad,diagnostic), "Malformed clipboard accepted");
		require(!pasteAccessPanel(world,room,0,2,payload,diagnostic) && !pasteAccessPanel(world,facade,1,12,payload,diagnostic)
			&& captureDocumentSnapshot(world)->yaml==before && world->getGraph()==graph && !world->isModified()
			&& gWorldDocumentHistory.undoCount()==1, "Failed paste changed document/history");
		auto copied=pasteAccessPanel(world,corridor,0,8,payload,diagnostic);
		auto copy=std::static_pointer_cast<const core::AccessPanelSectorObject>(copied)->getPanel();
		require(copy->getId()!=panel->getId() && copy->getSpeedOverride()==0.125f && copy->getGeometry()==panel->getGeometry() && copy->getState()==core::AccessPanel::State::Closed
			&& copy->getControl(core::AccessPanel::Action::Open)!=panel->getControl(core::AccessPanel::Action::Open)
			&& gWorldDocumentHistory.undoCount()==2, "Copy reused live identity/references/state");
		auto restore=[&](DocumentSnapshot const& snapshot) { auto loaded=deserializeDocumentSnapshot(snapshot,world,{}); if (!loaded) return false; world=loaded; world->pauseSimulation(); return true; };
		auto at=[&](uint32_t s,uint32_t x,uint32_t y) -> std::shared_ptr<const core::SectorObject> {
			for (uint32_t i=0;i<world->getSector(s)->getNumObjects();++i) { auto o=world->getSector(s)->getObject(i);
				if (o && o->getObjectType()==core::SectorObjectType::AccessPanel && o->getCellX()==x && o->getCellY()==y) return o; } return {}; };
		auto undo=[&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore), "Clipboard/move undo failed"); };
		auto redo=[&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore), "Clipboard/move redo failed"); };
		undo(); require(!at(corridor,8,0) && at(room,2,0), "Paste undo ownership wrong"); redo(); require(bool(at(corridor,8,0)), "Paste redo missing");
		auto stale=at(room,2,0);
		auto stalePanel=std::static_pointer_cast<const core::AccessPanelSectorObject>(stale)->getPanel();
		auto pending=world->requestAccessPanel(stalePanel->getId(),core::AccessPanel::Action::Open,actor);
		require(bool(pending), "Pending move fixture refused");
		auto moved=moveAccessPanel(world,stale,13,1,diagnostic);
		require(!world->lookupInteractionRequest(pending).entity && !world->lookupInteractionPoint(stalePanel->getControl(core::AccessPanel::Action::Open))
			&& !world->lookupAccessPanel(stalePanel->getId()), "Movement copied stale handles or pending requests");
		require(moved && at(facade,13,1)==moved && !at(room,2,0) && gWorldDocumentHistory.undoCount()==3, "Move destination/history wrong");
		undo(); require(at(room,2,0) && !at(facade,13,1), "Move undo ownership wrong"); redo(); require(!at(room,2,0) && at(facade,13,1), "Move redo ownership wrong");
		require(std::static_pointer_cast<const core::AccessPanelSectorObject>(at(facade,13,1))->getPanel()->getSpeedOverride()==0.125f,
			"Move/history lost authored speed");
		world->markSaved(); before=captureDocumentSnapshot(world)->yaml; graph=world->getGraph();
		require(!moveAccessPanel(world,stale,3,0,diagnostic) && !deleteAccessPanel(world,stale)
			&& !moveAccessPanel(world,at(facade,13,1),8,0,diagnostic) && !moveAccessPanel(world,at(facade,13,1),12,1,diagnostic)
			&& captureDocumentSnapshot(world)->yaml==before && world->getGraph()==graph && !world->isModified()
			&& gWorldDocumentHistory.undoCount()==3, "Invalid/stale move changed document history");
	}
	void structuralHistory()
	{
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("Panel structural history",12,4);
		auto room=world->addRoom("Room",0,0,0,6,2); world->addRoom("Back",1,0,0,12,3);
		auto walkway=world->addSectorWalkway(room,1,3);
		world->addAccessPanel(room,1,3,{1,1,0}); world->addAccessPanel(room,0,4,{0.5f,0.25f,0.5f});
		world->finishBuild(); world->pauseSimulation(); world->markSaved(); gWorldDocumentHistory.clear();
		auto before=captureDocumentSnapshot(world); auto graph=world->getGraph();
		require(!world->planRemoveSectorWalkway(room,walkway.index).valid
			&& !world->planResizeLocation(room,0,0,3,2).valid, "Editor support/shrink preview accepted");
		bool refused=false; try { world->addSectorLightSwitch(room,4); } catch (std::exception const&) { refused=true; }
		// The switch is below the edge-touching panel; this surrounding edit is valid.
		require(!refused, "Permitted surrounding Button edit refused"); commitDocumentEdit(before);
		world->finishBuild(); world->markSaved(); before=captureDocumentSnapshot(world); graph=world->getGraph();
		refused=false; try { world->addBoothWindow(0,0,4); } catch (std::exception const&) { refused=true; }
		require(!refused,"Permitted BoothWindow edge contact refused"); commitDocumentEdit(before); world->finishBuild();
		world->markSaved(); before=captureDocumentSnapshot(world); graph=world->getGraph();
		refused=false; try { world->addSectorDoor(0,1,3); } catch (std::exception const&) { refused=true; }
		require(refused && captureDocumentSnapshot(world)->yaml==before->yaml && world->getGraph()==graph
			&& !world->isModified() && gWorldDocumentHistory.undoCount()==2, "Rejected surrounding editor edit changed history/state");
		auto restore=[&](DocumentSnapshot const& snapshot) { auto loaded=deserializeDocumentSnapshot(snapshot,world,{});
			if (!loaded) return false;
			world=loaded; world->pauseSimulation(); return true; };
		auto assertPanels=[&](uint32_t x,uint32_t y) {
			unsigned count=0;
			for (uint32_t i=0;i<world->getSector(room)->getNumObjects();++i)
				if (auto p=std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(world->getSector(room)->getObject(i)))
				{
					++count; auto vertex=world->getGraph()->getVertexForObject(std::const_pointer_cast<core::AccessPanelSectorObject>(p));
					require(p->getCellX()==x+(p->getPanel()->getLevelOffset()==1 ? 3u : 4u)
						&& p->getCellY()==y+p->getPanel()->getLevelOffset() && vertex
						&& p->getPanel()->getState()==core::AccessPanel::State::Closed
						&& world->lookupInteractionPoint(p->getPanel()->getControl(core::AccessPanel::Action::Open)), "History reconstruction lost panel geometry/controls");
				}
			require(count==2,"History lost retained panels");
		};
		auto undo=[&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore),"Structural undo failed"); };
		auto redo=[&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore),"Structural redo failed"); };
		undo(); assertPanels(0,0); redo(); assertPanels(0,0);
		before=captureDocumentSnapshot(world); auto move=world->planResizeLocation(room,6,1,6,2);
		require(move.valid,"Editor Location carry refused: "+move.diagnostic); room=world->applyLocationEdit(move); commitDocumentEdit(before);
		assertPanels(6,1); undo(); assertPanels(0,0); redo(); assertPanels(6,1);
		before=captureDocumentSnapshot(world); auto shrink=world->planResizeLocation(room,6,1,5,2);
		require(shrink.valid,"Valid retained-panel shrink refused"); room=world->applyLocationEdit(shrink); commitDocumentEdit(before);
		assertPanels(6,1); undo(); assertPanels(6,1); redo(); assertPanels(6,1);
		before=captureDocumentSnapshot(world); auto removal=world->planRemoveLocation(room);
		require(removal.valid,"Editor Location deletion refused"); world->applyLocationEdit(removal); commitDocumentEdit(before);
		undo(); assertPanels(6,1); redo();
		for (uint32_t s=0;s<world->getNumSectors();++s) for (uint32_t i=0;i<world->getSector(s)->getNumObjects();++i)
			require(!std::dynamic_pointer_cast<const core::AccessPanelSectorObject>(world->getSector(s)->getObject(i)),"Owner deletion redo restored orphan panel");
	}
	void workflow(smoke::Context const&)
	{
		clipboardAndMovement();
		structuralHistory();
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("Panel editor",8,3);
		auto room=world->addFacade(0,0,1,6,2); world->addSectorWalkway(room,1,2);
		world->finishBuild(); auto actor = world->createAgent("Operator", room, 1, 2.5f);
		world->pauseSimulation(); world->markSaved(); gWorldDocumentHistory.clear();
		std::string diagnostic;
		require(paletteSlotRow(PaletteSlot::AccessPanel)==1 && paletteSlotColumn(PaletteSlot::AccessPanel)!=paletteSlotColumn(PaletteSlot::BoothWindow), "Missing distinct panel palette slot");
		auto before=captureDocumentSnapshot(world)->yaml; auto graph=world->getGraph();
		require(!placeAccessPanel(world,room,1,2,{},diagnostic) && captureDocumentSnapshot(world)->yaml==before
			&& !world->isModified() && world->getGraph()==graph && gWorldDocumentHistory.undoCount()==0, "Rejected palette placement changed document/history");
		auto object=placeAccessPanel(world,room,1,3,{},diagnostic);
		require(object && gWorldDocumentHistory.undoCount()==1, "Palette action not undoable");
		auto restore=[&](DocumentSnapshot const& snapshot) {
			auto loaded=deserializeDocumentSnapshot(snapshot,world,{}); if (!loaded) return false;
			world=loaded; world->pauseSimulation(); return true;
		};
		auto get=[&]() -> std::shared_ptr<const core::SectorObject> {
			for (uint32_t i=0;i<world->getSector(room)->getNumObjects();++i)
				if (auto o=world->getSector(room)->getObject(i); o && o->getObjectType()==core::SectorObjectType::AccessPanel) return o;
			return {};
		};
		auto undo=[&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore), "Panel undo failed"); };
		auto redo=[&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore), "Panel redo failed"); };
		undo(); require(!get(),"Placement undo retained panel"); redo(); require(bool(get()),"Placement redo lost panel");
		world->markSaved(); before=captureDocumentSnapshot(world)->yaml; graph=world->getGraph();
		require(!editAccessPanel(world,get(),{0.5f,0.8f,0.4f},diagnostic)
			&& !editAccessPanel(world,get(),{std::numeric_limits<float>::quiet_NaN(),0.25f,0.25f},diagnostic)
			&& captureDocumentSnapshot(world)->yaml==before && world->getGraph()==graph && !world->isModified()
			&& gWorldDocumentHistory.undoCount()==1, "Invalid geometry edit changed state/history");
		// Exercise the actual Selection property field and delete button using CPU-only ImGui.
		auto& io=ImGui::GetIO(); io.IniFilename=nullptr; io.LogFilename=nullptr;
		io.DisplaySize={1200,800}; io.Fonts->AddFontDefault(); io.Fonts->Build();
		auto frame=[&] {
			ImGui::NewFrame(); ImGui::SetNextWindowPos({10,10}); ImGui::SetNextWindowSize({900,500});
			ImGui::Begin("Access panel Selection"); if (auto selected=get()) renderAccessPanelPanel(world,selected);
			renderAccessPanelAgentActions(world, actor);
			ImGui::End(); ImGui::Render();
		};
		auto click=[&](char const* label) {
			frame(); auto window=ImGui::FindWindowByName("Access panel Selection"); auto id=window->GetID(label);
			bool found=false; ImVec2 point;
			for (float y=35;y<400 && !found;y+=7) for (float x=15;x<750 && !found;x+=15)
			{
				io.AddMousePosEvent(x,y); frame(); frame(); if (ImGui::GetHoveredID()==id) {found=true;point={x,y};}
			}
			require(found,std::string("Missing Selection control: ")+label);
			io.AddMousePosEvent(point.x,point.y); frame(); io.AddMouseButtonEvent(0,true); frame(); io.AddMouseButtonEvent(0,false); frame();
		};
		click("Width");
		io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
		io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); io.AddInputCharactersUTF8("0"); frame();
		io.AddKeyEvent(ImGuiKey_Enter,true); frame(); io.AddKeyEvent(ImGuiKey_Enter,false); frame();
		auto geometry=[&] { return std::static_pointer_cast<const core::AccessPanelSectorObject>(get())->getPanel()->getGeometry(); };
		require(geometry().width==0 && gWorldDocumentHistory.undoCount()==2, "Selection geometry did not author history");
		undo(); require(geometry().width==0.5f,"Geometry undo failed"); redo(); require(geometry().width==0,"Geometry redo failed");
		auto speedOverride=[&] { return std::static_pointer_cast<const core::AccessPanelSectorObject>(get())->getPanel()->getSpeedOverride(); };
		click("Override speed"); require(speedOverride().has_value(), "Selection override not authored");
		click("Speed (units/s)");
		io.AddKeyEvent(ImGuiMod_Ctrl,true); io.AddKeyEvent(ImGuiKey_A,true); frame();
		io.AddKeyEvent(ImGuiKey_A,false); io.AddKeyEvent(ImGuiMod_Ctrl,false); io.AddInputCharactersUTF8("0.125"); frame();
		io.AddKeyEvent(ImGuiKey_Enter,true); frame(); io.AddKeyEvent(ImGuiKey_Enter,false); frame();
		require(speedOverride()==0.125f, "Selection speed input ignored");
		undo(); undo(); require(!speedOverride(), "Speed undo did not restore default");
		redo(); redo(); require(speedOverride()==0.125f, "Speed redo lost override");
		undo(); undo();
		auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(get())->getPanel();
		core::Vector2 min,max; panel->getSelectionShape(min,max); auto centre=(min+max)*0.5f;
		std::shared_ptr<const core::SectorObject> selected;
		require(world->getObjectAtPosition(0,centre.x,centre.y,&selected)==panel && selected==get(),"Zero-width panel not canvas-selectable");
		world->markSaved(); auto authored = captureDocumentSnapshot(world)->yaml;
		auto actions = accessPanelAgentActions(*world, actor);
		require(actions.size() == 1 && actions[0].enabled && actions[0].action == core::AccessPanel::Action::Open, "Agent Open action missing");
		click(actions[0].label.c_str()); world->resumeSimulation(); world->advanceTicks(180); world->pauseSimulation();
		require(panel->getState() == core::AccessPanel::State::Open && !world->isModified()
			&& captureDocumentSnapshot(world)->yaml == authored && gWorldDocumentHistory.undoCount() == 2, "Runtime Open entered document history");
		actions = accessPanelAgentActions(*world, actor);
		require(actions.size() == 1 && actions[0].action == core::AccessPanel::Action::Close, "Empty menu exposes controls other than Close");
		click(actions[0].label.c_str()); world->resumeSimulation(); world->advanceTicks(180); world->pauseSimulation();
		require(panel->getState() == core::AccessPanel::State::Closed && gWorldDocumentHistory.undoCount() == 2, "Close action/history regression");
		world->setAgentActive(actor, false); actions = accessPanelAgentActions(*world, actor);
		require(actions.size() == 1 && !actions[0].enabled, "Inactive Agent menu enabled");
		world->setAgentActive(actor, true);
		auto staleControl = panel->getControl(core::AccessPanel::Action::Open);
		auto stale=get(); click("Delete Access panel"); require(!get() && gWorldDocumentHistory.undoCount()==3
			&& !world->getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(stale)),"Selection deletion left an orphan");
		undo(); require(get() && geometry().width==0,"Deletion undo lost panel geometry"); redo(); require(!get(),"Deletion redo retained panel");
		require(!deleteAccessPanel(world,stale) && gWorldDocumentHistory.undoCount()==3,"Stale Selection deleted a replacement");
		undo();
		auto restored = std::static_pointer_cast<const core::AccessPanelSectorObject>(get())->getPanel();
		require(!world->lookupInteractionPoint(staleControl) && !world->requestInteraction(staleControl, actor)
			&& world->requestAccessPanel(restored->getId(), core::AccessPanel::Action::Open, actor), "History restored stale/unusable owned interactions");
		world->resumeSimulation(); world->advanceTicks(180); world->pauseSimulation();
		require(restored->getState() == core::AccessPanel::State::Open, "Reconstructed panel operation failed");
	}
}
void editor_smoke::registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/selectionAndHistory",workflow});
}
