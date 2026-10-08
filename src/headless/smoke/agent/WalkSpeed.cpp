// Migrated from AgentWalkSpeedSmokeChecks.cpp (#286); core dependency tier.

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
#include "core/Staircase.h"
#include "core/StaircaseEdge.h"
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
			"The Walk speed registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work),
			"The Walk speed World did not deserialize");
		return world;
	}

	void rangesAreBoundedRevisionedAndPersisted()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("walkers");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		auto const* added = registry->getAgentTagWalkSpeedModifier(tag);
		require(added && added->range == core::DefaultAgentWalkSpeedModifierRange
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Walk speed did not default both endpoints to 1.0 at revision 1");

		auto const beforeInvalid = serializeRegistry(*registry);
		auto const revisionBeforeInvalid = registry->getNextPropertyRevision();
		for (auto const invalid : {
			core::AgentModifierRange{ 0.79f, 1.0f },
			core::AgentModifierRange{ 1.0f, 1.21f },
			core::AgentModifierRange{ 1.1f, 1.0f },
			core::AgentModifierRange{ std::numeric_limits<float>::infinity(), 1.0f },
			core::AgentModifierRange{ 1.0f, std::numeric_limits<float>::quiet_NaN() } })
		{
			require(!registry->setAgentTagWalkSpeedModifier(tag, invalid, &diagnostic)
				&& !diagnostic.empty() && serializeRegistry(*registry) == beforeInvalid
				&& registry->getNextPropertyRevision() == revisionBeforeInvalid,
				"An invalid Walk speed range changed the registry or revision allocator");
		}

		require(registry->setAgentTagWalkSpeedModifier(tag, { 0.8f, 1.2f }, &diagnostic),
			"A legal Walk speed range was refused: " + diagnostic);
		auto const expected = *registry->getAgentTagWalkSpeedModifier(tag);
		require(expected.revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Walk speed range edit did not allocate one revision");
		auto const yaml = serializeRegistry(*registry);
		require(yaml.find("type: walkSpeedModifier") != std::string::npos,
			"Walk speed property type was not persisted");
		auto reopened = deserializeRegistry(yaml);
		require(*reopened->getAgentTagWalkSpeedModifier(tag) == expected,
			"Walk speed endpoints or revision did not survive registry persistence");

		auto malformed = YAML::Load(yaml);
		malformed["tags"][0]["properties"][0]["min"] = 0.7f;
		bool refused{ false };
		try { (void)deserializeRegistry(YAML::Dump(malformed)); }
		catch (std::exception const& error)
		{
			refused = std::string(error.what()).find("between 0.8 and 1.2")
				!= std::string::npos;
		}
		require(refused, "A persisted out-of-bounds Walk speed range was accepted");
	}

	void independentAndFixedSamplesPersistAcrossLoadAndReset()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const varied = registry->addAgentTag("varied");
		auto const fixed = registry->addAgentTag("fixed");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(varied, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(varied, { 0.8f, 1.2f }, &diagnostic),
			diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(fixed, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(fixed, { 0.9f, 0.9f }, &diagnostic),
			diagnostic);

		auto world = std::make_shared<core::World>("Samples", 8, 2);
		world->attachAgentTagRegistry("samples.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		world->pauseSimulation();

		std::set<float> variedValues;
		core::AgentId retained{};
		for (int index = 0; index < 32; ++index)
		{
			auto const id = world->createAgent(
				"Varied " + std::to_string(index), corridor, 0, 1.0f);
			require(world->assignAgentTag(id, varied, &diagnostic), diagnostic);
			auto const& sample = world->lookupAgent(id).entity->getWalkSpeedModifierSample();
			require(sample && sample->type == core::SampledAgentPropertyType::WalkSpeedModifier
				&& sample->sourceTag == varied
				&& sample->propertyRevision
					== registry->getAgentTagWalkSpeedModifier(varied)->revision
				&& sample->value >= 0.8f && sample->value <= 1.2f,
				"A varied Agent sample lost its type, provenance, revision, or bounds");
			variedValues.insert(sample->value);
			if (index == 0) retained = id;
		}
		require(variedValues.size() > 1,
			"Independent non-degenerate Agent samples all received one shared value");

		auto const fixedAgent = world->createAgent("Fixed", corridor, 0, 2.0f);
		require(world->assignAgentTag(fixedAgent, fixed, &diagnostic), diagnostic);
		auto const fixedSample = world->lookupAgent(fixedAgent).entity
			->getWalkSpeedModifierSample();
		require(fixedSample && std::abs(fixedSample->value - 0.9f) < 0.000001f,
			"A degenerate Walk speed range did not return its exact endpoint");

		auto const retainedSample = *world->lookupAgent(retained).entity
			->getWalkSpeedModifierSample();
		auto const worldYaml = serializeWorld(*world);
		require(worldYaml.find("type: walkSpeedModifier") != std::string::npos
			&& worldYaml.find("sourceTag:") != std::string::npos
			&& worldYaml.find("propertyRevision:") != std::string::npos,
			"World persistence omitted Walk speed sample provenance");
		auto reopened = deserializeWorld(worldYaml);
		reopened->resolveAgentTagRegistry(registry);
		require(reopened->lookupAgent(retained).entity->getWalkSpeedModifierSample()
			== std::optional<core::AgentPropertySample>{ retainedSample },
			"The exact Walk speed sample did not survive save/load");

		reopened->resetSimulation();
		require(reopened->lookupAgent(retained).entity->getWalkSpeedModifierSample()
			== std::optional<core::AgentPropertySample>{ retainedSample },
			"Simulation reset rerolled or discarded the Walk speed sample");
	}

	void inheritedConflictsAreRefusedAtomically()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const first = registry->addAgentTag("first");
		auto const second = registry->addAgentTag("second");
		auto const pending = registry->addAgentTag("pending");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(first, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(second, &diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Conflicts", 6, 2);
		world->attachAgentTagRegistry("conflicts.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 5);
		world->finishBuild();
		auto const agent = world->createAgent("Tagged", corridor);
		world->pauseSimulation();
		require(world->assignAgentTag(agent, first, &diagnostic), diagnostic);
		auto const worldBefore = serializeWorld(*world);
		require(!world->assignAgentTag(agent, second, &diagnostic)
			&& diagnostic.find("Walk speed modifier") != std::string::npos
			&& diagnostic.find("#first") != std::string::npos
			&& diagnostic.find("#second") != std::string::npos
			&& serializeWorld(*world) == worldBefore,
			"A duplicate inherited Walk speed assignment was not refused atomically");

		require(world->assignAgentTag(agent, pending, &diagnostic), diagnostic);
		auto const registryBefore = serializeRegistry(*registry);
		auto const revisionBefore = registry->getNextPropertyRevision();
		require(!registry->addAgentTagWalkSpeedModifier(pending, &diagnostic)
			&& diagnostic.find("Walk speed modifier") != std::string::npos
			&& diagnostic.find("#first") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& registry->getNextPropertyRevision() == revisionBefore,
			"A conflicting property addition changed definitions or revisions");

		// Adding the property to an otherwise non-conflicting assigned tag samples
		// every assigned Agent at the property's exact default endpoint.
		auto const clean = registry->addAgentTag("clean");
		auto const cleanAgent = world->createAgent("Clean", corridor);
		require(world->assignAgentTag(cleanAgent, clean, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(clean, &diagnostic), diagnostic);
		auto const generated = world->lookupAgent(cleanAgent).entity
			->getWalkSpeedModifierSample();
		require(generated && generated->sourceTag == clean
			&& std::abs(generated->value - 1.0f) < 0.000001f,
			"Adding Walk speed to an assigned tag did not create its Agent sample");
	}

	void movementRouteTimeAndExplicitSpeedsUseTheRightObservations()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const slowTag = registry->addAgentTag("slow");
		auto const fastTag = registry->addAgentTag("fast");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(slowTag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(slowTag, { 0.8f, 0.8f }, &diagnostic),
			diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(fastTag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(fastTag, { 1.2f, 1.2f }, &diagnostic),
			diagnostic);

		auto world = std::make_shared<core::World>("Walking", 12, 2);
		world->attachAgentTagRegistry("walking.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 11);
		uint32_t sourceIdentifier{ 0x57313334u };
		uint32_t targetIdentifier{ 0x57313335u };
		world->addSectorMarker(corridor, 0, 1.5f, &sourceIdentifier);
		world->addSectorMarker(corridor, 0, 10.5f, &targetIdentifier);
		world->finishBuild();
		auto const slowId = world->createAgent("Slow", corridor, 0, 0.5f);
		auto const fastId = world->createAgent("Fast", corridor, 0, 0.5f);
		world->pauseSimulation();
		require(world->assignAgentTag(slowId, slowTag, &diagnostic), diagnostic);
		require(world->assignAgentTag(fastId, fastTag, &diagnostic), diagnostic);
		auto* slow = world->lookupAgent(slowId).entity;
		auto* fast = world->lookupAgent(fastId).entity;
		auto const& physical = slow->getPhysicalBaseline();
		require(std::abs(slow->getWalkSpeed() - physical.walkSpeed * 0.8f) < 0.00001f
			&& std::abs(fast->getWalkSpeed() - physical.walkSpeed * 1.2f) < 0.00001f,
			"Base walk speed did not use the sampled modifier");
		require(std::abs(slow->getClimbSpeed() - fast->getClimbSpeed()) < 0.000001f
			&& std::abs(slow->getClimbSpeed() - physical.climbSpeed) < 0.000001f,
			"Walk speed modifiers changed climb speed");

		auto const target = world->getGraph()->getVertexByIdentifier(targetIdentifier);
		auto slowPath = world->getGraph()->calculatePath(slow, target);
		auto fastPath = world->getGraph()->calculatePath(fast, target);
		require(slowPath && fastPath && slowPath->nodes.size() == fastPath->nodes.size()
			&& slowPath->nodes.back().cumulativePerceivedCost > fastPath->nodes.back().cumulativePerceivedCost,
			"Equal routes did not report a lower route time for the faster Agent");

		auto const slowStart = slow->getGlobalPosition();
		auto const fastStart = fast->getGlobalPosition();
		slow->setPath(std::move(slowPath), true);
		fast->setPath(std::move(fastPath), true);
		slow->update(0.1f);
		fast->update(0.1f);
		auto const slowDistance = slow->getGlobalPosition().distanceTo(slowStart);
		auto const fastDistance = fast->getGlobalPosition().distanceTo(fastStart);
		require(fastDistance > slowDistance
			&& std::abs(slowDistance - slow->getWalkSpeed() * 0.1f) < 0.0001f
			&& std::abs(fastDistance - fast->getWalkSpeed() * 0.1f) < 0.0001f,
			"Observable walking distance did not use each Agent's sample");

		auto staircase = std::make_shared<core::Staircase>(0, 0, 2, 1, 0.37f);
		core::StaircaseEdge escalator(staircase);
		require(std::abs(escalator.getTraversalSpeed(slow) - 0.37f) < 0.000001f
			&& std::abs(escalator.getTraversalSpeed(fast) - 0.37f) < 0.000001f,
			"An explicit transport movement speed was modified per Agent");
	}

}

void agent_smoke::registerWalkSpeed(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "walkSpeedRangesAreBoundedRevisionedAndPersisted", [](smoke::Context const&) { rangesAreBoundedRevisionedAndPersisted(); } });
	checks.push_back({ "walkSpeedIndependentAndFixedSamplesPersistAcrossLoadAndReset", [](smoke::Context const&) { independentAndFixedSamplesPersistAcrossLoadAndReset(); } });
	checks.push_back({ "walkSpeedInheritedConflictsAreRefusedAtomically", [](smoke::Context const&) { inheritedConflictsAreRefusedAtomically(); } });
	checks.push_back({ "movementRouteTimeAndExplicitSpeedsUseTheRightObservations", [](smoke::Context const&) { movementRouteTimeAndExplicitSpeedsUseTheRightObservations(); } });
}
