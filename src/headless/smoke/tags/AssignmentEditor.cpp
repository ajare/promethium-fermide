#include "ImGuiContext.h"
#include "Checks.h"
#include "EditorState.h"
// Property-free Agent tag assignments, ticket #131.

#include "AgentTagAssignmentPanel.h"
#include "AgentClipboard.h"
#include "DocumentEdit.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "core/Agent.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::shared_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(world->deserialize(*reader, workData), "The World did not deserialize");
		return world;
	}

	struct Fixture
	{
		std::shared_ptr<core::World> world;
		std::shared_ptr<core::AgentTagRegistry> registry;
		core::AgentTagId crew;
		core::AgentTagId night;
		core::AgentId alice;
		uint32_t corridor;

		Fixture()
			: world(std::make_shared<core::World>("Tag assignments", 10, 3))
			, registry(core::AgentTagRegistry::create())
		{
			crew = registry->addAgentTag("crew");
			night = registry->addAgentTag("night-shift");
			world->attachAgentTagRegistry("shared.tags.yaml", registry);
			corridor = world->addCorridor(0, 0, 8);
			world->finishBuild();
			alice = world->createAgent("Alice", corridor, 0, 1.5f);
		}
	};

	void newAndPalettePlacedAgentsRemainUntagged()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The fixture Agent could not be tagged");

		auto const normal = fixture.world->createAgent("Bob", fixture.corridor, 0, 2.5f);
		require(fixture.world->getAgentTags(normal).empty(),
			"Normal Agent creation copied an existing Agent's tags");

		gWorldDocumentHistory.clear();
		core::AgentId placed{};
		auto const sector = fixture.world->getSector(fixture.corridor);
		require(commitAgentPlacement(fixture.world,
			AgentClipboardPayload{ "Palette Agent", 0, true, std::nullopt },
			sector, 0, 3.5f, placed, diagnostic),
			"The palette-equivalent Agent placement failed: " + diagnostic);
		require(placed && fixture.world->getAgentTags(placed).empty(),
			"A palette-placed Agent did not start untagged");
	}

	void editorCommitsOneWorldUndoEntryPerAcceptedEdit()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		gWorldDocumentHistory.clear();
		std::string diagnostic;

		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, true, diagnostic), "The editor seam refused the first assignment");
		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.night, true, diagnostic), "The editor seam refused the second assignment");
		require(gWorldDocumentHistory.undoCount() == 2,
			"Two accepted assignment edits did not commit two World undo entries");
		require(!commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, true, diagnostic)
			&& gWorldDocumentHistory.undoCount() == 2,
			"A duplicate assignment committed a World undo entry");

		auto current = captureDocumentSnapshot(fixture.world);
		std::shared_ptr<core::World> restored;
		auto restore = [&](DocumentSnapshot const& target)
		{
			restored = deserializeWorld(target.yaml);
			restored->resolveAgentTagRegistry(fixture.registry);
			return true;
		};
		require(gWorldDocumentHistory.undo(std::move(current), restore),
			"Undo refused the accepted Agent tag assignment");
		fixture.world = restored;
		require(fixture.world->getAgentTags(fixture.alice)
			== std::set<core::AgentTagId>{ fixture.crew },
			"Undo did not remove exactly the last assigned tag");

		current = captureDocumentSnapshot(fixture.world);
		require(gWorldDocumentHistory.redo(std::move(current), restore),
			"Redo refused the Agent tag assignment");
		fixture.world = restored;
		require(fixture.world->getAgentTags(fixture.alice)
			== std::set<core::AgentTagId>{ fixture.crew, fixture.night },
			"Redo did not restore the two-tag assignment set");

		fixture.world->pauseSimulation();
		auto const entriesBeforeRemoval = gWorldDocumentHistory.undoCount();
		require(commitAgentTagAssignment(fixture.world, fixture.alice,
			fixture.crew, false, diagnostic)
			&& gWorldDocumentHistory.undoCount() == entriesBeforeRemoval + 1
			&& fixture.world->getAgentTags(fixture.alice)
				== std::set<core::AgentTagId>{ fixture.night },
			"Removing an assigned tag did not commit exactly one World undo entry");
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readCapturedClipboardText(void*) { return nullptr; }

	void selectionPanelRendersAssignedChipsWithoutLeakingDisabledState()
	{
		Fixture fixture;
		fixture.world->pauseSimulation();
		std::string diagnostic;
		require(fixture.world->assignAgentTag(fixture.alice, fixture.crew, &diagnostic),
			"The checklist fixture could not assign its removable tag");

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readCapturedClipboardText;
		io.ClipboardUserData = &clipboardWrites;

		for (bool paused : { true, false })
		{
			if (paused) fixture.world->pauseSimulation();
			else require(fixture.world->resumeSimulation(),
				"The checklist fixture could not resume simulation");

			clipboardWrites.clear();
			ImGui::NewFrame();
			ImGui::Begin("Selection");
			ImGui::LogToClipboard();
			auto const disabledDepth = GImGui->DisabledStackSize;
			renderAgentTagAssignmentChecklist(fixture.world, fixture.alice);
			require(GImGui->DisabledStackSize == disabledDepth,
				"The Agent tag checklist leaked a disabled scope");
			ImGui::End();
			ImGui::Render();

			std::string visible;
			for (auto const& text : clipboardWrites) visible += text;
			require(visible.find("Agent tags") != std::string::npos
				&& visible.find("#crew") != std::string::npos
				&& visible.find("Add tag...") != std::string::npos,
				"The Selection panel did not present the assigned tag chip and add-tag combo");
			require(visible.find("#night-shift") == std::string::npos,
				"The Selection panel listed an unassigned tag outside the add-tag combo");
			require(fixture.world->getAgentTags(fixture.alice)
				== std::set<core::AgentTagId>{ fixture.crew },
				"Merely rendering the tag chips changed its assigned tag");
		}
	}
}

void tag_smoke::registerAssignmentEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "tags/newAndPalettePlacedAgentsRemainUntagged",
		[](smoke::Context const&)
		{
			EditorState state;
			newAndPalettePlacedAgentsRemainUntagged();
		} });
	checks.push_back({ "tags/editorCommitsOneWorldUndoEntryPerAcceptedEdit",
		[](smoke::Context const&)
		{
			EditorState state;
			editorCommitsOneWorldUndoEntryPerAcceptedEdit();
		} });
	checks.push_back({ "tags/selectionPanelRendersAssignedChipsWithoutLeakingDisabledState",
		[](smoke::Context const&)
		{
			EditorState state;
			selectionPanelRendersAssignedChipsWithoutLeakingDisabledState();
		} });
}
