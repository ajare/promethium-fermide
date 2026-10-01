// Migrated from AgentGroupDeleteSmokeChecks.cpp (#285); core dependency tier.
#include <cstdint>
#include <algorithm>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
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

	// Every group the World reports, with its count, as one comparable
	// line. Used to ask whether anything other than the deletion moved.
	std::string groupSummary(core::World const& world)
	{
		std::string out;
		for (auto const id : world.getAgentGroupIds())
		{
			out += std::to_string(id.value) + ':' + world.getAgentGroupName(id) + '='
				+ std::to_string(world.getAgentGroupMemberCount(id)) + ';';
		}
		return out;
	}

	// Every Agent the World owns, with the group it carries, as a sorted
	// list of comparable entries. Sorted because the order Agents sit in a
	// Sector is not part of what an assignment is, and a reload is free to
	// put two Agents in the same room in a different order.
	std::vector<std::string> assignmentEntries(core::World const& world)
	{
		std::vector<std::string> entries;
		for (auto const* agent : allAgents(world))
		{
			auto const group = agent->getAgentGroupId();
			entries.push_back(std::string(agent->getName()) + "->"
				+ (group ? std::to_string(group.value) : std::string("<none>")));
		}
		std::sort(entries.begin(), entries.end());
		return entries;
	}

	std::string assignmentSummary(core::World const& world)
	{
		std::string out;
		for (auto const& entry : assignmentEntries(world)) out += entry + ";";
		return out;
	}

	// Nothing an Agent carries may name a group this World does not own.
	// The one state a save would refuse to read back, and the one state this
	// ticket exists to make unreachable.
	void requireNoDanglingAssignment(core::World const& world,
		std::string const& context)
	{
		for (auto const* agent : allAgents(world))
		{
			auto const group = agent->getAgentGroupId();
			if (!group) continue;
			require(static_cast<bool>(world.lookupAgentGroup(group)),
				("An Agent is left naming an Agent group this World does not own: "
					+ std::string(agent->getName()) + " -> " + std::to_string(group.value)
					+ " (" + context + ")").c_str());
		}
	}

	// ---------------------------------------------------------------- world

	// Two Layers, two Locations on the front one and two behind, so a group's
	// members are spread across the World rather than sitting side by side.
	struct DeleteWorldLayout
	{
		uint32_t frontCorridor{ 0 };
		uint32_t frontRoom{ 0 };
		uint32_t backRoom{ 0 };
		uint32_t backCorridor{ 0 };
	};

	DeleteWorldLayout buildDeleteWorldLayout(core::World& world)
	{
		DeleteWorldLayout layout;
		layout.frontCorridor = world.addCorridor(0, 0, 12);
		layout.frontRoom = world.addRoom("Front room", 0, 2, 0, 6, 1);
		layout.backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		layout.backCorridor = world.addCorridor(1u, 2u, 0u, 12u, 1u);
		world.finishBuild();
		return layout;
	}

	void assign(core::World& world, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(world.setAgentGroup(agent, group, &diagnostic),
			("Assigning an Agent for a deletion check failed: " + diagnostic).c_str());
	}

	// The fixture every check starts from: three groups, the middle one
	// occupied by four Agents spread over both Layers, the outer two empty.
	struct DeleteFixture
	{
		core::AgentGroupId alpha{};
		core::AgentGroupId crew{};
		core::AgentGroupId delta{};
		std::vector<core::AgentId> members;
		core::AgentId unassigned{};
	};

	DeleteFixture buildFixture(core::World& world)
	{
		DeleteFixture fixture;
		auto const layout = buildDeleteWorldLayout(world);

		fixture.alpha = world.addAgentGroup("Alpha");
		fixture.crew = world.addAgentGroup("Crew");
		fixture.delta = world.addAgentGroup("Delta");

		fixture.members.push_back(world.createAgent("Front corridor hand",
			layout.frontCorridor, 0, 1.5f));
		fixture.members.push_back(world.createAgent("Front desk",
			layout.frontRoom, 0, 1.0f));
		fixture.members.push_back(world.createAgent("Back office",
			layout.backRoom, 0, 2.0f));
		fixture.members.push_back(world.createAgent("Back corridor hand",
			layout.backCorridor, 0, 3.0f));
		fixture.unassigned = world.createAgent("Freelance", layout.frontCorridor, 0, 5.0f);

		for (auto const member : fixture.members) assign(world, member, fixture.crew);

		return fixture;
	}

	// ---------------------------------------------------------------- checks

	// An empty group is the simple case, and it has to be simple: the group
	// goes, and nothing else moves.
	void anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone()
	{
		core::World world("Empty delete", 12, 3);
		auto const fixture = buildFixture(world);
		auto const before = groupSummary(world);
		require(before.find("Alpha") != std::string::npos
			&& before.find("Delta") != std::string::npos,
			"The fixture did not start with the two empty groups: " + before);

		std::string diagnostic;
		require(world.canDeleteAgentGroup(fixture.alpha, &diagnostic),
			("Deleting an empty Agent group was refused: " + diagnostic).c_str());
		require(world.deleteAgentGroup(fixture.alpha, &diagnostic),
			("Deleting an empty Agent group failed: " + diagnostic).c_str());

		require(world.getAgentGroupCount() == 2,
			"The World still reports three Agent groups after deleting one: "
			+ groupSummary(world));
		require(!world.lookupAgentGroup(fixture.alpha),
			"The deleted Agent group is still resolvable through the World");
		require(groupSummary(world) == "2:Crew=4;3:Delta=0;",
			"Deleting an empty Agent group disturbed the others: " + groupSummary(world));
		requireNoDanglingAssignment(world, "after an empty group delete");
	}

	// An ID this World never issued is refused, and the refusal says which
	// ID. Nothing is written on the way out, so "atomically" has a concrete
	// meaning here: the whole summary is what it was before the call.
	void anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic()
	{
		core::World world("Unknown delete", 12, 3);
		auto const fixture = buildFixture(world);
		auto const before = groupSummary(world);
		auto const beforeAssignments = assignmentSummary(world);
		auto const beforeIds = world.getAgentGroupIds();

		std::string diagnostic;
		require(!world.canDeleteAgentGroup(core::AgentGroupId{ 4242 }, &diagnostic),
			"Deleting an Agent group this World never issued succeeded");
		require(diagnostic.find("4242") != std::string::npos,
			("The unknown-group refusal did not name the group: " + diagnostic).c_str());
		require(!world.deleteAgentGroup(core::AgentGroupId{ 4242 }, &diagnostic),
			"World.deleteAgentGroup accepted a group it does not own");
		require(diagnostic.find("4242") != std::string::npos,
			("The refused delete did not name the unknown group: " + diagnostic).c_str());

		// The null handle names no group either, so there is nothing for it
		// to delete; refusing it keeps a success meaning a deletion happened.
		require(!world.canDeleteAgentGroup(core::AgentGroupId{}, &diagnostic),
			"Deleting the empty AgentGroupId was treated as a deletion");
		require(!world.deleteAgentGroup(core::AgentGroupId{}, &diagnostic),
			"World.deleteAgentGroup accepted the empty AgentGroupId");
		require(!diagnostic.empty(),
			"The empty-AgentGroupId refusal came back without a reason");

		require(groupSummary(world) == before,
			"A refused delete changed the groups: " + groupSummary(world));
		require(assignmentSummary(world) == beforeAssignments,
			"A refused delete cleared an assignment it had no business touching");
		require(world.getAgentGroupIds() == beforeIds,
			"A refused delete disturbed the Agent group list");
	}

	// A deleted AgentGroupId is never handed out again, so a reference that
	// somehow survived could never silently come to mean a different group.
	void aDeletedAgentGroupIdIsNeverIssuedAgain()
	{
		core::World world("Id reuse", 12, 3);
		auto const fixture = buildFixture(world);

		std::string diagnostic;
		require(world.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting the occupied Agent group failed: " + diagnostic).c_str());

		auto const replacement = world.addAgentGroup("Replacement");
		require(replacement != fixture.crew,
			"A new Agent group was issued the deleted group's AgentGroupId");
		require(replacement.value > fixture.crew.value,
			"The new Agent group's AgentGroupId did not come after the deleted one");
		requireNoDanglingAssignment(world, "after reissuing Agent group IDs");
	}

	// A deterministic walk whose trace is the tick, the topology generation,
	// every Agent's position and state, and every event the run published.
	// Two runs that differ only in whether the middle of the walk deleted the
	// Agents' group must produce the same trace, which is what "deletion does
	// not alter Agent movement or other simulation state" means concretely.
	std::string walkWithOptionalMidRunDelete(bool deleteMidRun)
	{
		core::World world("Delete walk", 12, 3);
		auto const corridor = world.addCorridor(0, 0, 12);
		uint32_t destinationIdentifier{ 0x44454c31u };
		world.addSectorMarker(corridor, 0, 11.5f, &destinationIdentifier);
		world.finishBuild();

		auto const crew = world.addAgentGroup("Crew");
		auto const walker = world.createAgent("Walker", corridor, 0, 0.5f);
		auto const companion = world.createAgent("Companion", corridor, 0, 1.5f);
		assign(world, walker, crew);
		assign(world, companion, crew);

		auto* agent = world.lookupAgent(walker).entity;
		auto const destination = world.getGraph()->getVertexByIdentifier(destinationIdentifier);
		require(destination != nullptr, "The walk destination vertex is missing");
		auto path = world.getGraph()->calculatePath(agent, destination);
		require(path && !path->nodes.empty(), "The walk route could not be calculated");
		agent->setPath(std::move(path), true);

		for (uint32_t tick = 0; tick < 30; ++tick) world.advanceTick();

		auto const* walkingAgent = world.lookupAgent(walker).entity;
		auto const positionBeforeDelete = walkingAgent->getGlobalPosition();
		require(!world.isSimulationPaused(),
			"The test World was paused before the delete, so running was never tested");

		if (deleteMidRun)
		{
			std::string diagnostic;
			require(world.deleteAgentGroup(crew, &diagnostic),
				("Deleting an Agent group mid-run failed: " + diagnostic).c_str());
			require(!world.isSimulationPaused(),
				"Deleting an Agent group paused a running simulation");
		}

		for (uint32_t tick = 0; tick < 90; ++tick) world.advanceTick();

		require(world.lookupAgent(walker).entity->getGlobalPosition()
			.distanceTo(positionBeforeDelete) > 0.01f,
			"The walking Agent did not move after the delete, so the comparison proved nothing");

		std::ostringstream out;
		out.precision(6);
		out << std::fixed;

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

	// Deleting while the world runs changes nothing about the world. The
	// Agents that were the group's members carry on walking exactly as if the
	// group had never existed, because grouping was never part of their run.
	void deletingWhileTheSimulationRunsLeavesTheRunAlone()
	{
		auto const withoutDelete = walkWithOptionalMidRunDelete(false);
		auto const withDelete = walkWithOptionalMidRunDelete(true);

		require(!withoutDelete.empty() && !withDelete.empty(),
			"The movement traces came back empty, so the comparison proved nothing");
		require(withoutDelete == withDelete,
			"Deleting an Agent group while the simulation ran changed Agent movement, "
			"the runtime snapshot, or the published events:\n"
			+ withoutDelete + "\nvs\n" + withDelete);
	}

	// The snapshot at the instant of the deletion, and the snapshot a moment
	// later, are the same document: the deletion wrote nothing into the
	// runtime state it shares with the simulation.
	void theRuntimeSnapshotIsTheSameAcrossTheDeletion()
	{
		core::World world("Snapshot across delete", 12, 3);
		auto const fixture = buildFixture(world);
		for (uint32_t tick = 0; tick < 12; ++tick) world.advanceTick();

		auto const render = [](core::SimulationSnapshot const& snapshot)
		{
			std::ostringstream out;
			out.precision(6);
			out << std::fixed;
			out << "tick=" << snapshot.tick
				<< " paused=" << snapshot.paused
				<< " topology=" << snapshot.topologyGeneration << "\n";
			for (auto const& entry : snapshot.agents)
			{
				out << entry.id.value << ' ' << entry.name
					<< " sector=" << entry.sectorId.value
					<< " global=" << entry.globalPosition.x << ',' << entry.globalPosition.y
					<< " state=" << static_cast<int>(entry.state) << "\n";
			}
			return out.str();
		};

		auto const before = render(world.getSimulationSnapshot());

		std::string diagnostic;
		require(world.deleteAgentGroup(fixture.crew, &diagnostic),
			("Deleting the occupied Agent group failed: " + diagnostic).c_str());

		auto const after = render(world.getSimulationSnapshot());
		require(before == after,
			"The runtime snapshot changed across an Agent group deletion:\n"
			+ before + "\nvs\n" + after);
	}
}

void agent_smoke::registerGroupDelete(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone", [](smoke::Context const&) { anEmptyGroupDeletesAndLeavesEveryOtherGroupAlone(); } });
	checks.push_back({ "anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic", [](smoke::Context const&) { anUnknownGroupIdIsRefusedAtomicallyWithADiagnostic(); } });
	checks.push_back({ "aDeletedAgentGroupIdIsNeverIssuedAgain", [](smoke::Context const&) { aDeletedAgentGroupIdIsNeverIssuedAgain(); } });
	checks.push_back({ "deletingWhileTheSimulationRunsLeavesTheRunAlone", [](smoke::Context const&) { deletingWhileTheSimulationRunsLeavesTheRunAlone(); } });
	checks.push_back({ "theRuntimeSnapshotIsTheSameAcrossTheDeletion", [](smoke::Context const&) { theRuntimeSnapshotIsTheSameAcrossTheDeletion(); } });
}
