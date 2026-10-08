#include "core/Human.h"
#include "../support/CatalogueSource.h"
#include "WorldChecks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/MarkerSectorObject.h"
#include "core/SerializationException.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentBehaviourRegistry.h"
#include <fstream>
#include <limits>
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void demonstrationWorkflow(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot() / "demonstration";
		std::filesystem::create_directory(root);
		auto cataloguePath = root / "furniture-integration.furniture.lua";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/furniture-integration.furniture.lua"), cataloguePath);
		// Fresh authoring must not allocate any Marker for a circulation-only desk.
		core::World circulation("Desk without destinations", 8, 2);
		auto room = circulation.addRoom("Room", 0, 0, 0, 8, 1);
		circulation.attachFurnitureCatalogue("furniture-integration.furniture.lua", core::FurnitureCatalogue::load(cataloguePath));
		auto deskId = circulation.placeFurniture(room, "desk", 2, 0, "Desk", 2);
		circulation.finishBuild(); circulation.pauseSimulation();
		require(circulation.getMarkerIds().empty() && circulation.furniture().front().destinations.empty()
			&& !circulation.furniture().front().marker, "Desk placement allocated a usable Marker");
		for (auto extension : {"world.yaml", "world"})
		{
			auto output = root / (std::string("circulation.") + extension);
			circulation.saveTo(output.string());
			auto loaded = core::loadWorldDocument(output);
			require(loaded->furniture().front().id == deskId && loaded->getMarkerIds().empty()
				&& loaded->furniture().front().destinations.empty() && !loaded->furniture().front().marker,
				"Desk save/reopen invented destinations");
		}
		auto source = root / "demo.world.yaml";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/furniture-integration.world.yaml"), source);
		// This scenario relocates the World with its catalogue and revises the
		// catalogue beside it, so reference the adjacent file rather than the
		// shared manifest Resource.
		{
			auto document = YAML::LoadFile(source.string());
			document["furnitureCatalogue"]["resource"] = "furniture-integration.furniture.lua";
			std::ofstream output(source); output << document;
		}
		auto world = core::loadWorldDocument(source);
		require(world->furniture().size() == 6, "Required complete Furniture demonstration is missing instances");
		auto desk = world->furniture()[1];
		require(desk.destinations.empty() && !desk.marker,
			"Circulation-only desk created usable Markers");
		auto sofa = world->furniture()[2];
		auto walkerId = world->getSimulationSnapshot().agents.front().id;
		auto walker = world->lookupAgent(walkerId).entity;
		for (int tick = 0; tick < 600 && walker->getLocalDepth() != 2; ++tick) world->advanceTicks(1);
		require(walker->getLocalDepth() == 2, "Demo walker never traversed the composed front route");
		world->pauseSimulation();
		auto physical = walker->getGlobalPosition();
		std::string diagnostic;
		require(world->editFurniture(desk.id, 2.25f, 0, "Edited desk", &diagnostic), diagnostic);
		require(world->renameMarker(sofa.destinations[1].marker, "Chosen sofa destination", &diagnostic), diagnostic);
		world->applyLocationEdit(world->planResizeLocation(0, 0, 0, 12, 3));
		walker = world->lookupAgent(walkerId).entity;
		require(walker->getGlobalPosition() == physical && walker->getLocalDepth() == 2,
			"Combined overlapping edit/replay teleported walker or lost depth");
		// Target by public Marker identity after the live edit, never a stale vertex.
		auto vertexFor = [](core::World const& current, core::MarkerId id) {
			for (auto const& vertex : current.getGraph()->getVertices())
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getId() == id)
					return vertex;
			return std::shared_ptr<const core::Vertex>{};
		};
		auto target = sofa.destinations[1].marker;
		auto path = world->getGraph()->calculatePath(walker, vertexFor(*world, target));
		require(path != nullptr, "Edited overlapping catalogue routes lost the chosen destination");
		walker->setPath(path, true);
		for (auto extension : {"world.yaml", "world"}) world->saveTo((root / (std::string("authored.") + extension)).string());
		// Relocate the entire project, then revise the external definition. Both
		// formats must resolve current geometry and preserve independent names/IDs.
		auto portable = context.temporaryRoot() / "portable-demonstration";
		std::filesystem::rename(root, portable);
		auto catalogue = smoke::catalogueSource(portable / "furniture-integration.furniture.lua");
		smoke::writeCatalogue(portable / "furniture-integration.furniture.lua", catalogue + R"(
local definitions = catalogue.definitions
definitions[2].label = 'Revised sofa'
definitions[2].usablePoints[2].label = 'Revised right seat'
definitions[2].usablePoints[2].x = 1.625
for _,vertex in ipairs(definitions[2].vertices) do if vertex.key == 'rightSeat' then vertex.x = 1.625 end end
table.insert(definitions[1].usablePoints, {key='extra', label='Extra', x=0.75})
)");
		for (auto extension : {"world.yaml", "world"})
		{
			auto loaded = core::loadWorldDocument(portable / (std::string("authored.") + extension));
			require(loaded->furniture()[1].id == desk.id && loaded->furniture()[1].x == 2.25f
				&& loaded->furniture()[1].destinations.empty() && !loaded->furniture()[1].marker
				&& loaded->furniture()[2].destinations[1].marker == target
				&& loaded->lookupMarker(target)->getName() == "Chosen sofa destination"
				&& loaded->furniture()[0].destinations.size() == 2,
				"Portable reopen/reconciliation lost authored identities or current definitions");
			for (bool reset : {false, true})
			{
				if (reset) loaded->resetSimulation();
				auto visitor = loaded->lookupAgent(walkerId).entity;
				require(visitor->getPath() && visitor->getLocalDepth() == 2 && visitor->getGlobalPosition() == physical,
					"Reconciliation/Reset lost authored position, depth or intent");
				loaded->advanceTicks(1800);
				require(visitor->getGlobalPosition().x == 4.75f && visitor->getLocalDepth() == 2,
					"Restored intent failed to reach the revised overlapping destination");
			}
			// The same restored walker then crosses a real Sector boundary. Depth
			// is reset independently of the showroom's overlapping route numbers.
			auto visitor = loaded->lookupAgent(walkerId).entity;
			auto facadeTarget = loaded->furniture()[5].destinations[0].marker;
			auto crossing = loaded->getGraph()->calculatePath(visitor, vertexFor(*loaded, facadeTarget));
			require(crossing != nullptr, "Demonstration Facade destination is not reachable");
			visitor->setPath(crossing, true); loaded->advanceTicks(2400);
			require(visitor->getSector()->getIndex() == 2 && visitor->getLocalDepth() == 0,
				"Composed/reconciled journey failed Sector depth reset");
			loaded->pauseSimulation();
			auto placed = loaded->placeFurniture(1, "chair", 2.125f, 0, "New corridor chair");
			require(placed > desk.id, "Reopened place workflow reused an instance identity");
			loaded->saveTo((portable / (std::string("completed.") + extension)).string());
			require(core::loadWorldDocument(portable / (std::string("completed.") + extension))->furniture().back().id == placed,
				"Complete place/edit/target/save/reopen workflow lost placed instance");
		}
	}

	void furniture(smoke::Context const& context)
	{
		demonstrationWorkflow(context);
		using smoke::require;
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/chair.world.yaml"));
		require(demo->furniture().size() == 3 && demo->getMarkerIds().size() == 5, "Required chair demonstration World is incomplete");
		demo->advanceTicks(600);
		require(std::abs(demo->getSimulationSnapshot().agents.front().globalPosition.x - 2.75f) < 0.01f, "Demo Agent cannot reach chair");
		auto root = context.temporaryRoot();
		// Coincident destinations must restore by Marker identity, not nearest
		// coordinates. Replay preserves authored origin/depth independently of
		// the underway physical position and of the moved destination.
		{
			auto path = root / "movement.furniture.lua";
			std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/desk.furniture.lua"), path);
			core::World movement("Movement documents", 14, 2);
			auto sector = movement.addRoom("Room", 0, 0, 0, 14, 1);
			movement.attachFurnitureCatalogue("movement.furniture.lua", core::FurnitureCatalogue::load(path));
			auto first = movement.placeFurniture(sector, "desk", 2, 0, "First", 2);
			auto second = movement.placeFurniture(sector, "desk", 2, 0, "Second", 6);
			auto target = movement.furniture().back().marker;
			movement.finishBuild();
			auto id = movement.createAgent("Visitor", sector, 0, 0.5f);
			auto visitor = movement.lookupAgent(id).entity;
			auto findTarget = [&]() {
				for (auto const& vertex : movement.getGraph()->getVertices())
					if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
						marker && marker->getId() == target) return vertex;
				return std::shared_ptr<const core::Vertex>{};
			};
			visitor->setPath(movement.getGraph()->calculatePath(visitor, findTarget()), true);
			movement.advanceTicks(10);
			auto physical = visitor->getGlobalPosition();
			movement.pauseSimulation();
			std::string diagnostic;
			require(movement.editFurniture(first, 2, 0, "Unrelated", &diagnostic), diagnostic);
			visitor = movement.lookupAgent(id).entity;
			require(visitor->getGlobalPosition() == physical, "Replay teleported underway Agent");
			for (auto extension : { "world.yaml", "world" })
			{
				auto filename = root / (std::string("movement.") + extension);
				movement.saveTo(filename.string());
				auto loaded = core::loadWorldDocument(filename);
				auto restored = loaded->lookupAgent(id).entity;
				require(restored->getGlobalPosition().x == 0.5f && restored->getLocalDepth() == 0
					&& restored->getPath(), "Replay/save lost authored origin, depth or Path");
				auto marker = std::dynamic_pointer_cast<core::Marker>(restored->getPath()->nodes.back().targetVertex->getObject());
				require(marker && marker->getId() == target, "Coincident restored Path changed destination identity");
				loaded->advanceTicks(2000);
				require(restored->getGlobalPosition().x == 2.75f && restored->getLocalDepth() == 6,
					"Restored Path reached the wrong coincident Furniture depth");
				loaded->resetSimulation(); restored = loaded->lookupAgent(id).entity;
				marker = std::dynamic_pointer_cast<core::Marker>(restored->getPath()->nodes.back().targetVertex->getObject());
				require(marker && marker->getId() == target, "Reset changed coincident destination identity");
			}
			require(movement.editFurniture(second, 7, 0, "Moved target", &diagnostic, 8), diagnostic);
			for (auto extension : { "world.yaml", "world" })
			{
				auto filename = root / (std::string("moved.") + extension);
				movement.saveTo(filename.string());
				auto loaded = core::loadWorldDocument(filename);
				auto restored = loaded->lookupAgent(id).entity;
				require(restored->getGlobalPosition().x == 0.5f && restored->getPath(), "Moved saved Path lost origin/intent");
				loaded->advanceTicks(2000);
				require(restored->getGlobalPosition().x == 7.75f && restored->getLocalDepth() == 8,
					"Saved Marker intent did not follow moved Furniture");
			}
			// A saved Path is a document reference even without a behaviour
			// assignment, and even when inactive. Removing its usable-point key
			// must fail rather than idle the Agent or select a coincident new point.
			for (bool active : { false, true })
			{
				visitor = movement.lookupAgent(id).entity;
				auto route = movement.getGraph()->calculatePath(visitor, findTarget());
				require(route != nullptr, "Removed-point regression needs a saved Path");
				visitor->setPath(route, active);
				for (auto extension : { "world.yaml", "world" })
					movement.saveTo((root / (std::string(active ? "active." : "inactive.") + extension)).string());
			}
			smoke::writeCatalogue(path, smoke::catalogueSource(path) + R"(
local definition = catalogue.definitions[1]
definition.usablePoints[1].key = 'stool'
for _,vertex in ipairs(definition.vertices) do if vertex.usablePoint then vertex.usablePoint = 'stool' end end
)");
			for (auto prefix : { "active.", "inactive." })
				for (auto extension : { "world.yaml", "world" })
				{
					try { (void)core::loadWorldDocument(root / (std::string(prefix) + extension)); }
					catch (core::SerializationException const& error) {
						auto message = std::string(error.what());
						require(message.find("Furniture 'Moved target'") != std::string::npos
							&& message.find("definition 'desk'") != std::string::npos
							&& message.find("removed usable point 'seat'") != std::string::npos
							&& message.find("Agent 'Visitor'") != std::string::npos
							&& message.find("saved Path destination") != std::string::npos, message);
						continue;
					}
					require(false, "Removed saved Path destination did not fail loading");
				}
		}
		auto cataloguePath = root / "chair.furniture.lua";
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/chair.furniture.lua"), cataloguePath);
		auto world = std::make_shared<core::World>("Chair document", 8, 2);
		auto room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->attachFurnitureCatalogue("chair.furniture.lua", core::FurnitureCatalogue::load(cataloguePath));
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
				&& loaded->furnitureCatalogueResourceName() == "chair.furniture.lua", "World format lost Furniture reference or identities");
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
		// Furniture branch schema 47 predates the merged Dumbwaiter identity field.
		auto furnitureLegacy = YAML::LoadFile((root / "chair.world.yaml").string());
		furnitureLegacy["version"] = 47;
		furnitureLegacy.remove("nextDumbwaiterId");
		{ std::ofstream file(root / "legacy-furniture.world.yaml"); file << furnitureLegacy; }
		auto legacyFurniture = core::loadWorldDocument(root / "legacy-furniture.world.yaml");
		require(legacyFurniture->furniture().size() == 1 && legacyFurniture->furniture().front().id == id
			&& legacyFurniture->furniture().front().marker == marker,
			"Pre-merge Furniture schema lost instance or Marker identity");
		auto expectFailure = [&](std::string const& fragment) {
			try { (void)core::loadWorldDocument(root / "chair.world.yaml"); }
			catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); return; }
			require(false, "Incompatible Furniture dependency was silently accepted");
		};
		auto catalogue = smoke::catalogueSource(cataloguePath);
		auto saveCatalogue = [&] { smoke::writeCatalogue(cataloguePath, catalogue); };
		catalogue += "catalogue.definitions[1].label = 'Renamed chair'\n"
			"catalogue.definitions[1].usablePoints[1].label = 'Renamed seat'\n";
		saveCatalogue();
		auto revised = core::loadWorldDocument(root / "chair.world.yaml");
		require(revised->furnitureCatalogue()->definition("chair")->label == "Renamed chair"
			&& revised->lookupMarker(marker)->getName() == "Workstation", "Reload used an embedded snapshot or renamed an owned Marker");
		catalogue += "catalogue.uuid = 'e78a6c36-7902-4abc-9c90-1876058b32f4'\n";
		saveCatalogue(); expectFailure("UUID mismatch");
		catalogue += "catalogue.uuid = '" + world->furnitureCatalogue()->uuid() + "'\n"
			"catalogue.definitions[1].usablePoints[1].key = 'removed-seat'\n";
		saveCatalogue();
		auto replacedPoint = core::loadWorldDocument(root / "chair.world.yaml");
		require(!replacedPoint->lookupMarker(marker) && replacedPoint->furniture().front().marker.value > marker.value,
			"Removed point silently retargeted its identity to a new key");
		catalogue += "catalogue.definitions[1].usablePoints[1].key = 'seat'\n"
			"catalogue.definitions[1].key = 'removed-chair'\n";
		saveCatalogue(); expectFailure("Missing Furniture definition");
		std::filesystem::remove(cataloguePath); expectFailure("Missing Furniture catalogue");
		core::World legacy("No Furniture", 2, 1); legacy.addCorridor(0, 0, 2); legacy.finishBuild();
		legacy.saveTo((root / "legacy.world").string());
		require(!core::loadWorldDocument(root / "legacy.world")->furnitureCatalogue(), "Unfurnished World acquired a catalogue dependency");
		// Multi-tile layouts use the same document contract, preserving every point.
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/layouts.furniture.lua"), root / "layouts.furniture.lua");
		auto layouts = std::make_shared<core::World>("Layout documents", 16, 4);
		auto layoutRoom = layouts->addRoom("Room", 0, 0, 0, 16, 4);
		layouts->attachFurnitureCatalogue("layouts.furniture.lua", core::FurnitureCatalogue::load(root / "layouts.furniture.lua"));
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
		auto layoutCataloguePath = root / "layouts.furniture.lua";
		auto originalLayouts = smoke::catalogueSource(layoutCataloguePath);
		auto writeLayouts = [&](std::string const& value) { smoke::writeCatalogue(layoutCataloguePath, value); };
		writeLayouts(originalLayouts + R"(
local points = catalogue.definitions[1].usablePoints
points[1], points[2] = points[2], points[1]
points[2].label = 'Revised left seat'
)");
		auto reordered = core::loadWorldDocument(root / "layouts.world.yaml");
		require(reordered->furniture()[0].destinations[0].marker == sofaPoints[0].marker
			&& reordered->lookupMarker(sofaPoints[1].marker)->getName() == "Right destination", "Catalogue ordering retargeted point identities");
		// Reconcile new points and reordered definitions by stable keys, not ordinal.
		auto changed = originalLayouts + R"(
local definitions = catalogue.definitions
table.insert(definitions[1].usablePoints, {key='new', label='Right destination', x=1, y=0})
definitions[1].usablePoints[1].x = 0.5
definitions[1], definitions[2] = definitions[2], definitions[1]
)";
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
		changed += "table.remove(catalogue.definitions[2].usablePoints, 1)\n";
		writeLayouts(changed);
		for (auto filename : { "layouts.world.yaml", "layouts.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			require(!loaded->lookupMarker(sofaPoints[0].marker)
				&& loaded->furniture()[0].destinations[0].marker == sofaPoints[1].marker,
				"Unreferenced removal silently retargeted a destination");
		}
		auto failLayouts = [&](std::string const& value, std::string const& fragment) {
			writeLayouts(value);
			for (auto filename : { "layouts.world.yaml", "layouts.world" })
			{
				try { (void)core::loadWorldDocument(root / filename); }
				catch (std::exception const& error) { require(std::string(error.what()).find(fragment) != std::string::npos, error.what()); continue; }
				require(false, "Incompatible current layout was accepted");
			}
		};
		auto invalidLayout = originalLayouts + "table.insert(catalogue.definitions[1].tiles, {x=12, y=0, imageSet='ObjectAtlas', image='chair'})\n";
		failLayouts(invalidLayout, "inside one Location");
		invalidLayout = originalLayouts + "table.insert(catalogue.definitions[1].tiles, {x=7, y=0, imageSet='ObjectAtlas', image='chair'})\n";
		failLayouts(invalidLayout, "overlaps");
		invalidLayout = originalLayouts + "catalogue.definitions[1].usablePoints[1].x = 99\n";
		failLayouts(invalidLayout, "usable");
		writeLayouts(originalLayouts);
		// A width expansion across a Walkway gap is rejected, not omitted or moved.
		auto elevated = std::make_shared<core::World>("Elevated", 8, 3);
		auto elevatedRoom = elevated->addRoom("Room", 0, 0, 0, 8, 3);
		elevated->addSectorWalkway(elevatedRoom, 1, 1);
		elevated->addSectorWalkway(elevatedRoom, 1, 2);
		elevated->attachFurnitureCatalogue("layouts.furniture.lua", core::FurnitureCatalogue::load(layoutCataloguePath));
		elevated->placeFurniture(elevatedRoom, "sofa", 1, 1, "Elevated sofa");
		elevated->finishBuild();
		for (auto filename : { "elevated.world.yaml", "elevated.world" }) elevated->saveTo((root / filename).string());
		invalidLayout = originalLayouts + "table.insert(catalogue.definitions[1].tiles, {x=2, y=0, imageSet='ObjectAtlas', image='chair'})\n";
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
		changed = originalLayouts + "catalogue.definitions[1].usablePoints[1].label = 'New label'\n";
		writeLayouts(changed);
		for (auto filename : { "referenced.world.yaml", "referenced.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			auto assignment = loaded->getAgentBehaviourAssignment(behaviourVisitor);
			require(assignment && *core::agentBehaviourConfigurationGetIf<core::MarkerId>(&assignment->configuration.at("first_marker")) == sofaPoints[0].marker,
				"Label change lost a behaviour reference");
		}
		changed += "table.remove(catalogue.definitions[1].usablePoints, 1)\n";
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/desk.furniture.lua"), root / "desk.furniture.lua");
		auto desk = std::make_shared<core::World>("Desk documents", 8, 2);
		auto deskRoom = desk->addRoom("Room", 0, 0, 0, 8, 1);
		desk->attachFurnitureCatalogue("desk.furniture.lua", core::FurnitureCatalogue::load(root / "desk.furniture.lua"));
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
		smoke::writeCatalogue(root / "desk.furniture.lua", smoke::catalogueSource(root / "desk.furniture.lua")
			+ "catalogue.definitions[1].edges[2].depthOffset = -5\n");
		refuseDesk(deskYaml, "resolved edge depth");
		// Attachments are derived from current definitions in both supported formats.
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/attachments.furniture.lua"), root / "attachments.furniture.lua");
		auto arrangement = std::make_shared<core::World>("Attached documents", 8, 2);
		auto arrangementRoom = arrangement->addRoom("Room", 0, 0, 0, 8, 1);
		arrangement->attachFurnitureCatalogue("attachments.furniture.lua", core::FurnitureCatalogue::load(root / "attachments.furniture.lua"));
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
				core::Human query("Query");
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
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/composition.furniture.lua"), root / "composition.furniture.lua");
		for (float x : {3.f, 5.f, 6.f})
		{
			auto composed = std::make_shared<core::World>("Composed documents", 12, 2);
			auto host = composed->addRoom("Room", 0, 0, 0, 12, 1);
			composed->attachFurnitureCatalogue("composition.furniture.lua", core::FurnitureCatalogue::load(root / "composition.furniture.lua"));
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
					core::Human query("Query"); auto graph = loaded->getGraph();
					require(graph->calculatePath(&query, entrance, exit) && graph->calculatePath(&query, entrance, seat)
						&& !graph->calculatePath(&query, entrance, boundary), "Round trip/reset changed composed coverage or attachment");
					require(loaded->lookupMarker(owned)->getName() == "Composed destination", "Composed destination identity/name changed");
				}
			}
		}
		// Portable references survive moving the complete project directory.
		std::filesystem::copy_file(context.fixture("src/headless/smoke/fixtures/furniture/chair.furniture.lua"), cataloguePath);
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
