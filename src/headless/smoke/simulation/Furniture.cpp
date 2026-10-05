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
	checks.push_back({ "furniture/explicitIdle", [](smoke::Context const& context)
	{
		using smoke::require;
		for (auto const& definition : { "chair", "bed" })
			for (bool explicitIdle : { false, true })
			{
				core::World world("Idle arrival", 12, 2);
				auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
				world.attachFurnitureCatalogue("furniture.furniture.yaml", core::FurnitureCatalogue::readFile(
					context.fixture("resources/test-worlds/furniture.furniture.yaml")));
				require(world.placeFurniture(room, definition, 3, 0, "Target") != 0, "Idle fixture placement refused");
				world.addSectorMarker(room, 0, 8.5f, "Exit");
				world.finishBuild();
				auto target = world.furniture().front().destinations.front().marker;
				auto id = world.createAgent("Walker", room, 0, 0.5f);
				world.consumeSimulationEvents();
				require(world.availableAgentActions(target) == std::vector<std::string>{ "idle" },
					"Legacy Furniture use appeared in available Actions");
				require((explicitIdle ? world.moveAgentToMarker(id, target, core::IdleAction)
					: world.moveAgentToMarker(id, target)).accepted(), "Idle request refused");
				unsigned reached = 0;
				for (unsigned tick = 0; tick < 1800; ++tick)
				{
					world.advanceTick();
					require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing
						&& !world.usablePointOccupant(target), "Idle implicitly used Furniture");
					for (auto const& event : world.consumeSimulationEvents())
						if (event.type == core::SimulationEventType::DestinationReached)
						{
							++reached;
							require(event.destinationMarker == target && event.selectedAction == core::IdleAction
								&& event.agent.globalPosition.x == (std::string(definition) == "bed" ? 4.f : 3.5f),
								"Action completed before physical arrival");
						}
				}
				require(reached == 1 && world.lookupAgent(id).entity->getState() == core::Agent::State::Idle,
					"Idle scheduled replacement work or lost arrival");
				require(world.moveAgentToNamedMarker(id, "Exit").accepted(), "Pass-through request refused");
				world.advanceTicks(1800);
				require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing
					&& !world.usablePointOccupant(target), "Intermediate passage implicitly used Furniture");
			}
	} });
	checks.push_back({ "furniture/actions", actions });
	checks.push_back({ "furniture/bedLyingLifecycle", [](smoke::Context const& context)
	{
		using smoke::require;
		auto source = context.fixture("resources/test-worlds/furniture.furniture.yaml");
		for (int scenario = 0; scenario < 3; ++scenario)
		{
			core::World world("Bed arrival", 16, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 16, 1);
			world.attachFurnitureCatalogue("furniture.furniture.yaml", core::FurnitureCatalogue::readFile(source));
			auto bed = world.placeFurniture(room, "bed", 3, 0, "Bed");
			auto otherBed = world.placeFurniture(room, "bed", 10, 0, "Other bed");
			require(bed && otherBed, "Bed placement failed");
			world.addSectorMarker(room, 0, 8.5f, "Exit");
			world.finishBuild();
			auto middle = world.furniture()[0].destinations[0].marker;
			auto exit = world.getMarkerIds().back();
			auto id = world.createAgent("Sleeper", room, 0, scenario == 1 ? 8.5f : 0.5f);
			auto* agent = world.lookupAgent(id).entity;
			require(world.moveAgentToMarker(id, scenario == 2 ? exit : middle).accepted(), "Bed move refused");
			require(agent->getPose() == core::Pose::Standing, "Lying fired before arrival");
			for (int tick = 0; tick < 1800; ++tick)
			{
				world.advanceTicks(1);
				if (agent->getPose() == core::Pose::Lying)
					require(scenario != 2 && agent->getGlobalPosition().x == 4.f, "Lying fired away from destination");
			}
			if (scenario == 2)
			{
				require(agent->getState() == core::Agent::State::Idle && agent->getGlobalPosition().x == 8.5f
					&& agent->getPose() == core::Pose::Standing && !world.usablePointOccupant(middle),
					"Pass-through triggered Bed action");
				continue;
			}
			require(agent->getPose() == core::Pose::Lying && world.usablePointOccupant(middle) == id
				&& world.getSimulationSnapshot().agents[0].pose == core::Pose::Lying, "Bed arrival did not lie down and claim point");
			auto other = world.createAgent("Other", room, 0, 7.5f);
			world.moveAgentToMarker(other, middle);
			world.advanceTicks(1800);
			require(world.usablePointOccupant(middle) == id
				&& world.lookupAgent(other).entity->getPose() == core::Pose::Standing
				&& !world.lookupAgent(other).entity->getPath(), "Occupied Bed allowed another arrival");
			world.pauseSimulation();
			require(world.editFurniture(otherBed, 11, 0, "Moved"), "Unrelated Bed edit refused");
			agent = world.lookupAgent(id).entity;
			require(agent->getPose() == core::Pose::Lying && world.usablePointOccupant(middle) == id,
				"Structural replay lost unaffected Lying pose or claim");
			std::filesystem::copy_file(source, context.temporaryRoot() / "furniture.furniture.yaml",
				std::filesystem::copy_options::overwrite_existing);
			for (auto suffix : {".world.yaml", ".world"})
			{
				auto file = context.temporaryRoot() / (std::string("bed") + suffix);
				world.saveTo(file.string());
				auto loaded = core::loadWorldDocument(file);
				require(loaded->lookupAgent(id).entity->getPose() == core::Pose::Standing
					&& !loaded->usablePointOccupant(middle), "Bed Pose or occupancy survived reload");
			}
			if (scenario == 0)
			{
				require(world.resumeSimulation(), "Bed resume refused");
				require(world.moveAgentToMarker(id, exit).accepted(), "Bed departure refused");
				world.advanceTicks(1800);
				require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing && !world.usablePointOccupant(middle),
					"Bed departure did not stand and release claim");
			}
			else
			{
				auto position = agent->getGlobalPosition();
				require(world.editFurniture(bed, 4, 0, "Moved bed"), "Occupied Bed move refused");
				agent = world.lookupAgent(id).entity;
				require(agent->getPose() == core::Pose::Standing && agent->getGlobalPosition() == position
					&& !world.usablePointOccupant(middle), "Bed move did not stand in place and release claim");
			}
		}
	} });
	checks.push_back({ "furniture/authoredChairArrival", [](smoke::Context const& context)
	{
		using smoke::require;
		auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/furniture-test-1.world.yaml"));
		auto* agent = world->lookupAgent(core::AgentId{1}).entity;
		require(agent != nullptr, "Chair arrival Agent missing");
		require(world->resumeSimulation(), "Chair arrival simulation resume refused");
		require(world->moveAgentToMarker(core::AgentId{1}, world->furniture()[0].destinations[0].marker).accepted(),
			"Chair destination move refused");
		world->advanceTicks(1800);
		require(agent->getState() == core::Agent::State::Idle, "Agent did not arrive at chair");
		auto const& chair = world->furniture()[0];
		auto const chairX = world->getSector(chair.sector)->getPosition().x + chair.x + 0.5f;
		require(agent->getGlobalPosition().x == chairX, "Agent did not reach chair seat: x=" + std::to_string(agent->getGlobalPosition().x));
		require(agent->getPose() == core::Pose::Sitting, "Agent arrived at chair but Pose is not Sitting");
	} });
	checks.push_back({ "furniture/seatedEdits", [](smoke::Context const& context)
	{
		using smoke::require;
		for (int edit = 0; edit < 3; ++edit)
		{
			core::World world("Seated edits", 12, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
			world.attachFurnitureCatalogue("sit.furniture.yaml", core::FurnitureCatalogue::readFile(
				context.fixture("src/headless/smoke/fixtures/sit.furniture.yaml")));
			auto chair = world.placeFurniture(room, "chair", 3, 0, "Chair");
			world.placeFurniture(room, "chair", 8, 0, "Other");
			world.addSectorMarker(room, 0, 10.5f, "Exit");
			world.finishBuild();
			auto exit = world.getMarkerIds().back();
			auto seat = world.furniture()[0].destinations[0].marker;
			auto otherSeat = world.furniture()[1].destinations[0].marker;
			auto sitter = world.createAgent("Sitter", room, 0, 2.5f);
			auto other = world.createAgent("Unaffected", room, 0, 9.5f);
			require(world.moveAgentToMarker(sitter, seat).accepted()
				&& world.moveAgentToMarker(other, otherSeat).accepted(), "Seat moves refused");
			world.advanceTicks(1800);
			require(world.usablePointOccupant(seat) == sitter && world.usablePointOccupant(otherSeat) == other,
				"Fixture sitters did not claim seats");
			world.pauseSimulation();
			world.setAgentActive(other, false);
			std::string diagnostic;
			require(!world.editFurniture(chair, 8, 0, "Chair", &diagnostic), "Overlapping move accepted");
			require(world.usablePointOccupant(seat) == sitter
				&& world.lookupAgent(sitter).entity->getPose() == core::Pose::Sitting,
				"Rejected edit changed sitter");
			require(world.editFurniture(chair, 3, 0, "Renamed", &diagnostic), "Rename refused");
			require(world.usablePointOccupant(seat) == sitter
				&& world.lookupAgent(sitter).entity->getPose() == core::Pose::Sitting,
				"Rename released unchanged seat");
			auto position = world.lookupAgent(sitter).entity->getGlobalPosition();
			if (edit == 2) require(world.removeFurniture(chair, &diagnostic), "Occupied chair deletion refused");
			else require(world.editFurniture(chair, edit == 0 ? 5.f : 3.f, 0, "Renamed", &diagnostic,
				edit == 0 ? 0 : 1), "Occupied chair move refused");
			require(!world.usablePointOccupant(seat), "Edited seat leaked claim");
			require(world.lookupAgent(sitter).entity->getPose() == core::Pose::Standing
				&& world.lookupAgent(sitter).entity->getGlobalPosition() == position,
				"Edited sitter did not stand at its original physical position");
			require(world.usablePointOccupant(otherSeat) == other
				&& world.lookupAgent(other).entity->getPose() == core::Pose::Sitting
				&& !world.lookupAgent(other).entity->isActive(), "Unaffected sitter lost Pose, claim or activation");
			if (edit == 2)
			{
				require(!world.lookupMarker(seat), "Deleted seat still exists");
				chair = world.placeFurniture(room, "chair", 3, 0, "Replacement");
				for (auto const& instance : world.furniture())
					if (instance.id == chair) seat = instance.destinations[0].marker;
				world.finishBuild();
			}
			require(world.resumeSimulation(), "Resume after seated edit refused");
			require(world.moveAgentToMarker(sitter, exit).accepted(), "Released sitter could not depart");
			world.advanceTicks(1800);
			require(world.moveAgentToMarker(sitter, seat).accepted(), "Replacement/moved seat not routable");
			world.advanceTicks(1800);
			require(world.usablePointOccupant(seat) == sitter
				&& world.lookupAgent(sitter).entity->getPose() == core::Pose::Sitting,
				"Replacement/moved seat not claimable in scenario " + std::to_string(edit));
			require(world.usablePointOccupant(otherSeat) == other, "Unaffected claim lost after resume");
		}
	} });
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
