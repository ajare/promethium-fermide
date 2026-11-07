#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/Simulation.h"
#include "core/World.h"

namespace game_authoring_smoke
{
	// A built World with one Corridor and a destination Marker at its far end,
	// ready to be handed to a runtime. Everything set up here is built before
	// the runtime takes ownership: the checks below change the World only
	// through WorldAuthoring, which is the arrangement under test (#967).
	struct CorridorWorld
	{
		std::unique_ptr<core::World> world;
		uint32_t corridorId = 0;
		uint32_t destinationVertex = 0;
	};

	inline CorridorWorld makeCorridorWorld(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 8, 2);
		auto const corridor = world->addCorridor(0, 0, 6);
		uint32_t destinationVertex = 0;
		world->addSectorMarker(corridor, 0, 0.5f);
		world->addSectorMarker(corridor, 0, 5.5f, &destinationVertex);
		world->finishBuild();
		return CorridorWorld{ std::move(world), corridor, destinationVertex };
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
