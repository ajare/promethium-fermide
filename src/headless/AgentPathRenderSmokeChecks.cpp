#include "Render.h"
#include "UISettings.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"

extern core::Agent* gSelectedAgent;
extern UISettings gUISettings;

void runAgentPathRenderSmokeChecks();

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	size_t lineCount(WorldDrawList const& list)
	{
		size_t count = 0;
		for (auto const& command : list.commands())
			if (std::holds_alternative<WorldDrawList::Line>(command)) ++count;
		return count;
	}

	bool hasText(WorldDrawList const& list, std::string const& value)
	{
		for (auto const& command : list.commands())
			if (auto text = std::get_if<WorldDrawList::Text>(&command);
				text && text->value == value) return true;
		return false;
	}

	bool hasSolidColour(WorldDrawList const& list, ImU32 colour)
	{
		for (auto const& command : list.commands())
			if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
				triangle && triangle->texture == WorldDrawList::Texture::None
				&& triangle->colour == colour) return true;
		return false;
	}
}

void runAgentPathRenderSmokeChecks()
{
	core::World world("Agent Path Render", 16, 1);
	auto const corridor = world.addCorridor(0, 0, 15);
	auto const isolatedCorridor = world.addCorridor(1, 0, 0, 15, 1);
	uint32_t sourceIdentifier = 0;
	uint32_t destinationIdentifier = 0;
	uint32_t isolatedIdentifier = 0;
	world.addSectorMarker(corridor, 0, 1.5f, &sourceIdentifier);
	world.addSectorMarker(corridor, 0, 13.5f, &destinationIdentifier);
	world.addSectorMarker(isolatedCorridor, 0, 13.5f, &isolatedIdentifier);
	world.finishBuild();

	auto const graph = world.getGraph();
	auto const source = graph->getVertexByIdentifier(sourceIdentifier);
	auto const destination = graph->getVertexByIdentifier(destinationIdentifier);
	auto const isolated = graph->getVertexByIdentifier(isolatedIdentifier);
	auto const agentId = world.createAgent("Selected", corridor, 0, 1.5f);
	auto* agent = world.lookupAgent(agentId).entity;
	auto path = graph->calculatePath(agent, source, destination);
	require(path && path->nodes.size() >= 2, "The Path rendering fixture has no Path");
	agent->setPath(path, false);

	auto const previousSettings = gUISettings;
	auto* const previousSelection = gSelectedAgent;
	gUISettings = UISettings{};
	gUISettings.visibleLayer = 0;
	gUISettings.renderAgentDebug = true;
	gUISettings.worldViewportWidth = 1280.0f;
	gUISettings.worldViewportHeight = 720.0f;
	gSelectedAgent = agent;

	WorldDrawList visible({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderSelectedAgentPath(&world, &visible);
	require(lineCount(visible) >= 2,
		"Agent debug did not render the selected Agent's calculated Path");

	ImGui::CreateContext();
	auto& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.DisplaySize = { 1280.0f, 720.0f };
	io.Fonts->AddFontDefault();
	io.Fonts->Build();
	ImGui::NewFrame();

	WorldDrawList labelsRemoved({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &labelsRemoved);
	require(!hasText(labelsRemoved, "IDL") && !hasText(labelsRemoved, "0/2"),
		"Legacy Agent state or Path progress text was rendered");

	world.replanAgentAfterAuthorizationRefusal(agentId);
	WorldDrawList greyReplan({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &greyReplan);
	require(!hasText(greyReplan, "?"), "Immediate replan rendered a planning badge");

	WorldDrawList successfulReplan({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &successfulReplan);
	require(!hasText(successfulReplan, "?")
		&& !hasSolidColour(successfulReplan, IM_COL32(40, 160, 72, 255)),
		"A successful replan rendered the obsolete result phase");

	agent->pausePathing();
	core::InteractionBinding binding;
	binding.command = { core::DeviceCommandType::SetSectorLights,
		core::SectorId{ corridor + 1 }, false };
	auto const point = world.createInteractionPoint("Queue", core::SectorId{ corridor + 1 },
		{ 2.0f, 0.0f }, 0.25f, 1.0f, { binding });
	require(world.requestInteraction(point, agentId).value != 0,
		"The Agent debug queue fixture did not create an Interaction request");
	require(world.isAgentInQueue(agentId),
		"An Agent in an Interaction point queue was not reported as queued");
	WorldDrawList stacked({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &stacked);
	require(hasText(stacked, "!")
		&& hasSolidColour(stacked, IM_COL32(230, 126, 34, 255)),
		"A queued Agent did not render a white exclamation mark in an orange box");
	require(!hasText(stacked, "?"), "Queue displayed an obsolete replan badge");

	auto unreachable = std::make_shared<core::Path>();
	unreachable->nodes.push_back({ nullptr, source, 0.0f });
	unreachable->nodes.push_back({ nullptr, isolated, 0.0f });
	agent->setPath(unreachable, false);
	world.replanAgentAfterAuthorizationRefusal(agentId);
	WorldDrawList failedGrey({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &failedGrey);
	WorldDrawList failedReplan({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &failedReplan);
	require(!hasText(failedReplan, "?")
		&& !hasSolidColour(failedReplan, IM_COL32(200, 48, 48, 255)),
		"A failed replan rendered the obsolete result phase");

	auto const plannerId = world.createAgent("Planner", corridor, 0, 3.25f);
	auto* planner = world.lookupAgent(plannerId).entity;
	require(world.moveAgentToMarker(plannerId, world.getMarkerIds()[1]).accepted(), "Planning refused");
	for (int frame = 0; frame < 3; ++frame)
	{
		WorldDrawList planning({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
		renderAgent(planner, &planning);
		require(hasText(planning, "?") && !hasText(planning, "!")
			&& hasSolidColour(planning, IM_COL32(96, 96, 96, 255)),
			"Route planning must render only a neutral grey question badge on every frame");
	}
	world.advanceTicks(planner->getRoutePlanningRemainingTicks());
	WorldDrawList expired({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(planner, &expired);
	require(!hasText(expired, "?"), "Planning badge survived expiry");

	gUISettings.renderAgentDebug = false;
	WorldDrawList hiddenBadges({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &hiddenBadges);
	require(!hasText(hiddenBadges, "!") && !hasText(hiddenBadges, "?"),
		"Agent queue or replan badges rendered while Agent debug was disabled");
	gUISettings.renderAgentDebug = true;

	ImGui::EndFrame();
	ImGui::DestroyContext();

	// Restore a valid Path for the selected-Path pause check below.
	agent->setPath(path, false);
	world.pauseSimulation();
	require(!agent->getPath(),
		"Pausing the fixture did not tear down the Agent's live Path");
	WorldDrawList paused({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderSelectedAgentPath(&world, &paused);
	require(lineCount(paused) >= 2,
		"Agent debug did not render the selected Agent's calculated Path while paused");

	gUISettings.renderAgentDebug = false;
	WorldDrawList disabled({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderSelectedAgentPath(&world, &disabled);
	require(disabled.commands().empty(),
		"The calculated Path rendered while Agent debug was disabled");

	gUISettings.renderAgentDebug = true;
	gSelectedAgent = nullptr;
	WorldDrawList unselected({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderSelectedAgentPath(&world, &unselected);
	require(unselected.commands().empty(),
		"A calculated Path rendered without a selected Agent");

	gSelectedAgent = previousSelection;
	gUISettings = previousSettings;
}
