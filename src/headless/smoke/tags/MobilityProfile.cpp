#include "Checks.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

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

	void mobilityProfileRegistryAndAssignment()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("mobility");
		std::string diagnostic;
		require(registry->addAgentTagMobilityProfile(tag, &diagnostic), diagnostic);
		require(registry->getAgentTagMobilityProfile(tag)
			&& registry->getAgentTagMobilityProfile(tag)->value == core::MobilityProfile{}
			&& registry->getAgentTagMobilityProfile(tag)->revision == 1,
			"A Mobility profile did not begin with every traversal usable and a revision");

		core::MobilityProfile profile;
		profile.set(core::TraversalKind::Staircase, core::MobilityUse::CannotUse);
		profile.set(core::TraversalKind::Ladder, core::MobilityUse::OnlyIfNoOtherOption);
		profile.set(core::TraversalKind::Buttons, core::MobilityUse::OnlyIfNoOtherOption);
		require(registry->setAgentTagMobilityProfile(tag, profile, &diagnostic), diagnostic);
		require(registry->getAgentTagMobilityProfile(tag)->value == profile,
			"A ternary Mobility profile did not round-trip in memory");
		auto const serialized = serialize(*registry);
		require(serialized.find("version: 14") != std::string::npos
			&& serialized.find("onlyIfNoOtherOption") != std::string::npos,
			"A ternary Mobility profile was not serialized explicitly");

		auto loaded = deserialize(serialized);
		require(loaded->getAgentTagMobilityProfile(tag)
			&& loaded->getAgentTagMobilityProfile(tag)->value == profile,
			"A ternary Mobility profile did not survive serialization");
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
		require(static_cast<bool>(agent) && agent.entity->getEffectiveMobilityProfile().value == profile
			&& agent.entity->getEffectiveMobilityProfile().sourceTag == tag,
			"An Agent did not derive its effective Mobility profile from its tag");
	}
}

void tag_smoke::registerMobilityProfile(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "mobilityProfileRegistryAndAssignment",
		[](smoke::Context const&)
		{
			mobilityProfileRegistryAndAssignment();
		} });
}
