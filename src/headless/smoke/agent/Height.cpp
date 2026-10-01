// Migrated from AgentHeightSmokeChecks.cpp (#286); core dependency tier.

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work),
			"The Height registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work),
			"The Height World did not deserialize");
		return world;
	}

	void rangesAreBoundedRevisionedAndPersisted()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("height");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		auto const* added = registry->getAgentTagHeightModifier(tag);
		require(added && added->range == core::DefaultAgentHeightModifierRange
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Height did not default both endpoints to 1.0 at revision 1");

		auto const beforeInvalid = serializeRegistry(*registry);
		for (auto const invalid : {
			core::AgentModifierRange{ 0.69f, 1.0f },
			core::AgentModifierRange{ 0.7f, 1.01f },
			core::AgentModifierRange{ 0.9f, 0.8f },
			core::AgentModifierRange{ std::numeric_limits<float>::infinity(), 1.0f },
			core::AgentModifierRange{ 0.7f, std::numeric_limits<float>::quiet_NaN() } })
		{
			require(!registry->setAgentTagHeightModifier(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serializeRegistry(*registry) == beforeInvalid
				&& registry->getNextPropertyRevision() == 2,
				"An invalid Height range changed definitions or revisions");
		}

		require(registry->setAgentTagHeightModifier(tag, { 0.7f, 1.0f }, &diagnostic),
			diagnostic);
		auto const expected = *registry->getAgentTagHeightModifier(tag);
		require(expected.revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Height range edit did not allocate one revision");
		auto const yaml = serializeRegistry(*registry);
		require(yaml.find("type: heightModifier") != std::string::npos,
			"Height property type was not persisted");
		auto reopened = deserializeRegistry(yaml);
		require(*reopened->getAgentTagHeightModifier(tag) == expected,
			"Height endpoints or revision did not survive persistence");

		auto malformed = YAML::Load(yaml);
		malformed["tags"][0]["properties"][0]["min"] = 0.6f;
		bool refused{ false };
		try { (void)deserializeRegistry(YAML::Dump(malformed)); }
		catch (std::exception const& error)
		{
			refused = std::string(error.what()).find("between 0.7 and 1.0")
				!= std::string::npos;
		}
		require(refused, "A persisted out-of-bounds Height range was accepted");
	}

	void assignmentPersistenceAndConflictsMatchOtherProperties()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const shortTag = registry->addAgentTag("short");
		auto const duplicate = registry->addAgentTag("duplicate");
		auto const pending = registry->addAgentTag("pending");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(shortTag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(shortTag, { 0.7f, 0.7f },
			&diagnostic), diagnostic);
		require(registry->addAgentTagHeightModifier(duplicate, &diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Height assignment", 8, 2);
		world->attachAgentTagRegistry("height.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		auto const agent = world->createAgent("Short", corridor, 0, 1.5f);
		world->pauseSimulation();
		require(world->assignAgentTag(agent, shortTag, &diagnostic), diagnostic);
		auto const sample = world->lookupAgent(agent).entity->getHeightModifierSample();
		require(sample && sample->type == core::SampledAgentPropertyType::HeightModifier
			&& sample->sourceTag == shortTag
			&& sample->propertyRevision
				== registry->getAgentTagHeightModifier(shortTag)->revision
			&& std::abs(sample->value - 0.7f) < 0.000001f,
			"Height assignment did not create the fixed sample with exact provenance");

		auto const beforeConflict = serializeWorld(*world);
		require(!world->assignAgentTag(agent, duplicate, &diagnostic)
			&& diagnostic.find("Height modifier") != std::string::npos
			&& diagnostic.find("#short") != std::string::npos
			&& diagnostic.find("#duplicate") != std::string::npos
			&& serializeWorld(*world) == beforeConflict,
			"A duplicate inherited Height modifier was not refused atomically");

		require(world->assignAgentTag(agent, pending, &diagnostic), diagnostic);
		auto const registryBefore = serializeRegistry(*registry);
		auto const revisionBefore = registry->getNextPropertyRevision();
		require(!registry->addAgentTagHeightModifier(pending, &diagnostic)
			&& diagnostic.find("Height modifier") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& registry->getNextPropertyRevision() == revisionBefore,
			"A conflicting Height property addition changed the registry");

		auto const exactSample = *sample;
		auto reopened = deserializeWorld(serializeWorld(*world));
		reopened->resolveAgentTagRegistry(registry);
		require(reopened->lookupAgent(agent).entity->getHeightModifierSample()
			== std::optional<core::AgentPropertySample>{ exactSample },
			"The exact Height sample did not survive World persistence");
		reopened->resetSimulation();
		require(reopened->lookupAgent(agent).entity->getHeightModifierSample()
			== std::optional<core::AgentPropertySample>{ exactSample },
			"Simulation reset changed the persisted Height sample");
	}


}

void agent_smoke::registerHeight(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "heightRangesAreBoundedRevisionedAndPersisted", [](smoke::Context const&) { rangesAreBoundedRevisionedAndPersisted(); } });
	checks.push_back({ "heightAssignmentPersistenceAndConflictsMatchOtherProperties", [](smoke::Context const&) { assignmentPersistenceAndConflictsMatchOtherProperties(); } });
}
