#include "Checks.h"
#include "Fixture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	// The boundary shape is a compile-time contract, not a runtime hope: the
	// runtime cannot be copied, and the only World it hands out is const. A
	// change that makes the World copyable or exposes a mutable World fails the
	// build rather than a test (#960, Testing Decisions).
	static_assert(!std::is_copy_constructible_v<game::WorldRuntime>);
	static_assert(!std::is_copy_assignable_v<game::WorldRuntime>);
	// Not movable either, and that is the half of the contract #977 added. A
	// `WorldAuthoring` caches the address of the runtime it wraps, so a
	// runtime that could move could leave that handle naming the shell it
	// moved out of - whose World is null - with nothing to detect it. The
	// moves are deleted so the stale handle is not a hazard to be careful of
	// but a shape that cannot be written; ownership transfers through a
	// pointer to the pinned runtime instead, as the check below shows.
	static_assert(!std::is_move_constructible_v<game::WorldRuntime>);
	static_assert(!std::is_move_assignable_v<game::WorldRuntime>);
	static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime const&>().world()),
		core::World const&>);
	static_assert(!std::is_same_v<decltype(std::declval<game::WorldRuntime&>().world()),
		core::World&>);
	static_assert(!std::is_same_v<decltype(std::declval<game::WorldRuntime const&>().world()),
		std::shared_ptr<core::World> const&>);

	int worldsAlive = 0;

	// Counts construction and destruction through the virtual destructor, so
	// "the runtime owns the World" is observed rather than asserted from the
	// member's declared type.
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

void registerGameRuntimeOwnership(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "runtimeTakesExclusiveOwnershipOfItsWorld", [](smoke::Context const&)
		{
			auto owned = std::make_unique<core::World>("Transferred", 4, 2);
			auto const* raw = owned.get();
			game::WorldRuntime runtime(std::move(owned));
			require(owned == nullptr, "the caller still holds the World after handing it over");
			require(&runtime.world() == raw, "world() does not name the World the runtime was given");
		} });

	checks.push_back({ "runtimeDestroysTheWorldItOwns", [](smoke::Context const&)
		{
			{
				auto counted = std::make_unique<CountedWorld>("Counted", 4, 2);
				require(worldsAlive == 1, "the World was not constructed exactly once");
				game::WorldRuntime runtime(std::move(counted));
				require(worldsAlive == 1, "wrapping the World constructed or destroyed another one");
			}
			require(worldsAlive == 0,
				"destroying the runtime did not destroy the World it owned");
		} });

	checks.push_back({ "runtimeRefusesToOwnNothing", [](smoke::Context const&)
		{
			bool refused = false;
			try
			{
				game::WorldRuntime runtime(std::unique_ptr<core::World>{});
				(void)runtime;
			}
			catch (std::invalid_argument const&)
			{
				refused = true;
			}
			require(refused, "a runtime was constructed with no World to drive");
		} });

	checks.push_back({ "ownershipTransfersThroughThePointerNotTheRuntime",
		[](smoke::Context const&)
		{
			// The runtime itself cannot be moved (#977), so exclusive ownership
			// crosses between holders through the pointer that keeps the runtime
			// pinned. The World travels; the address it is driven at does not,
			// which is exactly what an authoring handle wrapped over it needs.
			auto owned = std::make_unique<core::World>("Moved", 4, 2);
			auto const* raw = owned.get();
			auto source = std::make_unique<game::WorldRuntime>(std::move(owned));
			auto const* pinned = source.get();

			auto destination = std::move(source);
			require(source == nullptr, "the source still names a runtime after the transfer");
			require(destination.get() == pinned,
				"the transfer moved the runtime instead of the pointer to it");
			require(&destination->world() == raw, "the transfer did not carry the same World");
			require(destination->advanceTick(), "the transferred runtime cannot drive the World");
			require(destination->getSimulationTick() == 1,
				"the transferred runtime ticked the wrong amount");
		} });

	checks.push_back({ "runtimeConstructsTheWorldItOwns", [](smoke::Context const&)
		{
			game::WorldRuntime runtime("Constructed", 4, 2);
			require(runtime.world().getName() == "Constructed",
				"the constructed World does not carry the requested name");
			require(runtime.world().getLayerCount() == 2,
				"the constructed World does not have the expected Layers");
		} });
}
