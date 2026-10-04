#include "Checks.h"
#include "EditorState.h"
#include "AgentPoseTestAccess.h"
#include "AgentPosePanel.h"
#include "core/Agent.h"
#include "core/World.h"
#include "imgui.h"
#include <stdexcept>
#include <string>

namespace
{
	void captureText(void* data, char const* text)
	{
		*static_cast<std::string*>(data) += text ? text : "";
	}
	char const* readText(void*) { return ""; }

	void selectionShowsCurrentPose()
	{
		auto world = std::make_shared<core::World>("Pose inspection", 5, 2);
		auto corridor = world->addCorridor(0, 0, 4);
		world->finishBuild();
		auto id = world->createAgent("Selected", corridor);
		auto* agent = world->lookupAgent(id).entity;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1000, 2000);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		io.SetClipboardTextFn = &captureText;
		io.GetClipboardTextFn = &readText;
		std::string visible;
		io.ClipboardUserData = &visible;
		for (bool active : {true, false})
		{
			world->pauseSimulation();
			if (!world->setAgentActive(id, active)) throw std::runtime_error("Activation refused");
			for (auto pose : {core::Pose::Standing, core::Pose::Sitting, core::Pose::Lying, core::Pose::Standing})
			{
				core::AgentPoseTestAccess::set(*agent, pose);
				visible.clear();
				ImGui::NewFrame();
				ImGui::SetNextWindowSize(ImVec2(950, 1900));
				ImGui::Begin("Pose Selection");
				ImGui::LogToClipboard();
				renderAgentPose(*agent);
				ImGui::LogFinish();
				ImGui::End();
				ImGui::Render();
				auto label = pose == core::Pose::Standing ? "Pose: Standing"
					: pose == core::Pose::Sitting ? "Pose: Sitting" : "Pose: Lying";
				if (visible.find(label) == std::string::npos)
					throw std::runtime_error("Selection omitted current Pose");
				if (agent->getPose() != pose)
					throw std::runtime_error("Pose inspection changed runtime state");
			}
		}
		io.ClipboardUserData = nullptr;
	}
}

void agent_smoke::registerPoseEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({"agent/selectionShowsCurrentPose", [](smoke::Context const&) {
		EditorState state;
		selectionShowsCurrentPose();
	}});
}
