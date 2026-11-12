// Migrated from AgentColourSmokeChecks.cpp (#286); core dependency tier.
//
// What is left of "colour" in the Agent tag domain is tag display data: the
// pastel a tag's own chip is drawn in. The Agent Colour property that used
// to sit beside it is gone - an Agent draws its sprite as the atlas paints
// it, and no tag colour reaches that picture - so what these checks pin is
// the display Colour's own lifecycle and the persisted property-type gate
// that now refuses `colour` along with every other unknown type.

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

	bool isPastelPaletteColour(core::AgentColour const& colour)
	{
		for (auto const& pastel : core::AgentTagColourPalette)
			if (pastel == colour) return true;
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

	std::shared_ptr<core::AgentTagRegistry> deserializeRegistry(std::string const& yaml)
	{
		auto registry = core::AgentTagRegistry::create();
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData work;
		require(registry->deserialize(*reader, work), "The Agent tag registry did not deserialize");
		return registry;
	}

	// The editor boundary carries exactly three byte channels. Every possible
	// byte survives the float working representation ColorEdit3 uses, which
	// is what lets a display Colour be edited without drift.
	void agentColourChannelRoundTripThroughTheEditorBoundary()
	{
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
	}

	// The persisted property set is closed: a type the format does not know
	// is refused rather than skipped. The retired Agent Colour property now
	// lands in that same refusal, which is what stops an older document's
	// `colour` block from being quietly discarded as unrecognised noise.
	void unknownPersistedAgentPropertyTypesAreRefused()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("crew");
		std::string diagnostic;
		require(registry->addAgentTagEscalatorWalkingChance(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagEscalatorWalkingChance(tag, 0.5f, &diagnostic),
			diagnostic);

		auto unknown = YAML::Load(serializeRegistry(*registry));
		unknown["tags"][0]["properties"][0]["type"] = "future-property";
		bool unknownRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(unknown)); }
		catch (std::exception const& error)
		{
			unknownRefused = std::string(error.what()).find("Unsupported Agent property type")
				!= std::string::npos;
		}
		require(unknownRefused, "An unknown persisted Agent property type was accepted");

		auto retired = YAML::Load(serializeRegistry(*registry));
		retired["tags"][0]["properties"][0]["type"] = "colour";
		bool retiredRefused{ false };
		try { (void)deserializeRegistry(YAML::Dump(retired)); }
		catch (std::exception const& error)
		{
			retiredRefused = std::string(error.what()).find("Unsupported Agent property type 'colour'")
				!= std::string::npos;
		}
		require(retiredRefused,
			"A retired `colour` property in an older document was accepted");
	}

	void displayColourIsRandomAtCreationBackfilledAndPersisted(smoke::Context const& context)
	{
		auto registry = core::AgentTagRegistry::create();
		auto const created = registry->addAgentTag("created");
		require(isPastelPaletteColour(registry->getAgentTagDisplayColour(created)),
			"A new tag did not receive a random pastel display Colour");

		auto const directory = context.temporaryRoot() / "colour-backfill";
		std::filesystem::create_directory(directory);
		auto const path = directory / "legacy.tags.yaml";
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
	checks.push_back({ "agentColourChannelRoundTripThroughTheEditorBoundary", [](smoke::Context const&) { agentColourChannelRoundTripThroughTheEditorBoundary(); } });
	checks.push_back({ "unknownPersistedAgentPropertyTypesAreRefused", [](smoke::Context const&) { unknownPersistedAgentPropertyTypesAreRefused(); } });
	checks.push_back({ "displayColourIsRandomAtCreationBackfilledAndPersisted", [](smoke::Context const& context) { displayColourIsRandomAtCreationBackfilledAndPersisted(context); } });
}
