#include "Checks.h"
#include "TemporaryDirectory.h"
// Property-free Agent tag assignments, ticket #131.

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	using tag_smoke::TemporaryDirectory;
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(world->deserialize(*reader, workData), "The World did not deserialize");
		return world;
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write an Agent tag fixture");
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId crew;
		core::AgentTagId night;
		core::AgentId alice;
		uint32_t corridor;

		Fixture()
			: world(std::make_shared<core::World>("Tag assignments", 10, 3))
			, registry(core::AgentTagRegistry::create())
		{
			crew = registry->addAgentTag("crew");
			night = registry->addAgentTag("night-shift");
			world->attachAgentTagRegistry("shared.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			alice = world->createAgent("Alice", corridor, 0, 1.5f);
		}
	};

	void assignmentsAreUniquePausedOnlyAndTransactional()
	{
		Fixture fixture;
		std::string diagnostic;
		require(fixture.world->getAgentTags(fixture.alice).empty(),
			"A newly created Agent did not start untagged");

		require(!fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& !diagnostic.empty(),
			"An Agent tag was assigned while the simulation was running");
		require(fixture.world->getAgentTags(fixture.alice).empty(),
			"A running-simulation refusal partially assigned a tag");

		fixture.world->pauseSimulation();
		require(fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"Two distinct property-free Agent tags could not be assigned");
		auto const expected = std::set<core::AgentTagId>{ fixture.crew, fixture.night };
		require(fixture.world->getAgentTags(fixture.alice) == expected,
			"Agent tag assignments are not exposed as one stable set");

		auto const beforeRefusals = serializeWorld(*fixture.world);
		require(!fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"A duplicate Agent tag assignment was accepted");
		require(!fixture.world->assignAgentTag(core::AgentId{ 9999 }, fixture.crew, &diagnostic),
			"An unknown Agent received a tag");
		require(!fixture.world->assignAgentTag(fixture.alice, core::AgentTagId{ 9999 }, &diagnostic),
			"An unknown Agent tag was assigned");
		require(!fixture.world->removeAgentTag(fixture.alice, core::AgentTagId{ 9999 }, &diagnostic),
			"An unknown Agent tag was removed");
		require(serializeWorld(*fixture.world) == beforeRefusals,
			"A refused assignment operation partially mutated the World");

		require(fixture.world->removeAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& !fixture.world->lookupAgent(fixture.alice).entity->hasAgentTag(fixture.crew)
			&& fixture.world->lookupAgent(fixture.alice).entity->hasAgentTag(fixture.night),
			"An assigned tag was not independently removable");

		auto noRegistry = std::make_shared<core::World>("No registry", 6, 2);
		auto const corridor = noRegistry->addCorridor(0, 0, 4);
		noRegistry->finishBuild();
		auto const agent = noRegistry->createAgent("No tags", corridor);
		noRegistry->pauseSimulation();
		require(!noRegistry->assignAgentTag(agent, fixture.crew, &diagnostic)
			&& diagnostic.find("registry") != std::string::npos
			&& noRegistry->getAgentTags(agent).empty(),
			"Assignment without a registry was not refused atomically");
	}

	void assignmentsSerializeInNumericOrderAndRejectMalformedInput()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		// Deliberately assign in descending ID order.
		require(fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The serialization fixture could not assign its tags");

		auto const yaml = serializeWorld(*fixture.world);
		auto document = YAML::Load(yaml);
		auto tags = document["agents"][0]["agent"]["tags"];
		require(tags && tags.IsSequence() && tags.size() == 2
			&& tags[0].as<uint64_t>() == fixture.crew.value
			&& tags[1].as<uint64_t>() == fixture.night.value,
			"Agent tag IDs did not serialize in stable numeric order:\n" + yaml);

		document["agents"][0]["agent"]["tags"].push_back(fixture.crew.value);
		bool duplicateRefused{ false };
		try { (void)deserializeWorld(YAML::Dump(document)); }
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("unique") != std::string::npos;
		}
		require(duplicateRefused, "Duplicate Agent tag IDs in input were not rejected");

		auto withoutRegistry = YAML::Load(yaml);
		withoutRegistry.remove("agentTagRegistry");
		bool absentRegistryRefused{ false };
		try { (void)deserializeWorld(YAML::Dump(withoutRegistry)); }
		catch (std::exception const& error)
		{
			absentRegistryRefused = std::string(error.what()).find("no Agent tag registry")
				!= std::string::npos;
		}
		require(absentRegistryRefused,
			"Serialized assignments without a registry reference were accepted");
	}

	void saveReopenAndUnknownTagValidationUseStableIds(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic)
			&& fixture.world->assignAgentTag(fixture.alice, fixture.night, &diagnostic),
			"The reopen fixture could not assign its tags");

		auto const registryPath = temporary.path / "shared.tags.yaml";
		auto const worldPath = temporary.path / "world.world.yaml";
		fixture.registry->saveTo(registryPath.string());
		fixture.world->saveTo(worldPath.string());

		auto reopened = core::loadWorldDocument(worldPath);
		auto const reopenedAgent = reopened->lookupAgent(fixture.alice);
		require(reopenedAgent
			&& reopenedAgent.entity->getAgentTagIds()
				== std::set<core::AgentTagId>{ fixture.crew, fixture.night },
			"Agent tag stable IDs did not survive save and reopen");

		auto malformed = YAML::Load(serializeWorld(*fixture.world));
		malformed["agents"][0]["agent"]["tags"][0] = 9999;
		auto const malformedPath = temporary.path / "malformed.world.yaml";
		writeText(malformedPath, YAML::Dump(malformed));
		// The registry basename in the fixture is shared.tags.yaml, so this file
		// resolves the same adjacent registry before validating assignments.
		bool unknownRefused{ false };
		try { (void)core::loadWorldDocument(malformedPath); }
		catch (std::exception const& error)
		{
			unknownRefused = std::string(error.what()).find("does not define")
				!= std::string::npos;
		}
		require(unknownRefused, "A serialized assignment to an unknown tag was accepted");
	}

}

void tag_smoke::registerAssignment(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "assignmentsAreUniquePausedOnlyAndTransactional",
		[](smoke::Context const&)
		{
			assignmentsAreUniquePausedOnlyAndTransactional();
		} });
	checks.push_back({ "assignmentsSerializeInNumericOrderAndRejectMalformedInput",
		[](smoke::Context const&)
		{
			assignmentsSerializeInNumericOrderAndRejectMalformedInput();
		} });
	checks.push_back({ "saveReopenAndUnknownTagValidationUseStableIds",
		[](smoke::Context const& context)
		{
			saveReopenAndUnknownTagValidationUseStableIds(context);
		} });
}
