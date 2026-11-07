#include "Checks.h"
#include "Fixture.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// The wrapping is a compile-time contract before it is a runtime one. The
	// only thing a WorldAuthoring can be built from is a WorldRuntime: there is
	// no constructor taking a World, a World pointer, or an owned World, so
	// "authoring wraps the runtime rather than owning a World beside it" is
	// enforced by the type rather than by review (#967, spec #960 decision 3).
	static_assert(std::is_constructible_v<game::WorldAuthoring, game::WorldRuntime&>);
	static_assert(!std::is_constructible_v<game::WorldAuthoring, game::WorldRuntime const&>);
	static_assert(!std::is_constructible_v<game::WorldAuthoring, core::World&>);
	static_assert(!std::is_constructible_v<game::WorldAuthoring, core::World*>);
	static_assert(!std::is_constructible_v<game::WorldAuthoring, std::unique_ptr<core::World>>);

	// The handle is movable and never copied: two independently held copies
	// would suggest two privileges over one World.
	static_assert(!std::is_copy_constructible_v<game::WorldAuthoring>);
	static_assert(!std::is_copy_assignable_v<game::WorldAuthoring>);
	static_assert(std::is_move_constructible_v<game::WorldAuthoring>);
	static_assert(std::is_move_assignable_v<game::WorldAuthoring>);

	// Authoring hands out a mutable World; the runtime it wraps does not. The
	// pair is the boundary: one mutable surface, one const one.
	static_assert(std::is_same_v<decltype(std::declval<game::WorldAuthoring&>().world()),
		core::World&>);
	static_assert(!std::is_same_v<decltype(std::declval<game::WorldAuthoring&>().world()),
		core::World const&>);
	static_assert(std::is_same_v<decltype(std::declval<game::WorldAuthoring&>().runtime()),
		game::WorldRuntime&>);
	static_assert(std::is_same_v<decltype(std::declval<game::WorldAuthoring const&>().runtime()),
		game::WorldRuntime const&>);
	static_assert(!std::is_same_v<decltype(std::declval<game::WorldRuntime&>().world()),
		core::World&>);

	int worldsAlive = 0;

	// Counts construction and destruction through the virtual destructor, so
	// "wrapping the runtime adds no World" is observed rather than inferred
	// from the member's declared type.
	class CountedWorld : public core::World
	{
	public:
		CountedWorld(std::string const& name, std::uint32_t cellsWide, std::uint32_t levelsHigh)
			: core::World(name, cellsWide, levelsHigh)
		{
			++worldsAlive;
		}

		~CountedWorld() override { --worldsAlive; }
	};
}

void registerGameAuthoringWrapping(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "authoringWrapsTheRuntimeItIsGiven", [](smoke::Context const&)
		{
			auto fixture = game_authoring_smoke::makeCorridorWorld("Wrapped");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			require(&authoring.runtime() == &runtime,
				"runtime() does not name the runtime the authoring handle was given");
			require(&authoring.world() == &runtime.world(),
				"authoring reaches a different World than the one the runtime owns");
		} });

	checks.push_back({ "authoringCreatesNoSecondWorld", [](smoke::Context const&)
		{
			{
				auto counted = std::make_unique<CountedWorld>("Counted", 8, 2);
				counted->finishBuild();
				require(worldsAlive == 1, "the World was not constructed exactly once");
				game::WorldRuntime runtime(std::move(counted));
				require(worldsAlive == 1, "the runtime constructed another World");
				game::WorldAuthoring authoring(runtime);
				require(worldsAlive == 1,
					"wrapping the runtime in an authoring handle constructed a second World");
				require(&authoring.world() == &runtime.world(),
					"the authoring handle is not looking at the runtime's World");
			}
			require(worldsAlive == 0,
				"destroying the runtime and its authoring handle left a World alive");
		} });

	checks.push_back({ "authoringHandleMovesWithoutRetargeting", [](smoke::Context const&)
		{
			auto fixture = game_authoring_smoke::makeCorridorWorld("Moved handle");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring source(runtime);

			game::WorldAuthoring moved(std::move(source));
			require(&moved.runtime() == &runtime, "the moved handle no longer names the same runtime");
			require(&moved.world() == &runtime.world(), "the moved handle no longer sees the same World");

			game::WorldAuthoring assigned(runtime);
			assigned = std::move(moved);
			require(&assigned.runtime() == &runtime,
				"move-assignment lost the runtime the handle was wrapping");
			require(assigned.world().getNumSectors() == 1,
				"the reassigned handle is not looking at the World the runtime owns");
		} });

	checks.push_back({ "previewRuntimeHandedBackByAuthoringIsDrivable", [](smoke::Context const&)
		{
			auto fixture = game_authoring_smoke::makeCorridorWorld("Preview");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			// The editor renders and drives through the runtime it takes back
			// from its authoring handle: the preview is the game's own path.
			game::WorldRuntime& preview = authoring.runtime();
			require(preview.advanceTicks(5), "the handed-back runtime cannot drive the World");
			require(preview.getSimulationTick() == 5,
				"the handed-back runtime ticked the wrong amount");
			require(preview.world().getNumSectors() == authoring.world().getNumSectors(),
				"the preview view and the authoring view disagree about the World");
		} });

	checks.push_back({ "authoringViewStaysInStepWithTheRuntimeAcrossATick", [](smoke::Context const&)
		{
			auto fixture = game_authoring_smoke::makeCorridorWorld("In step");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring authoring(runtime);

			require(authoring.world().getSimulationTick() == runtime.getSimulationTick(),
				"the authoring view and the runtime start out disagreeing about the tick");
			require(runtime.advanceTicks(3), "the runtime stopped before the authoring view could re-read");
			require(authoring.world().getSimulationTick() == 3,
				"the authoring view did not see the tick the runtime drove");
			require(authoring.world().getSimulationTick() == runtime.getSimulationTick(),
				"the authoring view and the runtime drifted apart across the tick");
		} });
}
