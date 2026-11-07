#include <iostream>
#include <stdexcept>
#include <string>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/Exceptions.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("RegistryBinding check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrows(Fn&& fn, std::string const& contains,
		std::string const& what)
	{
		try
		{
			fn();
		}
		catch (std::exception const& error)
		{
			require(std::string(error.what()).find(contains) != std::string::npos,
				what + " (expected '" + contains + "', got '" + error.what() + "')");
			return;
		}
		throw std::runtime_error("RegistryBinding check did not throw: " + what);
	}
}

int main()
{
	using namespace core;

	try
	{
		std::string diagnostic;

		// ================================================================
		// Agent tag family through the shared RegistryBinding.
		// ================================================================

		{
			World world("tag-binding-checks", 8, 4);
			auto const corridor = world.addCorridor(0, 0, 8);
			world.finishBuild();

			// A fresh World references and attaches nothing; the name/UUID
			// getters throw until a reference exists.
			require(!world.hasAgentTagRegistryReference(),
				"a fresh World has no tag registry reference");
			require(!world.hasAttachedAgentTagRegistry(),
				"a fresh World has no attached tag registry");
			requireThrows([&] { world.getAgentTagRegistryResourceName(); },
				"The World has no Agent tag registry reference",
				"reading the tag Resource name with no reference");
			requireThrows([&] { world.getExpectedAgentTagRegistryUuid(); },
				"The World has no Agent tag registry reference",
				"reading the expected tag UUID with no reference");

			// Resource-name shape and registry validity are refused before
			// any state changes.
			requireThrows([&] {
				world.attachAgentTagRegistry("nested/tags",
					AgentTagRegistry::create());
				}, "must be a Resource name",
				"attaching a tag registry under a path-shaped name");
			requireThrows([&] {
				world.attachAgentTagRegistry("tags", nullptr);
				}, "Cannot attach an invalid Agent tag registry",
				"attaching a null tag registry");

			auto registry = AgentTagRegistry::create();
			world.attachAgentTagRegistry("tags", registry);
			require(world.hasAgentTagRegistryReference()
				&& world.hasAttachedAgentTagRegistry(),
				"the tag registry is referenced and attached");
			require(world.getAgentTagRegistryResourceName() == "tags",
				"the tag Resource name round-trips");
			require(world.getExpectedAgentTagRegistryUuid() == registry->getUuid(),
				"the expected tag UUID matches the attached registry");
			require(registry->hasLoadedWorld(&world),
				"the World is registered with the attached tag registry");

			// An assignment makes the namespace "used": a switch is refused,
			// but the identical re-attach stays a no-op.
			world.pauseSimulation();
			auto const tag = registry->addAgentTag("walker");
			auto const agent = world.createAgent("Ada", corridor, 0, 1.0f);
			require(world.assignAgentTag(agent, tag, &diagnostic),
				"assigning a tag: " + diagnostic);
			requireThrows([&] {
				world.attachAgentTagRegistry("other-tags",
					AgentTagRegistry::create());
				}, "Cannot switch Agent tag registries while Agent tag assignments exist",
				"switching the tag registry while an assignment exists");
			world.attachAgentTagRegistry("tags", registry);
			require(world.getAgentTagAssignmentCount() == 1,
				"the identical tag re-attach preserved the assignment");

			// Plain detach refuses to strand the assignment; the confirmed
			// destructive variant clears it in the same transaction.
			requireThrows([&] { world.detachAgentTagRegistry(); },
				"Cannot detach the Agent tag registry while assignments or samples exist",
				"detaching the tag registry while an assignment exists");
			world.detachAgentTagRegistryAndClearAssignments();
			require(!world.hasAgentTagRegistryReference()
				&& !world.hasAttachedAgentTagRegistry()
				&& world.getAgentTagAssignmentCount() == 0,
				"detach-and-clear removed the namespace and the assignment");
			require(!registry->hasLoadedWorld(&world),
				"the World unregistered from the detached tag registry");

			// Resolve needs a persisted reference and the matching UUID.
			requireThrows([&] {
				world.resolveAgentTagRegistry(AgentTagRegistry::create());
				}, "The World has no Agent tag registry reference to resolve",
				"resolving a tag registry with no reference");
			world.attachAgentTagRegistry("tags", registry);
			requireThrows([&] {
				world.resolveAgentTagRegistry(AgentTagRegistry::create());
				}, "Agent tag registry UUID mismatch",
				"resolving a tag registry with the wrong UUID");
			world.resolveAgentTagRegistry(registry);
			require(world.getAgentTagRegistry() == registry
				&& registry->hasLoadedWorld(&world),
				"resolving the matching tag registry attached it");
			require(world.assignAgentTag(agent, tag, &diagnostic),
				"re-assigning a tag after resolve: " + diagnostic);

			// Save As: an equivalent independent copy swaps the namespace
			// atomically; a same-UUID or non-equivalent copy is refused.
			requireThrows([&] {
				world.replaceAgentTagRegistryWithIndependentCopy("tags", registry);
				}, "An Agent tag registry copy must have a new UUID",
				"replacing with the same-UUID tag registry");
			auto const copy = AgentTagRegistry::copyWithNewUuid(*registry);
			world.replaceAgentTagRegistryWithIndependentCopy("copied-tags", copy);
			require(world.getAgentTagRegistry() == copy
				&& world.getAgentTagRegistryResourceName() == "copied-tags"
				&& world.getExpectedAgentTagRegistryUuid() == copy->getUuid(),
				"the tag Save As copy swapped name, UUID, and registry together");
			require(copy->hasLoadedWorld(&world)
				&& !registry->hasLoadedWorld(&world),
				"the tag copy registered and the original unregistered");
			require(world.getAgentTagAssignmentCount() == 1,
				"the tag Save As preserved the assignment");

			// attachAndClearAssignments clears in one transaction.
			world.attachAgentTagRegistryAndClearAssignments("fresh-tags",
				AgentTagRegistry::create());
			require(world.getAgentTagRegistryResourceName() == "fresh-tags"
				&& world.getAgentTagAssignmentCount() == 0,
				"tag attach-and-clear replaced the namespace and cleared the assignment");
		}

		// ================================================================
		// Agent behaviour family through the shared RegistryBinding.
		// ================================================================

		{
			World world("behaviour-binding-checks", 8, 4);
			world.addCorridor(0, 0, 8);
			world.finishBuild();

			// The behaviour transaction is paused-only: the gate fires
			// before the Resource-name check.
			requireThrows([&] {
				world.attachAgentBehaviourRegistry("package",
					AgentBehaviourRegistry::create());
				}, "Pause the World before changing its Agent behaviour registry",
				"attaching a behaviour registry while running");
			require(!world.hasAgentBehaviourRegistryReference(),
				"a refused behaviour attach left no reference");

			world.pauseSimulation();
			requireThrows([&] {
				world.attachAgentBehaviourRegistry("pkg/module",
					AgentBehaviourRegistry::create());
				}, "must be a Resource name",
				"attaching a behaviour registry under a path-shaped name");
			requireThrows([&] {
				world.attachAgentBehaviourRegistry("package", nullptr);
				}, "Cannot attach an invalid Agent behaviour registry",
				"attaching a null behaviour registry");

			auto registry = AgentBehaviourRegistry::create();
			world.attachAgentBehaviourRegistry("package", registry);
			require(world.hasAgentBehaviourRegistryReference()
				&& world.hasAttachedAgentBehaviourRegistry(),
				"the behaviour registry is referenced and attached");
			require(world.getAgentBehaviourRegistryResourceName() == "package"
				&& world.getExpectedAgentBehaviourRegistryUuid() == registry->getUuid(),
				"the behaviour Resource name and expected UUID round-trip");
			require(registry->hasLoadedWorld(&world),
				"the World is registered with the attached behaviour registry");
			require(world.agentBehaviourConfigurationsAreValid(),
				"a fresh behaviour attachment has no dependency diagnostic");

			// The identical re-attach is a no-op while paused; the pause gate
			// still fires first while running, even for the identical pair.
			world.attachAgentBehaviourRegistry("package", registry);
			require(world.getAgentBehaviourRegistry() == registry,
				"the identical behaviour re-attach kept the registry while paused");
			require(world.resumeSimulation() && !world.isSimulationPaused(),
				"the World resumed for the running-state checks");
			requireThrows([&] {
					world.attachAgentBehaviourRegistry("package", registry);
					}, "Pause the World before changing its Agent behaviour registry",
					"the pause gate fires before the identical re-attach no-op");

			// A plain detach while running is refused on the pause gate;
			// the unused registry then detaches cleanly once paused.
			requireThrows([&] {
				world.detachAgentBehaviourRegistry();
				}, "Pause the World before detaching its Agent behaviour registry",
				"detaching a behaviour registry while running");
			world.pauseSimulation();
			world.detachAgentBehaviourRegistry();
			require(!world.hasAgentBehaviourRegistryReference()
				&& !world.hasAttachedAgentBehaviourRegistry()
				&& !registry->hasLoadedWorld(&world),
				"the unused behaviour registry detached cleanly");

			// markUnavailable records dependency state without dirtying or
			// discarding the persisted reference.
			world.markAgentBehaviourRegistryUnavailable("orphan diagnostic");
			require(world.agentBehaviourConfigurationsAreValid(),
				"marking unavailable with no reference is a no-op");
			world.attachAgentBehaviourRegistry("package", registry);
			world.markAgentBehaviourRegistryUnavailable("disk read failed");
			require(!world.agentBehaviourConfigurationsAreValid(),
				"marking unavailable records the dependency diagnostic");
			require(world.getAgentBehaviourDependencyDiagnostic().find(
				"Agent behaviour registry dependency 'package' is unavailable")
				!= std::string::npos,
				"the dependency diagnostic names the Resource");
			require(world.getAgentBehaviourDependencyDiagnostic().find(
				"disk read failed") != std::string::npos,
				"the dependency diagnostic carries the cause");
			require(world.isSimulationPaused(),
				"marking unavailable paused the simulation");
			require(world.hasAgentBehaviourRegistryReference()
				&& world.getAgentBehaviourRegistry() == registry,
				"marking unavailable kept the persisted reference and attachment");

			// Resolve needs the matching UUID.
			requireThrows([&] {
				world.resolveAgentBehaviourRegistry(
					AgentBehaviourRegistry::create());
				}, "Agent behaviour registry UUID mismatch",
				"resolving a behaviour registry with the wrong UUID");
			world.resolveAgentBehaviourRegistry(registry);
			require(world.getAgentBehaviourRegistry() == registry,
				"resolving the matching behaviour registry kept it attached");

			// Save As: two freshly created empty registries are definition-
			// equivalent, so the copy swap succeeds; the same registry is
			// refused. A stale dependency diagnostic present at swap time is
			// cleared by the successful replacement.
			world.markAgentBehaviourRegistryUnavailable("stale before Save As");
			require(!world.agentBehaviourConfigurationsAreValid(),
				"a stale diagnostic is present before the behaviour Save As");
			requireThrows([&] {
				world.replaceAgentBehaviourRegistryWithIndependentCopy(
					"package", registry);
				}, "An Agent behaviour registry copy must have a new UUID",
				"replacing with the same-UUID behaviour registry");
			auto const copy = AgentBehaviourRegistry::create();
			world.replaceAgentBehaviourRegistryWithIndependentCopy(
				"copied-package", copy);
			require(world.getAgentBehaviourRegistry() == copy
				&& world.getAgentBehaviourRegistryResourceName()
					== "copied-package",
				"the behaviour Save As copy swapped name and registry together");
			require(copy->hasLoadedWorld(&world)
				&& !registry->hasLoadedWorld(&world),
				"the behaviour copy registered and the original unregistered");
			require(world.agentBehaviourConfigurationsAreValid(),
				"the behaviour Save As cleared the dependency diagnostic");

			// The destructive switch never skips its clearing, even when
			// passed the already-attached registry.
			world.attachAgentBehaviourRegistryAndClearAssignments(
				"copied-package", copy);
			require(world.getAgentBehaviourRegistry() == copy
				&& world.getAgentBehaviourRegistryResourceName()
					== "copied-package",
				"behaviour attach-and-clear kept a consistent namespace");
			world.detachAgentBehaviourRegistryAndClearAssignments();
			require(!world.hasAgentBehaviourRegistryReference()
				&& !copy->hasLoadedWorld(&world),
				"behaviour detach-and-clear removed the namespace");
		}

		std::cout << "RegistryBinding checks passed ("
			<< checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "RegistryBinding checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
