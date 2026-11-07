#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagService.h"
#include "core/Exceptions.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition)
			throw std::runtime_error("Agent tag-service check failed: " + what);
		++checksRun;
	}

	// Stamps a current-revision Route familiarity sample onto a paste
	// set, mirroring what the assign path would have written.
	void plantRouteFamiliarity(core::AgentTagSampleSet& samples,
		core::AgentTagRegistry const& registry, core::AgentTagId walker)
	{
		auto const* property = registry.getAgentTagRouteFamiliarity(walker);
		samples[9] = core::AgentPropertySample{
			core::SampledAgentPropertyType::RouteFamiliarity, walker,
			property->revision, core::sampleAgentModifier(property->range) };
	}
}

int main()
{
	using namespace core;
	using namespace std;

	try
	{
		// The service is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the tag assignment
		// family is unit-testable without driving the World facade. World
		// keeps the state it edits (ADR 0001); the collaborator holds only
		// a reference and reaches it through friend access.

		string diagnostic;
		World world("tag-service", 10, 3);
		AgentTagService service(world);
		auto const corridor = world.addCorridor(0, 0, 10);
		world.finishBuild();
		auto const ada = world.createAgent("Ada", corridor, 0, 1.0f);
		auto const bob = world.createAgent("Bob", corridor, 0, 2.0f);
		auto* adaEntity = world.lookupAgent(ada).entity;
		auto* bobEntity = world.lookupAgent(bob).entity;

		// Canonical sampled-property row indices (the #677 table order).
		constexpr size_t CrowdAversionRow = 7;
		constexpr size_t RouteFamiliarityRow = 9;
		// tags() throws for an unknown Agent, like the replaced facade.
		bool threw = false;
		try { service.tags(AgentId{ 9999 }); }
		catch (WorldException const&) { threw = true; }
		require(threw, "tags() throws for an unknown Agent");
		require(service.tags(ada).empty(), "a fresh Agent carries no tags");

		// The pause gate fires while the simulation runs.
		require(!world.isSimulationPaused(), "the freshly built World simulates");
		require(!service.assign(ada, AgentTagId{ 1 }, &diagnostic)
			&& diagnostic == "Pause the simulation before assigning an Agent tag",
			"assign refuses while running");
		require(!service.remove(ada, AgentTagId{ 1 }, &diagnostic)
			&& diagnostic == "Pause the simulation before removing an Agent tag",
			"remove refuses while running");
		world.pauseSimulation();

		// Without an attached registry both gates refuse.
		require(!service.assign(ada, AgentTagId{ 1 }, &diagnostic)
			&& diagnostic == "This World has no attached Agent tag registry",
			"assign refuses without a registry");
		require(!service.validateAssignments({ AgentTagId{ 1 } }, {}, &diagnostic)
			&& diagnostic == "This World has no attached Agent tag registry",
			"validate refuses without a registry");

		// An untagged set carrying any sample is refused before the
		// registry is consulted.
		AgentTagSampleSet oneSample;
		oneSample[0] = AgentPropertySample{ SampledAgentPropertyType::WalkSpeedModifier,
			AgentTagId{ 1 }, 1, 1.0f };
		require(service.validateAssignments({}, {}, &diagnostic)
				&& diagnostic.empty(),
			"an empty untagged set validates");
		require(!service.validateAssignments({}, oneSample, &diagnostic)
			&& diagnostic == "An untagged Agent cannot carry modifier samples",
			"untagged samples are refused");

		// ---------------------------------------------------------------
		// Assign with the registry attached.
		// ---------------------------------------------------------------
		auto registry = AgentTagRegistry::create();
		world.attachAgentTagRegistry("tags", registry);
		auto const swift = registry->addAgentTag("swift");
		auto const brisk = registry->addAgentTag("brisk");

		require(!service.assign(ada, AgentTagId{ 404 }, &diagnostic)
			&& diagnostic.find("not defined in the attached registry") != string::npos,
			"an undefined tag is refused");

		require(registry->addAgentTagWalkSpeedModifier(swift, &diagnostic),
			"defining the walk speed modifier: " + diagnostic);
		require(service.assign(ada, swift, &diagnostic),
			"assigning the tag: " + diagnostic);
		require(service.tags(ada).count(swift) == 1, "the tag is assigned");
		require(service.countAssignments(swift) == 1, "the assignment counts");
		require(adaEntity->getWalkSpeedModifierSample()
			&& adaEntity->getWalkSpeedModifierSample()->sourceTag == swift,
			"assign stamps a walk-speed sample sourced from the tag");
		require(!adaEntity->getEffectiveWalkSpeedModifier().individual
			&& adaEntity->getEffectiveWalkSpeedModifier().value
				== adaEntity->getWalkSpeedModifierSample()->value,
			"the tag sample is effective without an override");

		// Re-assigning the same tag is refused and changes nothing.
		require(!service.assign(ada, swift, &diagnostic)
			&& diagnostic.find("already assigned") != string::npos,
			"a duplicate assignment is refused");

		// A conflicting inheritance source is refused with the row's own
		// property name, and the refusal leaves nothing behind.
		require(registry->addAgentTagWalkSpeedModifier(brisk, &diagnostic),
			"defining a second walk-speed tag: " + diagnostic);
		require(!service.assign(ada, brisk, &diagnostic)
			&& diagnostic == "Agent 'Ada' cannot be assigned to #brisk because "
				"Walk speed modifier is already inherited from #swift",
			"the sampled-conflict refusal names the property: " + diagnostic);
		require(service.tags(ada).size() == 1 && bobEntity->getAgentTagIds().empty(),
			"the refused assignment left every Agent unchanged");

		// Bob can take the second tag; two Agents share one tag.
		require(service.assign(bob, brisk, &diagnostic),
			"assigning Bob the second tag: " + diagnostic);
		require(service.countAssignments(brisk) == 1
			&& service.countAssignments(swift) == 1,
			"each tag counts its own assignments");

		// ---------------------------------------------------------------
		// Remove.
		// ---------------------------------------------------------------
		require(!service.remove(bob, swift, &diagnostic)
			&& diagnostic.find("is not assigned") != string::npos,
			"removing an unassigned tag is refused");
		require(service.remove(bob, brisk, &diagnostic),
			"removing the assigned tag: " + diagnostic);
		require(bobEntity->getAgentTagIds().empty()
			&& !bobEntity->getWalkSpeedModifierSample(),
			"remove drops the tag and its sourced sample");
		require(adaEntity->getWalkSpeedModifierSample()
			&& adaEntity->getWalkSpeedModifierSample()->sourceTag == swift,
			"the other Agent's sample is untouched");

		// ---------------------------------------------------------------
		// Tag-wide clearing.
		// ---------------------------------------------------------------
		require(service.countAssignments(swift) == 1, "Ada still carries swift");
		service.clearAssignments(swift);
		require(adaEntity->getAgentTagIds().empty()
			&& !adaEntity->getWalkSpeedModifierSample()
			&& service.countAssignments(swift) == 0,
			"clearAssignments drops the tag and only its sourced samples");

		// clearAllAssignmentsAndSamples keeps the pre-extraction cleared-
		// sample set: the route-planning samples survive it.
		auto const walker = registry->addAgentTag("walker");
		require(registry->addAgentTagCrowdAversion(walker, &diagnostic)
			&& registry->addAgentTagRouteFamiliarity(walker, &diagnostic),
			"defining crowd/route properties: " + diagnostic);
		require(service.assign(ada, walker, &diagnostic),
			"assigning the walker tag: " + diagnostic);
		require(adaEntity->getCrowdAversionSample()
			&& adaEntity->getRouteFamiliaritySample(),
			"both samples were stamped on assign");
		require(service.allAssignmentsCanBeCleared(&diagnostic),
			"the plain world can clear every assignment: " + diagnostic);
		service.clearAllAssignmentsAndSamples();
		require(adaEntity->getAgentTagIds().empty()
			&& !adaEntity->getCrowdAversionSample()
			&& adaEntity->getRouteFamiliaritySample(),
			"clearAll matches the replaced method's cleared-sample set");

		// ---------------------------------------------------------------
		// canApplyHeightModifier.
		// ---------------------------------------------------------------
		require(service.canApplyHeightModifier(swift, nullopt, &diagnostic),
			"no height sample sourced from swift: clearing may proceed");
		require(service.canApplyHeightModifier(swift,
			AgentModifierRange{ 1.0f, 1.0f }, &diagnostic),
			"a fitting height range is accepted");

		// ---------------------------------------------------------------
		// Paste validation and restoration.
		// ---------------------------------------------------------------
		require(!service.restoreAssignments(AgentId{ 9999 }, {}, {}, &diagnostic)
			&& !diagnostic.empty(),
			"restore refuses an unknown Agent");

		// Restore the walker assignment onto Ada through the validated
		// paste path.
		AgentTagSampleSet walkerSamples;
		walkerSamples[CrowdAversionRow] = AgentPropertySample{
			SampledAgentPropertyType::CrowdAversion, walker,
			registry->getAgentTagCrowdAversion(walker)->revision, 1.0f };
		plantRouteFamiliarity(walkerSamples, *registry, walker);
		require(service.validateAssignments({ walker }, walkerSamples, &diagnostic),
			"the walker paste validates: " + diagnostic);
		require(service.restoreAssignments(ada, { walker }, walkerSamples,
			&diagnostic),
			"restoring the walker assignment: " + diagnostic);
		require(adaEntity->getAgentTagIds() == set<AgentTagId>{ walker }
			&& adaEntity->getCrowdAversionSample()
			&& adaEntity->getCrowdAversionSample()->sourceTag == walker
			&& adaEntity->getRouteFamiliaritySample()
			&& adaEntity->getRouteFamiliaritySample()->sourceTag == walker,
			"the restore wrote the tags and samples");
		// An identical restore is a no-op success.
		require(service.restoreAssignments(ada, { walker }, walkerSamples,
			&diagnostic),
			"an identical restore succeeds");

		// A paste whose sample is stale against the current definition is
		// refused.
		AgentTagSampleSet staleSamples = walkerSamples;
		staleSamples[CrowdAversionRow]->propertyRevision = 9999;
		require(!service.validateAssignments({ walker }, staleSamples, &diagnostic)
			&& diagnostic.find("stale") != string::npos,
			"a stale pasted sample is refused");
		// A sample without an inherited property is refused.
		require(!service.validateAssignments({ swift }, walkerSamples, &diagnostic),
			"a sample without an inherited property is refused");

		// ---------------------------------------------------------------
		// Open-time inspection and reconciliation.
		// ---------------------------------------------------------------
		// A registry that no longer defines an assigned tag is refused.
		auto const emptyRegistry = AgentTagRegistry::create();
		require(!service.inspectAssignments(*emptyRegistry, false, nullptr,
			&diagnostic)
			&& diagnostic.find("does not define") != string::npos,
			"an undefined assigned tag is refused");

		// Build a prospective registry that keeps every assigned tag ID
		// but no longer defines their properties.
		auto const shrunk = AgentTagRegistry::create();
		shrunk->addAgentTag("swift");
		shrunk->addAgentTag("brisk");
		auto const shrunkWalker = shrunk->addAgentTag("walker");
		require(shrunkWalker == walker,
			"the prospective registry lines the tag up by stable ID");
		require(!service.inspectAssignments(*shrunk, false, nullptr, &diagnostic)
			&& diagnostic.find("without an inherited property") != string::npos,
			"an orphan sample is refused without reconciliation");
		vector<AgentTagReconciliation> repairs;
		require(service.inspectAssignments(*shrunk, true, &repairs, &diagnostic),
			"the orphan samples reconcile: " + diagnostic);
		require(repairs.size() == 1
			&& repairs[0].agent == ada
			&& repairs[0].crowdAversionAction
				== AgentTagSampleRepairAction::Clear
			&& repairs[0].routeFamiliarityAction
				== AgentTagSampleRepairAction::Clear,
			"the repair plans Clears of the orphan samples");
		service.applyReconciliations(repairs);
		require(!adaEntity->getCrowdAversionSample()
			&& !adaEntity->getRouteFamiliaritySample(),
			"applying the repair cleared the orphan samples");

		// A missing sample for an inherited property plans a Resample.
		vector<AgentTagReconciliation> resampleRepairs;
		require(service.inspectAssignments(*registry, true, &resampleRepairs,
			&diagnostic),
			"the missing samples reconcile: " + diagnostic);
		require(resampleRepairs.size() == 1
			&& resampleRepairs[0].crowdAversionAction
				== AgentTagSampleRepairAction::Resample
			&& resampleRepairs[0].crowdAversionSource == walker,
			"the repair plans a Resample from the walker tag");
		service.applyReconciliations(resampleRepairs);
		require(adaEntity->getCrowdAversionSample()
			&& adaEntity->getCrowdAversionSample()->sourceTag == walker
			&& adaEntity->getCrowdAversionSample()->propertyRevision
				== registry->getAgentTagCrowdAversion(walker)->revision,
			"the resample landed with current provenance");
		require(service.assign(bob, walker, &diagnostic),
			"Bob takes the walker tag: " + diagnostic);

		// A sample sourced from a different tag than the prospective
		// inheritance is refused even when reconciliation is allowed:
		// Ada's walk-speed sample is sourced from swift, while the
		// prospective registry moves the walk-speed inheritance onto
		// walker.
		require(service.assign(ada, swift, &diagnostic),
			"Ada takes the swift tag: " + diagnostic);
		auto const moved = AgentTagRegistry::create();
		moved->addAgentTag("swift");
		moved->addAgentTag("brisk");
		auto const movedWalker = moved->addAgentTag("walker");
		require(movedWalker == walker
			&& moved->addAgentTagWalkSpeedModifier(movedWalker, &diagnostic),
			"the moved registry carries walk speed on walker: " + diagnostic);
		require(!service.inspectAssignments(*moved, true, nullptr, &diagnostic)
			&& diagnostic.find("but inherits that property from") != string::npos,
			"a wrong-source sample is refused even with reconciliation: "
				+ diagnostic);

		// reconcileAssignments runs inspect+apply as one step: the full
		// current registry leaves nothing to repair.
		service.reconcileAssignments(*registry);
		require(adaEntity->getCrowdAversionSample()
			&& bobEntity->getCrowdAversionSample(),
			"reconciling against the live registry keeps the samples");

		// ---------------------------------------------------------------
		// Facade/service agreement on the shared judgements.
		// ---------------------------------------------------------------
		{
			string facadeDiagnostic;
			string serviceDiagnostic;
			auto agree = [&](bool facadeResult, bool serviceResult)
			{
				return facadeResult == serviceResult
					&& facadeDiagnostic == serviceDiagnostic;
			};
			require(agree(world.canAssignAgentTag(ada, brisk, &facadeDiagnostic),
				service.canAssign(ada, brisk, &serviceDiagnostic)),
				"canAssign agrees with the facade");
			require(agree(world.canRemoveAgentTag(ada, walker, &facadeDiagnostic),
				service.canRemove(ada, walker, &serviceDiagnostic)),
				"canRemove agrees with the facade");
			require(world.getAgentTags(ada) == service.tags(ada),
				"getAgentTags agrees with the service");
			require(world.validateAgentTagAssignments(set<AgentTagId>{ walker },
					nullopt, nullopt, nullopt, nullopt, nullopt, nullopt, nullopt,
					walkerSamples[CrowdAversionRow], nullopt,
					walkerSamples[RouteFamiliarityRow], nullopt, nullopt, nullopt,
					&facadeDiagnostic)
				&& service.validateAssignments({ walker }, walkerSamples, &serviceDiagnostic)
				&& facadeDiagnostic == serviceDiagnostic,
				"validateAssignments agrees with the facade");
		}

		std::cout << "Agent tag-service checks passed ("
			<< checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "Agent tag-service checks FAILED: "
			<< error.what() << "\n";
		return 1;
	}
}
