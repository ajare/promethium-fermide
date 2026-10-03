#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
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
	}
}
void registerFurniture(std::vector<smoke::Check>& checks) { checks.push_back({ "furniture/chair", chair }); }
