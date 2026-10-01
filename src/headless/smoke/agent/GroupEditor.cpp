#include "ImGuiContext.h"
// Migrated from AgentGroupSmokeChecks.cpp (#285); editor dependency tier.
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "core/AgentGroup.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/SerializationException.h"
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

	// A World with a little world in it, so a group is never the only thing
	// the document carries.
	void buildWorld(core::World& world)
	{
		world.addCorridor(0, 0, 8);
		world.addRoom("Depot", 0, 2, 0, 4, 1);
		world.finishBuild();
	}

	struct ImGuiGuard
	{
		headless::ScopedImGuiContext context;
		ImGuiGuard()
		{
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
		}
	};

	void resetUndoHistory()
	{
		gWorldDocumentHistory.clear();
	}

	// The editor's own commit path, with the simulation live: one accepted
	// operation is one undo entry, one refused operation is none, and neither
	// pauses the world nor touches its topology.
	void groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach()
	{
		resetUndoHistory();
		resetAgentGroupsPanelState();

		auto const world = std::make_shared<core::World>("Group edits", 12, 3);
		buildWorld(*world);
		require(!world->isSimulationPaused(),
			"The test World started paused, so it proved nothing about running edits");

		// Let the world actually run, then come back to the document clean so
		// "marked modified" means this operation did it.
		world->advanceTick();
		world->advanceTick();
		world->markUnmodified();
		require(!world->isModified(), "The test World did not come back clean");

		auto const topologyBefore = world->getTopologyGeneration();
		std::string diagnostic;

		auto const added = commitAgentGroupAdd(world, "  Response team  ", diagnostic);
		require(added.value != 0,
			("Adding an Agent group through the panel seam failed: " + diagnostic).c_str());
		require(world->getAgentGroupName(added) == "Response team",
			"The panel seam did not trim the new Agent group name");
		require(world->isModified(),
			"Adding an Agent group did not mark the document modified");
		require(gWorldDocumentHistory.undoCount() == 1,
			"Adding an Agent group did not commit exactly one undoable document edit");
		require(!gWorldDocumentHistory.canRedo(), "Adding an Agent group produced a redo entry");
		require(!world->isSimulationPaused(),
			"Adding an Agent group paused the simulation");
		require(world->getTopologyGeneration() == topologyBefore,
			"Adding an Agent group rebuilt the traversal topology");

		require(commitAgentGroupRename(world, added, "Response", diagnostic),
			("Renaming an Agent group through the panel seam failed: " + diagnostic).c_str());
		require(gWorldDocumentHistory.undoCount() == 2,
			"Renaming an Agent group did not commit exactly one undoable document edit");

		// Refused operations leave the history exactly where it was.
		require(!commitAgentGroupAdd(world, "Response", diagnostic),
			"A duplicate Agent group name was accepted through the panel seam");
		require(gWorldDocumentHistory.undoCount() == 2,
			"A refused Agent group add committed an undo entry");

		require(!commitAgentGroupRename(world, core::AgentGroupId{ 4242 }, "Ghost", diagnostic),
			"Renaming an unknown Agent group succeeded through the panel seam");
		require(gWorldDocumentHistory.undoCount() == 2,
			"A refused Agent group rename committed an undo entry");

		require(!commitAgentGroupAdd(world, "   ", diagnostic),
			"A blank Agent group name was accepted through the panel seam");
		require(gWorldDocumentHistory.undoCount() == 2,
			"A blank Agent group add committed an undo entry");

		// Undo is a snapshot restore, so the entries themselves are the
		// history: the newest holds the state before the rename, the oldest
		// the state before the group existed at all.
		require(gWorldDocumentHistory.undoCount() == 2, "The undo stack is not the two edits made");
		auto const beforeRename = loadWorld(gWorldDocumentHistory.undoEntries().back().yaml);
		require(beforeRename->getAgentGroupCount() == 1
			&& beforeRename->getAgentGroupName(added) == "Response team",
			"The undo snapshot did not hold the state before the rename");
		auto const beforeAdd = loadWorld(gWorldDocumentHistory.undoEntries().front().yaml);
		require(beforeAdd->getAgentGroupCount() == 0,
			"The oldest undo snapshot still carried the added Agent group");

		// And the live World is where the redo would take it.
		require(world->getAgentGroupName(added) == "Response",
			"The live World did not hold the renamed Agent group");
	}

	// The real panel, rendered for real. What matters here is that it leaves no
	// ImGui state behind: a leaked disabled scope once dimmed the rest of the
	// editor for every frame.
	void theAgentGroupsPanelRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::World>("Group panel", 12, 3);
		buildWorld(*shared);
		shared->addAgentGroup("Alpha");
		shared->addAgentGroup("Bravo");

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

			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}
}

void agent_smoke::registerGroupEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach", [](smoke::Context const&) { EditorState state; groupEditsRunAlongsideTheSimulationAndCommitOneUndoEach(); } });
	checks.push_back({ "agent/theAgentGroupsPanelRendersWithoutLeakingImGuiState", [](smoke::Context const&) { EditorState state; theAgentGroupsPanelRendersWithoutLeakingImGuiState(); } });
}
