#include "Checks.h"
#include "Render.h"
#include "WorldDrawList.h"
#include "UISettings.h"
#include "core/AirlockTransit.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Agent.h"
#include <set>
#include <cmath>

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
		// The normal Agent renderer draws each packed chamber occupant at its
		// observed cell, without a separate Airlock-only rendering path.
		world = std::make_shared<core::World>("Packed Airlock rendering", 18, 2);
		world->addRoom("Left", 0, 0, 0, 6, 1); world->addRoom("Right", 0, 0, 9, 6, 1);
		index = world->addAirlock(0, 0, 6, 3, 3);
		marker = world->addSectorMarker(1, 0, 3.0f); world->finishBuild();
		world->pauseSimulation();
		std::vector<core::AgentId> passengers;
		for (uint32_t member = 0; member < 3; ++member)
		{
			auto passenger = world->createAgent("Packed occupant", 0, 0, 5.5f);
			auto entity = world->lookupAgent(passenger).entity;
			require(world->setAgentIndividualColour(passenger, core::AgentColour{ uint8_t(60 + member * 50), 80, 120 }), "Packed occupant colour refused");
			auto route = world->getGraph()->calculatePath(entity, world->getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
			require(bool(route), "Packed renderer route unavailable"); entity->setPath(route, true);
			passengers.push_back(passenger);
		}
		require(world->resumeSimulation(), "Packed render resume refused");
		bool packed = false;
		for (uint32_t tick = 0; tick < 3000; ++tick)
		{
			world->advanceTick();
			auto const& snapshot = world->getSimulationSnapshot();
			if (snapshot.airlocks[0].occupants.size() != 3) continue;
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
			for (auto const& resource : snapshot.traversalResources)
				if (resource.id == chamber->getTraversalResourceId())
				{
					packed = std::all_of(resource.capacityPositions.begin(), resource.capacityPositions.end(), [&](auto const& position) {
						return position.occupant && world->lookupAgent(position.occupant).entity->getLocalPosition().distanceTo(position.position) < 0.001f;
					});
				}
			if (packed) break;
		}
		require(packed, "Renderer fixture did not pack multi-Agent batch");
		WorldDrawList batchDrawing({ { 0, 0 }, { 1600, 720 } }); renderWorld(world, &batchDrawing);
		for (auto passenger : passengers)
		{
			auto entity = world->lookupAgent(passenger).entity;
			auto passengerColour = agentRenderColour(*entity, false);
			bool visible = false;
			for (auto const& command : batchDrawing.commands())
			{
				if (auto triangle = std::get_if<WorldDrawList::Triangle>(&command); triangle && triangle->colour == passengerColour)
				{
					auto centre = (triangle->positions[0].x + triangle->positions[1].x + triangle->positions[2].x) / 3;
					visible = visible || std::abs(centre - entity->getGlobalPosition().x * CORE_CELL_WIDTH_PIXELS) < CORE_CELL_WIDTH_PIXELS / 2;
				}
				if (auto text = std::get_if<WorldDrawList::Text>(&command); text && text->colour == passengerColour)
					visible = visible || std::abs(text->position.x - entity->getGlobalPosition().x * CORE_CELL_WIDTH_PIXELS) < CORE_CELL_WIDTH_PIXELS / 2;
			}
			require(visible, "Renderer missing packed occupant at observed position");
		}
		ImGui::EndFrame();
	}
}

void render_smoke::registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/chamberAndControls", isolated<chamber> });
}
