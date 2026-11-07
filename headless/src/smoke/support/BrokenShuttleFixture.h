#pragma once

#include "Smoke.h"
#include "core/Agent.h"
#include "core/World.h"
#include "core/ShuttleTransit.h"
#include "core/DoorSectorObject.h"
#include "core/Path.h"
#include "core/RouteTraversalInputs.h"

namespace broken_shuttle
{
	struct Scene
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Broken Shuttle", 30, 2);
		uint32_t bottom, secondOrigin, top, secondDestination, remote, owner;
		core::World::CreateShuttleResult made;
		std::shared_ptr<core::Shuttle> shuttle;
		std::shared_ptr<const core::Vertex> goal, secondGoal, remoteGoal, bottomGoal;
		core::AgentId id;
		core::Agent* agent;

		Scene(bool broken = false, bool alternate = false)
		{
			world->addLayer();
			bottom = world->addRoom("Origin A", 1, 0, 0, 4, 1);
			secondOrigin = world->addRoom("Origin B", 1, 0, 5, 4, 1);
			top = world->addRoom("Destination A", 1, 0, 20, 4, 1);
			secondDestination = world->addRoom("Destination B", 1, 0, 25, 4, 1);
			remote = alternate ? world->addRoom("Alternative corridor", 0, 0, 0, 30, 1)
				: world->addRoom("Remote", 0, 0, 0, 4, 1);
			world->addSectorDoor(0, 0, 1);
			if (alternate)
			{
				world->addSectorDoor(0, 0, 6);
				world->addSectorDoor(0, 0, 21);
			}
			core::World::CreateShuttleOptions options{ 2, 4, { 0, 20 }, 0 };
			options.capacity = 1; options.doorMask = 0b0101;
			options.minimumDwellSeconds = 0.1f; options.maximumBoardingSeconds = 4;
			options.initiallyBroken = broken;
			made = world->addShuttle(2, 0, 0, 29, options);
			owner = made.shuttle.sector->getIndex();
			shuttle = std::static_pointer_cast<const core::ShuttleTransit>(made.shuttle.sector)->getShuttle();
			auto marker = [&](uint32_t sector, float x)
			{ auto result = world->addSectorMarker(sector, 0, x); return result.sector->getObject(result.index); };
			auto target = marker(top, 1.5f), second = marker(secondDestination, 1.5f);
			auto distant = marker(remote, 1.5f), lower = marker(bottom, 1.5f);
			world->finishBuild();
			goal = world->getGraph()->getVertexForObject(target);
			secondGoal = world->getGraph()->getVertexForObject(second);
			remoteGoal = world->getGraph()->getVertexForObject(distant);
			bottomGoal = world->getGraph()->getVertexForObject(lower);
			id = world->createAgent("Passenger A", bottom, 0, 1.5f); agent = world->lookupAgent(id).entity;
			world->pauseSimulation();
			world->setAgentIndividualMinimumRoutePlanningTime(id, 0.1f);
			world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f);
			auto policy = world->getRouteChoicePolicy(); policy.shuttleHeadwaySeconds = 1;
			policy.manualDoorInteraction = 50;
			world->setRouteChoicePolicy(policy);
			world->resumeSimulation();
		}
		std::shared_ptr<core::Door> door(uint32_t slot) const
		{ auto const& result = made.doors.at(slot); return std::static_pointer_cast<const core::DoorSectorObject>(result.door.sector->getObject(result.door.index))->getDoor(); }
		core::TraversalResourceSnapshot snapshot() const
		{
			for (auto const& resource : world->getSimulationSnapshotView().traversalResources)
				if (resource.id == made.traversalResource) return resource;
			throw std::runtime_error("Missing Shuttle snapshot");
		}
		void start(core::Agent* passenger = nullptr, std::shared_ptr<const core::Vertex> target = {})
		{
			if (!passenger) passenger = agent;
			auto path = world->getGraph()->calculatePath(passenger, target ? target : goal);
			smoke::require(bool(path), "Missing initial Shuttle Path"); passenger->setPath(path, true);
		}
		template<class Predicate> void until(Predicate predicate)
		{
			for (int tick = 0; tick < 6000; ++tick)
			{
				if (predicate()) return;
				world->advanceTick();
			}
			smoke::require(predicate(), "Shuttle scenario timed out");
		}
		void place(std::shared_ptr<const core::Vertex> target)
		{
			start(agent, target); until([&] { return agent->getState() == core::Agent::State::Idle; });
			smoke::require(agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f, "Placement failed");
		}
		bool uses(std::shared_ptr<const core::Path> const& path) const
		{
			return path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node)
			{ return node.edge && node.edge->getType() == core::EdgeType::Shuttle && node.edge->getTraversalResourceId() == made.traversalResource; });
		}
		core::DirectedTraversalFacts facts(core::Edge const& edge, std::shared_ptr<const core::Vertex> target, bool captured) const
		{
			core::RouteDecisionContext context{ agent, world->getRouteChoicePolicy().baselineProfile,
				world->getRouteChoicePolicy(), agent->getSector(), agent->getWalkSpeed(), world.get(),
				agent->getClimbSpeed(), true, 0, 0, agent->getEffectiveMobilityProfile().value };
			return captured ? core::RouteTraversalInputs::capture(edge, target, context).evaluate(context)
				: edge.getDirectedTraversalFacts(target, context);
		}
	};
}
