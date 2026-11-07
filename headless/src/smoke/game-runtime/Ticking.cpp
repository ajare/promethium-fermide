#include "Checks.h"
#include "Fixture.h"

#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/Simulation.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::findAgent;
	using game_runtime_smoke::makeWalkingWorld;

	float halfTick() { return game::WorldRuntime::getFixedTimestep() * 0.5f; }

	// A stable rendering of everything a run produced, so two runs that should
	// be identical can be compared as text.
	std::string canonical(core::SimulationSnapshot const& snapshot)
	{
		std::ostringstream out;
		out << "tick=" << snapshot.tick << " paused=" << snapshot.paused
			<< " topology=" << snapshot.topologyGeneration << '/' << snapshot.topologyValid << '\n';
		for (auto const& agent : snapshot.agents)
			out << agent.id.value << '|' << agent.name << '|' << agent.sectorId.value << '|'
				<< agent.localPosition.x << '|' << agent.localPosition.y << '|'
				<< agent.globalPosition.x << '|' << agent.globalPosition.y << '|'
				<< static_cast<int>(agent.state) << '|' << agent.hasPath << '\n';
		return out.str();
	}

	std::string digest(std::vector<core::SimulationEvent> const& events)
	{
		std::ostringstream out;
		for (auto const& event : events)
			out << event.sequence << ':' << static_cast<int>(event.type) << ':'
				<< static_cast<int>(event.phase) << ':' << event.agent.id.value << ':'
				<< event.agent.globalPosition.x << '\n';
		return out.str();
	}

	std::pair<std::string, std::string> driveOnce(std::string const& name)
	{
		auto fixture = makeWalkingWorld(name);
		game::WorldRuntime runtime(std::move(fixture.world));
		runtime.advanceTick();
		runtime.update(0.25f);
		runtime.advanceTicks(5);
		return { canonical(runtime.getSimulationSnapshot()),
			digest(runtime.consumeSimulationEvents()) };
	}
}

void registerGameRuntimeTicking(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "updateAdvancesWholeTicksOnly", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Whole ticks");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.update(halfTick());
			require(runtime.getSimulationTick() == 0,
				"a half tick executed a whole simulation tick");
			runtime.update(halfTick());
			require(runtime.getSimulationTick() == 1,
				"a full timestep of accumulated elapsed time did not execute one tick");
		} });

	checks.push_back({ "updateAdvancesTheSimulationByElapsedSeconds", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Elapsed seconds");
			game::WorldRuntime runtime(std::move(fixture.world));
			auto const start = findAgent(runtime.getSimulationSnapshot(), fixture.agent);
			require(start != nullptr, "the snapshot does not contain the fixture Agent");
			auto const startX = start->globalPosition.x;

			runtime.update(1.0f);
			require(runtime.getSimulationTick() == 60,
				"one second of elapsed time did not advance sixty fixed ticks");
			auto const moved = findAgent(runtime.getSimulationSnapshot(), fixture.agent);
			require(moved != nullptr, "the Agent disappeared from the snapshot");
			require(moved->globalPosition.x > startX + 0.01f,
				"the Agent did not move over one simulated second");
		} });

	checks.push_back({ "updateIgnoresNonPositiveAndNonFiniteElapsed", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Bad elapsed");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.update(0.0f);
			runtime.update(-1.0f);
			runtime.update(std::numeric_limits<float>::quiet_NaN());
			runtime.update(std::numeric_limits<float>::infinity());
			require(runtime.getSimulationTick() == 0,
				"a non-positive or non-finite elapsed time advanced the simulation");
		} });

	checks.push_back({ "timeScaleScalesElapsedSeconds", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Time scale");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.setSimulationTimeScale(2.0);
			require(runtime.getSimulationTimeScale() == 2.0,
				"the time scale did not read back");
			runtime.update(0.5f);
			require(runtime.getSimulationTick() == 60,
				"double time scale over half a second did not advance sixty ticks");
		} });

	checks.push_back({ "advanceTickStepsExactlyOneTick", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Advance tick");
			game::WorldRuntime runtime(std::move(fixture.world));
			for (uint64_t expected = 1; expected <= 5; ++expected)
			{
				require(runtime.advanceTick(), "advanceTick refused to step");
				require(runtime.getSimulationTick() == expected,
					"advanceTick did not step exactly one tick");
			}
		} });

	checks.push_back({ "advanceTicksStepsExactlyN", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Advance ticks");
			game::WorldRuntime runtime(std::move(fixture.world));
			require(runtime.advanceTicks(10), "advanceTicks(10) reported a stopped run");
			require(runtime.getSimulationTick() == 10, "advanceTicks(10) stepped the wrong amount");
			require(runtime.advanceTicks(0), "advanceTicks(0) reported a stopped run");
			require(runtime.getSimulationTick() == 10, "advanceTicks(0) changed the tick");
			require(runtime.advanceTicks(15), "advanceTicks(15) reported a stopped run");
			require(runtime.getSimulationTick() == 25, "the two batches did not accumulate");
		} });

	checks.push_back({ "drivingTheRuntimeIsDeterministic", [](smoke::Context const&)
		{
			auto const first = driveOnce("Determinism");
			auto const second = driveOnce("Determinism");
			require(first.first == second.first,
				"two identical drive sequences produced different snapshots");
			require(first.second == second.second,
				"two identical drive sequences produced different events");
			require(!first.first.empty() && !first.second.empty(),
				"the determinism run produced no snapshot or event content to compare");
		} });
}
