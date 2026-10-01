#include "Checks.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/MarkerSectorObject.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work;
		world.serialize(*writer, work);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::unique_ptr<core::World> deserializeWorld(std::string const& yaml)
	{
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		auto world = std::make_unique<core::World>("placeholder", 1, 1);
		core::SerializationWorkData work;
		require(world->deserialize(*reader, work), "A complete World did not deserialize");
		return world;
	}

	void markersHaveValidatedStableIdentity()
	{
		core::World world("Marker identity", 12, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 1);
		world.addSectorMarker(room, 0, 1.5f, "  Entrance  ");
		world.addSectorMarker(room, 0, 3.5f, "entrance");
		auto ids = world.getMarkerIds();
		require(ids.size() == 2 && ids[0].value != 0 && ids[1].value > ids[0].value,
			"New Markers did not receive monotonic nonzero IDs");
		require(world.lookupMarker(ids[0])->getName() == "Entrance",
			"Marker creation did not store the trimmed name");
		require(world.lookupMarker(ids[1])->getName() == "entrance",
			"Marker name uniqueness was not case-sensitive");

		auto otherRoom = world.addRoom("Other Room", 1, 0, 0, 12, 1);
		world.addSectorMarker(otherRoom, 0, 4.5f, "Elsewhere");
		std::string diagnostic;
		require(!world.canRenameMarker(ids[0], "Elsewhere", &diagnostic),
			"Marker names were only checked in the current Sector");
		require(world.canRenameMarker(ids[0], "Entrance", &diagnostic),
			"Marker name validation did not exclude its own identity");
		require(!world.canRenameMarker(ids[1], " Entrance ", &diagnostic),
			"A duplicate Marker name was accepted");
		require(!world.canRenameMarker(ids[1], "\xC0\x80", &diagnostic),
			"Malformed UTF-8 was accepted as a Marker name");
		require(world.renameMarker(ids[0], "  Lobby  ", &diagnostic),
			"A valid Marker rename was refused: " + diagnostic);
		require(world.lookupMarker(ids[0])->getName() == "Lobby",
			"Marker rename changed or lost the destination");
		auto const blocksPathing = core::markerPropertyBit(
			core::MarkerProperty::BlocksPathing);
		require(world.setMarkerProperties(ids[0], blocksPathing, &diagnostic),
			"A valid Marker property edit was refused: " + diagnostic);
		require(world.lookupMarker(ids[0])->hasProperty(core::MarkerProperty::BlocksPathing),
			"The Marker did not retain its Blocks pathing property");
		require(!world.setMarkerProperties(ids[0], blocksPathing | (1u << 31), &diagnostic),
			"An unknown Marker property bit was accepted");

		auto const beforeDelete = serializeWorld(world);
		auto firstObject = world.getSector(room)->getObject(0);
		require(firstObject && world.removeSectorMarker(room, 0),
			"Marker deletion through World failed");
		require(!world.lookupMarker(ids[0]), "A deleted Marker identity still resolves");
		auto restored = deserializeWorld(beforeDelete);
		require(restored->lookupMarker(ids[0])
			&& restored->lookupMarker(ids[0])->getName() == "Lobby",
			"Snapshot restoration did not restore the same Marker identity");
		require(restored->lookupMarker(ids[0])->hasProperty(
			core::MarkerProperty::BlocksPathing),
			"Snapshot restoration did not restore Marker properties");

		world.addSectorMarker(room, 0, 5.5f, "Replacement");
		auto afterDelete = world.getMarkerIds();
		require(afterDelete.back().value > ids.back().value,
			"A deleted Marker ID was reused");

		core::World highestDeleted("Deleted high-water mark", 4, 1);
		auto corridor = highestDeleted.addCorridor(0, 0, 4);
		auto only = highestDeleted.addSectorMarker(corridor, 0, 1.5f, "Only");
		auto deletedId = highestDeleted.getMarkerIds().front();
		require(highestDeleted.removeSectorMarker(corridor, only.index),
			"The highest Marker could not be deleted");
		auto reopened = deserializeWorld(serializeWorld(highestDeleted));
		reopened->pauseSimulation();
		reopened->addSectorMarker(corridor, 0, 2.5f, "Later");
		require(reopened->getMarkerIds().front().value > deletedId.value,
			"Save/load reused the highest deleted Marker ID");
	}

	void blockingMarkerCannotBeAnIntermediatePathVertex()
	{
		core::World world("Blocking Marker", 12, 1);
		auto corridor = world.addCorridor(0, 0, 12);
		uint32_t sourceVertex = 0, blockingVertex = 0, destinationVertex = 0;
		world.addSectorMarker(corridor, 0, 1.5f, "Source", &sourceVertex);
		world.addSectorMarker(corridor, 0, 5.5f, "Blocker", &blockingVertex);
		world.addSectorMarker(corridor, 0, 9.5f, "Destination", &destinationVertex);
		world.finishBuild();
		world.pauseSimulation();

		auto graph = world.getGraph();
		core::Agent routingAgent("Marker pathing check");
		auto source = graph->getVertexByIdentifier(sourceVertex);
		auto blocker = graph->getVertexByIdentifier(blockingVertex);
		auto destination = graph->getVertexByIdentifier(destinationVertex);
		require(graph->calculatePath(&routingAgent, source, destination) != nullptr,
			"The linear Marker route was not initially traversable");

		auto blockerId = world.getMarkerIds()[1];
		auto const blocksPathing = core::markerPropertyBit(
			core::MarkerProperty::BlocksPathing);
		std::string diagnostic;
		require(world.setMarkerProperties(blockerId, blocksPathing, &diagnostic),
			"Could not set Blocks pathing: " + diagnostic);
		require(graph->calculatePath(&routingAgent, source, destination) == nullptr,
			"A Path ran through a Marker which blocks pathing");
		require(graph->calculatePath(&routingAgent, source, blocker) != nullptr,
			"A blocking Marker could not remain a Path destination");
		require(graph->calculatePath(&routingAgent, blocker, destination) != nullptr,
			"A Path could not start at a blocking Marker");

		require(world.setMarkerProperties(blockerId, 0, &diagnostic),
			"Could not clear Blocks pathing: " + diagnostic);
		require(graph->calculatePath(&routingAgent, source, destination) != nullptr,
			"Clearing Blocks pathing did not restore the route");
	}

	void identitySurvivesReplayAndCompleteSerialization()
	{
		core::World world("Marker replay", 12, 1);
		auto left = world.addCorridor(0, 0, 5);
		auto right = world.addCorridor(0, 7, 5);
		auto created = world.addSectorMarker(left, 0, 2.5f, "Destination");
		auto id = world.getMarkerIds().front();
		world.finishBuild();
		world.pauseSimulation();
		auto plan = world.planMoveSectorObject(left, created.index, 9, 0);
		require(plan.valid, "A Marker move could not be planned: " + plan.diagnostic);
		require(static_cast<bool>(world.applyObjectMove(plan)), "A Marker move could not be applied");
		require(world.lookupMarker(id) && world.lookupMarker(id)->getName() == "Destination",
			"Object replay changed Marker identity");

		auto reopened = deserializeWorld(serializeWorld(world));
		require(reopened->lookupMarker(id) && reopened->lookupMarker(id)->getName() == "Destination",
			"Save/load did not preserve Marker identity and name");
		auto movedIds = reopened->getMarkerIds();
		require(movedIds.size() == 1 && movedIds.front() == id,
			"Save/load changed the Marker ID");

		auto moved = reopened->lookupMarker(id);
		require(moved && moved->getCellX() == 9, "The moved Marker did not round-trip");
		(void)right;
	}
}

void runMarkerIdentitySmokeChecks()
{
	markersHaveValidatedStableIdentity();
	blockingMarkerCannotBeAnIntermediatePathVertex();
	identitySurvivesReplayAndCompleteSerialization();
}
