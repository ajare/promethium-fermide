#include "Checks.h"
#include "EditorState.h"
#include "AgentBehaviourAssignmentPanel.h"
#include "AgentClipboard.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "core/Agent.h"
#include "imgui/imgui.h"
#include <yaml-cpp/yaml.h>

namespace
{
	void workflow()
	{
		agent_smoke::EditorState state;
		gWorldDocumentHistory.clear();
		auto world = std::make_shared<core::World>("Player editor", 12, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 12, 1);
		world->finishBuild(); world->pauseSimulation();
		auto id = world->createAgent("Player", room, 0, 2.f);
		std::string diagnostic;
		smoke::require(commitAgentBehaviourAssignment(world, id, core::PlayerControlledBehaviour,
			1, {}, diagnostic), "Assign Player controlled failed");
		smoke::require(gWorldDocumentHistory.undoCount() == 1 && !world->hasAttachedAgentBehaviourRegistry(),
			"Built-in assignment must be one edit without registry dependencies");
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {});
			return bool(world);
		};
		smoke::require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& !world->getPlayerControlledAgent(), "Undo must clear Player eligibility");
		smoke::require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore)
			&& world->getPlayerControlledAgent() == id, "Redo must restore built-in assignment");

		auto& io = ImGui::GetIO();
		io.DisplaySize = {640, 480}; io.DeltaTime = 1.f / 60.f;
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::Begin("Player picker");
		renderAgentBehaviourConfigurationPanel(world, id);
		ImGui::End(); ImGui::Render();
		auto portable = makeAgentClipboardPayload(*world, id, "Player copy");
		auto text = makeAgentClipboardText(portable, false);
		AgentClipboardPayload parsed;
		auto read = readAgentClipboardObject(YAML::Load(text)["promethiumClipboard"]["object"], parsed, diagnostic);
		smoke::require(read, "Read Player clipboard: " + diagnostic);
		auto destination = std::make_shared<core::World>("Player paste", 12, 2);
		auto targetRoom = destination->addRoom("Destination", 0, 0, 0, 12, 1);
		destination->finishBuild(); destination->pauseSimulation();
		core::AgentId pasted;
		auto placed = commitAgentPlacement(destination, parsed, destination->getSector(targetRoom),
			0, 4.f, pasted, diagnostic);
		smoke::require(placed, "Paste Player clipboard: " + diagnostic);
		smoke::require(destination->lookupAgent(pasted).entity->isPlayerControlled()
			&& destination->getPlayerControlledAgent() == pasted
			&& !destination->hasAttachedAgentBehaviourRegistry(), "Player clipboard must be portable between Worlds without registries");
		world->pauseSimulation();
		auto cleared = commitAgentBehaviourClear(world, id, diagnostic);
		smoke::require(cleared, "Clear Player: " + diagnostic);
		smoke::require(!world->getPlayerControlledAgent(), "Clearing built-in must disable controls");
	}
}

void agent_smoke::registerPlayerControlEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({"playerControlEditorWorkflow", [](smoke::Context const&) { workflow(); }});
}
