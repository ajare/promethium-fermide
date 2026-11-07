#pragma once

#include "core/Agent.h"
#include "core/World.h"

namespace core
{
	// Deliberately test-only: no editor, Lua or authored pose setter.
	struct AgentPoseTestAccess
	{
		static void set(Agent& agent, Pose pose)
		{
			agent.mPose = pose;
			if (agent.mWorld) agent.mWorld->invalidateSimulationSnapshot();
		}
	};
}
