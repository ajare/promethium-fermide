#pragma once

#include "core/Agent.h"
#include "imgui.h"

inline void renderAgentPose(core::Agent const& agent)
{
	char const* pose = "Unknown";
	switch (agent.getPose())
	{
	case core::Pose::Standing: pose = "Standing"; break;
	case core::Pose::Sitting: pose = "Sitting"; break;
	case core::Pose::Lying: pose = "Lying"; break;
	case core::Pose::Crouching: pose = "Crouching"; break;
	case core::Pose::Crawling: pose = "Crawling"; break;
	}
	ImGui::Text("Pose: %s", pose);
}
