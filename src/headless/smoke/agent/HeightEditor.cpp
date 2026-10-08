// Migrated from AgentHeightSmokeChecks.cpp (#286); editor dependency tier.

#include "AgentTagAssignmentPanel.h"
#include "AgentDropTargets.h"
#include "AgentClipboard.h"
#include "Render.h"
#include "TagsPanel.h"
#include "UISettings.h"

#include <algorithm>
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

	void rangeEditsAreSingleExactTransactions()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("revisioned");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(tag, { 0.8f, 0.8f }, &diagnostic),
			diagnostic);

		auto world = std::make_shared<core::World>("Height revisions", 8, 2);
		world->attachAgentTagRegistry("revisioned.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		world->pauseSimulation();
		std::vector<core::AgentId> agents;
		for (int index = 0; index < 3; ++index)
		{
			auto const agent = world->createAgent("Agent " + std::to_string(index),
				corridor, 0, static_cast<float>(index) + 0.5f);
			require(world->assignAgentTag(agent, tag, &diagnostic), diagnostic);
			agents.push_back(agent);
		}
		auto samples = [&]()
		{
			std::vector<core::AgentPropertySample> result;
			for (auto const agent : agents)
			{
				auto const& sample = world->lookupAgent(agent).entity
					->getHeightModifierSample();
				require(sample.has_value(), "A revisioned Agent lost its Height sample");
				result.push_back(*sample);
			}
			return result;
		};

		registry->markUnmodified();
		world->markSaved();
		forgetAgentTagRegistryDocument(registry);
		auto& history = agentTagRegistryDocumentHistory(registry);
		auto const originalProperty = *registry->getAgentTagHeightModifier(tag);
		auto const originalSamples = samples();
		auto const originalRegistry = serializeRegistry(*registry);
		auto const originalWorld = serializeWorld(*world);
		require(!commitAgentTagHeightModifierEdit(registry, tag,
			originalProperty.range, diagnostic)
			&& diagnostic.find("unchanged") != std::string::npos
			&& history.undoCount() == 0 && registry->getNextPropertyRevision() == 3
			&& serializeRegistry(*registry) == originalRegistry
			&& serializeWorld(*world) == originalWorld
			&& !agentTagRegistryIsModified(registry) && !world->isModified(),
			"An unchanged Height range consumed state, randomness, or history");

		require(commitAgentTagHeightModifierEdit(registry, tag, { 0.7f, 1.0f },
			diagnostic), diagnostic);
		auto const editedProperty = *registry->getAgentTagHeightModifier(tag);
		auto const editedSamples = samples();
		require(editedProperty.revision == 3 && registry->getNextPropertyRevision() == 4
			&& history.undoCount() == 1 && world->isModified(),
			"A Height range edit did not create one revisioned transaction");
		for (auto const& sample : editedSamples)
		{
			require(sample.type == core::SampledAgentPropertyType::HeightModifier
				&& sample.sourceTag == tag
				&& sample.propertyRevision == editedProperty.revision
				&& sample.value >= 0.7f && sample.value <= 1.0f,
				"A loaded inheriting Agent was not resampled exactly once");
		}

		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(*registry->getAgentTagHeightModifier(tag) == originalProperty
			&& samples() == originalSamples && registry->getNextPropertyRevision() == 4
			&& !world->isModified(),
			"Height undo did not restore the exact old revision and samples");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic), diagnostic);
		require(*registry->getAgentTagHeightModifier(tag) == editedProperty
			&& samples() == editedSamples && registry->getNextPropertyRevision() == 4
			&& world->isModified(),
			"Height redo rerolled instead of restoring exact replacement samples");

		require(commitAgentTagHeightModifierRemove(registry, tag, diagnostic), diagnostic);
		require(commitAgentTagHeightModifierAdd(registry, tag, diagnostic), diagnostic);
		auto const firstRevision = registry->getAgentTagHeightModifier(tag)->revision;
		require(firstRevision == 4, "Re-adding Height reused an old revision");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic), diagnostic);
		require(!registry->getAgentTagHeightModifier(tag)
			&& registry->getNextPropertyRevision() == 5,
			"Undoing Height addition made its issued revision reusable");
		require(commitAgentTagHeightModifierAdd(registry, tag, diagnostic), diagnostic);
		require(registry->getAgentTagHeightModifier(tag)->revision == 5
			&& registry->getNextPropertyRevision() == 6 && !history.canRedo(),
			"A replacement Height addition reused an abandoned revision");
		forgetAgentTagRegistryDocument(registry);
	}

	struct VertexExtent
	{
		float minY{ std::numeric_limits<float>::max() };
		float maxY{ std::numeric_limits<float>::lowest() };

		float height() const { return maxY - minY; }
	};

	VertexExtent vertexExtent(ImDrawList const* drawList, int firstVertex)
	{
		VertexExtent extent;
		for (int index = firstVertex; index < drawList->VtxBuffer.Size; ++index)
		{
			extent.minY = std::min(extent.minY, drawList->VtxBuffer[index].pos.y);
			extent.maxY = std::max(extent.maxY, drawList->VtxBuffer[index].pos.y);
		}
		return extent;
	}

	void captureClipboardText(void* userData, char const* text)
	{
		if (auto* writes = static_cast<std::vector<std::string>*>(userData))
			writes->emplace_back(text ? text : "");
	}

	char const* readClipboardText(void*) { return nullptr; }

	void heightChangesOnlyBoundsAndRendering()
	{
		auto registry = core::AgentTagRegistry::create();
		auto const shortTag = registry->addAgentTag("short");
		auto const tallTag = registry->addAgentTag("tall");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(shortTag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(shortTag, { 0.7f, 0.7f },
			&diagnostic), diagnostic);
		require(registry->addAgentTagHeightModifier(tallTag, &diagnostic), diagnostic);

		auto world = std::make_shared<core::World>("Visual Height", 12, 2);
		world->attachAgentTagRegistry("visual.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 11);
		uint32_t targetIdentifier{ 0x48313336u };
		world->addSectorMarker(corridor, 0, 10.5f, &targetIdentifier);
		world->finishBuild();
		auto const shortId = world->createAgent("Short", corridor, 0, 1.5f);
		auto const tallId = world->createAgent("Tall", corridor, 0, 3.5f);
		world->pauseSimulation();
		require(world->assignAgentTag(shortId, shortTag, &diagnostic), diagnostic);
		require(world->assignAgentTag(tallId, tallTag, &diagnostic), diagnostic);
		auto* shortAgent = world->lookupAgent(shortId).entity;
		auto* tallAgent = world->lookupAgent(tallId).entity;

		auto const& physical = shortAgent->getPhysicalBaseline();
		require(std::abs(shortAgent->getHeight() - physical.standingHeight * 0.7f)
			< 0.000001f && std::abs(tallAgent->getHeight() - physical.standingHeight)
			< 0.000001f,
			"Agent height did not use the sampled Height modifier");
		auto const shortBounds = shortAgent->getBounds();
		auto const tallBounds = tallAgent->getBounds();
		require(std::abs(shortBounds.getSize().y - shortAgent->getHeight()) < 0.000001f
			&& std::abs(tallBounds.getSize().y - tallAgent->getHeight()) < 0.000001f
			&& std::abs(shortBounds.getSize().x - physical.width) < 0.000001f
			&& std::abs(tallBounds.getSize().x - physical.width) < 0.000001f,
			"Height changed Agent width or failed to change visual bounds");
		require(std::abs(shortAgent->getWidth() - tallAgent->getWidth()) < 0.000001f
			&& std::abs(shortAgent->getWalkSpeed() - tallAgent->getWalkSpeed()) < 0.000001f
			&& std::abs(shortAgent->getClimbSpeed() - tallAgent->getClimbSpeed()) < 0.000001f,
			"Visual Height changed width or movement observations");

		// Creation previews have no World-owned Agent yet. They must agree with
		// actual Human bounds; modifiers change height, never the placement width.
		auto const baseline = core::Agent::placementDimensions("Human");
		require(baseline.x == tallAgent->getWidth()
			&& baseline.y == tallAgent->getStandingHeight(), "Human preview dimensions changed");
		gUISettings.visibleLayer = 0;
		auto const creation = pegmanAgentTargetAtWorld(world, { 0.01f, 0.1f });
		auto const moved = getAgentMoveTarget(world, shortAgent, { 10.99f, 0.1f });
		require(creation && moved && std::abs(creation.localX - baseline.x * 0.5f) < 0.000001f
			&& std::abs(moved.localX - (11.f - shortAgent->getWidth() * 0.5f)) < 0.000001f,
			"Human creation/move placement changed at the Location boundaries");
		require(world->getAgentAtPosition(0, 1.5f, shortAgent->getStandingHeight() * 0.5f) == shortAgent
			&& !world->getAgentAtPosition(0, 1.5f, baseline.y * 0.9f),
			"Human hit testing did not use modified bounds");
		require(world->setAgentIndividualHeightModifier(shortId, 0.9f, &diagnostic), diagnostic);
		require(shortAgent->getStandingHeight() == baseline.y * 0.9f
			&& shortAgent->getBounds().getSize().y == shortAgent->getStandingHeight()
			&& world->getAgentAtPosition(0, 1.5f, baseline.y * 0.8f) == shortAgent,
			"Individual Height did not override the inherited dimensions and hit bounds");
		require(world->setAgentIndividualHeightModifier(shortId, {}, &diagnostic), diagnostic);
		require(shortAgent->getStandingHeight() == baseline.y * 0.7f,
			"Removing individual Height did not reveal the retained tag sample");
		auto const payload = makeAgentClipboardPayload(*world, shortId, "Short copy");
		auto const pasteDimensions = agentClipboardPlacementDimensions(payload);
		require(pasteDimensions.x == shortAgent->getWidth()
			&& pasteDimensions.y == shortAgent->getStandingHeight(),
			"Tagged Human paste preview disagreed with the source dimensions");
		PendingAgentPlacement pending;
		require(armAgentPlacement(pending, *world, payload, world->getSector(corridor), 0, 5.5f, diagnostic), diagnostic);
		require(world->getSimulationSnapshot().agents.size() == 2, "Preview committed an Agent before landing");
		core::AgentId pasted;
		require(commitPendingAgentPlacement(pending, world, pasted, diagnostic), diagnostic);
		auto const* copy = world->lookupAgent(pasted).entity;
		require(copy && copy->getWidth() == pasteDimensions.x
			&& copy->getStandingHeight() == pasteDimensions.y,
			"Human paste landed with dimensions different from its preview");

		auto const target = world->getGraph()->getVertexByIdentifier(targetIdentifier);
		auto shortPath = world->getGraph()->calculatePath(shortAgent, target);
		auto tallPath = world->getGraph()->calculatePath(tallAgent, target);
		// Inferred Paths include the physical approach from each distinct spawn.
		auto const approachDifference = (shortAgent->getGlobalPosition().distanceTo(target->getPosition())
			- tallAgent->getGlobalPosition().distanceTo(target->getPosition())) / shortAgent->getWalkSpeed();
		require(shortPath && tallPath && shortPath->nodes.size() == tallPath->nodes.size()
			&& std::abs(shortPath->nodes.back().cumulativePerceivedCost
				- tallPath->nodes.back().cumulativePerceivedCost - approachDifference) < 0.000001f,
			"Visual Height changed path topology or route timing");
		auto const shortStart = shortAgent->getGlobalPosition();
		auto const tallStart = tallAgent->getGlobalPosition();
		shortAgent->setPath(std::move(shortPath), true);
		tallAgent->setPath(std::move(tallPath), true);
		shortAgent->update(0.1f);
		tallAgent->update(0.1f);
		require(std::abs(shortAgent->getGlobalPosition().distanceTo(shortStart)
			- tallAgent->getGlobalPosition().distanceTo(tallStart)) < 0.000001f,
			"Visual Height changed Agent movement");

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(1200.0f, 600.0f);
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
		auto const shortStartVertex = drawList->VtxBuffer.Size;
		renderAgent(shortAgent, drawList);
		auto const shortExtent = vertexExtent(drawList, shortStartVertex);
		auto const tallStartVertex = drawList->VtxBuffer.Size;
		renderAgent(tallAgent, drawList);
		auto const tallExtent = vertexExtent(drawList, tallStartVertex);
		require(shortExtent.height() > 0.0f && tallExtent.height() > shortExtent.height(),
			"The real renderer did not size the Agent icon from sampled Height");
		ImGui::EndFrame();

		ImGui::NewFrame();
		ImGui::Begin("Selection");
		ImGui::LogToClipboard();
		renderAgentEffectiveProperties(world, shortId);
		renderAgentEffectiveProperties(world, tallId);
		ImGui::End();
		ImGui::Render();
		std::string visible;
		for (auto const& text : clipboardWrites) visible += text;
		require(visible.find("Height modifier: 0.700x from #short")
				!= std::string::npos
			&& visible.find("Height modifier: 1.000x from #tall")
				!= std::string::npos,
			"Selection omitted sampled Height or its source tag");
	}
}

void agent_smoke::registerHeightEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/heightRangeEditsAreSingleExactTransactions", [](smoke::Context const&) { EditorState state; rangeEditsAreSingleExactTransactions(); } });
	checks.push_back({ "agent/heightChangesOnlyBoundsAndRendering", [](smoke::Context const&) { EditorState state; heightChangesOnlyBoundsAndRendering(); } });
}
