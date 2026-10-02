#include "WorldChecks.h"
#include "core/Agent.h"
#include "core/BinarySerializer.h"
#include "core/DoorSectorObject.h"
#include "core/LiftTransit.h"
#include "core/World.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/YamlSerializer.h"
#include "core/SerializationException.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void ordinaryDoorBrokenLifecycle(smoke::Context const& context)
	{
		using smoke::require;
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled, core::DoorActivationMode::Unavailable })
		{
			core::World world("Authored Broken Door", 12, 3);
			auto front = world.addRoom("Front", 0, 0, 0, 11, 1);
			world.addRoom("Back", 1, 0, 0, 11, 1);
			core::World::CreateDoorOptions options;
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			options.initiallyBroken = true;
			options.openStyle = core::Door::OpenStyle::OpenApart;
			options.height = core::Door::Height::Tall;
			auto made = world.addSectorDoor(0, 0, 2, options);
			world.finishBuild();
			auto id = world.createAgent("Bystander", front, 0, 0.5f);
			auto door = std::static_pointer_cast<core::DoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
			require(door->isBroken() && door->isInitiallyBroken(), "Initial Broken did not apply");
			world.markSaved();
			require(!world.setDoorInitiallyBroken(made.traversalResource, false) && !world.isModified(), "Running authored toggle mutated document");
			world.advanceTick();
			require(world.lookupAgent(id).entity->rememberedDeviceCondition(made.traversalResource).has_value(), "Bystander has no local memory");
			require(world.setDoorBroken(made.traversalResource, false) && !world.isModified()
				&& door->isInitiallyBroken(), "Live restoration changed authored state/dirty state");
			door->requestOpen(); door->update(door->getOpenCloseTime() * 0.5f);
			world.setDoorBroken(made.traversalResource, true);
			auto save = [&](bool binary)
			{
				auto write = [&](auto writer)
				{
					core::SerializationWorkData work; work.markSerializedUnmodified = false;
					world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
				};
				return binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
			};
			for (bool binary : { false, true })
			{
				auto saved = save(binary);
				std::unique_ptr<core::Serializer> reader;
				if (binary) reader = core::BinarySerializer::fromString(saved);
				else reader = core::YamlSerializer::fromString(saved);
				reader->deserialize(); core::SerializationWorkData work;
				core::World restored("Loaded", 1, 1);
				require(restored.deserialize(*reader, work), "Broken Door document failed to load");
				auto loaded = std::static_pointer_cast<core::DoorSectorObject>(restored.getSector(front)->getObject(made.door.index))->getDoor();
				require(loaded->isBroken() && loaded->isInitiallyBroken() && loaded->getOpenPercentage() == 0
					&& loaded->getOpenStyle() == options.openStyle && loaded->getHeight() == options.height
					&& loaded->getActivationMode() == mode, "Persistence saved live rather than authored Door condition");
				require(!restored.lookupAgent(id).entity->rememberedDeviceCondition(loaded->getTraversalResourceId()), "Memory was persisted");
			}
			world.resetSimulation();
			door = std::static_pointer_cast<core::DoorSectorObject>(world.getSector(front)->getObject(made.door.index))->getDoor();
			require(door->isBroken() && door->isInitiallyBroken() && door->getOpenPercentage() == 0
				&& !world.lookupAgent(id).entity->rememberedDeviceCondition(door->getTraversalResourceId()), "Reset did not restore authored condition/clear memory");
			world.pauseSimulation();
			auto permission = world.addAccessPermission("Operate");
			if (mode == core::DoorActivationMode::Manual)
			{
				require(world.setManualDoorPermissionRequirement(door->getTraversalResourceId(), { permission }), "Manual requirement refused");
				world.setDoorBroken(door->getTraversalResourceId(), false);
				require(world.getManualDoorPermissionRequirement(door->getTraversalResourceId()) == std::vector<core::AccessPermissionId>{ permission }, "Break/restore changed permissions");
			}
			require(world.setDoorInitiallyBroken(door->getTraversalResourceId(), false) && !door->isBroken()
				&& world.isModified(), "Paused authored toggle failed");
			world.markSaved(); world.setDoorBroken(door->getTraversalResourceId(), true);
			world.resetSimulation();
			door = std::static_pointer_cast<core::DoorSectorObject>(world.getSector(front)->getObject(made.door.index))->getDoor();
			require(!door->isBroken() && !door->isInitiallyBroken(), "Reset retained live breakage");

			// Existing schema 32 Worlds omit this field and still work. A field in
			// an older schema is rejected transactionally, rather than discarded.
			auto legacy = YAML::Load(save(false)); legacy["version"] = 32;
			for (auto record : legacy["construction"])
				record.remove("initiallyBroken");
			core::World old("Old", 1, 1);
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
			require(old.deserialize(*reader, work), "Legacy Door document failed");
			core::World::CreateDoorOptions legacyOptions;
			require(old.getSectorDoorOptions(0, 0, 2, 1, legacyOptions) && !legacyOptions.initiallyBroken, "Legacy Door defaulted Broken");
			for (auto record : legacy["construction"])
				if (record["type"].as<std::string>() == "door") record["initiallyBroken"] = true;
			bool refused = false;
			try
			{
				reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
				old.deserialize(*reader, work);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused && old.getName() == "Authored Broken Door", "Old-schema Broken data was silently dropped or mutated target");
		}

		// Excluded resources must not acquire independent breakability.
		core::World world("Excluded devices", 12, 3);
		auto hall = world.addRoom("Hall", 0, 0, 0, 11, 3);
		for (uint32_t x = 0; x < 11; ++x) world.addSectorWalkway(hall, 1, x);
		world.addRoom("Back", 1, 0, 6, 5, 3);
		auto window = world.addSectorWindow(0, 0, 8, 1, 1);
		core::World::CreateLiftOptions options; options.levelsHigh = 3; options.stopOffsets = { 0, 1 };
		auto lift = world.addLift(1, 0, 1, options);
		world.finishBuild(); world.pauseSimulation();
		for (auto const& made : lift.doors)
			require(!world.setDoorBroken(made.traversalResource, true)
				&& !world.setDoorInitiallyBroken(made.traversalResource, true), "Transport Door became independently breakable");
		(void)window;
		for (auto const& resource : world.getSimulationSnapshot().traversalResources)
			require(!world.setDoorBroken(resource.id, true), "Window or transport resource became independently breakable");
		core::World bulkheadWorld("Bulkhead follow-up", 8, 2);
		bulkheadWorld.addRoom("Left", 0, 0, 0, 3, 1);
		bulkheadWorld.addRoom("Right", 0, 0, 3, 3, 1);
		auto bulkhead = bulkheadWorld.addSectorBulkheadDoor(0, 0, 3, CORE_SIDE_LEFT);
		bulkheadWorld.finishBuild(); bulkheadWorld.pauseSimulation();
		require(!bulkheadWorld.setDoorBroken(bulkhead.traversalResource, true), "Bulkhead follow-up implemented accidentally");
		require(!world.setDoorBroken({}, true), "Invalid resource became breakable");

		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-ordinary-doors.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Demo did not load its authored journeys");
		demo->advanceTicks(3600);
		for (auto id : { core::AgentId{ 1 }, core::AgentId{ 2 } })
		{
			auto agent = demo->lookupAgent(id).entity;
			require(agent && agent->getState() == core::Agent::State::Idle
				&& agent->getSector()->getIndex() == (id.value == 1 ? 1u : 0u)
				&& std::abs(agent->getLocalPosition().x - 0.5f) < 0.001f,
				"Demo observer did not replan and complete its journey");
		}
	}
}
