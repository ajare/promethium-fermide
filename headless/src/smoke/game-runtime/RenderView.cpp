#include "Checks.h"
#include "Fixture.h"

#include <type_traits>
#include <utility>

#include "AgentPathForRendering.h"
#include "core/Agent.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::makeWalkingWorld;

	// The render contract is enforced by the type system rather than by
	// convention: a const World cannot be driven or edited, so a render path
	// holding the view the runtime hands out physically cannot mutate what it
	// renders (#960). These fail the build, not the run.
	static_assert(!std::is_invocable_v<decltype(&core::World::update), core::World const&, float>);
	static_assert(std::is_invocable_v<decltype(&core::World::update), core::World&, float>);
	static_assert(!std::is_invocable_v<decltype(&core::World::pauseSimulation), core::World const&>);
	static_assert(!std::is_invocable_v<decltype(&core::World::resetSimulation), core::World const&>);
}

void registerGameRuntimeRenderView(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "constWorldViewFeedsTheProductionRenderReadPath", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Render view");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();

			require(view.getLayerCount() == 2, "the render view cannot read the Layer count");
			require(view.getNumSectors() == 1, "the render view cannot read the Sector count");
			require(view.getSector(0) != nullptr, "the render view cannot resolve a Sector");
			require(view.getGraph() != nullptr, "the render view has no traversal graph");

			auto const* agent = view.lookupAgent(fixture.agent).entity;
			require(agent != nullptr, "the render view cannot resolve the Agent");
			auto path = agentPathForRendering(&view, agent);
			require(path != nullptr && !path->nodes.empty(),
				"the production render read path could not obtain the Agent's route");
		} });

	checks.push_back({ "constWorldViewShowsStateDrivenThroughTheRuntime", [](smoke::Context const&)
		{
			auto fixture = makeWalkingWorld("Render liveness");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();
			auto const before = view.lookupAgent(fixture.agent).entity->getGlobalPosition();

			require(runtime.advanceTicks(60), "the run stopped before the render view could re-read");
			auto const after = view.lookupAgent(fixture.agent).entity->getGlobalPosition();
			require(after.x > before.x + 0.01f,
				"the const view did not show the movement driven through the runtime");
			require(view.getSimulationSnapshotView().tick == runtime.getSimulationTick(),
				"the snapshot read through the const view is not the current tick");
		} });
}
