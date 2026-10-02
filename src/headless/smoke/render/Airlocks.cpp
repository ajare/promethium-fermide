#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/AirlockTransit.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Agent.h"
#include <set>

extern UISettings gUISettings;
extern std::shared_ptr<const core::Sector> gSelectedSector;

namespace
{
	void chamber(smoke::Context const&)
	{
		using smoke::require;
		ImGui::GetIO().DisplaySize = { 1600, 720 };
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.visibleLayer = 0; gUISettings.worldViewportWidth = 1600;
		gUISettings.worldViewportHeight = 720; gUISettings.worldZoom = 1;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		auto world = std::make_shared<core::World>("Airlock rendering", 10, 2);
		world->addRoom("Left", 0, 0, 0, 2, 1); world->addCorridor(0, 0, 5, 2, 1);
		auto index = world->addAirlock(0, 0, 2, 3); world->finishBuild();
		WorldDrawList drawing({ { 0, 0 }, { 1600, 720 } });
		renderWorld(world, &drawing);
		std::set<float> doors, buttons;
		bool chamberSurface = false;
		for (auto const& command : drawing.commands())
			if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command))
			{
				float lowX = triangle->positions[0].x, highX = lowX;
				for (auto const& point : triangle->positions) { lowX = std::min(lowX, point.x); highX = std::max(highX, point.x); }
				if (triangle->colour == ImU32(ImColor(128, 192, 182))) doors.insert((lowX + highX) * 0.5f);
				if (triangle->colour == ImU32(ImColor(0, 255, 128))) buttons.insert((lowX + highX) * 0.5f);
				if (lowX == 2 * CORE_CELL_WIDTH_PIXELS && highX == 5 * CORE_CELL_WIDTH_PIXELS) chamberSurface = true;
			}
		require(chamberSurface && doors.size() == 2 && buttons.size() == 3, "Renderer output missing chamber, two Bulkheads or three buttons");
		// Width-one chamber still renders all generated controls separately.
		world = std::make_shared<core::World>("Single cell chamber", 8, 2);
		world->addCorridor(0, 0, 0, 2, 1); world->addRoom("Right", 0, 0, 3, 2, 1);
		index = world->addAirlock(0, 0, 2, 1); world->finishBuild();
		WorldDrawList selected({ { 0, 0 }, { 1600, 720 } });
		gSelectedSector = world->getSector(index); gUISettings.selectionMode = UISettings::SelectionMode::Sector;
		renderWorld(world, &selected);
		require(std::any_of(selected.commands().begin(), selected.commands().end(), [](auto const& command) {
			auto line = std::get_if<WorldDrawList::Line>(&command);
			return line && line->colour == ImU32(ImColor(255, 255, 0)) && line->thickness == 3;
		}), "Selected Airlock missing highlight");
		gSelectedSector.reset();
		// Observe an occupied closed-door cycle through production movement and
		// inspect the normal renderer's command stream, not a test-only state hook.
		world->pauseSimulation();
		auto marker = world->addSectorMarker(1, 0, 1.5f);
		world->finishBuild(); require(world->resumeSimulation(), "Render journey resume refused");
		auto id = world->createAgent("Visible occupant", 0, 0, 0.5f);
		auto agent = world->lookupAgent(id).entity;
		auto path = world->getGraph()->calculatePath(agent, world->getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
		require(bool(path), "Render journey route unavailable"); agent->setPath(path, true);
		bool cycling = false;
		for (uint32_t tick = 0; tick < 1200; ++tick)
		{
			world->advanceTick();
			if (!world->getSimulationSnapshot().airlocks[0].cycleComplete
				&& !world->getSimulationSnapshot().airlocks[0].occupants.empty()) { cycling = true; break; }
		}
		require(cycling, "Renderer fixture never boarded/cycled");
		WorldDrawList occupied({ { 0, 0 }, { 1600, 720 } }); renderWorld(world, &occupied);
		require(std::any_of(occupied.commands().begin(), occupied.commands().end(), [](auto const& command) {
			auto text = std::get_if<WorldDrawList::Text>(&command);
			return text && text->value == "3.0 s";
		}), "Renderer missing simulated seconds countdown");
		auto colour = agentRenderColour(*agent, false);
		require(std::any_of(occupied.commands().begin(), occupied.commands().end(), [&](auto const& command) {
			auto triangle = std::get_if<WorldDrawList::Triangle>(&command);
			auto text = std::get_if<WorldDrawList::Text>(&command);
			return (triangle && triangle->colour == colour) || (text && text->colour == colour);
		}), "Renderer missing chamber occupant");
		ImGui::EndFrame();
	}
}

void render_smoke::registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/chamberAndControls", isolated<chamber> });
}
