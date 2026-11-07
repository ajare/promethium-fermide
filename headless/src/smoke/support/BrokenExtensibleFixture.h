#pragma once

#include "Smoke.h"
#include "core/Agent.h"
#include "core/ForceBridgeSectorObject.h"
#include "core/LadderSectorObject.h"
#include "core/LadderTransit.h"
#include "core/Graph.h"
#include "core/World.h"
#include <algorithm>

namespace broken_extensible
{
	enum class Kind { RoomLadder, TransitLadder, Bridge };
	inline bool uses(std::shared_ptr<core::Path> const& path, core::TraversalResourceId resource)
	{
		return path && std::any_of(path->nodes.begin(), path->nodes.end(), [&](auto const& node)
			{ return node.edge && node.edge->getTraversalResourceId() == resource; });
	}
	struct Scene
	{
		std::shared_ptr<core::World> world = std::make_shared<core::World>("Broken extensibles", 12, 3);
		Kind kind;
		uint32_t room{}, upper{}, remote{}, owner{}, objectIndex{};
		core::TraversalResourceId resource, alternative;
		std::shared_ptr<core::ExtensibleObject> device;
		std::shared_ptr<const core::Vertex> target;
		std::map<uint32_t, std::shared_ptr<const core::Vertex>> placements;
		core::AgentId id;
		core::Agent* agent;

		Scene(Kind type, bool extended = true, bool extensible = true,
			bool initiallyBroken = false, bool alternatives = false) : kind(type)
		{
			world->addLayer();
			room = world->addRoom("Approach", 0, 0, 0, 11, type == Kind::TransitLadder ? 1 : 2);
			upper = type == Kind::TransitLadder ? world->addRoom("Upper", 0, 1, 0, 11, 1) : room;
			if (type != Kind::TransitLadder)
				for (uint32_t x = 0; x < 11; ++x)
					if (type != Kind::Bridge || x < 2 || x > 3) world->addSectorWalkway(room, 1, x);
			if (type == Kind::Bridge)
			{
				auto made = world->addSectorForceBridge(room, 1, 2,
					{ 2, CORE_SIDE_LEFT, extensible, extended, extensible ? 2u : 0u, {}, initiallyBroken });
				owner = room; objectIndex = made.forceBridge.index; resource = made.traversalResource;
				device = std::static_pointer_cast<core::ForceBridgeSectorObject>(made.forceBridge.sector->getObject(objectIndex))->getForceBridge();
			}
			else
			{
				core::World::CreateLadderOptions options{ 2, extensible, extended, 4, {}, initiallyBroken };
				auto made = type == Kind::TransitLadder ? world->addLadder(1, 0, 3, options)
					: world->addSectorLadder(room, 0, 3, options);
				owner = made.ladder.sector->getIndex(); objectIndex = made.ladder.index; resource = made.traversalResource;
				device = type == Kind::TransitLadder
					? std::static_pointer_cast<const core::LadderTransit>(made.ladder.sector)->getLadder()
					: std::static_pointer_cast<core::LadderSectorObject>(made.ladder.sector->getObject(objectIndex))->getLadder();
				if (alternatives)
				{
					options.extensible = false; options.initiallyBroken = false; options.startExtended = true;
					auto other = type == Kind::TransitLadder ? world->addLadder(1, 0, 8, options)
						: world->addSectorLadder(room, 0, 8, options);
					alternative = other.traversalResource;
				}
			}
			auto middle = world->addRoom("Middle", 1, 0, 0, 2, 2);
			world->addSectorWalkway(middle, 1, 0); world->addSectorWalkway(middle, 1, 1);
			remote = world->addRoom("Remote", 2, 0, 0, 11, 2);
			for (uint32_t x = 0; x < 11; ++x) world->addSectorWalkway(remote, 1, x);
			world->addSectorDoor(0, type == Kind::Bridge ? 1 : 0, 0);
			world->addSectorDoor(1, type == Kind::Bridge ? 1 : 0, 0);
			auto marker = world->addSectorMarker(upper, type == Kind::TransitLadder ? 0 : 1, type == Kind::Bridge ? 8.5f : 0.5f);
			auto object = marker.sector->getObject(marker.index);
			std::map<uint32_t, std::shared_ptr<core::SectorObject>> placementObjects;
			for (auto sector : { room, remote })
			{
				auto made = world->addSectorMarker(sector, type == Kind::Bridge ? 1 : 0, 0.5f);
				placementObjects[sector] = made.sector->getObject(made.index);
			}
			world->finishBuild(); target = world->getGraph()->getVertexForObject(object);
			for (auto const& [sector, placed] : placementObjects) placements[sector] = world->getGraph()->getVertexForObject(placed);
			id = world->createAgent("Observer", room, type == Kind::Bridge ? 1 : 0, 0.5f);
			agent = world->lookupAgent(id).entity;
			world->pauseSimulation();
			smoke::require(world->setAgentIndividualMinimumRoutePlanningTime(id, 0.1f)
				&& world->setAgentIndividualMaximumRoutePlanningTime(id, 0.1f)
				&& world->setAgentIndividualRoutePersistence(id, 1.0f), "Fixture property setup failed");
			smoke::require(world->resumeSimulation(), "Fixture resume failed");
		}
		std::shared_ptr<core::Path> path() const { return world->getGraph()->calculatePath(agent, target); }
		void place(uint32_t sector)
		{
			auto route = world->getGraph()->calculatePath(agent, placements.at(sector));
			smoke::require(bool(route), "Observation fixture cannot reach Location");
			agent->setPath(route, true);
			for (uint32_t tick = 0; tick < 1800 && agent->getState() != core::Agent::State::Idle; ++tick) world->advanceTick();
			smoke::require(agent->getSector() == world->getSector(sector).get(), "Observer failed to reach Location");
			world->advanceTick();
		}
		core::TraversalResourceSnapshot snapshot() const
		{
			for (auto const& value : world->getSimulationSnapshot().traversalResources)
				if (value.id == resource) return value;
			throw std::runtime_error("Extensible resource missing");
		}
	};
}
