// Migrated from AgentGroupAssignmentSmokeChecks.cpp (#285); core dependency tier.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/Simulation.h"
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

	std::string replaceOnce(std::string const& yaml, std::string const& find,
		std::string const& replace)
	{
		auto const found = yaml.find(find);
		require(found != std::string::npos,
			("The serialised World did not contain \"" + find + "\" to rewrite").c_str());
		return yaml.substr(0, found) + replace + yaml.substr(found + find.size());
	}

	std::vector<core::Agent const*> allAgents(core::World const& world)
	{
		std::vector<core::Agent const*> agents;
		for (uint32_t layer = 0; layer < world.getLayerCount(); ++layer)
		{
			for (auto const& sector : world.getSectors(layer))
			{
				if (!sector) continue;
				for (auto* agent : sector->getAgents())
				{
					if (agent) agents.push_back(agent);
				}
			}
		}
		return agents;
	}

	// A World with a little world in it, so an Agent is never the only
	// thing the document carries.
	void buildWorld(core::World& world)
	{
		world.addCorridor(0, 0, 8);
		world.addRoom("Depot", 0, 2, 0, 4, 1);
		world.finishBuild();
	}

	// ---------------------------------------------------------------- checks

	// No Agent is born with a group, and none is refused a clear on the
	// strength of having none.
	void everyAgentStartsWithNoAgentGroup()
	{
		core::World world("Assignment defaults", 12, 3);
		buildWorld(world);

		world.addAgentGroup("Crew");
		auto const first = world.createAgent("Alice", 0);
		auto const second = world.createAgent("Bob", 0);

		require(!world.lookupAgent(first).entity->getAgentGroupId(),
			"A newly created Agent came with an Agent group already assigned");
		require(!world.getAgentGroup(first),
			"World reported an Agent group for a freshly created Agent");
		require(!world.lookupAgent(second).entity->getAgentGroupId(),
			"A second newly created Agent came with an Agent group already assigned");

		// Clearing an Agent that has nothing to clear is a legitimate no-op,
		// not an error: `<none>` is a choice the user can always make.
		std::string diagnostic;
		require(world.canSetAgentGroup(first, {}, &diagnostic),
			("Clearing an Agent with no Agent group was refused: " + diagnostic).c_str());
		require(world.setAgentGroup(first, {}, &diagnostic),
			("Clearing an Agent with no Agent group failed: " + diagnostic).c_str());
		require(!world.lookupAgent(first).entity->getAgentGroupId(),
			"Clearing an Agent that had no Agent group gave it one");
	}

	// The World is the only way in, and it refuses both halves of a bad
	// assignment without moving anything.
	void anAgentCanBeAssignedAndClearedThroughTheWorld()
	{
		core::World world("Assignment API", 12, 3);
		buildWorld(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");
		auto const alice = world.createAgent("Alice", 0);

		std::string diagnostic;
		require(world.canSetAgentGroup(alice, crew, &diagnostic),
			("Assigning an Agent to a defined group was refused: " + diagnostic).c_str());
		require(world.setAgentGroup(alice, crew, &diagnostic),
			("Assigning an Agent to a defined group failed: " + diagnostic).c_str());
		require(world.lookupAgent(alice).entity->getAgentGroupId() == crew,
			"An assigned Agent does not report the group it was assigned to");
		require(world.getAgentGroup(alice) == crew,
			"World.getAgentGroup did not read back the assignment");

		// Re-assigning replaces the previous group rather than adding a second:
		// an Agent belongs to one Agent group, or to none.
		require(world.setAgentGroup(alice, nightShift, &diagnostic),
			("Reassigning an Agent failed: " + diagnostic).c_str());
		require(world.getAgentGroup(alice) == nightShift,
			"Reassigning an Agent left it on its previous group");

		require(world.setAgentGroup(alice, {}, &diagnostic),
			("Clearing an assignment failed: " + diagnostic).c_str());
		require(!world.getAgentGroup(alice),
			"Clearing an assignment left the Agent holding a group");

		// Unknown Agent: refused, with a reason, and nothing else moved.
		require(!world.canSetAgentGroup(core::AgentId{ 424242 }, crew, &diagnostic),
			"Assigning an Agent this World never issued succeeded");
		require(diagnostic.find("424242") != std::string::npos,
			("The unknown-Agent refusal did not name the Agent: " + diagnostic).c_str());
		require(!world.setAgentGroup(core::AgentId{ 424242 }, crew, &diagnostic),
			"World.setAgentGroup accepted an Agent it does not own");
		require(!diagnostic.empty(),
			"An unknown Agent was refused without a diagnostic");

		// Unknown group: refused, with the group's ID in the reason.
		require(!world.canSetAgentGroup(alice, core::AgentGroupId{ 999 }, &diagnostic),
			"Assigning an Agent to an Agent group this World never defined succeeded");
		require(diagnostic.find("999") != std::string::npos,
			("The unknown-group refusal did not name the group: " + diagnostic).c_str());
		require(!world.setAgentGroup(alice, core::AgentGroupId{ 999 }, &diagnostic),
			"World.setAgentGroup accepted a group it does not own");
		require(!world.getAgentGroup(alice),
			"A refused assignment still changed the Agent");
	}

	// A world with two groups and three Agents: one per group and one with
	// none, so a round trip has every case to carry.
	struct AssignmentFixture
	{
		std::string yaml;
		core::AgentGroupId crew{};
		core::AgentGroupId nightShift{};
		core::AgentId alice{};
		core::AgentId bob{};
		core::AgentId carol{};
	};

	AssignmentFixture assignmentFixture()
	{
		AssignmentFixture fixture;
		core::World world("Assignment round trip", 12, 3);
		buildWorld(world);

		fixture.crew = world.addAgentGroup("Crew");
		fixture.nightShift = world.addAgentGroup("Night shift");
		fixture.alice = world.createAgent("Alice", 0);
		fixture.bob = world.createAgent("Bob", 0);
		fixture.carol = world.createAgent("Carol", 0);

		std::string diagnostic;
		require(world.setAgentGroup(fixture.alice, fixture.crew, &diagnostic)
			&& world.setAgentGroup(fixture.bob, fixture.nightShift, &diagnostic),
			("Setting up the assignment round trip failed: " + diagnostic).c_str());

		fixture.yaml = serializeWorld(world);
		return fixture;
	}

	// Each assignment travels by ID and comes back as the same reference.
	void assignmentsRoundTripThroughSaveAndLoad()
	{
		auto const fixture = assignmentFixture();

		require(fixture.yaml.find("version: 57") != std::string::npos,
			"Agent group assignments were not written under the current World schema");
		require(fixture.yaml.find("group: 1") != std::string::npos
			&& fixture.yaml.find("group: 2") != std::string::npos,
			"The Agent group assignments were not persisted by ID");

		auto const loaded = loadWorld(fixture.yaml);
		require(loaded->getAgentGroup(fixture.alice) == fixture.crew,
			"An assigned Agent came back without its Agent group");
		require(loaded->getAgentGroup(fixture.bob) == fixture.nightShift,
			"A second assigned Agent came back without its Agent group");
		require(!loaded->getAgentGroup(fixture.carol),
			"An Agent with no Agent group came back with one");
		require(loaded->getAgentGroupName(loaded->getAgentGroup(fixture.alice)) == "Crew",
			"The reloaded assignment does not name the group it points at");

		// Re-saving what was just loaded produces the same document, so the
		// stored form is canonical and a load cannot drift it.
		require(serializeWorld(*loaded) == fixture.yaml,
			"Re-saving a reloaded World produced a different assignment document");

		// And the reloaded World still assigns, clears and refuses exactly
		// the way the original did.
		std::string diagnostic;
		require(loaded->setAgentGroup(fixture.carol, fixture.crew, &diagnostic),
			("Assigning on a reloaded World failed: " + diagnostic).c_str());
		require(loaded->getAgentGroup(fixture.carol) == fixture.crew,
			"An assignment made after a load was not kept");
		require(!loaded->setAgentGroup(fixture.alice, core::AgentGroupId{ 7777 }, &diagnostic),
			"A reloaded World accepted an Agent group it never defined");
		require(loaded->getAgentGroup(fixture.alice) == fixture.crew,
			"A refused assignment on a reloaded World changed the Agent");
	}

	// The hazard this guards is a silent downgrade: an assignment to a group
	// that is not in the file could be dropped on the floor and look like a
	// choice the user never made. It is refused instead, and refused before any
	// of the file's Agents is taken in, so the World is never left holding
	// part of a document it rejected.
	void aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn()
	{
		auto const fixture = assignmentFixture();
		// Bob's assignment points at a group the document never defines.
		auto const dangling = replaceOnce(fixture.yaml, "group: 2", "group: 99");

		core::World target("Dangling target", 12, 3);
		buildWorld(target);
		target.addAgentGroup("Existing");
		auto const ownAgent = target.createAgent("Mine", 0);
		require(ownAgent.value != 0, "The test target could not create its own Agent");

		bool refused = false;
		std::string reason;
		try
		{
			loadInto(target, dangling);
		}
		catch (core::SerializationException const& error)
		{
			refused = true;
			reason = error.what();
		}

		require(refused, "A version-9 Agent assignment to an unknown Agent group was accepted");
		require(reason.find("99") != std::string::npos,
			("The dangling-assignment refusal did not name the group: " + reason).c_str());
		require(reason.find("Bob") != std::string::npos,
			("The dangling-assignment refusal did not name the Agent: " + reason).c_str());

		// Nothing from the refused document was taken in: the target holds no
		// Agent at all, and certainly none pointing at a group it does not own.
		require(allAgents(target).empty(),
			"A refused dangling assignment still let some of the file's Agents in");
	}

	// A deterministic little walk: one Agent crossing a Corridor to a Marker.
	// The trace is the tick, every Agent snapshot, and every event the run
	// published, rendered as text so two runs can be compared as wholes.
	std::string walkTheCorridor(bool withGroups)
	{
		core::World world("Assignment walk", 12, 3);
		auto const corridor = world.addCorridor(0, 0, 8);
		uint32_t destinationIdentifier{ 0x41475231u };
		world.addSectorMarker(corridor, 0, 7.5f, &destinationIdentifier);
		world.finishBuild();

		core::AgentGroupId crew{};
		if (withGroups)
		{
			crew = world.addAgentGroup("Crew");
			world.addAgentGroup("Night shift");
		}

		auto const walker = world.createAgent("Walker", corridor, 0, 0.5f);
		if (withGroups)
		{
			std::string diagnostic;
			require(world.setAgentGroup(walker, crew, &diagnostic),
				("Grouping the walking Agent failed: " + diagnostic).c_str());
		}

		auto* agent = world.lookupAgent(walker).entity;
		auto const destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		auto path = world.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "The walk route could not be calculated");
		agent->setPath(std::move(path), true);

		auto const startGlobal = agent->getGlobalPosition();
		for (uint32_t tick = 0; tick < 90; ++tick) world.advanceTick();

		require(agent->getGlobalPosition().distanceTo(startGlobal) > 0.01f,
			"The test Agent did not actually move, so the comparison proved nothing");
		require(agent->getAgentGroupId() == (withGroups ? crew : core::AgentGroupId{}),
			"Grouping changed what the Agent itself reports about its group");

		std::ostringstream out;
		out << std::fixed << std::setprecision(6);

		auto const snapshot = world.getSimulationSnapshot();
		out << "tick=" << snapshot.tick
			<< " paused=" << snapshot.paused
			<< " topology=" << snapshot.topologyGeneration << "\n";
		for (auto const& entry : snapshot.agents)
		{
			out << "agent " << entry.id.value << ' ' << entry.name
				<< " sector=" << entry.sectorId.value
				<< " local=" << entry.localPosition.x << ',' << entry.localPosition.y
				<< " global=" << entry.globalPosition.x << ',' << entry.globalPosition.y
				<< " state=" << static_cast<int>(entry.state)
				<< " hasPath=" << entry.hasPath
				<< " node=" << entry.targetPathNode << '/' << entry.pathNodeCount
				<< " loco=" << entry.hasLocomotionTask
				<< " request=" << entry.traversalRequest.value
				<< " permit=" << entry.traversalPermit.value
				<< " interaction=" << entry.interactionRequest.value << "\n";
		}
		for (auto const& event : world.consumeSimulationEvents())
		{
			out << "event " << event.sequence << ':' << event.tick
				<< ':' << static_cast<int>(event.type)
				<< ':' << static_cast<int>(event.phase) << "\n";
		}
		return out.str();
	}

	// Grouping is editor metadata. Two Worlds that differ only in whether
	// their Agent has an Agent group must move, snapshot, and event alike.
	void groupingAnAgentChangesNothingInTheSimulation()
	{
		auto const ungrouped = walkTheCorridor(false);
		auto const grouped = walkTheCorridor(true);

		require(!ungrouped.empty() && !grouped.empty(),
			"The movement traces came back empty, so the comparison proved nothing");
		require(ungrouped == grouped,
			"Assigning an Agent to an Agent group changed its movement, runtime snapshot, "
			"or events:\n" + ungrouped + "\nvs\n" + grouped);
	}
}

void agent_smoke::registerGroupAssignment(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "everyAgentStartsWithNoAgentGroup", [](smoke::Context const&) { everyAgentStartsWithNoAgentGroup(); } });
	checks.push_back({ "anAgentCanBeAssignedAndClearedThroughTheWorld", [](smoke::Context const&) { anAgentCanBeAssignedAndClearedThroughTheWorld(); } });
	checks.push_back({ "assignmentsRoundTripThroughSaveAndLoad", [](smoke::Context const&) { assignmentsRoundTripThroughSaveAndLoad(); } });
	checks.push_back({ "aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn", [](smoke::Context const&) { aDanglingAssignmentRefusesTheFileBeforeAnyAgentIsTakenIn(); } });
	checks.push_back({ "groupingAnAgentChangesNothingInTheSimulation", [](smoke::Context const&) { groupingAnAgentChangesNothingInTheSimulation(); } });
}
