// Migrated from AgentGroupAssignmentSmokeChecks.cpp (#285); editor dependency tier.
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
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/EntityId.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/Simulation.h"
#include "core/YamlSerializer.h"
#include "AgentGroupAssignmentPanel.h"
#include "DocumentEdit.h"
#include "Checks.h"
#include "EditorState.h"

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

	// Drops every Agent assignment line from the document, leaving everything
	// else exactly as it was: the way to ask what a file that never carried an
	// assignment loads as.
	std::string withoutAssignmentFields(std::string const& yaml)
	{
		std::ostringstream out;
		std::istringstream in(yaml);
		std::string line;
		bool removedAny{ false };

		while (std::getline(in, line))
		{
			auto const start = line.find_first_not_of(" \t");
			if (start != std::string::npos && line.compare(start, 6, "group:") == 0)
			{
				removedAny = true;
				continue;
			}
			out << line << "\n";
		}

		require(removedAny,
			"The serialised World carried no Agent group assignment to strip");
		return out.str();
	}

	// A World with a little world in it, so an Agent is never the only
	// thing the document carries.
	void buildWorld(core::World& world)
	{
		world.addCorridor(0, 0, 8);
		world.addRoom("Depot", 0, 2, 0, 4, 1);
		world.finishBuild();
	}

	// ImGui writes its render-time text log to the clipboard on the frame the
	// logged window ends. The clipboard here is a vector in this process, so
	// capturing what was drawn never reaches the desktop and never blocks.
	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readCapturedClipboardText(void*)
	{
		return nullptr;
	}

	struct ImGuiGuard
	{
		// Everything the clipboard was handed, one entry per logged window.
		std::vector<std::string> clipboardWrites;

		ImGuiGuard()
		{
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.DisplaySize = ImVec2(800.0f, 600.0f);
			io.Fonts->AddFontDefault();
			io.Fonts->Build();
			clipboardWrites.clear();
			io.SetClipboardTextFn = &captureClipboardText;
			io.GetClipboardTextFn = &readCapturedClipboardText;
			io.ClipboardUserData = &clipboardWrites;
		}
		~ImGuiGuard() { ImGui::DestroyContext(); }
	};

	// One frame of the Group cell, drawn inside a table the way the Agents
	// section draws it. Passing a vector captures that frame's visible text.
	using FrameRender = std::function<void(std::vector<std::string>*)>;

	// Press and release a frame apart, which is what a click-release widget -
	// a combobox, a list row - takes as a click.
	void clickAt(float x, float y, FrameRender const& renderFrame)
	{
		auto& io = ImGui::GetIO();
		io.AddMousePosEvent(x, y);
		renderFrame(nullptr);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		renderFrame(nullptr);
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		renderFrame(nullptr);
	}

	// The rows of the open list, top to bottom, each as the y of a point
	// inside it. Found by hovering down from the combobox: a row is the band
	// of screen over which ImGui reports one and the same hovered item, so the
	// list's own layout decides where its rows are rather than a guess here.
	std::vector<float> scanChoiceRows(float x, float fromY, float toY,
		FrameRender const& renderFrame)
	{
		std::vector<float> rows;
		ImGuiID previous{ 0 };
		float bandStart{ fromY };

		for (float y = fromY; y <= toY; y += 1.0f)
		{
			ImGui::GetIO().AddMousePosEvent(x, y);
			renderFrame(nullptr);
			auto const hovered = ImGui::GetHoveredID();
			if (hovered == previous) continue;

			if (previous != 0) rows.push_back((bandStart + y) * 0.5f);
			bandStart = y;
			previous = hovered;
		}
		if (previous != 0) rows.push_back((bandStart + toY) * 0.5f);

		return rows;
	}

	// Every string in the capture, in the order they were drawn.
	bool drawnInOrder(std::string const& haystack, std::vector<std::string> const& needles)
	{
		size_t from{ 0 };
		for (auto const& needle : needles)
		{
			auto const found = haystack.find(needle, from);
			if (found == std::string::npos) return false;
			from = found;
		}
		return true;
	}

	void resetUndoHistory()
	{
		gWorldDocumentHistory.clear();
	}

	// The Agent holds an ID, the World holds the name, and that is why a
	// rename never has to visit the members.
	void anAssignedAgentFollowsItsGroupRename()
	{
		core::World world("Assignment rename", 12, 3);
		buildWorld(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const alpha = world.addAgentGroup("Alpha");
		auto const alice = world.createAgent("Alice", 0);
		auto const bob = world.createAgent("Bob", 0);

		std::string diagnostic;
		require(world.setAgentGroup(alice, crew, &diagnostic)
			&& world.setAgentGroup(bob, crew, &diagnostic),
			("Assigning two Agents to one group failed: " + diagnostic).c_str());

		require(world.renameAgentGroup(crew, "Facilities", &diagnostic),
			("Renaming an assigned group was refused: " + diagnostic).c_str());

		require(world.getAgentGroup(alice) == crew
			&& world.getAgentGroup(bob) == crew,
			"Renaming an Agent group broke the assignment it carried");
		require(world.getAgentGroupName(world.getAgentGroup(alice)) == "Facilities",
			"An assigned Agent does not read the group's new name through its ID");
		require(world.getAgentGroupName(world.getAgentGroup(bob)) == "Facilities",
			"A second assigned Agent does not read the group's new name through its ID");
		require(world.getAgentGroupName(alpha) == "Alpha",
			"Renaming one Agent group disturbed another");

		// The panel's own label follows the same way, which is what the user
		// sees in the table without the Agent being touched.
		require(agentGroupAssignmentLabel(world, alice) == "Facilities",
			"The Group cell label did not follow the rename");
		require(agentGroupAssignmentLabel(world, bob) == "Facilities",
			"A second assigned Agent's Group cell label did not follow the rename");
		require(agentGroupAssignmentLabel(world, world.createAgent("Carol", 0)) == "<none>",
			"An Agent with no Agent group does not read as <none>");
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

	// A document that never carried the field is not an error and is not a
	// half-truth: every Agent simply has no group.
	void aMissingAssignmentFieldLoadsAsNoGroup()
	{
		auto const fixture = assignmentFixture();
		auto const stripped = withoutAssignmentFields(fixture.yaml);

		auto const loaded = loadWorld(stripped);
		require(loaded->getAgentGroupCount() == 2,
			"Stripping the assignment fields took the Agent group definitions with them");
		require(!loaded->getAgentGroup(fixture.alice)
			&& !loaded->getAgentGroup(fixture.bob)
			&& !loaded->getAgentGroup(fixture.carol),
			"An Agent whose document carried no assignment field loaded with a group");
		require(agentGroupAssignmentLabel(*loaded, fixture.alice) == "<none>",
			"A legacy Agent with no assignment does not read as <none>");
	}

	// The editor's own commit path, with the simulation live: one accepted
	// assignment or clearing is one undo entry, one refused operation is none,
	// and neither pauses the world nor touches its topology.
	void assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach()
	{
		resetUndoHistory();

		auto const world = std::make_shared<core::World>("Assignment edits", 12, 3);
		buildWorld(*world);
		require(!world->isSimulationPaused(),
			"The test World started paused, so it proved nothing about running edits");

		auto const crew = world->addAgentGroup("Crew");
		auto const nightShift = world->addAgentGroup("Night shift");
		auto const alice = world->createAgent("Alice", 0);

		// Let the world actually run, then come back to the document clean so
		// "marked modified" means this operation did it. markSaved() rather than
		// markUnmodified(): the Agents this test created are children of the
		// document, and a dirty Agent keeps isModified() true on its own.
		world->advanceTick();
		world->advanceTick();
		world->markSaved();
		require(!world->isModified(), "The test World did not come back clean");

		auto const topologyBefore = world->getTopologyGeneration();
		std::string diagnostic;

		require(commitAgentGroupAssignment(world, alice, crew, diagnostic),
			("Assigning through the panel seam failed: " + diagnostic).c_str());
		require(world->getAgentGroup(alice) == crew,
			"The panel seam did not assign the Agent");
		require(world->isModified(),
			"Assigning an Agent to an Agent group did not mark the document modified");
		require(gWorldDocumentHistory.undoCount() == 1,
			"Assigning an Agent did not commit exactly one undoable document edit");
		require(!gWorldDocumentHistory.canRedo(), "Assigning an Agent produced a redo entry");
		require(!world->isSimulationPaused(),
			"Assigning an Agent paused the simulation");
		require(world->getTopologyGeneration() == topologyBefore,
			"Assigning an Agent rebuilt the traversal topology");

		require(commitAgentGroupAssignment(world, alice, nightShift, diagnostic),
			("Reassigning through the panel seam failed: " + diagnostic).c_str());
		require(gWorldDocumentHistory.undoCount() == 2,
			"Reassigning an Agent did not commit exactly one undoable document edit");

		require(commitAgentGroupAssignment(world, alice, {}, diagnostic),
			("Clearing through the panel seam failed: " + diagnostic).c_str());
		require(!world->getAgentGroup(alice),
			"Clearing through the panel seam left the Agent assigned");
		require(gWorldDocumentHistory.undoCount() == 3,
			"Clearing an Agent did not commit exactly one undoable document edit");

		// Refused operations leave the history exactly where it was.
		require(!commitAgentGroupAssignment(world, core::AgentId{ 4242 }, crew, diagnostic),
			"Assigning an unknown Agent succeeded through the panel seam");
		require(gWorldDocumentHistory.undoCount() == 3,
			"A refused assignment committed an undo entry");
		require(!diagnostic.empty(),
			"An assignment refused through the panel seam failed without a diagnostic");

		require(!commitAgentGroupAssignment(world, alice, core::AgentGroupId{ 31337 }, diagnostic),
			"Assigning to an unknown Agent group succeeded through the panel seam");
		require(gWorldDocumentHistory.undoCount() == 3,
			"An assignment to an unknown group committed an undo entry");
		require(!world->getAgentGroup(alice),
			"A refused assignment changed the live Agent");

		// Undo is a snapshot restore, so the entries themselves are the
		// history: the newest holds the state with the assignment still on,
		// the oldest the state before the first assignment was made.
		require(gWorldDocumentHistory.undoCount() == 3, "The undo stack is not the three edits made");
		auto const beforeClear = loadWorld(gWorldDocumentHistory.undoEntries().back().yaml);
		require(beforeClear->getAgentGroup(alice) == nightShift,
			"The newest undo snapshot did not hold the state before the clearing");
		auto const beforeFirst = loadWorld(gWorldDocumentHistory.undoEntries().front().yaml);
		require(!beforeFirst->getAgentGroup(alice),
			"The oldest undo snapshot already carried the first assignment");

		// And the live World is where the redo would take it.
		require(!world->getAgentGroup(alice),
			"The live World did not hold the cleared assignment");
	}

	// The label the cell shows, checked against the states it can be in. The
	// choice order itself is the World's creation order, which #109 pins
	// down; the cell renders straight off it, `<none>` always leading.
	void theGroupCellLabelShowsTheAssignment()
	{
		core::World world("Assignment label", 12, 3);
		buildWorld(world);

		auto const crew = world.addAgentGroup("Crew");
		auto const alice = world.createAgent("Alice", 0);

		require(agentGroupAssignmentLabel(world, alice) == "<none>",
			"An unassigned Group cell does not read <none>");
		std::string diagnostic;
		require(world.setAgentGroup(alice, crew, &diagnostic),
			("Assigning for the label check failed: " + diagnostic).c_str());
		require(agentGroupAssignmentLabel(world, alice) == "Crew",
			"An assigned Group cell does not show the group's name");
		require(world.renameAgentGroup(crew, "Response team", &diagnostic),
			("Renaming for the label check failed: " + diagnostic).c_str());
		require(agentGroupAssignmentLabel(world, alice) == "Response team",
			"An assigned Group cell does not follow a rename");
		require(world.setAgentGroup(alice, {}, &diagnostic)
			&& agentGroupAssignmentLabel(world, alice) == "<none>",
			"A cleared Group cell does not read <none> again");
	}

	// The real cell, rendered for real, inside a table like the one the Agents
	// section builds. What matters is that it leaves no ImGui state behind: a
	// leaked disabled scope once dimmed the rest of the editor for every frame.
	void theGroupCellRendersWithoutLeakingImGuiState()
	{
		ImGuiGuard guard;

		auto const shared = std::make_shared<core::World>("Group cell render", 12, 3);
		buildWorld(*shared);
		auto const crew = shared->addAgentGroup("Crew");
		// A second group so the cell has a list to render, not just the choice
		// it already shows.
		shared->addAgentGroup("Night shift");
		auto const alice = shared->createAgent("Alice", 0);
		std::string diagnostic;
		require(shared->setAgentGroup(alice, crew, &diagnostic),
			("Assigning for the render check failed: " + diagnostic).c_str());

		for (bool const paused : { true, false })
		{
			if (paused) shared->pauseSimulation();
			else if (shared->isSimulationPaused()) shared->resumeSimulation();

			ImGui::NewFrame();
			ImGui::Begin("World");

			ImGuiTableFlags const flags =
				ImGuiTableFlags_SizingStretchSame |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_BordersV;
			require(ImGui::BeginTable("Agents", 5, flags),
				"The test Agents table could not be opened");
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Group");
			ImGui::TableSetupColumn("Sector");
			ImGui::TableSetupColumn("State");
			ImGui::TableSetupColumn("Path");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::PushID((void const*)shared->lookupAgent(alice).entity);
			ImGui::TableSetColumnIndex(1);

			auto const depthOnEntry = GImGui->DisabledStackSize;
			auto const flagsOnEntry = GImGui->CurrentItemFlags;
			auto const alphaOnEntry = GImGui->Style.Alpha;

			renderAgentGroupAssignmentCell(shared, alice);

			require(GImGui->DisabledStackSize == depthOnEntry,
				"The Group cell left a disabled scope open");
			require(GImGui->CurrentItemFlags == flagsOnEntry,
				"The Group cell changed the current item flags");
			// Compared as bits, not as floats: the question is whether the value
			// is exactly the one stored, which is what "unchanged" means here
			// and what -Wfloat-equal objects to otherwise.
			require(std::bit_cast<uint32_t>(GImGui->Style.Alpha)
				== std::bit_cast<uint32_t>(alphaOnEntry),
				"The Group cell changed the global alpha");
			require(shared->getAgentGroup(alice) == crew,
				"Merely rendering the Group cell changed the assignment");

			ImGui::PopID();
			ImGui::EndTable();
			ImGui::End();

			// The frame has to complete, which is where ImGui's own end-frame
			// checks would fire on an unbalanced window.
			ImGui::Render();
		}
	}

	// A group name may legally hold "##", which ImGui reads in a widget label
	// as the start of an invisible ID suffix - and then hides, along with
	// everything after it. "Crew##Day" would read as "Crew", and a name that
	// opens with the pair would read as nothing at all. What is checked here is
	// the text ImGui actually put on the screen, captured through ImGui's own
	// render-time text log rather than the string the panel handed to a
	// widget, and the row that shows a name is clicked to show it is still
	// that name's own control.
	void agentGroupNamesCarryingHashPairsReachTheScreenInFull()
	{
		resetUndoHistory();
		ImGuiGuard guard;

		auto const world = std::make_shared<core::World>("Hash pair names", 12, 3);
		buildWorld(*world);

		// Every shape the pair can take in an otherwise valid name: embedded,
		// leading, and a tripled run.
		auto const crewDay = world->addAgentGroup("Crew##Day");
		auto const crewNight = world->addAgentGroup("Crew##Night");
		auto const leadingHashes = world->addAgentGroup("##Night shift");
		auto const tripleHash = world->addAgentGroup("Trip###le");

		auto const alice = world->createAgent("Alice", 0);
		std::string diagnostic;
		require(world->setAgentGroup(alice, crewNight, &diagnostic),
			("Assigning a group whose name carries ## failed: " + diagnostic).c_str());

		ImVec2 cellMin{};
		ImVec2 cellMax{};
		bool cellRectRecorded{ false };

		FrameRender const frame = [&](std::vector<std::string>* capture)
		{
			ImGui::NewFrame();
			// Fix the first frame's geometry instead of relying on imgui.ini or
			// auto-fit changing the cell after its click coordinates are captured.
			ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f));
			ImGui::SetNextWindowSize(ImVec2(700.0f, 500.0f));
			ImGui::Begin("World");
			if (capture) ImGui::LogToClipboard();

			ImGuiTableFlags const flags =
				ImGuiTableFlags_SizingStretchSame |
				ImGuiTableFlags_BordersOuter |
				ImGuiTableFlags_BordersV;
			require(ImGui::BeginTable("Agents", 5, flags),
				"The test Agents table could not be opened");
			ImGui::TableSetupColumn("Name");
			ImGui::TableSetupColumn("Group");
			ImGui::TableSetupColumn("Sector");
			ImGui::TableSetupColumn("State");
			ImGui::TableSetupColumn("Path");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(1);
			renderAgentGroupAssignmentCell(world, alice);

			// Recorded on the first, closed frame, while the cell's own item is
			// still the last one ImGui laid out.
			if (!cellRectRecorded)
			{
				cellMin = ImGui::GetItemRectMin();
				cellMax = ImGui::GetItemRectMax();
				cellRectRecorded = true;
				require(cellMax.x - cellMin.x > 1.0f && cellMax.y - cellMin.y > 1.0f,
					"The Group cell drew no frame to click");
			}

			ImGui::EndTable();
			ImGui::End();
			ImGui::Render();
		};

		// The preview: the assigned name, whole, before anything is opened.
		guard.clipboardWrites.clear();
		frame(&guard.clipboardWrites);
		require(guard.clipboardWrites.size() == 1,
			"The closed Group cell logged " + std::to_string(guard.clipboardWrites.size())
				+ " windows of visible text, expected one");
		require(guard.clipboardWrites.front().find("Crew##Night") != std::string::npos,
			"The selected preview hides part of the name: ["
				+ guard.clipboardWrites.front() + "]");

		// Open the list. The preview area is clicked, never the arrow button.
		float const clickX = cellMin.x + 4.0f;
		float const clickY = (cellMin.y + cellMax.y) * 0.5f;
		clickAt(clickX, clickY, frame);
		require(ImGui::IsPopupOpen(ImGuiID{}, ImGuiPopupFlags_AnyPopup),
			"Clicking the Group cell never opened the list");

		// Every name, in full, in the order the list shows them: `<none>`
		// first, then creation order.
		guard.clipboardWrites.clear();
		frame(&guard.clipboardWrites);
		std::string visible;
		for (auto const& write : guard.clipboardWrites) visible += write;

		for (auto const name : { "Crew##Day", "Crew##Night", "##Night shift", "Trip###le" })
			require(visible.find(name) != std::string::npos,
				(std::string("A choice hides part of its name: ") + name
					+ " is not in [" + visible + "]").c_str());

		require(drawnInOrder(visible, { "<none>", "Crew##Day", "Crew##Night",
			"##Night shift", "Trip###le" }),
			"The choices are not drawn in the order the list is meant to show them: ["
				+ visible + "]");

		// The rows, as the list itself lays them out: one for `<none>` plus one
		// for each group the World defines.
		size_t const expectedRows = 1 + 4;
		auto const rows = scanChoiceRows(clickX, cellMax.y + 1.0f,
			cellMax.y + 400.0f, frame);
		require(rows.size() == expectedRows,
			"The open list presented " + std::to_string(rows.size())
				+ " clickable rows, expected " + std::to_string(expectedRows));

		// Close the list before the picks, which each open it in turn.
		clickAt(clickX, clickY, frame);
		require(!ImGui::IsPopupOpen(ImGuiID{}, ImGuiPopupFlags_AnyPopup),
			"The Group list did not close when its cell was clicked again");

		// Each row is its own control: clicking the row that shows a name
		// assigns exactly that Agent group, and the preview that follows shows
		// the whole name again.
		struct Pick
		{
			size_t row;
			core::AgentGroupId group;
			std::string name;
		};

		std::vector<Pick> const picks{
			{ 1, crewDay, "Crew##Day" },
			{ 3, leadingHashes, "##Night shift" },
			{ 4, tripleHash, "Trip###le" },
			{ 2, crewNight, "Crew##Night" },
		};

		for (auto const& pick : picks)
		{
			// The list is closed between picks, because that is what picking does:
			// one choice, one assignment, the list back out of the way.
			clickAt(clickX, clickY, frame);
			require(ImGui::IsPopupOpen(ImGuiID{}, ImGuiPopupFlags_AnyPopup),
				"The Group list did not reopen for the next pick");

			clickAt(clickX, rows[pick.row], frame);
			require(!ImGui::IsPopupOpen(ImGuiID{}, ImGuiPopupFlags_AnyPopup),
				"Picking " + pick.name + " left the list open");
			require(world->getAgentGroup(alice) == pick.group,
				"Clicking the row showing " + pick.name + " assigned "
					+ std::to_string(world->getAgentGroup(alice).value)
					+ " instead of " + std::to_string(pick.group.value));

			guard.clipboardWrites.clear();
			frame(&guard.clipboardWrites);
			require(guard.clipboardWrites.size() == 1,
				"The closed Group cell did not log exactly one window of visible text");
			require(guard.clipboardWrites.front().find(pick.name) != std::string::npos,
				"The preview does not show the whole name just chosen: ["
					+ guard.clipboardWrites.front() + "]");
		}

		// And the `<none>` row at the head of the list still clears, with the
		// same one-edit-per-pick accounting the plain names get.
		clickAt(clickX, clickY, frame);
		clickAt(clickX, rows[0], frame);
		require(!world->getAgentGroup(alice),
			"The <none> row did not clear the assignment");
		// One edit per pick, the clearing included: five choices made, five
		// entries on the stack, no more.
		require(gWorldDocumentHistory.undoCount() == picks.size() + 1,
			"The hash-pair picks committed " + std::to_string(gWorldDocumentHistory.undoCount())
				+ " undoable edits, expected " + std::to_string(picks.size() + 1));

		// A rename that adds another pair shows up in the preview whole, the
		// same way a freshly chosen name does: the cell reads the name through
		// the World and draws it literally either way.
		require(world->setAgentGroup(alice, crewDay, &diagnostic)
			&& world->renameAgentGroup(crewDay, "Crew##Day##Night", &diagnostic),
			("Renaming a hash-pair group failed: " + diagnostic).c_str());
		guard.clipboardWrites.clear();
		frame(&guard.clipboardWrites);
		require(guard.clipboardWrites.front().find("Crew##Day##Night") != std::string::npos,
			"The preview does not follow a rename that added a second pair: ["
				+ guard.clipboardWrites.front() + "]");
	}
}

void agent_smoke::registerGroupAssignmentEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "anAssignedAgentFollowsItsGroupRename", [](smoke::Context const&) { EditorState state; anAssignedAgentFollowsItsGroupRename(); } });
	checks.push_back({ "aMissingAssignmentFieldLoadsAsNoGroup", [](smoke::Context const&) { EditorState state; aMissingAssignmentFieldLoadsAsNoGroup(); } });
	checks.push_back({ "assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach", [](smoke::Context const&) { EditorState state; assignmentEditsRunAlongsideTheSimulationAndCommitOneUndoEach(); } });
	checks.push_back({ "theGroupCellLabelShowsTheAssignment", [](smoke::Context const&) { EditorState state; theGroupCellLabelShowsTheAssignment(); } });
	checks.push_back({ "theGroupCellRendersWithoutLeakingImGuiState", [](smoke::Context const&) { EditorState state; theGroupCellRendersWithoutLeakingImGuiState(); } });
	checks.push_back({ "agentGroupNamesCarryingHashPairsReachTheScreenInFull", [](smoke::Context const&) { EditorState state; agentGroupNamesCarryingHashPairsReachTheScreenInFull(); } });
}
