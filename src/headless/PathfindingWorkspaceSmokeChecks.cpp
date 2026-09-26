#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	using PathDigest = std::vector<std::pair<uint32_t, uint32_t>>;

	PathDigest digest(std::shared_ptr<core::Path> const& path)
	{
		PathDigest result;
		for (auto const& node : path->nodes)
			result.emplace_back(node.targetVertex->getId(), std::bit_cast<uint32_t>(node.edgeWeight));
		return result;
	}

	void reusedWorkspaceIsStableAndDoesNotGrow()
	{
		core::World world("Path workspace", 40, 1);
		auto const connected = world.addCorridor(0, 0, 15);
		world.addRoom("Separator", 0, 0, 15, 2, 1);
		auto const isolated = world.addCorridor(0, 17, 12);
		uint32_t sourceId = 0;
		uint32_t destinationId = 0;
		uint32_t unreachableId = 0;
		world.addSectorMarker(connected, 0, 1.5f, &sourceId);
		world.addSectorMarker(connected, 0, 13.5f, &destinationId);
		world.addSectorMarker(isolated, 0, 4.5f, &unreachableId);
		world.finishBuild();

		auto const graph = world.getGraph();
		auto const source = graph->getVertexByIdentifier(sourceId);
		auto const destination = graph->getVertexByIdentifier(destinationId);
		auto const unreachable = graph->getVertexByIdentifier(unreachableId);
		auto const agent = world.lookupAgent(
			world.createAgent("Workspace walker", connected, 0, 1.5f)).entity;

		for (size_t slot = 0; slot < graph->getVertices().size(); ++slot)
		{
			auto const& vertex = graph->getVertices()[slot];
			require(vertex->getSearchIndex() == static_cast<uint32_t>(slot),
				"Graph Vertex does not have its deterministic dense search slot");
		}
		for (auto const& edge : graph->getEdges())
			for (uint32_t endpoint = 0; endpoint < 2; ++endpoint)
			{
				auto const vertex = edge->getVertex(endpoint);
				require(vertex->getSearchIndex() < static_cast<uint32_t>(graph->getVertices().size())
					&& graph->getVertices()[vertex->getSearchIndex()].get() == vertex.get(),
					"An Edge endpoint is absent from the Graph's dense Vertex order");
			}

		auto const first = graph->calculatePath(agent, source, destination);
		require(first && first->nodes.size() >= 2, "The workspace fixture has no connected Path");
		float cumulativeCost = 0.0f;
		for (size_t i = 1; i < first->nodes.size(); ++i)
		{
			auto const& node = first->nodes[i];
			require(node.edge != nullptr, "A non-source Path node has no Edge");
			cumulativeCost += node.edge->getWeight(node.targetVertex, agent, true);
			require(std::abs(cumulativeCost - node.edgeWeight) < 0.0001f,
				"Path cumulative cost differs from its Edge weights");
		}
		auto const expectedDigest = digest(first);
		auto const allocationsAfterWarmup = graph->getScratchAllocationCount();
		require(allocationsAfterWarmup != 0, "The first Path search did not initialise workspace scratch");

		for (uint32_t search = 0; search < 300; ++search)
		{
			auto const repeated = graph->calculatePath(agent, source, destination);
			require(repeated && digest(repeated) == expectedDigest,
				"Repeated Path searches produced different Vertex or cost sequences");
		}
		require(graph->getScratchAllocationCount() == allocationsAfterWarmup,
			"A warmed Path workspace grew while searching the same Graph");

		require(!graph->calculatePath(agent, source, unreachable),
			"A Path crossed between disconnected Sectors");
		auto const afterFailure = graph->calculatePath(agent, source, destination);
		require(afterFailure && digest(afterFailure) == expectedDigest,
			"A failed search leaked stale workspace state into the next search");
		require(graph->getScratchAllocationCount() == allocationsAfterWarmup,
			"Failed/successful searches grew a warmed Path workspace");

		auto const stationary = graph->calculatePath(agent, source, source);
		require(stationary && stationary->nodes.size() == 1
			&& stationary->nodes.front().targetVertex == source
			&& !stationary->nodes.front().edge
			&& std::abs(stationary->nodes.front().edgeWeight) < 0.0001f,
			"Source-equals-target Path behaviour changed");
	}
}

void runPathfindingWorkspaceSmokeChecks()
{
	reusedWorkspaceIsStableAndDoesNotGrow();
}
