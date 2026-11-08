#include "Checks.h"
#include "Fixture.h"

#include <memory>
#include <optional>
#include <stdexcept>
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

	// The runtime the handle names is not movable, and that is what makes the
	// handle safe to hold (#977). A handle caches the runtime's address; a
	// movable runtime could move out from under it and leave the handle reading
	// a shell whose World is null. With the moves deleted there is no such
	// spelling, so the stale handle is not a thing to be careful about but a
	// thing that cannot be written.
	static_assert(!std::is_move_constructible_v<game::WorldRuntime>);
	static_assert(!std::is_move_assignable_v<game::WorldRuntime>);
	static_assert(!std::is_copy_constructible_v<game::WorldRuntime>);
	static_assert(!std::is_copy_assignable_v<game::WorldRuntime>);

	// The pair is the transfer the deleted moves make necessary, and it is
	// movable as a whole while the runtime inside it is not: the pair hands
	// over pointers, the runtime stays where it was pinned (#977).
	static_assert(!std::is_copy_constructible_v<game::AuthoringPair>);
	static_assert(!std::is_copy_assignable_v<game::AuthoringPair>);
	static_assert(std::is_move_constructible_v<game::AuthoringPair>);
	static_assert(std::is_move_assignable_v<game::AuthoringPair>);
	static_assert(std::is_constructible_v<game::AuthoringPair, std::unique_ptr<core::World>>);
	static_assert(std::is_constructible_v<game::AuthoringPair, std::unique_ptr<game::WorldRuntime>>);
	static_assert(!std::is_constructible_v<game::AuthoringPair, game::WorldRuntime>);
	static_assert(!std::is_constructible_v<game::AuthoringPair, core::World>);

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

	checks.push_back({ "movedFromAuthoringHandleFailsLoudly", [](smoke::Context const&)
		{
			// The mirror image of the stranded handle: a handle moved out from
			// under a holder names no runtime and owns no history. Every way
			// back into the document from it has to say so instead of taking a
			// null pointer as an answer (#978).
			auto fixture = game_authoring_smoke::makeCorridorWorld("Moved-from handle");
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldAuthoring source(runtime);
			game::WorldAuthoring live(std::move(source));

			require(&live.runtime() == &runtime,
				"the moved-to handle no longer names the runtime it was given");

			int failures = 0;
			try { (void)source.world(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.runtime(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.document(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.history(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.canUndo(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.isModified(); }
			catch (std::logic_error const&) { ++failures; }
			try { source.commitEdit(std::nullopt); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)source.undo(); }
			catch (std::logic_error const&) { ++failures; }
			require(failures == 8,
				"a moved-from authoring handle answered " + std::to_string(8 - failures) +
				" call(s) from behind a null pointer instead of failing");

			// Adopting into, or from, an empty handle would leave one of the two
			// with no history at all, so it is refused rather than performed.
			bool refused = false;
			try { source.adoptHistoryFrom(live); }
			catch (std::logic_error const&) { refused = true; }
			require(refused, "a moved-from handle adopted a history it cannot own");
			refused = false;
			try { live.adoptHistoryFrom(source); }
			catch (std::logic_error const&) { refused = true; }
			require(refused, "a live handle adopted the null history of a moved-from one");
		} });

	checks.push_back({ "movedFromAuthoringPairFailsLoudly", [](smoke::Context const&)
		{
			// The pair is the one value in this boundary that still moves, and a
			// moved-from pair holds neither the runtime nor the handle. Both
			// accessors have to say so rather than hand back a null for the
			// caller to dereference three frames later (#978).
			auto fixture = game_authoring_smoke::makeCorridorWorld("Moved-from pair");
			game::AuthoringPair source(std::move(fixture.world));
			game::AuthoringPair live(std::move(source));
			require(&live.authoring().runtime() == &live.runtime(),
				"the moved-to pair does not name one runtime on both sides");

			int failures = 0;
			std::string message;
			try { (void)source.runtime(); }
			catch (std::logic_error const& error) { ++failures; message = error.what(); }
			try { (void)source.authoring(); }
			catch (std::logic_error const& error) { ++failures; message = error.what(); }
			require(failures == 2,
				"a moved-from pair answered " + std::to_string(2 - failures) +
				" accessor(s) from behind a null pointer instead of failing");
			require(message.find("AuthoringPair::") != std::string::npos,
				"the moved-from failure does not name the type: " + message);
			require(message.find("moved from") != std::string::npos,
				"the moved-from failure does not name the condition: " + message);

			// Move-assignment empties the source the same way, so the pair that
			// was replaced cannot be driven by mistake.
			game::AuthoringPair replacement(std::make_unique<core::World>("Replaced", 4, 1));
			live = std::move(replacement);
			failures = 0;
			try { (void)replacement.runtime(); }
			catch (std::logic_error const&) { ++failures; }
			try { (void)replacement.authoring(); }
			catch (std::logic_error const&) { ++failures; }
			require(failures == 2,
				"the pair emptied by move-assignment still answers its accessors");
			require(live.runtime().world().getName() == "Replaced",
				"the reassigned pair is not driving the World it took");
		} });

	checks.push_back({ "transferredPairKeepsTheHandleWiredToItsRuntime", [](smoke::Context const&)
		{
			// The legitimate transfer (#977): the pair moves, the runtime it
			// pinned does not, and the handle it carries is still the handle
			// that was wired to that runtime when the World was taken.
			auto fixture = game_authoring_smoke::makeCorridorWorld("Transferred pair");
			game::AuthoringPair source(std::move(fixture.world));
			auto const* pinned = &source.runtime();

			game::AuthoringPair transferred(std::move(source));
			require(&transferred.runtime() == pinned,
				"the transfer moved the runtime instead of the pointer to it");
			require(&transferred.authoring().runtime() == pinned,
				"the transferred handle no longer names the runtime the pair owns");
			require(&transferred.authoring().world() == &transferred.runtime().world(),
				"the transferred handle authors a different World than its runtime reads");

			// Drive through the runtime, read it back through the handle, then
			// author through the handle and read it back through the runtime:
			// one document on both sides of the transfer, which is the property
			// the deleted runtime moves exist to guarantee.
			require(transferred.runtime().advanceTicks(4),
				"the transferred runtime cannot be driven");
			require(transferred.authoring().world().getSimulationTick() == 4,
				"the transferred handle is not looking at the tick its own runtime drove");
			transferred.runtime().pauseSimulation();
			require(transferred.authoring().addRoom("After the transfer", 1, 0, 0, 4, 1) != 0u,
				"the transferred handle cannot author the World");
			require(transferred.runtime().world().getNumSectors() == 2,
				"runtime().world() did not read the Room the transferred handle added");

			// Move-assignment transfers the same way, and the pair that was
			// replaced is gone rather than half-replaced.
			game::AuthoringPair replacement(std::make_unique<core::World>("Replacement", 4, 1));
			replacement = std::move(transferred);
			require(&replacement.authoring().runtime() == pinned,
				"move-assignment lost the runtime the pair had pinned");
			require(replacement.runtime().world().getNumSectors() == 2,
				"the reassigned pair is not driving the World the handle authored");
		} });

	checks.push_back({ "emptyAuthoringPairIsRefusedAtTheDoor", [](smoke::Context const&)
		{
			// A pair with nothing to pair is as unconstructible as a runtime
			// with nothing to drive, so no accessor on it can be asked a
			// question with no thing behind it.
			bool refused = false;
			try { game::AuthoringPair pair(std::unique_ptr<game::WorldRuntime>{}); }
			catch (std::invalid_argument const&) { refused = true; }
			require(refused, "an AuthoringPair was constructed with no runtime to pair");

			refused = false;
			try { game::AuthoringPair pair(std::unique_ptr<core::World>{}); }
			catch (std::invalid_argument const&) { refused = true; }
			require(refused, "an AuthoringPair was constructed with no World to own");
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
