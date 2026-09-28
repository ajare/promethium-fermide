#include <iostream>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Graph.h"
#include "core/World.h"

namespace
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	void clearPausedPathDoesNotResume()
	{
		core::World world("Clear paused Path", 6, 2);
		auto corridor = world.addCorridor(0, 0, 5);
		uint32_t destination = 0;
		world.addSectorMarker(corridor, 0, 4.5f, &destination);
		world.finishBuild();
		auto id = world.createAgent("Walker", corridor, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		agent->setPath(world.getGraph()->calculatePath(agent,
			world.getGraph()->getVertexByIdentifier(destination)), true);
		require(world.advanceTicks(30), "Clear Path fixture could not advance");
		require(!world.clearAgentPath(id), "Editor Path clear accepted a running World");
		world.pauseSimulation();
		core::World::TopologyPathIntent intent;
		require(world.getPausedPathIntent(*agent, intent), "Pause did not retain destination");
		auto const position = agent->getGlobalPosition();
		require(world.clearAgentPath(id), "Paused Path could not be cleared");
		require(!agent->getPath() && !world.getPausedPathIntent(*agent, intent),
			"Clear Path left live or paused destination intent");
		require(world.resumeSimulation() && world.advanceTicks(60), "Cleared World could not resume");
		require(!agent->getPath() && agent->getGlobalPosition() == position,
			"Cleared Path resumed movement");
	}

	void pauseWalkingAgent(bool startsAtMarker)
	{
		core::World world("Pause walking", 6, 2);
		auto corridor = world.addCorridor(0, 0, 5);
		uint32_t destination = 123;
		if (startsAtMarker) world.addSectorMarker(corridor, 0, 0.5f);
		world.addSectorMarker(corridor, 0, 4.5f, &destination);
		world.finishBuild();
		auto id = world.createAgent("Walker", corridor, 0, 0.5f);
		auto agent = world.lookupAgent(id).entity;
		agent->setPath(world.getGraph()->calculatePath(agent,
			world.getGraph()->getVertexByIdentifier(destination)), true);
		require(world.advanceTicks(150), "Pause regression could not advance");

		// Starting at a Marker exercises TraversingEdge; without it, the Agent
		// is still MovingToVertex. Pause must preserve both kinds of walking.
		require(agent->getState() == (startsAtMarker ? core::Agent::State::TraversingEdge
			: core::Agent::State::MovingToVertex), "Pause regression missed its walking state");
		for (unsigned cycle = 0; cycle < 3; ++cycle)
		{
			auto before = world.getSimulationSnapshot();
			auto position = agent->getGlobalPosition();
			require(position.x > 1.7f, "Agent did not leave its starting position");
			world.pauseSimulation();
			require(agent->getGlobalPosition() == position,
				"Pausing snapped a walking Agent back to its source Marker");
			auto after = world.getSimulationSnapshot();
			require(after.paused && after.tick == before.tick
				&& after.agents.front().globalPosition == before.agents.front().globalPosition
				&& after.agents.front().sectorId == before.agents.front().sectorId,
				"Paused snapshot lost the walking Agent's position");
			require(after.traversalRequests.empty() && after.traversalPermits.empty(),
				"Pause retained stale traversal ownership");
			world.update(1.0f);
			require(agent->getGlobalPosition() == position, "Paused Agent moved on update");
			require(world.resumeSimulation(), "Walking Agent could not resume");
			require(agent->getGlobalPosition() == position, "Resume moved the Agent backwards");
			require(world.advanceTicks(30), "Resumed World refused ticks");
			require(agent->getGlobalPosition().x > position.x,
				"Resumed Agent walked back toward its old source Marker");
		}
		require(world.advanceTicks(900), "Resumed path could not finish");
		require(agent->getState() == core::Agent::State::Idle
			&& agent->getGlobalPosition().distanceTo({ 4.5f, 0.0f }) < 0.001f,
			"Paused Agent failed to reach its original destination after resuming");
	}
}

void runPausePositionSmokeChecks()
{
	clearPausedPathDoesNotResume();
	pauseWalkingAgent(true);
	pauseWalkingAgent(false);
}

// Optional full-document reproduction, independent of the bundled fixture's
// location. Re-load each time so pause does not alter the next sampled run.
void runPausePositionRepro(char const* filename)
{
	for (uint64_t ticks = 30; ticks <= 1800; ticks += 30)
	{
		auto world = core::loadWorldDocument(filename);
		require(world->resumeSimulation() && world->advanceTicks(ticks),
			"Pause repro could not run World");
		auto before = world->getSimulationSnapshot();
		world->pauseSimulation();
		auto after = world->getSimulationSnapshot();
		bool moved = false;
		for (size_t i = 0; i < before.agents.size(); ++i)
		{
			auto const& a = before.agents[i];
			auto const& b = after.agents[i];
			if (a.globalPosition.distanceTo(b.globalPosition) <= 1.0f) continue;
			std::cerr << "Pause at tick " << ticks << ": " << a.name << " ("
				<< a.globalPosition.x << ',' << a.globalPosition.y << ") -> ("
				<< b.globalPosition.x << ',' << b.globalPosition.y << ")\n";
			moved = true;
		}
		require(!moved, "Pausing moved Agents by more than one unit");
	}
}
