// Migrated from AgentWalkSpeedSmokeChecks.cpp (#286); editor dependency tier.

#include "AgentTagAssignmentPanel.h"
#include "TagsPanel.h"

#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "imgui/imgui.h"

#include "core/Agent.h"
#include "core/AgentTag.h"
#include "core/AgentTagRegistry.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Graph.h"
#include "core/SerializationWorkData.h"
#include "core/Staircase.h"
#include "core/StaircaseEdge.h"
#include "core/YamlSerializer.h"

#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

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

	void rangeEditsResampleOnceAndRestoreExactSamples()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("revisioned");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(tag, { 0.85f, 0.85f },
			&diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Revisioned", 8, 2);
		world->attachAgentTagRegistry("revisioned.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		world->pauseSimulation();
		std::vector<core::AgentId> agents;
		for (int index = 0; index < 4; ++index)
		{
			auto const agent = world->createAgent(
				"Revisioned " + std::to_string(index), corridor, 0,
				static_cast<float>(index) + 0.5f);
			require(world->assignAgentTag(agent, tag, &diagnostic), diagnostic);
			agents.push_back(agent);
		}
		auto samples = [&]()
		{
			std::vector<core::AgentPropertySample> values;
			for (auto const agent : agents)
			{
				auto const& sample = world->lookupAgent(agent).entity
					->getWalkSpeedModifierSample();
				require(sample.has_value(), "A revisioned Agent lost its sample");
				values.push_back(*sample);
			}
			return values;
		};

		registry->markUnmodified();
		world->markSaved();
		forgetAgentTagRegistryDocument(registry);
		auto& history = agentTagRegistryDocumentHistory(registry);
		auto const originalProperty = *registry->getAgentTagWalkSpeedModifier(tag);
		auto const originalSamples = samples();
		auto const originalRegistry = serializeRegistry(*registry);
		auto const originalWorld = serializeWorld(*world);

		// Submitting the currently authored range is a complete no-op: even clean
		// document state and the allocator remain untouched.
		require(!commitAgentTagWalkSpeedModifierEdit(registry, tag,
			originalProperty.range, diagnostic)
			&& diagnostic.find("unchanged") != std::string::npos
			&& history.undoCount() == 0
			&& registry->getNextPropertyRevision() == 3
			&& serializeRegistry(*registry) == originalRegistry
			&& serializeWorld(*world) == originalWorld
			&& !agentTagRegistryIsModified(registry) && !world->isModified(),
			"An unchanged Walk speed range consumed state, history, or samples");

		require(commitAgentTagWalkSpeedModifierEdit(
			registry, tag, { 0.8f, 1.2f }, diagnostic), diagnostic);
		auto const editedProperty = *registry->getAgentTagWalkSpeedModifier(tag);
		auto const editedSamples = samples();
		require(editedProperty.revision == 3
			&& registry->getNextPropertyRevision() == 4
			&& history.undoCount() == 1 && world->isModified(),
			"A real range edit did not allocate one revision and one transaction");
		for (auto const& sample : editedSamples)
		{
			require(sample.sourceTag == tag
				&& sample.propertyRevision == editedProperty.revision
				&& sample.value >= editedProperty.range.minimum
				&& sample.value <= editedProperty.range.maximum,
				"A loaded Agent was not resampled against the committed range");
		}

		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(*registry->getAgentTagWalkSpeedModifier(tag) == originalProperty
			&& samples() == originalSamples
			&& registry->getNextPropertyRevision() == 4
			&& agentTagRegistryIsModified(registry) && !world->isModified(),
			"Undo did not restore the exact old range, revision, samples, and World dirty state");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
		require(*registry->getAgentTagWalkSpeedModifier(tag) == editedProperty
			&& samples() == editedSamples
			&& registry->getNextPropertyRevision() == 4
			&& agentTagRegistryIsModified(registry) && world->isModified(),
			"Redo rerolled instead of restoring the original replacement samples");

		// Removing and adding creates a new property instance. If that add is
		// undone and a different add is committed, even the abandoned revision is
		// retained in the allocator high-water mark and cannot be reissued.
		require(commitAgentTagWalkSpeedModifierRemove(registry, tag, diagnostic), diagnostic);
		require(commitAgentTagWalkSpeedModifierAdd(registry, tag, diagnostic), diagnostic);
		auto const firstReaddedRevision
			= registry->getAgentTagWalkSpeedModifier(tag)->revision;
		auto const firstReaddedSamples = samples();
		require(firstReaddedRevision == 4, "Re-adding the modifier reused its old revision");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(!registry->getAgentTagWalkSpeedModifier(tag)
			&& registry->getNextPropertyRevision() == 5,
			"Undoing a modifier add made its issued revision reusable");
		for (auto const agent : agents)
			require(!world->lookupAgent(agent).entity->getWalkSpeedModifierSample(),
				"Undoing a modifier add retained an Agent sample");
		require(commitAgentTagWalkSpeedModifierAdd(registry, tag, diagnostic), diagnostic);
		auto const secondReaddedRevision
			= registry->getAgentTagWalkSpeedModifier(tag)->revision;
		require(secondReaddedRevision == 5
			&& registry->getNextPropertyRevision() == 6
			&& samples() != firstReaddedSamples && !history.canRedo(),
			"A replacement add reused an abandoned revision or undo-restored samples");
		forgetAgentTagRegistryDocument(registry);
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readClipboardText(void*) { return nullptr; }

	void selectionReportsSampleAndSource()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("sprinter");
		std::string diagnostic;
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagWalkSpeedModifier(tag, { 1.125f, 1.125f }, &diagnostic),
			diagnostic);
		auto world = std::make_shared<core::World>("Inspection", 5, 2);
		world->attachAgentTagRegistry("inspection.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 4);
		world->finishBuild();
		auto const sampled = world->createAgent("Sampled", corridor);
		auto const plain = world->createAgent("Plain", corridor);
		world->pauseSimulation();
		require(world->assignAgentTag(sampled, tag, &diagnostic), diagnostic);

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		std::vector<std::string> clipboardWrites;
		io.SetClipboardTextFn = &captureClipboardText;
		io.GetClipboardTextFn = &readClipboardText;
		io.ClipboardUserData = &clipboardWrites;
		ImGui::NewFrame();
		ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(world, sampled);
		renderAgentEffectiveProperties(world, plain);
		ImGui::End();
		ImGui::Render();
		std::string visible;
		for (auto const& text : clipboardWrites) visible += text;
		require(visible.find("Walk speed modifier: 1.125x from #sprinter")
				!= std::string::npos
			&& visible.find("Walk speed modifier: 1.000x (base default)")
				!= std::string::npos,
			"Selection omitted the sampled Walk speed, source tag, or base default");
	}
}

void agent_smoke::registerWalkSpeedEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/walkSpeedRangeEditsResampleOnceAndRestoreExactSamples", [](smoke::Context const&) { EditorState state; rangeEditsResampleOnceAndRestoreExactSamples(); } });
	checks.push_back({ "agent/walkSpeedSelectionReportsSampleAndSource", [](smoke::Context const&) { EditorState state; selectionReportsSampleAndSource(); } });
}
