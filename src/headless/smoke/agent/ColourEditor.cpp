// Migrated from AgentColourSmokeChecks.cpp (#286); editor dependency tier.

#include "AgentTagAssignmentPanel.h"
#include "Render.h"
#include "TagsPanel.h"
#include "UISettings.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

extern core::Agent* gSelectedAgent;
extern UISettings gUISettings;

#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct TemporaryDirectory
	{
		std::filesystem::path path;

		explicit TemporaryDirectory(std::string const& purpose)
		{
			path = std::filesystem::temp_directory_path()
				/ ("promethium-fermide-tag-colour-" + purpose + "-"
					+ std::to_string(std::chrono::steady_clock::now()
						.time_since_epoch().count()));
			std::filesystem::create_directories(path);
		}

		~TemporaryDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	};

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		registry.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	struct ColourFixture
	{
		std::shared_ptr<core::AgentTagRegistry> registry{ core::AgentTagRegistry::create() };
		std::shared_ptr<core::World> world{
			std::make_shared<core::World>("Colours", 8, 2) };
		core::AgentTagId red{ registry->addAgentTag("red") };
		core::AgentTagId blue{ registry->addAgentTag("blue") };
		core::AgentTagId plain{ registry->addAgentTag("plain") };
		core::AgentId coloured{};
		core::AgentId fallback{};

		ColourFixture()
		{
			std::string diagnostic;
			require(registry->addAgentTagColour(red, &diagnostic), diagnostic);
			require(registry->setAgentTagColour(red, { 12, 34, 56 }, &diagnostic), diagnostic);
			world->attachAgentTagRegistry("colours.tags.yaml", registry);
			auto const corridor = world->addCorridor(0, 0, 7);
			world->finishBuild();
			coloured = world->createAgent("Coloured", corridor, 0, 1.5f);
			fallback = world->createAgent("Fallback", corridor, 0, 3.5f);
			world->pauseSimulation();
			require(world->assignAgentTag(coloured, red, &diagnostic), diagnostic);
		}
	};

	void assignmentAndPropertyAdditionConflictsAreAtomic()
	{
		ColourFixture fixture;
		std::string diagnostic;
		require(fixture.registry->addAgentTagColour(fixture.blue, &diagnostic), diagnostic);
		auto const beforeAssignment = serializeWorld(*fixture.world);
		require(!fixture.world->assignAgentTag(
			fixture.coloured, fixture.blue, &diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& diagnostic.find("#red") != std::string::npos
			&& diagnostic.find("#blue") != std::string::npos
			&& serializeWorld(*fixture.world) == beforeAssignment,
			"A conflicting Colour assignment was not refused atomically with both sources");

		// Assigning property-free tags is still unrestricted. Adding Colour to
		// one afterwards must preflight every loaded Agent before consuming a
		// revision or touching the tag.
		require(fixture.world->assignAgentTag(
			fixture.coloured, fixture.plain, &diagnostic), diagnostic);
		forgetAgentTagRegistryDocument(fixture.registry);
		auto& history = agentTagRegistryDocumentHistory(fixture.registry);
		auto const revisionBefore = fixture.registry->getNextPropertyRevision();
		auto const registryBefore = serializeRegistry(*fixture.registry);
		require(!commitAgentTagColourAdd(fixture.registry, fixture.plain, diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& diagnostic.find("Coloured") != std::string::npos
			&& diagnostic.find("#red") != std::string::npos
			&& fixture.registry->getNextPropertyRevision() == revisionBefore
			&& serializeRegistry(*fixture.registry) == registryBefore
			&& history.undoCount() == 0,
			"A conflicting Colour addition mutated the registry, revision, or history");
		forgetAgentTagRegistryDocument(fixture.registry);
	}

	void editorCommitsRevisionedColourAndUndoRedoExactly()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("crew");
		forgetAgentTagRegistryDocument(registry);
		(void)agentTagRegistryDocumentHistory(registry);
		std::string diagnostic;
		require(commitAgentTagColourAdd(registry, tag, diagnostic), diagnostic);
		require(commitAgentTagColourEdit(registry, tag, { 4, 80, 160 }, diagnostic), diagnostic);
		auto const edited = *registry->getAgentTagColour(tag);
		auto const undoCount = agentTagRegistryDocumentHistory(registry).undoCount();
		require(!commitAgentTagColourEdit(registry, tag, edited.value, diagnostic)
			&& agentTagRegistryDocumentHistory(registry).undoCount() == undoCount
			&& registry->getNextPropertyRevision() == edited.revision + 1,
			"A no-op editor Colour submission created history or consumed a revision");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		auto const* undone = registry->getAgentTagColour(tag);
		require(undone && undone->value == core::EditorDefaultAgentColour
			&& undone->revision == 1 && registry->getNextPropertyRevision() == 3,
			"Undo did not restore the exact initial Colour while retaining the revision high-water mark");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
		require(*registry->getAgentTagColour(tag) == edited,
			"Redo did not restore the exact edited Colour and revision");
		forgetAgentTagRegistryDocument(registry);
	}

	void conflictingColourRedoIsRefusedAtomically()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const source = registry->addAgentTag("source");
		auto const target = registry->addAgentTag("target");
		std::string diagnostic;
		require(registry->addAgentTagColour(source, &diagnostic), diagnostic);
		forgetAgentTagRegistryDocument(registry);
		(void)agentTagRegistryDocumentHistory(registry);
		require(commitAgentTagColourAdd(registry, target, diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Redo conflict", 6, 2);
		world->attachAgentTagRegistry("redo.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 5);
		world->finishBuild();
		auto const agent = world->createAgent("Redo Agent", corridor);
		world->pauseSimulation();
		require(world->assignAgentTag(agent, target, &diagnostic), diagnostic);
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic)
			&& !registry->getAgentTagColour(target),
			"Undo did not remove the newly added Colour: " + diagnostic);
		require(world->assignAgentTag(agent, source, &diagnostic), diagnostic);

		auto const registryBefore = serializeRegistry(*registry);
		auto const worldBefore = serializeWorld(*world);
		require(!restoreAgentTagRegistrySnapshot(registry, true, &diagnostic)
			&& diagnostic.find("Colour") != std::string::npos
			&& serializeRegistry(*registry) == registryBefore
			&& serializeWorld(*world) == worldBefore,
			"Redo introduced a conflicting Colour or partially changed loaded state");
		forgetAgentTagRegistryDocument(registry);
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readClipboardText(void*) { return nullptr; }

	bool drawListContainsColour(ImDrawList const* drawList, ImU32 colour)
	{
		for (int i = 0; i < drawList->VtxBuffer.Size; ++i)
			if (drawList->VtxBuffer[i].col == colour) return true;
		return false;
	}

	void effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold()
	{
		ColourFixture fixture;
		auto const coloured = fixture.world->lookupAgent(fixture.coloured).entity;
		auto const fallback = fixture.world->lookupAgent(fixture.fallback).entity;
		auto const effective = coloured->getEffectiveColour();
		require(effective.value == (core::AgentColour{ 12, 34, 56 })
			&& effective.sourceTag == fixture.red,
			"The Agent did not expose its inherited Colour and source tag");
		require(fallback->getEffectiveColour().value == core::EditorDefaultAgentColour
			&& !fallback->getEffectiveColour().sourceTag,
			"An uncoloured Agent did not expose the editor fallback");

		require(agentRenderColour(*coloured, false) == ImU32(ImColor(12, 34, 56))
			&& agentRenderColour(*fallback, false)
				== ImU32(ImColor(179, 77, 77))
			&& agentRenderColour(*coloured, true)
				== ImU32(ImColor(251, 188, 4)),
			"Agent render-colour resolution does not preserve inherited, fallback, and gold");

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readClipboardText;
		io.ClipboardUserData = &clipboardWrites;
		gUISettings.worldViewportHeight = 600.0f;

		ImGui::NewFrame();
		auto* drawList = ImGui::GetForegroundDrawList();
		gSelectedAgent = nullptr;
		renderAgent(coloured, drawList);
		renderAgent(fallback, drawList);
		gSelectedAgent = coloured;
		renderAgent(coloured, drawList);
		require(drawListContainsColour(drawList, ImU32(ImColor(12, 34, 56)))
			&& drawListContainsColour(drawList, ImU32(ImColor(179, 77, 77)))
			&& drawListContainsColour(drawList, ImU32(ImColor(251, 188, 4))),
			"The real Agent renderer did not emit inherited, fallback, and selected colours");
		gSelectedAgent = nullptr;
		ImGui::EndFrame();

		ImGui::NewFrame();
		ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(fixture.world, fixture.coloured);
		renderAgentEffectiveProperties(fixture.world, fixture.fallback);
		ImGui::End();
		ImGui::Render();
		std::string visible;
		for (auto const& text : clipboardWrites) visible += text;
		require(visible.find("RGB (12, 34, 56) from #red") != std::string::npos
			&& visible.find("RGB (179, 77, 77) (editor default)") != std::string::npos,
			"The Selection panel omitted the effective Colour, source tag, or editor default");
	}
}

void agent_smoke::registerColourEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/assignmentAndPropertyAdditionConflictsAreAtomic", [](smoke::Context const&) { EditorState state; assignmentAndPropertyAdditionConflictsAreAtomic(); } });
	checks.push_back({ "agent/editorCommitsRevisionedColourAndUndoRedoExactly", [](smoke::Context const&) { EditorState state; editorCommitsRevisionedColourAndUndoRedoExactly(); } });
	checks.push_back({ "agent/conflictingColourRedoIsRefusedAtomically", [](smoke::Context const&) { EditorState state; conflictingColourRedoIsRefusedAtomically(); } });
	checks.push_back({ "agent/effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold", [](smoke::Context const&) { EditorState state; effectiveInspectionAndRealRenderingUseInheritedFallbackAndGold(); } });
}
