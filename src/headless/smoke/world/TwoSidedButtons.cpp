// Two-sided Door Buttons, for ticket #125.
//
// Giving a Door a Button must give it Buttons on both sides of the threshold:
// agents approaching from the Layer behind were stuck, because the single
// Button sat on the front side only. These checks drive the real World
// edit actions:
//
//   * Add Door Button creates the missing Button on each side, binds both to
//     the Door's single traversal resource with idempotent open-only commands,
//     and records the pre-Button activation mode for removal
//   * the add is all-or-nothing: a side with no space for a Button refuses
//     the whole edit before anything is created
//   * a second add is refused once both sides carry a Button
//   * existing authored/YAML Buttons can be removed, falling back to manual
//     activation when no pre-Button mode was recorded; editor-added Buttons
//     restore the Door's recorded pre-Button activation mode
//   * serialization round-trips both Buttons and the recorded mode
//   * agents on both sides press their side's Button and cross, and every
//     device command either Button issues is an open
// Rendering coverage now belongs to smoke/render/DoorButtons.cpp (#294).

#include "Checks.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


#include "core/World.h"
#include "core/Button.h"
#include "core/BulkheadDoor.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Graph.h"
#include "core/Vertex.h"
#include <yaml-cpp/yaml.h>
#include "core/Coordination.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Sector.h"
#include "core/SectorObjectType.h"
#include "core/SerializationWorkData.h"
#include "core/YamlSerializer.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	struct DoorLocation
	{
		uint32_t sectorIndex{ ~0u };
		uint32_t objectIndex{ ~0u };
		std::shared_ptr<const core::DoorSectorObject> object;
	};

	// The ordinary Door in a freshly built two-Room scene, wherever it
	// registered itself.
	DoorLocation findDoor(core::World const& world)
	{
		for (uint32_t s = 0; s < world.getNumSectors(); ++s)
		{
			auto sector = world.getSector(s);
			if (!sector) continue;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
				return { s, i, std::static_pointer_cast<const core::DoorSectorObject>(object) };
			}
		}
		return {};
	}

	std::vector<std::shared_ptr<const core::Button>> buttonsIn(core::World const& world,
		uint32_t sectorIndex)
	{
		std::vector<std::shared_ptr<const core::Button>> buttons;
		auto sector = world.getSector(sectorIndex);
		require(sector != nullptr, "The Sector vanished");
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
		{
			auto const object = sector->getObject(i);
			if (!object || object->getObjectType() != core::SectorObjectType::InteractionPoint)
				continue;
			buttons.push_back(std::static_pointer_cast<const core::Button>(object->_getObject()));
		}
		return buttons;
	}

	// Two Rooms on adjacent Layers with one ordinary Door between them.
	// doorOptions, when given, are applied to the Door at creation.
	struct Scene
	{
		std::unique_ptr<core::World> world;
		DoorLocation door;
		uint32_t frontSector{ ~0u };
		uint32_t backSector{ ~0u };

		core::World& operator*() const { return *world; }
		core::World* operator->() const { return world.get(); }
	};

	Scene buildTwoRoomScene(char const* name, uint32_t foreWidth = 8, uint32_t foreX = 0,
		core::World::CreateDoorOptions const* doorOptions = nullptr, uint32_t doorX = ~0u)
	{
		Scene scene;
		scene.world = std::make_unique<core::World>(name, 16, 2);
		while (scene.world->getLayerCount() < 2) scene.world->addLayer();
		scene.world->addRoom("Fore", 0, 0, foreX, foreWidth, 1);
		scene.world->addRoom("Aft", 1, 0, 0, 16, 1);
		if (doorX == ~0u) doorX = foreX + 2;
		auto const created = doorOptions
			? scene.world->addSectorDoor(0, 0, doorX, *doorOptions)
			: scene.world->addSectorDoor(0, 0, doorX, {});
		scene.world->finishBuild();
		scene.frontSector = created.door.sector->getIndex();
		scene.door = findDoor(*scene.world);
		require(scene.door.object != nullptr, "The scene's Door could not be found");
		scene.backSector = scene.door.object->getDoor()->getBackSector()->getIndex();
		return scene;
	}

	void requireBothSidesHaveOneButton(Scene const& scene, char const* what)
	{
		auto const front = buttonsIn(*scene.world, scene.frontSector);
		auto const back = buttonsIn(*scene.world, scene.backSector);
		require(front.size() == 1 && back.size() == 1,
			std::string(what) + ": the Door should carry exactly one Button per side");
		require(front[0]->getThresholdLayer() == 0 && back[0]->getThresholdLayer() == 0,
			std::string(what) + ": a Door Button did not inherit the Door's authored Layer");
	}

	void placementBoundaryPreservesLegacyPolicy()
	{
		using namespace core::physicalControl;
		auto right = legacyCandidates(3, CORE_SIDE_RIGHT, 2, CORE_SIDE_LEFT);
		require(right.size() == 2 && right[0].centreKey() == 16
			&& right[1].centreKey() == 8, "Legacy candidate generation changed");
		Demand flexible{ right, 0, 0, {}, false };
		Demand fixed{ legacyCandidates(4, CORE_SIDE_LEFT), 0, 0, {}, false };
		// Distinct registrations with coincident centres must still use legacy
		// separation, not the future stacking/canonical policy.
		require(allocateLegacy({ flexible, fixed }) == std::vector<uint32_t>{1, 0},
			"Legacy allocator no longer separates coincident registrations");
		flexible.currentCandidate = 1;
		require(allocateLegacy({ flexible }) == std::vector<uint32_t>{1},
			"Prefactor activated preferred-side canonical reassignment");
		require(flexible.currentCandidate == 1 && flexible.candidates[0].cellX == 3,
			"Allocation mutated its input demand");
		bool refused = false;
		try { (void)allocateLegacy({ fixed, fixed }); }
		catch (std::runtime_error const&) { refused = true; }
		require(refused, "Duplicate legacy slots must remain impossible");

		for (int offset = 0; offset < 4; ++offset)
		{
			auto candidate = Candidate::explicitHost(4, offset, CORE_SIDE_MIDDLE);
			require(candidate.cellX == 4 && candidate.centreX() == 4 + offset * 0.25f,
				"Explicit host/quarter-cell centre changed ownership or coordinate");
		}
		refused = false;
		try { (void)Candidate::explicitHost(4, 4, CORE_SIDE_LEFT); }
		catch (std::invalid_argument const&) { refused = true; }
		require(refused, "Explicit offset 1.0 must use the next host cell");

		// Every stationary owner has the same ID-free geometry/role contract.
		for (auto type : { OwnerType::Airlock, OwnerType::BulkheadDoor, OwnerType::Door,
			OwnerType::Dumbwaiter, OwnerType::ForceBridge, OwnerType::Ladder,
			OwnerType::Lift, OwnerType::LocationLightSwitch, OwnerType::PlatformLift, OwnerType::Shuttle })
		{
			flexible.hasOwner = true;
			flexible.owner = { type, { 1, 2, 0, 3, 2 }, { 0, 0, 0, 8, 2 }, { 1, 4, 1 } };
			require(allocateLegacy({ flexible }) == std::vector<uint32_t>{1},
				"Owner metadata changed legacy policy");
		}
	}

	void authoredPlacementPositionsRemainUnchanged()
	{
		for (uint32_t width : {1u, 2u})
			for (bool fallback : {false, true})
			{
				auto options = core::World::RemoteControlledDoor1Options;
				options.width = width;
				uint32_t doorX = fallback ? 8 - width : 2;
				auto scene = buildTwoRoomScene("Legacy button positions", 8, 0, &options, doorX);
				auto front = buttonsIn(*scene.world, scene.frontSector);
				auto back = buttonsIn(*scene.world, scene.backSector);
				require(front.size() == 1 && back.size() == 1, "Required approach control lost");
				float frontX = fallback ? static_cast<float>(doorX) : static_cast<float>(doorX + width);
				for (auto const& [button, expected] : {std::pair{front[0], frontX},
					std::pair{back[0], static_cast<float>(doorX + width)}})
				{
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(std::abs(centre.x - expected) < 0.00001f,
						"One/multi-cell legacy preferred/fallback position changed");
					auto point = scene->lookupInteractionPoint(button->getInteractionPointId());
					require(point && std::abs(point.entity->getPosition().x - expected) < 0.00001f,
						"Production interaction approach drifted from its Button");
				}
			}
	}

	void wallSafeAuthoring()
	{
		for (uint32_t width : { 1u, 3u })
			for (bool leftWall : { false, true })
				for (bool rightHost : { false, true })
				{
					core::World world("Wall-safe candidates", 16, 3);
					if (leftWall) world.addRoom("Neighbour", 0, 0, 0, 4, 1);
					auto front = world.addRoom("Front", 0, 0, 4, width + (rightHost ? 1 : 0), 1);
					auto back = world.addRoom("Back", 1, 0, 0, 16, 1);
					auto options = core::World::RemoteControlledDoor1Options;
					options.width = width;
					auto beforeObjects = world.getSector(front)->getNumObjects();
					bool refused = false;
					try { world.addSectorDoor(0, 0, 4, options); }
					catch (core::Exception const&) { refused = true; }
					require(refused == (leftWall && !rightHost), "Door host/wall feasibility mismatch");
					if (refused)
					{
						require(world.getSector(front)->getNumObjects() == beforeObjects
							&& buttonsIn(world, back).empty(), "Refused creation partially mutated either approach");
						continue;
					}
					world.finishBuild();
					for (auto const& [sector, expected] : { std::pair{ front, rightHost ? 4.0f + width : 4.0f },
						std::pair{ back, 4.0f + width } })
					{
						auto button = buttonsIn(world, sector).at(0);
						require(std::abs((button->getPosition() + button->getSize() * 0.5f).x - expected) < 0.00001f,
							"Independent full-width candidate not applied");
						auto point = world.lookupInteractionPoint(button->getInteractionPointId());
						require(point && point.entity->getPosition().x == expected
							&& point.entity->getSector().value == uint64_t(sector) + 1,
							"Approach ownership/position changed");
						auto const& cell = std::as_const(world).getLayer(sector == front ? 0 : 1)->getCellDefinition((uint32_t)expected, 0);
						require(cell.controls[CORE_SIDE_LEFT] != ~0u, "Offset-zero Button not registered in its host cell");
						uint32_t vertices = 0;
						for (auto const& vertex : world.getGraph()->getVertices())
							if (vertex->getObject() == button)
							{
								++vertices;
								require(vertex->getPosition() == core::Vector2{ expected, 0.0f }
									&& vertex->getSector()->getIndex() == sector,
									"Graph approach is not at the approved host position");
							}
						require(vertices == 1, "Required Button lost its unique production graph approach");
					}
				}

		// A removed shared wall permits the left boundary, but restoring it
		// cannot silently drop a control when the right host is absent.
		core::World world("Wall edit transaction", 12, 3);
		auto neighbour = world.addRoom("Left", 0, 0, 0, 4, 1);
		auto front = world.addRoom("Front", 0, 0, 4, 2, 1);
		world.addRoom("Back", 1, 0, 0, 12, 1);
		world.removeLocationWall(neighbour, 0, CORE_SIDE_RIGHT);
		auto options = core::World::RemoteControlledDoor1Options;
		options.width = 2;
		world.addSectorDoor(0, 0, 4, options);
		world.finishBuild();
		auto button = buttonsIn(world, front).at(0);
		auto point = button->getInteractionPointId();
		auto graph = world.getGraph();
		auto generation = world.getTopologyGeneration();
		bool refused = false;
		try { world.addLocationWall(neighbour, 0, CORE_SIDE_RIGHT); }
		catch (core::Exception const&) { refused = true; }
		require(refused && world.getGraph() == graph && world.getTopologyGeneration() == generation
			&& buttonsIn(world, front).at(0) == button && world.lookupInteractionPoint(point),
			"Invalid wall restoration was not atomic");
		require(world.getSector(front)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::None,
			"Refused wall restoration retained a wall");
		core::SerializationWorkData data;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, data); writer->serialize();
		core::World loaded("Placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
		loaded.deserialize(*reader, data);
		require(buttonsIn(loaded, front).at(0)->getPosition() == button->getPosition(),
			"Removed-wall replay changed placement");
		auto invalid = YAML::Load(writer->getSerializedString());
		YAML::Node records(YAML::NodeType::Sequence);
		for (auto record : invalid["construction"])
			if (record["type"].as<std::string>() != "removeWall") records.push_back(record);
		invalid["construction"] = records;
		auto invalidReader = core::YamlSerializer::fromString(YAML::Dump(invalid)); invalidReader->deserialize();
		auto loadedGraph = loaded.getGraph();
		refused = false;
		try { refused = !loaded.deserialize(*invalidReader, data); }
		catch (std::exception const&) { refused = true; }
		require(refused && loaded.getGraph() == loadedGraph
			&& buttonsIn(loaded, front).at(0)->getPosition() == button->getPosition(),
			"Invalid authored load was not transactional");

		// Centred switches retain their authored cell, including a nonzero base
		// Level, and are unaffected by shared walls on either end.
		for (int type = 0; type < 3; ++type)
		{
			core::World switches("Centred switch", 12, 4);
			switches.addRoom("Left", 0, 1, 0, 4, 1);
			auto host = type == 0 ? switches.addRoom("Room", 0, 1, 4, 1, 1)
				: type == 1 ? switches.addCorridor(0, 1, 4, 1, 1)
				: switches.addFacade(0, 1, 4, 1, 1);
			switches.addRoom("Right", 0, 1, 5, 7, 1);
			switches.addSectorLightSwitch(host, 0);
			switches.finishBuild();
			auto light = buttonsIn(switches, host).at(0);
			require((light->getPosition() + light->getSize() * 0.5f).x == 4.5f,
				"Light switch relocated away from authored centre");
			auto interaction = switches.lookupInteractionPoint(light->getInteractionPointId());
			require(interaction && interaction.entity->getPosition().y == 1.0f,
				"Switch lost authored Location base Level");
			auto count = switches.getSector(host)->getNumObjects();
			refused = false;
			try { switches.addSectorLightSwitch(host, 1); }
			catch (core::Exception const&) { refused = true; }
			require(refused && switches.getSector(host)->getNumObjects() == count,
				"Out-of-host switch authoring was not atomic");
			refused = false;
			try { switches.addSectorLightSwitch(host, 0); }
			catch (core::Exception const&) { refused = true; }
			require(refused && switches.getSector(host)->getNumObjects() == count,
				"Coincident switches were stacked or partially created");
		}
	}

	void wallSafeSupportAndBulkheads()
	{
		for (int type = 0; type < 3; ++type)
		{
			core::World world("Door host kinds", 12, 3);
			auto front = type == 0 ? world.addRoom("Room", 0, 0, 0, 10, 1)
				: type == 1 ? world.addCorridor(0, 0, 0, 10, 1) : world.addFacade(0, 0, 0, 10, 1);
			world.addRoom("Back", 1, 0, 0, 12, 1);
			auto created = world.addSectorDoor(0, 0, 2, core::World::RemoteControlledDoor1Options);
			world.finishBuild(); world.pauseSimulation();
			auto button = buttonsIn(world, front).at(0);
			auto center = button->getPosition() + button->getSize() * 0.5f;
			require(center.x == 3.0f, "Door host type changed right preference");
			std::shared_ptr<const core::SectorObject> selected;
			require(world.getObjectAtPosition(0, center.x - CORE_BUTTON_SIZE * 0.25f, center.y, &selected) == button
				&& selected->getCellX() == 3, "Hit test lost boundary-owned Button's preceding-cell half");
			// Resize and move go through production replay; width changes must use
			// the complete new footprint rather than the old selected host.
			auto resize = world.planResizeSectorDoor(front, created.door.index, 2, 0, 2, 1);
			require(resize.valid, "Valid Door resize refused");
			auto resized = world.applyObjectMove(resize);
			require(resized && (buttonsIn(world, front).at(0)->getPosition()
				+ buttonsIn(world, front).at(0)->getSize() * 0.5f).x == 4.0f,
				"Resize retained stale Door control width");
		}

		core::World world("Support and controlled threshold", 12, 3);
		auto front = world.addRoom("Front", 0, 1, 0, 12, 1);
		auto left = world.addRoom("Left", 1, 0, 0, 4, 2);
		auto back = world.addRoom("Back", 1, 0, 4, 2, 2);
		for (uint32_t x = 0; x < 4; ++x) world.addSectorWalkway(left, 1, x);
		world.addSectorWalkway(back, 1, 0);
		world.removeLocationWall(left, 1, CORE_SIDE_RIGHT);
		auto options = core::World::RemoteControlledDoor1Options;
		auto created = world.addSectorDoor(0, 1, 4, options);
		world.finishBuild(); world.pauseSimulation();
		require((buttonsIn(world, back).at(0)->getPosition()
			+ buttonsIn(world, back).at(0)->getSize() * 0.5f).x == 4.0f,
			"Missing right support did not choose left in its own approach");
		// Adding the right support reselects the preferred candidate.
		auto walkway = world.addSectorWalkway(back, 1, 1);
		world.finishBuild();
		require((buttonsIn(world, back).at(0)->getPosition()
			+ buttonsIn(world, back).at(0)->getSize() * 0.5f).x == 5.0f,
			"Support edit failed to restore right preference");
		core::World::CreateBulkheadDoorOptions bulkOptions;
		bulkOptions.controls[0] = bulkOptions.controls[1] = false;
		require(world.removeSectorWalkway(back, walkway.index), "Supported fallback Walkway removal failed");
		require((buttonsIn(world, back).at(0)->getPosition()
			+ buttonsIn(world, back).at(0)->getSize() * 0.5f).x == 4.0f,
			"Support removal did not choose the remaining valid candidate");
		auto before = world.getSector(back)->getNumObjects();
		bool refused = false;
		try { world.addSectorBulkheadDoor(1, 1, 4, CORE_SIDE_LEFT, bulkOptions); }
		catch (core::Exception const&) { refused = true; }
		require(refused && world.getSector(back)->getNumObjects() == before,
			"New controlled threshold did not refuse impossible ordinary control atomically");
		walkway = world.addSectorWalkway(back, 1, 1);
		world.finishBuild();
		auto bulk = world.addSectorBulkheadDoor(1, 1, 4, CORE_SIDE_LEFT, bulkOptions);
		world.finishBuild();
		auto object = std::dynamic_pointer_cast<const core::BulkheadDoorSectorObject>(bulk.door.sector->getObject(bulk.door.index));
		auto shape = buttonsIn(world, back).at(0)->getPosition();
		for (bool open : { false, true })
		{
			if (open) { object->getDoor()->open(); object->getDoor()->update(100.0f); }
			world.finishBuild();
			require(buttonsIn(world, back).at(0)->getPosition() == shape,
				"Bulkhead runtime state repositioned an ordinary Button");
			auto plan = world.planRemoveSectorWalkway(back, walkway.index);
			require(!plan.valid, "Open/closed controlled threshold must still block fallback across it");
			refused = false;
			try { world.removeSectorWalkway(back, walkway.index); }
			catch (core::Exception const&) { refused = true; }
			require(refused && buttonsIn(world, back).at(0)->getPosition() == shape,
				"Support removal silently dropped a required control");
		}
		// A move to a level without either supported candidate is a preflight
		// refusal; the original Door, graph and interactions remain intact.
		auto move = world.planMoveSectorObject(front, created.door.index, 4, 2);
		require(!move.valid, "Unsupported Door movement must be rejected");
	}

	void canonicalOrderContract()
	{
		using namespace core::physicalControl;
		Demand earlier{ { Candidate::explicitHost(3, 0, CORE_SIDE_LEFT), Candidate::explicitHost(2, 0, CORE_SIDE_LEFT) },
			0, 1, { OwnerType::Door, {0, 2, 0, 1, 1}, {1, 0, 0, 8, 1}, {0, 2, 0} }, true };
		// Exercise every field of the approved tuple, including hosting-Location
		// fields (physical coincidence must not be scoped by their ownership).
		for (int field = 0; field < 14; ++field)
		{
			auto later = earlier;
			auto& g = later.owner.geometry; auto& h = later.owner.hostingLocation; auto& r = later.owner.role;
			switch (field)
			{
			case 0: ++g.x; break;
			case 1: later.owner.type = OwnerType::Dumbwaiter; break;
			case 2: ++g.layer; break;
			case 3: ++g.baseLevel; break;
			case 4: ++g.width; break;
			case 5: ++g.height; break;
			case 6: ++h.layer; break;
			case 7: ++h.x; break;
			case 8: ++h.baseLevel; break;
			case 9: ++h.width; break;
			case 10: ++h.height; break;
			case 11: ++r.order; break;
			case 12: ++r.x; break;
			case 13: ++r.level; break;
			}
			require(allocateCanonical({earlier, later}) == std::vector<uint32_t>{0, 1}
				&& allocateCanonical({later, earlier}) == std::vector<uint32_t>{1, 0},
				"Canonical tuple tie depends on creation order or previous placement: " + std::to_string(field));
		}
		bool refused = false;
		try { (void)allocateCanonical({earlier, earlier}); }
		catch (std::runtime_error const&) { refused = true; }
		require(refused, "Indistinguishable duplicate definitions were ordered by insertion");
	}

	void canonicalSideReassignment()
	{
		// Four Doors across three Layers. The last control has only its left
		// host; admitting it requires A and C to change sides simultaneously.
		for (bool reverse : { false, true })
		{
			core::World world("Canonical reassignment", 12, 2);
			world.addLayer();
			world.addRoom("Front left", 0, 0, 0, 4, 1);
			world.addRoom("Front right", 0, 0, 4, 8, 1);
			auto middle = world.addRoom("Middle", 1, 0, 0, 6, 1);
			world.addRoom("Back", 2, 0, 0, 12, 1);
			world.pauseSimulation();
			core::AccessPermissionId permissions[4];
			for (uint32_t i = 0; i < 4; ++i) permissions[i] = world.addAccessPermission("Control " + std::to_string(i));
			core::World::CreateObjectResult doors[4];
			auto add = [&](uint32_t i)
			{
				auto options = core::World::RemoteControlledDoor1Options;
				options.width = i == 0 ? 2 : 1;
				options.controlPermissionRequirements[i % 2 == 0 ? 1 : 0] = { permissions[i] };
				doors[i] = world.addSectorDoor(i % 2, 0, i + 2, options).door;
			};
			if (reverse) { for (uint32_t i = 4; i-- > 0;) add(i); }
			else { for (uint32_t i = 0; i < 3; ++i) add(i); }
			world.finishBuild(); world.pauseSimulation();
			auto centres = [&]
			{
				std::vector<float> result;
				for (auto button : buttonsIn(world, middle))
					result.push_back((button->getPosition() + button->getSize() * 0.5f).x);
				std::sort(result.begin(), result.end()); return result;
			};
			std::vector<core::InteractionPointId> retained;
			for (auto button : buttonsIn(world, middle)) retained.push_back(button->getInteractionPointId());
			if (!reverse)
			{
				require(centres() == std::vector<float>{3, 4, 5}, "Preferred-side tie did not favour earlier owner X");
				add(3); world.finishBuild();
			}
			require(centres() == std::vector<float>{2, 3, 4, 5}, "Simultaneous side reassignment was missed");
			for (auto id : retained) require(bool(world.lookupInteractionPoint(id)), "Reassignment replaced an existing interaction identity");
			core::InteractionPointId points[4];
			core::TraversalResourceId resources[4];
			for (uint32_t i = 0; i < 4; ++i)
			{
				auto door = std::dynamic_pointer_cast<const core::DoorSectorObject>(doors[i].sector->getObject(doors[i].index));
				resources[i] = door->getDoor()->getTraversalResourceId();
				auto resource = world.lookupTraversalResource(resources[i]);
				bool found = false;
				for (auto pointId : resource.entity->getControls())
				{
					auto point = world.lookupInteractionPoint(pointId);
					if (point.entity->getSector().value != uint64_t(middle) + 1) continue;
					found = true; points[i] = pointId;
					require(point.entity->getPosition().x == float(i + 2), "Creation order changed selected control's assignment");
					require(world.getInteractionPointPermissionRequirement(pointId) == std::vector<core::AccessPermissionId>{permissions[i]},
						"Reassignment merged or lost independent authorization");
				}
				require(found, "Required independent control was lost");
			}
			for (auto button : buttonsIn(world, middle))
			{
				auto centre = button->getPosition() + button->getSize() * 0.5f;
				auto point = world.lookupInteractionPoint(button->getInteractionPointId());
				require(point && point.entity->getPosition().x == centre.x, "Interaction targeting retained an old position");
				std::shared_ptr<const core::SectorObject> selected;
				require(world.getObjectAtPosition(1, centre.x, centre.y, &selected) == button,
					"Hit targeting selected another reassigned control");
				uint32_t approaches = 0;
				for (auto vertex : world.getGraph()->getVertices())
					if (vertex->getObject() == button)
					{
						++approaches;
						require(vertex->getPosition() == point.entity->getPosition(), "Graph retained a previous approach position");
					}
				require(approaches == 1, "Reassignment merged or omitted a graph approach");
			}
			auto before = world.getGraph();
			auto shape = centres();
			auto options = core::World::RemoteControlledDoor1Options;
			bool refused = false;
			// Another Door at x=2 on Layer 1 has only {2,3}, both occupied.
			try { world.addSectorDoor(1, 0, 2, options); }
			catch (core::Exception const&) { refused = true; }
			require(refused && world.getGraph() == before && centres() == shape,
				"Unsatisfiable cross-owner layout was not refused atomically");
			auto actor = world.createAgent("Selected control operator", middle, 0, 3.0f);
			require(world.grantAgentAccessPermission(actor, permissions[1]), "Could not authorize selected control");
			require(world.resumeSimulation(), "Reassigned scene could not resume");
			auto denied = world.requestInteraction(points[0], actor);
			auto rejected = world.lookupInteractionRequest(denied);
			require(rejected && rejected.entity->getResult() == core::InteractionResult::Rejected
				&& rejected.entity->getMissingPermissions() == std::vector<core::AccessPermissionId>{permissions[0]}
				&& rejected.entity->getOperations().empty(), "Reassignment bypassed another control's authorization");
			auto requested = world.requestInteraction(points[1], actor);
			auto admitted = world.lookupInteractionRequest(requested);
			require(admitted && admitted.entity->getOperations().size() == 1, "Selected reassigned control lost its command");
			auto operation = world.lookupDeviceOperation(admitted.entity->getOperations().front().first);
			require(operation && operation.entity->getCommand().type == core::DeviceCommandType::OpenDoor
				&& operation.entity->getCommand().traversalResource == resources[1], "Reassignment operated a different owner");
			world.advanceTicks(300); world.pauseSimulation();
			core::SerializationWorkData data;
			auto writer = core::YamlSerializer::toString(); world.serialize(*writer, data); writer->serialize();
			core::World loaded("Placeholder", 1, 1);
			auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
			require(loaded.deserialize(*reader, data), "Canonical layout did not load/replay");
			std::vector<float> restored;
			for (auto button : buttonsIn(loaded, middle)) restored.push_back((button->getPosition() + button->getSize() * 0.5f).x);
			std::sort(restored.begin(), restored.end());
			require(restored == shape, "Replay changed canonical layout");
			require(world.removeSectorDoor(doors[3].sector->getIndex(), doors[3].index), "Door deletion refused");
			world.finishBuild();
			require(centres() == std::vector<float>{3, 4, 5}, "Deletion retained previous side assignments");
		}
	}

	void addingAButtonGivesBothSidesOne()
	{
		Scene scene = buildTwoRoomScene("Add door button");
		// Add from the back side's registration: the action must not care which
		// side of the pair the selected Sector sits on.
		uint32_t backObjectIndex{ ~0u };
		auto backSector = scene.world->getSector(scene.backSector);
		for (uint32_t i = 0; i < backSector->getNumObjects(); ++i)
			if (backSector->getObject(i) == scene.door.object) backObjectIndex = i;
		require(backObjectIndex != ~0u, "The Door is not registered on its back Sector");

		scene->pauseSimulation();
		require(scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A Button-less Door should accept an added Door Button");
		scene->addSectorDoorButton(scene.backSector, backObjectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Adding a Door Button");
		require(!scene->canAddSectorDoorButton(scene.backSector, backObjectIndex),
			"A both-sided Door should not accept another Door Button");
		require(scene->canRemoveSectorDoorButton(scene.backSector, backObjectIndex),
			"Editor-added Door Buttons should be removable");

		auto const resource = scene->lookupTraversalResource(
			scene.door.object->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().size() == 2,
			"Both Buttons should bind to the Door's traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, scene.door.object->getCellX(),
				scene.door.object->getDoor()->getCellsWide(), options),
			"The Door's record could not be read back");
		require(options.controls[0] && options.controls[1]
			&& options.activationMode == core::DoorActivationMode::RemoteControlled,
			"The Door record did not move to remote-controlled with both controls");
	}

	void addingCompletesALegacyOneSidedDoor()
	{
		// A Door authored with only its fore Button, as older builds produced.
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Complete a legacy one-sided Door", 8, 0, &authored);
		require(buttonsIn(*scene.world, scene.frontSector).size() == 1
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"The authored Door should start with exactly its fore Button");

		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		requireBothSidesHaveOneButton(scene, "Completing a one-sided Door");
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"A completed legacy Door should allow its Buttons to be removed");
	}

	void addRefusesWhenASideHasNoSpace()
	{
		// The right host is outside the required Location; a retained shared
		// left wall also eliminates the boundary-owned left candidate.
		Scene scene = buildTwoRoomScene("No space for a Button", 1, 5, nullptr, 5);
		scene->pauseSimulation();
		scene->addRoom("Left neighbour", 0, 0, 0, 5, 1);
		scene->finishBuild();
		require(scene.door.object->getCellX() == 5
			&& scene.door.object->getDoor()->getCellsWide() == 1,
			"The scene's Door should span its fore Room exactly");

		scene->pauseSimulation();
		auto const buttonsBefore = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "An add with no space on one side should be refused");
		auto const buttonsAfter = buttonsIn(*scene.world, scene.frontSector).size()
			+ buttonsIn(*scene.world, scene.backSector).size();
		require(buttonsBefore == 0 && buttonsAfter == 0,
			"A refused add must not leave a Button behind on either side");
		require(!scene->canAddSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Editor feasibility must use the same wall-safe policy");
	}

	void secondAddIsRefused()
	{
		Scene scene = buildTwoRoomScene("Second add refused");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		bool refused = false;
		try
		{
			scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		}
		catch (core::Exception const&)
		{
			refused = true;
		}
		require(refused, "A both-sided Door should refuse a second add");
		requireBothSidesHaveOneButton(scene, "A refused second add");
	}

	void authoredButtonsCanBeRemoved()
	{
		core::World::CreateDoorOptions authored;
		authored.controls[0] = true;
		authored.controls[1] = true;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		Scene scene = buildTwoRoomScene("Authored Buttons", 8, 0, &authored);
		requireBothSidesHaveOneButton(scene, "The authored Door");

		scene->pauseSimulation();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Authored Buttons should read as removable");
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removing authored Buttons should rebuild the Door");
		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removing authored Buttons should take both away");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, rebuilt->getCellX(),
				rebuilt->getDoor()->getCellsWide(), options),
			"The Door record could not be read after removing authored Buttons");
		require(options.activationMode == core::DoorActivationMode::Manual,
			"A Door with removed authored Buttons should fall back to manual activation");
	}

	void removalRemovesBothButtonsAndRestoresTheMode()
	{
		Scene scene = buildTwoRoomScene("Remove door button");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->canRemoveSectorDoorButton(scene.frontSector, scene.door.objectIndex),
			"Editor-added Buttons should read as removable");

		auto const doorX = scene.door.object->getCellX();
		auto const doorWidth = scene.door.object->getDoor()->getCellsWide();
		auto const rebuilt = scene->removeSectorDoorButton(scene.frontSector,
			scene.door.objectIndex);
		require(rebuilt != nullptr, "Removal should return the rebuilt Door");
		require(rebuilt->getCellX() == doorX && rebuilt->getDoor()->getCellsWide() == doorWidth,
			"Removal returned a Door at the wrong position");

		require(buttonsIn(*scene.world, scene.frontSector).empty()
			&& buttonsIn(*scene.world, scene.backSector).empty(),
			"Removal should take both Buttons away");
		auto const resource = scene->lookupTraversalResource(
			rebuilt->getDoor()->getTraversalResourceId());
		require(resource && resource.entity->getControls().empty(),
			"Removal should unbind every Button from the traversal resource");

		core::World::CreateDoorOptions options;
		require(scene->getSectorDoorOptions(0, 0, doorX, doorWidth, options),
			"The rebuilt Door's record could not be read back");
		require(!options.controls[0] && !options.controls[1]
			&& options.activationMode == core::DoorActivationMode::Manual,
			"Removal should restore the Door's pre-Button activation mode");

		uint32_t rebuiltIndex{ ~0u };
		auto sector = scene.world->getSector(scene.frontSector);
		for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			if (sector->getObject(i) == rebuilt) rebuiltIndex = i;
		require(rebuiltIndex != ~0u, "The rebuilt Door is not on its front Sector");
		require(scene->canAddSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Button-less Door should accept Buttons again");
		require(!scene->canRemoveSectorDoorButton(scene.frontSector, rebuiltIndex),
			"The rebuilt Door should not read as removable");
	}

	void serializationPreservesButtonsAndProvenance()
	{
		Scene scene = buildTwoRoomScene("Serialize two-sided Buttons");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		scene->serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("preButtonActivation") != std::string::npos,
			"The pre-Button activation mode did not persist");

		auto loaded = std::make_unique<core::World>("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		loaded->deserialize(*reader, workData);

		auto const loadedDoor = findDoor(*loaded);
		require(loadedDoor.object != nullptr, "The loaded World has no Door");
		require(buttonsIn(*loaded, loadedDoor.object->getDoor()->getFrontSector()->getIndex()).size() == 1
			&& buttonsIn(*loaded, loadedDoor.object->getDoor()->getBackSector()->getIndex()).size() == 1,
			"The loaded Door did not keep one Button per side");
		for (auto sector : { scene.frontSector, scene.backSector })
		{
			auto before = buttonsIn(*scene.world, sector)[0];
			auto after = buttonsIn(*loaded, sector)[0];
			require(before->getPosition() == after->getPosition() && before->getSize() == after->getSize(),
				"Save/load reconstruction changed physical Button geometry");
			auto beforePoint = scene->lookupInteractionPoint(before->getInteractionPointId());
			auto afterPoint = loaded->lookupInteractionPoint(after->getInteractionPointId());
			require(beforePoint && afterPoint && beforePoint.entity->getPosition() == afterPoint.entity->getPosition(),
				"Save/load reconstruction changed interaction approach position");
		}
		require(loaded->canRemoveSectorDoorButton(loadedDoor.sectorIndex, loadedDoor.objectIndex),
			"The loaded Door lost its Buttons' removal provenance");
	}

	void agentsOnBothSidesPressAndCross()
	{
		Scene scene = buildTwoRoomScene("Cross from both sides");
		scene->pauseSimulation();
		scene->addSectorDoorButton(scene.frontSector, scene.door.objectIndex);
		scene->finishBuild();
		require(scene->resumeSimulation(), "The scene should resume after its edit");
		auto const door = scene.door.object->getDoor();
		auto const doorResource = door->getTraversalResourceId();

		auto graph = scene->getGraph();
		auto foreTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.frontSector).get(), { 2.0f, 0.0f });
		auto backTarget = graph->getClosestVertexInSector(
			scene.world->getSector(scene.backSector).get(), { 13.0f, 0.0f });
		require(foreTarget && backTarget, "The scene needs a target vertex on each side");

		auto const foreWalker = scene->createAgent("Fore walker", scene.frontSector, 0, 2.0f);
		auto const backWalker = scene->createAgent("Back walker", scene.backSector, 0, 13.0f);
		for (auto const& [id, target] : { std::pair{ foreWalker, backTarget },
			std::pair{ backWalker, foreTarget } })
		{
			auto agent = scene->lookupAgent(id).entity;
			require(agent != nullptr, "An Agent could not be created");
			auto path = graph->calculatePath(agent, target);
			require(path != nullptr, "An Agent's path could not be calculated");
			agent->setPath(path, true);
		}

		bool sawDoorOpen = false;
		bool sawOpenCommand = false;
		bool sawCloseCommand = false;
		for (uint32_t tick = 0; tick < 6000; ++tick)
		{
			scene->advanceTick();
			sawDoorOpen = sawDoorOpen || door->isOpen();
			auto const snapshot = scene->getSimulationSnapshot();
			for (auto const& operation : snapshot.deviceOperations)
			{
				if (operation.command.type != core::DeviceCommandType::OpenDoor
					|| operation.command.traversalResource != doorResource) continue;
				sawOpenCommand = sawOpenCommand || operation.command.desiredState;
				sawCloseCommand = sawCloseCommand || !operation.command.desiredState;
			}
			auto const foreIdle = scene->lookupAgent(foreWalker).entity->getState()
				== core::Agent::State::Idle;
			auto const backIdle = scene->lookupAgent(backWalker).entity->getState()
				== core::Agent::State::Idle;
			if (foreIdle && backIdle) break;
		}

		auto const foreAgent = scene->lookupAgent(foreWalker).entity;
		auto const backAgent = scene->lookupAgent(backWalker).entity;
		require(foreAgent->getState() == core::Agent::State::Idle
			&& backAgent->getState() == core::Agent::State::Idle,
			"Agents on both sides should finish their crossing");
		require(foreAgent->getGlobalPosition().distanceTo(backTarget->getPosition()) < 0.01f
			&& backAgent->getGlobalPosition().distanceTo(foreTarget->getPosition()) < 0.01f,
			"Agents on both sides should arrive at their destinations");
		require(sawDoorOpen, "The Door should have opened for the crossings");
		require(sawOpenCommand, "The Buttons should have issued open commands");
		require(!sawCloseCommand,
			"Neither Button may ever issue a close command: pressing the other one again must not close the Door");
	}


}

void runDoorTwoSidedButtonSmokeChecks()
{
	placementBoundaryPreservesLegacyPolicy();
	canonicalOrderContract();
	canonicalSideReassignment();
	wallSafeAuthoring();
	wallSafeSupportAndBulkheads();
	authoredPlacementPositionsRemainUnchanged();
	addingAButtonGivesBothSidesOne();
	addingCompletesALegacyOneSidedDoor();
	addRefusesWhenASideHasNoSpace();
	secondAddIsRefused();
	authoredButtonsCanBeRemoved();
	removalRemovesBothButtonsAndRestoresTheMode();
	serializationPreservesButtonsAndProvenance();
	agentsOnBothSidesPressAndCross();
}
