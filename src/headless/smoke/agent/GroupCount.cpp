// Migrated from AgentGroupCountSmokeChecks.cpp (#285); core dependency tier.
#include <bit>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Edge.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Path.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/Vertex.h"
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

	// Every Agent the World owns, read off the spatial index, so a check
	// can cross-read a count against the Agents it claims to be counting.
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

	// The count every group reports, as one comparable line per group. The
	// way to ask whether anything other than an assignment moved a number.
	std::string allCounts(core::World const& world)
	{
		std::string out;
		for (auto const id : world.getAgentGroupIds())
		{
			out += world.getAgentGroupName(id) + "="
				+ std::to_string(world.getAgentGroupMemberCount(id)) + ";";
		}
		return out;
	}

	uint32_t countByScanningAgents(core::World const& world, core::AgentGroupId group)
	{
		uint32_t count{ 0 };
		for (auto const* agent : allAgents(world))
		{
			if (agent->getAgentGroupId() == group) ++count;
		}
		return count;
	}

	// The key names used inside one top-level YAML collection - "agentGroups",
	// "agents" - as a whole document is written. A count that had been written
	// down would show up here; the point of the check is that none is.
	std::vector<std::string> keysInCollection(std::string const& yaml,
		std::string const& collection)
	{
		std::vector<std::string> keys;

		std::istringstream in(yaml);
		std::string line;
		bool inside{ false };

		while (std::getline(in, line))
		{
			auto const start = line.find_first_not_of(" \t");
			if (start == std::string::npos) continue;

			if (start == 0)
			{
				// A top-level key: inside this collection from its own line
				// until the next top-level key that is not it.
				inside = line.compare(0, collection.size() + 1, collection + ":") == 0;
				continue;
			}
			if (!inside) continue;

			// An array item's first key sits on the same line as its "-", so
			// the dash is stepped over rather than the whole line skipped.
			auto content = start;
			if (line[content] == '-')
			{
				++content;
				while (content < line.size()
					&& (line[content] == ' ' || line[content] == '\t'))
				{
					++content;
				}
				if (content >= line.size()) continue;
			}

			auto const colon = line.find(':', content);
			if (colon == std::string::npos) continue;
			keys.push_back(line.substr(content, colon - content));
		}
		return keys;
	}

	std::string lowerCase(std::string const& value)
	{
		std::string out;
		out.reserve(value.size());
		for (auto const c : value)
			out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		return out;
	}

	// Nothing count-shaped may appear in the part of the document that carries
	// groups and Agents. A stored total is a second source of truth, and a
	// second source of truth is one that can be wrong.
	void requireNoCountShapedKeys(std::string const& yaml, std::string const& collection)
	{
		auto const keys = keysInCollection(yaml, collection);
		require(!keys.empty(),
			("The serialised document carried no \"" + collection
				+ "\" keys to inspect").c_str());

		for (auto const& key : keys)
		{
			auto const name = lowerCase(key);
			bool const countShaped = name.find("count") != std::string::npos
				|| name.find("member") != std::string::npos
				|| name.find("total") != std::string::npos
				|| name == "size";
			require(!countShaped,
				("The serialised \"" + collection + "\" collection carries a count-shaped key \""
					+ key + "\", which would be a second source of truth for membership").c_str());
		}
	}

	// A two-Storey world with two Locations on each Layer, so a group's
	// members are genuinely spread out and a count has to reach all of them.
	struct CountingWorldLayout
	{
		uint32_t frontCorridor{ 0 };
		uint32_t frontRoom{ 0 };
		uint32_t backRoom{ 0 };
		uint32_t backCorridor{ 0 };
	};

	CountingWorldLayout buildCountingWorldLayout(core::World& world)
	{
		CountingWorldLayout layout;
		layout.frontCorridor = world.addCorridor(0, 0, 12);
		layout.frontRoom = world.addRoom("Front room", 0, 2, 0, 6, 1);
		layout.backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		layout.backCorridor = world.addCorridor(1u, 2u, 0u, 12u, 1u);
		world.finishBuild();
		return layout;
	}

	// Assignment goes through the World and the count is read straight
	// back: no refresh, no notification, no second call to make it correct.
	void assign(core::World& world, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(world.setAgentGroup(agent, group, &diagnostic),
			("Assigning an Agent for a count check failed: " + diagnostic).c_str());
	}

	// A two-node path over one edge, the shape the editor gives an
	// explicitly-directed short move. Used here to send an Agent at a Door it
	// is not allowed to open.
	std::shared_ptr<core::Path> twoNodePath(std::shared_ptr<const core::Vertex> source,
		std::shared_ptr<const core::Vertex> destination, std::shared_ptr<const core::Edge> edge)
	{
		auto path = std::make_shared<core::Path>();
		path->nodes.push_back({ nullptr, std::move(source), 0.0f, std::nullopt, std::nullopt });
		path->nodes.push_back({ std::move(edge), std::move(destination), 1.0f, std::nullopt, std::nullopt });
		return path;
	}

	// ---------------------------------------------------------------- checks

	// A count is not a per-Layer or per-Sector figure. One call covers the
	// whole World, and members scattered over Layers and Locations all
	// land in the same number.
	void aCountCoversEveryLayerAndSector()
	{
		core::World world("Count spread", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");

		// Three Crew members: two on the front Layer in two different
		// Locations, one on the Layer behind.
		auto const frontCorridorAgent = world.createAgent("Corridor hand",
			layout.frontCorridor, 0, 1.5f);
		auto const frontRoomAgent = world.createAgent("Front desk",
			layout.frontRoom, 0, 1.0f);
		auto const backRoomAgent = world.createAgent("Back office",
			layout.backRoom, 0, 2.0f);
		// And two Agents that belong to nobody's count but one each.
		auto const otherGroupAgent = world.createAgent("Night porter",
			layout.backCorridor, 0, 3.0f);
		auto const ungroupedAgent = world.createAgent("Passing through",
			layout.frontCorridor, 0, 5.0f);

		assign(world, frontCorridorAgent, crew);
		assign(world, frontRoomAgent, crew);
		assign(world, backRoomAgent, crew);
		assign(world, otherGroupAgent, nightShift);

		require(world.getAgentGroupMemberCount(crew) == 3,
			("A group's count did not reach its Agents across Layers and Sectors: "
				+ allCounts(world)).c_str());
		require(world.getAgentGroupMemberCount(nightShift) == 1,
			("The second group's count did not stay on its own member: "
				+ allCounts(world)).c_str());

		// Each count agrees with the Agents that actually carry the ID.
		require(world.getAgentGroupMemberCount(crew)
			== countByScanningAgents(world, crew),
			"A count disagrees with the Agents carrying the group's ID");
		require(world.getAgentGroupMemberCount(nightShift)
			== countByScanningAgents(world, nightShift),
			"A second count disagrees with the Agents carrying the group's ID");

		// The ungrouped Agent is in neither count: it has no Agent group to
		// be counted under, and no group counts it.
		require(!world.getAgentGroup(ungroupedAgent),
			"The ungrouped test Agent picked up an Agent group");
		require(allAgents(world).size() == 5,
			"The test world did not place every Agent the count is meant to cover");
	}

	// Assigning, reassigning and clearing each move the numbers by exactly
	// one, and do it before anything else is asked.
	void countsTrackAssignReassignAndClearImmediately()
	{
		core::World world("Count transitions", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");
		auto const alpha = world.createAgent("Alpha", layout.frontCorridor, 0, 1.0f);
		auto const beta = world.createAgent("Beta", layout.backRoom, 0, 1.0f);

		assign(world, alpha, crew);
		require(world.getAgentGroupMemberCount(crew) == 1
			&& world.getAgentGroupMemberCount(nightShift) == 0,
			"Assigning the first Agent did not move the counts at once: " + allCounts(world));

		// Reassigning is one Agent leaving one group and joining another: both
		// counts move, in the same call, and neither keeps a straggler.
		assign(world, alpha, nightShift);
		require(world.getAgentGroupMemberCount(crew) == 0,
			"Reassigning an Agent left its previous group counting it");
		require(world.getAgentGroupMemberCount(nightShift) == 1,
			"Reassigning an Agent did not reach its new group's count");

		assign(world, beta, nightShift);
		require(world.getAgentGroupMemberCount(nightShift) == 2,
			"A second member did not reach the group's count: " + allCounts(world));

		assign(world, beta, {});
		require(world.getAgentGroupMemberCount(nightShift) == 1,
			"Clearing an Agent left its group counting it");
		require(world.getAgentGroupMemberCount(crew) == 0,
			"Clearing an Agent disturbed a group it was never in: " + allCounts(world));

		// Clearing an Agent that has nothing to clear is a no-op, not a
		// second decrement: a count cannot fall below the zero it sits at.
		assign(world, beta, {});
		require(world.getAgentGroupMemberCount(crew) == 0
			&& world.getAgentGroupMemberCount(nightShift) == 1,
			"Clearing an unassigned Agent changed a count: " + allCounts(world));

		// A refused assignment moves nothing, in either direction.
		std::string diagnostic;
		require(!world.setAgentGroup(alpha, core::AgentGroupId{ 4242 }, &diagnostic),
			"Assigning to an unknown Agent group succeeded");
		require(world.getAgentGroupMemberCount(nightShift) == 1,
			"A refused assignment changed a count");
		require(!world.setAgentGroup(core::AgentId{ 777 }, crew, &diagnostic),
			"Assigning an unknown Agent succeeded");
		require(allCounts(world) == "Crew=0;Night shift=1;",
			"A refused assignment moved the counts: " + allCounts(world));
	}

	// Movement is not membership. Agents sent walking, held waiting at a Door
	// they cannot open, and left idle keep whatever count their assignment
	// gave them - whatever Sector they end up in and whatever state they are
	// in while getting there.
	void movementNeverChangesACount()
	{
		core::World world("Count movement", 12, 3);
		auto const frontCorridor = world.addCorridor(0, 0, 8);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 8, 1);
		// A marker to walk to, and a Door that will not open: the two ways an
		// Agent stops being idle in a way that has nothing to do with groups.
		uint32_t destinationIdentifier{ 0x434e5431u };
		world.addSectorMarker(frontCorridor, 0, 7.5f, &destinationIdentifier);
		core::World::CreateDoorOptions unavailable;
		unavailable.activationMode = core::DoorActivationMode::Unavailable;
		world.addSectorDoor(0, 0, 4, unavailable);
		world.finishBuild();

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");

		auto const walker = world.createAgent("Walker", frontCorridor, 0, 0.5f);
		// Started close to the Door so the wait for it to be refused is short
		// rather than a walk across the whole Corridor.
		auto const waiter = world.createAgent("Waiter", frontCorridor, 0, 3.6f);
		auto const idler = world.createAgent("Idler", frontCorridor, 0, 6.5f);
		auto const backAgent = world.createAgent("Back hand", backRoom, 0, 1.0f);

		assign(world, walker, crew);
		assign(world, waiter, crew);
		assign(world, idler, crew);
		assign(world, backAgent, nightShift);

		auto const before = allCounts(world);
		require(before == "Crew=3;Night shift=1;",
			"The movement fixture is not what the check expects: " + before);

		auto* walkerAgent = world.lookupAgent(walker).entity;
		auto* waiterAgent = world.lookupAgent(waiter).entity;
		require(walkerAgent != nullptr && waiterAgent != nullptr,
			"The movement fixture could not find its Agents");

		auto const destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walking Agent's destination vertex is missing");
		auto route = world.getGraph()->calculatePath(walkerAgent, destination);
		require(route && !route->nodes.empty(), "The walking Agent's route could not be calculated");
		walkerAgent->setPath(std::move(route), true);

		// Send the second Agent at the Door it may not open, which is what
		// holds it in the waiting state rather than merely walking.
		std::shared_ptr<const core::Edge> doorEdge;
		for (auto const& candidate : world.getGraph()->getEdges())
		{
			if (candidate && candidate->getType() == core::EdgeType::Door)
			{
				doorEdge = candidate;
				break;
			}
		}
		require(doorEdge != nullptr, "The test Door produced no traversal edge to be denied");
		auto const first = doorEdge->getVertex(0);
		require(first != nullptr, "The test Door edge has no first vertex");
		auto const source = first->getSector()->getIndex() == frontCorridor
			? first : doorEdge->getVertex(1);
		auto const blocked = doorEdge->getOtherVertex(source);
		waiterAgent->setPath(twoNodePath(source, blocked, doorEdge), false);
		world.advanceTick();
		waiterAgent->startPathing();

		// Run until the blocked Agent is actually held at the Door.
		for (uint32_t tick = 0; tick < 240
			&& world.getSimulationSnapshot().traversalRequests.empty(); ++tick)
		{
			world.advanceTick();
		}
		require(waiterAgent->getState() == core::Agent::State::WaitingForTraversal,
			"The blocked Agent never reached the waiting state, so the check was weak");
		require(walkerAgent->getState() != core::Agent::State::Idle,
			"The walking Agent never started moving, so the check was weak");
		auto* idlerAgent = world.lookupAgent(idler).entity;
		require(idlerAgent != nullptr && idlerAgent->getState() == core::Agent::State::Idle,
			"The idle test Agent was moved by something other than an assignment");

		// Three different movement states, two Layers, four Locations - and
		// the counts are exactly what the assignments made them.
		require(allCounts(world) == before,
			"Agent movement changed a group's count: " + before + " -> "
			+ allCounts(world));

		// Keep running: the counts do not drift as ticks pass and Agents go
		// on changing Sector, state, or both.
		auto const settled = allCounts(world);
		for (uint32_t tick = 0; tick < 90; ++tick) world.advanceTick();
		require(allCounts(world) == settled,
			"Counts drifted while the simulation kept running: " + settled + " -> "
			+ allCounts(world));

		// And each count still matches the Agents carrying the ID, wherever
		// those Agents have got to.
		require(world.getAgentGroupMemberCount(crew)
			== countByScanningAgents(world, crew),
			"After movement, a count disagrees with the Agents carrying its ID");
		require(world.getAgentGroupMemberCount(nightShift)
			== countByScanningAgents(world, nightShift),
			"After movement, a second count disagrees with the Agents carrying its ID");
	}

	// An Agent the World no longer owns cannot still be counted. Removing
	// one takes it out of its group's number with no further bookkeeping,
	// which is the flip side of there being no counter to update.
	void aRemovedAgentLeavesTheCount()
	{
		core::World world("Count removal", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const first = world.createAgent("First", layout.frontCorridor, 0, 1.0f);
		auto const second = world.createAgent("Second", layout.backRoom, 0, 1.0f);
		assign(world, first, crew);
		assign(world, second, crew);
		require(world.getAgentGroupMemberCount(crew) == 2,
			"The removal fixture did not start with two members: " + allCounts(world));

		require(world.removeAgent(first).removed, "Removing a grouped Agent failed");
		require(world.getAgentGroupMemberCount(crew) == 1,
			"A removed Agent is still counted in its group");

		require(world.removeAgent(second).removed, "Removing the second grouped Agent failed");
		require(world.getAgentGroupMemberCount(crew) == 0,
			"A group still counted members after all of them were removed");
		require(countByScanningAgents(world, crew) == 0,
			"An Agent the World removed still carries the group's ID");
	}

	// Counting a group this World never issued is refused rather than
	// answered as zero, which would be indistinguishable from a group that
	// happens to be empty and would hide a borrowed or stale ID.
	void countingAnUnknownGroupIsRefused()
	{
		core::World world("Count unknown group", 12, 3);
		buildCountingWorldLayout(world);
		auto const crew = world.addAgentGroup("Crew");

		bool threw = false;
		try
		{
			(void)world.getAgentGroupMemberCount(core::AgentGroupId{ 90210 });
		}
		catch (core::WorldException const& error)
		{
			threw = true;
			require(std::string(error.what()).find("90210") != std::string::npos,
				("The unknown-group count refusal did not name the group: "
					+ std::string(error.what())).c_str());
		}
		require(threw, "Counting an Agent group the World never issued was accepted");

		// The defined group is untouched by the refused question.
		require(world.getAgentGroupMemberCount(crew) == 0,
			"A refused count question changed a real group's count");
	}

	// The serialized document carries group definitions and per-Agent
	// assignments and nothing else: no count field exists to go stale, and a
	// load rebuilds every count from what the Agents came back carrying.
	void countsReturnFromASaveLoadWithoutStoredCountData()
	{
		core::World world("Count round trip", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");
		world.addAgentGroup("Unstaffed");
		assign(world, world.createAgent("One", layout.frontCorridor, 0, 1.0f), crew);
		assign(world, world.createAgent("Two", layout.frontRoom, 0, 1.0f), crew);
		assign(world, world.createAgent("Three", layout.backRoom, 0, 1.0f), nightShift);
		world.createAgent("Ungrouped", layout.backCorridor, 0, 4.0f);

		auto const before = allCounts(world);
		require(before == "Crew=2;Night shift=1;Unstaffed=0;",
			"The round-trip fixture is not what the check expects: " + before);

		auto const yaml = serializeWorld(world);
		require(yaml.find("version: 43") != std::string::npos,
			"The count document was not written under the current World schema");

		// Neither half of the document writes the number down. The groups say
		// what they are called, the Agents say which one they belong to, and
		// the count is derived from the pair.
		requireNoCountShapedKeys(yaml, "agentGroups");
		requireNoCountShapedKeys(yaml, "agents");

		auto const loaded = loadWorld(yaml);
		require(loaded->getAgentGroupCount() == 3,
			"The reloaded World does not hold the groups that were saved");
		require(allCounts(*loaded) == before,
			"Counts did not come back from the round trip: " + before + " -> "
			+ allCounts(*loaded));

		// Re-saving produces the same document, so the loaded World is
		// neither drifting its memberships nor inventing anything to store.
		require(serializeWorld(*loaded) == yaml,
			"Re-saving a reloaded World produced a different document");

		// And the reloaded World keeps counting live: an assignment made
		// after the load moves the count exactly as it did before.
		assign(*loaded, loaded->createAgent("Four", layout.frontCorridor, 0, 2.0f), crew);
		require(loaded->getAgentGroupMemberCount(crew) == 3,
			"A count stopped tracking after a load: " + allCounts(*loaded));
	}
}

void agent_smoke::registerGroupCount(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "aCountCoversEveryLayerAndSector", [](smoke::Context const&) { aCountCoversEveryLayerAndSector(); } });
	checks.push_back({ "countsTrackAssignReassignAndClearImmediately", [](smoke::Context const&) { countsTrackAssignReassignAndClearImmediately(); } });
	checks.push_back({ "movementNeverChangesACount", [](smoke::Context const&) { movementNeverChangesACount(); } });
	checks.push_back({ "aRemovedAgentLeavesTheCount", [](smoke::Context const&) { aRemovedAgentLeavesTheCount(); } });
	checks.push_back({ "countingAnUnknownGroupIsRefused", [](smoke::Context const&) { countingAnUnknownGroupIsRefused(); } });
	checks.push_back({ "countsReturnFromASaveLoadWithoutStoredCountData", [](smoke::Context const&) { countsReturnFromASaveLoadWithoutStoredCountData(); } });
}
