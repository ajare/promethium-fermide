#pragma once

#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/World.h"
#include "../support/Smoke.h"

namespace pose_journeys
{
	inline void attachRobot(core::World& world)
	{
		auto definition = core::resolveAgentTypeResource("standing-robot.agent.lua");
		smoke::require(bool(definition), "Standing-only Lua resource unavailable");
		std::string diagnostic;
		smoke::require(world.attachAgentType(definition->resourceName, definition->source, &diagnostic), diagnostic);
	}

	inline void refused(core::World& world, core::AgentId id, std::string const& marker)
	{
		auto* agent = world.lookupAgent(id).entity;
		auto origin = agent->getSector();
		smoke::require(world.moveAgentToNamedMarker(id, marker).accepted(), "Marker intent refused before planning");
		bool lost = false;
		for (unsigned tick = 0; tick < 1200 && !lost; ++tick)
		{
			smoke::require(world.advanceTick(), "Journey tick refused");
			smoke::require(agent->getPose() == core::Pose::Standing && agent->getSector() == origin,
				"Impossible robot passage invented a pose or crossed");
			for (auto const& event : world.consumeSimulationEvents())
				if (event.type == core::SimulationEventType::RouteLost) lost = true;
		}
		smoke::require(lost, "Impossible robot passage did not report Route loss");
		auto const& snapshot = world.getSimulationSnapshot();
		smoke::require(snapshot.traversalPermits.empty() && snapshot.traversalRequests.empty(),
			"Impossible robot passage retained admission ownership");
	}
}
