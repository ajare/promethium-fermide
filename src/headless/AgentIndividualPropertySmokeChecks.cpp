#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationWorkData.h"
#include "core/MobilityProfile.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runAgentIndividualPropertySmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serialize(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}
}

void runAgentIndividualPropertySmokeChecks()
{
	auto registry = core::AgentTagRegistry::create();
	auto const tag = registry->addAgentTag("tag-values");
	std::string diagnostic;
	require(registry->addAgentTagColour(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagColour(tag, { 10, 20, 30 }, &diagnostic), diagnostic);
	require(registry->addAgentTagEscalatorWalkingChance(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagEscalatorWalkingChance(tag, 0.25f, &diagnostic), diagnostic);
	require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagWalkSpeedModifier(tag, { 0.9f, 0.9f }, &diagnostic), diagnostic);
	require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagHeightModifier(tag, { 0.8f, 0.8f }, &diagnostic), diagnostic);
	require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
	require(registry->setAgentTagMobilityProfile(tag,
		core::traversalMask(core::TraversalKind::Staircase), &diagnostic), diagnostic);

	auto world = std::make_shared<core::World>("Individual properties", 8, 2);
	world->attachAgentTagRegistry("individual.tags.yaml", registry);
	auto const corridor = world->addCorridor(0, 0, 7);
	world->finishBuild();
	auto const id = world->createAgent("Agent", corridor, 0, 1.5f);
	world->pauseSimulation();
	require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
	auto* agent = world->lookupAgent(id).entity;
	require(agent->getEffectiveColour().value == (core::AgentColour{ 10, 20, 30 })
		&& !agent->getEffectiveColour().individual,
		"The fixture did not inherit its tag Colour");

	require(world->setAgentIndividualColour(id, core::AgentColour{ 100, 110, 120 }, &diagnostic), diagnostic);
	require(world->setAgentIndividualEscalatorWalkingChance(id, 0.75f, &diagnostic), diagnostic);
	require(world->setAgentIndividualWalkSpeedModifier(id, 1.1f, &diagnostic), diagnostic);
	require(world->setAgentIndividualHeightModifier(id, 0.95f, &diagnostic), diagnostic);
	auto const directMask = core::traversalMask(core::TraversalKind::Lift);
	require(world->setAgentIndividualMobilityProfile(id, directMask, &diagnostic), diagnostic);

	require(agent->getEffectiveColour().individual
		&& agent->getEffectiveColour().value == (core::AgentColour{ 100, 110, 120 })
		&& agent->getEffectiveEscalatorWalkingChance().individual
		&& agent->getEffectiveEscalatorWalkingChance().value == 0.75f
		&& agent->getEffectiveWalkSpeedModifier().individual
		&& agent->getEffectiveWalkSpeedModifier().value == 1.1f
		&& agent->getEffectiveHeightModifier().individual
		&& agent->getEffectiveHeightModifier().value == 0.95f
		&& agent->getEffectiveMobilityProfile().individual
		&& agent->getEffectiveMobilityProfile().forbiddenTraversals == directMask,
		"Individual Agent properties did not override inherited tag values");
	require(core::agentForbidsTraversal(agent, core::TraversalKind::Lift)
		&& !core::agentForbidsTraversal(agent, core::TraversalKind::Staircase),
		"Routing did not use the individual Mobility profile instead of its tag");
	require(!world->setAgentIndividualMobilityProfile(id,
		core::TraversalMask{ 1 } << 9, &diagnostic)
		&& diagnostic.find("reserved") != std::string::npos,
		"An individual Mobility profile accepted a reserved bit");

	auto const yaml = serialize(*world);
	require(yaml.find("version: 17") != std::string::npos
		&& yaml.find("individualProperties") != std::string::npos,
		"Individual Agent properties were not persisted in World schema 16");
	auto loaded = std::make_shared<core::World>("Loading", 1, 1);
	auto reader = core::YamlSerializer::fromString(yaml);
	reader->deserialize();
	core::SerializationWorkData work;
	require(loaded->deserialize(*reader, work), "The individual-property World did not deserialize");
	auto* loadedAgent = loaded->lookupAgent(id).entity;
	require(loadedAgent && loadedAgent->getEffectiveColour().individual
		&& loadedAgent->getEffectiveMobilityProfile().forbiddenTraversals == directMask,
		"Individual Agent properties did not round-trip");

	require(world->setAgentIndividualColour(id, std::nullopt, &diagnostic), diagnostic);
	require(!agent->getEffectiveColour().individual
		&& agent->getEffectiveColour().sourceTag == tag
		&& agent->getEffectiveColour().value == (core::AgentColour{ 10, 20, 30 }),
		"Removing an individual property did not reveal its inherited tag value");

	require(world->resumeSimulation(), "The individual-property fixture did not resume");
	require(!world->setAgentIndividualHeightModifier(id, 0.9f, &diagnostic)
		&& diagnostic.find("Pause") != std::string::npos,
		"An individual Agent property changed while simulation was running");
}
