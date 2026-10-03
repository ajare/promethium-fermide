#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "BoothWindowEditor.h"
#include "PaletteLayout.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui_internal.h"

namespace
{
	void historyAndClipboard(smoke::Context const&)
	{
		editor_smoke::State state; using smoke::require;
		auto world=std::make_shared<core::World>("BoothWindow editor",10,3);
		world->addRoom("Front",0,0,0,9,2); world->addRoom("Back",1,0,0,9,2);
		world->finishBuild(); world->pauseSimulation(); gWorldDocumentHistory.clear();
		auto edit=[&](auto action) { auto before=captureDocumentSnapshot(world); action(); commitDocumentEdit(std::move(before)); };
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
		auto payload=makeBoothWindowClipboardObject(*world,*get(2));
		require(payload["initialState"].as<std::string>()=="Open" && !payload["traversable"] && !payload["style"],"Clipboard leaked ordinary Window capabilities");
		edit([&] { pasteBoothWindow(world,0,0,4,readBoothWindowClipboardObject(payload)); });
		require(bool(get(4)),"Production paste lost BoothWindow");
		auto restore=[&](DocumentSnapshot const& snapshot) {
			auto reader=core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize(); core::SerializationWorkData work;
			bool result=world->deserialize(*reader,work); world->pauseSimulation(); return result;
		};
		auto undo=[&] { require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore),"Undo failed"); };
		auto redo=[&] { require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore),"Redo failed"); };
		undo(); require(!get(4),"Paste undo failed"); redo(); require(bool(get(4)),"Paste redo failed");
		auto indexOf=[&](auto object) { auto owner=world->getSector(0); for (uint32_t i=0;i<owner->getNumObjects();++i) if (owner->getObject(i)==object) return i; return ~0u; };
		// Cut writes the payload before using normal deletion; paste retains initial state.
		payload=makeBoothWindowClipboardObject(*world,*get(4));
		edit([&] { require(world->removeSectorWindow(0,indexOf(get(4))),"Cut deletion refused"); });
		require(!get(4),"Cut retained object"); undo(); require(bool(get(4)),"Cut undo failed"); redo(); require(!get(4),"Cut redo failed");
		edit([&] { pasteBoothWindow(world,0,0,5,readBoothWindowClipboardObject(payload)); });
		require(get(5)->getWindow()->getState()==core::Window::State::Open,"Cut/paste lost initial state");
		for (auto field : {"width","height","initialState","style","traversable","initiallyBroken"})
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
		world->resetSimulation(); require(get(3)->getWindow()->getState()==core::Window::State::Open,"Reset lost authored state");
	}
}
void editor_smoke::registerBoothWindows(std::vector<smoke::Check>& checks)
{
	checks.push_back({"boothWindows/historyAndClipboard",historyAndClipboard});
}
