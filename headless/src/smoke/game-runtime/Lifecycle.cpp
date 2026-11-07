#include "Checks.h"
#include "Fixture.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Simulation.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::findAgent;
	using game_runtime_smoke::makeWalkingWorld;

	bool published(std::vector<core::SimulationEvent> const& events, core::SimulationEventType type)
	{
		return std::any_of(events.begin(), events.end(),
			[type](core::SimulationEvent const& event) { return event.type == type; });
	}
}

void registerGameRuntimeLifecycle(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "pauseStopsTheSimulation", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Pause");
			game::WorldRuntime runtime(std::move(fixture.world));
			require(runtime.advanceTicks(5), "the run stopped before it could be paused");
			require(!runtime.isSimulationPaused(), "the fixture started paused");

			runtime.pauseSimulation();
			require(runtime.isSimulationPaused(), "pauseSimulation did not pause the runtime");
			require(runtime.getSimulationSnapshotView().paused,
				"the snapshot does not report the runtime as paused");
			require(!runtime.advanceTick(), "advanceTick stepped a paused simulation");
			runtime.update(5.0f);
			require(runtime.getSimulationTick() == 5,
				"update advanced a paused simulation");

			auto const events = runtime.consumeSimulationEvents();
			require(published(events, core::SimulationEventType::SimulationPaused),
				"pausing published no SimulationPaused event");
		} });

	checks.push_back({ "resumeRestartsTheSimulation", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Resume");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.pauseSimulation();
			require(runtime.resumeSimulation(), "resumeSimulation refused a clean paused runtime");
			require(!runtime.isSimulationPaused(), "the runtime is still paused after resuming");
			require(published(runtime.consumeSimulationEvents(),
					core::SimulationEventType::SimulationResumed),
				"resuming published no SimulationResumed event");
			require(runtime.advanceTick(), "advanceTick still refused after resuming");
			require(runtime.getSimulationTick() == 1, "the tick did not advance after resuming");
			runtime.update(1.0f);
			require(runtime.getSimulationTick() == 61,
				"update did not resume advancing the simulation by elapsed time");
		} });

	checks.push_back({ "resetRestoresTheAuthoredState", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Reset");
			game::WorldRuntime runtime(std::move(fixture.world));
			auto const* authored = findAgent(runtime.getSimulationSnapshot(), fixture.agent);
			require(authored != nullptr, "the fixture Agent is missing from the authored snapshot");
			auto const authoredPosition = authored->globalPosition;

			require(runtime.advanceTicks(60), "the run stopped before the reset");
			auto const* walked = findAgent(runtime.getSimulationSnapshot(), fixture.agent);
			require(walked != nullptr, "the fixture Agent is missing after walking");
			require(std::abs(walked->globalPosition.x - authoredPosition.x) > 0.01f,
				"the Agent never moved, so the reset proves nothing");

			runtime.pauseSimulation();
			runtime.resetSimulation();
			require(runtime.isSimulationPaused(), "reset dropped the paused state");
			require(published(runtime.consumeSimulationEvents(),
					core::SimulationEventType::SimulationPaused),
				"a reset of a paused runtime published no SimulationPaused event");

			auto const restored = runtime.getSimulationSnapshot();
			require(restored.tick == 0, "reset did not restore the simulation tick");
			auto const* agent = findAgent(restored, fixture.agent);
			require(agent != nullptr, "reset dropped the Agent from the snapshot");
			require(std::abs(agent->globalPosition.x - authoredPosition.x) < 0.001f
				&& std::abs(agent->globalPosition.y - authoredPosition.y) < 0.001f,
				"reset did not restore the authored Agent position");

			require(runtime.resumeSimulation(), "resume after reset was refused");
			require(runtime.advanceTicks(30), "the reset runtime cannot tick");
			auto const* again = findAgent(runtime.getSimulationSnapshot(), fixture.agent);
			require(again != nullptr && again->globalPosition.x > agent->globalPosition.x + 0.01f,
				"the reset runtime did not walk again");
		} });

	checks.push_back({ "wakeAllAgentsKeepsTheRuntimeDriving", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Wake");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.wakeAllAgents();
			require(runtime.advanceTicks(3), "the runtime could not tick after waking its Agents");
			require(runtime.getSimulationTick() == 3, "the wake run ticked the wrong amount");
		} });
}
