#include "PausePosition.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/World.h"
#include "ManifestResourceResolver.h"

namespace pause_position
{
	void require(bool value, char const* message)
	{
		if (!value) throw std::runtime_error(message);
	}

	std::filesystem::path testWorld(char const* name)
	{
		return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
			/ "resources" / "test-worlds" / name;
	}

	void pauseOnStairsPreservesPosition(std::filesystem::path const& filename, core::EdgeType edgeType)
	{
		auto world = core::loadWorldDocument(filename);
		auto agent = world->lookupAgent(core::AgentId{ 1 }).entity;
		std::shared_ptr<core::Path> path;
		std::shared_ptr<const core::Edge> stair;
		std::shared_ptr<const core::Vertex> source;
		std::shared_ptr<const core::Vertex> destination;
		for (auto const& candidate : world->getGraph()->getVertices())
		{
			auto candidatePath = world->getGraph()->calculatePath(agent, candidate);
			if (!candidatePath) continue;
			for (size_t node = 1; node < candidatePath->nodes.size(); ++node)
			{
				auto const& edge = candidatePath->nodes[node].edge;
				if (!edge || edge->getType() != edgeType) continue;
				if (edgeType == core::EdgeType::Staircase
					&& edge->getTraversalSpeed(nullptr) > 0.0f) continue;
				path = std::move(candidatePath);
				stair = edge;
				source = path->nodes[node - 1].targetVertex;
				destination = path->nodes[node].targetVertex;
				break;
			}
			if (stair) break;
		}
		require((bool)stair, "Pause fixture has no reachable stationary stair edge");
		agent->setPath(path, true);

		bool partway = false;
		for (unsigned tick = 0; tick < 600; ++tick)
		{
			require(world->advanceTicks(1), "Stair pause fixture could not advance");
			auto const position = agent->getGlobalPosition();
			if (agent->getState() == core::Agent::State::TraversingEdge
				&& position.distanceTo(source->getPosition()) > 0.02f
				&& position.distanceTo(destination->getPosition()) > 0.02f
				&& std::abs(position.distanceTo(source->getPosition())
					+ position.distanceTo(destination->getPosition()) - stair->getLength()) < 0.001f)
			{
				partway = true;
				break;
			}
		}
		require(partway, "Agent did not reach the middle of the stair edge");
		auto const beforePause = agent->getGlobalPosition();
		auto const distanceBeforeResume = beforePause.distanceTo(destination->getPosition());
		world->pauseSimulation();
		require(agent->getGlobalPosition() == beforePause,
			"Pausing moved an Agent back to the start of the stairs");
		require(world->resumeSimulation(), "Paused stair traversal could not resume");
		world->advanceTicks(agent->getRoutePlanningRemainingTicks());
		auto previousDistance = distanceBeforeResume;
		bool continued = false;
		for (unsigned tick = 0; tick < 30; ++tick)
		{
			require(world->advanceTicks(1), "Resumed stair traversal could not advance");
			auto const distance = agent->getGlobalPosition().distanceTo(destination->getPosition());
			require(distance <= previousDistance + 0.0001f,
				"Resumed Agent moved back toward the start of the stairs");
			continued = continued || distance < previousDistance - 0.0001f;
			previousDistance = distance;
		}
		require(continued, "Resumed Agent did not continue across the stairs");
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
			world.advanceTicks(agent->getRoutePlanningRemainingTicks());
			require(world.advanceTicks(30), "Resumed World refused ticks");
			require(agent->getGlobalPosition().x > position.x,
				"Resumed Agent walked back toward its old source Marker");
		}
		require(world.advanceTicks(900), "Resumed path could not finish");
		require(agent->getState() == core::Agent::State::Idle
			&& agent->getGlobalPosition().distanceTo({ 4.5f, 0.0f }) < 0.001f,
			"Paused Agent failed to reach its original destination after resuming");
	}

	void runAll()
	{
		// The bundled fixtures name their Agent tag registry Resource; this
		// standalone tool installs the same manifest resolver the editor uses.
		headless_support::installManifestCatalogResolver(
			testWorld("staircase-test-1.world.yaml").parent_path().parent_path()
				/ "Resources.yaml");
		clearPausedPathDoesNotResume();
		pauseWalkingAgent(true);
		pauseWalkingAgent(false);
		pauseOnStairsPreservesPosition(testWorld("staircase-test-1.world.yaml"),
			core::EdgeType::Staircase);
		pauseOnStairsPreservesPosition(testWorld("stairwell-test-1.world.yaml"),
			core::EdgeType::Stairwell);
	}

	// Optional full-document reproduction, independent of the bundled fixture's
	// location. Re-load each time so pause does not alter the next sampled run.
	void runRepro(char const* filename)
	{
		headless_support::installManifestCatalogResolver(
			std::filesystem::path(filename).parent_path().parent_path()
				/ "Resources.yaml");
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
}
