// Migrated from AgentColourSmokeChecks.cpp (#286); editor dependency tier.
//
// The Agent Colour tag is gone, and with it the editor's Colour editing,
// revision handling and inheritance-conflict machinery. What remains here
// is the other half of what that property used to be able to do: prove the
// Agent renderer draws raw artwork. An Agent's sprite carries no colour of
// its own, and selection is an outline laid over that sprite rather than a
// colour replacing it.

#include "AgentTagAssignmentPanel.h"
#include "WorldDrawReplay.h"
#include "Render.h"
#include "ObjectTileset.h"
#include "TagsPanel.h"
#include "UISettings.h"

#include <algorithm>
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

	// The colours a sprite must never be drawn with: the old editor fallback
	// for an untagged Agent, the old inherited-Colour value, and the old
	// gold selection tint. None of them reaches the artwork any more.
	ImU32 const kFormerFallback{ ImColor(179, 77, 77) };
	ImU32 const kFormerInherited{ ImColor(12, 34, 56) };
	ImU32 const kSelection{ ImColor(255, 255, 0) };

	// Every vertex appended since `from`, so a check can speak about one
	// renderAgent() call rather than the whole draw list.
	std::vector<ImU32> appendedColours(ImDrawList const* drawList, int from)
	{
		std::vector<ImU32> out;
		for (int i = from; i < drawList->VtxBuffer.Size; ++i)
			out.push_back(drawList->VtxBuffer[i].col);
		return out;
	}

	bool anyOf(std::vector<ImU32> const& colours, ImU32 want)
	{
		return std::find(colours.begin(), colours.end(), want) != colours.end();
	}

	//
	// An Agent's sprite is its artwork and nothing else. There is no Agent
	// Colour to resolve and nothing is multiplied into the sprite, so every
	// vertex the sprite draws is raw. Selection adds an outline over that
	// same picture rather than replacing its colour.
	//
	void agentsDrawRawArtworkAndSelectionIsAnOutline()
	{
		struct ArtworkScope {
			ArtworkScope() {
				ObjectTileset tiles; tiles.width = 26; tiles.height = 72;
				tiles.sprites.emplace("human-standing", ObjectSprite{{0, 0, 26, 72}});
				setAgentTileset(std::move(tiles), static_cast<TextureId>(1));
			}
			~ArtworkScope() { clearAgentTileset(); }
		} artwork;

		auto registry = core::AgentTagRegistry::create();
		auto world = std::make_shared<core::World>("Raw artwork", 8, 2);
		world->attachAgentTagRegistry("raw.artwork.tags.yaml", registry);
		auto const corridor = world->addCorridor(0, 0, 7);
		world->finishBuild();
		auto const first = world->createAgent("First", corridor, 0, 1.5f);
		auto const second = world->createAgent("Second", corridor, 0, 3.5f);
		world->pauseSimulation();
		auto const firstEntity = world->lookupAgent(first).entity;
		auto const secondEntity = world->lookupAgent(second).entity;

		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(800.0f, 600.0f);
		io.Fonts->AddFontDefault();
		io.Fonts->Build();
		gUISettings.worldViewportHeight = 600.0f;

		ImGui::NewFrame();
		auto* drawList = ImGui::GetForegroundDrawList();
		gSelectedAgent = nullptr;
		auto const beforeFirst = drawList->VtxBuffer.Size;
		renderAgent(firstEntity, drawList);
		auto const firstVertices = appendedColours(drawList, beforeFirst);
		auto const beforeSecond = drawList->VtxBuffer.Size;
		renderAgent(secondEntity, drawList);
		auto const secondVertices = appendedColours(drawList, beforeSecond);

		require(!firstVertices.empty() && !secondVertices.empty(),
			"renderAgent() drew no artwork");
		for (auto const& vertices : { firstVertices, secondVertices })
		{
			require(!anyOf(vertices, kFormerFallback)
					&& !anyOf(vertices, kFormerInherited)
					&& !anyOf(vertices, kSelection),
				"An Agent's sprite was drawn with a colour multiplied into it");
		}

		// Selecting adds the outline and nothing else: the sprite underneath
		// is the same picture it was unselected.
		auto const beforeSelected = drawList->VtxBuffer.Size;
		gSelectedAgent = firstEntity;
		renderAgent(firstEntity, drawList);
		auto const selectedVertices = appendedColours(drawList, beforeSelected);
		require(selectedVertices.size() > firstVertices.size(),
			"Selecting an Agent drew no outline over its artwork");
		require(anyOf(selectedVertices, kSelection),
			"The selection outline is not drawn in the selection colour");
		require(!anyOf(selectedVertices, kFormerFallback)
				&& !anyOf(selectedVertices, kFormerInherited),
			"Selecting an Agent still multiplied a colour into its sprite");
		gSelectedAgent = nullptr;
		ImGui::EndFrame();
	}
}

void agent_smoke::registerColourEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agent/agentsDrawRawArtworkAndSelectionIsAnOutline", [](smoke::Context const&) { EditorState state; agentsDrawRawArtworkAndSelectionIsAnOutline(); } });
}
