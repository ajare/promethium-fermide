#include "../support/CatalogueSource.h"
#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/MarkerSectorObject.h"
#include "core/YamlSerializer.h"
#include "core/AgentTagRegistryDocument.h"
#include <limits>
#include "core/Exceptions.h"
#include "core/SerializationException.h"
#include "core/WorldDocument.h"
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

	void catalogResourceResolution(smoke::Context const&)
	{
		using smoke::require;
		// Composite catalogue Resources (those that also declare dependent
		// resources, such as a Furniture catalogue's Artwork dependency) must
		// resolve to their manifest `location` file, never the manifest directory.
		struct { std::string name; std::string filename; } const furniture[] = {
			{ "ChairCatalogue", "chair.furniture.lua" },
			{ "DeskCatalogue", "desk.furniture.lua" },
			{ "FurnitureCatalogue", "furniture.furniture.lua" },
			{ "FurnitureIntegrationCatalogue", "furniture-integration.furniture.lua" },
		};
		for (auto const& entry : furniture)
		{
			auto source = core::resolveCatalogSource("FurnitureCatalogue", entry.name);
			require(!source.empty(), "Manifest Furniture catalogue did not resolve: " + entry.name);
			require(source.filename().string() == entry.filename
				&& std::filesystem::is_regular_file(source),
				"Composite Furniture catalogue resolved to the wrong source: " + source.string());
		}
		auto tags = core::resolveCatalogSource("AgentTagRegistry", "TestAgentTags");
		require(!tags.empty() && tags.filename().string() == "test.tags.yaml"
			&& std::filesystem::is_regular_file(tags),
			"Tag registry Resource resolved to the wrong source: " + tags.string());
		auto behaviours = core::resolveCatalogSource("AgentBehaviourRegistry", "DoorTestBehaviours");
		require(!behaviours.empty() && behaviours.filename().string() == "door-test-1.behaviours"
			&& std::filesystem::is_directory(behaviours),
			"Behaviour package Resource resolved to the wrong source: " + behaviours.string());
	}

	void bundledLua(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot() / "bundled-lua";
		std::filesystem::create_directories(root);
		auto manifest = YAML::LoadFile(context.fixture("resources/Resources.yaml").string());
		unsigned registered = 0;
		for (auto resource : manifest["Resources"]["Resource"])
			if (resource["type"].as<std::string>() == "FurnitureCatalogue")
			{
				auto location = resource["location"].as<std::string>();
				require(location.ends_with(".furniture.lua") && core::FurnitureCatalogue::load(context.fixture("resources/" + location)),
					"Manifest retains an invalid/old-format bundled Furniture reference");
				++registered;
			}
		require(registered == 4, "Bundled catalogue registration inventory changed");
		auto sameInstances = [&](core::World const& expected, core::World const& actual) {
			require(expected.furnitureCatalogue()->uuid() == actual.furnitureCatalogue()->uuid()
				&& expected.furniture().size() == actual.furniture().size(), "Bundled catalogue/instance identities changed");
			for (size_t i = 0; i < expected.furniture().size(); ++i)
			{
				auto const& a = expected.furniture()[i]; auto const& b = actual.furniture()[i];
				require(a.id == b.id && a.sector == b.sector && a.definitionKey == b.definitionKey && a.name == b.name
					&& a.x == b.x && a.y == b.y && a.localDepth == b.localDepth && a.marker == b.marker
					&& a.destinations.size() == b.destinations.size(), "Bundled placement or identity changed");
				for (size_t j = 0; j < a.destinations.size(); ++j)
				{
					auto const& x = a.destinations[j]; auto const& y = b.destinations[j];
					require(x.key == y.key && x.marker == y.marker && x.name == y.name && x.properties == y.properties,
						"Bundled owned Marker identity/name/properties changed");
				}
			}
		};
		for (auto name : {"chair", "desk", "layouts", "attachments", "composition", "furniture", "furniture-integration"})
		{
			auto filename = std::string(name) + ".furniture.lua";
			auto package = root / filename;
			std::filesystem::copy_file(context.fixture("resources/test-worlds/" + filename), package);
			auto catalogue = core::FurnitureCatalogue::load(package);
			auto legacy = core::FurnitureCatalogue::readFile(context.fixture(
				"src/headless/smoke/fixtures/furniture/" + std::string(name) + ".furniture.lua"));
			require(catalogue->uuid() == legacy->uuid() && catalogue->definitions().size() == legacy->definitions().size(),
				"Bundled conversion changed UUID/definition keys");
			for (auto const& [key, definition] : catalogue->definitions())
			{
				auto original = *legacy->definition(key);
				// Teaching catalogues add use to geometry-only regression seating.
				original.hasUse = key == "chair" || key == "sofa" || key == "bed";
				require(original == definition, "Conversion changed artwork, geometry, keys, explicit routes or defaults: " + filename + "/" + key);
				core::World world("Bundled " + key, 24, 3);
				auto room = world.addRoom("Room", 0, 0, 0, 24, 2);
				world.attachFurnitureCatalogue(filename, catalogue);
				world.placeFurniture(room, key, 4.125f, 0, "Example", 4);
				// This chair intentionally has no depth-zero Floor port: attach it to the desk's matching front route.
				if (std::string(name) == "attachments" && key == "chair")
					world.placeFurniture(room, "desk", 3.125f, 0, "Supporting desk", 5);
				world.addSectorMarker(room, 0, 0.5f, "Entrance");
				world.addSectorMarker(room, 0, 22.5f, "Exit");
				world.finishBuild();
				auto visitor = world.createAgent("Visitor", room, 0, 0.5f);
				require(world.moveAgentToNamedMarker(visitor, "Exit").accepted(), "Bundled route request refused");
				world.advanceTicks(4000);
				require(world.lookupAgent(visitor).entity->getGlobalPosition().x == 22.5f, "Bundled explicit circulation route lost");
				std::vector<core::AgentId> users;
				for (auto const& destination : world.furniture().front().destinations)
				{
					auto id = world.createAgent("User " + destination.key, room, 0, 0.5f); users.push_back(id);
					auto expected = std::vector<std::string>{"idle"};
					if (definition.hasUse) expected.push_back("use-furniture");
					require(world.availableAgentActions(destination.marker) == expected, "Bundled use availability changed");
					require(world.moveAgentToMarker(id, destination.marker).accepted(), "Default Idle request refused");
					world.advanceTicks(4000);
					auto point = std::find_if(definition.usablePoints.begin(), definition.usablePoints.end(),
						[&](auto const& p) { return p.key == destination.key; });
					require(world.lookupAgent(id).entity->getGlobalPosition().x == 4.125f + point->x
						&& world.lookupAgent(id).entity->getPose() == core::Pose::Standing && !world.usablePointOccupant(destination.marker),
						"Default Idle did not arrive Standing without use: " + filename + "/" + key);
					if (definition.hasUse)
					{
						require(world.moveAgentToMarker(id, destination.marker, core::UseFurnitureAction).accepted(), "Explicit bundled use refused");
						world.advanceTicks(600);
						require(world.lookupAgent(id).entity->getPose() == (key == "bed" ? core::Pose::Lying : core::Pose::Sitting)
							&& world.usablePointOccupant(destination.marker) == id, "Bundled use did not set Pose/claim selected point: " + filename + "/" + key + "/" + destination.key);
					}
				}
				for (size_t i = 0; i < users.size(); ++i)
				{
					auto marker = world.furniture().front().destinations[i].marker;
					require(world.moveAgentToMarker(users[i], marker, core::IdleAction).accepted(), "Explicit Idle refused");
					world.advanceTicks(600);
					require(world.lookupAgent(users[i]).entity->getPose() == core::Pose::Standing && !world.usablePointOccupant(marker),
						"Bundled finish did not stand/release");
					for (size_t j = i + 1; j < users.size(); ++j)
						if (definition.hasUse) require(world.usablePointOccupant(world.furniture().front().destinations[j].marker) == users[j],
							"Sofa finishing released an independent seat");
				}
				for (auto suffix : {"world.yaml", "world"})
				{
					auto document = root / (std::string(name) + "-" + key + "." + suffix);
					world.saveTo(document.string()); auto loaded = core::loadWorldDocument(document);
					sameInstances(world, *loaded);
					require(loaded->furnitureCatalogueResourceName() == filename, "Round trip lost Lua reference");
				}
			}
		}
		for (auto name : {"chair", "desk", "furniture", "furniture-test-1", "furniture-integration"})
		{
			auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/" + std::string(name) + ".world.yaml"));
			auto legacy = core::loadWorldDocument(context.fixture("src/headless/smoke/fixtures/furniture/" + std::string(name) + ".world.yaml"));
			// furniture-test-1 is an editable chair sample, not the frozen bed regression.
			// Validate each World's identities against its own round trip below.
			if (std::string(name) != "furniture-test-1") sameInstances(*legacy, *world);
			for (auto suffix : {"world.yaml", "world"})
			{
				auto document = root / (std::string(name) + "-bundled." + suffix);
				world->saveTo(document.string()); auto loaded = core::loadWorldDocument(document); sameInstances(*world, *loaded);
				loaded->advanceTicks(4000);
				if (std::string(name) == "furniture" || std::string(name) == "furniture-test-1")
					require(loaded->lookupAgent(core::AgentId{1}).entity->getPose() ==
						core::Pose::Sitting,
						"Teaching journey lost explicit Use furniture intent");
			}
			for (auto suffix : {"world.yaml", "world"})
			{
				auto document = root / (std::string(name) + "-regression." + suffix);
				legacy->saveTo(document.string());
				auto loaded = core::loadWorldDocument(document);
				sameInstances(*legacy, *loaded);
			}
			legacy->advanceTicks(4000);
			if (std::string(name) == "furniture" || std::string(name) == "furniture-test-1")
				require(legacy->lookupAgent(core::AgentId{1}).entity->getPose() ==
					(std::string(name) == "furniture-test-1" ? core::Pose::Lying : core::Pose::Sitting),
					"Converted regression World lost explicit use intent");
			else
				for (auto const& agent : legacy->getSimulationSnapshot().agents)
					require(agent.pose == core::Pose::Standing, "Omitted regression Action implicitly used Furniture");
		}
	}

	void luaObjects(smoke::Context const& context)
	{
		using smoke::require;
		auto root = context.temporaryRoot() / "lua-objects";
		std::filesystem::create_directories(root);
		auto source = context.fixture("src/headless/smoke/fixtures/objects.furniture.lua");
		auto package = root / "objects.furniture.lua";
		std::filesystem::copy_file(source, package);
		std::ifstream input(source); std::string original((std::istreambuf_iterator<char>(input)), {});
		auto write = [&](std::string const& text) { std::ofstream output(package); output << text; };
		auto variant = [&](std::string const& edit) {
			auto text = original;
			text.replace(text.find("return {"), 8, "local catalogue = {");
			return text + "\n" + edit + "\nreturn catalogue\n";
		};
		core::World world("Lua objects", 12, 3);
		auto room = world.addRoom("Room", 0, 0, 0, 12, 2);
		world.attachFurnitureCatalogue(package.filename().string(), core::FurnitureCatalogue::load(package));
		auto desk = world.placeFurniture(room, "desk", 2.125f, 0, "Desk", 2);
		world.placeFurniture(room, "table", 6, 0, "Table");
		world.placeFurniture(room, "chair", 8, 0, "Chair");
		auto seat = world.furniture().front().marker;
		auto chairSeat = world.furniture().back().marker;
		require(!world.furnitureCatalogue()->definition("desk")->hasUse
			&& world.furnitureCatalogue()->definition("chair")->hasUse
			&& world.furniture()[1].destinations.empty(), "Paired functions/non-usable definitions were lost");
		world.addSectorMarker(room, 0, 0.5f, "Entrance");
		world.addSectorMarker(room, 0, 10.5f, "Exit");
		world.finishBuild(); world.pauseSimulation();
		require(world.availableAgentActions(chairSeat) == std::vector<std::string>{"idle", "use-furniture"},
			"Paired Lua callbacks did not derive Use furniture availability");
		std::string diagnostic;
		require(world.renameMarker(seat, "Authored desk point", &diagnostic), diagnostic);
		require(world.setMarkerProperties(seat, 0, &diagnostic), diagnostic);
		auto before = snapshot(world);
		for (auto placement : {std::pair{2.5f, 0.f}, {11.f, 0.f}, {4.f, 1.f}, {4.f, 0.25f}})
			require(!world.canPlaceFurniture(room, "desk", placement.first, placement.second, "Invalid", &diagnostic, 2)
				&& snapshot(world) == before, "Lua geometry bypassed overlap/support/containment validation");
		require(!world.canPlaceFurniture(room, "desk", 4, 0, "Negative", &diagnostic, -1), "Negative depth admitted");
		auto visitor = world.createAgent("Visitor", room, 0, 0.5f);
		require(world.moveAgentToNamedMarker(visitor, "Exit").accepted(), "Desk circulation request refused");
		world.resumeSimulation();
		bool side = false;
		for (int tick = 0; tick < 1600; ++tick)
		{
			world.advanceTicks(1);
			auto agent = world.lookupAgent(visitor).entity;
			if (agent->getGlobalPosition().x > 2.5f && agent->getGlobalPosition().x < 3.5f)
			{
				side = true;
				require(agent->getLocalDepth() == 2 || agent->getLocalDepth() == 3, "Lua desk acquired floor shortcut");
			}
		}
		require(side && world.lookupAgent(visitor).entity->getGlobalPosition().x == 10.5f, "Lua desk normal movement failed");
		require(world.moveAgentToMarker(visitor, chairSeat).accepted(), "Stored Lua chair point unavailable to Idle");
		world.advanceTicks(1600);
		require(world.lookupAgent(visitor).entity->getGlobalPosition().x == 8.5f
			&& world.lookupAgent(visitor).entity->getPose() == core::Pose::Standing,
			"Catalogue loading activated deferred use callback");
		world.pauseSimulation();
		require(world.editFurniture(desk, 3.125f, 0, "Edited desk", &diagnostic, 3), diagnostic);
		require(world.furniture().front().marker == seat, "Lua instance edit changed owned identity");
		require(world.authorAgentMarkerRequest(visitor, chairSeat, core::IdleAction, &diagnostic), diagnostic);
		std::vector<std::filesystem::path> documents;
		for (auto suffix : {"world.yaml", "world"})
		{
			auto document = root / (std::string("objects.") + suffix); documents.push_back(document);
			world.saveTo(document.string());
			auto loaded = core::loadWorldDocument(document);
			require(loaded->furnitureCatalogueResourceName() == "objects.furniture.lua"
				&& loaded->furnitureCatalogue()->uuid() == world.furnitureCatalogue()->uuid()
				&& loaded->furniture().front().id == desk && loaded->furniture().front().marker == seat
				&& loaded->lookupMarker(seat)->getName() == "Authored desk point"
				&& !loaded->lookupMarker(seat)->hasProperty(core::MarkerProperty::BlocksPathing),
				"Lua document round trip lost stable/authored identities");
			auto yaml = snapshot(*loaded);
			require(yaml.find("finish_use") == std::string::npos && yaml.find("world.set_pose") == std::string::npos,
				"Document serialized executable catalogue state");
		}
		before = snapshot(world);
		auto acceptedCatalogue = world.furnitureCatalogue();
		std::string const legacySource = "furnitureCatalogue:\n  version: 1\n  uuid: " + acceptedCatalogue->uuid() + "\n  definitions: []\n";
		for (auto suffix : {".furniture.yaml", ".furniture.yml", ".yaml", ".yml"})
		{
			auto external = root / (std::string("external") + suffix);
			{ std::ofstream output(external); output << legacySource; }
			auto refusesConversion = [&](auto operation) {
				std::string message;
				try { operation(); }
				catch (std::exception const& error) { message = error.what(); }
				require(message.find("requires conversion to Lua") != std::string::npos,
					"YAML Furniture did not report conversion required: " + external.string()
						+ " (" + message + ")");
			};
			refusesConversion([&] { (void)core::FurnitureCatalogue::readFile(external); });
			refusesConversion([&] { (void)core::FurnitureCatalogue::load(external); });
			auto pathShapedRefused = false;
			try { world.attachFurnitureCatalogue("../" + external.filename().string(), acceptedCatalogue); }
			catch (std::exception const& error)
			{
				pathShapedRefused = std::string(error.what()).find("Resource name")
					!= std::string::npos;
			}
			require(pathShapedRefused, "A path-shaped Furniture reference was accepted: " + external.string());
			require(!world.reloadFurnitureCatalogue(external, &diagnostic)
				&& diagnostic.find("requires conversion to Lua") != std::string::npos, "YAML reload lost conversion diagnostic");
			auto oldDocument = YAML::Load(before);
			oldDocument["furnitureCatalogue"].remove("filename");
			oldDocument["furnitureCatalogue"]["resource"] = external.filename().string();
			auto document = root / "external.world.yaml";
			{ std::ofstream output(document); output << oldDocument; }
			refusesConversion([&] { (void)core::loadWorldDocument(document); });
			std::ifstream input(external); std::string bytes((std::istreambuf_iterator<char>(input)), {});
			require(bytes == legacySource && !std::filesystem::exists(root / "external.furniture.lua")
				&& snapshot(world) == before && world.furnitureCatalogue() == acceptedCatalogue,
				"YAML rejection mutated the World/package or silently rewrote the file");
		}
		auto rejected = [&](std::string const& sourceText) {
			write(sourceText);
			for (auto const& document : documents)
			{
				bool refused = false;
				try { (void)core::loadWorldDocument(document); } catch (std::exception const&) { refused = true; }
				require(refused, "Invalid Lua dependency accepted by World document loader: " + sourceText.substr(sourceText.size() > 200 ? sourceText.size() - 200 : 0));
			}
			require(snapshot(world) == before && world.furnitureCatalogue()->definition("chair")->hasUse,
				"Rejected dependency modified the live immutable catalogue/World");
		};
		for (auto edit : {
			"catalogue.api_version = 2", "catalogue.uuid = 'bad'", "catalogue.uuid = '00000000-0000-4000-8000-000000000001'",
			"catalogue.definitions[2].use = nil", "catalogue.definitions[2].finish_use = false",
			"catalogue.definitions[2].usablePoints = {}; catalogue.definitions[2].use = nil; catalogue.definitions[2].finish_use = nil",
			"catalogue.definitions = {}", "catalogue.definitions[1].tiles[1].imageSet = 'Missing'",
			"catalogue.definitions[1].self = catalogue", "catalogue.definitions[1].usablePoints[1].key = 'bad key'",
			"catalogue.definitions[2].use = print", "local n = 0; catalogue.definitions[2].use = function() n = n + 1 end",
			"catalogue.definitions[1].tiles[1].x = 0.5", "catalogue.definitions[1].tiles[2].x = 0",
			"catalogue.definitions[1].usablePoints[1].x = 0/0", "catalogue.definitions[1].usablePoints[1].y = 1",
			"catalogue.definitions[1].vertices[1].x = 100", "catalogue.definitions[1].edges[1].to = 'missing'",
			"catalogue.definitions[1].edges[2] = catalogue.definitions[1].edges[1]",
			"catalogue.definitions[1].tiles[2].x = 12", "catalogue.definitions[1].usablePoints[1].action = 'Sit'",
			"catalogue.definitions[2].key = 'desk'", "catalogue.definitions[1].vertices[7].usablePoint = 'missing'",
			"catalogue.definitions[1].vertices[1].external = 1", "catalogue.definitions[4] = catalogue.definitions[3]; catalogue.definitions[3] = nil",
			"catalogue.definitions.extra = {}", "catalogue.unexpected = function() end",
			"setmetatable(catalogue, {})", "catalogue.definitions[1].edges[1].depthOffset = '2'"})
			rejected(variant(edit));
		for (auto text : {"not Lua!", "return 1", "require('missing')", "require('../escape')", "io.open('file')",
			"while true do end", "pcall(function() while true do end end); return {}",
			"local t = {}; for i=1,100000 do t[i] = string.rep('x', 10000) end; return t"}) rejected(text);
		rejected(legacySource); // Renaming YAML to .furniture.lua is not conversion or a parsing fallback.
		rejected(std::string(256 * 1024 + 1, ' '));
		std::filesystem::remove(package);
		for (auto const& document : documents)
		{
			bool refused = false;
			try { (void)core::loadWorldDocument(document); } catch (std::exception const&) { refused = true; }
			require(refused, "Missing Lua catalogue dependency accepted");
		}
		write(variant("catalogue.definitions[1].usablePoints[2] = {key='extra', label='Extra', x=1.25, blocksPathing=false}; "
			"catalogue.definitions[1].vertices[9] = {key='extra', x=1.25, usablePoint='extra'}; "
			"catalogue.definitions[1].edges[8] = {from='frontRight', to='extra', depthOffset=0}"));
		for (auto const& document : documents)
		{
			auto loaded = core::loadWorldDocument(document);
			require(loaded->furniture().front().destinations.size() == 2
				&& loaded->furniture().front().marker == seat && loaded->lookupMarker(seat)->getName() == "Authored desk point"
				&& loaded->furniture().front().destinations[1].marker != seat,
				"Lua reconciliation lost stable keys/Marker identity");
		}
		write(variant("catalogue.definitions[2].use = function(agent, world, marker) sit(agent, world, marker) end"));
		for (auto const& document : documents)
			require(core::loadWorldDocument(document)->furnitureCatalogue()->definition("chair")->hasUse,
				"Stateless shared Lua helpers were refused");
		write(original);
	}

	void actions(smoke::Context const& context)
	{
		using smoke::require;
		auto source = context.fixture("src/headless/smoke/fixtures/sit.furniture.lua");
		auto catalogue = core::FurnitureCatalogue::readFile(source);
		require(catalogue->definition("chair")->hasUse
			&& catalogue->definition("chair")->usablePoints.size() == 2,
			"Paired chair callbacks or independent points lost");
		auto bundled = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/furniture/furniture.furniture.lua"));
		auto bed = bundled->definition("bed");
		require(bed && bed->maxX - bed->minX == 2 && bed->maxY - bed->minY == 1
			&& bed->usablePoints.size() == 1 && bed->usablePoints[0].x == 1.f && bed->hasUse,
			"Bed must span two cells with one central usable point and paired callbacks");
		auto plain = core::FurnitureCatalogue::readFile(context.fixture("src/headless/smoke/fixtures/furniture/chair.furniture.lua"));
		require(!plain->definition("chair")->hasUse, "Geometry-only chair gained use callbacks");
		for (auto mutation : {"catalogue.definitions[1].use = true", "catalogue.definitions[1].finish_use = nil",
			"catalogue.definitions[1].usablePoints[1].action = 'Sit'"})
		{
			auto file = context.temporaryRoot() / "invalid-action.furniture.lua";
			smoke::writeCatalogue(file, smoke::catalogueSource(source) + mutation);
			bool rejected = false;
			try { core::FurnitureCatalogue::readFile(file); }
			catch (core::SerializationException const&) { rejected = true; }
			require(rejected, "Malformed callback pair or retired point action accepted");
		}
	}

	void demonstration(smoke::Context const& context)
	{
		using smoke::require;
		auto world = core::loadWorldDocument(context.fixture("resources/test-worlds/furniture-integration.world.yaml"));
		require(world->furniture().size() == 6 && world->furniture()[3].y == 1
			&& world->furniture()[2].destinations.size() == 2
			&& world->furniture()[2].destinations[0].marker != world->furniture()[2].destinations[1].marker
			&& world->furniture()[1].destinations.empty() && !world->furniture()[1].marker,
			"Required demonstration lost Walkway support or distinct sofa destinations");
		std::shared_ptr<const core::Vertex> exit;
		for (auto const& vertex : world->getGraph()->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject()); marker && marker->getName() == "Exit") exit = vertex;
		require(exit != nullptr, "Required demo Exit Marker is missing");
		for (auto const& entry : world->getSimulationSnapshot().agents)
		{
			auto agent = world->lookupAgent(entry.id).entity;
			if (agent->getName() == "Showroom walker") continue;
			auto path = world->getGraph()->calculatePath(agent, exit);
			require(path != nullptr, "Stationary demonstration observer cannot depart");
			bool side = false;
			for (auto const& node : path->nodes)
				if (node.edge && node.edge->getLength() > 0
					&& std::min(node.edge->getVertex(0)->getPosition().x, node.edge->getVertex(1)->getPosition().x) < 4.125f
					&& std::max(node.edge->getVertex(0)->getPosition().x, node.edge->getVertex(1)->getPosition().x) > 2.125f)
				{
					side = true;
					require(node.edge->getLocalDepth() == 2 || node.edge->getLocalDepth() == 3,
						"Demo departure bypassed the authored Furniture side routes");
				}
			require(side, "Demo observer avoided the explicit front/back routes");
			agent->setPath(path, true);
		}
		world->advanceTicks(1800);
		for (auto const& entry : world->getSimulationSnapshot().agents)
			require(entry.globalPosition.x == 9.5f,
				"Bundled demo journey did not arrive normally");
	}

	void chair(smoke::Context const& context)
	{
		using smoke::require;
		auto catalogue = core::FurnitureCatalogue::load(context.fixture("src/headless/smoke/fixtures/furniture/chair.furniture.lua"));
		core::World transitWorld("Unsupported Furniture host", 4, 2);
		transitWorld.addCorridor(0, 0, 0, 4, 1); transitWorld.addCorridor(0, 1, 0, 4, 1);
		auto ladder = transitWorld.addLadder(1, 0, 2, {2, false, true});
		transitWorld.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
		std::string refusal;
		require(!transitWorld.canPlaceFurniture(ladder.ladder.sector->getIndex(), "chair", 0, 0, "Transit chair", &refusal)
			&& refusal.find("Room, Corridor or Facade") != std::string::npos, "Transit admitted Furniture");
		core::World world("Chair authoring", 16, 4);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 3);
		auto corridor = world.addCorridor(0, 3, 0, 8, 1);
		auto facade = world.addFacade("Facade", 0, 0, 8, 8, 1);
		auto background = world.addBackground(1, 0, 0, 8, 1);
		world.attachFurnitureCatalogue("chair.furniture.lua", catalogue);
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
	void usablePointDefaults(smoke::Context const& context)
	{
		using smoke::require;
		auto cataloguePath = context.temporaryRoot() / "pass-through.furniture.lua";
		// Pin the routes/defaults under test; the user-facing demo catalogue is editable.
		auto source = context.fixture("src/headless/smoke/fixtures/usable-points.furniture.lua");
		auto catalogue = core::FurnitureCatalogue::load(source);
		require(!catalogue->definition("chair")->usablePoints.front().blocksPathing,
			"Test chair seat must default to non-blocking");
		core::World chairWorld("One-seat sofa", 8, 2);
		auto chairRoom = chairWorld.addRoom("Room", 0, 0, 0, 8, 1);
		chairWorld.attachFurnitureCatalogue(source.filename().string(), catalogue);
		chairWorld.placeFurniture(chairRoom, "chair", 2, 0, "Chair", 2);
		auto chairSeat = chairWorld.furniture().front().marker;
		uint32_t chairEntrance = 0, chairExit = 0;
		chairWorld.addSectorMarker(chairRoom, 0, 0.5f, "Entrance", &chairEntrance);
		chairWorld.addSectorMarker(chairRoom, 0, 6.5f, "Exit", &chairExit);
		chairWorld.finishBuild(); chairWorld.pauseSimulation();
		core::Agent chairQuery("Query");
		for (bool reverse : {false, true})
		{
			auto graph = chairWorld.getGraph();
			auto path = graph->calculatePath(&chairQuery, graph->getVertexByIdentifier(reverse ? chairExit : chairEntrance),
				graph->getVertexByIdentifier(reverse ? chairEntrance : chairExit));
			require(path != nullptr, "Chair severed circulation");
			bool throughSeat = false;
			for (auto const& node : path->nodes)
				if (auto marker = std::dynamic_pointer_cast<core::Marker>(node.targetVertex->getObject()); marker && marker->getId() == chairSeat)
				{
					throughSeat = true;
					require(node.edge && node.edge->getLocalDepth() == 1, "Chair seat route is not in front");
				}
			require(throughSeat, "Chair front route did not pass through its non-blocking seat");
		}
		std::string chairDiagnostic;
		require(chairWorld.setMarkerProperties(chairSeat, core::markerPropertyBit(core::MarkerProperty::BlocksPathing), &chairDiagnostic), chairDiagnostic);
		auto chairGraph = chairWorld.getGraph();
		auto backPath = chairGraph->calculatePath(&chairQuery, chairGraph->getVertexByIdentifier(chairEntrance), chairGraph->getVertexByIdentifier(chairExit));
		require(backPath != nullptr, "Chair has no separate back route");
		bool behind = false;
		for (auto const& node : backPath->nodes) if (node.edge && node.edge->getLocalDepth() == 3) behind = true;
		require(behind, "Blocking the chair seat did not leave the back route available");

		// Omit the new chair default in this private copy to retain compatibility coverage.
		auto defaultSource = smoke::catalogueSource(source)
			+ "catalogue.definitions[1].usablePoints[1].blocksPathing = nil\n";
		smoke::writeCatalogue(cataloguePath, defaultSource);
		core::World world("Pass-through sofa seats", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
		world.attachFurnitureCatalogue(cataloguePath.filename().string(), core::FurnitureCatalogue::load(cataloguePath));
		world.placeFurniture(room, "sofa", 2, 0, "Sofa", 2);
		auto seats = world.furniture().front().destinations;
		for (auto const& point : seats)
			require(!world.lookupMarker(point.marker)->hasProperty(core::MarkerProperty::BlocksPathing),
				"Sofa usable-point blocksPathing:false was ignored");
		world.placeFurniture(room, "chair", 7, 0, "Chair", 2);
		require(world.lookupMarker(world.furniture().back().marker)->hasProperty(core::MarkerProperty::BlocksPathing),
			"Omitted blocksPathing must default to true");
		uint32_t entrance = 0, exit = 0;
		world.addSectorMarker(room, 0, 0.5f, "Entrance", &entrance);
		world.addSectorMarker(room, 0, 6.5f, "Exit", &exit);
		world.finishBuild(); world.pauseSimulation();
		auto graph = world.getGraph(); core::Agent query("Query");
		auto from = graph->getVertexByIdentifier(entrance), to = graph->getVertexByIdentifier(exit);
		auto path = graph->calculatePath(&query, from, to);
		require(path != nullptr, "Sofa seat chain severed front circulation");
		std::vector<core::MarkerId> visited;
		for (auto const& node : path->nodes)
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(node.targetVertex->getObject());
				marker && world.isFurnitureMarker(marker->getId())) visited.push_back(marker->getId());
		require(visited == std::vector<core::MarkerId>{seats[0].marker, seats[1].marker},
			"Front circulation did not pass through both sofa seats");
		for (auto const& vertex : graph->getVertices())
			if (auto marker = std::dynamic_pointer_cast<core::Marker>(vertex->getObject());
				marker && (marker->getId() == seats[0].marker || marker->getId() == seats[1].marker))
				require(graph->calculatePath(&query, from, vertex) != nullptr, "Pass-through seat is not a selectable destination");
		std::string diagnostic;
		require(world.setMarkerProperties(seats[0].marker, core::markerPropertyBit(core::MarkerProperty::BlocksPathing), &diagnostic), diagnostic);
		// A newly added catalogue point must also use its default during reconciliation.
		smoke::writeCatalogue(cataloguePath, defaultSource + R"(
local sofa = catalogue.definitions[2]
table.insert(sofa.usablePoints, {key='extra', label='Extra', x=1.625, blocksPathing=false})
table.insert(sofa.vertices, {key='extra', x=1.625, usablePoint='extra'})
table.insert(sofa.edges, {from='rightSeat', to='extra', depthOffset=-1})
)");
		for (auto extension : {"world.yaml", "world"})
		{
			auto filename = context.temporaryRoot() / (std::string("pass-through.") + extension);
			world.saveTo(filename.string());
			auto loaded = core::loadWorldDocument(filename);
			require(loaded->lookupMarker(seats[0].marker)->hasProperty(core::MarkerProperty::BlocksPathing)
				&& !loaded->lookupMarker(seats[1].marker)->hasProperty(core::MarkerProperty::BlocksPathing),
				"Catalogue defaults overwrote authored Marker properties on reopen");
			require(loaded->furniture().front().destinations.size() == 3
				&& !loaded->lookupMarker(loaded->furniture().front().destinations.back().marker)->hasProperty(core::MarkerProperty::BlocksPathing),
				"Catalogue reconciliation ignored the new usable point's non-blocking default");
		}
	}

	void layouts(smoke::Context const& context)
	{
		usablePointDefaults(context);
		using smoke::require;
		core::World world("Multi-point layouts", 20, 5);
		auto room = world.addRoom("Room", 0, 0, 0, 20, 5);
		world.attachFurnitureCatalogue("layouts.furniture.lua",
			core::FurnitureCatalogue::load(context.fixture("src/headless/smoke/fixtures/furniture/layouts.furniture.lua")));
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
		auto malformedPath = context.temporaryRoot() / "invalid.furniture.lua";
		auto original = smoke::catalogueSource(context.fixture("src/headless/smoke/fixtures/furniture/layouts.furniture.lua"));
		for (auto mutation : {"d.usablePoints[2].key = 'left'", "d.usablePoints[2].y = 0.25",
			"d.tiles[2].x = 1.25", "d.usablePoints[2].label = 'Left seat'", "d.usablePoints[2].x = 0/0"})
		{
			smoke::writeCatalogue(malformedPath, original + "local d = catalogue.definitions[1]\n" + mutation);
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
			auto source = smoke::catalogueSource(context.fixture("src/headless/smoke/fixtures/furniture/attachments.furniture.lua"));
			source += "local desk, chair = catalogue.definitions[1], catalogue.definitions[2]\n"
				"table.remove(desk.edges, 6); table.remove(desk.edges, 4)\n";
			if (variant == 2) source += "chair.vertices[1].external = false\n";
			if (variant == 4) source += "desk.vertices[3].external = true\n";
			if (variant == 5) source += "chair.vertices[2].external = true\n";
			if (variant == 7) source += "chair.edges[1].depthOffset = 2\n";
			if (variant == 9) source += R"(
local function copy(value)
  if type(value) ~= 'table' then return value end
  local result = {}; for k,v in pairs(value) do result[k] = copy(v) end; return result
end
local other = copy(chair); other.key = 'other'; other.edges[1].depthOffset = -2
table.insert(catalogue.definitions, other)
local coincident = copy(other); coincident.key = 'coincident'; coincident.edges[1].depthOffset = -3
table.insert(catalogue.definitions, coincident)
)";
			if (variant == 10) source += "chair.edges[1].depthOffset = nil\n";
			auto filename = context.temporaryRoot() / ("attachment" + std::to_string(variant) + ".furniture.lua");
			smoke::writeCatalogue(filename, source);
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
			auto source = smoke::catalogueSource(context.fixture("src/headless/smoke/fixtures/furniture/composition.furniture.lua"));
			source += "local definitions = catalogue.definitions\n";
			if (variant == 1) source += "for _,edge in ipairs(definitions[2].edges) do if edge.depthOffset then edge.depthOffset = 1 end end\n";
			if (variant == 2) source += "definitions[2].vertices[1].external = false; definitions[2].vertices[4].external = false\n";
			if (variant == 3) source += "table.insert(definitions[1].vertices, {key='gap', x=2.5}); definitions[1].edges[3].from = 'gap'\n";
			if (reverse) source += "definitions[1], definitions[2] = definitions[2], definitions[1]\n";
			auto filename = context.temporaryRoot() / "composition.furniture.lua";
			smoke::writeCatalogue(filename, source);
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
		auto fixture = context.fixture("src/headless/smoke/fixtures/furniture/desk.furniture.lua");
		auto catalogue = core::FurnitureCatalogue::load(fixture);
		// furniture-test-1: the chair overlaps the desk at matching back-route depth.
		for (int variant = 0; variant < 4; ++variant)
		{
			auto sourcePath = context.fixture("resources/test-worlds/furniture.furniture.lua");
			std::ifstream input(sourcePath);
			std::string source{std::istreambuf_iterator<char>(input), {}};
			source.replace(source.find("return {"), 8, "local catalogue = {");
			bool blocked = variant >= 2;
			if (blocked) source += "\ntable.remove(catalogue.definitions[4].edges, 5)\n";
			auto package = context.temporaryRoot() / ("chair-behind-desk-" + std::to_string(variant) + ".furniture.lua");
			smoke::writeCatalogue(package, source);
			core::World world("Chair behind desk", 12, 3);
			auto room = world.addRoom("Room", 0, 2, 2, 9, 1);
			world.attachFurnitureCatalogue(package.filename().string(), core::FurnitureCatalogue::load(package));
			if (variant % 2 == 0) world.placeFurniture(room, "chair", 3, 0, "Chair", 1);
			world.placeFurniture(room, "desk", 2, 0, "Desk", 0);
			if (variant % 2 != 0) world.placeFurniture(room, "chair", 3, 0, "Chair", 1);
			world.finishBuild();
			auto id = world.createAgent("Walker", room, 0, 0.734375f);
			auto chair = std::find_if(world.furniture().begin(), world.furniture().end(),
				[](auto const& instance) { return instance.definitionKey == "chair"; });
			world.pauseSimulation();
			std::string diagnostic;
			require(world.authorAgentMarkerRequest(id, chair->marker, core::UseFurnitureAction, &diagnostic), diagnostic);
			require(world.resumeSimulation(), "Chair movement resume refused");
			auto document = context.temporaryRoot() / ("chair-behind-desk-" + std::to_string(variant) + ".world.yaml");
			world.saveTo(document.string());
			auto restored = core::loadWorldDocument(document);
			auto& movingWorld = variant % 2 ? *restored : world;
			auto agent = movingWorld.lookupAgent(id).entity;
			bool passedDesk = false;
			float maximumX = 0, previousX = agent->getGlobalPosition().x;
			int previousDepth = agent->getLocalDepth();
			for (int tick = 0; tick < 1200; ++tick)
			{
				movingWorld.advanceTicks(1);
				auto x = agent->getGlobalPosition().x;
				maximumX = std::max(maximumX, x);
				if (x > 4 && x < 6)
				{
					passedDesk = true;
					require(agent->getLocalDepth() == (blocked && maximumX < 6 ? 0 : 1),
						"Chair approach crossed through desk: x=" + std::to_string(x)
						+ " depth=" + std::to_string(agent->getLocalDepth()));
					if (previousX > 4 && previousX < 6)
						require(agent->getLocalDepth() == previousDepth, "Depth changed inside desk footprint");
				}
				previousX = x; previousDepth = agent->getLocalDepth();
			}
			require(passedDesk && agent->getPose() == core::Pose::Sitting, "Chair approach did not arrive and sit: variant="
				+ std::to_string(variant) + " x=" + std::to_string(agent->getGlobalPosition().x)
				+ " state=" + std::to_string(static_cast<int>(agent->getState())));
			require(blocked ? maximumX >= 6 : maximumX == 5.5f, "Chair did not choose the expected direct/detour route");
		}
		int baselineArrival = 0;
		for (int depth : { 0, 2, 5 })
		{
			core::World world("Isolated desk", 8, 2);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("desk.furniture.lua", catalogue);
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
			auto source = smoke::catalogueSource(fixture) + "local d = catalogue.definitions[1]\n";
			source += "table.remove(d.edges, " + std::to_string(variant == 0 ? 2 : 5) + ")\n";
			if (variant == 2) source += "d.edges[2].depthOffset = -3\n";
			if (variant == 3) source += R"(
d.sideRoutes = false
d.vertices = {{key='left', x=0, external=true}, {key='seat', x=0.75, usablePoint='seat'}}
d.edges = {{from='left', to='seat', depthOffset=0}}
)";
			auto filename = context.temporaryRoot() / ("side" + std::to_string(variant) + ".furniture.lua");
			smoke::writeCatalogue(filename, source);
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
		// #370: either authoring order must diagnose stranded ordinary row objects.
		for (int variant = 0; variant < 8; ++variant)
		{
			core::World world("Stranded row object", 8, 2);
			auto room = world.addRoom("Front", 0, 0, 0, 8, 1);
			world.addRoom("Back", 1, 0, 0, 8, 1);
			world.attachFurnitureCatalogue("desk.furniture.lua", catalogue);
			auto object = [&] {
				if (variant < 2) world.addSectorMarker(room, 0, 3, "Stranded Marker");
				else if (variant < 4) world.addSectorDoor(0, 0, 3);
				else if (variant < 6) world.addSectorWindow(0, 0, 3, 1, 1);
				else world.addSectorLightSwitch(room, 3);
			};
			if (variant % 2 == 0) object();
			world.placeFurniture(room, "desk", 2, 0, "Desk", 2);
			if (variant % 2 != 0) object();
			world.finishBuild();
			auto const& log = world.getGraph()->getBuildLog();
			require(std::any_of(log.begin(), log.end(), [](auto const& entry) {
				return entry.level == core::LogLevel::Error && entry.msg.find("strands") != std::string::npos;
			}), "Furniture silently stranded a row object, variant " + std::to_string(variant));
		}
		for (auto width : {2u, 3u})
		{
			core::World world("Wall attachment", width, 2);
			auto room = world.addRoom("Room", 0, 0, 0, width, 1);
			world.attachFurnitureCatalogue("desk.furniture.lua", catalogue);
			auto id = world.placeFurniture(room, "desk", 0, 0, "Wall desk", 2);
			uint32_t marker = 0;
			world.addSectorMarker(room, 0, width == 2 ? 1.0f : 2.0f, "Room Marker", &marker);
			world.finishBuild();
			auto const& log = world.getGraph()->getBuildLog();
			auto unattached = std::any_of(log.begin(), log.end(), [](auto const& entry) {
				return entry.level == core::LogLevel::Error && entry.msg.find("no ordinary floor attachment") != std::string::npos;
			});
			require(unattached == (width == 2), "Wall-to-wall attachment diagnostic was missing or rejected one-sided attachment");
			if (width == 3)
			{
				std::shared_ptr<const core::Vertex> seat;
				for (auto const& vertex : world.getGraph()->getVertices())
					if (vertex->getTopologyKey() == "furniture:" + std::to_string(id) + ":seat") seat = vertex;
				core::Agent query("Query");
				require(seat && world.getGraph()->calculatePath(&query,
					world.getGraph()->getVertexByIdentifier(marker), seat), "One-sided floor attachment lost its seat or boundary Marker");
				require(std::none_of(log.begin(), log.end(), [](auto const& entry) {
					return entry.level == core::LogLevel::Error;
				}), "Valid one-sided Furniture generated an error");
			}
			world.pauseSimulation();
			require(world.removeFurniture(id), "Unable to repair diagnosed placement");
			world.finishBuild();
			for (auto const& entry : world.getGraph()->getBuildLog())
				require(entry.level != core::LogLevel::Error, "Rebuild retained a stale Furniture error after repair");
		}
		core::World protectedWorld("Protected desk", 8, 2);
		auto frontRoom = protectedWorld.addRoom("Approach", 0, 0, 0, 8, 1);
		auto backRoom = protectedWorld.addRoom("Protected", 1, 0, 0, 8, 1);
		protectedWorld.addSectorDoor(0, 0, 0);
		protectedWorld.attachFurnitureCatalogue("desk.furniture.lua", catalogue);
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
	checks.push_back({ "furniture/bundledLua", bundledLua });
	checks.push_back({ "furniture/catalogResourceResolution", catalogResourceResolution });
	checks.push_back({ "furniture/luaObjects", luaObjects });
	checks.push_back({ "furniture/actions", actions });
	checks.push_back({ "furniture/demo", demonstration });
	checks.push_back({ "furniture/chair", chair });
	checks.push_back({ "furniture/layouts", layouts });
	checks.push_back({ "furniture/deskRoutes", deskRoutes });
	checks.push_back({ "furniture/attachments", attachments });
	checks.push_back({ "furniture/composition", composition });
}
