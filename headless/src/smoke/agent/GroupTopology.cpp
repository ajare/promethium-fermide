// Migrated from AgentGroupTopologySmokeChecks.cpp (#285); core dependency tier.
#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/YamlSerializer.h"
#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string text(core::AgentId id)
	{
		return "Agent " + std::to_string(id.value);
	}

	std::string text(core::AgentGroupId id)
	{
		return "Agent group " + std::to_string(id.value);
	}

	std::string groupText(core::AgentGroupId id)
	{
		return id ? std::to_string(id.value) : std::string("<none>");
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
		auto loaded = std::make_shared<core::World>("Reopened World", 1, 1);
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(reader != nullptr, "The serialised World could not be read back");
		require(loaded->deserialize(*reader, workData), "The World did not reload");
		return loaded;
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
					if (agent) agents.push_back(agent);
			}
		}
		return agents;
	}

	void assign(core::World& world, core::AgentId agent, core::AgentGroupId group)
	{
		std::string diagnostic;
		require(world.setAgentGroup(agent, group, &diagnostic),
			"Assigning an Agent for a topology check failed: " + diagnostic);
	}

	// The four Agents every check reads. Two of them are `Crew`, one is `Gang`,
	// and one belongs to no group at all. Two groups rather than one so a
	// restoration that reassigned everyone to the first group it found would be
	// caught, and one Agent with no group so a restoration that invented one for
	// the sake of tidiness would be caught too.
	struct Crew
	{
		core::AgentGroupId crew{};
		core::AgentGroupId gang{};
		core::AgentId crewFront{};
		core::AgentId crewBack{};
		core::AgentId gangBack{};
		core::AgentId ungrouped{};
	};

	Crew authorCrew(core::World& world, uint32_t frontRoom, uint32_t backRoom)
	{
		Crew crew;
		crew.crew = world.addAgentGroup("Crew");
		crew.gang = world.addAgentGroup("Gang");
		crew.crewFront = world.createAgent("Front desk", frontRoom, 0, 1.0f);
		crew.ungrouped = world.createAgent("Freelance", frontRoom, 0, 2.0f);
		crew.crewBack = world.createAgent("Back office", backRoom, 0, 1.0f);
		crew.gangBack = world.createAgent("Back guard", backRoom, 0, 2.0f);
		assign(world, crew.crewFront, crew.crew);
		assign(world, crew.crewBack, crew.crew);
		assign(world, crew.gangBack, crew.gang);
		return crew;
	}

	// What one Agent is expected to carry.
	using Expectation = std::pair<core::AgentId, core::AgentGroupId>;

	std::vector<Expectation> expectedOf(Crew const& crew)
	{
		return {
			{ crew.crewFront, crew.crew },
			{ crew.crewBack, crew.crew },
			{ crew.gangBack, crew.gang },
			{ crew.ungrouped, core::AgentGroupId{} }
		};
	}

	std::vector<core::AgentGroupId> bothGroups(Crew const& crew)
	{
		return { crew.crew, crew.gang };
	}

	// Nothing an Agent carries may name a group this World does not own. A
	// World in that state cannot even be saved and read back - the file
	// format refuses an assignment to a group the document never defines - so
	// a replay that introduced one would only be found the next time the user
	// saved.
	void requireNoDanglingAssignment(core::World const& world, std::string const& when)
	{
		for (auto const* agent : allAgents(world))
		{
			require(std::string(agent->getTypeName()) == "Human",
				"Topology reconstruction lost Human identity " + when);
			auto const group = agent->getAgentGroupId();
			if (!group) continue;
			auto const lookup = world.lookupAgentGroup(group);
			require(lookup.entity != nullptr,
				"An Agent carries an Agent group this World does not own " + when + ": "
				+ std::string(agent->getName()) + " points at " + text(group));
		}
	}

	// The whole assignment picture, checked against the caller's expectation.
	// The member count is derived from the expectation rather than read off
	// the World, so a group whose count drifted from its members fails here
	// even when every individual assignment survived.
	void expectAssignments(core::World const& world,
		std::vector<Expectation> const& expected,
		std::vector<core::AgentGroupId> const& groups,
		std::string const& when)
	{
		for (auto const& [agent, group] : expected)
		{
			auto const actual = world.getAgentGroup(agent);
			require(actual == group,
				"The Agent group is wrong " + when + ": " + text(agent) + " reads "
				+ groupText(actual) + ", expected " + groupText(group));
		}
		for (auto const& group : groups)
		{
			uint32_t expectedCount{ 0 };
			for (auto const& [agent, memberOf] : expected)
			{
				(void)agent;
				if (memberOf == group) ++expectedCount;
			}
			auto const actual = world.getAgentGroupMemberCount(group);
			require(actual == expectedCount,
				"The live member count is wrong " + when + ": " + text(group) + " counts "
				+ std::to_string(actual) + ", expected " + std::to_string(expectedCount));
		}
		requireNoDanglingAssignment(world, when);
	}

	void requireGone(core::World const& world, core::AgentId agent, std::string const& when)
	{
		require(world.lookupAgent(agent).entity == nullptr,
			"An Agent the edit was meant to remove is still here " + when + ": " + text(agent));
	}

	void requireHere(core::World const& world, core::AgentId agent, std::string const& when)
	{
		require(world.lookupAgent(agent).entity != nullptr,
			"An Agent the edit was meant to keep is gone " + when + ": " + text(agent));
	}

	// ---------------------------------------------------------------- checks

	// Path 1: the plain construction-record replay. applyLiftEdit() rebuilds
	// the whole World from its records with no Sector moved, which is the
	// shape rebuildFromConstructionRecords() takes when a Lift, Shuttle,
	// Stairwell or Bulkhead Door edit replays the world behind its back.
	void aPlainConstructionRecordReplayKeepsEveryAssignment()
	{
		core::World world("Plain replay", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		auto const frontRoom = world.addRoom("Landing A", 0, 0, 0, 12, 1);
		auto const backRoom = world.addRoom("Landing B", 0, 1, 0, 12, 1);
		world.addRoom("Landing C", 0, 2, 0, 12, 1);
		auto const lift = world.addLift(1, 0, 2, 1, 3);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		auto const liftSector = lift.lift.sector->getIndex();
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before a plain construction-record replay");

		world.pauseSimulation();
		auto const plan = world.planResizeLift(liftSector, 2, 0, 1, 3);
		require(plan.valid, "The unchanged-topology Lift rebuild plan was refused: " + plan.diagnostic);
		world.applyLiftEdit(plan);

		requireHere(world, crew.crewFront, "after a plain construction-record replay");
		requireHere(world, crew.crewBack, "after a plain construction-record replay");
		requireHere(world, crew.gangBack, "after a plain construction-record replay");
		requireHere(world, crew.ungrouped, "after a plain construction-record replay");
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after a plain construction-record replay");
	}

	// Path 2: Layer deletion. The Agents on the deleted Layer are the edit's
	// intended casualties: they go, and their groups stop counting them. The
	// Agents that were somewhere else keep exactly what they had.
	void layerDeletionKeepsSurvivorsAndStopsCountingTheGone()
	{
		core::World world("Layer deletion", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 6, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 6, 1);
		auto const doomedRoom = world.addRoom("Top room", 2, 0, 0, 6, 1);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		auto const crewTop = world.createAgent("Top level hand", doomedRoom, 0, 1.0f);
		assign(world, crewTop, crew.crew);

		auto const before = expectedOf(crew);
		auto const beforeWithTop = [&crew, crewTop]()
		{
			auto expected = expectedOf(crew);
			expected.push_back({ crewTop, crew.crew });
			return expected;
		}();
		expectAssignments(world, beforeWithTop, bothGroups(crew),
			"before a Layer deletion");
		require(world.getAgentGroupMemberCount(crew.crew) == 3,
			"The fixture did not put three Agents in the Crew group");

		world.pauseSimulation();
		auto const plan = world.planDeleteLayer(2);
		require(plan.valid, "The Layer deletion plan was refused: " + plan.diagnostic);
		require(world.applyDeleteLayer(plan), "The Layer deletion was not applied");

		requireGone(world, crewTop, "after a Layer deletion");
		requireHere(world, crew.crewFront, "after a Layer deletion");
		requireHere(world, crew.crewBack, "after a Layer deletion");
		// The deleted Agent is not counted: the group's live count is its
		// surviving members, not its former ones.
		expectAssignments(world, before, bothGroups(crew), "after a Layer deletion");
		require(world.getAgentGroupMemberCount(crew.crew) == 2,
			"The Crew group still counts an Agent its Layer deleted");
	}

	// Path 3: Door removal.
	void doorRemovalKeepsEveryAssignment()
	{
		core::World world("Door removal", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 8, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 8, 1);
		auto const door = world.addSectorDoor(0, 0, 3);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before a Door removal");

		world.pauseSimulation();
		require(world.removeSectorDoor(door.door.sector->getIndex(), door.door.index),
			"The Door could not be removed");

		requireHere(world, crew.crewFront, "after a Door removal");
		requireHere(world, crew.crewBack, "after a Door removal");
		requireHere(world, crew.gangBack, "after a Door removal");
		requireHere(world, crew.ungrouped, "after a Door removal");
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after a Door removal");
	}

	// Path 4: Window removal.
	void windowRemovalKeepsEveryAssignment()
	{
		core::World world("Window removal", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 8, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 8, 1);
		world.addRoom("Cellar", 2, 0, 0, 8, 1);
		auto const window = world.addSectorWindow(1, 0, 5, 1, 1, { true });
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before a Window removal");

		world.pauseSimulation();
		require(world.removeSectorWindow(window.window.sector->getIndex(), window.window.index),
			"The Window could not be removed");

		requireHere(world, crew.crewFront, "after a Window removal");
		requireHere(world, crew.crewBack, "after a Window removal");
		requireHere(world, crew.gangBack, "after a Window removal");
		requireHere(world, crew.ungrouped, "after a Window removal");
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after a Window removal");
	}

	// Path 5: object movement. The object moves; the Agents standing near it
	// are rebuilt where they were and come back assigned as they were.
	void objectMovementKeepsEveryAssignment()
	{
		core::World world("Object movement", 12, 4);
		while (world.getLayerCount() < 2) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 4, 3);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 4, 3);
		auto const walkway = world.addSectorWalkway(frontRoom, 1, 1);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before an object move");

		world.pauseSimulation();
		auto const plan = world.planMoveSectorObject(frontRoom, walkway.index, 2, 1);
		require(plan.valid, "The object move plan was refused: " + plan.diagnostic);
		auto const moved = world.applyObjectMove(plan);
		require(moved != nullptr, "The object move produced no object");
		require(moved->getCellX() == 2 && moved->getCellY() == 1,
			"The object did not move to the planned cell");

		requireHere(world, crew.crewFront, "after an object move");
		requireHere(world, crew.crewBack, "after an object move");
		requireHere(world, crew.gangBack, "after an object move");
		requireHere(world, crew.ungrouped, "after an object move");
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after an object move");
	}

	// Path 6: Location editing. The Room itself moves out from under the
	// Agents standing in it, which is the exact sequence the ticket reported:
	// a Room move preserved the Agent and its AgentId and dropped the
	// assignment.
	void locationEditingKeepsEveryAssignment()
	{
		core::World world("Location editing", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 4, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 4, 1);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before a Location move");

		world.pauseSimulation();
		auto const plan = world.planResizeLocation(frontRoom, 6, 0, 4, 1);
		require(plan.valid, "The Location move plan was refused: " + plan.diagnostic);
		require(plan.move, "The plan did not read as a move, so it would not exercise the shift");
		world.applyLocationEdit(plan);

		requireHere(world, crew.crewFront, "after a Location move");
		requireHere(world, crew.crewBack, "after a Location move");
		requireHere(world, crew.gangBack, "after a Location move");
		requireHere(world, crew.ungrouped, "after a Location move");
		// The Agents that were in the moved Room came along with it, and are
		// still in the same groups they were in before it moved.
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after a Location move");

		auto const movedRoom = world.getSectorAtPosition(0, 7.5f, 0.5f);
		require(movedRoom != nullptr, "The moved Room cannot be found where it was placed");
		auto const carried = movedRoom->getAgents();
		require(carried.size() == 2,
			"The moved Room did not carry its two Agents along with it");
	}

	// Path 7: Background editing. Nothing walks a Background, but editing one
	// replays every other Sector and every Agent in the World.
	void backgroundEditingKeepsEveryAssignment()
	{
		core::World world("Background editing", 12, 3);
		while (world.getLayerCount() < 3) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 4, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 4, 1);
		auto const background = world.addBackground(2, 0, 0, 4, 1, { 10, 20, 30 });
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"before a Background edit");

		world.pauseSimulation();
		auto const plan = world.planResizeBackground(background, 0, 0, 6, 1);
		require(plan.valid, "The Background resize plan was refused: " + plan.diagnostic);
		world.applyBackgroundEdit(plan);

		requireHere(world, crew.crewFront, "after a Background edit");
		requireHere(world, crew.crewBack, "after a Background edit");
		requireHere(world, crew.gangBack, "after a Background edit");
		requireHere(world, crew.ungrouped, "after a Background edit");
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after a Background edit");
	}

	// The assignments the edit left behind have to survive the file: the saved
	// document carries them, and a reopen agrees with the live World about
	// every Agent and every count.
	void anEditSurvivesSavingAndReopening()
	{
		core::World world("Save and reopen", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		auto const frontRoom = world.addRoom("Front room", 0, 0, 0, 4, 1);
		auto const backRoom = world.addRoom("Back room", 1, 0, 0, 4, 1);
		world.finishBuild();

		auto const crew = authorCrew(world, frontRoom, backRoom);
		world.pauseSimulation();
		auto const plan = world.planResizeLocation(frontRoom, 6, 0, 4, 1);
		require(plan.valid, "The Location move plan was refused: " + plan.diagnostic);
		world.applyLocationEdit(plan);
		expectAssignments(world, expectedOf(crew), bothGroups(crew),
			"after the edit, before saving");

		auto const reopened = loadWorld(serializeWorld(world));
		reopened->pauseSimulation();
		// The reopened World holds the same Agents under the same IDs, so
		// the expectation above is the expectation for it too.
		require(reopened->lookupAgent(crew.crewFront).entity != nullptr,
			"The reopened World lost an Agent the edit kept");
		require(reopened->lookupAgent(crew.ungrouped).entity != nullptr,
			"The reopened World lost the ungrouped Agent the edit kept");
		expectAssignments(*reopened, expectedOf(crew), bothGroups(crew),
			"after saving and reopening the edited World");
	}
}

void agent_smoke::registerGroupTopology(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "aPlainConstructionRecordReplayKeepsEveryAssignment", [](smoke::Context const&) { aPlainConstructionRecordReplayKeepsEveryAssignment(); } });
	checks.push_back({ "layerDeletionKeepsSurvivorsAndStopsCountingTheGone", [](smoke::Context const&) { layerDeletionKeepsSurvivorsAndStopsCountingTheGone(); } });
	checks.push_back({ "doorRemovalKeepsEveryAssignment", [](smoke::Context const&) { doorRemovalKeepsEveryAssignment(); } });
	checks.push_back({ "windowRemovalKeepsEveryAssignment", [](smoke::Context const&) { windowRemovalKeepsEveryAssignment(); } });
	checks.push_back({ "objectMovementKeepsEveryAssignment", [](smoke::Context const&) { objectMovementKeepsEveryAssignment(); } });
	checks.push_back({ "locationEditingKeepsEveryAssignment", [](smoke::Context const&) { locationEditingKeepsEveryAssignment(); } });
	checks.push_back({ "backgroundEditingKeepsEveryAssignment", [](smoke::Context const&) { backgroundEditingKeepsEveryAssignment(); } });
	checks.push_back({ "anEditSurvivesSavingAndReopening", [](smoke::Context const&) { anEditSurvivesSavingAndReopening(); } });
}
