#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentTagRegistryDocument.h"

namespace
{
	void actions(smoke::Context const& context)
	{
		using smoke::require;
		auto catalogue = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/sit.furniture.yaml"));
		for (int scenario = 0; scenario < 4; ++scenario)
		{
			core::World world("Sit arrival", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("sit.furniture.yaml", catalogue);
			require(world.placeFurniture(room, "chair", 3, 0, "Chair") != 0, "Could not place test chair");
			world.addSectorMarker(room, 0, 6.5f, "Exit");
			world.finishBuild();
			auto seat = world.furniture()[0].destinations[0].marker;
			auto plain = world.furniture()[0].destinations[1].marker;
			auto exit = world.getMarkerIds().back();
			auto destination = scenario < 2 ? seat : scenario == 2 ? plain : exit;
			auto id = world.createAgent("Walker", room, 0, scenario == 1 ? 7.5f : 0.5f);
			auto* agent = world.lookupAgent(id).entity;
			require(world.moveAgentToMarker(id, destination).accepted(), "Arrival move refused");
			require(agent->getPose() == core::Pose::Standing, "Action fired before physical arrival");
			if (scenario == 3)
			{
				std::shared_ptr<const core::Vertex> target;
				for (auto const& vertex : world.getGraph()->getVertices())
					if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getId() == exit)
						target = vertex;
				auto path = world.getGraph()->calculatePath(agent, target);
				require(path != nullptr, "Pass-through fixture has no route");
				bool passesSeat = false;
				for (auto const& node : path->nodes)
					if (auto marker = std::dynamic_pointer_cast<core::Marker>(node.targetVertex->getObject()))
						passesSeat |= marker->getId() == seat;
				require(passesSeat, "Pass-through fixture does not route through the Sit Marker");
			}
			for (int tick = 0; tick < 1800; ++tick)
			{
				world.advanceTicks(1);
				if (scenario >= 2) require(agent->getPose() == core::Pose::Standing, "Intermediate/no-action Marker fired Sit");
				if (agent->getPose() == core::Pose::Sitting)
					require(agent->getGlobalPosition().x == 3.5f, "Sit fired before reaching the seat");
			}
			require(agent->getState() == core::Agent::State::Idle, "Agent did not finish arrival");
			auto expected = scenario < 2 ? core::Pose::Sitting : core::Pose::Standing;
			require(agent->getPose() == expected, "Destination action produced wrong Pose in scenario " + std::to_string(scenario));
			require(world.getSimulationSnapshot().agents[0].pose == expected, "Arrival Pose not visible in snapshot");
			require(agent->getGlobalPosition().x == (scenario < 2 ? 3.5f : scenario == 2 ? 3.75f : 6.5f), "Wrong arrival position");
			if (scenario < 2)
			{
				require(world.moveAgentToMarker(id, exit).accepted(), "Seated Agent could not depart");
				world.advanceTicks(1800);
				require(agent->getPose() == core::Pose::Standing, "Departing Agent did not stand");
			}
		}
	}
}

void registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/actions", actions });
	checks.push_back({ "furniture/occupancyLifecycle", [](smoke::Context const& context)
	{
		using smoke::require;
		for (int release = 0; release < 3; ++release)
		{
			core::World world("Seat lifecycle", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("sit.furniture.yaml", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/sit.furniture.yaml")));
			world.placeFurniture(room, "chair", 3, 0, "Chair");
			world.addSectorMarker(room, 0, 6.5f, "Exit");
			world.finishBuild();
			auto seat = world.furniture()[0].destinations[0].marker;
			auto exit = world.getMarkerIds().back();
			auto id = world.createAgent("Sitter", room, 0, 0.5f);
			require(world.moveAgentToMarker(id, seat).accepted(), "Seat move refused");
			world.advanceTicks(1800);
			require(world.usablePointOccupant(seat) == id && world.lookupAgent(id).entity->getPose() == core::Pose::Sitting,
				"Sitting did not claim usable point");
			if (release == 0)
			{
				std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/sit.furniture.yaml"),
					context.temporaryRoot() / "sit.furniture.yaml", std::filesystem::copy_options::overwrite_existing);
				for (auto suffix : { ".world.yaml", ".world" })
				{
					auto file = context.temporaryRoot() / (std::string("occupied") + suffix);
					world.saveTo(file.string());
					auto loaded = core::loadWorldDocument(file);
					require(!loaded->usablePointOccupant(seat), "Occupancy survived save/load");
					require(world.usablePointOccupant(seat) == id, "Saving released live claim");
				}
			}
			world.setAgentActive(id, false);
			world.advanceTicks(100);
			require(world.usablePointOccupant(seat) == id, "Deactivation released seat");
			world.pauseSimulation();
			require(world.usablePointOccupant(seat) == id, "Pause released seat");
			require(world.resumeSimulation(), "Resume refused");
			require(world.usablePointOccupant(seat) == id, "Resume released seat");
			world.setAgentActive(id, true);
			if (release == 0)
			{
				require(world.moveAgentToMarker(id, exit).accepted(), "Departure refused");
				for (int tick = 0; tick < 600 && world.usablePointOccupant(seat); ++tick) world.advanceTicks(1);
				require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing, "Departure did not stand");
			}
			else if (release == 1) world.resetSimulation();
			else require(world.removeAgent(id).removed, "Sitter deletion refused");
			require(!world.usablePointOccupant(seat), "Lifecycle trigger did not release claim");
		}
	} });
}
