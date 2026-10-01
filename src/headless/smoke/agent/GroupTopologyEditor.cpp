// Migrated from AgentGroupTopologySmokeChecks.cpp (#285); editor dependency tier.
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
#include "DocumentEdit.h"
#include "Checks.h"
#include "EditorState.h"

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

	void requireHere(core::World const& world, core::AgentId agent, std::string const& when)
	{
		require(world.lookupAgent(agent).entity != nullptr,
			"An Agent the edit was meant to keep is gone " + when + ": " + text(agent));
	}

	// The editor's own undo and redo, the same shape as UI.cpp's
	// restoreDocumentSnapshot(): the live state crosses to the other stack and
	// the newest snapshot on the source stack becomes the live World.
	void restoreDocument(std::shared_ptr<core::World>& world, bool redo)
	{
		auto const current = captureDocumentSnapshot(world);
		require(current.has_value(), "The live document could not be captured");

		std::shared_ptr<core::World> loaded;
		auto restore = [&loaded](DocumentSnapshot const& target)
		{
			loaded = loadWorld(target.yaml);
			loaded->markModified();
			return true;
		};
		auto const restored = redo
			? gWorldDocumentHistory.redo(current, restore)
			: gWorldDocumentHistory.undo(current, restore);
		require(restored, redo ? "There is no redo entry to restore"
			: "There is no undo entry to restore");
		world = std::move(loaded);
	}

	void resetUndoHistory()
	{
		gWorldDocumentHistory.clear();
	}

	// The editor's undo and redo over a topology edit. The snapshot taken
	// before the edit is what undo restores, so both sides of the history have
	// to agree with the live World about every assignment.
	void anEditSurvivesUndoAndRedo()
	{
		resetUndoHistory();
		auto world = std::make_shared<core::World>("Undo and redo", 12, 3);
		while (world->getLayerCount() < 2) world->addLayer();
		auto const frontRoom = world->addRoom("Front room", 0, 0, 0, 4, 1);
		auto const backRoom = world->addRoom("Back room", 1, 0, 0, 4, 1);
		world->finishBuild();

		auto const crew = authorCrew(*world, frontRoom, backRoom);
		world->pauseSimulation();

		auto const snapshot = captureDocumentSnapshot(world);
		require(snapshot.has_value(), "The editor snapshot before the edit could not be captured");
		auto const plan = world->planResizeLocation(frontRoom, 6, 0, 4, 1);
		require(plan.valid, "The Location move plan was refused: " + plan.diagnostic);
		world->applyLocationEdit(plan);
		commitDocumentEdit(snapshot);
		require(gWorldDocumentHistory.undoCount() == 1, "The topology edit did not commit one undo entry");
		require(!gWorldDocumentHistory.canRedo(), "The topology edit produced a redo entry");
		expectAssignments(*world, expectedOf(crew), bothGroups(crew),
			"after the edit, before undoing it");

		restoreDocument(world, false);
		requireHere(*world, crew.crewFront, "after undoing the edit");
		requireHere(*world, crew.ungrouped, "after undoing the edit");
		expectAssignments(*world, expectedOf(crew), bothGroups(crew),
			"after undoing the topology edit");

		restoreDocument(world, true);
		requireHere(*world, crew.crewFront, "after redoing the edit");
		requireHere(*world, crew.ungrouped, "after redoing the edit");
		expectAssignments(*world, expectedOf(crew), bothGroups(crew),
			"after redoing the topology edit");
		resetUndoHistory();
	}
}

void agent_smoke::registerGroupTopologyEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "anEditSurvivesUndoAndRedo", [](smoke::Context const&) { EditorState state; anEditSurvivesUndoAndRedo(); } });
}
