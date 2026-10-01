// Migrated from AgentColourSmokeChecks.cpp (#286); core dependency tier.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		explicit TemporaryDirectory(std::string const& purpose)
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-tag-colour-" + purpose + "-"
					+ std::to_string(std::chrono::steady_clock::now()
						.time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	bool isPastelPaletteColour(core::AgentColour const& colour)
	{
		for (auto const& candidate : core::AgentTagColourPalette)
			if (candidate == colour) return true;
		return false;
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
		require(registry->deserialize(*reader, work), "The Agent tag registry did not deserialize");
		return registry;
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work), "The World did not deserialize");
		return world;
	}

	void colourIsUniqueRevisionedAndPersisted()
	{
		// The editor boundary carries exactly three byte channels. Every possible
		// byte survives the float working representation used by ColorEdit3.
		for (int value = 0; value < 256; ++value)
		{
			auto const channel = static_cast<uint8_t>(value);
			float rgb[3];
			core::agentColourToFloats({ channel, 0, 0 }, rgb);
			require(core::agentColourFromFloats(rgb).r == channel,
				"An Agent Colour red channel did not survive its editor round-trip");
			core::agentColourToFloats({ 0, channel, 0 }, rgb);
			require(core::agentColourFromFloats(rgb).g == channel,
				"An Agent Colour green channel did not survive its editor round-trip");
			core::agentColourToFloats({ 0, 0, channel }, rgb);
			require(core::agentColourFromFloats(rgb).b == channel,
				"An Agent Colour blue channel did not survive its editor round-trip");
		}

		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("crew");
		registry->markUnmodified();
		std::string diagnostic;

		require(registry->addAgentTagColour(tag, &diagnostic),
			"Default Colour addition failed: " + diagnostic);
		auto const* added = registry->getAgentTagColour(tag);
		require(added && added->value == core::EditorDefaultAgentColour
			&& added->revision == 1 && registry->getNextPropertyRevision() == 2,
			"Colour did not start at RGB (179, 77, 77) with revision 1");
		auto const afterAdd = serializeRegistry(*registry);
		require(!registry->addAgentTagColour(tag, &diagnostic)
			&& diagnostic.find("already has Colour") != std::string::npos
			&& serializeRegistry(*registry) == afterAdd,
			"A tag accepted a second Colour or changed on refusal");

		registry->markUnmodified();
		require(!registry->setAgentTagColour(tag, core::EditorDefaultAgentColour, &diagnostic)
			&& registry->getNextPropertyRevision() == 2 && !registry->isModified(),
			"An unchanged Colour consumed a revision or dirtied the registry");
		require(registry->setAgentTagColour(tag, { 12, 34, 56 }, &diagnostic),
			"A real Colour edit failed: " + diagnostic);
		auto const* edited = registry->getAgentTagColour(tag);
		require(edited && edited->value == (core::AgentColour{ 12, 34, 56 })
			&& edited->revision == 2 && registry->getNextPropertyRevision() == 3,
			"A real Colour edit did not allocate exactly one new revision");

		auto reopened = deserializeRegistry(serializeRegistry(*registry));
		auto const* persisted = reopened->getAgentTagColour(tag);
		require(persisted && *persisted == *edited
			&& reopened->getNextPropertyRevision() == 3,
			"Colour value, revision, or allocator did not survive persistence");
		require(reopened->removeAgentTagColour(tag, &diagnostic)
			&& reopened->getNextPropertyRevision() == 3,
			"Removing Colour unexpectedly consumed a revision");
		require(reopened->addAgentTagColour(tag, &diagnostic)
			&& reopened->getAgentTagColour(tag)->revision == 3
			&& reopened->getNextPropertyRevision() == 4,
			"Removing and re-adding Colour reused an old revision");

		// The persisted closed property set rejects both unknown types and a
		// duplicate Colour, rather than silently choosing one.
		auto unknown = YAML::Load(serializeRegistry(*reopened));
		unknown["tags"][0]["properties"][0]["type"] = "future-property";
		bool unknownRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(unknown)); }
		catch (std::exception const& error)
		{
			unknownRefused = std::string(error.what()).find("Unsupported Agent property type")
				!= std::string::npos;
		}
		require(unknownRefused, "An unknown persisted Agent property type was accepted");

		auto document = YAML::Load(serializeRegistry(*reopened));
		YAML::Node duplicate(YAML::NodeType::Map);
		duplicate["type"] = "colour";
		duplicate["revision"] = 3;
		duplicate["r"] = 179;
		duplicate["g"] = 77;
		duplicate["b"] = 77;
		document["tags"][0]["properties"].push_back(duplicate);
		bool duplicateRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(document)); }
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("more than one Colour")
				!= std::string::npos;
		}
		require(duplicateRefused, "Serialized duplicate Colour properties were accepted");
	}

	struct ColourFixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{
			std::make_shared<core::World>("Colours", 8, 2) };
		core::AgentTagId red{ registry->addAgentTag("red") };
		core::AgentTagId blue{ registry->addAgentTag("blue") };
		core::AgentTagId plain{ registry->addAgentTag("plain") };
		core::AgentId coloured{};
		core::AgentId fallback{};

		ColourFixture()
		{
			std::string diagnostic;
			require(registry->addAgentTagColour(red, &diagnostic), diagnostic);
			require(registry->setAgentTagColour(red, { 12, 34, 56 }, &diagnostic), diagnostic);
			world->attachAgentTagRegistry("colours.tags.yaml", registry);
			auto const corridor = world->addCorridor(0, 0, 7);
			world->finishBuild();
			coloured = world->createAgent("Coloured", corridor, 0, 1.5f);
			fallback = world->createAgent("Fallback", corridor, 0, 3.5f);
			world->pauseSimulation();
			require(world->assignAgentTag(coloured, red, &diagnostic), diagnostic);
		}
	};

	void closedWorldConflictsAreRejectedWhenTheRegistryIsResolved()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const first = registry->addAgentTag("first");
		auto const second = registry->addAgentTag("second");
		std::string yaml;
		{
			auto world = std::make_shared<core::World>("Closed", 6, 2);
			world->attachAgentTagRegistry("closed.tags.yaml", registry);
			auto const corridor = world->addCorridor(0, 0, 5);
			world->finishBuild();
			auto const agent = world->createAgent("Conflict", corridor);
			world->pauseSimulation();
			std::string diagnostic;
			require(world->assignAgentTag(agent, first, &diagnostic)
				&& world->assignAgentTag(agent, second, &diagnostic), diagnostic);
			yaml = serializeWorld(*world);
		}

		std::string diagnostic;
		require(registry->addAgentTagColour(first, &diagnostic)
			&& registry->addAgentTagColour(second, &diagnostic), diagnostic);
		auto reopened = deserializeWorld(yaml);
		bool refused{ false };
		try { reopened->resolveAgentTagRegistry(registry); }
		catch (std::exception const& error)
		{
			auto const text = std::string(error.what());
			refused = text.find("Colour") != std::string::npos
				&& text.find("#first") != std::string::npos
				&& text.find("#second") != std::string::npos;
		}
		require(refused && !reopened->hasAttachedAgentTagRegistry(),
			"A closed World with duplicate inherited Colour sources was attached");
	}

	void displayColourIsRandomAtCreationBackfilledAndPersisted()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const created = registry->addAgentTag("created");
		require(isPastelPaletteColour(registry->getAgentTagDisplayColour(created)),
			"A new tag did not receive a random pastel display Colour");
		require(!registry->getAgentTagColour(created),
			"A new tag's display Colour leaked into the inherited Agent Colour property");

		TemporaryDirectory temporary("backfill");
		auto const path = temporary.path / "legacy.tags.yaml";
		auto document = YAML::Load(serializeRegistry(*registry));
		document["tags"][0].remove("displayColour");
		{
			std::ofstream output(path, std::ios::binary);
			require(static_cast<bool>(output),
				"Could not write the legacy registry file");
			output << YAML::Dump(document);
		}

		auto loaded = core::AgentTagRegistry::loadFrom(path.string());
		auto const backfilled = loaded->getAgentTagDisplayColour(created);
		require(isPastelPaletteColour(backfilled),
			"A legacy tag without a display Colour was not backfilled with a pastel");
		require(loaded->isModified(),
			"Backfilling display Colours did not mark the registry modified for saving");

		auto reopened = deserializeRegistry(serializeRegistry(*loaded));
		require(reopened->getAgentTagDisplayColour(created) == backfilled,
			"A backfilled display Colour did not survive persistence");
		require(!reopened->isModified(),
			"Re-deserializing a complete document backfilled or dirtied state");
	}
}

void agent_smoke::registerColour(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "colourIsUniqueRevisionedAndPersisted", [](smoke::Context const&) { colourIsUniqueRevisionedAndPersisted(); } });
	checks.push_back({ "displayColourIsRandomAtCreationBackfilledAndPersisted", [](smoke::Context const&) { displayColourIsRandomAtCreationBackfilledAndPersisted(); } });
	checks.push_back({ "closedWorldConflictsAreRejectedWhenTheRegistryIsResolved", [](smoke::Context const&) { closedWorldConflictsAreRejectedWhenTheRegistryIsResolved(); } });
}
