#include "RoutingPopulation.h"
#include <bit>
#include <chrono>
#include <ostream>
#include "core/Defines.h"
#include "core/AgentTagRegistry.h"
#include <filesystem>
#include "core/AgentTagRegistryDocument.h"
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Marker.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/World.h"


namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}
}

namespace routing_support
{
	uint64_t populationRoutingRun(std::filesystem::path const& output, bool verifyReset,
		std::ostream* report, size_t (*workingSetBytes)())
	{
		auto const registryPath = output.empty() ? std::filesystem::path{}
			: core::defaultAgentTagRegistryPath(output);
		if (!output.empty())
			require(!std::filesystem::exists(output) && !std::filesystem::exists(registryPath),
				"Refusing to overwrite a routing stress World or tag registry");
		core::World world("Population routing", 512, 4);
		std::vector<uint32_t> sectors;
		std::vector<uint32_t> markers;
		for (uint32_t level = 0; level < 4; ++level)
		{
			auto sector = world.addCorridor(level, 0, 512);
			sectors.push_back(sector);
			for (uint32_t x = 0; x < 500; ++x)
			{
				uint32_t marker;
				world.addSectorMarker(sector, 0, x + 0.5f,
					"Population marker " + std::to_string(level * 512 + x), &marker);
				markers.push_back(marker);
			}
		}
		for (uint32_t level = 0; level < 3; ++level)
		{
			world.addStaircase(1, level, 10 + level * 10, { 2, CORE_SIDE_RIGHT, 0.0f });
			world.addStaircase(1, level, 60 + level * 10, { 2, CORE_SIDE_RIGHT, 0.5f });
		}
		world.addLadder(1, 0, 110, { 4, false, true });
		world.addLift(1, 0, 510, 2, 4);
		world.finishBuild();
		world.pauseSimulation();
		auto registry = core::AgentTagRegistry::create();
		world.attachAgentTagRegistry(output.empty() ? "population.tags.yaml" : registryPath.filename().string(), registry);
		auto tag = registry->addAgentTag("shared-route");
		require(registry->addAgentTagEffortAversion(tag), "Could not add population tag property");
		require(registry->setAgentTagEffortAversion(tag, { 2.0f, 2.0f }), "Could not set population tag property");
		std::vector<core::Agent*> agents;
		for (uint32_t index = 0; index < 1000; ++index)
		{
			auto id = world.createAgent("Population walker", sectors[index % 4], 0, (index % 512) + 0.5f);
			if (index % 3 == 1)
				require(world.assignAgentTag(id, tag), "Could not assign population tag");
			if (index % 3 == 2)
			{
				require(world.setAgentIndividualEffortAversion(id, (index % 301) / 100.0f), "Could not set population effort");
				require(world.setAgentIndividualWalkSpeedModifier(id, 0.8f + (index % 401) / 1000.0f), "Could not set population speed");
				require(world.setAgentIndividualRiskAversion(id, (index % 301) / 100.0f), "Could not set population risk");
				require(world.setAgentIndividualWaitingAversion(id, 0.5f + (index % 251) / 100.0f), "Could not set population waiting");
				require(world.setAgentIndividualRouteFamiliarity(id, (index % 101) / 100.0f), "Could not set population familiarity");
			}
			agents.push_back(world.lookupAgent(id).entity);
		}
		auto graph = world.getGraph();
		uint64_t expected = 0;
		uint64_t allocations = 0;
		auto const memoryBeforeRouting = workingSetBytes ? workingSetBytes() : 0;
		constexpr uint32_t destinations[]{ 601, 1243, 1981, 17 };
		std::array<core::MarkerId, 4> destinationIds;
		std::array<std::shared_ptr<const core::Vertex>, 4> targets;
		for (size_t index = 0; index < targets.size(); ++index)
		{
			targets[index] = graph->getVertexByIdentifier(markers[destinations[index]]);
			auto marker = std::dynamic_pointer_cast<core::Marker>(targets[index]->getObject());
			require(marker != nullptr, "Population destination is not a Marker");
			destinationIds[index] = marker->getMarkerId();
		}
		for (uint32_t pass = 0; pass < (output.empty() && verifyReset ? 3u : 2u); ++pass)
		{
			if (pass == 2)
			{
				std::vector<core::AgentId> ids;
				for (auto agent : agents) ids.push_back(world.getAgentId(agent));
				world.resetSimulation();
				graph = world.getGraph();
				targets = {};
				for (auto const& vertex : graph->getVertices())
				{
					auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
					if (!marker) continue;
					for (size_t index = 0; index < targets.size(); ++index)
						if (marker->getMarkerId() == destinationIds[index]) targets[index] = vertex;
				}
				for (auto const& target : targets) require(target != nullptr, "Reset lost a destination Marker");
				for (size_t index = 0; index < ids.size(); ++index)
					agents[index] = world.lookupAgent(ids[index]).entity;
			}
			auto const start = report ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
			uint64_t hash = 1469598103934665603ULL;
			uint32_t traversalKinds = 0;
			for (uint32_t index = 0; index < agents.size(); ++index)
			{
				auto const& target = targets[index % targets.size()];
				// Include real source inference and virtual floor-edge splitting,
				// not just the cheaper explicit-Vertex diagnostic query.
				auto path = graph->calculatePath(agents[index], target);
				require(path != nullptr, "Population route was unreachable");
				for (auto const& node : path->nodes)
				{
					if (node.edge) traversalKinds |= 1u << static_cast<uint32_t>(node.edge->getType());
					hash = (hash ^ node.targetVertex->getSearchIndex()) * 1099511628211ULL;
					hash = (hash ^ std::bit_cast<uint32_t>(node.cumulativePerceivedCost)) * 1099511628211ULL;
				}
			}
			for (auto kind : { core::EdgeType::Lift, core::EdgeType::Ladder, core::EdgeType::Staircase })
				require((traversalKinds & (1u << static_cast<uint32_t>(kind))) != 0,
					"Population routes did not exercise a mixed set of traversal modes");
			if (report)
			{
				auto const seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
				*report << "routing-population agents=" << agents.size()
					<< " vertices=" << graph->getVertices().size() << ' ' << (pass == 2 ? "reset" : pass ? "warm" : "cold")
					<< " paths/s=" << 1000 / seconds << " scratch-bytes=" << graph->getPathfindingScratchBytes()
					<< " working-set-MiB=" << (workingSetBytes ? workingSetBytes() : 0) / (1024.0 * 1024.0)
					<< " before-routing-MiB=" << memoryBeforeRouting / (1024.0 * 1024.0)
					<< " evaluated-arcs=" << graph->getRouteWorkCounts().evaluatedArcs
					<< " prepared-arcs=" << graph->getRouteWorkCounts().preparedArcs
					<< " expanded=" << graph->getRouteWorkCounts().expandedVertices
					<< " preparation-ms=" << graph->getRouteWorkCounts().preparationSeconds * 1000
					<< " lower-bound-builds=" << graph->getRouteLowerBoundBuildCount()
					<< " lower-bound-hits=" << graph->getRouteLowerBoundHitCount()
					<< " source-index-bytes=" << graph->getSourceIndexStatistics().bytes
					<< " source-selection-ms=" << graph->getSourceIndexStatistics().selectionSeconds * 1000
					<< " source-seeding-ms=" << graph->getSourceIndexStatistics().seedingSeconds * 1000
					<< " digest=" << hash << '\n';
			}
			if (!pass) { expected = hash; allocations = graph->getScratchAllocationCount(); }
			else if (pass == 1)
			{
				require(hash == expected, "Population warm Path digest changed");
				require(allocations == graph->getScratchAllocationCount(), "Population warm scratch grew");
				require(graph->getRouteLowerBoundBuildCount() == 4
					&& graph->getRouteLowerBoundHitCount() >= 1996,
					"Population did not share target bounds across exact individual profiles");
			}
			else require(hash == expected, "World reset changed the population Path digest");
		}
		require(graph->getDirectedFactsBuildCount() == 1, "Population rebuilt immutable directed geometry");
		if (!output.empty())
		{
			registry->saveTo(registryPath.string());
			for (uint32_t index = 0; index < agents.size(); ++index)
			{
				auto const& target = targets[index % targets.size()];
				auto path = graph->calculatePath(agents[index], target);
				require(path != nullptr, "Exported population Agent has no destination Path");
				agents[index]->setPath(path, true);
			}
			world.saveTo(output.string());
			auto reopened = core::loadWorldDocument(output);
			require(reopened->getSimulationSnapshot().agents.size() == agents.size()
				&& reopened->getGraph()->getVertices().size() == graph->getVertices().size(),
				"Exported routing stress World did not round-trip with its tag registry");
		}
		return expected;
	}
}
