#pragma once

#include <memory>
#include <string>

#include "core/Agent.h"
#include "core/Graph.h"
#include "core/Simulation.h"
#include "core/World.h"

namespace game_runtime_smoke
{
	// A built World with one active Agent walking a six-cell corridor. Every
	// input the checks need is authored before the World is handed over, because
	// nothing reachable from a WorldRuntime can set a World up afterwards: the
	// runtime drives a World, it does not build one.
	struct WalkingWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId agent{};
	};

	inline WalkingWorld makeWalkingWorld(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 7, 2);
		auto const corridor = world->addCorridor(0, 0, 6);
		uint32_t destinationVertex = 0;
		world->addSectorMarker(corridor, 0, 0.5f);
		world->addSectorMarker(corridor, 0, 5.5f, &destinationVertex);
		world->finishBuild();

		auto const agentId = world->createAgent("Runner", corridor, 0, 0.5f);
		core::Agent* agent = world->lookupAgent(agentId).entity;
		auto const graph = world->getGraph();
		agent->setPath(graph->calculatePath(agent, graph->getVertexByIdentifier(destinationVertex)),
			true);
		return WalkingWorld{ std::move(world), agentId };
	}

	// A World carrying one live entity of every kind the const read surface
	// (#963) resolves: an InteractionPoint, a DeviceOperation and a
	// TraversalResource, alongside the walking Agent. The const view cannot
	// create any of them, so they are authored before the World is handed over
	// and named here by id.
	struct ReadSurfaceWorld
	{
		std::unique_ptr<core::World> world;
		core::AgentId agent{};
		core::InteractionPointId interactionPoint{};
		core::DeviceOperationId deviceOperation{};
		core::TraversalResourceId traversalResource{};
	};

	inline ReadSurfaceWorld makeReadSurfaceWorld(std::string const& name)
	{
		auto walking = makeWalkingWorld(name);
		auto const agent = walking.agent;
		auto const point = walking.world->createInteractionPoint("Read surface control");
		auto const operation = walking.world->createDeviceOperation("Read surface operation", agent);
		auto const resource = walking.world->createTraversalResource("Read surface resource");
		return ReadSurfaceWorld{ std::move(walking.world), agent, point, operation, resource };
	}

	// Snapshots are read by identity: the runtime hands out plain data, so a
	// check looks its Agent up rather than holding a live pointer past the
	// ownership transfer.
	inline core::AgentSnapshot const* findAgent(core::SimulationSnapshot const& snapshot,
		core::AgentId id)
	{
		for (auto const& agent : snapshot.agents)
			if (agent.id == id) return &agent;
		return nullptr;
	}
}
