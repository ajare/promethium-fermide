#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "MarkerPanel.h"
#include "AgentBehaviourAssignmentPanel.h"
#include "core/World.h"
#include "core/Log.h"
#include "imgui/imgui.h"
#include <fstream>

namespace
{
	void workflow(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto path = context.temporaryRoot() / "editor.actions.lua";
		std::ofstream(path) << R"lua(return {api_version=1, uuid='ad603358-5ebf-45bb-a686-c3f491152c61', actions={
{key='hello',name='Hello',run=function(a,w,m) w.log('editor:' .. m.name) end},
{key='other',name='Other',run=function() end}}})lua";
		auto world = std::make_shared<core::World>("Editor",10,2);
		auto room = world->addRoom("Room",0,0,0,10,1);
		world->addSectorMarker(room,0,6.5f,"Target");
		world->finishBuild(); auto agent = world->createAgent("Visitor",room,0,1.5f);
		world->pauseSimulation();
		auto marker = world->getMarkerIds()[0];
		std::string const first = "ad603358-5ebf-45bb-a686-c3f491152c61:hello";
		std::string const second = "ad603358-5ebf-45bb-a686-c3f491152c61:other";
		std::string diagnostic;
		require(commitActionRegistrySelection(world,path,diagnostic), diagnostic);
		require(commitMarkerActionAssignment(world,marker,{second,first,second},diagnostic), diagnostic);
		require(commitAgentMarkerActionRequest(world,agent,marker,first,diagnostic), diagnostic);
		require(gWorldDocumentHistory.undoCount() == 3, "Authoring did not commit document history");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto replacement = deserializeDocumentSnapshot(snapshot, world, {});
			if (!replacement) return false;
			world = std::move(replacement); world->pauseSimulation(); return true;
		};
		for (int i = 0; i < 3; ++i)
			require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore), "Action history undo failed");
		require(!world->actionRegistry() && world->markerActions(marker).empty(), "Registry/assignment undo lost authored state");
		for (int i = 0; i < 3; ++i)
			require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world),restore), "Action history redo failed");
		require(world->markerActions(marker) == std::vector<std::string>{second,first}, "Action identity/order lost through history");
		require(world->renameMarker(marker,"Renamed"), "Marker rename refused");
		auto& io = ImGui::GetIO(); io.DisplaySize = {800,600};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Marker actions");
		require(renderAgentMovementActionSelector(world,marker) == core::IdleAction, "Editor no longer defaults to visible Idle");
		auto sector = world->getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (auto object = sector->getObject(i); object && object->getObjectType() == core::SectorObjectType::Marker)
				renderMarkerEditorPanel(world,object,[&](auto const& error) { diagnostic = error; });
		ImGui::End(); ImGui::Render();
		require(diagnostic.empty(), "Headless editor surface produced an error");
		require(world->resumeSimulation(), "Editor World resume failed");
		auto const historyCount = gWorldDocumentHistory.undoCount();
		require(!commitMarkerActionAssignment(world,marker,{},diagnostic)
			&& gWorldDocumentHistory.undoCount() == historyCount, "Running assignment changed World/history");
		bool arrived = false;
		core::consumeLogMessages();
		for (int tick = 0; tick < 1800 && !arrived; ++tick)
		{
			require(world->advanceTick(), "Editor-authored Lua Action failed");
			for (auto const& event : world->consumeSimulationEvents())
				if (event.type == core::SimulationEventType::DestinationReached)
				{
					arrived = true; require(event.selectedAction == first, "Editor request selected a different Action");
				}
		}
		require(arrived, "Editor-authored request never arrived");
		bool logged = false;
		for (auto const& log : core::consumeLogMessages()) logged |= log.msg == "editor:Renamed";
		require(logged, "Editor workflow did not execute real Lua logging");
		world->pauseSimulation();
		auto undo = captureDocumentSnapshot(world);
		require(world->clearActionRegistry(&diagnostic), diagnostic);
		commitDocumentEdit(std::move(undo));
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world),restore)
			&& world->markerActions(marker) == std::vector<std::string>{second,first}, "Registry removal cannot be undone");
	}
}

void editor_smoke::registerMarkerActions(std::vector<smoke::Check>& checks)
{
	checks.push_back({"markerActions/workflow",workflow});
}
