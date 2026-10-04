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
#include <map>
#include <tuple>
#include <type_traits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>


#include "core/World.h"
#include "core/Button.h"
#include "core/AirlockTransit.h"
#include "core/ForceBridgeSectorObject.h"
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
#include "core/BinarySerializer.h"

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

	void canonicalOnlyBoundary()
	{
		using namespace core::physicalControl;
		for (int offset = 0; offset < 4; ++offset)
		{
			auto candidate = Candidate::explicitHost(4, offset);
			require(candidate.cellX == 4 && candidate.centreX() == 4 + offset * 0.25f,
				"Explicit host/quarter-cell centre changed ownership or coordinate");
		}
		bool refused = false;
		try { (void)Candidate::explicitHost(4, 4); }
		catch (std::invalid_argument const&) { refused = true; }
		require(refused, "Explicit offset 1.0 must use the next host cell");
		// Every owner uses canonical preference, with no prior-placement input.
		for (auto type : { OwnerType::Airlock, OwnerType::BulkheadDoor, OwnerType::Door,
			OwnerType::Dumbwaiter, OwnerType::ForceBridge, OwnerType::Ladder,
			OwnerType::Lift, OwnerType::LocationLightSwitch, OwnerType::PlatformLift, OwnerType::Shuttle })
		{
			Demand demand{{Candidate::explicitHost(4, 0), Candidate::explicitHost(3, 0)}, 0,
				{type, {1, 2, 0, 3, 2}, {0, 0, 0, 8, 2}, {1, 4, 1}}};
			require(allocateCanonical({demand}) == std::vector<uint32_t>{0}, "Owner bypassed canonical preference");
			refused = false;
			try { (void)allocateCanonical({demand, demand}); }
			catch (std::runtime_error const&) { refused = true; }
			require(refused, "Duplicate definitions escaped canonical validation");
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
						require(!cell.physicalControls.empty(), "Offset-zero Button not registered in its host cell");
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
		Demand earlier{ { Candidate::explicitHost(3, 0), Candidate::explicitHost(2, 0) },
			0, { OwnerType::Door, {0, 2, 0, 1, 1}, {1, 0, 0, 8, 1}, {0, 2, 0} } };
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
			require(canonicalLess(earlier.owner, later.owner) && !canonicalLess(later.owner, earlier.owner),
				"Stack order did not use the complete canonical tuple");
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
			// A duplicate Door footprint is still an atomic refusal.
			try { world.addSectorDoor(1, 0, 3, options); }
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

	void completeOptimisation()
	{
		using namespace core::physicalControl;
		auto demand = [](uint32_t ownerX, std::initializer_list<uint32_t> positions)
		{
			Demand d;
			d.owner = {OwnerType::Door, {0, ownerX, 0, 1, 1}, {1, 0, 0, 20, 1}, {0, ownerX, 0}};
			for (auto x : positions) d.candidates.push_back(Candidate::explicitHost(x, 0));
			return d;
		};
		auto solve = [&](std::vector<Demand> const& input, std::vector<uint32_t> const& expected, char const* tier)
		{
			require(allocateCanonical(input) == expected, tier);
			auto reversed = input; std::reverse(reversed.begin(), reversed.end());
			auto choice = allocateCanonical(reversed); std::reverse(choice.begin(), choice.end());
			require(choice == expected, "Creation order changed optimisation");
		};
		// Above-bottom count (not coincident pair count) comes before preferences.
		solve({demand(0, {1, 0}), demand(1, {2, 1}), demand(2, {3, 2}), demand(3, {3})},
			{1, 1, 1, 0}, "Collision-free multi-side assignment lost to a stack");
		// Same three above-bottom members: 3+2 beats 4+1 even at extra penalty.
		solve({demand(0, {0}), demand(1, {0}), demand(2, {1}), demand(3, {0, 1}), demand(4, {0, 1})},
			{0, 0, 0, 0, 1}, "Maximum-stack tier did not beat preferred sides/canonical tie");
		// Feasibility, not an early refusal: the preferred branch reaches five.
		solve({demand(0, {0}), demand(1, {0}), demand(2, {0}), demand(3, {0}), demand(4, {0, 1})},
			{0, 0, 0, 0, 1}, "Overcapacity branch hid a feasible assignment");
		solve({demand(0, {0}), demand(1, {0}), demand(2, {1}), demand(3, {0, 1})},
			{0, 0, 0, 1}, "Maximum-stack tier lost to preference");
		// A disconnected forced four-stack sets the GLOBAL max. At that bound,
		// the second component should retain its preferred 3+1, not force 2+2.
		solve({demand(0, {8}), demand(1, {8}), demand(2, {8}), demand(3, {8}),
			demand(4, {0}), demand(5, {0}), demand(6, {1}), demand(7, {0, 1})},
			{0, 0, 0, 0, 0, 0, 0, 0}, "Disconnected components optimised maximum locally");

		// Independent exhaustive oracle for every five-control layout over three
		// centres, including fixed and reversible sides. It compares all tiers
		// only AFTER filtering capacity and hosting-Location feasibility.
		for (uint32_t layout = 0; layout < 7776; ++layout)
		{
			std::vector<Demand> input; auto code = layout;
			for (uint32_t i = 0; i < 5; ++i)
			{
				auto shape = code % 6; code /= 6;
				input.push_back(shape < 3 ? demand(i, {shape}) : demand(i, {shape - 3, (shape - 2) % 3}));
				if (layout % 7 == 0 && i == 4) input.back().owner.hostingLocation.x = 1;
			}
			using Score = std::tuple<uint32_t, uint32_t, uint32_t, std::vector<uint32_t>>;
			Score best{~0u, ~0u, ~0u, {}}; std::vector<uint32_t> expected;
			for (uint32_t mask = 0; mask < 32; ++mask)
			{
				std::vector<uint32_t> choice; std::map<int64_t, std::vector<uint32_t>> groups;
				bool valid = true; uint32_t penalty = 0;
				for (uint32_t i = 0; i < 5; ++i)
				{
					auto side = (mask >> i) & 1;
					if (side >= input[i].candidates.size()) { valid = false; break; }
					choice.push_back(side); penalty += side;
					groups[input[i].candidates[side].centreKey()].push_back(i);
				}
				uint32_t above = 0, maximum = 0;
				for (auto const& [centre, members] : groups)
				{
					(void)centre;
					uint32_t size = static_cast<uint32_t>(members.size());
					above += size - 1; maximum = std::max(maximum, size);
					valid &= size <= 4;
					for (auto i : members) valid &= input[i].owner.hostingLocation.x == input[members.front()].owner.hostingLocation.x;
				}
				Score score{above, maximum, penalty, choice};
				if (valid && score < best) { best = score; expected = choice; }
			}
			bool refused = false; std::vector<uint32_t> actual;
			try { actual = allocateCanonical(input); } catch (std::runtime_error const&) { refused = true; }
			require(refused == expected.empty() && actual == expected, "Allocator disagreed with exhaustive four-tier oracle: " + std::to_string(layout));
		}

		// Public authoring isolates the max tier (two pairs rather than 3+1),
		// then preference count and canonical ties as a retained wall is removed.
		for (bool reverse : {false, true})
		{
			core::World world("Optimised pairs", 10, 1); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 10, 1);
			auto middle = world.addRoom("Middle", 1, 0, 3, 3, 1);
			world.addRoom("Back", 2, 0, 0, 10, 1);
			auto neighbour = world.addRoom("Wall", 1, 0, 0, 3, 1);
			world.pauseSimulation();
			core::AccessPermissionId permissions[4];
			for (uint32_t i = 0; i < 4; ++i) permissions[i] = world.addAccessPermission("Owner " + std::to_string(i));
			for (uint32_t step = 0; step < 4; ++step)
			{
				uint32_t i = reverse ? 3 - step : step;
				auto options = core::World::RemoteControlledDoor1Options;
				options.controlPermissionRequirements[i % 2 == 0 ? 1 : 0] = {permissions[i]};
				world.addSectorDoor(i % 2, 0, i < 2 ? 3 : 4, options);
			}
			world.finishBuild(); world.pauseSimulation();
			auto check = [&](std::vector<float> const& expected)
			{
				auto buttons = buttonsIn(world, middle); require(buttons.size() == 4, "Optimisation dropped controls");
				for (auto button : buttons)
				{
					auto requirement = world.getInteractionPointPermissionRequirement(button->getInteractionPointId());
					auto owner = std::find(std::begin(permissions), std::end(permissions), requirement.front()) - std::begin(permissions);
					auto point = world.lookupInteractionPoint(button->getInteractionPointId());
					require(point && point.entity->getPosition().x == expected[owner], "Public authoring lost an optimisation tier");
					std::shared_ptr<const core::SectorObject> object;
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(world.getObjectAtPosition(1, centre.x, centre.y, &object) == button, "Optimised stack hit failed");
					auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<core::SectorObject>(object));
					require(vertex && vertex->getPosition() == point.entity->getPosition(), "Optimised graph placement differed");
				}
			};
			check({4, 4, 5, 5});
			world.removeLocationWall(neighbour, 0, CORE_SIDE_RIGHT); world.finishBuild();
			check({4, 3, 5, 5});
		}
	}

	void sharedApproachStacks()
	{
		using namespace core::physicalControl;
		Demand first{{Candidate::explicitHost(2, 0)}, 0,
			{OwnerType::Door, {0, 2, 0, 1, 1}, {1, 1, 0, 2, 1}, {1, 2, 0}}};
		auto second = first; second.owner.geometry.layer = 1; second.owner.role.order = 0;
		require(allocateCanonical({first, second}) == std::vector<uint32_t>{0, 0}, "Unavoidable pair refused");
		auto third = second; third.owner.geometry.layer = 2;
		bool refused = false;
		try { (void)allocateCanonical({first, second, third}); } catch (std::runtime_error const&) { refused = true; }
		require(!refused, "Unavoidable triple was refused");
		auto fourth = third; fourth.owner.geometry.layer = 3;
		auto fifth = fourth; fifth.owner.geometry.layer = 4;
		require(allocateCanonical({first, second, third, fourth}) == std::vector<uint32_t>{0, 0, 0, 0}, "Four-stack refused");
		refused = false;
		try { (void)allocateCanonical({first, second, third, fourth, fifth}); } catch (std::runtime_error const&) { refused = true; }
		require(refused, "Unavoidable fifth control was admitted");
		second.owner.hostingLocation.x = 0;
		refused = false;
		try { (void)allocateCanonical({first, second}); } catch (std::runtime_error const&) { refused = true; }
		require(refused, "Stack crossed Locations");


		for (bool reverse : {false, true})
		{
			core::World world("Shared approach", 6, 1); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 6, 1);
			auto middle = world.addRoom("Middle", 1, 0, 1, 2, 1);
			world.addRoom("Back", 2, 0, 0, 6, 1);
			world.pauseSimulation();
			core::AccessPermissionId permissions[2]{world.addAccessPermission("Bottom"), world.addAccessPermission("Upper")};
			core::World::CreateDoorResult doors[2];
			auto add = [&](uint32_t layer)
			{
				auto options = core::World::RemoteControlledDoor1Options;
				options.controlPermissionRequirements[layer == 0 ? 1 : 0] = {permissions[layer]};
				doors[layer] = world.addSectorDoor(layer, 0, 2, options);
			};
			add(reverse ? 1 : 0); add(reverse ? 0 : 1); world.finishBuild(); world.pauseSimulation();
			auto check = [&](core::World& scene)
			{
				auto buttons = buttonsIn(scene, middle);
				require(buttons.size() == 2, "Stack omitted a required Button");
				std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
				require(buttons[0]->getPosition().y == CORE_BUTTON_Y_OFFSET
					&& std::abs(buttons[1]->getPosition().y - buttons[0]->getPosition().y - buttons[0]->getSize().y * 1.25f) < 0.00001f
					&& buttons[0]->getPosition().x == buttons[1]->getPosition().x, "Incorrect stack spacing");
				std::shared_ptr<const core::Vertex> shared;
				for (uint32_t rank = 0; rank < 2; ++rank)
				{
					auto button = buttons[rank]; auto point = scene.lookupInteractionPoint(button->getInteractionPointId());
					require(point && point.entity->getPosition() == core::Vector2{2, 0}, "Upper interaction moved above walkable height");
					require(scene.getInteractionPointPermissionRequirement(button->getInteractionPointId()) == std::vector<core::AccessPermissionId>{permissions[rank]}, "Equal-X canonical order lost owner Layer tie");
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					std::shared_ptr<const core::SectorObject> selected;
					require(scene.getObjectAtPosition(1, centre.x, centre.y, &selected) == button, "Stack hit selected another Button");
					auto object = std::const_pointer_cast<core::SectorObject>(selected);
					auto vertex = scene.getGraph()->getVertexForObject(object);
					require(vertex && vertex->getPosition() == core::Vector2{2, 0}
						&& object->getVertexIdentifier() != ~0u
						&& scene.getGraph()->getVertexByIdentifier(object->getVertexIdentifier()) == vertex, "Stack lookup missing");
					if (shared) require(shared == vertex, "Stack has two production approaches"); else shared = vertex;
				}
				uint32_t count = 0;
				for (auto vertex : scene.getGraph()->getVertices())
					if (vertex->getSector()->getIndex() == middle && vertex->getPosition() == core::Vector2{2, 0}
						&& vertex->getSubType() == core::VertexSubType::Interactable) ++count;
				require(count == 1, "Stack duplicated graph vertex");
				return buttons;
			};
			auto buttons = check(world);
			for (uint32_t rank = 0; rank < 2; ++rank)
			{
				auto actor = world.createAgent("Operator", middle, 0, 1.2f);
				require(world.grantAgentAccessPermission(actor, permissions[rank]), "Stack permission grant failed");
				require(world.resumeSimulation(), "Stack failed topology validation");
				auto denied = world.requestInteraction(buttons[1-rank]->getInteractionPointId(), actor);
				auto rejection = world.lookupInteractionRequest(denied);
				require(rejection && rejection.entity->getResult() == core::InteractionResult::Rejected
					&& rejection.entity->getOperations().empty(), "Shared approach merged authorization");
				auto request = world.requestInteraction(buttons[rank]->getInteractionPointId(), actor);
				require(bool(request), "Selected stack control refused");
				for (int tick = 0; tick < 300; ++tick)
				{
					world.advanceTicks(1);
					require(world.lookupAgent(actor).entity->getGlobalPosition().y == 0, "Agent climbed to upper Button");
					auto progress = world.lookupInteractionRequest(request);
					if (progress && progress.entity->getResult() != core::InteractionResult::Pending) break;
				}
				auto result = world.lookupInteractionRequest(request);
				require(result && result.entity->getResult() == core::InteractionResult::Succeeded
					&& result.entity->getOperations().size() == 1, "Selected stack operation failed: " + std::to_string(result ? int(result.entity->getResult()) : -1));
				auto operation = world.lookupDeviceOperation(result.entity->getOperations().front().first);
				auto door = std::dynamic_pointer_cast<const core::DoorSectorObject>(doors[rank].door.sector->getObject(doors[rank].door.index));
				require(operation && operation.entity->getCommand().traversalResource == door->getDoor()->getTraversalResourceId(), "Stack activated wrong command");
				// Route intent must select the required Door's control, even though
				// the shared vertex itself carries the bottom Button's object.
				door->getDoor()->close(); door->getDoor()->update(100);
				auto destinationSector = rank == 0 ? door->getDoor()->getFrontSector() : door->getDoor()->getBackSector();
				auto destination = world.getGraph()->getClosestVertexInSector(destinationSector.get(), {2.5f, 0});
				auto agent = world.lookupAgent(actor).entity;
				auto path = world.getGraph()->calculatePath(agent, destination);
				require(bool(path), "Stack approach lost selected Door route"); agent->setPath(path, true);
				bool selected = false;
				for (uint32_t tick = 0; tick < 3000; ++tick)
				{
					world.advanceTick();
					require(agent->getGlobalPosition().y == 0, "Routing climbed toward upper artwork");
					for (auto const& interaction : world.getSimulationSnapshot().interactionRequests)
						if (interaction.actor == actor && interaction.id != request && interaction.result != core::InteractionResult::Rejected)
						{
							require(interaction.point == buttons[rank]->getInteractionPointId(), "Route intent operated the other stacked control");
							selected = true;
						}
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(selected && agent->getSector() == destinationSector.get(), "Agent did not activate and cross its intended stacked Door");
				world.pauseSimulation();
			}
			core::SerializationWorkData data; auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, data); writer->serialize();
			core::World loaded("Placeholder", 1, 1); auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
			require(loaded.deserialize(*reader, data), "Stack load/replay failed"); check(loaded);
			auto binary = core::BinarySerializer::toString(); world.serialize(*binary, data); binary->serialize();
			auto binaryReader = core::BinarySerializer::fromString(binary->getSerializedString()); binaryReader->deserialize();
			require(loaded.deserialize(*binaryReader, data), "Binary stack load/replay failed"); check(loaded);
			require(world.removeSectorDoor(doors[0].door.sector->getIndex(), doors[0].door.index), "Stack deletion refused");
			world.finishBuild();
			auto remaining = buttonsIn(world, middle);
			require(remaining.size() == 1 && remaining.front()->getPosition().y == CORE_BUTTON_Y_OFFSET, "Survivor did not unstack");
		}
		for (uint32_t count : {3u, 4u}) for (bool reverse : {false, true})
		{
			core::World world("Four shared approaches", 10, 1); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 10, 1);
			auto middle = world.addRoom("Middle", 1, 0, 3, 2, 1);
			world.addRoom("Back", 2, 0, 0, 10, 1);
			world.addRoom("Retained left wall", 1, 0, 0, 3, 1);
			world.pauseSimulation();
			core::AccessPermissionId permissions[4];
			core::World::CreateDoorResult doors[4];
			for (uint32_t i = 0; i < count; ++i) permissions[i] = world.addAccessPermission("Member " + std::to_string(i));
			// Four non-overlapping footprints, two on each adjacent Layer,
			// all forced to the sole wall-safe host at x=4.
			uint32_t layers[]{0, 1, 0, 1}, xs[]{3, 3, 4, 4}, widths[]{1, 1, 1, 1};
			auto add = [&](uint32_t i)
			{
				auto options = core::World::RemoteControlledDoor1Options; options.width = widths[i];
				options.controlPermissionRequirements[layers[i] == 0 ? 1 : 0] = {permissions[i]};
				doors[i] = world.addSectorDoor(layers[i], 0, xs[i], options);
			};
			for (uint32_t i = 0; i < count; ++i) add(reverse ? count - i - 1 : i);
			world.finishBuild(); world.pauseSimulation();
			auto check = [&](core::World& scene)
			{
				auto buttons = buttonsIn(scene, middle);
				require(buttons.size() == count, "Stack omitted a required Button");
				std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
				require(buttons[0]->getPosition().y == CORE_BUTTON_Y_OFFSET
					&& std::abs(buttons[1]->getPosition().y - buttons[0]->getPosition().y - buttons[0]->getSize().y * 1.25f) < 0.00001f
					&& buttons[0]->getPosition().x == buttons[1]->getPosition().x, "Incorrect stack spacing");
				std::shared_ptr<const core::Vertex> shared;
				for (uint32_t rank = 0; rank < count; ++rank)
				{
					require(std::abs(buttons[rank]->getPosition().y - (CORE_BUTTON_Y_OFFSET + rank * buttons[rank]->getSize().y * 1.25f)) < 0.00001f
						&& buttons[rank]->getPosition().x == buttons[0]->getPosition().x, "Member geometry lost canonical spacing");
					auto button = buttons[rank]; auto point = scene.lookupInteractionPoint(button->getInteractionPointId());
					require(point && point.entity->getPosition() == core::Vector2{4, 0}, "Upper interaction moved above walkable height");
					require(scene.getInteractionPointPermissionRequirement(button->getInteractionPointId()) == std::vector<core::AccessPermissionId>{permissions[rank]}, "Equal-X canonical order lost owner Layer tie");
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					std::shared_ptr<const core::SectorObject> selected;
					require(scene.getObjectAtPosition(1, centre.x, centre.y, &selected) == button, "Stack hit selected another Button");
					auto object = std::const_pointer_cast<core::SectorObject>(selected);
					auto vertex = scene.getGraph()->getVertexForObject(object);
					require(vertex && vertex->getPosition() == core::Vector2{4, 0}
						&& object->getVertexIdentifier() != ~0u
						&& scene.getGraph()->getVertexByIdentifier(object->getVertexIdentifier()) == vertex, "Stack lookup missing");
					if (shared) require(shared == vertex, "Stack has two production approaches"); else shared = vertex;
				}
				uint32_t count = 0;
				for (auto vertex : scene.getGraph()->getVertices())
					if (vertex->getSector()->getIndex() == middle && vertex->getPosition() == core::Vector2{4, 0}
						&& vertex->getSubType() == core::VertexSubType::Interactable) ++count;
				require(count == 1, "Stack duplicated graph vertex");
				return buttons;
			};
			auto buttons = check(world);
			// Failed authoring and malformed document reconstruction leave all
			// members, identities, commands, permissions and graph untouched.
			core::SerializationWorkData refusalData;
			auto before = core::YamlSerializer::toString(); world.serialize(*before, refusalData); before->serialize();
			auto graph = world.getGraph(); bool refused = false;
			try { world.addSectorDoor(0, 0, 3, core::World::RemoteControlledDoor1Options); }
			catch (core::Exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && check(world) == buttons, "Duplicate authoring changed stack");
			auto invalid = YAML::Load(before->getSerializedString());
			YAML::Node duplicate;
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() == "door") { duplicate = YAML::Clone(record); break; }
			require(bool(duplicate), "Stack document has no Door record");
			invalid["construction"].push_back(duplicate);
			auto invalidReader = core::YamlSerializer::fromString(YAML::Dump(invalid)); invalidReader->deserialize();
			refused = false;
			try { refused = !world.deserialize(*invalidReader, refusalData); } catch (std::exception const&) { refused = true; }
			auto after = core::YamlSerializer::toString(); world.serialize(*after, refusalData); after->serialize();
			require(refused && world.getGraph() == graph && check(world) == buttons
				&& before->getSerializedString() == after->getSerializedString(), "Invalid stack reconstruction was not atomic");

			for (uint32_t rank = 0; rank < count; ++rank)
			{
				auto actor = world.createAgent("Operator", middle, 0, rank < 2 ? 0.7f : 1.3f);
				require(world.grantAgentAccessPermission(actor, permissions[rank]), "Stack permission grant failed");
				require(world.resumeSimulation(), "Stack failed topology validation");
				auto denied = world.requestInteraction(buttons[(rank + 1) % count]->getInteractionPointId(), actor);
				auto rejection = world.lookupInteractionRequest(denied);
				require(rejection && rejection.entity->getResult() == core::InteractionResult::Rejected
					&& rejection.entity->getOperations().empty(), "Shared approach merged authorization");
				auto request = world.requestInteraction(buttons[rank]->getInteractionPointId(), actor);
				require(bool(request), "Selected stack control refused");
				for (int tick = 0; tick < 300; ++tick)
				{
					world.advanceTicks(1);
					require(world.lookupAgent(actor).entity->getGlobalPosition().y == 0, "Agent climbed to upper Button");
					auto progress = world.lookupInteractionRequest(request);
					if (progress && progress.entity->getResult() != core::InteractionResult::Pending) break;
				}
				auto result = world.lookupInteractionRequest(request);
				require(result && result.entity->getResult() == core::InteractionResult::Succeeded
					&& result.entity->getOperations().size() == 1, "Selected stack operation failed at rank " + std::to_string(rank) + ": " + std::to_string(result ? int(result.entity->getResult()) : -1));
				auto operation = world.lookupDeviceOperation(result.entity->getOperations().front().first);
				auto door = std::dynamic_pointer_cast<const core::DoorSectorObject>(doors[rank].door.sector->getObject(doors[rank].door.index));
				require(operation && operation.entity->getCommand().traversalResource == door->getDoor()->getTraversalResourceId(), "Stack activated wrong command");
				// Route intent must select the required Door's control, even though
				// the shared vertex itself carries the bottom Button's object.
				door->getDoor()->close(); door->getDoor()->update(100);
				auto destinationSector = layers[rank] == 0 ? door->getDoor()->getFrontSector() : door->getDoor()->getBackSector();
				for (uint32_t i = 0; i < count; ++i)
				{
					auto other = std::dynamic_pointer_cast<const core::DoorSectorObject>(doors[i].door.sector->getObject(doors[i].door.index));
					other->getDoor()->close(); other->getDoor()->update(100);
				}
				auto destination = world.getGraph()->getClosestVertexInSector(destinationSector.get(), {float(xs[rank]) + 0.5f, 0});
				auto agent = world.lookupAgent(actor).entity;
				auto path = world.getGraph()->calculatePath(agent, destination);
				require(bool(path), "Stack approach lost selected Door route"); agent->setPath(path, true);
				bool selected = false;
				for (uint32_t tick = 0; tick < 3000; ++tick)
				{
					world.advanceTick();
					require(agent->getGlobalPosition().y == 0, "Routing climbed toward upper artwork");
					for (auto const& interaction : world.getSimulationSnapshot().interactionRequests)
						if (interaction.actor == actor && interaction.id != request && interaction.result != core::InteractionResult::Rejected)
						{
							require(interaction.point == buttons[rank]->getInteractionPointId(), "Route intent operated the other stacked control");
							selected = true;
						}
					if (agent->getState() == core::Agent::State::Idle) break;
				}
				require(selected && agent->getSector() == destinationSector.get(), "Agent did not activate and cross its intended stacked Door rank " + std::to_string(rank) + " selected " + std::to_string(selected) + " sector " + std::to_string(agent->getSector()->getIndex()));
				world.pauseSimulation();
				world.removeAgent(actor);
				check(world);
			}
			core::SerializationWorkData data; auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, data); writer->serialize();
			core::World loaded("Placeholder", 1, 1); auto reader = core::YamlSerializer::fromString(writer->getSerializedString()); reader->deserialize();
			require(loaded.deserialize(*reader, data), "Stack load/replay failed"); check(loaded);
			auto binary = core::BinarySerializer::toString(); world.serialize(*binary, data); binary->serialize();
			auto binaryReader = core::BinarySerializer::fromString(binary->getSerializedString()); binaryReader->deserialize();
			require(loaded.deserialize(*binaryReader, data), "Binary stack load/replay failed"); check(loaded);
			for (uint32_t i = 0; i + 1 < count; ++i) require(world.removeSectorDoor(doors[i].door.sector->getIndex(), doors[i].door.index), "Stack deletion refused");
			world.finishBuild();
			auto remaining = buttonsIn(world, middle);
			require(remaining.size() == 1 && remaining.front()->getPosition().y == CORE_BUTTON_Y_OFFSET, "Survivor did not unstack");
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

namespace
{
	void deterministicTransportControls()
	{
		using namespace core;
		// Full-width Lift doorways choose independently at every authored Stop.
		for (uint32_t width : {1u, 2u})
		{
			World world("Independent Lift landings", 16, 5);
			auto lower = world.addRoom("Lower", 0, 0, 4, width, 1);
			auto middle = world.addRoom("Middle", 0, 2, 3, width + 3, 1);
			world.addRoom("Upper neighbour", 0, 4, 4 + width, 2, 1);
			auto upper = world.addRoom("Upper", 0, 4, 3, width + 1, 1);
			World::CreateLiftOptions options; options.cellsWide = width; options.stopOffsets = {0, 2, 4};
			auto lift = world.addLift(1, 0, 4, options); world.finishBuild();
			for (auto const& [host, expected] : {std::pair{lower, 4.0f}, std::pair{middle, 4.0f + width}, std::pair{upper, 4.0f}})
			{
				auto button = buttonsIn(world, host).at(0);
				require(button->getPosition().x + button->getSize().x * 0.5f == expected, "Lift Stop did not choose independently/full-width");
				auto point = world.lookupInteractionPoint(button->getInteractionPointId());
				require(point && point.entity->getPosition().x == expected, "Lift approach drifted");
			}
			// A supported whole-unit move is rebuilt, while an impossible move is a no-op.
			world.pauseSimulation();
			auto before = world.getGraph();
			auto invalid = world.planResizeLift(lift.lift.sector->getIndex(), 12, 0, width, 5);
			require(!invalid.valid && world.getGraph() == before, "Invalid Lift move mutated graph");
		}

		// A wide Shuttle with multiple doorways retains every distinct call. A
		// narrow first landing needs simultaneous side changes, not omitted calls.
		{
			World world("Multiple Shuttle calls", 24, 2);
			auto first = world.addRoom("First", 0, 0, 0, 4, 1);
			auto second = world.addRoom("Second", 0, 0, 10, 5, 1);
			world.pauseSimulation();
			std::vector<AccessPermissionId> permissions;
			World::CreateShuttleOptions options{1, 4, {0, 10}, 0}; options.doorMask = 0b1111;
			for (uint32_t i = 0; i < 8; ++i)
			{
				permissions.push_back(world.addAccessPermission("Call " + std::to_string(i)));
				options.landingControlPermissionRequirements.push_back({permissions.back()});
			}
			auto shuttle = world.addShuttle(1, 0, 0, 20, options); world.finishBuild();
			require(shuttle.doors.size() == 8 && buttonsIn(world, first).size() == 4
				&& buttonsIn(world, second).size() == 4, "Shuttle doorway calls were merged/omitted");
			for (uint32_t i = 0; i < 8; ++i)
			{
				auto const& control = shuttle.doors[i].controls[0];
				auto button = std::dynamic_pointer_cast<const Button>(control.sector->getObject(control.index)->_getObject());
				auto expected = i < 4 ? float(i) : float(11 + i - 4);
				require(button && button->getPosition().x + button->getSize().x * 0.5f == expected
					&& button->getPosition().y == CORE_BUTTON_Y_OFFSET, "Shuttle complete side reassignment/preference failed");
				require(world.getInteractionPointPermissionRequirement(control.interactionPoint) == std::vector<AccessPermissionId>{permissions[i]}, "Doorway-specific call requirement changed");
				auto point = world.lookupInteractionPoint(control.interactionPoint);
				require(point && point.entity->getPosition() == Vector2{expected, 0}, "Shuttle call approach drifted");
			}
		}

		// The Shuttle's leftmost track geometry (not its second doorway at X=10)
		// puts its call below an incoming Door at X=9 in the canonical stack.
		for (bool reverse : {false, true})
		{
			World world("Shuttle owner geometry", 20, 2); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 20, 1);
			world.addRoom("Origin", 1, 0, 0, 3, 1);
			world.addRoom("Neighbour", 1, 0, 8, 1, 1);
			auto host = world.addRoom("Destination", 1, 0, 9, 2, 1);
			World::CreateShuttleOptions options{1, 3, {0, 9}, 0};
			World::CreateShuttleResult shuttle;
			if (reverse) shuttle = world.addShuttle(2, 0, 0, 12, options);
			world.addSectorDoor(0, 0, 9, World::RemoteControlledDoor1Options);
			if (!reverse) shuttle = world.addShuttle(2, 0, 0, 12, options);
			world.finishBuild();
			auto buttons = buttonsIn(world, host);
			std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
			require(buttons.size() == 2 && buttons[0]->getInteractionPointId() == shuttle.doors[1].controls[0].interactionPoint
				&& buttons[0]->getPosition().x == buttons[1]->getPosition().x, "Canonical Shuttle owner was its generated Door/allocation ID");
			world.pauseSimulation();
			auto before = world.getGraph();
			auto invalid = world.planResizeShuttle(shuttle.shuttle.sector->getIndex(), 4, 0, 12);
			require(!invalid.valid && world.getGraph() == before, "Invalid Shuttle movement changed controls/graph");
		}

		// Both retained shared boundaries invalidate required controls before any
		// transport, Door, interaction, IDs, or simulation pause is created.
		for (bool shuttle : {false, true})
		{
			World world("Refused transport", 12, 3);
			world.addRoom("Left", 0, 0, 0, 4, 3);
			world.addRoom("Landing", 0, 0, 4, 1, 3);
			world.addRoom("Right", 0, 0, 5, 7, 3);
			world.finishBuild();
			auto graph = world.getGraph(); auto generation = world.getTopologyGeneration(); auto sectors = world.getNumSectors();
			bool refused = false;
			try
			{
				if (shuttle) { World::CreateShuttleOptions options{1, 3, {0, 4}, 0}; options.doorMask = 1; world.addShuttle(1, 0, 4, 7, options); }
				else { World::CreateLiftOptions options; options.stopOffsets = {0, 2}; world.addLift(1, 0, 4, options); }
			}
			catch (Exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && world.getTopologyGeneration() == generation
				&& world.getNumSectors() == sectors, "Invalid transport creation was not atomic");
		}

		// Migrated transport calls join three/four-member mixed stacks without
		// acquiring an extra horizontal offset or elevated interaction approach.
		for (uint32_t count : {3u, 4u})
		{
			World world("Capacity-bounded mixed Lift stack", 10, 3); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 10, 1);
			world.addRoom("Neighbour", 1, 0, 0, 1, 1);
			auto host = world.addRoom("Landing", 1, 0, 1, 2, 1);
			world.addRoom("Upper", 1, 2, 0, 10, 1);
			world.addRoom("Back", 2, 0, 1, 1, 1);
			World::CreateLiftOptions options; options.stopOffsets = {0, 2};
			world.addLift(2, 0, 2, options);
			world.addSectorDoor(0, 0, 1, World::RemoteControlledDoor1Options);
			world.addSectorDoor(0, 0, 2, World::RemoteControlledDoor1Options);
			if (count == 4) world.addSectorDoor(1, 0, 1, World::RemoteControlledDoor1Options);
			world.finishBuild();
			auto buttons = buttonsIn(world, host);
			std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
			require(buttons.size() == count, "Mixed transport stack capacity omitted a required call");
			std::shared_ptr<const Vertex> approach;
			for (uint32_t rank = 0; rank < count; ++rank)
			{
				auto button = buttons[rank];
				require(button->getPosition().x + button->getSize().x * 0.5f == 2
					&& std::abs(button->getPosition().y - CORE_BUTTON_Y_OFFSET - rank * button->getSize().y * 1.25f) < 0.00001f,
					"Mixed transport stack spacing/centre changed");
				auto point = world.lookupInteractionPoint(button->getInteractionPointId());
				require(point && point.entity->getPosition() == Vector2{2, 0}, "Upper transport stack interaction moved upward");
			}
			for (uint32_t i = 0; i < world.getSector(host)->getNumObjects(); ++i)
			{
				auto object = world.getSector(host)->getObject(i);
				if (!object || !std::dynamic_pointer_cast<const Button>(object->_getObject())) continue;
				auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
				if (approach) require(vertex == approach, "Large mixed stack duplicated approaches");
				approach = vertex;
			}
		}

		// Removed shared walls permit an otherwise impossible landing. Restoring
		// them, including through authored loading, must be an atomic refusal.
		for (bool shuttle : {false, true})
		{
			World world("Transport structural transaction", 12, 3);
			auto neighbour = world.addRoom("Left", 0, 0, 0, 4, 3);
			auto host = world.addRoom("Landing", 0, 0, 4, 1, 3);
			world.addRoom("Right", 0, 0, 5, 7, 3);
			world.addSectorWalkway(host, 2, 0);
			world.removeLocationWall(neighbour, 0, CORE_SIDE_RIGHT);
			world.removeLocationWall(neighbour, 2, CORE_SIDE_RIGHT);
			if (shuttle) { World::CreateShuttleOptions options{1, 3, {0, 4}, 0}; options.doorMask = 1; world.addShuttle(1, 0, 4, 7, options); }
			else { World::CreateLiftOptions options; options.stopOffsets = {0, 2}; world.addLift(1, 0, 4, options); }
			world.finishBuild(); world.pauseSimulation();
			auto graph = world.getGraph(); auto buttons = buttonsIn(world, host); bool refused = false;
			try { world.addLocationWall(neighbour, 0, CORE_SIDE_RIGHT); } catch (Exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && buttonsIn(world, host) == buttons, "Wall restoration partially changed transport controls");
			SerializationWorkData data; auto writer = YamlSerializer::toString(); world.serialize(*writer, data); writer->serialize();
			auto invalid = YAML::Load(writer->getSerializedString()); YAML::Node records(YAML::NodeType::Sequence);
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() != "removeWall") records.push_back(record);
			invalid["construction"] = records;
			auto reader = YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); refused = false;
			try { refused = !world.deserialize(*reader, data); } catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && buttonsIn(world, host) == buttons, "Invalid transport load was not transactional");
		}

		// A protected incoming Door and an independently protected Lift call must
		// share a normal-height approach, not a command or permission requirement.
		for (bool reverse : {false, true})
		{
			World world("Mixed transport stack", 10, 3); world.addLayer();
			world.addRoom("Front", 0, 0, 0, 10, 1);
			auto neighbour = world.addRoom("Neighbour", 1, 0, 0, 1, 1);
			auto host = world.addRoom("Landing", 1, 0, 1, 2, 1);
			world.addRoom("Upper", 1, 2, 0, 10, 1);
			world.pauseSimulation();
			auto doorPermission = world.addAccessPermission("Door"); auto callPermission = world.addAccessPermission("Call");
			auto doorOptions = World::RemoteControlledDoor1Options;
			doorOptions.controlPermissionRequirements[1] = {doorPermission};
			World::CreateLiftOptions options; options.stopOffsets = {0, 2};
			options.landingControlPermissionRequirements = {{callPermission}, {}}; options.initialStop = 1;
			World::CreateLiftResult lift;
			if (reverse) lift = world.addLift(2, 0, 2, options);
			world.addSectorDoor(0, 0, 1, doorOptions);
			if (!reverse) lift = world.addLift(2, 0, 2, options);
			world.finishBuild();
			auto verify = [&](World const& scene)
			{
				auto buttons = buttonsIn(scene, host);
				std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
				require(buttons.size() == 2 && buttons[0]->getPosition().x == buttons[1]->getPosition().x
					&& std::abs(buttons[1]->getPosition().y - buttons[0]->getPosition().y - buttons[0]->getSize().y * 1.25f) < 0.00001f,
					"Mixed stack was not canonical");
				std::shared_ptr<const Vertex> approach;
				for (uint32_t i = 0; i < scene.getSector(host)->getNumObjects(); ++i)
				{
					auto object = scene.getSector(host)->getObject(i);
					if (!object || !std::dynamic_pointer_cast<const Button>(object->_getObject())) continue;
					auto vertex = scene.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
					require(vertex && vertex->getPosition() == Vector2{2, 0}, "Mixed stack approach elevated/moved");
					if (approach) require(approach == vertex, "Mixed stack duplicated approach");
					approach = vertex;
				}
				require(scene.getInteractionPointPermissionRequirement(buttons[0]->getInteractionPointId()) == std::vector<AccessPermissionId>{doorPermission}
					&& scene.getInteractionPointPermissionRequirement(buttons[1]->getInteractionPointId()) == std::vector<AccessPermissionId>{callPermission}, "Mixed stack merged/swapped permissions");
				for (auto button : buttons)
				{
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(scene.getObjectAtPosition(1, centre.x, centre.y) == button, "Runtime targeting chose other mixed member");
				}
				return buttons;
			};
			auto buttons = verify(world);
			world.pauseSimulation();
			world.removeLocationWall(neighbour, 0, CORE_SIDE_RIGHT);
			world.finishBuild();
			auto separated = buttonsIn(world, host);
			require(separated.size() == 2 && separated[0]->getPosition().x != separated[1]->getPosition().x
				&& separated[0]->getPosition().y == CORE_BUTTON_Y_OFFSET && separated[1]->getPosition().y == CORE_BUTTON_Y_OFFSET,
				"Wall removal did not separate mixed transport controls");
			world.pauseSimulation(); world.addLocationWall(neighbour, 0, CORE_SIDE_RIGHT); world.finishBuild();
			require(verify(world) == buttons, "Wall restoration replaced protected interaction identities");
			SerializationWorkData data;
			auto replay = [&]<typename SerializerType>()
			{
				auto writer = SerializerType::toString(); world.serialize(*writer, data); writer->serialize();
				auto reader = SerializerType::fromString(writer->getSerializedString());
				reader->deserialize(); World loaded("Placeholder", 1, 1);
				require(loaded.deserialize(*reader, data), "Transport stack replay failed"); verify(loaded);
			};
			replay.template operator()<YamlSerializer>(); replay.template operator()<BinarySerializer>();
			world.pauseSimulation();
			auto actor = world.createAgent("Caller", host, 0, 2.0f);
			require(world.grantAgentAccessPermission(actor, callPermission), "Call authorization failed");
			require(world.resumeSimulation(), "Mixed stack topology invalid");
			auto denied = world.requestInteraction(buttons[0]->getInteractionPointId(), actor);
			require(world.lookupInteractionRequest(denied).entity->getResult() == InteractionResult::Rejected, "Mixed stack bypassed independent authorization");
			auto call = world.requestInteraction(buttons[1]->getInteractionPointId(), actor);
			for (uint32_t tick = 0; tick < 100; ++tick) world.advanceTick();
			require(world.lookupInteractionRequest(call).entity->getOperations().size() == 1, "Landing call merged operations");
			verify(world); // Runtime motion/opening does not move stationary controls.
			world.resetSimulation();
			auto upper = world.getSectorAtPosition(1, 2.0f, 2.0f);
			auto target = world.getGraph()->getClosestVertexInSector(upper.get(), {2.5f, 2});
			auto agent = world.lookupAgent(actor).entity;
			auto path = world.getGraph()->calculatePath(agent, target);
			require(bool(path), "Mixed stack lost transport route"); agent->setPath(path, true);
			bool selectedCall = false, sawOccupiedMotion = false;
			for (uint32_t tick = 0; tick < 8000; ++tick)
			{
				world.advanceTick();
				if (agent->getSector() == world.getSector(host).get())
					require(agent->getGlobalPosition().y == 0, "Agent climbed to stacked Lift artwork");
				auto snapshot = world.getSimulationSnapshot();
				for (auto const& interaction : snapshot.interactionRequests)
					if (interaction.actor == actor && interaction.result != InteractionResult::Rejected
						&& agent->getSector() == world.getSector(host).get())
					{
						require(interaction.point == buttons[1]->getInteractionPointId(), "Transport intent activated incoming protected Door");
						selectedCall = true;
					}
				for (auto const& resource : snapshot.traversalResources)
					if (resource.id == lift.traversalResource && resource.occupantCount && resource.liftMoving) sawOccupiedMotion = true;
				verify(world);
				if (agent->getState() == Agent::State::Idle) break;
			}
			require(selectedCall && sawOccupiedMotion && agent->getSector() == upper.get(), "Selected mixed-stack Lift journey did not complete");
			world.pauseSimulation(); auto plan = world.planRemoveLift(lift.lift.sector->getIndex());
			require(plan.valid, "Lift deletion refused"); world.applyLiftEdit(plan);
			auto remaining = buttonsIn(world, host);
			require(remaining.size() == 1 && remaining[0]->getPosition().y == CORE_BUTTON_Y_OFFSET, "Transport deletion did not unstack Door");
		}
	}
}

namespace
{
	void independentEndpointControls()
	{
		using namespace core;
		auto centreX = [](World::CreateObjectResult const& control)
		{
			auto b = control.sector->getObject(control.index)->_getObject();
			return b->getPosition().x + b->getSize().x * 0.5f;
		};
		{
			World world("Independent Transit endpoints", 6, 4);
			world.addRoom("Low", 0, 0, 0, 4, 1);
			world.addRoom("High", 0, 3, 2, 1, 1);
			auto ladder = world.addLadder(1, 0, 2, {4, true, false}); world.finishBuild();
			require(centreX(ladder.controls[0]) == 3 && centreX(ladder.controls[1]) == 2, "Transit endpoints inherited one side");
		}
		{
			World world("Transit refusal", 6, 4);
			world.addRoom("Low", 0, 0, 0, 4, 1);
			world.addRoom("High left", 0, 3, 0, 2, 1);
			world.addRoom("High", 0, 3, 2, 1, 1);
			world.addRoom("High right", 0, 3, 3, 1, 1);
			world.finishBuild(); auto graph = world.getGraph(); auto count = world.getNumSectors(); bool refused = false;
			try { world.addLadder(1, 0, 2, {4, true, false}); } catch (std::exception const&) { refused = true; }
			require(refused && world.getNumSectors() == count && world.getGraph() == graph, "Invalid Ladder creation partially mutated World");
		}
		{
			World world("Endpoint wall transaction", 8, 3);
			auto neighbour = world.addRoom("Left", 0, 0, 0, 4, 3);
			auto host = world.addRoom("Host", 0, 0, 4, 1, 3);
			world.addRoom("Right", 0, 0, 5, 3, 3); world.addSectorWalkway(host, 2, 0);
			world.removeLocationWall(neighbour, 0, CORE_SIDE_RIGHT); world.removeLocationWall(neighbour, 2, CORE_SIDE_RIGHT);
			world.addRoomLadder(host, 0, 0, {0, true, false}); world.finishBuild(); world.pauseSimulation();
			auto graph = world.getGraph(); auto buttons = buttonsIn(world, host); bool refused = false;
			try { world.addLocationWall(neighbour, 2, CORE_SIDE_RIGHT); } catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && buttonsIn(world, host) == buttons, "Endpoint wall refusal changed controls/graph");
			SerializationWorkData data; auto writer = YamlSerializer::toString(); world.serialize(*writer, data); writer->serialize();
			auto invalid = YAML::Load(writer->getSerializedString()); YAML::Node records(YAML::NodeType::Sequence);
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() != "removeWall") records.push_back(record);
			invalid["construction"] = records;
			auto reader = YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); refused = false;
			try { refused = !world.deserialize(*reader, data); } catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && buttonsIn(world, host) == buttons, "Invalid endpoint load was not transactional");
		}
		{
			World world("Independent Room endpoints", 6, 4);
			auto room = world.addRoom("Room", 0, 0, 0, 5, 4);
			world.addSectorWalkway(room, 3, 2);
			auto fixed = world.addRoomLadder(room, 0, 2);
			require(!fixed.controls[0].interactionPoint && !fixed.controls[1].interactionPoint
				&& buttonsIn(world, room).empty(), "Non-extensible Ladder gained controls");
			world.finishBuild(); world.pauseSimulation(); world.removeRoomLadder(room, fixed.ladder.index);
			auto ladder = world.addRoomLadder(room, 0, 2, {0, true, false}); world.finishBuild();
			require(centreX(ladder.controls[0]) == 3 && centreX(ladder.controls[1]) == 2, "Room endpoints inherited one side");
			world.pauseSimulation(); world.addSectorWalkway(room, 3, 3); world.finishBuild();
			auto controls = buttonsIn(world, room);
			require(controls.size() == 2 && controls[0]->getPosition().x == controls[1]->getPosition().x,
				"New endpoint support did not restore preferred candidate");
		}
		{
			World world("Stacked Ladder endpoints", 6, 5);
			auto room = world.addRoom("Room", 0, 0, 0, 1, 5);
			world.addSectorWalkway(room, 2, 0); world.addSectorWalkway(room, 4, 0);
			auto lower = world.addRoomLadder(room, 0, 0, {0, true, false});
			auto upper = world.addRoomLadder(room, 2, 0, {0, true, false}); world.finishBuild();
			auto a = lower.controls[1].sector->getObject(lower.controls[1].index);
			auto b = upper.controls[0].sector->getObject(upper.controls[0].index);
			auto ba = a->_getObject(); auto bb = b->_getObject();
			require(centreX(lower.controls[1]) == 0 && centreX(upper.controls[0]) == 0
				&& std::abs(bb->getPosition().y - ba->getPosition().y - ba->getSize().y * 1.25f) < 0.00001f, "Meeting Ladder endpoints did not stack canonically");
			auto va = world.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(a));
			auto vb = world.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(b));
			require(va && va == vb && va->getPosition() == Vector2{0, 2}, "Ladder endpoints lost shared approach");
			world.pauseSimulation(); world.removeRoomLadder(room, upper.ladder.index);
			require(buttonsIn(world, room).size() == 2, "Ladder deletion retained endpoint control");
		}
		{
			World world("Opposite Platform Stops", 8, 5);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 5);
			for (auto x : {3u, 4u}) world.addSectorWalkway(room, 2, x);
			for (auto x : {2u, 3u}) world.addSectorWalkway(room, 4, x);
			World::CreateLiftOptions options; options.stopOffsets = {0, 2, 4};
			require(world.canAddPlatformLift(room, 3, options), "Opposite-side Stops refused");
			auto lift = world.addSectorPlatformLift(room, 0, 3, options); world.finishBuild();
			require(centreX(lift.buttons[0]) == 4 && centreX(lift.buttons[1]) == 4 && centreX(lift.buttons[2]) == 3, "Platform inherited a common Stop side");
			world.pauseSimulation(); auto edit = world.planPlatformLiftEdit(room, lift.lift.index, options);
			require(edit.valid, "Independent Stop configuration refused: " + edit.diagnostic); world.applyPlatformLiftEdit(edit);
		}
		{
			World world("Permanent endpoint support", 8, 3);
			auto room = world.addRoom("Room", 0, 0, 0, 8, 3);
			world.addSectorWalkway(room, 2, 3); world.addSectorWalkway(room, 2, 5);
			World::CreateForceBridgeOptions bridge; bridge.extensible = false; bridge.controlCount = 0;
			world.addSectorForceBridge(room, 2, 4, bridge);
			World::CreateLiftOptions options; options.stopOffsets = {0, 2};
			require(!world.canAddPlatformLift(room, 3, options), "Platform accepted retractable adjoining support");
			auto before = world.getSector(room)->getNumObjects(); bool refused = false;
			try { world.addSectorPlatformLift(room, 0, 3, options); } catch (std::exception const&) { refused = true; }
			require(refused && world.getSector(room)->getNumObjects() == before, "Invalid Platform creation left partial controls");
			auto ladder = world.addRoomLadder(room, 0, 3, {0, true, false}); world.finishBuild();
			require(centreX(ladder.controls[0]) == 4 && centreX(ladder.controls[1]) == 3, "Ladder used temporary endpoint host support");
		}
		for (bool reverse : {false, true})
		{
			World world("Mixed endpoint stack", 8, 3);
			world.addRoom("Neighbour", 1, 0, 0, 1, 3);
			auto room = world.addRoom("Room", 1, 0, 1, 2, 3);
			world.addRoom("Front", 0, 0, 0, 8, 3);
			world.addSectorWalkway(room, 2, 0); world.addSectorWalkway(room, 2, 1);
			world.pauseSimulation();
			auto ladderPermission = world.addAccessPermission("Ladder"); auto callPermission = world.addAccessPermission("Call");
			World::CreateLadderOptions ladderOptions{0, true, false}; ladderOptions.controlPermissionRequirements[0] = {ladderPermission};
			World::CreateLiftOptions liftOptions; liftOptions.stopOffsets = {0, 2}; liftOptions.initialStop = 1;
			liftOptions.landingControlPermissionRequirements = {{callPermission}, {}};
			if (reverse) world.addRoomLadder(room, 0, 1, ladderOptions);
			world.addSectorPlatformLift(room, 0, 0, liftOptions);
			if (!reverse) world.addRoomLadder(room, 0, 1, ladderOptions);
			world.addSectorDoor(0, 0, 1, World::RemoteControlledDoor1Options); world.finishBuild();
			auto verify = [&](World const& scene)
			{
				auto buttons = buttonsIn(scene, room); std::erase_if(buttons, [](auto b) { return b->getPosition().y >= 2; });
				std::sort(buttons.begin(), buttons.end(), [](auto a, auto b) { return a->getPosition().y < b->getPosition().y; });
				require(buttons.size() == 3, "Mixed endpoint stack omitted controls"); std::shared_ptr<const Vertex> approach;
				for (uint32_t i = 0; i < scene.getSector(room)->getNumObjects(); ++i)
				{
					auto object = scene.getSector(room)->getObject(i);
					if (!object || object->getCellY() != 0 || object->getObjectType() != SectorObjectType::InteractionPoint) continue;
					auto vertex = scene.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
					require(vertex && vertex->getPosition() == Vector2{2, 0}, "Mixed endpoint approach moved upward");
					if (approach) require(approach == vertex, "Mixed endpoints duplicated approach");
					approach = vertex;
				}
				for (uint32_t rank = 0; rank < buttons.size(); ++rank)
				{
					auto button = buttons[rank]; auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(centre.x == 2 && std::abs(button->getPosition().y - CORE_BUTTON_Y_OFFSET - rank * button->getSize().y * 1.25f) < 0.00001f, "Mixed endpoint order/spacing changed");
					require(scene.getObjectAtPosition(1, centre.x, centre.y) == button, "Mixed endpoint targeting selected wrong member");
					require(scene.lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition() == Vector2{2, 0}, "Mixed interaction moved upward");
				}
				require(scene.getInteractionPointPermissionRequirement(buttons[1]->getInteractionPointId()) == std::vector<AccessPermissionId>{callPermission}
					&& scene.getInteractionPointPermissionRequirement(buttons[2]->getInteractionPointId()) == std::vector<AccessPermissionId>{ladderPermission}, "Endpoint permissions merged/swapped");
				return buttons;
			};
			auto buttons = verify(world); SerializationWorkData data;
			auto replay = [&]<typename SerializerType>()
			{
				auto writer = SerializerType::toString(); world.serialize(*writer, data); writer->serialize();
				auto reader = SerializerType::fromString(writer->getSerializedString()); reader->deserialize(); World loaded("Placeholder", 1, 1);
				require(loaded.deserialize(*reader, data), "Endpoint replay failed"); verify(loaded);
			};
			replay.template operator()<YamlSerializer>(); replay.template operator()<BinarySerializer>();
			world.pauseSimulation(); auto actor = world.createAgent("Caller", room, 0, 2.0f);
			world.grantAgentAccessPermission(actor, callPermission); require(world.resumeSimulation(), "Endpoint topology invalid");
			auto denied = world.requestInteraction(buttons[2]->getInteractionPointId(), actor);
			require(world.lookupInteractionRequest(denied).entity->getResult() == InteractionResult::Rejected, "Protected Ladder operation bypassed");
			auto request = world.requestInteraction(buttons[1]->getInteractionPointId(), actor);
			for (uint32_t tick = 0; tick < 100; ++tick)
			{
				world.advanceTick(); verify(world);
				require(world.lookupAgent(actor).entity->getGlobalPosition().y == 0, "Agent climbed Button stack");
			}
			auto result = world.lookupInteractionRequest(request);
			require(result && result.entity->getOperations().size() == 1, "Platform call merged endpoint operations: "
				+ (result ? std::to_string(static_cast<int>(result.entity->getResult())) : "missing"));
			world.pauseSimulation(); auto climber = world.createAgent("Extender", room, 0, 2.0f);
			world.grantAgentAccessPermission(climber, ladderPermission); world.resumeSimulation();
			auto deniedCall = world.requestInteraction(buttons[1]->getInteractionPointId(), climber);
			require(world.lookupInteractionRequest(deniedCall).entity->getResult() == InteractionResult::Rejected, "Protected Platform call bypassed");
			auto extension = world.requestInteraction(buttons[2]->getInteractionPointId(), climber);
			for (uint32_t tick = 0; tick < 10; ++tick) { world.advanceTick(); verify(world); }
			auto extended = world.lookupInteractionRequest(extension);
			require(extended && extended.entity->getOperations().size() == 1, "Selected Ladder control did not retain extension command: "
				+ (extended ? std::to_string(static_cast<int>(extended.entity->getResult())) : "missing"));
			world.resetSimulation();
			auto agent = world.lookupAgent(climber).entity;
			auto target = world.getGraph()->getClosestVertexInSector(world.getSector(room).get(), {2.5f, 2});
			auto path = world.getGraph()->calculatePath(agent, target);
			require(bool(path), "Stacked Ladder lost authorized Agent route"); agent->setPath(path, true);
			bool selectedEndpoint = false;
			for (uint32_t tick = 0; tick < 4000; ++tick)
			{
				world.advanceTick(); verify(world);
				for (auto const& interaction : world.getSimulationSnapshot().interactionRequests)
					if (interaction.actor == climber && interaction.result != InteractionResult::Rejected)
					{
						require(interaction.point == buttons[2]->getInteractionPointId(), "Ladder intent operated another stacked control");
						selectedEndpoint = true;
					}
				if (agent->getState() == Agent::State::Idle) break;
			}
			require(selectedEndpoint && agent->getGlobalPosition().y == 2, "Selected stacked Ladder traversal did not complete");
		}
	}
}

namespace
{
	// #433: fixed candidates use the same authoring/replay boundary as movable
	// Door-rule candidates, but may never transfer approach or endpoint.
	void fixedInsetControls()
	{
		using namespace core;
		auto centre = [](std::shared_ptr<const Button> button)
		{ return button->getPosition().x + button->getSize().x * 0.5f; };
		auto verify = [&](World const& world, uint32_t host, std::vector<float> expected)
		{
			auto buttons = buttonsIn(world, host);
			require(buttons.size() == expected.size(), "Inset owner changed control demand");
			std::vector<float> actual;
			for (auto button : buttons)
			{
				actual.push_back(centre(button));
				auto position = button->getPosition() + button->getSize() * 0.5f;
				require(world.getObjectAtPosition(world.getSector(host)->getLayerIndex(), position.x, position.y) == button,
					"Inset control targeting disagrees with artwork");
				auto point = world.lookupInteractionPoint(button->getInteractionPointId());
				require(point && point.entity->getPosition() == Vector2{centre(button),
					std::floor(button->getPosition().y)},
					"Inset interaction approach differs from its walkable position");
			}
			for (uint32_t i = 0; i < world.getSector(host)->getNumObjects(); ++i)
			{
				auto object = world.getSector(host)->getObject(i);
				auto button = object ? std::dynamic_pointer_cast<const Button>(object->_getObject()) : nullptr;
				if (!button) continue;
				auto vertex = world.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
				require(vertex && vertex->getPosition() == world.lookupInteractionPoint(button->getInteractionPointId()).entity->getPosition(),
					"Inset control omitted its production graph approach");
			}
			std::sort(actual.begin(), actual.end()); std::sort(expected.begin(), expected.end());
			require(actual == expected, "Inset owner used an unapproved candidate");
			return buttons;
		};
		auto replay = [&]<typename SerializerType>(World& world, auto check)
		{
			SerializationWorkData data;
			auto writer = SerializerType::toString(); world.serialize(*writer, data); writer->serialize();
			auto reader = SerializerType::fromString(writer->getSerializedString()); reader->deserialize();
			World loaded("Placeholder", 1, 1);
			require(loaded.deserialize(*reader, data), "Fixed inset reconstruction failed"); check(loaded);
		};
		// Quarter-cell centres are not cell-side registration slots. Three
		// independent controls can occupy one cell without merging or stacking.
		for (bool reverse : {false, true})
		{
			World world("Three distinct inset approaches", 6, 2);
			world.addRoom("Left", 0, 0, 0, 2, 1);
			auto middle = world.addRoom("Middle", 0, 0, 2, 1, 1);
			world.addRoom("Right", 0, 0, 3, 2, 1);
			if (!reverse) world.addSectorLightSwitch(middle, 0);
			world.addSectorBulkheadDoor(0, 0, reverse ? 3 : 2, CORE_SIDE_LEFT);
			world.addSectorBulkheadDoor(0, 0, reverse ? 2 : 3, CORE_SIDE_LEFT);
			if (reverse) world.addSectorLightSwitch(middle, 0);
			world.finishBuild();
			auto check = [&](World const& scene) { verify(scene, middle, {2.25f, 2.5f, 2.75f}); };
			check(world); replay.template operator()<YamlSerializer>(world, check); replay.template operator()<BinarySerializer>(world, check);
		}
		// One-cell hosts, retained walls at both ends, asymmetric authored demand
		// and independent authorization. Opening never frees a placement side.
		for (auto demand : {std::array<bool, 2>{true, false}, {false, true}, {true, true}})
		{
			World world("Walled Bulkhead insets", 8, 2);
			world.addRoom("Beyond left", 0, 0, 0, 2, 1);
			auto left = world.addRoom("Left", 0, 0, 2, 1, 1);
			auto right = world.addRoom("Right", 0, 0, 3, 1, 1);
			world.addRoom("Beyond right", 0, 0, 4, 2, 1);
			world.pauseSimulation(); auto lp = world.addAccessPermission("Left operator"); auto rp = world.addAccessPermission("Right operator");
			World::CreateBulkheadDoorOptions options;
			options.controls[0] = demand[0]; options.controls[1] = demand[1];
			if (demand[0]) options.controlPermissionRequirements[0] = {lp};
			if (demand[1]) options.controlPermissionRequirements[1] = {rp};
			auto created = world.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT, options); world.finishBuild();
			auto check = [&](World const& scene)
			{
				verify(scene, left, demand[0] ? std::vector<float>{2.75f} : std::vector<float>{});
				verify(scene, right, demand[1] ? std::vector<float>{3.25f} : std::vector<float>{});
				for (int side = 0; side < 2; ++side)
					if (demand[side])
					{
						auto button = buttonsIn(scene, side == 0 ? left : right).front();
						require(scene.getInteractionPointPermissionRequirement(button->getInteractionPointId())
							== std::vector<AccessPermissionId>{side == 0 ? lp : rp}, "Bulkhead transferred approach authorization");
					}
			};
			check(world); replay.template operator()<YamlSerializer>(world, check); replay.template operator()<BinarySerializer>(world, check);
			world.pauseSimulation(); world.removeLocationWall(left, 0, CORE_SIDE_LEFT); world.finishBuild(); check(world);
			world.pauseSimulation(); world.addLocationWall(left, 0, CORE_SIDE_LEFT); world.finishBuild(); check(world);
			auto door = std::dynamic_pointer_cast<const BulkheadDoorSectorObject>(world.getSector(created.door.sector->getIndex())->getObject(created.door.index))->getDoor();
			door->open(); door->update(100); require(door->isOpen(), "Bulkhead did not open"); check(world);
			door->close(); door->update(100); check(world);
			world.pauseSimulation();
			int side = demand[0] ? 0 : 1; auto button = buttonsIn(world, side == 0 ? left : right).front();
			auto actor = world.createAgent("Operator", side == 0 ? left : right, 0, side == 0 ? 0.75f : 0.25f);
			world.resumeSimulation();
			auto denied = world.requestInteraction(button->getInteractionPointId(), actor);
			require(world.lookupInteractionRequest(denied).entity->getResult() == InteractionResult::Rejected, "Inset bypassed Bulkhead authorization");
			require(world.setAgentRuntimeAccessPermissionGrant(actor, side == 0 ? lp : rp, true), "Bulkhead grant failed");
			auto request = world.requestInteraction(button->getInteractionPointId(), actor);
			auto initialRequest = world.lookupInteractionRequest(request);
			require(initialRequest && initialRequest.entity->getResult() != InteractionResult::Rejected,
				"Authorized Bulkhead interaction was rejected");
			bool operated = initialRequest.entity->getOperations().size() == 1, opened = false;
			for (uint32_t tick = 0; tick < 600; ++tick)
			{
				world.advanceTick(); check(world); opened |= door->isOpen();
				if (auto result = world.lookupInteractionRequest(request))
					operated |= result.entity->getOperations().size() == 1;
			}
			require(operated && opened, "Inset lost its OpenDoor command");
		}
		{
			World world("Walled Airlock insets", 8, 2);
			world.addRoom("Beyond left", 0, 0, 0, 1, 1);
			auto left = world.addRoom("Left", 0, 0, 1, 1, 1);
			auto right = world.addRoom("Right", 0, 0, 4, 1, 1);
			world.addRoom("Beyond right", 0, 0, 5, 1, 1);
			auto index = world.addAirlock(0, 0, 2, 2); world.finishBuild();
			auto check = [&](World const& scene)
			{
				verify(scene, left, {1.75f}); verify(scene, right, {4.25f});
				require(buttonsIn(scene, index).empty() && scene.getSimulationSnapshot().interactionPoints.size() == 2,
					"Airlock generated inside/owned-Bulkhead controls");
			};
			check(world); replay.template operator()<YamlSerializer>(world, check); replay.template operator()<BinarySerializer>(world, check);
			world.pauseSimulation(); world.removeLocationWall(right, 0, CORE_SIDE_RIGHT); world.finishBuild(); check(world);
			world.pauseSimulation(); world.addLocationWall(right, 0, CORE_SIDE_RIGHT); world.finishBuild(); check(world);
		}
		{
			World world("Bridge walls beyond supports", 8, 3);
			world.addRoom("Left neighbour", 0, 0, 0, 2, 3);
			auto room = world.addRoom("Room", 0, 0, 2, 4, 3);
			world.addRoom("Right neighbour", 0, 0, 6, 2, 3);
			world.addSectorWalkway(room, 1, 0); world.addSectorWalkway(room, 1, 3);
			world.addSectorForceBridge(room, 1, 1, {2, CORE_SIDE_LEFT, true, false, 2}); world.finishBuild();
			verify(world, room, {2.75f, 5.25f});
			SerializationWorkData data; auto writer = YamlSerializer::toString(); world.serialize(*writer, data); writer->serialize();
			auto invalid = YAML::Load(writer->getSerializedString()); YAML::Node records(YAML::NodeType::Sequence);
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() != "walkway" || record["xOffset"].as<uint32_t>() != 0) records.push_back(record);
			invalid["construction"] = records;
			auto reader = YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize();
			auto graph = world.getGraph(); auto buttons = buttonsIn(world, room); bool refused = false;
			try { refused = !world.deserialize(*reader, data); } catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && buttonsIn(world, room) == buttons,
				"Invalid bridge-support load was not transactional");
		}
		for (int side : {CORE_SIDE_LEFT, CORE_SIDE_RIGHT})
			for (uint32_t count : {1u, 2u})
			{
				World world("Permanent bridge insets", 10, 3);
				auto room = world.addRoom("Room", 0, 0, 0, 8, 3);
				for (auto x : {0u, 1u, 4u, 5u, 6u, 7u}) world.addSectorWalkway(room, 1, x);
				world.pauseSimulation();
				auto lp = world.addAccessPermission("Left endpoint"); auto rp = world.addAccessPermission("Right endpoint");
				World::CreateForceBridgeOptions options{2, side, true, false, count};
				options.controlPermissionRequirements[side] = {side == 0 ? lp : rp};
				if (count == 2) options.controlPermissionRequirements[1 - side] = {side == 0 ? rp : lp};
				auto made = world.addSectorForceBridge(room, 1, 2, options); world.finishBuild();
				auto expected = count == 2 ? std::vector<float>{1.75f, 4.25f} : std::vector<float>{side == 0 ? 1.75f : 4.25f};
				auto check = [&](World const& scene)
				{
					for (auto button : verify(scene, room, expected))
						require(scene.getInteractionPointPermissionRequirement(button->getInteractionPointId())
							== std::vector<AccessPermissionId>{centre(button) < 2 ? lp : rp}, "Bridge endpoint authorization transferred/merged");
				};
				check(world); replay.template operator()<YamlSerializer>(world, check); replay.template operator()<BinarySerializer>(world, check);
				auto device = std::dynamic_pointer_cast<const ForceBridgeSectorObject>(made.forceBridge.sector->getObject(made.forceBridge.index))->getForceBridge();
				auto buttons = buttonsIn(world, room);
				world.pauseSimulation(); auto graph = world.getGraph(); bool refused = false;
				auto support = std::as_const(world).getLayer(0)->getCellDefinition(side == 0 ? 1 : 4, 1).floorIndex;
				try { refused = !world.removeSectorWalkway(room, support); } catch (std::exception const&) { refused = true; }
				require(refused && world.getGraph() == graph && buttonsIn(world, room) == buttons, "Bridge support removal was not atomic");
				check(world);
				// Only the authored endpoint can prepare a retracted one-control bridge.
				auto actor = world.createAgent("Crossing", room, 1, side == 0 ? 0.5f : 6.5f);
				require(world.grantAgentAccessPermission(actor, side == 0 ? lp : rp), "Bridge endpoint grant failed");
				if (count == 1)
				{
					auto opposite = world.createAgent("Opposite", room, 1, side == 0 ? 6.5f : 0.5f);
					world.grantAgentAccessPermission(opposite, side == 0 ? lp : rp);
					auto destination = world.getGraph()->getClosestVertexInSector(world.getSector(room).get(), {side == 0 ? 0.5f : 6.5f, 1});
					require(!world.getGraph()->calculatePath(world.lookupAgent(opposite).entity, destination), "One-control bridge gained opposite-end preparation");
				}
				auto agent = world.lookupAgent(actor).entity;
				auto target = world.getGraph()->getClosestVertexInSector(world.getSector(room).get(), {side == 0 ? 6.5f : 0.5f, 1});
				auto path = world.getGraph()->calculatePath(agent, target); require(bool(path), "Inset bridge lost preparation route");
				agent->setPath(path, true); world.resumeSimulation(); bool operated = false;
				for (uint32_t tick = 0; tick < 4000; ++tick)
				{
					world.advanceTick(); check(world);
					for (auto const& request : world.getSimulationSnapshot().interactionRequests)
						if (request.actor == actor && request.result != InteractionResult::Rejected)
						{
							require(request.point == made.controls[0].interactionPoint, "Bridge used opposite endpoint control"); operated = true;
						}
					if (agent->getState() == Agent::State::Idle) break;
				}
				require(operated && device->isExtended() && agent->getGlobalPosition().distanceTo(target->getPosition()) < 0.001f,
					"Bridge inset preparation/crossing failed");
			}
	}
}

namespace
{
	// #435: four mixed controls must reassign together; the fifth demand is
	// feasible only while the shared wall is removed. No derived positions are
	// copied or retained during structural reconstruction.
	void allOwnerReconstruction()
	{
		using namespace core;
		World world("All stationary control families", 40, 5);
		auto room = world.addRoom("Devices", 0, 0, 0, 8, 5);
		world.addRoom("Door back", 1, 0, 0, 8, 5);
		for (auto x : {1u, 4u}) world.addSectorWalkway(room, 1, x);
		for (auto x : {5u, 6u, 7u}) world.addSectorWalkway(room, 2, x);
		world.addSectorForceBridge(room, 1, 2, {2, CORE_SIDE_LEFT, true, false, 2});
		world.addRoomLadder(room, 0, 5, {0, true, false});
		World::CreateLiftOptions platform; platform.stopOffsets = {0, 2};
		world.addSectorPlatformLift(room, 0, 6, platform);
		world.addSectorDoor(0, 0, 0, World::RemoteControlledDoor1Options);
		world.addSectorLightSwitch(room, 0);
		auto dumb = world.addRoom("Dumbwaiter hall", 0, 0, 8, 2, 2);
		world.addSectorWalkway(dumb, 1, 1);
		world.addDumbwaiter(1, 0, 9);
		auto hall = world.addRoom("Lift hall", 0, 0, 10, 2, 3);
		world.addSectorWalkway(hall, 2, 1);
		World::CreateLiftOptions lift; lift.stopOffsets = {0, 2}; world.addLift(1, 0, 11, lift);
		world.addRoom("Shuttle hall", 0, 0, 12, 14, 1);
		World::CreateShuttleOptions shuttle{1, 3, {0, 4}, 0}; world.addShuttle(1, 0, 13, 7, shuttle);
		world.addRoom("Ladder lower", 0, 0, 26, 2, 1);
		world.addRoom("Ladder upper", 0, 2, 26, 2, 1);
		world.addLadder(1, 0, 26, {3, true, false});
		world.addRoom("Airlock left", 0, 0, 28, 2, 1);
		world.addRoom("Airlock right", 0, 0, 32, 2, 1);
		world.addAirlock(0, 0, 30, 2);
		world.addSectorBulkheadDoor(0, 0, 8, CORE_SIDE_LEFT);
		world.finishBuild(); world.pauseSimulation();
		for (uint32_t sector = 0; sector < world.getNumSectors(); ++sector)
			for (auto button : buttonsIn(world, sector))
			{
				auto id = button->getInteractionPointId();
				auto permission = world.addAccessPermission("Control " + std::to_string(id.value));
				std::string diagnostic;
				require(world.setInteractionPointPermissionRequirement(id, {permission}, &diagnostic), diagnostic);
			}
		auto layout = [](World const& scene)
		{
			std::vector<std::tuple<uint32_t, float, float, std::vector<AccessPermissionId>>> result;
			std::set<uint64_t> identities;
			for (uint32_t sector = 0; sector < scene.getNumSectors(); ++sector)
				for (auto button : buttonsIn(scene, sector))
				{
					auto point = scene.lookupInteractionPoint(button->getInteractionPointId());
					require(bool(point) && identities.insert(button->getInteractionPointId().value).second,
						"All-owner reconstruction omitted/merged interaction identity");
					auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(scene.getObjectAtPosition(scene.getSector(sector)->getLayerIndex(), centre.x, centre.y) == button,
						"All-owner reflow lost independently targetable geometry");
					result.emplace_back(scene.getSector(sector)->getLayerIndex(), centre.x, button->getPosition().y,
						scene.getInteractionPointPermissionRequirement(button->getInteractionPointId()));
				}
			std::sort(result.begin(), result.end()); return result;
		};
		auto prior = layout(world); require(prior.size() == 21, "All-owner fixture omitted a control family");
		// Removed walls do not let either landing move its Button to the other
		// Location. Bulkhead insets remain blockers elsewhere in the same row.
		world.removeLocationWall(dumb, 0, CORE_SIDE_RIGHT); world.finishBuild();
		require(layout(world) == prior, "Mixed row crossed approach Locations after wall removal");
		SerializationWorkData data;
		auto replay = [&]<typename Serializer>()
		{
			auto writer = Serializer::toString(); world.serialize(*writer, data); writer->serialize();
			auto reader = Serializer::fromString(writer->getSerializedString()); reader->deserialize(); World loaded("Placeholder", 1, 1);
			try { require(loaded.deserialize(*reader, data) && layout(loaded) == prior, "All-owner mixed reconstruction changed placement"); }
			catch (std::exception const& error) { throw std::runtime_error(std::string("All-owner current replay: ") + error.what()); }
		};
		replay.template operator()<YamlSerializer>(); replay.template operator()<BinarySerializer>();
		for (int iteration = 0; iteration < 2; ++iteration)
		{
			world.resetSimulation();
			require(layout(world) == prior, "All-owner construction replay changed geometry/independent permissions");
			if (iteration == 0)
			{
				world.pauseSimulation(); auto extra = world.addAccessPermission("Edited after reconstruction");
				for (uint32_t sector = 0; sector < world.getNumSectors(); ++sector)
					for (auto button : buttonsIn(world, sector))
					{
						auto id = button->getInteractionPointId(); auto permissions = world.getInteractionPointPermissionRequirement(id);
						permissions.push_back(extra);
						require(world.setInteractionPointPermissionRequirement(id, permissions), "Reconstructed control edit refused");
					}
				prior = layout(world);
			}
		}
		world.pauseSimulation(); world.addLocationWall(dumb, 0, CORE_SIDE_RIGHT); world.finishBuild();
		require(layout(world) == prior, "All-owner wall restoration retained stale placement");

		// Patch production-written typed fields, not private construction state.
		// The Binary serializer is self-describing; derive each field fragment
		// with its writer so malformed documents still have valid wire structure.
		auto binaryField = [](std::string const& name, auto value)
		{
			auto writer = BinarySerializer::toString(); writer->beginMap("");
			if constexpr (std::is_same_v<decltype(value), uint32_t>) writer->writeUint32(name, value);
			else writer->writeString(name, value);
			writer->endMap(); writer->serialize();
			auto bytes = writer->getSerializedString(); auto at = bytes.find(name);
			require(at != std::string::npos, "Binary field fragment missing");
			return bytes.substr(at);
		};
		auto yamlWriter = YamlSerializer::toString(); world.serialize(*yamlWriter, data); yamlWriter->serialize();
		auto binaryWriter = BinarySerializer::toString(); world.serialize(*binaryWriter, data); binaryWriter->serialize();
		auto yaml = yamlWriter->getSerializedString(); auto binary = binaryWriter->getSerializedString();
		auto graph = world.getGraph(); auto generation = world.getTopologyGeneration(); world.markSaved();
		for (std::string type : {"door", "lift", "shuttle", "dumbwaiter", "sectorLadder", "ladder",
			"platformLift", "forceBridge", "bulkheadDoor", "airlock", "lightSwitch"})
		{
			auto invalid = YAML::Load(yaml); YAML::Node selected;
			for (YAML::Node record : invalid["construction"])
				if (record["type"].as<std::string>() == type) { selected.reset(record); break; }
			require(bool(selected), "All-owner invalid fixture missing " + type);
			std::string field = selected["xOffset"] ? "xOffset" : "x";
			auto original = selected[field].as<uint32_t>(); auto impossible = world.getCellsWide() + 1;
			selected[field] = impossible;
			auto malformedBinary = binary;
			auto recordAt = malformedBinary.find(binaryField("type", type));
			require(recordAt != std::string::npos, "Binary owner record missing");
			auto fragment = binaryField(field, original);
			auto fieldAt = malformedBinary.find(fragment, recordAt);
			require(fieldAt != std::string::npos, "Binary owner geometry missing");
			malformedBinary.replace(fieldAt, fragment.size(), binaryField(field, impossible));
			for (bool binaryInput : {false, true})
			{
				std::unique_ptr<Serializer> reader = binaryInput
					? std::unique_ptr<Serializer>(BinarySerializer::fromString(malformedBinary))
					: std::unique_ptr<Serializer>(YamlSerializer::fromString(YAML::Dump(invalid)));
				reader->deserialize(); bool refused = false;
				try { refused = !world.deserialize(*reader, data); } catch (std::exception const&) { refused = true; }
				require(refused && world.getGraph() == graph && layout(world) == prior && !world.isModified()
					&& world.getTopologyGeneration() == generation && world.isTraversalTopologyValid(),
					"Mixed invalid " + type + (binaryInput ? " binary" : " YAML") + " document was not transactional");
			}
		}
		// Historical authored records migrate through the current policy, never
		// an old allocator. Schema 49 predates the consolidated document schema.
		auto historical = YAML::Load(yaml); historical["version"] = 49;
		auto oldBinary = binary; auto version = binaryField("version", uint32_t{50});
		auto at = oldBinary.find(version); require(at != std::string::npos, "Binary version field missing");
		oldBinary.replace(at, version.size(), binaryField("version", uint32_t{49}));
		for (bool binaryInput : {false, true})
		{
			std::unique_ptr<Serializer> reader = binaryInput
				? std::unique_ptr<Serializer>(BinarySerializer::fromString(oldBinary))
				: std::unique_ptr<Serializer>(YamlSerializer::fromString(YAML::Dump(historical)));
			reader->deserialize(); World loaded("Historical", 1, 1);
			require(loaded.deserialize(*reader, data) && layout(loaded) == prior,
				"Historical mixed layout failed canonical migration or changed stable controls/permissions");
		}
	}

	void mixedStructuralReflow()
	{
		using namespace core;
		for (bool reverse : {false, true})
		{
			World world("Mixed structural reflow", 12, 5); world.addLayer();
			auto front = world.addRoom("Front", 0, 0, 0, 12, 5);
			world.addSectorWalkway(front, 2, 1); world.addSectorWalkway(front, 2, 2);
			world.addRoom("Neighbour", 1, 0, 0, 1, 5);
			auto room = world.addRoom("Room", 1, 0, 1, 2, 5);
			auto back = world.addRoom("Back", 2, 0, 2, 1, 5);
			world.addSectorWalkway(back, 2, 0);
			for (auto level : {2u, 4u})
				for (auto x : {0u, 1u}) world.addSectorWalkway(room, level, x);
			world.pauseSimulation();
			auto callPermission = world.addAccessPermission("Platform call");
			auto ladderPermission = world.addAccessPermission("Ladder extension");
			World::CreateLiftOptions platform; platform.stopOffsets = {0, 2}; platform.initialStop = 0;
			platform.landingControlPermissionRequirements = {{}, {callPermission}};
			World::CreateLadderOptions ladder{0, true, false}; ladder.controlPermissionRequirements[0] = {ladderPermission}; ladder.controlPermissionRequirements[1] = {ladderPermission};
			if (reverse) world.addRoomLadder(room, 2, 1, ladder);
			world.addSectorPlatformLift(room, 0, 0, platform);
			world.addRoomLadder(room, 0, 1, ladder);
			if (!reverse) world.addRoomLadder(room, 2, 1, ladder);
			world.addSectorDoor(0, 2, 1, World::RemoteControlledDoor1Options);
			world.addSectorLightSwitch(room, 0);
			world.finishBuild(); world.pauseSimulation();
			auto row = [&](World const& scene)
			{
				auto buttons = buttonsIn(scene, room);
				std::erase_if(buttons, [](auto b) { return (b->getPosition().y < 2 || b->getPosition().y >= 3) || b->getPosition().x + b->getSize().x * 0.5f == 1.5f; });
				std::sort(buttons.begin(), buttons.end(), [](auto a, auto b)
				{ return std::tuple{a->getPosition().x, a->getPosition().y} < std::tuple{b->getPosition().x, b->getPosition().y}; });
				return buttons;
			};
			auto verify = [&](World const& scene, std::vector<float> centres)
			{
				auto buttons = row(scene); require(buttons.size() == centres.size(), "Mixed reflow omitted a demand");
				std::map<float, std::shared_ptr<const Vertex>> approaches;
				for (size_t i = 0; i < buttons.size(); ++i)
				{
					auto button = buttons[i]; auto centre = button->getPosition() + button->getSize() * 0.5f;
					require(centre.x == centres[i], "Mixed reflow did not minimise stacks/preferences: count " + std::to_string(buttons.size()) + " member " + std::to_string(i) + " expected " + std::to_string(centres[i]) + " got " + std::to_string(centre.x));
					auto point = scene.lookupInteractionPoint(button->getInteractionPointId());
					require(point && point.entity->getPosition() == Vector2{centre.x, 2}, "Mixed reflow elevated interaction");
					require(scene.getObjectAtPosition(1, centre.x, centre.y) == button, "Mixed reflow lost independent targeting");
					for (uint32_t slot = 0; slot < scene.getSector(room)->getNumObjects(); ++slot)
					{
						auto object = scene.getSector(room)->getObject(slot);
						if (!object || object->_getObject() != button) continue;
						auto vertex = scene.getGraph()->getVertexForObject(std::const_pointer_cast<SectorObject>(object));
						require(vertex && vertex->getPosition() == Vector2{centre.x, 2}, "Mixed reflow lost normal graph approach");
						if (approaches.contains(centre.x)) require(approaches[centre.x] == vertex, "Mixed stack duplicated approach");
						approaches[centre.x] = vertex;
					}
				}
				return buttons;
			};
			verify(world, {2, 2, 2, 2});
			auto graph = world.getGraph(); auto prior = row(world); auto modified = world.isModified();
			bool refused = false;
			try { world.addSectorDoor(0, 2, 2, World::RemoteControlledDoor1Options); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && row(world) == prior && world.isModified() == modified,
				"Fifth mixed Button creation was not atomic");
			world.removeLocationWall(room, 2, CORE_SIDE_LEFT); world.finishBuild();
			verify(world, {1, 2, 2, 2});
			world.pauseSimulation(); auto extra = world.addSectorDoor(0, 2, 2, World::RemoteControlledDoor1Options); world.finishBuild();
			auto buttons = verify(world, {1, 2, 2, 2, 2});
			// Canonical order puts Door at 1, and Platform before the other
			// Door and both Ladders at 2. Permissions remain operation-specific.
			require(world.getInteractionPointPermissionRequirement(buttons[1]->getInteractionPointId()) == std::vector<AccessPermissionId>{callPermission}, "Reflow swapped Platform authorization");
			require(world.getInteractionPointPermissionRequirement(buttons[4]->getInteractionPointId()) == std::vector<AccessPermissionId>{ladderPermission}, "Reflow swapped Ladder authorization");
			world.pauseSimulation(); auto actor = world.createAgent("Caller", room, 2, 1.0f);
			world.grantAgentAccessPermission(actor, callPermission); world.resumeSimulation();
			auto denied = world.requestInteraction(buttons[4]->getInteractionPointId(), actor);
			require(world.lookupInteractionRequest(denied).entity->getResult() == InteractionResult::Rejected, "Reflow merged stacked permissions");
			auto selected = world.requestInteraction(buttons[1]->getInteractionPointId(), actor);
			world.advanceTick(); world.pauseSimulation(); world.markSaved();
			graph = world.getGraph(); prior = row(world); modified = world.isModified();
			auto request = world.lookupInteractionRequest(selected).entity;
			auto result = request->getResult(); auto operations = request->getOperations();
			refused = false;
			try { world.addLocationWall(room, 2, CORE_SIDE_LEFT); } catch (std::exception const&) { refused = true; }
			require(refused && world.getGraph() == graph && row(world) == prior && world.isModified() == modified
				&& world.lookupInteractionRequest(selected).entity == request && request->getResult() == result && request->getOperations() == operations,
				"Impossible mixed wall restoration mutated layout/topology/operations");
			for (uint32_t tick = 0; tick < 100; ++tick)
			{
				world.resumeSimulation(); world.advanceTick(); verify(world, {1, 2, 2, 2, 2});
				require(world.lookupAgent(actor).entity->getGlobalPosition().y == 2, "Reflow raised stacked-control Agent");
			}
			require(request->getOperations().size() == 1, "Selected reflowed Platform control lost/merged commands");
			require(world.setAgentRuntimeAccessPermissionGrant(actor, ladderPermission, true), "Ladder runtime grant failed");
			auto extension = world.requestInteraction(buttons[4]->getInteractionPointId(), actor);
			auto extensionRequest = world.lookupInteractionRequest(extension).entity;
			require(extensionRequest && extensionRequest->getResult() != InteractionResult::Rejected, "Selected reflowed Ladder control refused authorized actor");
			for (uint32_t tick = 0; tick < 30 && extensionRequest->getOperations().empty(); ++tick) world.advanceTick();
			require(extensionRequest->getOperations().size() == 1 && world.lookupAgent(actor).entity->getGlobalPosition().y == 2,
				"Selected reflowed Ladder merged commands or elevated its actor");
			verify(world, {1, 2, 2, 2, 2});
			world.pauseSimulation();
			SerializationWorkData data;
			auto replay = [&]<typename Serializer>()
			{
				auto writer = Serializer::toString(); world.serialize(*writer, data); writer->serialize();
				auto reader = Serializer::fromString(writer->getSerializedString()); reader->deserialize();
				World loaded("Placeholder", 1, 1); require(loaded.deserialize(*reader, data), "Mixed reconstruction failed");
				verify(loaded, {1, 2, 2, 2, 2});
			};
			replay.template operator()<YamlSerializer>(); replay.template operator()<BinarySerializer>();
			// Removing the extra Door makes restoring the wall feasible again.
			require(world.removeSectorDoor(extra.door.sector->getIndex(), extra.door.index), "Mixed Door deletion failed");
			verify(world, {1, 2, 2, 2});
			world.pauseSimulation(); world.addLocationWall(room, 2, CORE_SIDE_LEFT); world.finishBuild();
			verify(world, {2, 2, 2, 2});
		}
		// Adding support shortens the Ladder, but its new upper endpoint would
		// have neither a wall-safe left host nor permanent right support. The
		// detached mixed replay must fail without starting a live structural edit.
		World world("Mixed support refusal", 8, 5);
		world.addRoom("Neighbour", 1, 0, 0, 1, 5);
		auto room = world.addRoom("Room", 1, 0, 1, 2, 5);
		world.addRoom("Front", 0, 0, 0, 8, 5);
		world.addSectorWalkway(room, 4, 0); world.addSectorWalkway(room, 4, 1);
		World::CreateLiftOptions options; options.stopOffsets = {0, 4};
		world.addSectorPlatformLift(room, 0, 1, options);
		world.addRoomLadder(room, 0, 0, {0, true, false});
		world.addSectorDoor(0, 0, 1, World::RemoteControlledDoor1Options);
		world.finishBuild(); world.pauseSimulation(); world.markSaved();
		auto graph = world.getGraph(); auto buttons = buttonsIn(world, room);
		auto generation = world.getTopologyGeneration(); bool refused = false;
		try { world.addSectorWalkway(room, 2, 0); } catch (std::exception const&) { refused = true; }
		require(refused && world.getGraph() == graph && buttonsIn(world, room) == buttons && !world.isModified()
			&& !world.isTraversalTopologyDirty() && world.isTraversalTopologyValid() && world.getTopologyGeneration() == generation,
			"Refused mixed support edit dirtied topology/document or changed derived controls");
		require(world.resumeSimulation(), "Refused support edit broke resumable topology");
	}
}

void runDoorTwoSidedButtonSmokeChecks()
{
	allOwnerReconstruction();
	mixedStructuralReflow();
	fixedInsetControls();
	independentEndpointControls();
	deterministicTransportControls();
	canonicalOnlyBoundary();
	canonicalOrderContract();
	canonicalSideReassignment();
	sharedApproachStacks();
	completeOptimisation();
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
