#include "WorldChecks.h"
#include "AgentPoseTestAccess.h"
#include "core/Graph.h"
#include "core/AgentTagRegistryDocument.h"

namespace persistence
{
	void agentPoses(smoke::Context const& context)
	{
		using smoke::require;
		core::World world("Pose lifetime", 8, 2);
		world.addRoom("Room", 0, 0, 0, 8, 1);
		world.addSectorMarker(0, 0, 6.5f, "Destination");
		world.finishBuild();
		auto standing = world.createAgent("Standing", 0, 0, 1.5f);
		auto sitting = world.createAgent("Sitting", 0, 0, 2.5f);
		auto lying = world.createAgent("Lying", 0, 0, 3.5f);
		auto crouching = world.createAgent("Crouching", 0, 0, 4.5f);
		auto crawling = world.createAgent("Crawling", 0, 0, 5.5f);
		for (auto id : {standing, sitting, lying, crouching, crawling})
			require(world.lookupAgent(id).entity->getPose() == core::Pose::Standing, "New Agent must be Standing");
		world.markSaved();
		core::AgentPoseTestAccess::set(*world.lookupAgent(sitting).entity, core::Pose::Sitting);
		core::AgentPoseTestAccess::set(*world.lookupAgent(lying).entity, core::Pose::Lying);
		core::AgentPoseTestAccess::set(*world.lookupAgent(crouching).entity, core::Pose::Crouching);
		core::AgentPoseTestAccess::set(*world.lookupAgent(crawling).entity, core::Pose::Crawling);
		require(!world.isModified(), "Runtime pose must not dirty authored state");
		for (auto suffix : {".world.yaml", ".world"})
		{
			auto file = context.temporaryRoot() / (std::string("poses") + suffix);
			world.saveTo(file.string());
			auto loaded = core::loadWorldDocument(file);
			for (auto const& snapshot : loaded->getSimulationSnapshot().agents)
				require(snapshot.pose == core::Pose::Standing, "Saved runtime pose survived reload");
			require(world.lookupAgent(sitting).entity->getPose() == core::Pose::Sitting
				&& world.lookupAgent(lying).entity->getPose() == core::Pose::Lying
				&& world.lookupAgent(crouching).entity->getPose() == core::Pose::Crouching
				&& world.lookupAgent(crawling).entity->getPose() == core::Pose::Crawling, "Saving changed live Pose");
		}
		world.resetSimulation();
		for (auto const& snapshot : world.getSimulationSnapshot().agents)
			require(snapshot.pose == core::Pose::Standing, "Reset did not restore Standing");
		for (auto pose : {core::Pose::Sitting, core::Pose::Lying, core::Pose::Crouching, core::Pose::Crawling})
		{
			auto* agent = world.lookupAgent(sitting).entity;
			core::AgentPoseTestAccess::set(*agent, pose);
			auto path = world.getGraph()->calculatePath(agent, world.getGraph()->getVertices().back());
			require(path && !path->nodes.empty(), "Pose path fixture has no route");
			agent->setPath(path, false);
			require(agent->getPose() == pose, "Inactive Path must not change Pose");
			agent->startPathing();
			require(agent->getPose() == core::Pose::Standing, "Beginning a Path did not restore Standing");
			core::AgentPoseTestAccess::set(*agent, pose);
			agent->setPath(path, true);
			require(agent->getPose() == core::Pose::Standing, "Immediately started Path did not restore Standing");
		}
	}
}
