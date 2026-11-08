#include "Checks.h"

#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "game/WorldRuntime.h"

// The unbound half of the resource-binding read contract (#980).
//
// `core::World` answers an absent registry reference by throwing, which is
// what the editor's save path wants: a reference that should be there and is
// not is a programming error there. The runtime is a read surface, and a
// document authored with no tag or behaviour package is a normal state, so
// the runtime answers the same question with an empty string. Before #980
// the resource-name reads were guarded and the expected-UUID reads beside
// them were not, so two adjacent reads of one document state had opposite
// failure contracts - and the unguarded one crossed the shared-library
// boundary as a `std::runtime_error` raised in another module.
//
// One check per registry pins the runtime's empty read, and one pins the
// other side of the split: the throwing forms are still on `core::World`,
// reachable through `world()`, so the editor keeps its signal without the
// runtime forwarding it.

namespace
{
	using smoke::require;

	// A built World with nothing bound to it - no tag package, no behaviour
	// package, no reference of either kind. Every read below is asked of a
	// document that legitimately has no answer to give.
	struct UnboundWorld
	{
		std::unique_ptr<core::World> world;
	};

	UnboundWorld unboundWorld(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 8, 2);
		world->addCorridor(0, 0, 6);
		world->finishBuild();
		return UnboundWorld{ std::move(world) };
	}

	// Runs a read the runtime is required not to throw. A throw is reported
	// as this check failing, naming the read and what was thrown, rather than
	// escaping and ending the run with a message about something unrelated.
	template <typename Read>
	std::string readWithoutFaulting(Read read, char const* label)
	{
		try
		{
			return std::string(read());
		}
		catch (std::exception const& fault)
		{
			throw smoke::Failure(std::string(label)
				+ " threw on a document with no reference: " + fault.what());
		}
	}

	// The same read taken again, to show the empty answer is a stable object
	// rather than a temporary that has already gone. The call sites name
	// `std::string const&` as the lambda's return type: a deduced lambda
	// return copies, which would make this check compare the addresses of two
	// dead temporaries instead of the guard's one shared empty value.
	template <typename Read>
	std::string const& readAgain(Read read, char const* label)
	{
		try
		{
			return read();
		}
		catch (std::exception const& fault)
		{
			throw smoke::Failure(std::string(label)
				+ " threw on a document with no reference: " + fault.what());
		}
	}
}

void registerGameRuntimeResourceReads(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "unboundTagRegistryReadsEmptyThroughTheRuntime",
		[](smoke::Context const&)
		{
			auto fixture = unboundWorld("Unbound tag registry");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;

			require(!surface.hasAgentTagRegistryReference(),
				"the fixture World already references an Agent tag registry");

			// The read is taken through a `WorldRuntime const&` - the shape a
			// game holds - and must answer rather than throw.
			auto const uuid = readWithoutFaulting(
				[&surface]() -> std::string const& { return surface.getExpectedAgentTagRegistryUuid(); },
				"getExpectedAgentTagRegistryUuid()");
			require(uuid.empty(),
				"an unbound Agent tag registry reported a non-empty expected UUID");

			// The name read beside it, which was already guarded, agrees - the
			// two now share one contract rather than claiming two.
			auto const name = readWithoutFaulting(
				[&surface]() -> std::string const& { return surface.agentTagRegistryResourceName(); },
				"agentTagRegistryResourceName()");
			require(name.empty(),
				"an unbound Agent tag registry reported a non-empty Resource name");

			// The guard hands out one shared empty value, so the reference a
			// caller keeps stays valid and the two reads answer identically.
			require(&readAgain([&surface]() -> std::string const& { return surface.getExpectedAgentTagRegistryUuid(); },
					"getExpectedAgentTagRegistryUuid()")
					== &readAgain([&surface]() -> std::string const& { return surface.agentTagRegistryResourceName(); },
						"agentTagRegistryResourceName()"),
				"the guarded UUID and Resource-name reads do not answer with the one shared empty value");
		} });

	checks.push_back({ "unboundBehaviourRegistryReadsEmptyThroughTheRuntime",
		[](smoke::Context const&)
		{
			auto fixture = unboundWorld("Unbound behaviour registry");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;

			require(!surface.hasAgentBehaviourRegistryReference(),
				"the fixture World already references an Agent behaviour registry");

			auto const uuid = readWithoutFaulting(
				[&surface]() -> std::string const& { return surface.getExpectedAgentBehaviourRegistryUuid(); },
				"getExpectedAgentBehaviourRegistryUuid()");
			require(uuid.empty(),
				"an unbound Agent behaviour registry reported a non-empty expected UUID");

			auto const name = readWithoutFaulting(
				[&surface]() -> std::string const& { return surface.agentBehaviourRegistryResourceName(); },
				"agentBehaviourRegistryResourceName()");
			require(name.empty(),
				"an unbound Agent behaviour registry reported a non-empty Resource name");

			require(&readAgain([&surface]() -> std::string const& { return surface.getExpectedAgentBehaviourRegistryUuid(); },
					"getExpectedAgentBehaviourRegistryUuid()")
					== &readAgain([&surface]() -> std::string const& { return surface.agentBehaviourRegistryResourceName(); },
						"agentBehaviourRegistryResourceName()"),
				"the guarded UUID and Resource-name reads do not answer with the one shared empty value");
		} });

	// The other side of the split (#980). The runtime answers empty; the
	// World the runtime owns still raises, because that is the form the
	// editor's save path reads as a programming-error signal. Nothing on
	// `WorldRuntime` forwards the raise, and nothing about the safe read
	// took the throwing read away from the code that legitimately wants it.
	checks.push_back({ "theThrowingRegistryReadsStayOnTheWorld",
		[](smoke::Context const&)
		{
			auto fixture = unboundWorld("Throwing forms stay on the World");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;
			core::World const& world = runtime.world();

			require(!world.hasAgentTagRegistryReference()
					&& !world.hasAgentBehaviourRegistryReference(),
				"the fixture World is not unbound on both registries");

			int stillThrowing = 0;
			try { (void)world.getExpectedAgentTagRegistryUuid(); }
			catch (std::runtime_error const&) { ++stillThrowing; }
			try { (void)world.getAgentTagRegistryResourceName(); }
			catch (std::runtime_error const&) { ++stillThrowing; }
			try { (void)world.getExpectedAgentBehaviourRegistryUuid(); }
			catch (std::runtime_error const&) { ++stillThrowing; }
			try { (void)world.getAgentBehaviourRegistryResourceName(); }
			catch (std::runtime_error const&) { ++stillThrowing; }
			require(stillThrowing == 4,
				"an unbound World no longer raises on the registry reads the editor's save path depends on");

			// And the runtime over that same World reads the same document
			// without raising, which is the whole difference between them.
			require(readWithoutFaulting(
					[&surface]() -> std::string const& { return surface.getExpectedAgentTagRegistryUuid(); },
					"getExpectedAgentTagRegistryUuid()")
					.empty(),
				"the runtime read did not stay empty over a World whose own read throws");
		} });
}
