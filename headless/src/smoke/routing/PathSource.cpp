#include "Checks.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>

namespace
{

	bool inferredPathSourceDoesNotMakeAgentDoubleBack()
	{
		core::World world("Path source selection", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);
		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto source = world.getGraph()->getVertexByIdentifier(sourceVertexId);
		auto destination = world.getGraph()->getVertexByIdentifier(destinationVertexId);
		auto agentId = world.createAgent("Path source traveller", corridor, 0, 2.5f);
		auto agent = world.lookupAgent(agentId).entity;

		auto inferredPath = world.getGraph()->calculatePath(agent, destination);
		auto explicitPath = world.getGraph()->calculatePath(agent, source, destination);
		return inferredPath && inferredPath->nodes.size() == 1
			&& inferredPath->nodes.front().targetVertex->sameAs(destination)
			&& !inferredPath->nodes.front().edge
			&& inferredPath->nodes.front().cumulativePerceivedCost == 3.0f / agent->getWalkSpeed()
			&& inferredPath->nodes.front().objectiveDurationSeconds == 3.0f / agent->getWalkSpeed()
			&& explicitPath && explicitPath->nodes.size() == 2
			&& explicitPath->nodes.front().targetVertex->sameAs(source);
	}
}

void routing_smoke::registerPathSource(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "inferredPathSourceDoesNotMakeAgentDoubleBack", [](smoke::Context const&)
		{
			smoke::require(inferredPathSourceDoesNotMakeAgentDoubleBack(), "inferred path source made the agent double back");
		} });
}
