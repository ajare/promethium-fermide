#include "WorldChecks.h"
#include "core/Agent.h"
#include "core/BinarySerializer.h"
#include "core/DoorSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/AirlockTransit.h"
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

		{
		// A Door authored Broken also carries the frozen-open fraction it is stuck
		// at (ordinary Doors only, including button Doors). It persists, resets to
		// the authored amount, and the live setter is ordinary-Door-only.
		core::World world("Broken open percentage", 12, 3);
		auto front = world.addRoom("Front", 0, 0, 0, 11, 1);
		world.addRoom("Back", 1, 0, 0, 11, 1);
		core::World::CreateDoorOptions authored;
		authored.activationMode = core::DoorActivationMode::RemoteControlled;
		authored.controls[0] = authored.controls[1] = true;
		authored.initiallyBroken = true;
		authored.brokenOpenPercentage = 0.35f;
		auto made = world.addSectorDoor(0, 0, 2, authored);
		world.finishBuild();
		auto door = std::static_pointer_cast<core::DoorSectorObject>(
			made.door.sector->getObject(made.door.index))->getDoor();
		require(door->isBroken() && door->isInitiallyBroken()
			&& door->getOpenPercentage() == 0.35f && door->getBrokenOpenPercentage() == 0.35f,
			"Authored Broken open percentage did not freeze the Door");
		core::World::CreateDoorOptions readBack;
		require(world.getSectorDoorOptions(0, 0, 2, 1, readBack)
			&& readBack.initiallyBroken && readBack.brokenOpenPercentage == 0.35f,
			"Authored options lost the Broken open percentage");
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
			require(restored.deserialize(*reader, work), "Broken-open-percentage document failed to load");
			auto loaded = std::static_pointer_cast<core::DoorSectorObject>(
				restored.getSector(front)->getObject(made.door.index))->getDoor();
			require(loaded->isBroken() && loaded->isInitiallyBroken()
				&& loaded->getOpenPercentage() == 0.35f && loaded->getBrokenOpenPercentage() == 0.35f,
				"Broken open percentage did not persist");
		}
		world.resetSimulation();
		door = std::static_pointer_cast<core::DoorSectorObject>(
			world.getSector(front)->getObject(made.door.index))->getDoor();
		require(door->isBroken() && door->getOpenPercentage() == 0.35f,
			"Reset did not restore the authored Broken open percentage");
		world.pauseSimulation();
		require(world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 0.8f)
			&& door->getBrokenOpenPercentage() == 0.8f && door->getOpenPercentage() == 0.8f,
			"Paused Broken-open-percentage edit refused or did not apply");
		require(!world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 1.5f)
			&& !world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), std::nanf(""))
			&& door->getBrokenOpenPercentage() == 0.8f, "Invalid percentage was accepted");
		world.markSaved();
		world.resumeSimulation();
		require(!world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 0.5f)
			&& !world.isModified(), "Running authored edit was accepted/dirtied the document");
		// The new field requires schema 58; an older reader refuses it.
		auto legacy = YAML::Load(save(false)); legacy["version"] = 57;
		core::World old("Old", 1, 1);
		core::SerializationWorkData work;
		bool refused = false;
		try
		{
			auto legacyReader = core::YamlSerializer::fromString(YAML::Dump(legacy));
			legacyReader->deserialize();
			old.deserialize(*legacyReader, work);
		}
		catch (core::SerializationException const&) { refused = true; }
		require(refused, "Old-schema Broken open percentage was silently accepted");
		}
	}
	void bulkheadDoorBrokenLifecycle(smoke::Context const& context)
	{
		using smoke::require;
		for (auto mode : { core::DoorActivationMode::Automatic, core::DoorActivationMode::Manual,
			core::DoorActivationMode::RemoteControlled, core::DoorActivationMode::Unavailable })
		{
			core::World world("Authored Broken Bulkhead Door", 12, 3);
			auto front = world.addRoom("Left", 0, 0, 0, 5, 1);
			world.addRoom("Right", 0, 0, 5, 6, 1);
			core::World::CreateBulkheadDoorOptions options;
			options.activationMode = mode;
			options.controls[0] = options.controls[1] = mode == core::DoorActivationMode::RemoteControlled;
			options.initiallyBroken = true;
			auto made = world.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_RIGHT, options);
			world.finishBuild();
			auto id = world.createAgent("Bystander", front, 0, 0.5f);
			auto door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(made.door.sector->getObject(made.door.index))->getDoor();
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
				auto loaded = std::static_pointer_cast<core::BulkheadDoorSectorObject>(restored.getSector(front)->getObject(made.door.index))->getDoor();
				require(loaded->isBroken() && loaded->isInitiallyBroken() && loaded->getOpenPercentage() == 0
					&& loaded->getActivationMode() == mode, "Persistence saved live rather than authored Door condition");
				require(!restored.lookupAgent(id).entity->rememberedDeviceCondition(loaded->getTraversalResourceId()), "Memory was persisted");
			}
			world.resetSimulation();
			door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(world.getSector(front)->getObject(made.door.index))->getDoor();
			require(door->isBroken() && door->isInitiallyBroken() && door->getOpenPercentage() == 0
				&& !world.lookupAgent(id).entity->rememberedDeviceCondition(door->getTraversalResourceId()), "Reset did not restore authored condition/clear memory");
			world.pauseSimulation();
			core::World::CreateBulkheadDoorOptions edited;
			require(world.getSectorBulkheadDoorOptions(front, made.door.index, edited)
				&& edited.initiallyBroken, "Authored Bulkhead options lost initial condition");
			edited.holdOpenSeconds += 1.0f;
			auto editedObject = world.applySectorBulkheadDoorOptions(front, made.door.index, edited);
			door = std::static_pointer_cast<const core::BulkheadDoorSectorObject>(editedObject)->getDoor();
			require(door->isInitiallyBroken() && door->isBroken(), "Settings replay lost initial Broken");
			require(world.setDoorInitiallyBroken(door->getTraversalResourceId(), false) && !door->isBroken()
				&& world.isModified(), "Paused authored toggle failed");
			world.markSaved(); world.setDoorBroken(door->getTraversalResourceId(), true);
			world.resetSimulation();
			door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(world.getSector(front)->getObject(made.door.index))->getDoor();
			require(!door->isBroken() && !door->isInitiallyBroken(), "Reset retained live breakage");

			// Existing schema 33 Worlds omit this field and still work. A field in
			// an older schema is rejected transactionally, rather than discarded.
			auto legacy = YAML::Load(save(false)); legacy["version"] = 33;
			for (auto record : legacy["construction"])
				record.remove("initiallyBroken");
			core::World old("Old", 1, 1);
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
			require(old.deserialize(*reader, work), "Legacy Door document failed");
			core::World::CreateBulkheadDoorOptions legacyOptions;
			require(old.getSectorBulkheadDoorOptions(front, made.door.index, legacyOptions) && !legacyOptions.initiallyBroken, "Legacy Door defaulted Broken");
			for (auto record : legacy["construction"])
				if (record["type"].as<std::string>() == "bulkheadDoor") record["initiallyBroken"] = true;
			bool refused = false;
			try
			{
				reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
				old.deserialize(*reader, work);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused && old.getName() == "Authored Broken Bulkhead Door", "Old-schema Broken data was silently dropped or mutated target");
		}
		auto demo = core::loadWorldDocument(context.fixture("resources/test-worlds/broken-bulkhead-doors.world.yaml"));
		require(demo && demo->getLoadWarnings().empty(), "Bulkhead demo did not load its journeys");
		demo->advanceTicks(3600);
		for (auto id : { core::AgentId{ 1 }, core::AgentId{ 2 } })
		{
			auto agent = demo->lookupAgent(id).entity;
			require(agent && agent->getState() == core::Agent::State::Idle
				&& agent->getSector()->getIndex() == 1
				&& std::abs(agent->getLocalPosition().x - 0.5f) < 0.001f,
				"Bulkhead demo observer did not complete its detour/approach");
		}

		// A standalone Bulkhead Door authored Broken also carries the frozen-open
		// fraction it is stuck at; Chamber Doors stay excluded.
		{
			core::World world("Broken Bulkhead open percentage", 12, 3);
			auto left = world.addRoom("Left", 0, 0, 0, 5, 1);
			world.addRoom("Right", 0, 0, 5, 6, 1);
			core::World::CreateBulkheadDoorOptions authored;
			authored.activationMode = core::DoorActivationMode::RemoteControlled;
			authored.controls[0] = authored.controls[1] = true;
			authored.initiallyBroken = true;
			authored.brokenOpenPercentage = 0.35f;
			auto made = world.addSectorBulkheadDoor(0, 0, 4, CORE_SIDE_RIGHT, authored);
			world.finishBuild();
			auto door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(
				made.door.sector->getObject(made.door.index))->getDoor();
			require(door->isBroken() && door->isInitiallyBroken()
				&& door->getOpenPercentage() == 0.35f && door->getBrokenOpenPercentage() == 0.35f,
				"Authored Broken open percentage did not freeze the Bulkhead Door");
			core::World::CreateBulkheadDoorOptions readBack;
			require(world.getSectorBulkheadDoorOptions(left, made.door.index, readBack)
				&& readBack.initiallyBroken && readBack.brokenOpenPercentage == 0.35f,
				"Authored options lost the Bulkhead Broken open percentage");
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
				require(restored.deserialize(*reader, work), "Bulkhead broken-open-percentage document failed to load");
				auto loaded = std::static_pointer_cast<core::BulkheadDoorSectorObject>(
					restored.getSector(left)->getObject(made.door.index))->getDoor();
				require(loaded->isBroken() && loaded->isInitiallyBroken()
					&& loaded->getOpenPercentage() == 0.35f && loaded->getBrokenOpenPercentage() == 0.35f,
					"Bulkhead Broken open percentage did not persist");
			}
			world.resetSimulation();
			door = std::static_pointer_cast<core::BulkheadDoorSectorObject>(
				world.getSector(left)->getObject(made.door.index))->getDoor();
			require(door->isBroken() && door->getOpenPercentage() == 0.35f,
				"Reset did not restore the authored Bulkhead Broken open percentage");
			world.pauseSimulation();
			require(world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 0.8f)
				&& door->getBrokenOpenPercentage() == 0.8f && door->getOpenPercentage() == 0.8f,
				"Paused Bulkhead Broken-open-percentage edit refused or did not apply");
			require(!world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 1.5f)
				&& !world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), std::nanf(""))
				&& door->getBrokenOpenPercentage() == 0.8f, "Invalid percentage was accepted");
			world.markSaved();
			world.resumeSimulation();
			require(!world.setDoorBrokenOpenPercentage(door->getTraversalResourceId(), 0.5f)
				&& !world.isModified(), "Running Bulkhead authored edit was accepted/dirtied the document");

			// Chamber Doors are Bulkhead Doors but are not independently breakable,
			// so they never take a broken open percentage.
			core::World airlockWorld("Airlock exclusion", 12, 3);
			airlockWorld.addRoom("Left", 0, 0, 0, 4, 1);
			airlockWorld.addRoom("Right", 0, 0, 6, 6, 1);
			auto chamberIndex = airlockWorld.addAirlock(0, 0, 4, 2);
			airlockWorld.finishBuild();
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(
				airlockWorld.getSector(chamberIndex));
			require(chamber != nullptr, "Airlock did not build");
			for (int side = 0; side < 2; ++side)
			{
				auto chamberDoor = chamber->getDoor(side);
				require(chamberDoor && !chamberDoor->isBreakable(), "Airlock Door became independently breakable");
				require(!airlockWorld.setDoorBrokenOpenPercentage(chamberDoor->getTraversalResourceId(), 0.5f),
					"Airlock Door accepted a broken open percentage");
			}
		}
	}
}
