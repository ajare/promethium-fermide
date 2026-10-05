#include "AgentPaths.h"
#include "ImGuiContext.h"

#include "Render.h"
#include "UISettings.h"

#include <memory>
#include <set>
#include "core/Marker.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "core/Agent.h"
#include "core/Graph.h"
#include "core/World.h"

extern core::Agent* gSelectedAgent;
extern UISettings gUISettings;

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

	ImVec2 textPosition(WorldDrawList const& list, std::string const& value)
	{
		for (auto const& command : list.commands())
			if (auto text = std::get_if<WorldDrawList::Text>(&command);
				text && text->value == value) return text->position;
		throw std::runtime_error("Missing badge text: " + value);
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

void agentPathTargets(smoke::Context const& context)
{
	auto world = std::make_shared<core::World>("Marker targets", 12, 2);
	auto room = world->addRoom("Room", 0, 0, 0, 12, 1);
	auto other = world->addRoom("Other Layer", 1, 0, 0, 12, 1);
	world->addSectorMarker(room, 0, 8.5f, "Standalone target");
	world->addSectorMarker(other, 0, 8.5f, "Other Layer target");
	world->finishBuild(); world->pauseSimulation();
	world->attachFurnitureCatalogue("desk.furniture.yaml",
		core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/legacy-furniture/desk.furniture.yaml")));
	auto id = world->placeFurniture(room, "desk", 1.25f, 0, "Desk", 2);
	world->finishBuild();
	auto seat = markerPathTargetAtPosition(*world, 0, {2, 0}, .1f);
	require(seat && std::dynamic_pointer_cast<core::Marker>(seat->getObject())
		&& isMarkerPathTarget(*world, *seat), "Furniture usable Marker was not selectable as a path target");
	require(markerPathTargetAtPosition(*world, 0, {8.5f, 0}, .1f)
		&& markerPathTargetAtPosition(*world, 1, {8.5f, 0}, .1f), "Standalone Marker or Layer filtering failed");
	require(!markerPathTargetAtPosition(*world, 0, {1.5f, 0}, .1f)
		&& !markerPathTargetAtPosition(*world, 0, {1.25f, 0}, .1f),
		"Private Furniture vertices or external/floor anchors were selectable as destinations");
	require(markerPathTargetAtPosition(*world, 0, {1.5f, 0}, .75f) == seat,
		"Closer private vertex masked a nearby actual Marker target");
	require(!markerPathTargetAtPosition(*world, 0, {5, 0}, .1f)
		&& !markerPathTargetAtPosition(*world, 0, {2, 0}, 0), "Empty hit or zero radius selected a target");
	core::World foreign("Foreign", 12, 2);
	foreign.addRoom("Foreign room", 0, 0, 0, 12, 1); foreign.finishBuild();
	require(!isMarkerPathTarget(foreign, *seat), "Another World's Marker was accepted as a target");

	gUISettings = UISettings{};
	gUISettings.renderGraph = true; gUISettings.visibleLayer = 0;
	gUISettings.worldViewportWidth = 800; gUISettings.worldViewportHeight = 400;
	gUISettings.xOffset = gUISettings.yOffset = 0;
	WorldDrawList picker({{0, 0}, {800, 500}});
	renderGraph(world->getGraph(), world, &picker, true);
	std::set<float> targetXs;
	for (auto const& command : picker.commands())
		if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
		{
			auto centre = triangle->positions[0]; targetXs.insert(centre.x);
			require((centre.x == 2 * CORE_CELL_WIDTH_PIXELS || centre.x == 8.5f * CORE_CELL_WIDTH_PIXELS)
				&& centre.y == 398, "Destination overlay included a non-Marker or another Layer's vertex");
		}
	require(targetXs.size() == 2 && lineCount(picker) == 0,
		"Destination picker omitted Markers or rendered non-destination graph edges");
	WorldDrawList inspection({{0, 0}, {800, 500}});
	renderGraph(world->getGraph(), world, &inspection);
	require(lineCount(inspection) > 0 && inspection.commands().size() > picker.commands().size(),
		"Marker filtering leaked into ordinary graph inspection");

	auto agentId = world->createAgent("Visitor", room, 0, 8.5f);
	auto agent = world->lookupAgent(agentId).entity;
	require(world->getGraph()->calculatePath(agent, seat) != nullptr, "Filtered Furniture seat cannot receive an actual Agent Path");
	std::string diagnostic;
	require(world->removeFurniture(id, &diagnostic), diagnostic);
	require(!isMarkerPathTarget(*world, *seat) && !markerPathTargetAtPosition(*world, 0, {2, 0}, .1f),
		"Removed/rebuilt Marker left a stale selectable target");
}

void agentPaths(smoke::Context const&)
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

	struct RenderState
	{
		UISettings settings = gUISettings;
		core::Agent* selection = gSelectedAgent;
		~RenderState() { gSelectedAgent = selection; gUISettings = settings; }
	} state;
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

	headless::ScopedImGuiContext context;
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
	require(hasText(greyReplan, "?"), "Mandatory replan omitted its planning badge");
	world.advanceTicks(agent->getRoutePlanningRemainingTicks());

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
	unreachable->nodes.push_back({ nullptr, source, 0.0f, std::nullopt, std::nullopt });
	unreachable->nodes.push_back({ nullptr, isolated, 0.0f, std::nullopt, std::nullopt });
	agent->setPath(unreachable, false);
	world.replanAgentAfterAuthorizationRefusal(agentId);
	WorldDrawList failedGrey({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &failedGrey);
	require(hasText(failedGrey, "?"), "Failed replan revealed its result before expiry");
	world.advanceTicks(agent->getRoutePlanningRemainingTicks());
	WorldDrawList failedReplan({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &failedReplan);
	require(!hasText(failedReplan, "?")
		&& !hasSolidColour(failedReplan, IM_COL32(200, 48, 48, 255)),
		"A failed replan rendered the obsolete result phase");

	auto const plannerId = world.createAgent("Planner", corridor, 0, 3.25f);
	auto* planner = world.lookupAgent(plannerId).entity;
	require(world.moveAgentToMarker(plannerId, world.getMarkerIds()[1]).accepted(), "Planning refused");
	auto const planningPosition = planner->getGlobalPosition();
	for (uint64_t frame = 0; frame < planner->getRoutePlanningTotalTicks(); ++frame)
	{
		WorldDrawList planning({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
		renderAgent(planner, &planning);
		require(hasText(planning, "?") && !hasText(planning, "!")
			&& hasSolidColour(planning, IM_COL32(96, 96, 96, 255)),
			"Route planning must render only a neutral grey question badge on every frame");
		gUISettings.renderAgentDebug = false;
		WorldDrawList hidden({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
		renderAgent(planner, &hidden);
		require(!hasText(hidden, "?"), "Planning badge ignored Agent Debug gating");
		gUISettings.renderAgentDebug = true;
		world.advanceTick();
		require(planner->getGlobalPosition() == planningPosition, "Badge lifetime fixture moved during planning");
	}
	WorldDrawList expired({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(planner, &expired);
	require(!hasText(expired, "?"), "Planning badge survived expiry");

	// Planning forfeits queues, so the badge stack must collapse immediately
	// from the queue badge to one planning badge at the same body-relative slot.
	planner->pausePathing();
	require(world.requestInteraction(point, plannerId).value != 0 && planner->isInQueue(),
		"Queue-to-planning fixture did not queue");
	WorldDrawList queued({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(planner, &queued);
	world.replanAgentAfterAuthorizationRefusal(plannerId);
	WorldDrawList thinking({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(planner, &thinking);
	require(hasText(queued, "!") && !hasText(thinking, "!") && hasText(thinking, "?")
		&& textPosition(thinking, "?").y == textPosition(queued, "!").y,
		"Queue/planning badge stack retained a stale badge or gap");
	require(world.cancelAgentMovement(plannerId).accepted(), "Planning cancellation refused");
	world.advanceTick();
	require(planner->getState() == core::Agent::State::Idle, "Cancellation did not exit planning");
	WorldDrawList cancelled({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(planner, &cancelled);
	require(!hasText(cancelled, "?"), "Planning badge survived cancellation state exit");

	gUISettings.renderAgentDebug = false;
	WorldDrawList hiddenBadges({ { 0.0f, 0.0f }, { 1280.0f, 720.0f } });
	renderAgent(agent, &hiddenBadges);
	require(!hasText(hiddenBadges, "!") && !hasText(hiddenBadges, "?"),
		"Agent queue or replan badges rendered while Agent debug was disabled");
	gUISettings.renderAgentDebug = true;

	ImGui::EndFrame();

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

}
