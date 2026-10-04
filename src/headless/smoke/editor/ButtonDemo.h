// Reproducible #437 fixture builder and document/editor production-seam check.
// Included by DoorPanel.cpp so this check shares its existing module ownership.
#include "core/AgentTagRegistryDocument.h"
#include "core/Agent.h"
#include <cmath>
#include <map>
#include <tuple>

namespace
{
	std::shared_ptr<core::World> buildButtonDemo(bool reverse)
	{
		using namespace core;
		auto world = std::make_shared<World>("Mixed button placement", 40, 5);
		auto front = world->addRoom("Door approaches", 0, 0, 0, 18, 5);
		for (uint32_t x = 0; x < 18; ++x) world->addSectorWalkway(front, 2, x);
		world->pauseSimulation();
		auto call = world->addAccessPermission("Platform call");
		auto climb = world->addAccessPermission("Ladder extension");
		for (uint32_t count = 2; count <= 4; ++count)
		{
			auto x = 1 + (count - 2) * 4;
			world->addRoom("Retained wall", 1, 0, x - 1, 1, 5);
			auto room = world->addRoom(std::to_string(count) + "-button stack", 1, 0, x, 2, 5);
			for (auto level : {2u, 4u})
				for (auto cell : {0u, 1u}) world->addSectorWalkway(room, level, cell);
			World::CreateLiftOptions platform; platform.stopOffsets = {0, 2};
			platform.landingControlPermissionRequirements = {{call}, {call}};
			World::CreateLadderOptions ladder{0, true, false};
			ladder.controlPermissionRequirements[0] = {climb}; ladder.controlPermissionRequirements[1] = {climb};
			auto add = [&](uint32_t owner)
			{
				if (owner == 0) world->addSectorPlatformLift(room, 0, 0, platform);
				if (owner == 1) world->addRoomLadder(room, 0, 1, ladder);
				if (owner == 2) world->addSectorDoor(0, 2, x, World::RemoteControlledDoor1Options);
				if (owner == 3) world->addRoomLadder(room, 2, 1, ladder);
			};
			for (uint32_t i = 0; i < count; ++i) add(reverse ? count - 1 - i : i);
			if (count == 4)
			{
				auto caller = world->createAgent("Caller", room, 2, 1.0f);
				world->grantAgentAccessPermission(caller, call);
				auto climber = world->createAgent("Climber", room, 2, 0.5f);
				world->grantAgentAccessPermission(climber, climb);
			}
		}
		world->addRoom("Removed wall neighbour", 1, 0, 12, 1, 5);
		auto reassignment = world->addRoom("Side reassignment", 1, 0, 13, 3, 3);
		for (uint32_t x = 0; x < 3; ++x) world->addSectorWalkway(reassignment, 2, x);
		world->removeLocationWall(reassignment, 0, CORE_SIDE_LEFT);
		auto addSide = [&](uint32_t i)
		{
			if (i == 2) world->addRoomLadder(reassignment, 0, 2, {0, true, false});
			else world->addSectorDoor(0, 0, 13 + i, World::RemoteControlledDoor1Options);
		};
		for (uint32_t i = 0; i < 3; ++i) addSide(reverse ? 2 - i : i);
		auto independent = world->addRoom("Independent Stops", 0, 0, 18, 4, 5);
		for (auto x : {1u, 2u}) world->addSectorWalkway(independent, 2, x);
		for (auto x : {0u, 1u}) world->addSectorWalkway(independent, 4, x);
		World::CreateLiftOptions platform; platform.stopOffsets = {0, 2, 4};
		world->addSectorPlatformLift(independent, 0, 1, platform);
		auto left = world->addRoom("Inset left", 0, 0, 24, 2, 1);
		world->addRoom("Inset right", 0, 0, 26, 2, 1);
		World::CreateBulkheadDoorOptions bulkhead; bulkhead.controls[0] = bulkhead.controls[1] = true;
		world->addSectorBulkheadDoor(0, 0, 26, CORE_SIDE_LEFT, bulkhead);
		world->addSectorLightSwitch(left, 0);
		auto bridge = world->addRoom("Permanent bridge supports", 0, 0, 29, 7, 3);
		for (auto x : {0u, 1u, 4u, 5u, 6u}) world->addSectorWalkway(bridge, 1, x);
		world->addSectorForceBridge(bridge, 1, 2, {2, CORE_SIDE_LEFT, true, false, 2});
		world->addRoom("Support refusal neighbour", 1, 0, 36, 1, 5);
		auto support = world->addRoom("Atomic support refusal", 1, 0, 37, 2, 5);
		world->addSectorWalkway(support, 4, 0); world->addSectorWalkway(support, 4, 1);
		platform.stopOffsets = {0, 4};
		if (reverse) world->addRoomLadder(support, 0, 0, {0, true, false});
		world->addSectorPlatformLift(support, 0, 1, platform);
		if (!reverse) world->addRoomLadder(support, 0, 0, {0, true, false});
		world->finishBuild(); world->pauseSimulation();
		return world;
	}

	using DemoLayout = std::vector<std::tuple<uint32_t, float, float, std::string, std::vector<core::AccessPermissionId>>>;
	DemoLayout buttonDemoLayout(core::World const& world)
	{
		using namespace core;
		DemoLayout result;
		for (uint32_t sector = 0; sector < world.getNumSectors(); ++sector)
		{
			auto host = world.getSector(sector);
			if (!host) continue;
			std::map<Vector2, std::shared_ptr<const Vertex>, bool(*)(Vector2, Vector2)> approaches(
				[](Vector2 a, Vector2 b) { return std::tuple{a.x, a.y} < std::tuple{b.x, b.y}; });
			for (uint32_t slot = 0; slot < host->getNumObjects(); ++slot)
			{
				auto object = host->getObject(slot);
				auto button = object ? std::dynamic_pointer_cast<const Button>(object->_getObject()) : nullptr;
				if (!button) continue;
				auto centre = button->getPosition() + button->getSize() * 0.5f;
				auto point = world.lookupInteractionPoint(button->getInteractionPointId());
				auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
				auto normal = Vector2{centre.x, std::floor(button->getPosition().y)};
				require(point && point.entity->getPosition() == normal && vertex && vertex->getPosition() == normal,
					"Demo raised or lost its production interaction/graph approach");
				if (approaches.contains(normal)) require(approaches[normal] == vertex, "Demo stack has multiple approaches");
				approaches[normal] = vertex;
				require(world.getObjectAtPosition(host->getLayerIndex(), centre.x, centre.y) == button, "Demo hit test selected another member");
				result.emplace_back(host->getLayerIndex(), centre.x, button->getPosition().y, point.entity->getName(),
					world.getInteractionPointPermissionRequirement(button->getInteractionPointId()));
			}
		}
		std::sort(result.begin(), result.end());
		return result;
	}

	void checkButtonDemo(smoke::Context const& context)
	{
		using namespace core;
		auto world = buildButtonDemo(false);
		auto expected = buttonDemoLayout(*world);
		require(buttonDemoLayout(*buildButtonDemo(true)) == expected, "Opposite mixed owner creation order changed canonical layout/order");
		// This generated file is also a reproducible, normal World document.
		auto generated = context.temporaryRoot() / "mixed-buttons.world.yaml";
		world->saveTo(generated.string());
		auto bundled = loadWorldDocument(context.fixture("resources/test-worlds/mixed-buttons.world.yaml"));
		require(bundled && bundled->getLoadWarnings().empty() && buttonDemoLayout(*bundled) == expected,
			"Bundled mixed-button demonstration differs from reproducible builder");
		for (auto extension : {"world.yaml", "world"})
		{
			auto path = context.temporaryRoot() / (std::string("reloaded.") + extension);
			bundled->saveTo(path.string()); auto loaded = loadWorldDocument(path);
			require(buttonDemoLayout(*loaded) == expected, "Demo YAML/binary load changed canonical placement/order");
			loaded->resetSimulation(); require(buttonDemoLayout(*loaded) == expected, "Demo construction replay changed placement/order");
		}
		// Freeze exact centre, rank and protected operation order, not runtime IDs.
		for (uint32_t count = 2; count <= 4; ++count)
		{
			float x = static_cast<float>(2 + (count - 2) * 4); uint32_t rank = 0;
			for (auto const& [layer, centre, y, name, permissions] : expected)
			{
				(void)name;
				if (layer != 1 || centre != x || y < 2 || y >= 3) continue;
				require(std::abs(y - (2 + CORE_BUTTON_Y_OFFSET + rank * (CORE_BUTTON_SIZE / CORE_CELL_YX_RENDER_RATIO) * 1.25f)) < 0.00001f,
					"Demo stack has noncanonical spacing");
				auto wanted = count >= 3 && rank == 0 ? std::vector<AccessPermissionId>{}
					: std::vector<AccessPermissionId>{AccessPermissionId{rank == (count >= 3 ? 1u : 0u) ? 1u : 2u}};
				require(permissions == wanted, "Demo stack changed canonical type/role order or merged protection"); ++rank;
			}
			require(rank == count, "Demo lacks two/three/four-member stack");
		}
		auto at = [&](uint32_t layer, uint32_t level, float x)
		{
			return std::count_if(expected.begin(), expected.end(), [&](auto const& member)
			{ return std::get<0>(member) == layer && std::get<1>(member) == x && std::floor(std::get<2>(member)) == level; });
		};
		require(at(0, 2, 10) == 1 && at(1, 2, 10) == 4, "Coincident different-Layer controls were combined");
		require(at(1, 0, 13) == 1 && at(1, 0, 14) == 1 && at(1, 0, 15) == 1,
			"Demo missed simultaneous collision-free side reassignment");
		require(at(0, 0, 20) == 1 && at(0, 2, 20) == 1 && at(0, 4, 19) == 1,
			"Demo Platform Stops did not choose independently");
		for (auto [level, x] : {std::pair{0u, 24.5f}, {0u, 25.75f}, {0u, 26.25f}, {1u, 30.75f}, {1u, 33.25f}})
			require(at(0, level, x) == 1, "Demo lost quarter/centred inset candidates");
		DocumentHistory history;
		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			world = deserializeDocumentSnapshot(snapshot, world, {});
			if (world) world->pauseSimulation();
			return bool(world);
		};
		// Production clipboard option readback / placement reconstructs the same
		// authored owner; derived Button geometry and runtime IDs are never copied.
		World::CreateDoorOptions copied;
		require(world->getSectorDoorOptions(0, 2, 9, 1, copied), "Demo clipboard readback failed");
		uint32_t doorIndex = ~0u;
		for (uint32_t i = 0; i < world->getSector(0)->getNumObjects(); ++i)
		{
			auto object = world->getSector(0)->getObject(i);
			if (object && object->getCellX() == 9 && object->getCellY() == 2 && object->getObjectType() == SectorObjectType::Door) doorIndex = i;
		}
		require(doorIndex != ~0u, "Demo clipboard source Door missing");
		auto before = captureDocumentSnapshot(world, history);
		require(world->removeSectorDoor(0, doorIndex), "Demo cut failed"); commitDocumentEdit(before, history);
		auto cut = buttonDemoLayout(*world); require(cut != expected, "Demo cut retained derived control");
		before = captureDocumentSnapshot(world, history);
		world->addSectorDoor(0, 2, 9, copied); world->finishBuild(); commitDocumentEdit(before, history);
		require(buttonDemoLayout(*world) == expected, "Demo clipboard reconstruction changed canonical order");
		require(history.undo(*captureDocumentSnapshot(world, history), restore) && buttonDemoLayout(*world) == cut, "Demo paste undo changed layout");
		require(history.redo(*captureDocumentSnapshot(world, history), restore) && buttonDemoLayout(*world) == expected, "Demo paste redo changed canonical layout");
		// Exercise the bundled protected stack itself, not just its builder.
		std::vector<std::shared_ptr<const Button>> stack;
		for (uint32_t i = 0; i < world->getSector(6)->getNumObjects(); ++i)
		{
			auto object = world->getSector(6)->getObject(i);
			auto button = object ? std::dynamic_pointer_cast<const Button>(object->_getObject()) : nullptr;
			if (button && object->getCellY() == 2) stack.push_back(button);
		}
		std::sort(stack.begin(), stack.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
		require(stack.size() == 4, "Demo protected stack missing");
		world->resumeSimulation();
		auto denied = world->requestInteraction(stack[3]->getInteractionPointId(), AgentId{1});
		require(world->lookupInteractionRequest(denied).entity->getResult() == InteractionResult::Rejected,
			"Caller bypassed independently protected upper Ladder control");
		auto selected = world->requestInteraction(stack[1]->getInteractionPointId(), AgentId{1});
		bool issued = false;
		for (uint32_t tick = 0; tick < 180; ++tick)
		{
			world->advanceTick();
			if (auto request = world->lookupInteractionRequest(selected); request && !request.entity->getOperations().empty())
			{
				require(request.entity->getOperations().size() == 1, "Selected stacked control merged commands");
				issued = true;
			}
			require(buttonDemoLayout(*world) == expected, "Runtime motion changed demo placement");
			require(world->lookupAgent(AgentId{1}).entity->getGlobalPosition().y == 2, "Demo Caller climbed stack artwork");
		}
		require(issued, "Selected stacked control lost its command");
		world->pauseSimulation(); world->markSaved(); history.markSaved();
		auto refuse = [&](auto edit)
		{
			auto snapshot = captureDocumentSnapshot(world, history); auto graph = world->getGraph();
			auto generation = world->getTopologyGeneration(); auto state = history.currentStateId(); bool rejected = false;
			try { edit(); } catch (std::exception const&) { rejected = true; }
			require(rejected && world->getGraph() == graph && world->getTopologyGeneration() == generation
				&& captureDocumentSnapshot(world, history)->yaml == snapshot->yaml && buttonDemoLayout(*world) == expected
				&& !world->isModified() && !history.isModified() && history.currentStateId() == state,
				"Demo refusal partially changed controls, graph, permissions or document/history");
		};
		refuse([&] { world->addSectorDoor(0, 2, 10, copied); });
		uint32_t support = world->getNumSectors() - 1;
		refuse([&] { world->addSectorWalkway(support, 2, 0); });
		// No invalid state is saved: opening the wall makes the fifth demand
		// feasible, then restoring that wall must be an atomic refusal.
		world->removeLocationWall(6, 2, CORE_SIDE_LEFT); world->finishBuild();
		world->addSectorDoor(0, 2, 10, copied); world->finishBuild();
		expected = buttonDemoLayout(*world); world->markSaved();
		refuse([&] { world->addLocationWall(6, 2, CORE_SIDE_LEFT); });
	}
}
