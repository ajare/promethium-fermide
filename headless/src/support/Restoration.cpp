#include "Restoration.h"
#include <bit>
#include <chrono>
#include <filesystem>
#include "core/AgentTagRegistryDocument.h"
#include "core/BinarySerializer.h"
#include "core/SerializationWorkData.h"
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/Agent.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "core/World.h"


namespace
{
	using PathDigest = std::vector<std::pair<uint32_t, uint32_t>>;
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}
}

void restoration_support::verify(std::filesystem::path const& input, unsigned cycles, Report report)
{
	using Clock = std::chrono::steady_clock;
	auto authored = [](core::World const& world)
	{
		auto output = core::BinarySerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*output, work);
		output->serialize();
		return output->getSerializedString();
	};
	auto paths = [](core::World const& world)
	{
		std::vector<PathDigest> result;
		for (auto const& snapshot : world.getSimulationSnapshot().agents)
		{
			PathDigest route;
			auto const path = world.lookupAgent(snapshot.id).entity->getPath();
			if (path) for (auto const& node : path->nodes)
				route.emplace_back(node.targetVertex->getSearchIndex(),
					std::bit_cast<uint32_t>(node.cumulativePerceivedCost));
			result.push_back(std::move(route));
		}
		return result;
	};
	auto trace = [](core::World& world)
	{
		uint64_t hash = 1469598103934665603ULL;
		auto mix = [&](uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
		world.resumeSimulation();
		for (unsigned tick = 0; tick < 30; ++tick)
		{
			world.update(1.0f / 60.0f);
			for (auto const& agent : world.getSimulationSnapshot().agents)
			{
				mix(agent.id.value);
				mix(std::bit_cast<uint32_t>(agent.globalPosition.x));
				mix(std::bit_cast<uint32_t>(agent.globalPosition.y));
				mix(static_cast<uint64_t>(agent.state));
				mix(agent.active);
				mix(agent.targetPathNode);
			}
		}
		return hash;
	};
	std::string expectedAuthored;
	std::vector<PathDigest> expectedPaths;
	uint64_t expectedTrace = 0;
	for (unsigned cycle = 0; cycle < cycles; ++cycle)
	{
		auto start = Clock::now();
		auto world = core::loadWorldDocument(input);
		auto reloadMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		if (cycle == 0)
		{
			expectedAuthored = authored(*world);
			expectedPaths = paths(*world);
		}
		require(authored(*world) == expectedAuthored && paths(*world) == expectedPaths,
			"Reload changed authored state or restored Paths");
		auto const replay = trace(*world);
		if (cycle == 0) expectedTrace = replay;
		require(replay == expectedTrace, "Reload changed simulation trace");
		if (cycle % 2) world->pauseSimulation();
		if (cycle % 2) world->markModified();
		else world->markSaved();
		auto const paused = world->isSimulationPaused();
		auto const modified = world->isModified();
		auto const tags = world->getAgentTagRegistry();
		auto const behaviours = world->getAgentBehaviourRegistry();
		std::weak_ptr<core::Graph const> oldGraph = world->getGraph();
		std::weak_ptr<core::Sector const> oldSector;
		if (world->getNumSectors()) oldSector = world->getSector(0);
		start = Clock::now();
		world->resetSimulation();
		auto resetMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		require(oldGraph.expired() && oldSector.expired(), "Reset retained superseded Graph or Sector");
		require(world->isSimulationPaused() == paused && world->isModified() == modified,
			"Reset changed paused or dirty state");
		require(world->getAgentTagRegistry() == tags && world->getAgentBehaviourRegistry() == behaviours,
			"Reset replaced registry references");
		require(authored(*world) == expectedAuthored, "Reset changed authored state");
		// Pausing intentionally detaches Paths into retained destination intents.
		world->resumeSimulation();
		require(paths(*world) == expectedPaths, "Reset changed restored Paths");
		require(trace(*world) == expectedTrace, "Reset changed simulation trace");
		if (report) report(cycle, reloadMs, resetMs);
		oldGraph = world->getGraph();
		if (world->getNumSectors()) oldSector = world->getSector(0);
		std::weak_ptr<core::World> oldWorld = world;
		world.reset();
		require(oldWorld.expired() && oldGraph.expired() && oldSector.expired(),
			"Reload retained World, Graph or Sector");
	}
}
