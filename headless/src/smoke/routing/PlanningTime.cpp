#include "Checks.h"
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

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

	std::string serialize(core::Serializable const& document)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		document.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	void deserialize(core::Serializable& document, std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(document.deserialize(*reader, work), "Route planning time fixture failed to load");
	}

	struct Fixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{ std::make_shared<core::World>("Planning times", 10, 2) };
		core::AgentTagId tag{ registry->addAgentTag("planner") };
		uint32_t corridor{};
		core::AgentId id{};
		std::string diagnostic;

		Fixture()
		{
			world->attachAgentTagRegistry("planning.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 9);
			world->finishBuild();
			world->pauseSimulation();
			id = world->createAgent("Planner", corridor, 0, 1.5f);
			require(world->assignAgentTag(id, tag, &diagnostic), diagnostic);
		}

		core::Agent* agent() const { return world->lookupAgent(id).entity; }

		void addProperties()
		{
			require(registry->addAgentTagMinimumRoutePlanningTime(tag, &diagnostic), diagnostic);
			require(registry->addAgentTagMaximumRoutePlanningTime(tag, &diagnostic), diagnostic);
		}
	};

	void defaultsValidationAndIndependentOverrides()
	{
		Fixture f;
		for (auto type : { core::AgentPropertyType::MinimumRoutePlanningTime,
			core::AgentPropertyType::MaximumRoutePlanningTime })
			require(core::agentPropertyMetadata(type).propertyNamespace == "Pathing",
				"Planning times are not in the Pathing namespace");
		require(f.agent()->getEffectiveMinimumRoutePlanningTime().value == 1.0f
			&& f.agent()->getEffectiveMaximumRoutePlanningTime().value == 3.0f,
			"Planning time defaults are not [1,3]");
		f.addProperties();
		require(f.registry->getAgentTagMinimumRoutePlanningTime(f.tag)->range
			== (core::AgentModifierRange{ 1, 1 })
			&& f.registry->getAgentTagMaximumRoutePlanningTime(f.tag)->range
			== (core::AgentModifierRange{ 3, 3 }), "Initial tag ranges are not fixed defaults");
		for (float invalid : { 0.09f, 10.01f, std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
		{
			auto const before = serialize(*f.world);
			require(!f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, invalid, &f.diagnostic)
				&& !f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, invalid, &f.diagnostic)
				&& !f.registry->setAgentTagMinimumRoutePlanningTime(f.tag, { invalid, invalid }, &f.diagnostic)
				&& !f.registry->setAgentTagMaximumRoutePlanningTime(f.tag, { invalid, invalid }, &f.diagnostic)
				&& serialize(*f.world) == before, "Invalid planning time was accepted or mutated the World");
		}
		require(!f.registry->setAgentTagMinimumRoutePlanningTime(f.tag, { 5.0f, 4.0f }, &f.diagnostic)
			&& !f.registry->setAgentTagMaximumRoutePlanningTime(f.tag, { 5.0f, 4.0f }, &f.diagnostic),
			"Reversed sampling range was accepted");
		require(f.registry->setAgentTagMinimumRoutePlanningTime(f.tag, { 0.1f, 10.0f }, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagMaximumRoutePlanningTime(f.tag, { 0.1f, 10.0f }, &f.diagnostic), f.diagnostic);
		auto const minimum = f.agent()->getMinimumRoutePlanningTimeSample();
		auto const maximum = f.agent()->getMaximumRoutePlanningTimeSample();
		require(minimum && maximum && minimum->value >= 0.1f && minimum->value <= 10
			&& maximum->value >= 0.1f && maximum->value <= 10
			&& minimum->sourceTag == f.tag && maximum->sourceTag == f.tag
			&& minimum->propertyRevision == f.registry->getAgentTagMinimumRoutePlanningTime(f.tag)->revision
			&& maximum->propertyRevision == f.registry->getAgentTagMaximumRoutePlanningTime(f.tag)->revision,
			"Sampling lost bounds, source, or revision");
		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 10.0f, &f.diagnostic), f.diagnostic);
		require(f.agent()->getEffectiveMaximumRoutePlanningTime().value == 10
			&& !f.agent()->getEffectiveMaximumRoutePlanningTime().individual
			&& f.agent()->getMaximumRoutePlanningTimeSample() == maximum,
			"Minimum override rewrote the inherited maximum");
		require(f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 0.1f, &f.diagnostic), f.diagnostic);
		require(f.agent()->getIndividualMaximumRoutePlanningTime() == 0.1f
			&& f.agent()->getEffectiveMaximumRoutePlanningTime().value == 10,
			"Crossed individual endpoints were rejected or rewritten");
		require(f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, std::nullopt, &f.diagnostic), f.diagnostic);
		require(f.agent()->getEffectiveMinimumRoutePlanningTime().value == minimum->value
			&& f.agent()->getEffectiveMaximumRoutePlanningTime().value == minimum->value,
			"Removing minimum did not independently reveal its sample");
		require(f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, std::nullopt, &f.diagnostic), f.diagnostic);
		require(f.agent()->getMinimumRoutePlanningTimeSample() == minimum
			&& f.agent()->getMaximumRoutePlanningTimeSample() == maximum,
			"Individual overrides changed underlying samples");
		require(f.world->resumeSimulation(), "Planning time fixture could not resume");
		require(!f.world->setAgentIndividualMinimumRoutePlanningTime(f.id, 2.0f, &f.diagnostic)
			&& !f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 4.0f, &f.diagnostic)
			&& !f.registry->setAgentTagMaximumRoutePlanningTime(f.tag, { 2.0f, 4.0f }, &f.diagnostic),
			"Planning time edits were allowed during simulation");
	}

	void persistenceReconciliationAndDependencies()
	{
		Fixture f;
		// Previous schemas have neither property and must remain clean on load.
		auto legacyWorld = YAML::Load(serialize(*f.world));
		legacyWorld["world"]["version"] = 28;
		auto legacyRegistry = YAML::Load(serialize(*f.registry));
		legacyRegistry["agentTagRegistry"]["version"] = 12;
		auto registry = core::AgentTagRegistry::create();
		deserialize(*registry, YAML::Dump(legacyRegistry));
		auto loaded = std::make_shared<core::World>("Loaded", 1, 1);
		deserialize(*loaded, YAML::Dump(legacyWorld));
		loaded->resolveAgentTagRegistry(registry);
		require(!loaded->isModified() && !registry->isModified()
			&& loaded->lookupAgent(f.id).entity->getEffectiveMinimumRoutePlanningTime().value == 1
			&& loaded->lookupAgent(f.id).entity->getEffectiveMaximumRoutePlanningTime().value == 3,
			"Legacy defaults dirtied the documents");

		f.addProperties();
		require(f.registry->setAgentTagMinimumRoutePlanningTime(f.tag, { 4.0f, 6.0f }, &f.diagnostic), f.diagnostic);
		require(f.registry->setAgentTagMaximumRoutePlanningTime(f.tag, { 1.0f, 2.0f }, &f.diagnostic), f.diagnostic);
		require(f.world->setAgentIndividualMaximumRoutePlanningTime(f.id, 0.1f, &f.diagnostic), f.diagnostic);
		auto const minimum = f.agent()->getMinimumRoutePlanningTimeSample();
		auto const maximum = f.agent()->getMaximumRoutePlanningTimeSample();
		auto const yaml = serialize(*f.world);
		auto clonedRegistry = core::AgentTagRegistry::create();
		deserialize(*clonedRegistry, serialize(*f.registry));
		deserialize(*loaded, yaml);
		loaded->resolveAgentTagRegistry(clonedRegistry);
		require(!loaded->isModified()
			&& loaded->lookupAgent(f.id).entity->getMinimumRoutePlanningTimeSample() == minimum
			&& loaded->lookupAgent(f.id).entity->getMaximumRoutePlanningTimeSample() == maximum
			&& loaded->lookupAgent(f.id).entity->getIndividualMaximumRoutePlanningTime() == 0.1f,
			"World/registry round trip changed planning time state");

		// Reopen old samples against an evolved registry through the public resolver.
		require(f.registry->setAgentTagMinimumRoutePlanningTime(f.tag, { 7.0f, 7.0f }, &f.diagnostic), f.diagnostic);
		require(f.registry->removeAgentTagMaximumRoutePlanningTime(f.tag, &f.diagnostic), f.diagnostic);
		deserialize(*loaded, yaml);
		loaded->pauseSimulation();
		loaded->resolveAgentTagRegistry(f.registry);
		require(loaded->isModified()
			&& loaded->lookupAgent(f.id).entity->getMinimumRoutePlanningTimeSample()->value == 7
			&& !loaded->lookupAgent(f.id).entity->getMaximumRoutePlanningTimeSample()
			&& loaded->lookupAgent(f.id).entity->getIndividualMaximumRoutePlanningTime() == 0.1f,
			"Reconciliation did not replace stale samples and clear removed samples independently");

		auto const other = f.registry->addAgentTag("other");
		require(f.world->assignAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(!f.registry->addAgentTagMinimumRoutePlanningTime(other, &f.diagnostic),
			"Registry addition allowed conflicting inherited minimums");
		require(f.registry->addAgentTagMaximumRoutePlanningTime(f.tag, &f.diagnostic), f.diagnostic);
		require(!f.registry->addAgentTagMaximumRoutePlanningTime(other, &f.diagnostic),
			"Registry addition allowed conflicting inherited maximums");
		require(f.world->removeAgentTag(f.id, other, &f.diagnostic), f.diagnostic);
		require(f.registry->addAgentTagMinimumRoutePlanningTime(other, &f.diagnostic), f.diagnostic);
		require(!f.world->assignAgentTag(f.id, other, &f.diagnostic),
			"Assignment allowed conflicting planning times");
	}

}

namespace routing_smoke
{
	void registerPlanningTime(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "defaultsValidationAndIndependentOverrides", [](smoke::Context const&)
		{
			defaultsValidationAndIndependentOverrides();
		} });
		checks.push_back({ "persistenceReconciliationAndDependencies", [](smoke::Context const&)
		{
			persistenceReconciliationAndDependencies();
		} });
	}
}
