#include "WorldChecks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentBehaviourRegistry.h"
#include <fstream>
#include <limits>
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void furniture(smoke::Context const& context)
	{
		using smoke::require;
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/chair.world.yaml"));
		require(demo->furniture().size() == 3 && demo->getMarkerIds().size() == 5, "Required chair demonstration World is incomplete");
		demo->advanceTicks(600);
		require(std::abs(demo->getSimulationSnapshot().agents.front().globalPosition.x - 2.75f) < 0.01f, "Demo Agent cannot reach chair");
		auto root = context.temporaryRoot();
		auto cataloguePath = root / "chair.furniture.yaml";
		std::filesystem::copy_file(context.fixture("resources/test-worlds/chair.furniture.yaml"), cataloguePath);
		auto world = std::make_shared<core::World>("Chair document", 8, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->attachFurnitureCatalogue("chair.furniture.yaml", core::FurnitureCatalogue::load(cataloguePath));
		auto id = world->placeFurniture(room, "chair", 1.25f, 0, "Desk chair");
		auto marker = world->furniture().front().marker;
		world->renameMarker(marker, "Workstation"); world->finishBuild(); world->pauseSimulation();
		// Unrelated structural reconstruction carries the owned layout/identity.
		world->applyLocationEdit(world->planResizeLocation(room, 0, 0, 8, 2));
		require(world->furniture().front().id == id && world->lookupMarker(marker)->getName() == "Workstation", "Replay lost chair identities");
		std::string diagnostic;
		require(world->editFurniture(id, 2.25f, 0, "Renamed desk chair", &diagnostic), diagnostic);
		for (auto filename : { "chair.world.yaml", "chair.world" })
		{
			world->saveTo((root / filename).string());
			auto loaded = core::loadWorldDocument(root / filename);
			require(loaded->furniture().size() == 1 && loaded->furniture().front().id == id
				&& loaded->furniture().front().x == 2.25f && loaded->furniture().front().name == "Renamed desk chair"
				&& loaded->furniture().front().marker == marker
				&& loaded->lookupMarker(marker)->getName() == "Workstation"
				&& loaded->furnitureCatalogueFilename() == "chair.furniture.yaml", "World format lost Furniture reference or identities");
			loaded->resetSimulation();
			require(loaded->furniture().front().marker == marker, "Reset lost chair Marker identity");
			loaded->pauseSimulation();
			require(loaded->removeFurniture(id, &diagnostic), diagnostic);
			loaded->saveTo((root / "deleted.world").string());
			loaded = core::loadWorldDocument(root / "deleted.world");
			require(loaded->furniture().empty() && !loaded->lookupMarker(marker), "Deleted destination returned on reopen");
			loaded->pauseSimulation();
			auto nextId = loaded->placeFurniture(room, "chair", 4, 0, "New chair");
			require(nextId > id && loaded->furniture().back().marker.value > marker.value, "Reopening reused Furniture or Marker identity");
		}
		auto expectFailure = [&](std::string const& fragment) {
			try { (void)core::loadWorldDocument(root / "chair.world.yaml"); }
			catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); return; }
			require(false, "Incompatible Furniture dependency was silently accepted");
		};
		auto catalogue = YAML::LoadFile(cataloguePath.string());
		auto saveCatalogue = [&] { std::ofstream file(cataloguePath); file << catalogue; };
		catalogue["furnitureCatalogue"]["definitions"][0]["label"] = "Renamed chair";
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["label"] = "Renamed seat";
		saveCatalogue();
		auto revised = core::loadWorldDocument(root / "chair.world.yaml");
		require(revised->furnitureCatalogue()->definition("chair")->label == "Renamed chair"
			&& revised->lookupMarker(marker)->getName() == "Workstation", "Reload used an embedded snapshot or renamed an owned Marker");
		catalogue["furnitureCatalogue"]["uuid"] = "e78a6c36-7902-4abc-9c90-1876058b32f4"; saveCatalogue(); expectFailure("UUID mismatch");
		catalogue["furnitureCatalogue"]["uuid"] = world->furnitureCatalogue()->uuid();
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["key"] = "removed-seat"; saveCatalogue();
		auto replacedPoint = core::loadWorldDocument(root / "chair.world.yaml");
		require(!replacedPoint->lookupMarker(marker) && replacedPoint->furniture().front().marker.value > marker.value,
			"Removed point silently retargeted its identity to a new key");
		catalogue["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["key"] = "seat";
		catalogue["furnitureCatalogue"]["definitions"][0]["key"] = "removed-chair"; saveCatalogue(); expectFailure("Missing Furniture definition");
		std::filesystem::remove(cataloguePath); expectFailure("Missing Furniture catalogue");
		core::World legacy("No Furniture", 2, 1); legacy.addCorridor(0, 0, 2); legacy.finishBuild();
		legacy.saveTo((root / "legacy.world").string());
		require(!core::loadWorldDocument(root / "legacy.world")->furnitureCatalogue(), "Unfurnished World acquired a catalogue dependency");
		// Multi-tile layouts use the same document contract, preserving every point.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/layouts.furniture.yaml"), root / "layouts.furniture.yaml");
		auto layouts = std::make_shared<core::World>("Layout documents", 16, 4);
		auto layoutRoom = layouts->addRoom("Room", 0, 0, 0, 16, 4);
		layouts->attachFurnitureCatalogue("layouts.furniture.yaml", core::FurnitureCatalogue::load(root / "layouts.furniture.yaml"));
		auto sofa = layouts->placeFurniture(layoutRoom, "sofa", 1.125f, 0, "Sofa");
		auto larger = layouts->placeFurniture(layoutRoom, "larger", 8.25f, 0, "Sparse");
		auto sofaPoints = layouts->furniture()[0].destinations;
		auto largerPoints = layouts->furniture()[1].destinations;
		layouts->renameMarker(sofaPoints[1].marker, "Right destination");
		layouts->renameMarker(largerPoints[2].marker, "Third destination");
		layouts->setMarkerProperties(largerPoints[1].marker, 0);
		layouts->addSectorMarker(layoutRoom, 0, 15.f, "Moved sofa Right destination");
		auto later = layouts->addSectorMarker(layoutRoom, 0, 15.5f, "Later standalone Marker");
		auto retiredMarker = layouts->getMarkerIds().back();
		layouts->finishBuild(); layouts->pauseSimulation();
		require(layouts->removeSectorMarker(layoutRoom, later.index, &diagnostic), diagnostic);
		require(layouts->editFurniture(sofa, 3.375f, 0, "Moved sofa", &diagnostic), diagnostic);
		require(layouts->editFurniture(larger, 10.625f, 0, "Moved larger", &diagnostic), diagnostic);
		layouts->applyLocationEdit(layouts->planResizeLocation(layoutRoom, 0, 0, 16, 3));
		for (auto filename : { "layouts.world.yaml", "layouts.world" })
		{
			layouts->saveTo((root / filename).string());
			auto loaded = core::loadWorldDocument(root / filename);
			require(loaded->furniture()[0].x == 3.375f && loaded->furniture()[1].x == 10.625f, "Round trip lost fractional layout position");
			for (size_t i = 0; i < sofaPoints.size(); ++i)
				require(loaded->furniture()[0].destinations[i].marker == sofaPoints[i].marker, "Sofa point identity changed");
			for (size_t i = 0; i < largerPoints.size(); ++i)
				require(loaded->furniture()[1].destinations[i].marker == largerPoints[i].marker, "Larger point identity changed");
			require(loaded->lookupMarker(sofaPoints[1].marker)->getName() == "Right destination"
				&& loaded->lookupMarker(largerPoints[2].marker)->getName() == "Third destination"
				&& !loaded->lookupMarker(largerPoints[1].marker)->hasProperty(core::MarkerProperty::BlocksPathing)
				&& loaded->lookupMarker(largerPoints[0].marker)->hasProperty(core::MarkerProperty::BlocksPathing),
				"Round trip lost independent names/properties");
			for (auto const& instance : loaded->furniture())
				for (auto const& point : instance.destinations)
				{
					auto const& definition = *loaded->furnitureCatalogue()->definition(instance.definitionKey);
					auto authored = std::find_if(definition.usablePoints.begin(), definition.usablePoints.end(),
						[&](auto const& p) { return p.key == point.key; });
					require(loaded->lookupMarker(point.marker)->getCellX() + loaded->lookupMarker(point.marker)->getOffset() == instance.x + authored->x
						&& loaded->lookupMarker(point.marker)->getCellY() == instance.y,
						"Round trip changed a fractional horizontal offset or floor height");
				}
			loaded->pauseSimulation();
			require(loaded->removeFurniture(sofa, &diagnostic), diagnostic);
			for (auto const& p : sofaPoints) require(!loaded->lookupMarker(p.marker), "Multi-point deletion left an orphan");
			loaded->saveTo((root / "deleted-layout.world").string());
			loaded = core::loadWorldDocument(root / "deleted-layout.world");
			require(loaded->furniture().size() == 1 && loaded->furniture()[0].destinations.size() == 3, "Deletion/replay damaged unrelated layout slots");
		}
		// Definition/point order is not identity; current fractional offsets are resolved by key.
		auto layoutCataloguePath = root / "layouts.furniture.yaml";
		auto layoutCatalogue = YAML::LoadFile(layoutCataloguePath.string());
		auto definitions = layoutCatalogue["furnitureCatalogue"]["definitions"];
		auto firstPoint = YAML::Clone(definitions[0]["usablePoints"][0]);
		definitions[0]["usablePoints"][0] = YAML::Clone(definitions[0]["usablePoints"][1]);
		definitions[0]["usablePoints"][1] = firstPoint;
		definitions[0]["usablePoints"][1]["label"] = "Revised left seat";
		{ std::ofstream file(layoutCataloguePath); file << layoutCatalogue; }
		auto reordered = core::loadWorldDocument(root / "layouts.world.yaml");
		require(reordered->furniture()[0].destinations[0].marker == sofaPoints[0].marker
			&& reordered->lookupMarker(sofaPoints[1].marker)->getName() == "Right destination", "Catalogue ordering retargeted point identities");
		// Compatible edits reconcile by key in both formats. New points allocate
		// beyond even deleted standalone identities and use valid unique names.
		auto originalLayouts = YAML::LoadFile(context.fixture("resources/test-worlds/layouts.furniture.yaml").string());
		auto writeLayouts = [&](YAML::Node const& value) { std::ofstream file(layoutCataloguePath); file << value; };
		auto changed = YAML::Clone(originalLayouts);
		auto added = YAML::Load("{key: new, label: Right destination, x: 1, y: 0}");
		changed["furnitureCatalogue"]["definitions"][0]["usablePoints"].push_back(added);
		changed["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["x"] = 0.5f;
		// Also reorder definitions, independently of usable-point order.
		auto sofaDefinition = YAML::Clone(changed["furnitureCatalogue"]["definitions"][0]);
		changed["furnitureCatalogue"]["definitions"][0] = YAML::Clone(changed["furnitureCatalogue"]["definitions"][1]);
		changed["furnitureCatalogue"]["definitions"][1] = sofaDefinition;
		writeLayouts(changed);
		core::MarkerId newPoint;
		for (auto filename : { "layouts.world.yaml", "layouts.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			auto const& points = loaded->furniture()[0].destinations;
			require(points.size() == 3 && points[0].marker == sofaPoints[0].marker
				&& points[1].marker == sofaPoints[1].marker && points[2].marker.value > retiredMarker.value,
				"Compatible current layout lost stable identities or reused an issued identity");
			if (newPoint) require(newPoint == points[2].marker, "Binary/YAML new identities differ");
			newPoint = points[2].marker;
			require(loaded->lookupMarker(newPoint)->getName() != "Moved sofa Right destination"
				&& core::Marker::nameIsValid(loaded->lookupMarker(newPoint)->getName(), nullptr),
				"New point did not resolve its generated-name conflict");
			require(loaded->lookupMarker(sofaPoints[1].marker)->getName() == "Right destination"
				&& loaded->lookupMarker(sofaPoints[0].marker)->getOffset() == 0.875f,
				"Current offset or independently authored name was not adopted");
			loaded->saveTo((root / "reconciled.world").string());
			auto reopened = core::loadWorldDocument(root / "reconciled.world");
			reopened->resetSimulation();
			require(reopened->furniture()[0].destinations[2].marker == newPoint,
				"Save/reopen/Reset allocated a second identity for the new point");
		}
		require(layouts->furniture()[0].destinations.size() == 2,
			"Editing the external catalogue live-reloaded an already open World");
		// Allocation exhaustion stays explicit; the final uint64 identity is valid
		// and advances to the exhausted (zero) mark without wrapping to one.
		auto allocationDocument = YAML::LoadFile((root / "layouts.world.yaml").string());
		allocationDocument["nextMarkerId"] = std::numeric_limits<uint64_t>::max();
		auto allocationPath = root / "allocation.world.yaml";
		{ std::ofstream file(allocationPath); file << allocationDocument; }
		auto finalIdentity = core::loadWorldDocument(allocationPath);
		require(finalIdentity->furniture()[0].destinations.back().marker.value == std::numeric_limits<uint64_t>::max(),
			"Final available Marker identity was refused or wrapped");
		allocationDocument["nextMarkerId"] = 0;
		{ std::ofstream file(allocationPath); file << allocationDocument; }
		try { (void)core::loadWorldDocument(allocationPath); require(false, "Exhausted allocator accepted a new point"); }
		catch (std::exception const& error) {
			require(std::string(error.what()).find("identity space is exhausted") != std::string::npos, error.what());
		}
		// Remove a point and add another in the same edit: never reuse by ordinal.
		changed["furnitureCatalogue"]["definitions"][1]["usablePoints"].remove(0);
		writeLayouts(changed);
		for (auto filename : { "layouts.world.yaml", "layouts.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			require(!loaded->lookupMarker(sofaPoints[0].marker)
				&& loaded->furniture()[0].destinations[0].marker == sofaPoints[1].marker,
				"Unreferenced removal silently retargeted a destination");
		}
		auto failLayouts = [&](YAML::Node const& value, std::string const& fragment) {
			writeLayouts(value);
			for (auto filename : { "layouts.world.yaml", "layouts.world" })
			{
				try { (void)core::loadWorldDocument(root / filename); }
				catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); continue; }
				require(false, "Incompatible current layout was accepted");
			}
		};
		auto invalidLayout = YAML::Clone(originalLayouts);
		invalidLayout["furnitureCatalogue"]["definitions"][0]["tiles"].push_back(
			YAML::Load("{x: 12, y: 0, imageSet: ObjectAtlas, image: chair}"));
		failLayouts(invalidLayout, "inside one Location");
		invalidLayout = YAML::Clone(originalLayouts);
		invalidLayout["furnitureCatalogue"]["definitions"][0]["tiles"].push_back(
			YAML::Load("{x: 7, y: 0, imageSet: ObjectAtlas, image: chair}"));
		failLayouts(invalidLayout, "overlaps");
		invalidLayout = YAML::Clone(originalLayouts);
		invalidLayout["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["x"] = 99;
		failLayouts(invalidLayout, "usable");
		writeLayouts(originalLayouts);
		// A width expansion across a Walkway gap is rejected, not omitted or moved.
		auto elevated = std::make_shared<core::World>("Elevated", 8, 3);
		auto elevatedRoom = elevated->addRoom("Room", 0, 0, 0, 8, 3);
		elevated->addSectorWalkway(elevatedRoom, 1, 1);
		elevated->addSectorWalkway(elevatedRoom, 1, 2);
		elevated->attachFurnitureCatalogue("layouts.furniture.yaml", core::FurnitureCatalogue::load(layoutCataloguePath));
		elevated->placeFurniture(elevatedRoom, "sofa", 1, 1, "Elevated sofa");
		elevated->finishBuild();
		for (auto filename : { "elevated.world.yaml", "elevated.world" }) elevated->saveTo((root / filename).string());
		invalidLayout = YAML::Clone(originalLayouts);
		invalidLayout["furnitureCatalogue"]["definitions"][0]["tiles"].push_back(
			YAML::Load("{x: 2, y: 0, imageSet: ObjectAtlas, image: chair}"));
		writeLayouts(invalidLayout);
		for (auto filename : { "elevated.world.yaml", "elevated.world" })
		{
			try { (void)core::loadWorldDocument(root / filename); }
			catch (std::exception const& error) {
				require(std::string(error.what()).find("continuous Floor or Walkway support") != std::string::npos, error.what()); continue;
			}
			require(false, "Current width silently crossed a support gap");
		}
		writeLayouts(originalLayouts);
		// Behaviour references survive label edits; referenced removals fail with
		// the Agent, configuration field, definition and point identified.
		auto package = root / "patrol.behaviours";
		std::filesystem::copy(context.fixture("resources/test-worlds/door-test-1.behaviours/behaviours.yaml").parent_path(), package,
			std::filesystem::copy_options::recursive);
		auto registry = core::AgentBehaviourRegistry::loadFrom((package / "behaviours.yaml").string());
		auto behaviourVisitor = layouts->createAgent("Catalogue visitor", layoutRoom, 0, 0.5f);
		layouts->attachAgentBehaviourRegistry("patrol.behaviours", registry);
		require(layouts->setAgentBehaviourAssignment(behaviourVisitor, core::AgentBehaviourId{1}, 1,
			{{"first_marker", sofaPoints[0].marker}, {"second_marker", sofaPoints[1].marker}}, &diagnostic), diagnostic);
		for (auto filename : { "referenced.world.yaml", "referenced.world" }) layouts->saveTo((root / filename).string());
		changed = YAML::Clone(originalLayouts);
		changed["furnitureCatalogue"]["definitions"][0]["usablePoints"][0]["label"] = "New label";
		writeLayouts(changed);
		for (auto filename : { "referenced.world.yaml", "referenced.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			auto assignment = loaded->getAgentBehaviourAssignment(behaviourVisitor);
			require(assignment && *core::agentBehaviourConfigurationGetIf<core::MarkerId>(&assignment->configuration.at("first_marker")) == sofaPoints[0].marker,
				"Label change lost a behaviour reference");
		}
		changed["furnitureCatalogue"]["definitions"][0]["usablePoints"].remove(0);
		writeLayouts(changed);
		for (auto filename : { "referenced.world.yaml", "referenced.world" })
		{
			try { (void)core::loadWorldDocument(root / filename); }
			catch (std::exception const& error) {
				auto message = std::string(error.what());
				require(message.find("Catalogue visitor") != std::string::npos && message.find("first_marker") != std::string::npos
					&& message.find("sofa") != std::string::npos && message.find("left") != std::string::npos, message);
				continue;
			}
			require(false, "Referenced removal did not fail loading");
		}
		writeLayouts(originalLayouts);
		// Schema-43 chair fixtures still load with default Local depth 0.
		require(core::loadWorldDocument(context.fixture("resources/test-worlds/chair.world.yaml"))->furniture().size() == 3,
			"Schema-43 chair compatibility was lost");
		auto deskDemo = core::loadWorldDocument(context.fixture("resources/test-worlds/desk.world.yaml"));
		require(deskDemo->furniture().size() == 1 && deskDemo->furniture().front().localDepth == 2,
			"Required isolated desk demonstration is incomplete");
		deskDemo->advanceTicks(1200);
		require(std::abs(deskDemo->getSimulationSnapshot().agents.front().globalPosition.x - 6.5f) < 0.01f,
			"Demonstration Agent did not traverse the desk");
		// Both document representations resolve desk edge offsets from the saved instance depth.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/desk.furniture.yaml"), root / "desk.furniture.yaml");
		auto desk = std::make_shared<core::World>("Desk documents", 8, 2);
		auto deskRoom = desk->addRoom("Room", 0, 0, 0, 8, 1);
		desk->attachFurnitureCatalogue("desk.furniture.yaml", core::FurnitureCatalogue::load(root / "desk.furniture.yaml"));
		auto deskId = desk->placeFurniture(deskRoom, "desk", 2.125f, 0, "Desk", 2);
		auto deskSeat = desk->furniture().front().marker;
		desk->finishBuild(); desk->pauseSimulation();
		require(desk->editFurniture(deskId, 2.125f, 0, "Desk", &diagnostic, 4), diagnostic);
		desk->placeFurniture(deskRoom, "desk", 2.125f, 0, "Overlapping artwork", 6);
		desk->finishBuild(); desk->pauseSimulation();
		for (auto filename : { "desk.world.yaml", "desk.world" })
		{
			desk->saveTo((root / filename).string());
			auto loaded = core::loadWorldDocument(root / filename);
			require(loaded->furniture().front().localDepth == 4 && loaded->furniture().front().marker == deskSeat,
				"Desk depth/identity lost on document round trip");
			bool front = false, back = false;
			for (auto const& edge : loaded->getGraph()->getEdges()) if (edge->getLength() == 1.5f)
			{ front |= edge->getLocalDepth() == 4; back |= edge->getLocalDepth() == 5; }
			require(front && back, "Loaded desk did not resolve current relative route depths");
			loaded->resetSimulation();
			require(loaded->furniture().front().localDepth == 4, "Reset lost authored Local depth");
		}
		auto deskYaml = YAML::LoadFile((root / "desk.world.yaml").string());
		auto refuseDesk = [&](YAML::Node const& invalid, std::string const& fragment) {
			auto filename = root / "invalid-desk.world.yaml";
			{ std::ofstream file(filename); file << invalid; }
			try { (void)core::loadWorldDocument(filename); }
			catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); return; }
			require(false, "Invalid saved desk depth was accepted");
		};
		auto invalidDesk = YAML::Clone(deskYaml);
		// Construction records are the shared YAML/binary authority.
		for (auto record : invalidDesk["construction"])
			if (record["type"].as<std::string>() == "furniture") record["localDepth"] = -1;
		refuseDesk(invalidDesk, "non-negative");
		invalidDesk = YAML::Clone(deskYaml);
		for (auto record : invalidDesk["construction"])
			if (record["type"].as<std::string>() == "furniture") record["localDepth"] = 4;
		refuseDesk(invalidDesk, "overlaps");
		auto revisedDesk = YAML::LoadFile((root / "desk.furniture.yaml").string());
		revisedDesk["furnitureCatalogue"]["definitions"][0]["edges"][1]["depthOffset"] = -5;
		{ std::ofstream file(root / "desk.furniture.yaml"); file << revisedDesk; }
		refuseDesk(deskYaml, "resolved edge depth");
		// Attachments are derived from current definitions in both supported formats.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/attachments.furniture.yaml"), root / "attachments.furniture.yaml");
		auto arrangement = std::make_shared<core::World>("Attached documents", 8, 2);
		auto arrangementRoom = arrangement->addRoom("Room", 0, 0, 0, 8, 1);
		arrangement->attachFurnitureCatalogue("attachments.furniture.yaml", core::FurnitureCatalogue::load(root / "attachments.furniture.yaml"));
		arrangement->placeFurniture(arrangementRoom, "desk", 2.125f, 0, "Desk", 2);
		arrangement->placeFurniture(arrangementRoom, "chair", 3.125f, 0, "Chair", 1);
		auto attachedMarker = arrangement->furniture().back().marker;
		arrangement->addSectorMarker(arrangementRoom, 0, 0.5f, "Entrance");
		arrangement->finishBuild(); arrangement->pauseSimulation();
		require(arrangement->renameMarker(attachedMarker, "Attached destination", &diagnostic), diagnostic);
		for (auto filename : { "attachment.world.yaml", "attachment.world" })
		{
			arrangement->saveTo((root / filename).string());
			auto loaded = core::loadWorldDocument(root / filename);
			for (bool reset : { false, true })
			{
				if (reset) loaded->resetSimulation();
				require(loaded->furniture().back().marker == attachedMarker
					&& loaded->lookupMarker(attachedMarker)->getName() == "Attached destination"
					&& loaded->lookupMarker(attachedMarker)->hasProperty(core::MarkerProperty::BlocksPathing),
					"Attachment replay lost Marker identity/name/properties");
				std::shared_ptr<const core::Vertex> seat, approach;
				for (uint32_t i = 0; i < loaded->getSector(arrangementRoom)->getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(loaded->getSector(arrangementRoom)->getObject(i));
						object)
					{
						if (object->getMarker()->getId() == attachedMarker) seat = loaded->getGraph()->getVertexForObject(object);
						if (object->getMarker()->getName() == "Entrance") approach = loaded->getGraph()->getVertexForObject(object);
					}
				core::Agent query("Query");
				require(seat && loaded->getGraph()->calculatePath(&query,
					approach, seat), "Document replay lost route attachment");
				auto visitor = loaded->lookupAgent(loaded->createAgent("Visitor", arrangementRoom, 0, 0.5f)).entity;
				visitor->setPath(loaded->getGraph()->calculatePath(visitor, seat), true);
				loaded->advanceTicks(900);
				require(visitor->getGlobalPosition().x == 3.625f && visitor->getLocalDepth() == 2,
					"Reopened attachment failed physical arrival");
			}
		}
		// Composed replacement coverage is derived, never serialized as graph state.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/composition.furniture.yaml"), root / "composition.furniture.yaml");
		for (float x : {3.f, 5.f, 6.f})
		{
			auto composed = std::make_shared<core::World>("Composed documents", 12, 2);
			auto host = composed->addRoom("Room", 0, 0, 0, 12, 1);
			composed->attachFurnitureCatalogue("composition.furniture.yaml", core::FurnitureCatalogue::load(root / "composition.furniture.yaml"));
			composed->placeFurniture(host, "inner", x, 0, "Inner", 3);
			composed->placeFurniture(host, "outer", 2, 0, "Outer", 2);
			composed->addSectorMarker(host, 0, 0.5f, "Entrance");
			composed->addSectorMarker(host, 0, 10.5f, "Exit");
			composed->addSectorMarker(host, 0, x, "Internal boundary");
			composed->finishBuild(); composed->pauseSimulation();
			auto owned = composed->furniture().front().marker;
			require(composed->renameMarker(owned, "Composed destination", &diagnostic), diagnostic);
			for (auto filename : {"composition.world.yaml", "composition.world"})
			{
				composed->saveTo((root / filename).string());
				auto loaded = core::loadWorldDocument(root / filename);
				for (bool reset : {false, true})
				{
					if (reset) loaded->resetSimulation();
					std::shared_ptr<const core::Vertex> entrance, exit, boundary, seat;
					for (uint32_t i = 0; i < loaded->getSector(host)->getNumObjects(); ++i)
						if (auto object = std::dynamic_pointer_cast<core::MarkerSectorObject>(loaded->getSector(host)->getObject(i)); object)
						{
							auto vertex = loaded->getGraph()->getVertexForObject(object);
							auto name = object->getMarker()->getName();
							if (name == "Entrance") entrance = vertex;
							if (name == "Exit") exit = vertex;
							if (name == "Internal boundary") boundary = vertex;
							if (object->getMarker()->getId() == owned) seat = vertex;
						}
					core::Agent query("Query"); auto graph = loaded->getGraph();
					require(graph->calculatePath(&query, entrance, exit) && graph->calculatePath(&query, entrance, seat)
						&& !graph->calculatePath(&query, entrance, boundary), "Round trip/reset changed composed coverage or attachment");
					require(loaded->lookupMarker(owned)->getName() == "Composed destination", "Composed destination identity/name changed");
				}
			}
		}
		// Portable references survive moving the complete project directory.
		std::filesystem::copy_file(context.fixture("resources/test-worlds/chair.furniture.yaml"), cataloguePath);
		auto moved = root / "moved"; std::filesystem::create_directory(moved);
		std::filesystem::rename(cataloguePath, moved / cataloguePath.filename());
		for (auto filename : { "chair.world.yaml", "chair.world" })
		{
			std::filesystem::rename(root / filename, moved / filename);
			require(core::loadWorldDocument(moved / filename)->furniture().front().marker == marker,
				"Project relocation broke relative Furniture reference");
		}
	}
}
