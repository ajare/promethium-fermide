#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistry.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition)
			throw std::runtime_error("Agent property-descriptor check failed: " + what);
		++checksRun;
	}
}

int main()
{
	using namespace core;
	using namespace std;

	try
	{
		string diagnostic;

		// ================================================================
		// Individual overrides through the single descriptor path (#677).
		// ================================================================

		World world("property-descriptor-checks", 8, 4);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const agent = world.createAgent("Ada", corridor, 0, 1.0f);
		auto* ada = world.lookupAgent(agent).entity;

		// An unknown Agent is refused through the shared prologue.
		require(!world.setAgentIndividualWalkSpeedModifier(
			AgentId{ 9999 }, 1.1f, &diagnostic) && !diagnostic.empty(),
			"an unknown Agent is refused with a diagnostic");

		// The pause gate fires while the simulation runs, before the
		// unchanged check: even setting the current value reports the gate.
		require(!world.isSimulationPaused(),
			"the freshly built World simulates");
		require(!world.setAgentIndividualWalkSpeedModifier(agent, 1.1f, &diagnostic)
			&& diagnostic == "Pause the simulation before editing individual Agent properties",
			"the pause gate fires while running");

		world.pauseSimulation();

		// A valid write lands and the effective value becomes individual
		// (ADR 0012).
		require(world.setAgentIndividualWalkSpeedModifier(agent, 1.2f, &diagnostic)
			&& diagnostic.empty(),
			"setting the individual Walk speed modifier: " + diagnostic);
		require(ada->getEffectiveWalkSpeedModifier().individual
			&& ada->getEffectiveWalkSpeedModifier().value == 1.2f,
			"the effective Walk speed modifier is the individual value");

		// The unchanged refusal carries the row's own wording.
		require(!world.setAgentIndividualWalkSpeedModifier(agent, 1.2f, &diagnostic)
			&& diagnostic == "The individual Walk speed modifier is unchanged",
			"the unchanged refusal names the property");

		// The row's validator refuses an out-of-range value before anything
		// changes.
		require(!world.setAgentIndividualWalkSpeedModifier(agent, 99.0f, &diagnostic)
			&& !diagnostic.empty()
			&& ada->getEffectiveWalkSpeedModifier().value == 1.2f,
			"an out-of-range Walk speed modifier is refused");

		// Clearing the override reveals the inherited default again.
		require(world.setAgentIndividualWalkSpeedModifier(agent, nullopt, &diagnostic)
			&& !ada->getEffectiveWalkSpeedModifier().individual
			&& ada->getEffectiveWalkSpeedModifier().value == 1.0f,
			"clearing the override reveals the default");

		// Colour: the same path, its own unchanged wording.
		require(world.setAgentIndividualColour(agent, AgentColour{ 10, 20, 30 },
			&diagnostic),
			"setting the individual Colour");
		require(!world.setAgentIndividualColour(agent, AgentColour{ 10, 20, 30 },
			&diagnostic)
			&& diagnostic == "The individual Agent Colour is unchanged",
			"the Colour unchanged refusal keeps its wording");
		require(world.setAgentIndividualColour(agent, nullopt, &diagnostic),
			"clearing the individual Colour");

		// Escalator walking chance: validated by its own float validator.
		require(!world.setAgentIndividualEscalatorWalkingChance(agent, 1.5f,
			&diagnostic) && !diagnostic.empty(),
			"an out-of-range Escalator walking chance is refused");
		require(world.setAgentIndividualEscalatorWalkingChance(agent, 0.5f,
			&diagnostic)
			&& ada->getEffectiveEscalatorWalkingChance().individual,
			"a valid Escalator walking chance is accepted");

		// Mobility profile: the row validator rejects a malformed profile.
		MobilityProfile badProfile;
		badProfile.uses[0] = static_cast<MobilityUse>(99);
		require(!world.setAgentIndividualMobilityProfile(agent, badProfile,
			&diagnostic)
			&& diagnostic.find("invalid Mobility use") != string::npos,
			"a malformed Mobility profile is refused");
		MobilityProfile goodProfile;
		goodProfile.set(TraversalKind::Staircase, MobilityUse::CanUse);
		require(world.setAgentIndividualMobilityProfile(agent, goodProfile,
			&diagnostic)
			&& ada->getEffectiveMobilityProfile().individual,
			"a valid Mobility profile is accepted");

		// Permission adherence: the effective value flips through the
		// wrapped write, and clearing restores the default.
		require(world.setAgentIndividualPermissionAdherence(agent, false,
			&diagnostic)
			&& !ada->getEffectivePermissionAdherence().value,
			"individual non-adherence is effective");
		require(world.setAgentIndividualPermissionAdherence(agent, nullopt,
			&diagnostic)
			&& ada->getEffectivePermissionAdherence().value,
			"clearing adherence restores the default");

		// Remote object-control flags ride the same path.
		require(world.setAgentIndividualRemoteAccessPanels(agent, false,
			&diagnostic)
			&& !ada->getEffectiveRemoteAccessPanels().value,
			"individual Remote Access panels off is effective");
		require(world.setAgentIndividualRemoteBoothWindowShutters(agent, false,
			&diagnostic)
			&& !ada->getEffectiveRemoteBoothWindowShutters().value,
			"individual Remote BoothWindow shutters off is effective");

		// Height modifier: the row's preflight keeps the pose-fit rule.
		require(world.setAgentIndividualHeightModifier(agent, 1.0f, &diagnostic)
			&& ada->getEffectiveHeightModifier().individual,
			"setting the individual Height modifier: " + diagnostic);
		require(!world.setAgentIndividualHeightModifier(agent, 99.0f, &diagnostic),
			"an out-of-range Height modifier is refused");
		require(world.setAgentIndividualHeightModifier(agent, nullopt, &diagnostic)
			&& ada->getEffectiveHeightModifier().value == 1.0f,
			"clearing the Height modifier restores the default");

		// ================================================================
		// Tag samples through the shared add/clear paths (#677), driven
		// from the registry the way the editor drives them.
		// ================================================================

		auto registry = AgentTagRegistry::create();
		world.attachAgentTagRegistry("tags", registry);
		auto const tagA = registry->addAgentTag("swift");
		auto const tagB = registry->addAgentTag("brisk");
		auto const bob = world.createAgent("Bob", corridor, 0, 2.0f);
		auto* bobber = world.lookupAgent(bob).entity;

		require(world.assignAgentTag(agent, tagA, &diagnostic)
			&& world.assignAgentTag(bob, tagB, &diagnostic),
			"assigning one tag per Agent: " + diagnostic);

		// Adding the definition samples every assigned Agent with the
		// tag's provenance; unassigned Agents keep nothing.
		require(registry->addAgentTagWalkSpeedModifier(tagA, &diagnostic)
			&& ada->getWalkSpeedModifierSample()
			&& ada->getWalkSpeedModifierSample()->sourceTag == tagA
			&& ada->getWalkSpeedModifierSample()->propertyRevision
				== registry->getAgentTagWalkSpeedModifier(tagA)->revision,
			"the assigned Agent gets a stamped sample: " + diagnostic);
		require(!bobber->getWalkSpeedModifierSample(),
			"an unassigned Agent gets no sample");
		require(!ada->getEffectiveWalkSpeedModifier().individual
			&& ada->getEffectiveWalkSpeedModifier().value == 1.0f,
			"the sample is effective without an override");

		// The individual override wins over the tag sample (ADR 0012)
		// without destroying it.
		require(world.setAgentIndividualWalkSpeedModifier(agent, 1.1f,
			&diagnostic)
			&& ada->getEffectiveWalkSpeedModifier().individual
			&& ada->getEffectiveWalkSpeedModifier().value == 1.1f
			&& ada->getWalkSpeedModifierSample()->sourceTag == tagA,
			"the individual value overrides the sample, which survives");
		require(world.setAgentIndividualWalkSpeedModifier(agent, nullopt,
			&diagnostic)
			&& ada->getEffectiveWalkSpeedModifier().value
				== ada->getWalkSpeedModifierSample()->value,
			"clearing the override reveals the sample again");

		// Clearing a tag's samples only touches samples sourced from that
		// tag: Bob's tag-B sample survives tag A's removal.
		require(registry->addAgentTagWalkSpeedModifier(tagB, &diagnostic)
			&& bobber->getWalkSpeedModifierSample()->sourceTag == tagB,
			"Bob is sampled from tag B: " + diagnostic);
		require(registry->removeAgentTagWalkSpeedModifier(tagA, &diagnostic)
			&& !ada->getWalkSpeedModifierSample()
			&& bobber->getWalkSpeedModifierSample()
			&& bobber->getWalkSpeedModifierSample()->sourceTag == tagB,
			"removing tag A clears only A-sourced samples");

		// Height samples take the same row-driven path, including the
		// pose-syncing Agent accessors.
		require(registry->addAgentTagHeightModifier(tagB, &diagnostic)
			&& bobber->getHeightModifierSample()
			&& bobber->getHeightModifierSample()->sourceTag == tagB,
			"Bob gets a Height modifier sample: " + diagnostic);
		require(registry->removeAgentTagHeightModifier(tagB, &diagnostic)
			&& !bobber->getHeightModifierSample(),
			"removing tag B clears Bob's Height modifier sample");

		std::cout << "Agent property-descriptor checks passed ("
			<< checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "Agent property-descriptor checks FAILED: "
			<< error.what() << "\n";
		return 1;
	}
}
