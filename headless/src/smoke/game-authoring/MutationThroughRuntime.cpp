#include "Checks.h"
#include "Fixture.h"

#include <cstdint>
#include <string>

#include "core/Agent.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Simulation.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_authoring_smoke::findAgent;
	using game_authoring_smoke::makeCorridorWorld;
}

// The end-to-end proof the arrangement exists for (#967, spec #960 decision
// 3): a change made through WorldAuthoring is seen by the wrapped
// WorldRuntime, because there is only one World and the runtime owns it. If
// authoring held a World of its own beside the runtime, every check here
// would fail.
void registerGameAuthoringMutationThroughRuntime(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "authoringMutationIsVisibleThroughTheWrappedRuntime", [](smoke::Context const&)
		{
			auto fixture = makeCorridorWorld("Visible edit");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);
			auto const before = runtime.world().getNumSectors();
			require(before == 1, "the fixture World does not start with one Location");

			// The edit/simulation boundary the World already enforces, crossed
			// through the two handles: pause on the runtime, edit on authoring.
			runtime.pauseSimulation();
			authoring.world().addCorridor(1, 0, 4);

			require(runtime.world().getNumSectors() == before + 1,
				"the Location added through WorldAuthoring is not visible through the runtime");
			require(runtime.world().getSector(1) != nullptr,
				"the Location added through WorldAuthoring cannot be resolved through the "
				"runtime's const view");
		} });

	checks.push_back({ "structuralEditThroughAuthoringIsRefusedWhileRunning", [](smoke::Context const&)
		{
			auto fixture = makeCorridorWorld("Refused edit");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			// Authoring does not loosen the World's own guard: a structural
			// edit made while the simulation runs is refused, and refused
			// cleanly, with nothing left half-applied.
			bool refused = false;
			try
			{
				authoring.world().addCorridor(1, 0, 4);
			}
			catch (core::WorldException const&)
			{
				refused = true;
			}
			require(refused, "a structural edit went through while the simulation was running");
			require(runtime.world().getNumSectors() == 1,
				"the refused edit left part of itself applied to the World the runtime owns");

			// The same edit lands once the runtime has been paused: the refusal
			// was the running state, not the authoring handle being unable to
			// reach the World.
			runtime.pauseSimulation();
			authoring.world().addCorridor(1, 0, 4);
			require(runtime.world().getNumSectors() == 2,
				"the edit the runtime's pause should have allowed did not land");
		} });

	checks.push_back({ "authoredAgentIsSimulatedByTheWrappedRuntime", [](smoke::Context const&)
		{
			auto fixture = makeCorridorWorld("Authored agent");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			auto const agentId = authoring.world().createAgent("Runner", fixture.corridorId, 0, 0.5f);
			core::Agent* agent = authoring.world().lookupAgent(agentId).entity;
			require(agent != nullptr, "the Agent created through WorldAuthoring cannot be resolved");
			auto const graph = authoring.world().getGraph();
			require(graph != nullptr, "the authored Agent has no traversal graph to plan against");
			agent->setPath(
				graph->calculatePath(agent, graph->getVertexByIdentifier(fixture.destinationVertex)),
				true);

			require(runtime.advanceTicks(30), "the runtime stopped before the authored Agent could walk");

			auto const snapshot = runtime.getSimulationSnapshot();
			auto const* authored = findAgent(snapshot, agentId);
			require(authored != nullptr,
				"the Agent authored through WorldAuthoring never appears in the runtime's snapshot");
			require(authored->active, "the authored Agent is not being simulated by the runtime");
			require(authored->hasPath,
				"the path booked through WorldAuthoring is not on the runtime's run");
			require(authored->globalPosition.x > 0.6f,
				"the authored Agent did not move under the runtime's drive: "
				+ std::to_string(authored->globalPosition.x));
		} });

	checks.push_back({ "authoredTopologyExtendsTheGraphTheRuntimeDrives", [](smoke::Context const&)
		{
			auto fixture = makeCorridorWorld("Extended graph");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);
			auto const verticesBefore = runtime.world().getGraph()->getVertices().size();
			auto const generationBefore = runtime.getSimulationSnapshot().topologyGeneration;

			runtime.pauseSimulation();
			authoring.world().addCorridor(1, 0, 4);
			std::uint32_t authoredVertex = 0;
			authoring.world().addSectorMarker(1, 0, 1.5f, &authoredVertex);

			require(runtime.rebuildTraversalTopology(),
				"the runtime refused to rebuild the topology authoring changed");

			auto const snapshot = runtime.getSimulationSnapshot();
			require(snapshot.topologyGeneration > generationBefore,
				"the runtime rebuilt without advancing the topology the snapshot reports");
			require(!snapshot.topologyDirty,
				"the runtime still reports the authored edit as undigested");
			require(runtime.world().getGraph()->getVertices().size() > verticesBefore,
				"the graph the runtime drives did not grow for the authored Marker");
			require(runtime.world().getGraph()->getVertexByIdentifier(authoredVertex) != nullptr,
				"the Marker authored through WorldAuthoring cannot be found in the runtime's graph");
			require(runtime.resumeSimulation(),
				"the runtime cannot resume after rebuilding the authored topology");
		} });

	checks.push_back({ "rebuildIsRefusedWhileTheRuntimeIsUnpaused", [](smoke::Context const&)
		{
			auto fixture = makeCorridorWorld("Unpaused rebuild");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			// Nothing has been paused, so the rebuild the authored edit will
			// need is refused rather than run under a running simulation.
			require(!runtime.isSimulationPaused(), "the fixture World is paused before the check");
			require(!runtime.rebuildTraversalTopology(),
				"the runtime rebuilt topology without the pause the World requires");
		} });
}
