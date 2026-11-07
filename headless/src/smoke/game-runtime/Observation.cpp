#include "Checks.h"
#include "Fixture.h"

#include <cstdint>
#include <vector>

#include "core/Simulation.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::makeWalkingWorld;

	// Non-owning on purpose: WorldRuntime registration does not take the
	// observer, so the recorder must outlive it.
	class Recorder : public core::SimulationObserver
	{
	public:
		void onTick(uint64_t tick, std::vector<core::SimulationEvent> const& events) noexcept override
		{
			++calls;
			lastTick = tick;
			eventCount += events.size();
		}

		int calls = 0;
		uint64_t lastTick = 0;
		std::size_t eventCount = 0;
	};
}

void registerGameRuntimeObservation(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "snapshotIsACopyOfTheView", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Snapshot copy");
			game::WorldRuntime runtime(std::move(fixture.world));
			require(runtime.advanceTicks(2), "the run stopped before observation");

			auto copy = runtime.getSimulationSnapshot();
			auto const& view = runtime.getSimulationSnapshotView();
			require(copy.tick == view.tick && copy.agents.size() == view.agents.size(),
				"the snapshot copy does not match the view");

			copy.tick = 9999;
			copy.agents.clear();
			require(runtime.getSimulationSnapshotView().tick != 9999,
				"mutating the copy changed the runtime's view");
			require(!runtime.getSimulationSnapshotView().agents.empty(),
				"clearing the copy cleared the runtime's view");
		} });

	checks.push_back({ "snapshotViewIsRebuiltOnlyWhenItIsStale", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Snapshot view");
			game::WorldRuntime runtime(std::move(fixture.world));
			auto const before = runtime.getSimulationSnapshotBuildCount();

			auto const& first = runtime.getSimulationSnapshotView();
			require(runtime.getSimulationSnapshotBuildCount() == before + 1,
				"the first read did not build the snapshot");
			auto const& second = runtime.getSimulationSnapshotView();
			require(&first == &second, "the view is not the cached snapshot");
			require(runtime.getSimulationSnapshotBuildCount() == before + 1,
				"an unchanged view was rebuilt");

			require(runtime.advanceTick(), "the run stopped while ticking the view");
			auto const& afterTick = runtime.getSimulationSnapshotView();
			require(runtime.getSimulationSnapshotBuildCount() == before + 2,
				"a tick did not invalidate the cached view");
			require(afterTick.tick == runtime.getSimulationTick(),
				"the rebuilt view does not report the current tick");
		} });

	checks.push_back({ "eventsAreConsumedExactlyOnce", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Event consumption");
			game::WorldRuntime runtime(std::move(fixture.world));
			require(runtime.advanceTicks(3), "the run stopped before events were published");

			auto events = runtime.consumeSimulationEvents();
			require(events.size() > 1, "ticking published no events to consume");
			require(events.front().sequence < events.back().sequence,
				"event sequences did not increase");
			require(runtime.consumeSimulationEvents().empty(),
				"a second consume returned events that had already been delivered");

			require(runtime.advanceTick(), "the run stopped before the second batch");
			auto const next = runtime.consumeSimulationEvents();
			require(!next.empty() && next.front().sequence > events.back().sequence,
				"the second batch of events did not continue the sequence");
		} });

	checks.push_back({ "observerReceivesEveryTickAndItsEvents", [](smoke::Context const&)
		{
			Recorder recorder;
			auto fixture = makeWalkingWorld("Observer");
			game::WorldRuntime runtime(std::move(fixture.world));
			runtime.setSimulationObserver(&recorder);
			require(runtime.advanceTicks(3), "the run stopped while observing");
			require(recorder.calls == 3, "the observer was not called once per tick");
			require(recorder.lastTick == runtime.getSimulationTick(),
				"the observer's last tick is not the current tick");
			require(recorder.eventCount > 0, "the observer received no events");

			runtime.setSimulationObserver(nullptr);
			require(runtime.advanceTicks(2), "the run stopped after unregistering");
			require(recorder.calls == 3, "the observer was still called after unregistering");
		} });
}
