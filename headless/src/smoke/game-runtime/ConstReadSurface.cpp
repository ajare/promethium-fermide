#include "Checks.h"
#include "Fixture.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/Coordination.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Defines.h"
#include "core/World.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
	using game_runtime_smoke::makeReadSurfaceWorld;
	using game_runtime_smoke::makeFullReadSurfaceWorld;

	// #963 asks that the six read-shaped World methods be reachable through a
	// `core::World const&`, and that each be confirmed logically const rather than
	// merely re-labelled. Two shapes answer that, and both are pinned at compile
	// time here so a later edit that drops a const path breaks the build instead
	// of quietly narrowing the read surface:
	//
	//   * `getFixedTimestep`, `getMaxTicksPerUpdate` and
	//     `isValidTransportStopShortName` are static. They read no World state at
	//     all — the first two are compile-time constants of the simulation, the
	//     third validates its argument alone — so there is no `this` to
	//     cv-qualify, and a const World reference reaches them exactly as a
	//     mutable one does.
	//   * `lookupInteractionPoint`, `lookupDeviceOperation` and
	//     `lookupTraversalResource` carry a `const` overload that answers with a
	//     const entity, so the read view cannot be widened back into a mutable
	//     handle by way of a lookup.
	//
	// The runtime checks below then show the const path actually resolves live
	// entities and leaves no trace on the World, which is the part a type
	// assertion cannot prove on its own.

	// --- The three statics: no object, therefore nothing to mutate ----------
	static_assert(std::is_invocable_v<decltype(core::World::getFixedTimestep)>);
	static_assert(std::is_invocable_v<decltype(core::World::getMaxTicksPerUpdate)>);
	static_assert(std::is_invocable_v<decltype(core::World::isValidTransportStopShortName), std::string_view>);
	static_assert(std::is_same_v<decltype(std::declval<core::World const&>().getFixedTimestep()), float>);
	static_assert(std::is_same_v<decltype(std::declval<core::World const&>().getMaxTicksPerUpdate()), std::uint32_t>);
	static_assert(std::is_same_v<decltype(std::declval<core::World const&>().isValidTransportStopShortName("AB")), bool>);

	// The same statics are reachable through the runtime with no object at all
	// (#981). The forward is `static` on both sides, so a game holding nothing
	// but the type can validate a Stop label or a candidate World size - and
	// reaching them cannot hand it a World to write to, because there is no
	// World behind the call. This is the pin that was missing: the World-side
	// assertions above proved the read existed, and nothing proved the runtime
	// answered it, which is how #960's sixth method stayed unreachable.
	static_assert(std::is_invocable_v<
		decltype(game::WorldRuntime::isValidTransportStopShortName), std::string_view>);
	// The defaulted `diagnostic` argument is only visible through the name, not
	// through the function type, so the three-argument spelling is pinned with
	// a requires-expression rather than `is_invocable_v`.
	static_assert(requires {
		game::WorldRuntime::dimensionsAreSupported(16u, 4u, 2u);
	});
	static_assert(std::is_invocable_v<
		decltype(game::WorldRuntime::dimensionsAreSupported),
		std::uint32_t, std::uint32_t, std::uint32_t, std::string*>);
	static_assert(std::is_same_v<
		decltype(game::WorldRuntime::isValidTransportStopShortName("AB")), bool>);
	static_assert(std::is_same_v<
		decltype(game::WorldRuntime::dimensionsAreSupported(16u, 4u, 2u, nullptr)), bool>);

	// --- The three lookups: the const overload is the const view's only one ---
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().lookupInteractionPoint(std::declval<core::InteractionPointId>())),
		core::EntityLookup<core::InteractionPoint const>>);
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().lookupDeviceOperation(std::declval<core::DeviceOperationId>())),
		core::EntityLookup<core::DeviceOperation const>>);
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().lookupTraversalResource(std::declval<core::TraversalResourceId>())),
		core::EntityLookup<core::TraversalResource const>>);

	// The mutable overloads stay out of reach of a const World: overload
	// resolution against `World const&` cannot select a non-const member, so a
	// caller holding the render view cannot pick the writable handle.
	constexpr auto mutableInteractionPointLookup =
		static_cast<core::EntityLookup<core::InteractionPoint>(core::World::*)(core::InteractionPointId)>(
			&core::World::lookupInteractionPoint);
	constexpr auto mutableDeviceOperationLookup =
		static_cast<core::EntityLookup<core::DeviceOperation>(core::World::*)(core::DeviceOperationId)>(
			&core::World::lookupDeviceOperation);
	constexpr auto mutableTraversalResourceLookup =
		static_cast<core::EntityLookup<core::TraversalResource>(core::World::*)(core::TraversalResourceId)>(
			&core::World::lookupTraversalResource);

	static_assert(!std::is_invocable_v<decltype(mutableInteractionPointLookup), core::World const&, core::InteractionPointId>);
	static_assert(!std::is_invocable_v<decltype(mutableDeviceOperationLookup), core::World const&, core::DeviceOperationId>);
	static_assert(!std::is_invocable_v<decltype(mutableTraversalResourceLookup), core::World const&, core::TraversalResourceId>);

	// --- #974: the three reads that used to widen the const view ----------
	// `getAgentAtPosition` and the two registry getters were faithful mirrors
	// of `World`'s own leaky `const` signatures, so a caller holding nothing
	// but a `WorldRuntime const&` could write an `Agent` and both registries.
	// Each is pinned three ways: reachable through the const view, answering
	// the `const` entity type, and unable to answer the mutable type it used
	// to return.
	//
	// The third pin is the one that closes the hole. `Agent const*` does not
	// convert back to `Agent*`, and `shared_ptr<T const>` does not convert
	// back to `shared_ptr<T>`, so the widening the const view used to allow is
	// now a compile error rather than a convention the caller could route
	// around. That is the same shape as the mutable-lookup assertions above,
	// applied where the widening came from the return type rather than from a
	// second overload.
	static_assert(std::is_same_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentAtPosition(
			std::declval<std::uint32_t>(), std::declval<float>(), std::declval<float>())),
		core::Agent const*>);
	static_assert(std::is_same_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentTagRegistry()),
		std::shared_ptr<core::AgentTagRegistry const>>);
	static_assert(std::is_same_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentBehaviourRegistry()),
		std::shared_ptr<core::AgentBehaviourRegistry const>>);

	static_assert(!std::is_convertible_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentAtPosition(
			std::declval<std::uint32_t>(), std::declval<float>(), std::declval<float>())),
		core::Agent*>,
		"a const WorldRuntime still answers getAgentAtPosition with something that widens into a mutable Agent (#974)");
	static_assert(!std::is_convertible_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentTagRegistry()),
		std::shared_ptr<core::AgentTagRegistry>>,
		"a const WorldRuntime still answers getAgentTagRegistry with something that widens into a mutable registry (#974)");
	static_assert(!std::is_convertible_v<
		decltype(std::declval<game::WorldRuntime const&>().getAgentBehaviourRegistry()),
		std::shared_ptr<core::AgentBehaviourRegistry>>,
		"a const WorldRuntime still answers getAgentBehaviourRegistry with something that widens into a mutable registry (#974)");

	// The guarantee does not stop at the façade. `runtime.world()` is the
	// `core::World const&` rendering is handed, and it answers the same three
	// with `const` entities too - which is what makes "a render path cannot
	// mutate what it renders" true of these reads as well (#960 decision 9).
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().getAgentAtPosition(
			std::declval<std::uint32_t>(), std::declval<float>(), std::declval<float>())),
		core::Agent const*>);
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().getAgentTagRegistry()),
		std::shared_ptr<core::AgentTagRegistry const>>);
	static_assert(std::is_same_v<
		decltype(std::declval<core::World const&>().getAgentBehaviourRegistry()),
		std::shared_ptr<core::AgentBehaviourRegistry const>>);
	static_assert(!std::is_convertible_v<
		decltype(std::declval<core::World const&>().getAgentTagRegistry()),
		std::shared_ptr<core::AgentTagRegistry>>,
		"a const World still answers getAgentTagRegistry with something that widens into a mutable registry (#974)");
	static_assert(!std::is_convertible_v<
		decltype(std::declval<core::World const&>().getAgentBehaviourRegistry()),
		std::shared_ptr<core::AgentBehaviourRegistry>>,
		"a const World still answers getAgentBehaviourRegistry with something that widens into a mutable registry (#974)");

	// The mutable spellings of all three still exist on a mutable `World`, so
	// nothing was deleted - only moved behind the privilege that needs it.
	// Naming them as pointer-to-member is what makes the unreachability claim
	// checkable rather than merely asserted: overload resolution against a
	// `World const&` cannot select any of them, which is the same pin #963
	// uses for the mutable entity lookups above.
	constexpr auto mutableAgentAtPosition =
		static_cast<core::Agent*(core::World::*)(std::uint32_t, float, float)>(
			&core::World::getAgentAtPosition);
	constexpr auto mutableTagRegistryGetter =
		static_cast<std::shared_ptr<core::AgentTagRegistry> const& (core::World::*)()>(
			&core::World::getAgentTagRegistry);
	constexpr auto mutableBehaviourRegistryGetter =
		static_cast<std::shared_ptr<core::AgentBehaviourRegistry> const& (core::World::*)()>(
			&core::World::getAgentBehaviourRegistry);

	static_assert(!std::is_invocable_v<decltype(mutableAgentAtPosition), core::World const&, std::uint32_t, float, float>);
	static_assert(!std::is_invocable_v<decltype(mutableTagRegistryGetter), core::World const&>);
	static_assert(!std::is_invocable_v<decltype(mutableBehaviourRegistryGetter), core::World const&>);

	// Everything a const read could plausibly disturb, read in one pass. The
	// snapshot view is touched last so a rebuild happens inside the fingerprint:
	// the cache is mutable and rebuilding it is the established const-read
	// pattern, but a read that *invalidated* it would show up as a build-count
	// change against the warmed baseline.
	struct Fingerprint
	{
		bool modified{ false };
		std::uint64_t tick{ 0 };
		std::uint64_t snapshotTick{ 0 };
		std::uint64_t snapshotBuilds{ 0 };
		std::size_t events{ 0 };
		float agentX{ 0.0f };
		std::array<int, 3> resolved{ { 0, 0, 0 } };

		bool operator==(Fingerprint const& other) const
		{
			return modified == other.modified && tick == other.tick && snapshotTick == other.snapshotTick
				&& snapshotBuilds == other.snapshotBuilds && events == other.events
				&& agentX == other.agentX && resolved == other.resolved;
		}
	};

	Fingerprint fingerprint(core::World const& view, core::AgentId agent,
		core::InteractionPointId point, core::DeviceOperationId operation,
		core::TraversalResourceId resource)
	{
		Fingerprint state;
		state.modified = view.isModified();
		state.tick = view.getSimulationTick();
		state.events = view.getSimulationEvents().size();
		state.agentX = view.lookupAgent(agent).entity->getGlobalPosition().x;
		state.resolved = { {
			view.lookupInteractionPoint(point) ? 1 : 0,
			view.lookupDeviceOperation(operation) ? 1 : 0,
			view.lookupTraversalResource(resource) ? 1 : 0 } };
		state.snapshotTick = view.getSimulationSnapshotView().tick;
		state.snapshotBuilds = view.getSimulationSnapshotBuildCount();
		return state;
	}
}

void registerGameRuntimeConstReadSurface(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "constWorldReportsTheStaticReads", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Const static reads");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();

			require(view.getFixedTimestep() == 1.0f / 60.0f,
				"the const view does not report the fixed simulation timestep");
			require(view.getMaxTicksPerUpdate() == 600,
				"the const view does not report the per-update Tick bound");

			require(view.isValidTransportStopShortName("A"),
				"the const view rejects a one-character Stop name");
			require(view.isValidTransportStopShortName("A1Z"),
				"the const view rejects a three-character alphanumeric Stop name");
			require(!view.isValidTransportStopShortName(""),
				"the const view accepts an empty Stop name");
			require(!view.isValidTransportStopShortName("ABCD"),
				"the const view accepts a four-character Stop name");
			require(!view.isValidTransportStopShortName("ab"),
				"the const view accepts a lowercase Stop name");
			require(!view.isValidTransportStopShortName("A-"),
				"the const view accepts a punctuated Stop name");
		} });

	checks.push_back({ "runtimeReportsTheStaticReads", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Runtime static reads");
			game::WorldRuntime runtime(std::move(fixture.world));

			// The static half of the read surface, read through the runtime
			// rather than through a `World const&` (#981). Every answer has to
			// match the World's across the shape of the argument rather than on
			// one accepted example: a divergence would mean the forward had
			// quietly grown a rule of its own instead of forwarding one.
			static constexpr std::string_view labels[] = {
				"A", "9", "A1", "ZZZ", "ABCD", "", "ab", "A-", "A 1", "A_1", "\303\201" };
			for (auto const label : labels)
			{
				require(game::WorldRuntime::isValidTransportStopShortName(label)
						== core::World::isValidTransportStopShortName(label),
					"the Stop label read through the runtime disagrees with the World's for \""
						+ std::string(label) + "\"");
			}
			require(game::WorldRuntime::isValidTransportStopShortName("A1Z"),
				"the runtime rejects a three-character alphanumeric Stop label");
			require(!game::WorldRuntime::isValidTransportStopShortName(""),
				"the runtime accepts an empty Stop label");
			require(!game::WorldRuntime::isValidTransportStopShortName("ABCD"),
				"the runtime accepts a four-character Stop label");

			// The size rule, same agreement - and the diagnostic it writes has
			// to be the World's diagnostic, not something the forward made up.
			// The set covers each rejection the rule has, plus the cell budget
			// boundary on both sides of it.
			struct Size
			{
				std::uint32_t cellsWide;
				std::uint32_t levelsHigh;
				std::uint32_t layerCount;
				bool expected;
			};
			static constexpr Size sizes[] = {
				{ 16u, 4u, 2u, true },
				{ 0u, 4u, 2u, false },
				{ 16u, 0u, 2u, false },
				{ 16u, 4u, 1u, false },
				{ 16u, 4u, CORE_MAX_LAYERS + 1u, false },
				{ 16u, CORE_MAX_LEVELS + 1u, 2u, false },
				{ 4096u, CORE_MAX_LEVELS, 8u, true },
				{ 4096u, CORE_MAX_LEVELS, 9u, false },
				{ 0xFFFFFFFFu, 0xFFFFFFFFu, 2u, false },
			};
			for (auto const& size : sizes)
			{
				std::string throughRuntime;
				std::string throughWorld;
				auto const runtimeAnswer = game::WorldRuntime::dimensionsAreSupported(
					size.cellsWide, size.levelsHigh, size.layerCount, &throughRuntime);
				auto const worldAnswer = core::World::dimensionsAreSupported(
					size.cellsWide, size.levelsHigh, size.layerCount, &throughWorld);
				require(runtimeAnswer == worldAnswer && runtimeAnswer == size.expected,
					"the size read through the runtime answered " + std::string(runtimeAnswer ? "yes" : "no")
						+ " where the World answered " + std::string(worldAnswer ? "yes" : "no")
						+ " for " + std::to_string(size.cellsWide) + "x" + std::to_string(size.levelsHigh)
						+ " over " + std::to_string(size.layerCount) + " Layers");
				require(throughRuntime == throughWorld,
					"the size diagnostic read through the runtime differs from the World's for "
						+ std::to_string(size.cellsWide) + "x" + std::to_string(size.levelsHigh)
						+ " over " + std::to_string(size.layerCount) + " Layers");
			}
		} });

	checks.push_back({ "constWorldResolvesEveryEntityLookup", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Const entity lookups");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();

			auto const point = view.lookupInteractionPoint(fixture.interactionPoint);
			require(point && point.entity != nullptr, "the const view cannot resolve the InteractionPoint");
			require(point.diagnostic.empty(), "the const view diagnosed a live InteractionPoint");
			require(point.entity->getName() == "Read surface control",
				"the const view resolved the InteractionPoint to a different entity");

			auto const operation = view.lookupDeviceOperation(fixture.deviceOperation);
			require(operation && operation.entity != nullptr, "the const view cannot resolve the DeviceOperation");
			require(operation.diagnostic.empty(), "the const view diagnosed a live DeviceOperation");
			require(operation.entity->getName() == "Read surface operation",
				"the const view resolved the DeviceOperation to a different entity");

			auto const resource = view.lookupTraversalResource(fixture.traversalResource);
			require(resource && resource.entity != nullptr, "the const view cannot resolve the TraversalResource");
			require(resource.diagnostic.empty(), "the const view diagnosed a live TraversalResource");
			require(resource.entity->getName() == "Read surface resource",
				"the const view resolved the TraversalResource to a different entity");
		} });

	checks.push_back({ "constLookupsDiagnoseStaleHandles", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Stale const handles");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();

			auto const point = view.lookupInteractionPoint(core::InteractionPointId{ 987654 });
			require(!point, "a stale InteractionPoint handle resolved through the const view");
			require(point.entity == nullptr, "a stale InteractionPoint lookup returned an entity");
			require(!point.diagnostic.empty(), "the const view rejected a stale handle without a diagnostic");

			auto const operation = view.lookupDeviceOperation(core::DeviceOperationId{ 987654 });
			require(!operation, "a stale DeviceOperation handle resolved through the const view");
			require(!operation.diagnostic.empty(), "the const view rejected a stale handle without a diagnostic");

			auto const resource = view.lookupTraversalResource(core::TraversalResourceId{ 987654 });
			require(!resource, "a stale TraversalResource handle resolved through the const view");
			require(!resource.diagnostic.empty(), "the const view rejected a stale handle without a diagnostic");
		} });

	checks.push_back({ "constReadsLeaveNoTraceOnTheWorld", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Const read purity");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();
			require(view.lookupAgent(fixture.agent).entity != nullptr,
				"the fixture Agent is missing, so the purity fingerprint would be meaningless");

			// Warm the lazily built snapshot before baselining, so the first
			// build is not counted as a change caused by the reads under test.
			(void)view.getSimulationSnapshot();
			auto const before = fingerprint(view, fixture.agent, fixture.interactionPoint,
				fixture.deviceOperation, fixture.traversalResource);

			for (int i = 0; i < 200; ++i)
			{
				(void)view.getFixedTimestep();
				(void)view.getMaxTicksPerUpdate();
				(void)view.isValidTransportStopShortName("AB");
				(void)view.lookupInteractionPoint(fixture.interactionPoint);
				(void)view.lookupDeviceOperation(fixture.deviceOperation);
				(void)view.lookupTraversalResource(fixture.traversalResource);
			}

			auto const after = fingerprint(view, fixture.agent, fixture.interactionPoint,
				fixture.deviceOperation, fixture.traversalResource);
			require(before == after,
				"a const read changed observable World state: the read surface is not logically const");
		} });

	checks.push_back({ "constReadsTrackStateDrivenThroughTheRuntime", [](smoke::Context const&)
		{
			auto fixture = makeReadSurfaceWorld("Const reads follow the run");
			game::WorldRuntime runtime(std::move(fixture.world));
			core::World const& view = runtime.world();

			auto const before = view.lookupAgent(fixture.agent).entity->getGlobalPosition().x;
			require(runtime.advanceTicks(60), "the run stopped before the const reads could re-read");
			auto const after = view.lookupAgent(fixture.agent).entity->getGlobalPosition().x;
			require(after > before + 0.01f,
				"the const lookups did not follow the state driven through the runtime");

			// The same six reads still report the same constants after the run,
			// so the const surface is stable across ticks as well as pure.
			require(view.getFixedTimestep() == 1.0f / 60.0f,
				"the fixed timestep changed across the run");
			require(view.getMaxTicksPerUpdate() == 600,
				"the per-update Tick bound changed across the run");
			require(view.lookupInteractionPoint(fixture.interactionPoint)
				&& view.lookupDeviceOperation(fixture.deviceOperation)
				&& view.lookupTraversalResource(fixture.traversalResource),
				"an entity lookup stopped resolving across the run");
		} });

	// The compile-time half says the three reads answer `const`; this says the
	// const answer is still a real answer. Const-correcting them must not have
	// emptied the read - a render or game path holding only the runtime still
	// finds the Agent under a position and still resolves both attached
	// packages (#974).
	checks.push_back({ "constHandlesStillResolveLiveEntities", [](smoke::Context const& ctx)
		{
			auto fixture = makeFullReadSurfaceWorld("const handles live", ctx.temporaryRoot());
			game::WorldRuntime runtime(std::move(fixture.world));
			game::WorldRuntime const& surface = runtime;

			auto const position = surface.lookupAgent(fixture.agent).entity->getGlobalPosition();
			auto const* found = surface.getAgentAtPosition(0, position.x, position.y);
			require(found != nullptr,
				"the const view could not find the walking Agent under its own position");
			require(surface.getAgentId(found) == fixture.agent,
				"the const view resolved the Agent under the position to a different Agent");

			require(surface.getAgentTagRegistry() != nullptr,
				"the const view answered a null Agent tag registry for a document that has one");
			require(surface.getAgentBehaviourRegistry() != nullptr,
				"the const view answered a null Agent behaviour registry for a document that has one");
			require(surface.getAgentTagRegistry()->getUuid()
					== surface.getExpectedAgentTagRegistryUuid(),
				"the const Agent tag registry handle resolves a package other than the one the document names");
			require(surface.getAgentBehaviourRegistry()->getUuid()
					== surface.getExpectedAgentBehaviourRegistryUuid(),
				"the const Agent behaviour registry handle resolves a package other than the one the document names");

			// The const handles name the same packages the authoring side wrote,
			// so const-correcting the read did not fork the document's view of
			// its own registry.
			require(surface.getAgentTagRegistry() == runtime.world().getAgentTagRegistry()
				&& surface.getAgentBehaviourRegistry() == runtime.world().getAgentBehaviourRegistry(),
				"the const registry handles read through the runtime differ from the same read through world()");
		} });
}
