#pragma once

#include "Smoke.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/LiftSectorObject.h"
#include "core/Path.h"
#include "core/RouteTraversalInputs.h"

namespace broken_platform_lift
{
	struct Scene
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Broken Platform lift", 14, 4);
		uint32_t bottom, top, remote, owner;
		core::World::CreatePlatformLiftResult made, alternative;
		std::shared_ptr<core::Lift> lift;
		std::shared_ptr<const core::Vertex> goal, remoteGoal, bottomGoal;
		core::AgentId id;
		core::Agent* agent;

		Scene(bool broken = false, bool alternate = false)
		{
			bottom = top = world->addRoom("Platform room", 0, 0, 0, 14, 4);
			remote = world->addRoom("Remote", 1, 0, 0, 2, 1);
			world->addSectorDoor(0, 0, 0);
			for (uint32_t level : { 1u, 3u })
				for (uint32_t x = 0; x < 14; ++x) world->addSectorWalkway(bottom, level, x);
			core::World::CreateLiftOptions options;
			options.cellsWide = 1; options.stopOffsets = { 0, 3 }; options.capacity = 2;
			options.platformStopDurationSeconds = 4; options.initiallyBroken = broken;
			made = world->addSectorPlatformLift(bottom, 0, 4, options);
			owner = bottom;
			lift = std::static_pointer_cast<const core::LiftSectorObject>(made.lift.sector->getObject(made.lift.index))->getLift();
			if (alternate) { options.initiallyBroken = false; alternative = world->addSectorPlatformLift(bottom, 0, 10, options); }
			auto marker = [&](uint32_t sector, uint32_t level, float x)
			{ auto result = world->addSectorMarker(sector, level, x); return result.sector->getObject(result.index); };
			auto target = marker(top, 3, 6.5f), distant = marker(remote, 0, 0.5f), lower = marker(bottom, 0, 0.5f);
			world->finishBuild();
			goal = world->getGraph()->getVertexForObject(target);
			remoteGoal = world->getGraph()->getVertexForObject(distant);
			bottomGoal = world->getGraph()->getVertexForObject(lower);
			id = world->createAgent("Passenger", bottom, 0, 0.5f); agent = world->lookupAgent(id).entity;
			world->pauseSimulation();
			world->setAgentIndividualMinimumRoutePlanningTime(id, 0.1f);
			world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f);
			world->resumeSimulation();
		}
		core::TraversalResourceSnapshot snapshot() const
		{
			for (auto const& resource : world->getSimulationSnapshotView().traversalResources)
				if (resource.id == made.traversalResource) return resource;
			throw std::runtime_error("Missing Platform lift snapshot");
		}
		void start(core::Agent* passenger = nullptr)
		{
			if (!passenger) passenger = agent;
			auto path = world->getGraph()->calculatePath(passenger, goal);
			smoke::require(bool(path), "Missing initial Platform lift Path"); passenger->setPath(path, true);
		}
		template<class Predicate> void until(Predicate predicate)
		{
			for (int tick = 0; tick < 3600; ++tick)
			{
				if (predicate()) return;
				world->advanceTick();
			}
			smoke::require(predicate(), "Platform lift scenario timed out");
		}
		void place(std::shared_ptr<const core::Vertex> target)
		{
			auto path = world->getGraph()->calculatePath(agent, target);
			smoke::require(bool(path), "Missing placement Path"); agent->setPath(path, true);
			until([&] { return agent->getState() == core::Agent::State::Idle; });
			smoke::require(agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f, "Placement failed");
		}
		bool uses(std::shared_ptr<const core::Path> const& path, core::TraversalResourceId resource) const
		{
			return path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Lift && node.edge->getTraversalResourceId() == resource; });
		}
		core::DirectedTraversalFacts facts(core::Edge const& edge, std::shared_ptr<const core::Vertex> target, bool captured = true) const
		{
			core::RouteDecisionContext context{ agent, world->getRouteChoicePolicy().baselineProfile,
				world->getRouteChoicePolicy(), agent->getSector(), agent->getWalkSpeed(), world.get(),
				agent->getClimbSpeed(), true, 0, 0, agent->getEffectiveMobilityProfile().value };
			return captured ? core::RouteTraversalInputs::capture(edge, target, context).evaluate(context)
				: edge.getDirectedTraversalFacts(target, context);
		}
	};
}
