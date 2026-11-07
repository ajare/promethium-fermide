#pragma once
#include "core/Agent.h"
#include "core/World.h"
#include "core/StaircaseEdge.h"
#include "core/StaircaseTransit.h"
#include "core/RouteTraversalInputs.h"
#include "Smoke.h"
#include "core/Path.h"

namespace broken_escalator
{
	struct Scene
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Broken Escalator", 12, 2);
		uint32_t bottom, top, remote, owner;
		core::AgentId id;
		core::Agent* agent;
		std::shared_ptr<core::Staircase> stairs;
		std::shared_ptr<const core::Edge> edge;
		std::shared_ptr<const core::Vertex> low, high;
		std::map<uint32_t, std::shared_ptr<const core::Vertex>> placements;

		Scene(bool broken = false, float speed = 0.75f, bool alternative = false)
		{
			bottom = world->addCorridor(0, 0, 12);
			top = world->addCorridor(1, 0, 12);
			remote = world->addRoom("Remote", 1, 0, 4, 4, 1);
			world->addSectorDoor(0, 0, 6);
			owner = world->addStaircase(1, 0, 0, { 4, CORE_SIDE_RIGHT, speed, broken });
			if (alternative) world->addStaircase(1, 0, 8, { 4, CORE_SIDE_RIGHT, 0.75f });
			std::map<uint32_t, std::shared_ptr<core::SectorObject>> markers;
			for (auto sector : { bottom, top, remote })
			{
				auto made = world->addSectorMarker(sector, 0, 0.5f);
				markers[sector] = made.sector->getObject(made.index);
			}
			world->finishBuild();
			for (auto const& [sector, marker] : markers) placements[sector] = world->getGraph()->getVertexForObject(marker);
			stairs = std::static_pointer_cast<const core::StaircaseTransit>(world->getSector(owner))->getStaircase();
			for (auto const& candidate : world->getGraph()->getEdges())
				if (candidate->getType() == core::EdgeType::Staircase
					&& candidate->getVertex(0)->getSector()->getIndex() == owner) edge = candidate;
			low = edge->getVertex(0)->getPosition().y < edge->getVertex(1)->getPosition().y
				? edge->getVertex(0) : edge->getVertex(1);
			high = edge->getOtherVertex(low);
			id = world->createAgent("Observer", bottom, 0, 0.5f);
			agent = world->lookupAgent(id).entity;
			world->pauseSimulation();
			world->setAgentIndividualMinimumRoutePlanningTime(id, 0.1f);
			world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f);
			world->resumeSimulation();
		}

		void place(uint32_t sector)
		{
			auto path = world->getGraph()->calculatePath(agent, placements.at(sector));
			smoke::require(bool(path), "Fixture placement has no Path to " + std::to_string(sector) + " from " + std::to_string(agent->getSector()->getIndex()));
			agent->setPath(path, true);
			for (int tick = 0; tick < 3600 && agent->getState() != core::Agent::State::Idle; ++tick) world->advanceTick();
			smoke::require(agent->getSector() == world->getSector(sector).get(), "Fixture placement failed");
			world->advanceTick();
		}
		void mobility(core::MobilityUse stairsUse, core::MobilityUse escalatorUse)
		{
			core::MobilityProfile profile;
			profile.set(core::TraversalKind::Staircase, stairsUse);
			profile.set(core::TraversalKind::Escalator, escalatorUse);
			world->pauseSimulation();
			world->setAgentIndividualMobilityProfile(id, profile);
			world->resumeSimulation();
		}
		core::DirectedTraversalFacts facts(std::shared_ptr<const core::Vertex> target, bool captured = true)
		{
			auto profile = world->getRouteChoicePolicy().baselineProfile;
			profile.stairSpeedModifier = agent->getEffectiveStairSpeedModifier().value;
			core::RouteDecisionContext context{ agent, profile, world->getRouteChoicePolicy(),
				agent->getSector(), agent->getWalkSpeed(), world.get(), agent->getClimbSpeed(), true,
				0, 0, agent->getEffectiveMobilityProfile().value };
			return captured ? core::RouteTraversalInputs::capture(*edge, target, context).evaluate(context)
				: edge->getDirectedTraversalFacts(target, context);
		}
	};
}
