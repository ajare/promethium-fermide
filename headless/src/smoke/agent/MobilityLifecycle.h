#pragma once

#include "Smoke.h"
#include "core/Agent.h"
#include "core/Graph.h"
#include "core/Pathing.h"
#include "core/World.h"

namespace agent_smoke
{
	// Exercise the production route search across an authored ordinary Door,
	// not Lua storage or a synthetic edge. The fixture has no alternate passage.
	inline void requireDoorRoute(core::World const& world, core::AgentId id,
		uint32_t destinationLayer, bool permitted)
	{
		auto const* agent = world.lookupAgent(id).entity;
		smoke::require(agent != nullptr, "Missing Mobility lifecycle Agent");
		auto const& graph = world.getGraph();
		auto origin = graph->getClosestVertexInSector(agent->getSector(), { 1.f, 0.f });
		auto const sectors = world.getSectors(destinationLayer);
		smoke::require(sectors.size() == 1, "Expected one destination Location on the back Layer");
		auto target = graph->getClosestVertexInSector(sectors.front().get(), { 1.f, 0.f });
		smoke::require(origin && target && origin != target, "Missing Mobility lifecycle route endpoints");
		smoke::require(bool(core::pathing::findPath(agent, graph.get(), origin, target)) == permitted,
			"Door routing did not reflect the lifetime's effective Mobility profile");
	}

	inline void replaceSource(std::string& source, std::string const& from, std::string const& to)
	{
		auto const at = source.find(from);
		smoke::require(at != std::string::npos, "Missing Mobility fixture source field: " + from);
		source.replace(at, from.size(), to);
	}
}
