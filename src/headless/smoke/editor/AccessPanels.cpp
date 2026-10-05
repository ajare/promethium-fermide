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
	void workflow(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("Panel editor",8,3);
		auto room=world->addFacade(0,0,1,6,2); world->addSectorWalkway(room,1,2);
		world->finishBuild(); world->pauseSimulation(); world->markSaved(); gWorldDocumentHistory.clear();
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
		auto panel=std::static_pointer_cast<const core::AccessPanelSectorObject>(get())->getPanel();
		core::Vector2 min,max; panel->getSelectionShape(min,max); auto centre=(min+max)*0.5f;
		std::shared_ptr<const core::SectorObject> selected;
		require(world->getObjectAtPosition(0,centre.x,centre.y,&selected)==panel && selected==get(),"Zero-width panel not canvas-selectable");
		auto stale=get(); click("Delete Access panel"); require(!get() && gWorldDocumentHistory.undoCount()==3
			&& !world->getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(stale)),"Selection deletion left an orphan");
		undo(); require(get() && geometry().width==0,"Deletion undo lost panel geometry"); redo(); require(!get(),"Deletion redo retained panel");
		require(!deleteAccessPanel(world,stale) && gWorldDocumentHistory.undoCount()==3,"Stale Selection deleted a replacement");
	}
}
void editor_smoke::registerAccessPanels(std::vector<smoke::Check>& checks)
{
	checks.push_back({"accessPanels/selectionAndHistory",workflow});
}
