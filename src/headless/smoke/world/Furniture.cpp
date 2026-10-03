#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"
#include "core/AgentTagRegistryDocument.h"
#include <limits>
#include "core/Exceptions.h"
#include <fstream>
#include <yaml-cpp/yaml.h>

namespace
{
	std::string snapshot(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
	}

	void demonstration(smoke::Context const& context)
	{
		using smoke::require;
		auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/furniture.world.yaml"));
		require(world->furniture().size() == 6 && world->furniture()[3].y == 1
			&& world->furniture()[2].destinations.size() == 2
			&& world->furniture()[2].destinations[0].marker != world->furniture()[2].destinations[1].marker,
			"Required demonstration lost Walkway support or distinct sofa destinations");
		std::shared_ptr<const core::Vertex> exit;
		for (auto const& vertex : world->getGraph()->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getName() == "Desk Far side") exit = vertex;
		require(exit != nullptr, "Required demo Exit Marker is missing");
		for (auto const& entry : world->getSimulationSnapshot().agents)
		{
			auto agent = world->lookupAgent(entry.id).entity;
			if (agent->getName() == "Showroom walker") continue;
			auto depth = agent->getLocalDepth();
			auto path = world->getGraph()->calculatePath(agent, exit);
			require(path != nullptr, "Stationary demonstration observer cannot depart");
			bool side = false;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getLength() > 0 && node.edge->getVertex(0)->getPosition().x >= 2.375f
					&& node.edge->getVertex(1)->getPosition().x <= 3.875f)
				{
					side = true;
					require(node.edge->getLocalDepth() == depth, "Equal-cost demo departure lost retained depth continuity: " + agent->getName()
						+ " selected=" + std::to_string(node.edge->getLocalDepth()) + " x=" + std::to_string(node.edge->getVertex(0)->getPosition().x));
				}
			require(side, "Demo observer avoided the explicit front/back routes");
			agent->setPath(path, true);
		}
		world->advanceTicks(1800);
		for (auto const& entry : world->getSimulationSnapshot().agents)
			require(entry.globalPosition.x == (world->lookupAgent(entry.id).entity->getName() == "Showroom walker" ? 9.5f : 3.875f),
				"Bundled demo journey did not arrive normally");
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
	void layouts(smoke::Context const& context)
	{
		using smoke::require;
		core::World world("Multi-point layouts", 20, 5);
		auto room = world.addRoom("Room", 0, 0, 0, 20, 5);
		world.attachFurnitureCatalogue("layouts.furniture.yaml",
			core::FurnitureCatalogue::load(context.fixture("resources/test-worlds/layouts.furniture.yaml")));
		for (uint32_t x = 7; x < 12; ++x) world.addSectorWalkway(room, 1, x);
		for (uint32_t x = 4; x < 7; ++x) world.addSectorWalkway(room, 2, x);
		world.addSectorWalkway(room, 2, 7); world.addSectorWalkway(room, 2, 9);
		for (uint32_t x = 12; x < 16; ++x) world.addSectorWalkway(room, 2, x);
		auto sofa = world.placeFurniture(room, "sofa", 1.125f, 0, "Sofa");
		world.placeFurniture(room, "sofa", 3.125f, 0, "Touching sofa");
		world.placeFurniture(room, "larger", 8.25f, 0, "Sparse layout");
		world.placeFurniture(room, "sofa", 4.25f, 2, "Walkway sofa");
		world.placeFurniture(room, "larger", 13.25f, 2, "Walkway layout");
		auto points = world.furniture().front().destinations;
		require(points.size() == 2 && points[0].marker != points[1].marker
			&& world.lookupMarker(points[0].marker)->getName() == "Sofa Left seat"
			&& world.lookupMarker(points[1].marker)->getName() == "Sofa Right seat", "Sofa did not create independent labelled identities");
		auto refuse = [&](std::string const& key, float x, float y, std::string const& fragment) {
			auto before = snapshot(world); auto generation = world.getTopologyGeneration(); auto dirty = world.isModified();
			std::string diagnostic;
			require(!world.canPlaceFurniture(room, key, x, y, "Invalid", &diagnostic)
				&& diagnostic.find(fragment) != std::string::npos, diagnostic);
			try { world.placeFurniture(room, key, x, y, "Invalid"); require(false, "Invalid layout accepted"); }
			catch (core::Exception const&) {}
			require(snapshot(world) == before && generation == world.getTopologyGeneration() && dirty == world.isModified(),
				"Rejected multi-point placement mutated the World");
		};
		refuse("sofa", 18.25f, 0, "complete width");
		refuse("larger", 0.5f, 0, "complete width");
		refuse("larger", 8, 2, "continuous"); // Empty artwork cell still requires support.
		refuse("sofa", 6.25f, 2, "continuous"); // Fractional Walkway overhang.
		refuse("sofa", 8.25f, 1, "overlaps"); // Empty/transparent artwork still occupies the rectangle.
		refuse("larger", 13.25f, 2.25f, "floor-aligned");
		world.addSectorMarker(room, 0, 19.5f, "Invalid Right seat");
		refuse("sofa", 12.5f, 0, "already exists"); // Validate the second point before issuing either identity.
		auto malformedPath = context.temporaryRoot() / "invalid.furniture.yaml";
		auto catalogueYaml = YAML::LoadFile(context.fixture("resources/test-worlds/layouts.furniture.yaml").string());
		for (int variant = 0; variant < 5; ++variant)
		{
			auto invalid = YAML::Clone(catalogueYaml);
			auto definition = invalid["furnitureCatalogue"]["definitions"][0];
			if (variant == 0) definition["usablePoints"][1]["key"] = "left";
			if (variant == 1) definition["usablePoints"][1]["y"] = 0.25;
			if (variant == 2) definition["tiles"][1]["x"] = 1.25;
			if (variant == 3) definition["usablePoints"][1]["label"] = "Left seat";
			if (variant == 4) definition["usablePoints"][1]["x"] = ".nan";
			{ std::ofstream file(malformedPath); file << invalid; }
			bool refused = false;
			try { (void)core::FurnitureCatalogue::load(malformedPath); }
			catch (std::exception const&) { refused = true; }
			require(refused, "Malformed multi-point catalogue was accepted");
		}
		uint32_t entranceId = 0, exitId = 0;
		world.addSectorMarker(room, 0, 0.125f, "Entrance", &entranceId);
		world.addSectorMarker(room, 0, 18.875f, "Exit", &exitId);
		world.finishBuild(); world.pauseSimulation();
		auto graph = world.getGraph(); auto sector = world.getSector(room);
		auto vertexFor = [&](core::MarkerId id) {
			std::shared_ptr<const core::Vertex> result;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(sector->getObject(i));
					object && object->getMarker()->getId() == id) result = graph->getVertexForObject(object);
			return result;
		};
		core::Agent query("Query");
		auto entrance = graph->getVertexByIdentifier(entranceId), exit = graph->getVertexByIdentifier(exitId);
		auto through = graph->calculatePath(&query, entrance, exit);
		require(through != nullptr, "Multi-point layouts severed circulation");
		for (auto const& instance : world.furniture())
			for (auto const& point : instance.destinations)
			{
				auto seat = vertexFor(point.marker);
				auto const& definition = *world.furnitureCatalogue()->definition(instance.definitionKey);
				auto authored = std::find_if(definition.usablePoints.begin(), definition.usablePoints.end(),
					[&](auto const& p) { return p.key == point.key; });
				require(seat && seat->getPosition().x == instance.x + authored->x
					&& seat->getPosition().y == instance.y, "Graph lost fractional x or introduced an elevated movement point");
				require(world.lookupMarker(point.marker)->hasProperty(core::MarkerProperty::BlocksPathing), "Seat did not default to Blocks pathing");
				if (instance.y == 0)
				{
					for (auto const& node : through->nodes) require(node.targetVertex != seat, "Passing route used a seat");
					require(graph->calculatePath(&query, entrance, seat) && graph->calculatePath(&query, seat, exit), "Seat is not a valid origin/destination");
				}
			}
		std::string diagnostic;
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (auto object = sector->getObject(i); object && object->getObjectType() == core::SectorObjectType::Walkway
				&& object->getCellX() == 13 && object->getCellY() == 2)
			{
				auto plan = world.planRemoveSectorWalkway(room, i);
				require(!plan.valid && plan.diagnostic.find("Walkway layout") != std::string::npos,
					"Removing support under a transparent layout gap was permitted");
			}
		require(world.renameMarker(points[1].marker, "Independent right destination", &diagnostic), diagnostic);
		auto before = snapshot(world);
		require(!world.editFurniture(sofa, 4, 0, "Moved", &diagnostic) && snapshot(world) == before, "Overlapping multi-point move mutated identities");
		require(world.editFurniture(sofa, 15.375f, 0, "Moved sofa", &diagnostic), diagnostic);
		require(world.furniture().front().destinations[0].marker == points[0].marker
			&& world.furniture().front().destinations[1].marker == points[1].marker
			&& world.lookupMarker(points[1].marker)->getName() == "Independent right destination"
			&& world.lookupMarker(points[0].marker)->getCellX() + world.lookupMarker(points[0].marker)->getOffset() == 15.625f
			&& world.lookupMarker(points[1].marker)->getCellX() + world.lookupMarker(points[1].marker)->getOffset() == 17.f, "Rigid move lost identity/name/offset");
		auto registry = core::AgentBehaviourRegistry::create();
		world.attachAgentBehaviourRegistry("layouts.behaviours", registry);
		auto behaviour = registry->addAgentBehaviour("Visit", "visit.lua", {
			{ "destination", core::AgentBehaviourSchemaType::Marker, {}, true, std::nullopt }
		});
		for (size_t i = 0; i < points.size(); ++i)
		{
			auto agent = world.createAgent("Seat visitor " + std::to_string(i), room, 0, 0.125f);
			require(world.setAgentBehaviourAssignment(agent, behaviour, 1, {{"destination", points[i].marker}}, &diagnostic), diagnostic);
			require(*core::agentBehaviourConfigurationGetIf<core::MarkerId>(&world.getAgentBehaviourAssignment(agent)->configuration.at("destination"))
				== points[i].marker, "Independent destination selection retargeted a seat");
		}
		before = snapshot(world);
		require(!world.removeFurniture(sofa, &diagnostic) && diagnostic.find("Seat visitor 0") != std::string::npos
			&& diagnostic.find("Seat visitor 1") != std::string::npos && snapshot(world) == before,
			"Deletion did not protect every owned destination atomically");
	}
	void attachments(smoke::Context const& context)
	{
		using smoke::require;
		int baselineArrival = 0;
		for (int variant = 0; variant < 11; ++variant)
		{
			auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/attachments.furniture.yaml").string());
			auto desk = yaml["furnitureCatalogue"]["definitions"][0];
			auto chair = yaml["furnitureCatalogue"]["definitions"][1];
			// Isolate the back route: a chair must not acquire it through the front.
			desk["edges"].remove(5); desk["edges"].remove(3);
			if (variant == 2) chair["vertices"][0]["external"] = false;
			if (variant == 4) desk["vertices"][2]["external"] = true;
			if (variant == 5) chair["vertices"][1]["external"] = true;
			if (variant == 7) chair["edges"][0]["depthOffset"] = 2;
			if (variant == 9)
			{
				auto other = YAML::Clone(chair); other["key"] = "other";
				other["edges"][0]["depthOffset"] = -2;
				yaml["furnitureCatalogue"]["definitions"].push_back(other);
				auto coincident = YAML::Clone(other); coincident["key"] = "coincident";
				coincident["edges"][0]["depthOffset"] = -3;
				yaml["furnitureCatalogue"]["definitions"].push_back(coincident);
			}
			if (variant == 10) chair["edges"][0].remove("depthOffset");
			auto filename = context.temporaryRoot() / ("attachment" + std::to_string(variant) + ".furniture.yaml");
			{ std::ofstream file(filename); file << yaml; }
			core::World world("Attached arrangement", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue(filename.filename().string(), core::FurnitureCatalogue::load(filename));
			auto chairX = variant == 10 ? 2.25f : (variant == 3 || variant == 4 ? 2.375f : 3.125f);
			auto chairDepth = variant == 1 ? 3 : 1;
			uint64_t chairId = 0;
			if (variant == 6) chairId = world.placeFurniture(room, "chair", chairX, 0, "Chair", chairDepth);
			world.placeFurniture(room, "desk", 2.125f, 0, "Desk", 2);
			if (variant != 6 && variant != 8) chairId = world.placeFurniture(room, "chair", chairX, 0, "Chair", chairDepth);
			if (variant == 9)
			{
				world.placeFurniture(room, "other", 2.75f, 0, "Other chair", 4);
				world.placeFurniture(room, "coincident", 3.125f, 0, "Coincident chair", 5);
			}
			uint32_t a = 0, b = 0;
			world.addSectorMarker(room, 0, 0.5f, "Entrance", &a);
			world.addSectorMarker(room, 0, 6.5f, "Exit", &b);
			world.finishBuild();
			auto graph = world.getGraph(); core::Agent query("Query");
			auto left = graph->getVertexByIdentifier(a), right = graph->getVertexByIdentifier(b);
			auto through = graph->calculatePath(&query, left, right);
			require(through != nullptr, "Attachment severed authored front circulation");
			float frontLength = 0;
			unsigned frontSegments = 0;
			for (auto const& node : through->nodes)
				if (node.edge && node.edge->getLocalDepth() == 2 && node.edge->getLength() > 0)
				{ frontLength += node.edge->getLength(); ++frontSegments; }
			require(frontLength == 1.5f, "Splitting altered the authored route geometry/depth");
			if (variant == 0 || variant == 5 || variant == 6 || variant == 9)
				require(frontSegments > 1, "Middle external point did not split the route");
			if (variant != 8)
			{
				auto instance = std::find_if(world.furniture().begin(), world.furniture().end(),
					[&](auto const& f) { return f.id == chairId; });
				auto marker = instance->marker;
				std::shared_ptr<const core::Vertex> seat;
				for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
						object && object->getMarker()->getId() == marker) seat = graph->getVertexForObject(object);
				bool reachable = variant == 0 || variant == 4 || variant == 5 || variant == 6 || variant == 9 || variant == 10;
				require(bool(graph->calculatePath(&query, left, seat)) == reachable,
					"Port accessibility violated explicit matching-depth connectivity: " + std::to_string(variant));
				require(bool(graph->calculatePath(&query, seat, right)) == reachable, "Attachment was not bidirectional");
				for (auto const& node : through->nodes) require(node.targetVertex != seat, "Blocking seat became an intermediate waypoint");
				// The isolated back route is never implicitly joined by a front chair.
				if (variant == 0)
				{
					for (auto const& edge : graph->getEdges())
						if (edge->getLocalDepth() == 3 && edge->getLength() > 0)
							require(!graph->calculatePath(&query, seat, edge->getVertex(0)), "Chair approach implicitly joined the back route");
					auto visitor = world.lookupAgent(world.createAgent("Visitor", room, 0, 0.5f)).entity;
					visitor->setPath(graph->calculatePath(visitor, seat), true); world.advanceTicks(900);
					require(visitor->getGlobalPosition().x == 3.625f && visitor->getLocalDepth() == 2,
						"Agent did not arrive through the matching-depth port");
				}
			}
			auto walkerId = world.createAgent("Walker", room, 0, 0.5f);
			auto walker = world.lookupAgent(walkerId).entity;
			walker->setPath(graph->calculatePath(walker, right), true);
			int arrival = 0;
			for (int tick = 1; tick <= 900 && !arrival; ++tick)
			{
				world.advanceTicks(1);
				if (walker->getGlobalPosition().x == 6.5f) arrival = tick;
			}
			require(arrival > 0, "Split route failed physical traversal");
			if (variant == 0) baselineArrival = arrival;
			require(arrival == baselineArrival, "Attachment changed objective traversal duration");
			if (variant == 0)
			{
				world.pauseSimulation();
				auto position = walker->getGlobalPosition(); std::string diagnostic;
				require(world.editFurniture(chairId, 5, 0, "Moved chair", &diagnostic), diagnostic);
				require(world.lookupAgent(walkerId).entity->getGlobalPosition() == position, "Attachment rebuild teleported an Agent");
				require(world.removeFurniture(chairId, &diagnostic), diagnostic);
				unsigned restored = 0;
				for (auto const& edge : world.getGraph()->getEdges())
					if (edge->getLocalDepth() == 2 && edge->getLength() == 1.5f) ++restored;
				require(restored == 1, "Removing attachment left stale split contributions");
			}
		}
	}

	void composition(smoke::Context const& context)
	{
		using smoke::require;
		std::string outcomes[4][4];
		for (int shape = 0; shape < 4; ++shape)
			for (int variant = 0; variant < 4; ++variant)
				for (bool reverse : { false, true })
		{
			auto yaml = YAML::LoadFile(context.fixture("resources/test-worlds/composition.furniture.yaml").string());
			auto definitions = yaml["furnitureCatalogue"]["definitions"];
			if (variant == 1)
				for (auto edge : definitions[1]["edges"])
					if (edge["depthOffset"]) edge["depthOffset"] = 1;
			if (variant == 2)
			{
				definitions[1]["vertices"][0]["external"] = false;
				definitions[1]["vertices"][3]["external"] = false;
			}
			if (variant == 3)
			{
				definitions[0]["vertices"].push_back(YAML::Load("{key: gap, x: 2.5}"));
				definitions[0]["edges"][2]["from"] = "gap";
			}
			if (reverse)
			{
				auto first = YAML::Clone(definitions[0]);
				definitions[0] = YAML::Clone(definitions[1]); definitions[1] = first;
			}
			auto filename = context.temporaryRoot() / "composition.furniture.yaml";
			{ std::ofstream file(filename); file << yaml; }
			core::World world("Composed replacements", 16, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 16, 1);
			world.attachFurnitureCatalogue(filename.filename().string(), core::FurnitureCatalogue::load(filename));
			// Partial overlap, containment, shared boundary and coincident left boundary.
			float innerX = shape == 0 ? 5.f : shape == 1 ? 3.f : shape == 2 ? 6.f : 2.f;
			uint64_t outerId = 0, innerId = 0;
			if (reverse) innerId = world.placeFurniture(room, "inner", innerX, 0, "Inner", 3);
			outerId = world.placeFurniture(room, "outer", 2, 0, "Outer", 2);
			if (!reverse) innerId = world.placeFurniture(room, "inner", innerX, 0, "Inner", 3);
			uint32_t leftId = 0, rightId = 0, internalId = 0;
			world.addSectorMarker(room, 0, 0.5f, "Entrance", &leftId);
			world.addSectorMarker(room, 0, 14.5f, "Exit", &rightId);
			world.addSectorMarker(room, 0, shape == 2 ? 6.f : innerX, "Internal floor point", &internalId);
			world.finishBuild(); world.pauseSimulation();
			auto point = [&](std::string const& name) {
				std::shared_ptr<const core::Vertex> vertex;
				for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
						object && object->getMarker()->getName() == name) vertex = world.getGraph()->getVertexForObject(object);
				return vertex;
			};
			auto check = [&](bool throughExpected, bool seatExpected, bool internalExpected) {
				auto graph = world.getGraph(); core::Agent query("Query");
				auto left = point("Entrance"), right = point("Exit");
				auto through = graph->calculatePath(&query, left, right);
				require(bool(through) == throughExpected, "Replacement union acquired a bypass or lost authored circulation: shape=" + std::to_string(shape)
					+ " variant=" + std::to_string(variant) + " actual=" + std::to_string(bool(through)));
				if (through)
				{
					std::map<int, float> distances;
					for (auto const& node : through->nodes) if (node.edge)
						distances[node.edge->getLocalDepth()] += node.edge->getLength();
					std::string outcome;
					for (auto const& [depth, length] : distances)
						if (length > 0) outcome += std::to_string(depth) + ":" + std::to_string(length) + ";";
					if (outcomes[shape][variant].empty()) outcomes[shape][variant] = outcome;
					require(outcomes[shape][variant] == outcome, "Processing order/rebuild/replay changed Path geometry or depth");
				}
				if (through)
					for (auto const& node : through->nodes)
						if (node.edge && node.edge->getLength() > 0)
						{
							auto a = node.edge->getVertex(0)->getPosition().x;
							auto b = node.edge->getVertex(1)->getPosition().x;
							if (std::min(a, b) >= 2.25f && std::max(a, b) <= 5.75f)
								require(node.edge->getLocalDepth() == 2, "Path used an unassigned floor/cross-depth shortcut");
						}
				auto instance = std::find_if(world.furniture().begin(), world.furniture().end(),
					[&](auto const& f) { return f.id == innerId; });
				if (instance != world.furniture().end())
				{
					std::shared_ptr<const core::Vertex> seat;
					for (uint32_t i = 0; i < world.getSector(room)->getNumObjects(); ++i)
						if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(world.getSector(room)->getObject(i));
							object && object->getMarker()->getId() == instance->marker) seat = graph->getVertexForObject(object);
					require(seat && bool(graph->calculatePath(&query, left, seat)) == seatExpected,
						"Composed ports ignored designation or matching depth: shape=" + std::to_string(shape) + " variant=" + std::to_string(variant));
					if (through) for (auto const& node : through->nodes)
						require(node.targetVertex != seat, "Owned blocking destination became a shortcut");
				}
				require(bool(graph->calculatePath(&query, left, point("Internal floor point"))) == internalExpected,
					"Replacement boundary invented a floor junction");
			};
			bool connected = variant != 2 && (variant == 0 || shape == 2 || shape == 3 || (variant == 3 && shape == 1));
			if (variant == 3 && shape == 2) connected = false;
			bool through = variant == 3 ? shape == 1 : shape == 0 ? variant == 0 : shape == 2 ? variant != 2 : true;
			check(through, connected, shape == 3);
			for (int rebuild = 0; rebuild < 2; ++rebuild)
			{
				require(world.rebuildTraversalTopology(), world.getTopologyDiagnostic());
				check(through, connected, shape == 3);
			}
			world.applyLocationEdit(world.planResizeLocation(room, 0, 0, 16, 2));
			check(through, connected, shape == 3);
			auto before = snapshot(world); std::string diagnostic;
			require(!world.editFurniture(innerId, 3, 0, "Refused", &diagnostic, 2)
				&& snapshot(world) == before, "Same-depth edit mutated composed destinations");
			require(!world.canPlaceFurniture(room, "inner", 3, 0, "Refused", &diagnostic, 2)
				&& snapshot(world) == before, "Same-depth placement was admitted");
			// Exercise the opposite movement/removal order as well.
			if (reverse)
			{
				require(world.editFurniture(outerId, 8, 0, "Moved outer", &diagnostic), diagnostic);
				core::Agent movedQuery("Moved query");
				require(bool(world.getGraph()->calculatePath(&movedQuery, point("Entrance"), point("Exit"))) == (variant != 2 && variant != 3),
					"Outer movement restored a bypass or lost remaining contributions");
				require(world.removeFurniture(outerId, &diagnostic), diagnostic);
				core::Agent query("Query");
				require(bool(world.getGraph()->calculatePath(&query, point("Entrance"), point("Exit"))) == (variant != 2),
					"Removing outer damaged remaining inner routes or restored a bypass");
				require(world.removeFurniture(innerId, &diagnostic), diagnostic);
				require(world.getGraph()->calculatePath(&query, point("Entrance"), point("Exit")) != nullptr,
					"Reverse removal did not restore floor");
				continue;
			}
			// Moving/removing either piece rebuilds only the surviving authored network.
			require(world.editFurniture(innerId, 8, 0, "Moved", &diagnostic), diagnostic);
			core::Agent movedQuery("Moved query");
			require(bool(world.getGraph()->calculatePath(&movedQuery, point("Entrance"), point("Exit"))) == (variant != 2 && variant != 3),
				"Inner movement restored a bypass or lost remaining contributions");
			require(world.removeFurniture(innerId, &diagnostic), diagnostic);
			auto graph = world.getGraph(); core::Agent query("Query");
			require(bool(graph->calculatePath(&query, point("Entrance"), point("Exit")))
				== (variant != 3), "Removing inner restored a bypass through remaining outer");
			require(world.removeFurniture(outerId, &diagnostic), diagnostic);
			graph = world.getGraph();
			auto restored = graph->calculatePath(&query, point("Entrance"), point("Exit"));
			require(restored != nullptr, "Removing all replacements did not restore ordinary circulation");
			for (auto const& node : restored->nodes) if (node.edge)
				require(node.edge->getLocalDepth() == 0, "Deleted instance left a route contribution");
		}
	}

	void deskRoutes(smoke::Context const& context)
	{
		using smoke::require;
		auto fixture = context.fixture("resources/test-worlds/desk.furniture.yaml");
		auto catalogue = core::FurnitureCatalogue::load(fixture);
		int baselineArrival = 0;
		for (int depth : { 0, 2, 5 })
		{
			core::World world("Isolated desk", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("desk.furniture.yaml", catalogue);
			auto id = world.placeFurniture(room, "desk", 2.125f, 0, "Desk", depth);
			uint32_t leftId = 0, rightId = 0;
			world.addSectorMarker(room, 0, 0.5f, "Entrance", &leftId);
			world.addSectorMarker(room, 0, 6.5f, "Exit", &rightId);
			world.finishBuild();
			auto graph = world.getGraph(); core::Agent query("Query");
			auto left = graph->getVertexByIdentifier(leftId), right = graph->getVertexByIdentifier(rightId);
			auto path = graph->calculatePath(&query, left, right);
			require(path != nullptr, "Desk severed circulation");
			bool side = false;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getLength() == 1.5f)
				{
					side = true;
					require(node.edge->getLocalDepth() == depth || node.edge->getLocalDepth() == depth + 1,
						"Desk used an ordinary floor shortcut");
				}
			require(side, "Desk Path did not use an authored side edge");
			unsigned front = 0, back = 0;
			std::shared_ptr<const core::Vertex> seat, disconnected;
			for (auto const& v : graph->getVertices())
			{
				if (v->getPosition().x == 2.875f)
				{
					if (v->getEdges().empty()) disconnected = v;
					else seat = v;
				}
			}
			for (auto const& edge : graph->getEdges())
			{
				if (edge->getLength() == 1.5f)
				{
					front += edge->getLocalDepth() == depth;
					back += edge->getLocalDepth() == depth + 1;
				}
				else if (edge->getLength() == 0.25f) require(edge->getLocalDepth() == 0, "Unassigned approaches moved with instance depth");
			}
			require(front == 1 && back == 1 && seat && disconnected, "Explicit route layout/depth resolution was lost");
			require(!graph->calculatePath(&query, left, disconnected), "Coincidence invented internal connectivity");
			require(graph->calculatePath(&query, left, seat) && graph->calculatePath(&query, seat, right), "Seat cannot be reached/departed");
			for (auto const& node : path->nodes) require(node.targetVertex != seat, "Blocks pathing seat became a through-waypoint");
			require(world.getMarkerIds().size() == 3, "Routing-only vertices appeared as behaviour destinations");
			auto agentId = world.createAgent("Walker", room, 0, 0.5f);
			auto agent = world.lookupAgent(agentId).entity;
			agent->setPath(graph->calculatePath(agent, right), true);
			bool traversed = false;
			int arrival = 0;
			for (int tick = 0; tick < 1200; ++tick)
			{
				world.advanceTicks(1);
				if (!arrival && std::abs(agent->getGlobalPosition().x - 6.5f) < 0.0001f) arrival = tick + 1;
				if (agent->getGlobalPosition().x > 2.5f && agent->getGlobalPosition().x < 3.5f)
				{
					traversed = true;
					require(agent->getLocalDepth() == depth || agent->getLocalDepth() == depth + 1,
						"Active traversal did not adopt side edge depth");
					require(agent->getGlobalPosition().y == 0 && agent->getSector()->getLayerIndex() == 0,
						"Local depth changed physical geometry or Layer");
				}
			}
			require(traversed && std::abs(agent->getGlobalPosition().x - 6.5f) < 0.01f, "Desk traversal did not arrive: x=" + std::to_string(agent->getGlobalPosition().x)
				+ " state=" + std::to_string(static_cast<int>(agent->getState())) + " seen=" + std::to_string(traversed));
			if (depth == 0) baselineArrival = arrival;
			require(arrival == baselineArrival, "Local depth altered physical traversal duration");
			world.pauseSimulation();
			auto before = snapshot(world); std::string diagnostic;
			require(!world.editFurniture(id, 2.125f, 0, "Desk", &diagnostic, -1) && snapshot(world) == before,
				"Negative instance depth edit was not atomic");
			world.placeFurniture(room, "desk", 2.125f, 0, "Overlapping artwork", depth + 2);
			world.finishBuild(); world.pauseSimulation();
			before = snapshot(world);
			require(!world.editFurniture(id, 2.125f, 0, "Desk", &diagnostic, depth + 2) && snapshot(world) == before,
				"Same-depth footprint edit was not atomic");
		}
		// Removing a front/back edge cannot be repaired by coordinate coincidence.
		// Force each side independently, and also exercise signed relative offsets.
		for (int variant = 0; variant < 4; ++variant)
		{
			auto yaml = YAML::LoadFile(fixture.string());
			auto definition = yaml["furnitureCatalogue"]["definitions"][0];
			definition["edges"].remove(variant == 0 ? 1 : 4);
			if (variant == 2) definition["edges"][1]["depthOffset"] = -3;
			if (variant == 3)
			{
				definition["sideRoutes"] = false;
				definition["vertices"] = YAML::Load("[{key: left, x: 0, external: true}, {key: seat, x: 0.75, usablePoint: seat}]");
				definition["edges"] = YAML::Load("[{from: left, to: seat, depthOffset: 0}]");
			}
			auto filename = context.temporaryRoot() / ("side" + std::to_string(variant) + ".furniture.yaml");
			{ std::ofstream file(filename); file << yaml; }
			core::World world("One side", 8, 2); auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue(filename.filename().string(), core::FurnitureCatalogue::load(filename));
			std::string diagnostic;
			if (variant == 2)
			{
				auto before = snapshot(world);
				require(!world.canPlaceFurniture(room, "desk", 2, 0, "Desk", &diagnostic, 2)
					&& diagnostic.find("resolved edge") != std::string::npos && snapshot(world) == before,
					"Negative resolved edge depth accepted");
				continue;
			}
			world.placeFurniture(room, "desk", 2, 0, "Desk", 2);
			uint32_t a = 0, b = 0; world.addSectorMarker(room, 0, 0.5f, "Left", &a); world.addSectorMarker(room, 0, 6.5f, "Right", &b);
			world.finishBuild(); core::Agent query("Query");
			auto path = world.getGraph()->calculatePath(&query, world.getGraph()->getVertexByIdentifier(a), world.getGraph()->getVertexByIdentifier(b));
			require(path != nullptr, "Remaining explicit side was unusable");
			if (variant == 3)
			{
				for (auto const& node : path->nodes) if (node.edge)
					require(node.edge->getLocalDepth() == 0, "Definition without side routes lost ordinary floor routing");
				continue;
			}
			bool side = false;
			for (auto const& node : path->nodes) if (node.edge && node.edge->getLength() == 1.5f
				&& node.edge->getVertex(0)->getPosition().x >= 2)
			{ side = true; require(node.edge->getLocalDepth() == (variant == 0 ? 3 : 2), "Removed side or floor shortcut remained usable"); }
			require(side, "Selected Path avoided the remaining authored side");
		}
		core::World protectedWorld("Protected desk", 8, 2);
		auto frontRoom = protectedWorld.addRoom("Approach", 0, 0, 0, 8, 1);
		auto backRoom = protectedWorld.addRoom("Protected", 1, 0, 0, 8, 1);
		protectedWorld.addSectorDoor(0, 0, 0);
		protectedWorld.attachFurnitureCatalogue("desk.furniture.yaml", catalogue);
		protectedWorld.placeFurniture(backRoom, "desk", 2, 0, "Desk", 2);
		uint32_t destinationId = 0; protectedWorld.addSectorMarker(backRoom, 0, 6.5f, "Destination", &destinationId);
		protectedWorld.finishBuild(); protectedWorld.pauseSimulation();
		auto permission = protectedWorld.addAccessPermission("Room access"); std::string diagnostic;
		require(protectedWorld.setLocationPermissionRequirement(backRoom, { permission }, &diagnostic), diagnostic);
		auto agentId = protectedWorld.createAgent("Visitor", frontRoom, 0, 0.5f);
		auto agent = protectedWorld.lookupAgent(agentId).entity;
		auto destination = protectedWorld.getGraph()->getVertexByIdentifier(destinationId);
		require(!protectedWorld.getGraph()->calculatePath(agent, destination), "Desk routes bypassed Location permission requirements");
		require(protectedWorld.grantAgentAccessPermission(agentId, permission, &diagnostic), diagnostic);
		require(protectedWorld.getGraph()->calculatePath(agent, destination) != nullptr, "Authorized desk route unavailable");
		core::MobilityProfile mobility; mobility.set(core::TraversalKind::Door, core::MobilityUse::CannotUse);
		require(protectedWorld.setAgentIndividualMobilityProfile(agentId, mobility, &diagnostic), diagnostic);
		require(!protectedWorld.getGraph()->calculatePath(agent, destination), "Furniture depth bypassed Door Mobility constraints");
	}
}
void registerFurniture(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "furniture/demo", demonstration });
	checks.push_back({ "furniture/chair", chair });
	checks.push_back({ "furniture/layouts", layouts });
	checks.push_back({ "furniture/deskRoutes", deskRoutes });
	checks.push_back({ "furniture/attachments", attachments });
	checks.push_back({ "furniture/composition", composition });
}
