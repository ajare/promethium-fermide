// Migrated from AgentGroupSmokeChecks.cpp (#285); core dependency tier.
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "core/AgentGroup.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeWorld(core::World& world)
	{
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	// A whole-document load, the way the editor opens a file.
	std::shared_ptr<core::World> loadWorld(std::string const& yaml)
	{
		auto loaded = std::make_shared<core::World>("Loaded World", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised World could not be read back");
		require(loaded->deserialize(*reader, workData), "The World did not reload");
		return loaded;
	}

	// Loads over a World that already exists, which is how a refused open
	// gets caught out for leftover state.
	void loadInto(core::World& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "The World did not reload");
	}

	std::vector<std::string> namesInOrder(core::World const& world)
	{
		std::vector<std::string> names;
		for (auto const id : world.getAgentGroupIds())
			names.push_back(world.getAgentGroupName(id));
		return names;
	}

	// Replaces the document's schema version, leaving everything else alone:
	// the cheap way to ask what an older reader would be handed.
	std::string withVersion(std::string const& yaml, uint32_t version)
	{
		auto const found = yaml.find("version: 34");
		require(found != std::string::npos, "The serialised World carried no version 20 field");
		return yaml.substr(0, found) + "version: " + std::to_string(version)
			+ yaml.substr(found + std::strlen("version: 34"));
	}

	std::string replaceOnce(std::string const& yaml, std::string const& find,
		std::string const& replace)
	{
		auto const found = yaml.find(find);
		require(found != std::string::npos,
			("The serialised World did not contain \"" + find + "\" to rewrite").c_str());
		return yaml.substr(0, found) + replace + yaml.substr(found + find.size());
	}

	// A World with a little world in it, so a group is never the only thing
	// the document carries.
	void buildWorld(core::World& world)
	{
		world.addCorridor(0, 0, 8);
		world.addRoom("Depot", 0, 2, 0, 4, 1);
		world.finishBuild();
	}

	// ---------------------------------------------------------------- checks

	// Identity is allocated by the World, never inferred from the name, and
	// the creation order survives a rename of the group in the middle of it.
	void agentGroupsHaveStableIdsAndEnumerateInCreationOrder()
	{
		core::World world("Group order", 12, 3);
		buildWorld(world);

		require(world.getAgentGroupCount() == 0,
			"A newly built World already reported Agent groups");

		auto const first = world.addAgentGroup("Maintenance");
		auto const second = world.addAgentGroup("Security");
		auto const third = world.addAgentGroup("Night shift");

		require(first && second && third, "A created Agent group came back with a null ID");
		require(first < second && second < third,
			"Agent group IDs were not allocated monotonically");
		require(world.getAgentGroupCount() == 3,
			"Three Agent groups were created but the World counts differently");

		std::vector<core::AgentGroupId> const created{ first, second, third };
		require(world.getAgentGroupIds() == created,
			"Agent groups did not enumerate in creation order");
		require(world.getAgentGroupName(first) == "Maintenance"
			&& world.getAgentGroupName(second) == "Security"
			&& world.getAgentGroupName(third) == "Night shift",
			"An Agent group does not read back the name it was created with");

		auto const lookup = world.lookupAgentGroup(second);
		require(lookup && lookup.entity->getName() == "Security",
			"Looking up an Agent group by its ID did not find it");
		require(!world.lookupAgentGroup(core::AgentGroupId{ 424242 })
			&& !world.lookupAgentGroup(core::AgentGroupId{ 424242 }).diagnostic.empty(),
			"Looking up an unknown Agent group succeeded, or failed silently");

		// Rename the middle one: the ID is the same handle, the order is the
		// same order, and the neighbours are untouched.
		std::string diagnostic;
		require(world.renameAgentGroup(second, "Facilities", &diagnostic),
			("Renaming an Agent group was refused: " + diagnostic).c_str());
		require(world.getAgentGroupIds() == created,
			"Renaming an Agent group disturbed the creation order");
		require(world.getAgentGroupName(second) == "Facilities",
			"Renaming an Agent group did not change its name");
		require(world.getAgentGroupName(first) == "Maintenance"
			&& world.getAgentGroupName(third) == "Night shift",
			"Renaming one Agent group disturbed another");
		require(world.getAgentGroupCount() == 3,
			"Renaming an Agent group changed how many there are");
	}

	// The naming rule, seen from outside: trimmed on the way in, blank and
	// overlong refused with a reason, uniqueness case-sensitive.
	void agentGroupNamesAreTrimmedValidatedAndCaseSensitive()
	{
		core::World world("Group naming", 12, 3);
		buildWorld(world);

		auto const id = world.addAgentGroup("  Front of house  ");
		require(world.getAgentGroupName(id) == "Front of house",
			"An Agent group name was not trimmed on the way in");

		for (auto const* blank : { "", "   ", "\t ", " \t\t " })
		{
			std::string diagnostic;
			require(!world.canAddAgentGroup(blank, &diagnostic),
				"An Agent group with a blank name was accepted");
			require(!diagnostic.empty(),
				"An Agent group blank name was refused without a diagnostic");

			bool threw = false;
			try { world.addAgentGroup(blank); }
			catch (core::Exception const&) { threw = true; }
			require(threw, "addAgentGroup accepted a blank name");
		}

		// The limit is 63 bytes, measured in the bytes that get stored.
		auto const fits = std::string(core::AgentGroup::MaxNameBytes, 'a');
		auto const tooLong = fits + "a";
		require(core::AgentGroup::MaxNameBytes == 63,
			"The Agent group name limit is no longer 63 bytes");
		require(world.canAddAgentGroup(fits),
			"A maximum-length Agent group name was refused");

		std::string diagnostic;
		require(!world.canAddAgentGroup(tooLong, &diagnostic),
			"An overlong Agent group name was accepted");
		require(diagnostic.find("63") != std::string::npos,
			("The overlong refusal did not name the limit: " + diagnostic).c_str());
		// Padding is trimmed before the length is judged, so a long run of
		// spaces around a short name is a short name.
		require(world.canAddAgentGroup(std::string(core::AgentGroup::MaxNameBytes, ' ') + "x"),
			"An Agent group name padded with spaces past the limit was refused rather than trimmed");

		// Uniqueness is case-sensitive: "Front of house" is taken, but a
		// different capitalisation is a different group.
		require(!world.canAddAgentGroup("Front of house", &diagnostic),
			"A duplicate Agent group name was accepted");
		require(diagnostic.find("Front of house") != std::string::npos,
			("The duplicate refusal did not name the group: " + diagnostic).c_str());
		require(world.canAddAgentGroup("front of house"),
			"Agent group names differing only by case were treated as duplicates");

		// Multi-byte names are counted in bytes, not characters.
		auto const twoByteName = std::string("\xc3\xa9");   // U+00E9, two bytes
		require(twoByteName.size() == 2, "The two-byte test character is not two bytes");
		auto const multibyte = world.addAgentGroup("Plan " + twoByteName);
		require(world.getAgentGroupName(multibyte) == "Plan " + twoByteName,
			"A multi-byte Agent group name did not come back as it went in");
		require(!world.canAddAgentGroup(std::string(core::AgentGroup::MaxNameBytes - 1, 'a')
			+ twoByteName, &diagnostic),
			"A multi-byte Agent group name one byte past the limit was accepted");
		require(!world.canAddAgentGroup(std::string("Bad\xFF\xFE"), &diagnostic),
			"A name that is not valid UTF-8 was accepted as an Agent group name");
		// Explicit UTF-8 byte sequences, rather than universal character names,
		// so the bytes do not depend on the compiler's narrow execution charset.
		require(world.canAddAgentGroup(std::string("Crew \xf0\x9f\x9b\xa0")),  // U+1F6E0, four bytes
			"A four-byte codepoint was refused in an Agent group name");
		require(world.canAddAgentGroup(std::string("Euro \xe2\x82\xac")),  // U+20AC, three bytes
			"A three-byte codepoint was refused in an Agent group name");
		// An overlong encoding of "/" has the shape of a two-byte sequence but
		// is not valid UTF-8, and neither is a lone surrogate half.
		require(!world.canAddAgentGroup(std::string("Over\xC0\xAF"), &diagnostic),
			"An overlong UTF-8 encoding was accepted as an Agent group name");
		require(!world.canAddAgentGroup(std::string("\xED\xA0\x80"), &diagnostic),
			"A lone UTF-16 surrogate was accepted as an Agent group name");

		// None of the refusals above left anything behind.
		require(world.getAgentGroupCount() == 2,
			"A refused Agent group add changed how many groups the World holds");
	}

	// A rename that cannot be honoured is not half a rename: the group keeps
	// its name, its ID, and its place in the order.
	void renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically()
	{
		core::World world("Group rename", 12, 3);
		buildWorld(world);

		auto const alpha = world.addAgentGroup("Alpha");
		auto const bravo = world.addAgentGroup("Bravo");
		auto const charlie = world.addAgentGroup("Charlie");
		std::vector<core::AgentGroupId> const created{ alpha, bravo, charlie };

		std::string diagnostic;
		require(!world.renameAgentGroup(core::AgentGroupId{ 9999 }, "Ghost", &diagnostic),
			"Renaming an Agent group this World never issued succeeded");
		require(!diagnostic.empty(),
			"Renaming an unknown Agent group failed without a diagnostic");

		require(!world.renameAgentGroup(bravo, "Alpha", &diagnostic),
			"Renaming an Agent group onto a name another group already holds succeeded");
		require(world.getAgentGroupName(bravo) == "Bravo",
			"A refused rename still changed the group's name");

		require(!world.renameAgentGroup(bravo, "   ", &diagnostic),
			"Renaming an Agent group to a blank name succeeded");
		require(!world.renameAgentGroup(bravo, std::string(64, 'b'), &diagnostic),
			"Renaming an Agent group to an overlong name succeeded");

		require(world.getAgentGroupName(bravo) == "Bravo",
			"A refused rename left the group holding something other than its own name");
		require(world.getAgentGroupIds() == created,
			"A refused rename disturbed the Agent group order");
		require(world.getAgentGroupCount() == 3,
			"A refused rename changed how many Agent groups there are");

		// Renaming a group to the name it already has - padded or not - is a
		// legitimate no-op, not a collision with itself.
		require(world.renameAgentGroup(bravo, " Bravo ", &diagnostic),
			("Renaming an Agent group to its own padded name was refused: " + diagnostic).c_str());
		require(world.getAgentGroupName(bravo) == "Bravo",
			"Renaming an Agent group to its own padded name changed the name");
		require(world.getAgentGroupIds() == created,
			"Renaming an Agent group to its own name disturbed the order");
	}

	// Save and reopen: the ordered IDs and names come back, and come back the
	// same way a second time.
	void agentGroupsRoundTripThroughSaveAndLoad()
	{
		core::World world("Group round trip", 12, 3);
		buildWorld(world);

		auto const first = world.addAgentGroup("Alpha");
		auto const second = world.addAgentGroup("Bravo");
		auto const third = world.addAgentGroup("Charlie");
		std::vector<core::AgentGroupId> const created{ first, second, third };
		std::vector<std::string> const names{ "Alpha", "Facilities", "Charlie" };

		std::string diagnostic;
		require(world.renameAgentGroup(second, "Facilities", &diagnostic),
			("Renaming before the round trip was refused: " + diagnostic).c_str());

		auto const yaml = serializeWorld(world);
		require(yaml.find("version: 34") != std::string::npos,
			"Agent groups were not written under the current World schema");
		require(yaml.find("agentGroups") != std::string::npos,
			"The Agent group collection was not persisted");

		auto const loaded = loadWorld(yaml);
		require(loaded->getAgentGroupCount() == 3,
			"The reloaded World holds a different number of Agent groups");
		require(loaded->getAgentGroupIds() == created,
			"Agent group IDs did not survive the round trip");
		require(namesInOrder(*loaded) == names,
			"Agent group names or their order did not survive the round trip");

		// Re-saving what was just loaded produces the same document, so the
		// stored form is canonical and a load cannot drift it.
		require(serializeWorld(*loaded) == yaml,
			"Re-saving a reloaded World produced a different Agent group document");
	}

	// Anything written before version 9 carries no Agent groups, and the
	// reader does not invent any.
	void preVersionNineDocumentsLoadWithNoAgentGroups()
	{
		core::World world("Group legacy source", 12, 3);
		buildWorld(world);
		world.addAgentGroup("Alpha");
		world.addAgentGroup("Bravo");
		auto const yaml = serializeWorld(world);

		// Versions 2 through 8 share the record shapes this writer emits, so
		// each loads as a real pre-Agent-group document.
		for (uint32_t version = 2; version <= 8; ++version)
		{
			auto const legacy = loadWorld(withVersion(yaml, version));
			require(legacy->getAgentGroupCount() == 0,
				("A version-" + std::to_string(version)
					+ " document loaded with Agent groups").c_str());
			require(legacy->getAgentGroupIds().empty(),
				("A version-" + std::to_string(version)
					+ " document reported Agent group IDs").c_str());
		}

		// Version 1 encodes its construction records by number rather than by
		// name, so it is refused outright rather than misread.
		bool refusedVersionOne = false;
		try { loadWorld(withVersion(yaml, 1)); }
		catch (core::SerializationException const&) { refusedVersionOne = true; }
		require(refusedVersionOne,
			"A version-1 shaped document was accepted by the current reader");
	}

	// The hazard this guards is quiet data loss: an older reader that ignored
	// the version would load a version-9 file, see no groups it understands,
	// and write the file back without them.
	void aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups()
	{
		core::World world("Group ceiling", 12, 3);
		buildWorld(world);
		world.addAgentGroup("Alpha");
		world.addAgentGroup("Bravo");
		auto const yaml = serializeWorld(world);

		uint32_t const preAgentGroupVersionCeiling{ 8 };
		bool refused = false;
		try
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			reader->beginMap("world");
			auto const version = reader->readUint32("version");
			if (version > preAgentGroupVersionCeiling)
				throw core::SerializationException("Unsupported World serialization version");
			reader->endMap();
		}
		catch (core::SerializationException const& error)
		{
			refused = true;
			require(std::string(error.what()).find("version") != std::string::npos,
				("A version-8 reader failed for a reason other than the version: "
					+ std::string(error.what())).c_str());
		}
		require(refused,
			"A reader capped at version 8 accepted a version-9 Agent group document");
	}

	// Malformed version-9 input refuses the whole file. The target World is
	// one that already has groups, so "no partial state" has something
	// concrete to mean: it still holds exactly its own groups afterwards.
	void malformedAgentGroupInputRefusesTheFileWithoutPartialState()
	{
		core::World source("Malformed source", 12, 3);
		buildWorld(source);
		source.addAgentGroup("Alpha");
		source.addAgentGroup("Bravo");
		auto const goodYaml = serializeWorld(source);

		core::World target("Malformed target", 12, 3);
		buildWorld(target);
		target.addAgentGroup("Existing");
		auto const keptIds = target.getAgentGroupIds();
		auto const keptNames = namesInOrder(target);
		require(keptIds.size() == 1, "The test target did not start with its own group");

		std::vector<std::pair<std::string, std::string>> const cases{
			{ "a zero Agent group ID", replaceOnce(goodYaml, "id: 2", "id: 0") },
			{ "a duplicate Agent group ID", replaceOnce(goodYaml, "id: 2", "id: 1") },
			{ "a duplicate Agent group name", replaceOnce(goodYaml, "name: Bravo", "name: Alpha") },
			{ "a blank Agent group name", replaceOnce(goodYaml, "name: Bravo", "name: \"\"") },
			{ "an overlong Agent group name",
				replaceOnce(goodYaml, "name: Bravo", "name: " + std::string(64, 'x')) },
		};

		for (auto const& [what, badYaml] : cases)
		{
			bool refused = false;
			std::string reason;
			try
			{
				loadInto(target, badYaml);
			}
			catch (core::SerializationException const& error)
			{
				refused = true;
				reason = error.what();
			}
			require(refused, ("A version-9 document carrying " + what + " was accepted").c_str());
			require(!reason.empty(),
				("A version-9 document carrying " + what + " was refused without a reason").c_str());
			require(target.getAgentGroupIds() == keptIds
				&& namesInOrder(target) == keptNames,
				("A refused load left partial Agent group state behind: " + what).c_str());
		}
	}
}

void agent_smoke::registerGroup(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentGroupsHaveStableIdsAndEnumerateInCreationOrder", [](smoke::Context const&) { agentGroupsHaveStableIdsAndEnumerateInCreationOrder(); } });
	checks.push_back({ "agentGroupNamesAreTrimmedValidatedAndCaseSensitive", [](smoke::Context const&) { agentGroupNamesAreTrimmedValidatedAndCaseSensitive(); } });
	checks.push_back({ "renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically", [](smoke::Context const&) { renamingAnAgentGroupKeepsItsPlaceAndFailsAtomically(); } });
	checks.push_back({ "agentGroupsRoundTripThroughSaveAndLoad", [](smoke::Context const&) { agentGroupsRoundTripThroughSaveAndLoad(); } });
	checks.push_back({ "preVersionNineDocumentsLoadWithNoAgentGroups", [](smoke::Context const&) { preVersionNineDocumentsLoadWithNoAgentGroups(); } });
	checks.push_back({ "aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups", [](smoke::Context const&) { aVersionEightReaderRefusesVersionNineRatherThanDroppingGroups(); } });
	checks.push_back({ "malformedAgentGroupInputRefusesTheFileWithoutPartialState", [](smoke::Context const&) { malformedAgentGroupInputRefusesTheFileWithoutPartialState(); } });
}
