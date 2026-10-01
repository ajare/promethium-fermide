// Migrated from AgentGroupCountSmokeChecks.cpp (#285); editor dependency tier.
#include <bit>
#include <cctype>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
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
#include "AgentGroupsPanel.h"
#include "DocumentEdit.h"
#include "Checks.h"
#include "EditorState.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Loads over a World that already exists, which is how the editor's
	// undo restores a snapshot: the snapshot's document replaces the live one.
	void restoreInto(core::World& target, std::string const& yaml)
	{
		core::SerializationWorkData workData;
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(target.deserialize(*reader, workData), "The World did not reload");
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

	struct ImGuiGuard
	{
		ImGuiGuard()
		{
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	void resetUndoHistory()
	{
		gWorldDocumentHistory.clear();
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

	// An empty group is a group that counts zero, not a group with no number.
	// A freshly defined group and a group whose last member has just gone both
	// read as zero, and the column shows it.
	void anEmptyGroupCountsZero()
	{
		core::World world("Empty group", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		require(world.getAgentGroupMemberCount(crew) == 0,
			"A newly defined Agent group did not count zero members");
		require(agentGroupMemberCountLabel(world, crew) == "0",
			"The Agents column did not show zero for an empty Agent group");

		auto const agent = world.createAgent("Only member", layout.frontCorridor, 0, 1.0f);
		assign(world, agent, crew);
		require(world.getAgentGroupMemberCount(crew) == 1,
			"The first member of an empty group was not counted");

		assign(world, agent, {});
		require(world.getAgentGroupMemberCount(crew) == 0,
			"Clearing the last member left the group counting something");
		require(agentGroupMemberCountLabel(world, crew) == "0",
			"The Agents column did not return to zero once the group was emptied");
	}

	// A rename moves a label and nothing else. The count rides on the ID, so
	// the number a group shows is undisturbed by what it is called.
	void aRenameLeavesTheCountAlone()
	{
		core::World world("Count rename", 12, 3);
		auto const layout = buildCountingWorldLayout(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const nightShift = world.addAgentGroup("Night shift");
		assign(world, world.createAgent("One", layout.frontCorridor, 0, 1.0f), crew);
		assign(world, world.createAgent("Two", layout.frontRoom, 0, 1.0f), crew);
		assign(world, world.createAgent("Three", layout.backRoom, 0, 1.0f), crew);
		assign(world, world.createAgent("Four", layout.backCorridor, 0, 1.0f), nightShift);

		auto const before = allCounts(world);
		require(before == "Crew=3;Night shift=1;",
			"The rename fixture is not what the check expects: " + before);

		std::string diagnostic;
		require(world.renameAgentGroup(crew, "Facilities team", &diagnostic),
			("Renaming a populated Agent group was refused: " + diagnostic).c_str());

		require(world.getAgentGroupMemberCount(crew) == 3,
			"Renaming an Agent group changed its count");
		require(world.getAgentGroupMemberCount(nightShift) == 1,
			"Renaming one Agent group changed another's count");
		require(agentGroupMemberCountLabel(world, crew) == "3",
			"The Agents column's count did not survive a rename of its group");

		// Renaming back, and renaming the other group too, still moves no
		// number.
		require(world.renameAgentGroup(crew, "Crew", &diagnostic)
			&& world.renameAgentGroup(nightShift, "Graveyard", &diagnostic),
			("A follow-up rename was refused: " + diagnostic).c_str());
		require(world.getAgentGroupMemberCount(crew) == 3
			&& world.getAgentGroupMemberCount(nightShift) == 1,
			"A second rename changed a count: " + allCounts(world));

		// A refused rename changes nothing either - counts included.
		require(!world.renameAgentGroup(crew, "Graveyard", &diagnostic),
			"A rename onto a name another group holds was accepted");
		require(world.getAgentGroupMemberCount(crew) == 3
			&& world.getAgentGroupMemberCount(nightShift) == 1,
			"A refused rename moved a count: " + allCounts(world));
	}

	// The undo snapshot is the other restoration path. A snapshot holds the
	// memberships, not the counts; restoring one brings the counts back as a
	// consequence of the memberships, not as a remembered number.
	void countsReturnFromARestoredUndoSnapshot()
	{
		resetUndoHistory();

		auto const world = std::make_shared<core::World>("Count undo", 12, 3);
		auto const layout = buildCountingWorldLayout(*world);

		auto const crew = world->addAgentGroup("Crew");
		auto const nightShift = world->addAgentGroup("Night shift");
		auto const alpha = world->createAgent("Alpha", layout.frontCorridor, 0, 1.0f);
		auto const beta = world->createAgent("Beta", layout.backRoom, 0, 1.0f);
		assign(*world, alpha, crew);

		require(allCounts(*world) == "Crew=1;Night shift=0;",
			"The undo fixture is not what the check expects: " + allCounts(*world));

		// Snapshot this state, then move both Agents about.
		auto const snapshot = captureDocumentSnapshot(world);
		require(snapshot.has_value(), "The undo snapshot could not be captured");

		assign(*world, beta, crew);
		assign(*world, alpha, nightShift);
		require(world->getAgentGroupMemberCount(crew) == 1
			&& world->getAgentGroupMemberCount(nightShift) == 1,
			"The live counts before the restore are wrong: " + allCounts(*world));

		// The snapshot carries no count data either - only the definitions
		// and the assignments, which is exactly what restoring needs.
		requireNoCountShapedKeys(snapshot->yaml, "agentGroups");
		requireNoCountShapedKeys(snapshot->yaml, "agents");

		// Restoring is a load of the snapshot's document over the live
		// World, which is how the editor's undo works.
		restoreInto(*world, snapshot->yaml);
		require(world->getAgentGroupMemberCount(crew) == 1,
			"Restoring the snapshot did not rebuild the group's count: "
			+ allCounts(*world));
		require(world->getAgentGroupMemberCount(nightShift) == 0,
			"Restoring the snapshot left the other group counting a member: "
			+ allCounts(*world));
		require(world->getAgentGroup(alpha) == crew,
			"Restoring the snapshot did not restore the assignment the count reads");
		require(!world->getAgentGroup(beta),
			"Restoring the snapshot left an Agent assigned that was unassigned before");

		// And the restored World counts live from here on.
		assign(*world, beta, nightShift);
		require(world->getAgentGroupMemberCount(nightShift) == 1,
			"A restored World stopped counting: " + allCounts(*world));
	}

	// The panel's own column list is what the table is built from. Active is
	// immediately after the group name, followed by Agents and Delete.
	void theGroupsTableDeclaresAnAgentsColumn()
	{
		auto const& columns = agentGroupsPanelColumns();
		require(columns.size() == 4,
			("The Groups table does not have Name, Active, Agents and Delete columns; it has "
				+ std::to_string(columns.size())).c_str());
		require(columns[0] == "Name",
			"The Groups table's first column is not the name column");
		require(columns[1] == "Active",
			"The Groups table's second column is not the Active toggle");
		require(columns[2] == "Agents",
			"The Groups table's count column is not called Agents");
		require(columns[3] == "Delete",
			"The Groups table's fourth column is not the Delete column");
	}

	// The real panel, rendered for real, with the count column in it. What
	// matters is that rendering counts leaves no ImGui state behind: a
	// leaked disabled scope once dimmed the rest of the editor for every
	// frame.
	void theCountColumnRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::World>("Count panel", 12, 3);
		auto const layout = buildCountingWorldLayout(*shared);
		auto const crew = shared->addAgentGroup("Crew");
		auto const nightShift = shared->addAgentGroup("Night shift");
		shared->addAgentGroup("Unstaffed");
		assign(*shared, shared->createAgent("One", layout.frontCorridor, 0, 1.0f), crew);
		assign(*shared, shared->createAgent("Two", layout.backRoom, 0, 1.0f), crew);
		assign(*shared, shared->createAgent("Three", layout.backCorridor, 0, 1.0f), nightShift);

		for (bool const paused : { true, false })
		{
			if (paused) shared->pauseSimulation();
			else if (shared->isSimulationPaused()) shared->resumeSimulation();

			ImGui::NewFrame();
			ImGui::Begin("World");

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;
			auto const alphaOnEntry = GImGui->Style.Alpha;

			renderAgentGroupsPanel(shared);

			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Agent groups panel left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Agent groups panel changed the current item flags");
			// Compared as bits, not as floats: the question is whether the value
			// is exactly the one stored, which is what "unchanged" means here
			// and what -Wfloat-equal objects to otherwise.
			require(std::bit_cast<uint32_t>(GImGui->Style.Alpha)
				== std::bit_cast<uint32_t>(alphaOnEntry),
				"The Agent groups panel changed the global alpha");
			// Merely drawing the counts changed nothing about them.
			require(allCounts(*shared) == "Crew=2;Night shift=1;Unstaffed=0;",
				"Rendering the counts changed them: " + allCounts(*shared));

			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}

	// The count cell on its own, rendered inside a two-column table built the
	// way the Groups table builds its own, and checked against the numbers it
	// has to show as membership changes underneath it.
	void theCountCellShowsTheLiveCount()
	{
		ImGuiGuard guard;

		core::World world("Count cell", 12, 3);
		auto const layout = buildCountingWorldLayout(world);
		auto const crew = world.addAgentGroup("Crew");
		auto const empty = world.addAgentGroup("Unstaffed");

		require(agentGroupMemberCountLabel(world, crew) == "0",
			"A new group's cell does not read zero before its first member");
		require(agentGroupMemberCountLabel(world, empty) == "0",
			"An empty group's cell does not read zero");

		assign(world, world.createAgent("One", layout.frontCorridor, 0, 1.0f), crew);
		require(agentGroupMemberCountLabel(world, crew) == "1",
			"A cell does not show one member after one assignment");

		assign(world, world.createAgent("Two", layout.backRoom, 0, 1.0f), crew);
		require(agentGroupMemberCountLabel(world, crew) == "2",
			"A cell does not show two members after a second assignment");

		ImGui::NewFrame();
		ImGui::Begin("World");

		ImGuiTableFlags const flags =
			ImGuiTableFlags_SizingStretchSame |
			ImGuiTableFlags_BordersOuter |
			ImGuiTableFlags_BordersV;
		require(ImGui::BeginTable("AgentGroups", 2, flags),
			"The test Groups table could not be opened");
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Agents");
		ImGui::TableHeadersRow();

		ImGui::TableNextRow();
		ImGui::PushID(crew.value);
		ImGui::TableSetColumnIndex(1);

		auto const depthOnEntry = GImGui->DisabledStackSize;
		auto const flagsOnEntry = GImGui->CurrentItemFlags;

		renderAgentGroupMemberCountCell(world, crew);

		require(GImGui->DisabledStackSize == depthOnEntry,
			"The Agents count cell left a disabled scope open");
		require(GImGui->CurrentItemFlags == flagsOnEntry,
			"The Agents count cell changed the current item flags");
		require(agentGroupMemberCountLabel(world, crew) == "2",
			"Merely rendering the count cell changed the count");

		ImGui::PopID();
		ImGui::EndTable();
		ImGui::End();
		ImGui::Render();
	}
}

void agent_smoke::registerGroupCountEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "anEmptyGroupCountsZero", [](smoke::Context const&) { EditorState state; anEmptyGroupCountsZero(); } });
	checks.push_back({ "aRenameLeavesTheCountAlone", [](smoke::Context const&) { EditorState state; aRenameLeavesTheCountAlone(); } });
	checks.push_back({ "countsReturnFromARestoredUndoSnapshot", [](smoke::Context const&) { EditorState state; countsReturnFromARestoredUndoSnapshot(); } });
	checks.push_back({ "theGroupsTableDeclaresAnAgentsColumn", [](smoke::Context const&) { EditorState state; theGroupsTableDeclaresAnAgentsColumn(); } });
	checks.push_back({ "theCountColumnRendersWithoutLeakingImGuiState", [](smoke::Context const&) { EditorState state; theCountColumnRendersWithoutLeakingImGuiState(); } });
	checks.push_back({ "theCountCellShowsTheLiveCount", [](smoke::Context const&) { EditorState state; theCountCellShowsTheLiveCount(); } });
}
