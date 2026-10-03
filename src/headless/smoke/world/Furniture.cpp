#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"
#include <limits>
#include "core/Exceptions.h"

namespace
{
	std::string snapshot(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
	}

	void chair(smoke::Context const& context)
	{
		using smoke::require;
		auto catalogue = core::FurnitureCatalogue::load(context.fixture("resources/test-worlds/chair.furniture.yaml"));
		core::World transitWorld("Unsupported Furniture host", 4, 2);
		transitWorld.addCorridor(0, 0, 0, 4, 1); transitWorld.addCorridor(0, 1, 0, 4, 1);
		auto ladder = transitWorld.addLadder(1, 0, 2, {2, false, true});
		transitWorld.attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		std::string refusal;
		require(!transitWorld.canPlaceFurniture(ladder.ladder.sector->getIndex(), "chair", 0, 0, "Transit chair", &refusal)
			&& refusal.find("Room, Corridor or Facade") != std::string::npos, "Transit admitted Furniture");
		core::World world("Chair authoring", 16, 4);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 3);
		auto corridor = world.addCorridor(0, 3, 0, 8, 1);
		auto facade = world.addFacade("Facade", 0, 0, 8, 8, 1);
		auto background = world.addBackground(1, 0, 0, 8, 1);
		world.attachFurnitureCatalogue("chair.furniture.yaml", catalogue);
		world.addSectorWalkway(room, 1, 2); world.addSectorWalkway(room, 1, 3);
		world.placeFurniture(room, "chair", 2.25f, 0, "Reading chair");
		world.placeFurniture(room, "chair", 3.25f, 0, "Adjacent chair");
		world.placeFurniture(room, "chair", 2.25f, 1, "Walkway chair");
		world.placeFurniture(corridor, "chair", 2.5f, 0, "Corridor chair");
		world.placeFurniture(facade, "chair", 2.5f, 0, "Facade chair");
		auto seat = world.furniture().front().marker;
		require(world.getMarkerIds().size() == 5 && world.lookupMarker(seat)->getName() == "Reading chair Seat"
			&& world.lookupMarker(seat)->hasProperty(core::MarkerProperty::BlocksPathing), "Chair Marker is not an ordinary named blocking destination");
		auto refused = [&](uint32_t sector, float x, float y, std::string const& name, std::string key = "chair") {
			auto before = snapshot(world); auto dirty = world.isModified(); std::string diagnostic;
			require(!world.canPlaceFurniture(sector, key, x, y, name, &diagnostic) && !diagnostic.empty(), "Invalid Furniture placement accepted");
			try { world.placeFurniture(sector, key, x, y, name); require(false, "Invalid Furniture mutation accepted"); }
			catch (core::Exception const&) {}
			require(snapshot(world) == before && dirty == world.isModified(), "Refused Furniture placement partially mutated World");
		};
		refused(background, 2, 0, "Backdrop chair"); refused(room, 7.5f, 0, "Overhang");
		refused(room, 2, 0.1f, "Floating"); refused(room, 1.5f, 1, "Gap");
		refused(room, 3.5f, 1, "Walkway overhang"); refused(room, 2.5f, 0, "Overlap");
		refused(room, 5, 0, "Reading chair"); refused(room, 5, 0, "");
		refused(room, 5, 0, "Missing", "not-a-chair");
		refused(room, std::numeric_limits<float>::quiet_NaN(), 0, "NaN");
		uint32_t leftId = 0, rightId = 0;
		world.addSectorMarker(room, 0, 0.5f, "Entrance", &leftId);
		world.addSectorMarker(room, 0, 6.5f, "Exit", &rightId);
		world.finishBuild();
		auto graph = world.getGraph(); auto sector = world.getSector(room);
		std::shared_ptr<const core::Vertex> seatVertex;
		// Look up the ordinary Marker object by stable identity, not object slot.
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(sector->getObject(i));
				object && object->getMarker()->getId() == seat) seatVertex = graph->getVertexForObject(object);
		require(seatVertex != nullptr, "Owned seat has no routing vertex");
		core::Agent query("Query");
		auto left = graph->getVertexByIdentifier(leftId), right = graph->getVertexByIdentifier(rightId);
		auto through = graph->calculatePath(&query, left, right);
		require(through != nullptr, "Blocking seat severed ordinary floor circulation");
		for (auto const& node : through->nodes) require(node.targetVertex != seatVertex, "Blocking seat became a through-waypoint");
		require(graph->calculatePath(&query, left, seatVertex) && graph->calculatePath(&query, seatVertex, right), "Seat cannot be reached or left");
		auto agent = world.lookupAgent(world.createAgent("Visitor", room, 0, 0.5f)).entity;
		agent->setPath(graph->calculatePath(agent, seatVertex), true); world.advanceTicks(600);
		require(std::abs(agent->getGlobalPosition().x - 2.75f) < 0.01f, "Agent did not arrive at chair");
		agent->setPath(graph->calculatePath(agent, right), true); world.advanceTicks(600);
		require(std::abs(agent->getGlobalPosition().x - 6.5f) < 0.01f, "Agent could not leave chair");
		world.pauseSimulation();
		auto id = world.furniture().front().id;
		auto walkerId = world.createAgent("Walker", room, 0, 0.5f);
		auto walker = world.lookupAgent(walkerId).entity;
		walker->setPath(world.getGraph()->calculatePath(walker, seatVertex), true);
		require(world.resumeSimulation(), "Could not resume destination journey");
		world.advanceTicks(10); world.pauseSimulation();
		auto walkerPosition = world.lookupAgent(walkerId).entity->getGlobalPosition();
		auto visitor = world.getAgentId(agent);
		auto position = world.lookupAgent(visitor).entity->getGlobalPosition();
		std::string diagnostic;
		require(world.renameMarker(seat, "Independently authored", &diagnostic), diagnostic);
		auto before = snapshot(world);
		require(!world.editFurniture(id, 3.5f, 0, "Renamed", &diagnostic)
			&& snapshot(world) == before, "Overlapping move was not a no-op");
		require(!world.editFurniture(id, 5, 0.2f, "Renamed", &diagnostic)
			&& snapshot(world) == before, "Floating move was not a no-op");
		require(world.editFurniture(id, 5.25f, 0, "Renamed", &diagnostic), diagnostic);
		require(world.furniture().front().marker == seat && world.lookupMarker(seat)->getName() == "Independently authored"
			&& world.lookupAgent(visitor).entity->getGlobalPosition() == position, "Move changed names, identities or Agent position");
		require(world.lookupAgent(walkerId).entity->getGlobalPosition() == walkerPosition, "Moving a destination teleported its travelling Agent");
		require(world.resumeSimulation(), "Could not resume relocated destination intent");
		world.advanceTicks(900);
		require(std::abs(world.lookupAgent(walkerId).entity->getGlobalPosition().x - 5.75f) < 0.01f,
			"Existing planning did not follow the same relocated Marker");
		world.pauseSimulation();
		uint32_t ownedSlot = ~0u, walkwaySlot = ~0u;
		sector = world.getSector(room);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto object = sector->getObject(i);
			if (auto marker = std::dynamic_pointer_cast<core::MarkerSectorObject>(object);
				marker && marker->getMarker()->getId() == seat) ownedSlot = i;
			if (object && object->getObjectType() == core::SectorObjectType::Walkway) walkwaySlot = i;
		}
		before = snapshot(world);
		auto generation = world.getTopologyGeneration();
		auto dirty = world.isModified();
		require(!world.removeSectorMarker(room, ownedSlot, &diagnostic)
			&& !diagnostic.empty(), "Owned Marker deletion succeeded");
		try { world.applyObjectMove(world.planMoveSectorObject(room, ownedSlot, 6, 0)); require(false, "Owned Marker movement succeeded"); }
		catch (core::Exception const&) {}
		require(!world.canRemoveSectorMarker(room, ownedSlot, &diagnostic)
			&& !world.planMoveSectorObject(room, ownedSlot, 6, 0).valid, "Owned Marker accepted an independent layout edit");
		require(!world.renameMarker(seat, "Exit", &diagnostic) && snapshot(world) == before,
			"Owned Marker rename bypassed World namespace or mutated on failure");
		auto removeRoom = world.planRemoveLocation(room);
		require(!removeRoom.valid && removeRoom.diagnostic.find("Renamed") != std::string::npos,
			"Ground Floor removal did not identify blocking Furniture");
		auto cropRoom = world.planResizeLocation(room, 0, 0, 5, 3);
		require(!cropRoom.valid && cropRoom.diagnostic.find("Renamed") != std::string::npos,
			"Room cropping silently removed unsupported Furniture");
		try { world.applyLocationEdit(removeRoom); require(false, "Ground Floor removal succeeded"); }
		catch (core::Exception const&) {}
		try { world.applyLocationEdit(cropRoom); require(false, "Unsupported Room crop succeeded"); }
		catch (core::Exception const&) {}
		auto plan = world.planRemoveSectorWalkway(room, walkwaySlot);
		require(!plan.valid && plan.diagnostic.find("Walkway chair") != std::string::npos, "Walkway preflight did not identify Furniture");
		try { world.removeSectorWalkway(room, walkwaySlot); require(false, "Unsupported Furniture accepted"); }
		catch (core::Exception const&) {}
		require(snapshot(world) == before, "Refused owned-point or support edits mutated serialization");
		require(world.getTopologyGeneration() == generation && world.isModified() == dirty,
			"Refused edits changed topology generation or modified state");
		auto registry = core::AgentBehaviourRegistry::create();
		world.attachAgentBehaviourRegistry("furniture.behaviours", registry);
		auto behaviour = registry->addAgentBehaviour("Visit", "visit.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
		});
		require(world.setAgentBehaviourAssignment(visitor, behaviour, 1, {{"destination", seat}}, &diagnostic), diagnostic);
		require(world.renameMarker(seat, "New destination label", &diagnostic), diagnostic);
		require(*core::agentBehaviourConfigurationGetIf<core::MarkerId>(
			&world.getAgentBehaviourAssignment(visitor)->configuration.at("destination")) == seat,
			"Marker rename retargeted behaviour configuration");
		before = snapshot(world);
		require(!world.removeFurniture(id, &diagnostic) && diagnostic.find("Visitor") != std::string::npos
			&& diagnostic.find("destination") != std::string::npos && snapshot(world) == before, "Referenced Furniture deletion was not transactional");
		require(world.clearAgentBehaviourAssignment(visitor, &diagnostic), diagnostic);
		auto markerCount = world.getMarkerIds().size();
		require(world.removeFurniture(id, &diagnostic) && !world.lookupMarker(seat)
			&& world.getMarkerIds().size() == markerCount - 1, "Deletion left an owned destination");
		auto next = world.placeFurniture(room, "chair", 5.25f, 0, "Replacement");
		require(next > id && world.furniture().back().marker.value > seat.value, "Deletion reused identities");
	}
}
void registerFurniture(std::vector<smoke::Check>& checks) { checks.push_back({ "furniture/chair", chair }); }
