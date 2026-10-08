#include "Checks.h"
#include "core/Agent.h"
#include "core/Location.h"
#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"
#include <cmath>
#include <string>

// One-cell-high Rooms may override the standard height with a scale in
// [0.2, 1.0]. Pathing must respect the effective ceiling: an Agent entering a
// low Room Crouches (60% height) or Crawls (30% height) as appropriate, and
// stands back up on exit when the destination allows.
namespace
{
	using smoke::require;

	struct Scene
	{
		core::World world{"Low Room poses", 8, 2};
		uint32_t corridor;
		uint32_t low;
		core::AgentId traveller;

		Scene(float scale)
		{
			corridor = world.addCorridor(0u, 0u, 0u, 2u, 1u);
			low = world.addRoom("Low", 0, 0, 2, 2, 1);
			world.addSectorMarker(corridor, 0, 1.0f, "Entry");
			world.addSectorMarker(low, 0, 1.0f, "Low goal");
			world.finishBuild();
			world.pauseSimulation();
			require(world.setRoomHeightScale(low, scale), "Low Room height scale refused");
			world.removeLocationWall(low, 0, CORE_SIDE_LEFT);
			world.finishBuild();
			world.resumeSimulation();
			traveller = world.createAgent("Traveller", corridor, 0, 0.5f);
		}

		core::Agent& agent() { return *world.lookupAgent(traveller).entity; }

		void runTo(std::string const& marker)
		{
			require(world.moveAgentToNamedMarker(traveller, marker).accepted(),
				"movement to " + marker + " refused");
			for (unsigned tick = 0; tick != 20000
				&& agent().getState() != core::Agent::State::Idle; ++tick)
				world.advanceTick();
		}
	};

	void lowRoomCrouchesAndStands(smoke::Context const&)
	{
		Scene scene(0.4f);
		require(scene.agent().getPose() == core::Pose::Standing,
			"an Agent in a Corridor did not start Standing");
		scene.runTo("Low goal");
		require(scene.agent().getState() == core::Agent::State::Idle
			&& scene.agent().getPose() == core::Pose::Crouching,
			"a low Room that only clears the Crouch envelope did not crouch its occupant");
		scene.runTo("Entry");
		require(scene.agent().getState() == core::Agent::State::Idle
			&& scene.agent().getPose() == core::Pose::Standing,
			"an Agent did not stand back up on leaving the low Room");
	}

	void lowRoomCrawlsAndStands(smoke::Context const&)
	{
		Scene scene(0.2f);
		require(scene.agent().getPose() == core::Pose::Standing,
			"an Agent in a Corridor did not start Standing");
		scene.runTo("Low goal");
		require(scene.agent().getState() == core::Agent::State::Idle
			&& scene.agent().getPose() == core::Pose::Crawling,
			"a very low Room that only clears the Crawl envelope did not crawl its occupant");
		scene.runTo("Entry");
		require(scene.agent().getState() == core::Agent::State::Idle
			&& scene.agent().getPose() == core::Pose::Standing,
			"a crawling Agent did not stand back up on leaving the low Room");
	}

	void lowRoomPlacementDucks(smoke::Context const&)
	{
		core::World world("Low Room placement", 8, 2);
		auto low = world.addRoom("Low", 0, 0, 0, 4, 1);
		world.finishBuild();
		world.pauseSimulation();
		require(world.setRoomHeightScale(low, 0.2f), "Low Room height scale refused");
		world.resumeSimulation();
		auto id = world.createAgent("Occupant", low, 0, 0.5f);
		require(world.lookupAgent(id).entity->getPose() == core::Pose::Crawling,
			"an Agent placed directly into a low Room did not adopt a lowered pose");
	}

	// A crawling Agent leaving a low Room through a Door stands back up as soon
	// as it commits into the normal Room behind the Door.
	void lowRoomDoorExitStands(smoke::Context const&)
	{
		core::World world("Low Room door exit", 8, 2);
		auto low = world.addRoom("Low", 0, 0, 0, 4, 1);
		auto normal = world.addRoom("Normal", 1, 0, 0, 4, 1);
		core::World::CreateDoorOptions options;
		world.addSectorDoor(0, 0, 2, options);
		world.addSectorMarker(normal, 0, 2.5f, "Normal goal");
		world.finishBuild();
		world.pauseSimulation();
		require(world.setRoomHeightScale(low, 0.2f), "Low Room height scale refused");
		world.resumeSimulation();
		auto id = world.createAgent("Traveller", low, 0, 0.5f);
		auto* agent = world.lookupAgent(id).entity;
		require(agent->getPose() == core::Pose::Crawling,
			"the Agent did not start Crawling in the low Room");
		require(world.moveAgentToNamedMarker(id, "Normal goal").accepted(), "Door exit movement refused");
		for (unsigned tick = 0; tick != 20000
			&& agent->getState() != core::Agent::State::Idle; ++tick)
			world.advanceTick();
		require(agent->getState() == core::Agent::State::Idle
			&& agent->getSector() == world.getSector(normal).get()
			&& agent->getPose() == core::Pose::Standing,
			"a crawling Agent did not stand up on entering the normal Room through the Door");
	}

	// A World restored from a saved active Path re-derives the Pose from the
	// Sector the Agent's body is physically in, not only the logical Sector its
	// traversal still belongs to. The fixture's Room 2 overrides its height to
	// below the Crouch envelope, so the Agent must Crawl while physically inside
	// Room 2 and stand as soon as it crosses into normal Room 3 - before the
	// cross-sector Location edge commits at Room 3's Marker.
	void loadedRoomHeights(smoke::Context const& context)
	{
		auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/room-height-test-1.world.yaml"));
		require(world != nullptr, "fixture world failed to load");
		auto* agent = world->lookupAgent(core::AgentId{1}).entity;
		require(agent, "fixture agent missing");
		bool physicallyInRoom2 = false;
		bool physicallyInRoom3 = false;
		bool wrongPoseInRoom2 = false;
		bool wrongPoseInRoom3 = false;
		for (unsigned tick = 0; tick != 4000; ++tick)
		{
			if (auto const* sector = agent->getSector())
			{
				auto const global = agent->getGlobalPosition();
				auto const physical = world->getSectorAtPosition(sector->getLayerIndex(), global.x, global.y);
				if (physical && physical->getName() == "Room 2")
				{
					physicallyInRoom2 = true;
					if (agent->getPose() != core::Pose::Crawling) wrongPoseInRoom2 = true;
				}
				else if (physical && physical->getName() == "Room 3")
				{
					physicallyInRoom3 = true;
					if (agent->getPose() != core::Pose::Standing) wrongPoseInRoom3 = true;
				}
			}
			world->advanceTick();
		}
		require(physicallyInRoom2, "the Agent never physically entered Room 2");
		require(!wrongPoseInRoom2, "the Agent was not Crawling while physically in Room 2");
		require(physicallyInRoom3, "the Agent never physically entered Room 3");
		require(!wrongPoseInRoom3, "the Agent was not Standing while physically in Room 3");
		require(agent->getSector() && agent->getSector()->getName() == "Room 3"
			&& agent->getPose() == core::Pose::Standing,
			"the Agent did not finish Standing in Room 3");
	}
}

void registerRoomHeights(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "roomHeights/crouchAndStand", lowRoomCrouchesAndStands });
	checks.push_back({ "roomHeights/crawlAndStand", lowRoomCrawlsAndStands });
	checks.push_back({ "roomHeights/placementDucks", lowRoomPlacementDucks });
	checks.push_back({ "roomHeights/doorExitStands", lowRoomDoorExitStands });
	checks.push_back({ "roomHeights/loadedWorld", loadedRoomHeights });
}
