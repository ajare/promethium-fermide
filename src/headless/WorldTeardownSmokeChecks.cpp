// World teardown and topology replacement must release owned entities, for
// ticket #182.
//
// The graph and the authored World used to keep each other alive through
// strong-reference cycles: Vertex <-> Edge, Sector <-> SectorObject, and
// Door <-> Sector. These checks keep only weak pointers to those entities,
// destroy the World, and require every one of them to expire. A failure means
// a cycle still retains owned memory after the World is gone.
//
// The checks mirror the manual verification in the ticket:
//   * a built World (graph present) releases its Vertices, Edges, and Sectors
//   * an unbuilt World still releases its Sector, isolating the
//     Sector <-> SectorObject cycle
//   * a Door's threshold references do not retain its Sectors either

#include <memory>
#include <stdexcept>

#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Edge.h"
#include "core/Graph.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/Vertex.h"
#include "core/World.h"

namespace
{
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void builtWorldReleasesGraphAndSectorOwnership()
	{
		std::weak_ptr<const core::Vertex> weakVertex;
		std::weak_ptr<const core::Edge> weakEdge;
		std::weak_ptr<const core::Sector> weakSector;

		{
			core::World world("Teardown built", 20, 1);
			auto const sectorIndex = world.addCorridor(0, 0, 10);
			world.addSectorMarker(sectorIndex, 0, 1.5f);
			world.addSectorMarker(sectorIndex, 0, 8.5f);
			world.finishBuild();

			auto const graph = world.getGraph();
			require(!graph->getVertices().empty(), "The built fixture has no Vertices");
			require(!graph->getEdges().empty(), "The built fixture has no Edges");

			weakVertex = graph->getVertices().front();
			weakEdge = graph->getEdges().front();
			weakSector = world.getSector(sectorIndex);

			require(!weakVertex.expired() && !weakEdge.expired() && !weakSector.expired(),
				"The fixture's weak handles must be live before the World is destroyed");
		}

		require(weakVertex.expired(), "A Vertex survived World destruction");
		require(weakEdge.expired(), "An Edge survived World destruction");
		require(weakSector.expired(), "A Sector survived World destruction");
	}

	void unbuiltWorldReleasesSectorOwnership()
	{
		std::weak_ptr<const core::Sector> weakSector;

		{
			core::World world("Teardown unbuilt", 20, 1);
			auto const sectorIndex = world.addCorridor(0, 0, 10);
			// One Marker authored but no finishBuild(): this isolates the
			// Sector <-> SectorObject ownership cycle from any graph cycle.
			world.addSectorMarker(sectorIndex, 0, 4.5f);

			weakSector = world.getSector(sectorIndex);
			require(!weakSector.expired(), "The fixture Sector must be live before the World is destroyed");
		}

		require(weakSector.expired(), "A Sector survived an unbuilt World's destruction");
	}

	void doorThresholdDoesNotRetainSectors()
	{
		std::weak_ptr<const core::Sector> weakFront;
		std::weak_ptr<const core::Sector> weakBack;
		std::weak_ptr<const core::Door> weakDoor;

		{
			core::World world("Teardown door", 20, 1);
			auto const fore = world.addRoom("Fore", 0, 0, 0, 8, 1);
			auto const aft = world.addRoom("Aft", 1, 0, 0, 8, 1);
			world.addSectorDoor(0, 0, 0);
			world.finishBuild();

			weakFront = world.getSector(fore);
			weakBack = world.getSector(aft);

			// Find the Door's SectorObject and hold its Door weakly. The Door is
			// owned by the SectorObject, which is owned by the Sector.
			for (uint32_t s = 0; s < world.getNumSectors(); ++s)
			{
				auto const sector = world.getSector(s);
				for (uint32_t i = 0; sector && i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					auto const doorObject = std::static_pointer_cast<const core::DoorSectorObject>(object);
					weakDoor = doorObject->getDoor();
					break;
				}
				if (!weakDoor.expired()) break;
			}

			require(!weakFront.expired() && !weakBack.expired() && !weakDoor.expired(),
				"The door fixture's weak handles must be live before the World is destroyed");
		}

		require(weakFront.expired(), "A front Sector survived a door World's destruction");
		require(weakBack.expired(), "A back Sector survived a door World's destruction");
		require(weakDoor.expired(), "A Door survived a door World's destruction");
	}

	void topologyRebuildReleasesPreviousGraph()
	{
		std::weak_ptr<const core::Vertex> staleVertex;
		std::weak_ptr<const core::Edge> staleEdge;

		core::World world("Teardown rebuild", 20, 1);
		auto const sectorIndex = world.addCorridor(0, 0, 10);
		world.addSectorMarker(sectorIndex, 0, 1.5f);
		world.addSectorMarker(sectorIndex, 0, 8.5f);
		world.finishBuild();

		{
			auto const graph = world.getGraph();
			staleVertex = graph->getVertices().front();
			staleEdge = graph->getEdges().front();
			require(!staleVertex.expired() && !staleEdge.expired(),
				"The initial Graph's weak handles must be live before the rebuild");
		}

		world.pauseSimulation();
		world.finishBuild();

		require(staleVertex.expired(), "A Vertex from the replaced Graph survived a topology rebuild");
		require(staleEdge.expired(), "An Edge from the replaced Graph survived a topology rebuild");
	}
}

void runWorldTeardownSmokeChecks()
{
	builtWorldReleasesGraphAndSectorOwnership();
	unbuiltWorldReleasesSectorOwnership();
	doorThresholdDoesNotRetainSectors();
	topologyRebuildReleasesPreviousGraph();
}
