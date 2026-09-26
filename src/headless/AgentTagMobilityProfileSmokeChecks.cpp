#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

void runAgentTagMobilityProfileSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serialize(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::AgentTagRegistry> deserialize(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work),
			"The Mobility profile registry did not deserialize");
		return registry;
	}
}

void runAgentTagMobilityProfileSmokeChecks()
{
	auto registry = core::AgentTagRegistry::create();
	auto const tag = registry->addAgentTag("mobility");
	std::string diagnostic;
	require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
	require(registry->getAgentTagMobilityProfile(tag)
		&& registry->getAgentTagMobilityProfile(tag)->forbiddenTraversals == 0
		&& registry->getAgentTagMobilityProfile(tag)->revision == 1,
		"A Mobility profile did not begin at zero with a revision");

	for (core::TraversalMask mask = 1; mask <= core::AllTraversalMaskBits; ++mask)
	{
		require(registry->setAgentTagMobilityProfile(tag, mask, &diagnostic), diagnostic);
		require(registry->getAgentTagMobilityProfile(tag)->forbiddenTraversals == mask,
			"A valid Mobility profile mask did not round-trip in memory");
	}
	auto const beforeRefusal = serialize(*registry);
	require(!registry->setAgentTagMobilityProfile(tag, core::TraversalMask{ 1 } << 9,
		&diagnostic) && diagnostic.find("reserved") != std::string::npos
		&& serialize(*registry) == beforeRefusal,
		"A reserved Mobility profile bit was not refused atomically");

	auto loaded = deserialize(beforeRefusal);
	require(loaded->getAgentTagMobilityProfile(tag)
		&& loaded->getAgentTagMobilityProfile(tag)->forbiddenTraversals
			== core::AllTraversalMaskBits,
		"A version 2 Mobility profile did not survive serialization");
	auto copy = core::AgentTagRegistry::copyWithNewUuid(*loaded);
	require(copy->hasEquivalentDefinitions(*loaded),
		"Registry copying did not preserve a Mobility profile");

	auto world = std::make_shared<core::World>("Mobility", 4, 2);
	world->attachAgentTagRegistry("mobility.tags.yaml", loaded);
	auto const corridor = world->addCorridor(0, 0, 3);
	world->finishBuild();
	auto const agentId = world->createAgent("Agent", corridor, 0, 1.5f);
	world->pauseSimulation();
	require(world->assignAgentTag(agentId, tag, &diagnostic), diagnostic);
	auto const agent = world->lookupAgent(agentId);
	require(static_cast<bool>(agent) && agent.entity->getEffectiveMobilityProfile().forbiddenTraversals
		== core::AllTraversalMaskBits
		&& agent.entity->getEffectiveMobilityProfile().sourceTag == tag,
		"An Agent did not derive its effective Mobility profile from its tag");
}
