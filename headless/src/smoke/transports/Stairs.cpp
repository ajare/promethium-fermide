#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include "core/Agent.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/LiftTransit.h"
#include "core/LiftSectorObject.h"
#include "core/DoorSectorObject.h"
#include "core/DoorVertex.h"
#include "core/Staircase.h"
#include "core/Transit.h"

namespace
{
	bool staircasePathSpansOuterCellEdges()
	{
		core::Staircase risingRight(5, 0, 3, CORE_SIDE_RIGHT);
		auto const rightPath = risingRight.getPath();
		core::Staircase risingLeft(5, 0, 3, CORE_SIDE_LEFT);
		auto const leftPath = risingLeft.getPath();
		return rightPath[0] == core::Vector2{ 0.0f, 0.0f }
			&& rightPath[1] == core::Vector2{ 3.0f, 1.0f }
			&& leftPath[0] == core::Vector2{ 3.0f, 0.0f }
			&& leftPath[1] == core::Vector2{ 0.0f, 1.0f };
	}

	bool staircaseCanUseForeRoomEndpoints()
	{
		core::World world("Room staircase landing", 10, 3);
		world.addCorridor(0, 0, 6);
		auto upperCorridor = world.addCorridor(1, 0, 7);
		auto room = world.addRoom("Upper room", 0, 1, 7, 3, 1);

		std::string diagnostic;
		if (world.canAddStaircase(1, 0, 5, 3, CORE_SIDE_RIGHT, &diagnostic)) return false;
		world.removeLocationWall(room, 0, CORE_SIDE_LEFT);
		if (!world.canAddStaircase(1, 0, 5, 3, CORE_SIDE_RIGHT, &diagnostic)) return false;
		auto staircase = world.addStaircase(1, 0, 5,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		world.finishBuild();
		if (staircase == ~0u || !world.isTraversalTopologyValid()) return false;
		world.pauseSimulation();
		auto edit = world.planResizeStaircase(staircase, 5, 0,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		if (!edit.valid
			|| world.getSector(room)->getEndType(0, CORE_SIDE_LEFT) != core::SectorEndType::None
			|| world.getSector(upperCorridor)->getEndType(0, CORE_SIDE_RIGHT) != core::SectorEndType::None)
			return false;

		core::World lowerRoomWorld("Lower Room staircase endpoint", 8, 3);
		lowerRoomWorld.addRoom("Lower room", 0, 0, 0, 3, 1);
		lowerRoomWorld.addCorridor(1, 4, 4);
		if (!lowerRoomWorld.canAddStaircase(1, 0, 2, 3, CORE_SIDE_RIGHT, &diagnostic))
			return false;
		lowerRoomWorld.addStaircase(1, 0, 2,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.0f });
		lowerRoomWorld.finishBuild();
		if (!lowerRoomWorld.isTraversalTopologyValid()) return false;

		// escalator-test-1.world.yaml: the flight starts on the Room's ground-level floor and
		// reaches its upper-right edge, where the wall into the upper Corridor is open.
		core::World mapWorld("Escalator map Room landing", 16, 3);
		mapWorld.addRoom("Room 1", 0, 1, 10, 4, 2);
		mapWorld.addCorridor(2, 14, 2);
		mapWorld.removeLocationWall(0, 1, CORE_SIDE_RIGHT);
		if (!mapWorld.canAddStaircase(1, 1, 11, 3, CORE_SIDE_RIGHT, &diagnostic))
			return false;
		mapWorld.addStaircase(1, 1, 11,
			core::World::CreateStaircaseOptions{ 3, CORE_SIDE_RIGHT, 0.4f });
		mapWorld.finishBuild();
		return mapWorld.isTraversalTopologyValid();
	}

	bool stairwellCoordinationIsExplicitlyOptIn()
	{
		core::World ordinary("Ordinary stairwell", 5, 3);
		ordinary.addCorridor(0, 0, 4);
		ordinary.addCorridor(1, 0, 4);
		ordinary.addStairwell(1, 0, 1, 2, CORE_SIDE_LEFT);
		ordinary.finishBuild();
		if (!ordinary.getSimulationSnapshot().traversalResources.empty()) return false;

		core::World narrow("Narrow stairwell", 5, 3);
		narrow.addCorridor(0, 0, 4);
		narrow.addCorridor(1, 0, 4);
		core::World::CreateStairwellOptions options{ 2, CORE_SIDE_LEFT };
		options.directionalCapacity = 1;
		options.directionalBatchLimit = 3;
		auto created = narrow.addStairwell(1, 0, 1, options);
		narrow.finishBuild();
		auto snapshot = narrow.getSimulationSnapshot();
		if (!created.traversalResource || snapshot.traversalResources.size() != 1
			|| !snapshot.traversalResources.front().isNarrowStairwell
			|| snapshot.traversalResources.front().capacity != 1
			|| snapshot.traversalResources.front().directionalBatchLimit != 3) return false;
		return std::all_of(narrow.getGraph()->getEdges().begin(), narrow.getGraph()->getEdges().end(),
			[&](auto const& edge)
			{
				return edge->getType() != core::EdgeType::Stairwell
					|| edge->getTraversalResourceId() == created.traversalResource;
			});
	}
}

void registerStairs(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "staircasePathSpansOuterCellEdges", [](smoke::Context const&) { smoke::require(staircasePathSpansOuterCellEdges(), "staircasePathSpansOuterCellEdges"); } });
	checks.push_back({ "staircaseCanUseForeRoomEndpoints", [](smoke::Context const&) { smoke::require(staircaseCanUseForeRoomEndpoints(), "staircaseCanUseForeRoomEndpoints"); } });
	checks.push_back({ "stairwellCoordinationIsExplicitlyOptIn", [](smoke::Context const&) { smoke::require(stairwellCoordinationIsExplicitlyOptIn(), "stairwellCoordinationIsExplicitlyOptIn"); } });
}
