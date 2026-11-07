// The game's whole link line, compiled as a contract (#967, spec #960).
//
// This translation unit is built against pf-game-runtime and nothing else, so
// it sees exactly what the game sees: the runtime and the core types the
// runtime exposes as boundary types. Authoring is not linked here, which is
// what makes this the compile-time half of "a game linking only
// pf-game-runtime cannot reach mutable World state".
//
// It is an object target with no runner: the contract is proved by compiling,
// the same way `InteractionApiCompileCheck.cpp` proves the interaction API
// contract. The functions below have external linkage so they are compiled
// rather than warned away as unused; nothing calls them.

#include "game/WorldRuntime.h"

#include <memory>
#include <type_traits>

// The link line itself is part of the contract: this target links
// pf-game-runtime alone, so the authoring header is not even findable from
// here. If it ever becomes findable, the game's closure has grown authoring,
// which is precisely the change this assertion exists to catch.
#if defined(__has_include)
#if __has_include("game/WorldAuthoring.h")
#error "The game-only link contract must not be able to reach game/WorldAuthoring.h: authoring does not belong in a game's link closure (#967)."
#endif
#endif

// The only World a runtime hands out is const, whichever cv-qualification the
// runtime itself has. There is no mutable overload to reach for.
static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime&>().world()),
	core::World const&>);
static_assert(std::is_same_v<decltype(std::declval<game::WorldRuntime const&>().world()),
	core::World const&>);

// A `core::World const&` cannot be turned back into a mutable one by ordinary
// means, so holding the runtime's view is holding a read-only World.
static_assert(!std::is_convertible_v<core::World const&, core::World&>);
static_assert(!std::is_same_v<core::World const&, core::World&>);

// The runtime is neither copyable nor shareable, so a game cannot hand a
// second holder the World it drives, and a World cannot be injected beside
// the runtime that owns it.
static_assert(!std::is_copy_constructible_v<game::WorldRuntime>);
static_assert(!std::is_copy_assignable_v<game::WorldRuntime>);
static_assert(!std::is_constructible_v<game::WorldRuntime, core::World&>);
static_assert(!std::is_constructible_v<game::WorldRuntime, core::World*>);
static_assert(!std::is_constructible_v<game::WorldRuntime, std::shared_ptr<core::World>>);

namespace game_only_contract
{
	// What a game does with its runtime, in the shape a game does it: read
	// through the const view, drive through the runtime's own methods.
	void readsThroughTheConstView(game::WorldRuntime& runtime)
	{
		core::World const& world = runtime.world();
		static_assert(std::is_same_v<decltype(world), core::World const&>);

		// Reads reach the state the game renders.
		(void)world.getName();
		(void)world.getLayerCount();
		(void)world.getNumSectors();

		// The drive and structural-edit paths are not reachable through the
		// const view: each of those compiles only against a mutable World,
		// which a game no longer has.
		static_assert(!std::is_invocable_v<decltype(&core::World::update), core::World const&, float>);
		static_assert(std::is_invocable_v<decltype(&core::World::update), core::World&, float>);
		static_assert(!std::is_invocable_v<decltype(&core::World::resetSimulation),
			core::World const&>);
		static_assert(!std::is_invocable_v<decltype(&core::World::finishBuild), core::World const&>);
		static_assert(!std::is_invocable_v<decltype(&core::World::rebuildTraversalTopology),
			core::World const&>);
	}

	// The runtime's own drive surface stays reachable, so the contract above
	// constrains the World view rather than crippling the game.
	void drivesThroughTheRuntime(game::WorldRuntime& runtime)
	{
		runtime.update(1.0f / 60.0f);
		(void)runtime.advanceTick();
		(void)runtime.getSimulationTick();
		(void)runtime.consumeSimulationEvents();
	}
}
