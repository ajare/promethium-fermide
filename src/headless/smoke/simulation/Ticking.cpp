#include "Checks.h"
#include "SimulationTrace.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include <algorithm>
#include <vector>

namespace
{
	using smoke::ScenarioResult;
	using smoke::canonicalResult;
	constexpr uint64_t MaximumSimulationTicks = 1000;

	bool phasesAreOrdered(std::vector<core::SimulationEvent> const& events, uint64_t ticks)
	{
		constexpr core::SimulationPhase expected[] = {
			core::SimulationPhase::ResourceAdvancement,
			core::SimulationPhase::IntentCollection,
			core::SimulationPhase::Allocation,
			core::SimulationPhase::Movement,
			core::SimulationPhase::Commit,
			core::SimulationPhase::CleanupAndEventPublication
		};

		uint64_t phaseEventCount = 0;
		for (auto const& event : events)
		{
			if (event.type != core::SimulationEventType::PhaseCompleted)
			{
				continue;
			}
			if (event.phase != expected[phaseEventCount % std::size(expected)])
			{
				return false;
			}
			++phaseEventCount;
		}
		return phaseEventCount == ticks * std::size(expected);
	}

	bool accumulatedRenderTimeAdvancesWholeTicksOnly()
	{
		core::World world("Accumulator check", 1, 1);
		auto halfTick = core::World::getFixedTimestep() * 0.5f;
		world.update(halfTick);
		if (world.getSimulationTick() != 0)
		{
			return false;
		}
		world.update(halfTick);
		return world.getSimulationTick() == 1;
	}

	ScenarioResult runOrdinaryPathScenario()
	{
		core::World world("Headless smoke world", 7, 2);
		auto corridor = world.addCorridor(0, 0, 6);

		uint32_t sourceVertexId;
		uint32_t destinationVertexId;
		world.addSectorMarker(corridor, 0, 0.5f, &sourceVertexId);
		world.addSectorMarker(corridor, 0, 5.5f, &destinationVertexId);
		world.finishBuild();

		auto agentId = world.createAgent("Headless smoke agent", corridor, 0, 0.5f);
		auto agentLookup = world.lookupAgent(agentId);
		if (!agentLookup)
		{
			return {};
		}
		auto agent = agentLookup.entity;

		auto graph = world.getGraph();
		auto source = graph->getVertexByIdentifier(sourceVertexId);
		auto destination = graph->getVertexByIdentifier(destinationVertexId);
		auto path = graph->calculatePath(agent, source, destination);
		if (!path)
		{
			return {};
		}
		agent->setPath(path, true);

		while (agent->getState() != core::Agent::State::Idle
			&& world.getSimulationTick() < MaximumSimulationTicks)
		{
			world.advanceTick();
		}

		ScenarioResult result;
		result.snapshot = world.getSimulationSnapshot();
		result.events = world.consumeSimulationEvents();
		auto finalPosition = agent->getGlobalPosition();
		result.reachedDestination = agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(corridor).get()
			&& finalPosition.distanceTo(destination->getPosition()) < 0.001f
			&& phasesAreOrdered(result.events, result.snapshot.tick);
		return result;
	}
}

void registerTicking(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "accumulatedRenderTimeAdvancesWholeTicksOnly", [](smoke::Context const&)
		{
			smoke::require(accumulatedRenderTimeAdvancesWholeTicksOnly(), "render-time accumulation did not advance exactly one whole tick");
		} });
	checks.push_back({ "runOrdinaryPathScenario", [](smoke::Context const&)
		{
			auto first = runOrdinaryPathScenario();
			auto second = runOrdinaryPathScenario();
			smoke::require(first.reachedDestination && second.reachedDestination,
				"deterministic ordinary path scenario did not complete");
			smoke::require(canonicalResult(first) == canonicalResult(second),
				"repeated runs produced different snapshots or events");
		} });
}
