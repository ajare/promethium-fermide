#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"

namespace routing_smoke
{
	void registerSeatOccupancy(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "furniture/seatRouting", [](smoke::Context const& context)
		{
			using smoke::require;
			core::World world("Seat routing", 12, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
			world.attachFurnitureCatalogue("sit.furniture.lua", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/sit.furniture.lua")));
			world.placeFurniture(room, "chair", 6, 0, "Chair");
			world.addSectorMarker(room, 0, 10.5f, "Exit");
			world.finishBuild();
			auto seat = world.furniture()[0].destinations[0].marker;
			auto exit = world.getMarkerIds().back();
			auto loser = world.createAgent("Loser", room, 0, 0.5f);
			auto winner = world.createAgent("Winner", room, 0, 6.25f);
			auto* other = world.lookupAgent(loser).entity;
			world.pauseSimulation();
			require(world.setAgentIndividualRoutePersistence(loser, 1.f), "Could not set Route persistence");
			require(world.resumeSimulation(), "Could not resume");
			std::shared_ptr<const core::Vertex> seatVertex, exitVertex;
			for (auto const& vertex : world.getGraph()->getVertices())
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()))
				{
					if (marker->getId() == seat) seatVertex = vertex;
					if (marker->getId() == exit) exitVertex = vertex;
				}
			auto path = world.getGraph()->calculatePath(other, seatVertex);
			require(path != nullptr, "Free seat was excluded");
			require(world.moveAgentToMarker(loser, seat, core::UseFurnitureAction).accepted()
				&& world.moveAgentToMarker(winner, seat, core::UseFurnitureAction).accepted(), "Seat requests refused");
			for (int tick = 0; tick < 600 && !world.usablePointOccupant(seat); ++tick) world.advanceTicks(1);
			require(world.usablePointOccupant(seat) == winner, "First arrival did not claim seat");
			require(other->getState() == core::Agent::State::RoutePlanning, "Competing Path not hard-invalidated");
			require(!world.getGraph()->calculatePath(other, seatVertex), "Occupied destination admitted");
			world.advanceTicks(600);
			bool lost = false;
			for (auto const& event : world.consumeSimulationEvents())
				lost |= event.type == core::SimulationEventType::RouteLost && event.destinationMarker == seat;
			require(lost && other->getGlobalPosition().x < 6.5f, "Competing Agent did not replan into Route loss");
			auto through = world.getGraph()->calculatePath(other, exitVertex);
			require(through != nullptr, "Occupied waypoint blocked route");
			bool passesSeat = false;
			for (auto const& node : through->nodes) passesSeat |= node.targetVertex == seatVertex;
			require(passesSeat, "Fixture did not exercise occupied waypoint");
			other->setPath(through, true);
			world.advanceTicks(1800);
			require(other->getGlobalPosition().x == 10.5f && world.usablePointOccupant(seat) == winner,
				"Through-route disturbed claim");
			require(world.moveAgentToMarker(loser, seat).accepted(), "Occupied intent should be accepted for ordinary Route loss");
			world.advanceTicks(600);
			lost = false;
			for (auto const& event : world.consumeSimulationEvents())
				lost |= event.type == core::SimulationEventType::RouteLost && event.destinationMarker == seat;
			require(lost, "New occupied destination did not produce Route loss");
			auto coincident = world.createAgent("Already at seat", room, 0, 6.5f);
			require(world.moveAgentToMarker(coincident, seat).accepted(), "Coincident intent refused");
			world.advanceTicks(600);
			lost = false;
			for (auto const& event : world.consumeSimulationEvents())
				lost |= event.type == core::SimulationEventType::RouteLost && event.destinationMarker == seat;
			require(lost && world.usablePointOccupant(seat) == winner,
				"Coincident Agent bypassed occupied destination exclusion");
		} });
	}
}
