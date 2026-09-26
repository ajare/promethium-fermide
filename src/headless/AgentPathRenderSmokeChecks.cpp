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
}

void runAgentPathRenderSmokeChecks()
{
	core::World world("Agent Path Render", 16, 1);
	auto const corridor = world.addCorridor(0, 0, 15);
	uint32_t sourceIdentifier = 0;
	uint32_t destinationIdentifier = 0;
	world.addSectorMarker(corridor, 0, 1.5f, &sourceIdentifier);
	world.addSectorMarker(corridor, 0, 13.5f, &destinationIdentifier);
	world.finishBuild();

	auto const graph = world.getGraph();
	auto const source = graph->getVertexByIdentifier(sourceIdentifier);
	auto const destination = graph->getVertexByIdentifier(destinationIdentifier);
	auto* agent = world.lookupAgent(
		world.createAgent("Selected", corridor, 0, 1.5f)).entity;
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
