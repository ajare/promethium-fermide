#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/Exceptions.h"

#include "core/Graph.h"
#include "core/World.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Defines.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/SerializationException.h"
#include "core/LadderTransit.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include "core/Stairwell.h"
#include "core/StairwellTransit.h"
#include "core/Staircase.h"
#include "core/StaircaseTransit.h"
#include "core/YamlSerializer.h"

#include "WorldChecks.h"

namespace persistence
{
	using smoke::require;

	void bulkheadDoorsSupportIndependentObjectEditing(smoke::Context const&)
	{
		core::World world("Bulkhead editor", 7, 2);
		auto const left = world.addRoom("Left", 0, 0, 0, 2, 1);
		world.addRoom("Middle", 0, 0, 2, 2, 1);
		world.addRoom("Right", 0, 0, 4, 2, 1);
		std::string diagnostic;
		require(world.canAddSectorBulkheadDoor(0, 0, 2,
			CORE_SIDE_LEFT, {}, &diagnostic), "valid left-edge Bulkhead Door placement was rejected");
		require(!world.canAddSectorBulkheadDoor(0, 0, 0,
			CORE_SIDE_LEFT, {}, &diagnostic), "Bulkhead Door was accepted at the world edge");
		require(!world.canAddSectorBulkheadDoor(0, 0, 1,
			CORE_SIDE_LEFT, {}, &diagnostic), "Bulkhead Door was accepted inside one Location");

		auto created = world.addSectorBulkheadDoor(0, 0, 2, CORE_SIDE_LEFT);
		std::shared_ptr<const core::SectorObject> object =
			created.door.sector->getObject(created.door.index);
		require(object && object->getObjectType() == core::SectorObjectType::BulkheadDoor
			&& object->getCellX() + 1 == 2,
			"Bulkhead Door was not created on the selected cell's left edge");
		uint32_t ownedControls = 0;
		for (auto const& sector : world.getSectors(0))
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				if (world.isBulkheadDoorOwnedControl(sector->getObject(i))) ++ownedControls;
		require(ownedControls == 2, "Bulkhead Door controls were not recognized as managed objects");

		core::World::CreateBulkheadDoorOptions options;
		require(world.getSectorBulkheadDoorOptions(left, created.door.index, options)
			&& options.controls[0] && options.controls[1]
			&& options.activationMode == core::DoorActivationMode::RemoteControlled
			&& std::abs(options.automaticSensorDistance
				- CORE_BULKHEAD_DOOR_AUTOMATIC_SENSOR_DISTANCE) < 0.0001f,
			"Bulkhead Door authored options could not be read");
		world.finishBuild();
		world.pauseSimulation();
		options.controls[0] = options.controls[1] = false;
		options.activationMode = core::DoorActivationMode::Manual;
		options.holdOpenSeconds = 3.0f;
		options.crossingLanes = 1;
		options.automaticSensorDistance = 0.75f;
		object = world.applySectorBulkheadDoorOptions(left, created.door.index, options);
		require(object && object->getCellX() + 1 == 2,
			"Bulkhead Door settings edit lost the selected object");

		auto owner = object->getSector();
		uint32_t objectIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == object) { objectIndex = i; break; }
		auto plan = world.planMoveSectorObject(owner->getIndex(), objectIndex, 4, 0);
		require(plan.valid, "Bulkhead Door move to another left-edge boundary was rejected");
		object = world.applyObjectMove(plan);
		require(object && object->getCellX() + 1 == 4,
			"Bulkhead Door move did not use the target cell's left edge");
		owner = object->getSector(); objectIndex = ~0u;
		for (uint32_t i = 0; i < owner->getNumObjects(); ++i)
			if (owner->getObject(i) == object) { objectIndex = i; break; }
		require(world.getSectorBulkheadDoorOptions(owner->getIndex(), objectIndex, options)
			&& options.activationMode == core::DoorActivationMode::Manual
			&& !options.controls[0] && !options.controls[1]
			&& std::abs(options.holdOpenSeconds - 3.0f) < 0.0001f
			&& options.crossingLanes == 1
			&& std::abs(options.automaticSensorDistance - 0.75f) < 0.0001f,
			"Bulkhead Door move did not preserve authored settings");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, workData); writer->serialize();
		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(writer->getSerializedString());
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Bulkhead Door world did not round-trip");
		auto loadedLeft = loaded.getSectorAtPosition(0, 3.5f, 0.5f);
		objectIndex = ~0u;
		for (uint32_t i = 0; loadedLeft && i < loadedLeft->getNumObjects(); ++i)
		{
			auto candidate = loadedLeft->getObject(i);
			if (candidate && candidate->getObjectType() == core::SectorObjectType::BulkheadDoor)
				{ objectIndex = i; break; }
		}
		require(loadedLeft && objectIndex != ~0u
			&& loaded.getSectorBulkheadDoorOptions(loadedLeft->getIndex(), objectIndex, options)
			&& std::abs(options.automaticSensorDistance - 0.75f) < 0.0001f,
			"Bulkhead Door authored settings did not round-trip");
		loaded.pauseSimulation();
		require(loaded.removeSectorBulkheadDoor(loadedLeft->getIndex(), objectIndex),
			"Bulkhead Door could not be deleted independently");
	}

	void doorOpeningStyleIsAuthoredPersistedAndLegacyDefaulted(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};

		core::World original("OpenUp door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenUp;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const createdDoor = findDoor(original, created.door.sector->getIndex());
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Ordinary Door creation did not carry the OpenUp opening style");
		core::World::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Authored Door options did not report the OpenUp opening style");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto const yaml = writer->getSerializedString();
		require(yaml.find("openStyle: openUp") != std::string::npos,
			"World YAML did not persist the Door's openStyle: openUp");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "OpenUp Door world did not round-trip");
		auto const loadedDoor = findDoor(loaded, created.door.sector->getIndex());
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Loaded Door lost its OpenUp opening style");
		require(loaded.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Loaded Door options lost the OpenUp opening style");

		// A move replays the authored record at a new position; the style rides along.
		loaded.pauseSimulation();
		uint32_t loadedDoorObjectIndex{ ~0u };
		{
			auto const sector = loaded.getSector(created.door.sector->getIndex());
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor() == loadedDoor)
					{ loadedDoorObjectIndex = i; break; }
			}
		}
		require(loadedDoorObjectIndex != ~0u, "Loaded Door object could not be found");
		auto const movePlan = loaded.planMoveSectorObject(
			loadedDoor->getFrontSector()->getIndex(), loadedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenUp Door move plan was rejected");
		require(loaded.applyObjectMove(movePlan) != nullptr, "OpenUp Door move failed");
		require(loaded.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Moving the Door did not preserve its OpenUp opening style");

		// Legacy records written before opening styles existed load as OpenUp.
		std::string legacyYaml;
		{
			std::string const needle = "    openStyle: openUp\n";
			auto const at = yaml.find(needle);
			require(at != std::string::npos, "Door openStyle line was not where expected");
			legacyYaml = yaml.substr(0, at) + yaml.substr(at + needle.size());
		}
		core::World legacy("placeholder", 1, 1);
		{
			auto legacyReader = core::YamlSerializer::fromString(legacyYaml);
			legacyReader->deserialize();
			require(legacy.deserialize(*legacyReader, workData),
				"Legacy Door record without an openStyle no longer loads");
			auto const legacyDoor = findDoor(legacy, created.door.sector->getIndex());
			require(legacyDoor && legacyDoor->getOpenStyle() == core::Door::OpenStyle::OpenUp,
				"Legacy Door without a style did not load as OpenUp");
		}

		// An unknown persisted style name fails with a clear serialization error.
		{
			auto const at = legacyYaml.find("crossingLanes:");
			require(at != std::string::npos, "Door crossingLanes line was not where expected");
			auto const lineEnd = legacyYaml.find('\n', at);
			auto const unknownStyle = legacyYaml.substr(0, lineEnd + 1)
				+ "    openStyle: openSideways\n" + legacyYaml.substr(lineEnd + 1);
			core::World rejected("placeholder", 1, 1);
			auto badReader = core::YamlSerializer::fromString(unknownStyle);
			badReader->deserialize();
			bool threw{ false };
			std::string message;
			try
			{
				rejected.deserialize(*badReader, workData);
			}
			catch (core::SerializationException const& error)
			{
				threw = true;
				message = error.what();
			}
			require(threw, "Unknown Door opening style was accepted");
			require(message.find("opening style") != std::string::npos
				&& message.find("openSideways") != std::string::npos,
				("Unknown Door opening style gave an imprecise diagnostic: " + message).c_str());
		}
	}

	void doorHeightPersistsAndIsLimitedToRooms(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};

		core::World original("Tall Room door", 8, 2);
		original.addRoom("Fore room", 0, 0, 0, 7, 1);
		original.addRoom("Back room", 1, 0, 0, 7, 1);
		core::World::CreateDoorOptions options;
		options.height = core::Door::Height::Tall;
		auto const created = original.addSectorDoor(0, 0, 3, options);
		original.finishBuild();
		auto const door = findDoor(original, created.door.sector->getIndex());
		require(door && door->getHeight() == core::Door::Height::Tall
			&& std::fabs(door->getSize().y - CORE_DOOR_TALL_HEIGHT) < 0.001f,
			"Room Door creation did not carry the tall height");
		require(std::fabs(door->getOpenCloseTime()
			- CORE_DOOR_OPEN_CLOSE_TIME * CORE_DOOR_TALL_HEIGHT / CORE_DOOR_HEIGHT) < 0.001f,
			"Tall OpenUp Door does not move at the regular Door's vertical speed");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("height: tall") != std::string::npos,
			"World YAML did not persist the tall Door height");
		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Tall Door world did not round-trip");
		require(findDoor(loaded, created.door.sector->getIndex())->getHeight()
			== core::Door::Height::Tall, "Loaded Door lost its tall height");

		auto const heightAt = yaml.find("    height: tall");
		require(heightAt != std::string::npos, "No Door height field in YAML fixture");
		yaml.erase(heightAt, yaml.find('\n', heightAt) - heightAt + 1);
		core::World legacy("placeholder", 1, 1);
		auto legacyReader = core::YamlSerializer::fromString(yaml);
		legacyReader->deserialize();
		require(legacy.deserialize(*legacyReader, workData), "Legacy Door record did not load");
		require(findDoor(legacy, created.door.sector->getIndex())->getHeight()
			== core::Door::Height::Regular, "Legacy Door did not default to regular height");

		core::World corridor("Corridor tall refusal", 8, 1);
		corridor.addCorridor(0, 0, 7);
		corridor.addCorridor(1, 0, 0, 7, 1);
		std::string diagnostic;
		require(!corridor.canAddCorridorDoor(0, 0, 3, options, &diagnostic),
			"A Corridor Door accepted the tall height");
	}

	void doorOpenLeftPersistsThroughEveryEditorPath(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		core::World original("OpenLeft door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Ordinary Door creation did not carry the OpenLeft opening style");
		core::World::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Authored Door options did not report the OpenLeft opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openLeft") != std::string::npos,
			"World YAML did not persist the Door's openStyle: openLeft");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Loaded Door lost its OpenLeft opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Loaded Door options lost the OpenLeft opening style");

		// The selected-Door editor's change: record and live Door move together.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Re-authoring the Door to OpenUp was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Re-authored Door record did not take the new opening style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"Re-authored live Door did not take the new opening style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door back to OpenLeft was refused: " + diagnostic).c_str());

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenUp, &diagnostic),
			("Second re-author to OpenUp was refused: " + diagnostic).c_str());
		auto const openUpSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openUpSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenUp,
			"Undo snapshot did not restore the OpenUp style");
		auto const redone = loadYaml(openLeftSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Redo snapshot did not restore the OpenLeft style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic) == false,
			"Opening-style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenLeft Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenLeft Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenLeft Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Moving the OpenLeft Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::World pasteTarget("OpenLeft paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Pasted Door did not carry the copied OpenLeft opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Pasted Door record lost the OpenLeft opening style");
	}

	void doorOpenRightPersistsThroughEveryEditorPath(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		core::World original("OpenRight door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenRight;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Ordinary Door creation did not carry the OpenRight opening style");
		core::World::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Authored Door options did not report the OpenRight opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openRight") != std::string::npos,
			"World YAML did not persist the Door's openStyle: openRight");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Loaded Door lost its OpenRight opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Loaded Door options lost the OpenRight opening style");

		// The selected-Door editor's change: record and live Door move together,
		// and OpenRight is never confused with its OpenLeft neighbour.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door to OpenLeft was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Re-authored Door record did not take the OpenLeft style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Re-authored live Door did not take the OpenLeft style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic),
			("Re-authoring the Door back to OpenRight was refused: " + diagnostic).c_str());
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Re-authored live Door did not return to OpenRight");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openRightSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Second re-author to OpenLeft was refused: " + diagnostic).c_str());
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openLeftSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Undo snapshot did not restore the OpenLeft style");
		auto const redone = loadYaml(openRightSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Redo snapshot did not restore the OpenRight style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenRight, &diagnostic) == false,
			"OpenRight style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenRight Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenRight Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenRight Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Moving the OpenRight Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::World pasteTarget("OpenRight paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenRight,
			"Pasted Door did not carry the copied OpenRight opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenRight,
			"Pasted Door record lost the OpenRight opening style");
	}

	void doorOpenApartPersistsThroughEveryEditorPath(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		auto snapshotYaml = [](core::World& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};
		auto loadYaml = [](std::string const& yaml)
		{
			auto world = std::make_shared<core::World>("placeholder", 1, 1);
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			world->deserialize(*reader, workData);
			return world;
		};

		core::World original("OpenApart door", 8, 3);
		original.addRoom("Fore room", 0, 0, 0, 7, 2);
		original.addRoom("Back room", 1, 0, 0, 7, 2);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenApart;
		auto const created = original.addSectorDoor(0, 0, 3, doorOptions);
		original.finishBuild();
		auto const doorSectorIndex = created.door.sector->getIndex();
		auto const createdDoor = findDoor(original, doorSectorIndex);
		require(createdDoor && createdDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Ordinary Door creation did not carry the OpenApart opening style");
		core::World::CreateDoorOptions readBack;
		require(original.getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Authored Door options did not report the OpenApart opening style");

		// Save/load: the record persists the style and replays it on load.
		auto const yaml = snapshotYaml(original);
		require(yaml.find("openStyle: openApart") != std::string::npos,
			"World YAML did not persist the Door's openStyle: openApart");
		auto loaded = loadYaml(yaml);
		auto const loadedDoor = findDoor(*loaded, doorSectorIndex);
		require(loadedDoor && loadedDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Loaded Door lost its OpenApart opening style");
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Loaded Door options lost the OpenApart opening style");

		// The selected-Door editor's change: record and live Door move together,
		// and OpenApart is never confused with its OpenLeft neighbour.
		loaded->pauseSimulation();
		std::string diagnostic;
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Re-authoring the Door to OpenLeft was refused: " + diagnostic).c_str());
		require(loaded->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Re-authored Door record did not take the OpenLeft style");
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenLeft,
			"Re-authored live Door did not take the OpenLeft style");
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenApart, &diagnostic),
			("Re-authoring the Door back to OpenApart was refused: " + diagnostic).c_str());
		require(findDoor(*loaded, doorSectorIndex)->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Re-authored live Door did not return to OpenApart");

		// Undo/redo mirror: the editor snapshots YAML before and after a change
		// and restores either side verbatim. Both sides carry their own style.
		auto const openApartSnapshot = snapshotYaml(*loaded);
		require(loaded->setSectorDoorOpenStyle(0, 0, 3, 2,
			core::Door::OpenStyle::OpenLeft, &diagnostic),
			("Second re-author to OpenLeft was refused: " + diagnostic).c_str());
		auto const openLeftSnapshot = snapshotYaml(*loaded);
		auto const undone = loadYaml(openLeftSnapshot);
		require(undone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenLeft,
			"Undo snapshot did not restore the OpenLeft style");
		auto const redone = loadYaml(openApartSnapshot);
		require(redone->getSectorDoorOptions(0, 0, 3, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Redo snapshot did not restore the OpenApart style");
		require(redone->setSectorDoorOpenStyle(0, 0, 9, 2,
			core::Door::OpenStyle::OpenApart, &diagnostic) == false,
			"OpenApart style edit was accepted where no Door record exists");

		// A move replays the authored record at a new position; the style rides along.
		auto& moveTarget = *redone;
		moveTarget.pauseSimulation();
		uint32_t movedDoorObjectIndex{ ~0u };
		{
			auto const sector = moveTarget.getSector(doorSectorIndex);
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const candidate = sector->getObject(i);
				if (candidate && candidate->getObjectType() == core::SectorObjectType::Door
					&& static_pointer_cast<const core::DoorSectorObject>(candidate)->getDoor()
						== findDoor(moveTarget, doorSectorIndex))
					{ movedDoorObjectIndex = i; break; }
			}
		}
		require(movedDoorObjectIndex != ~0u, "OpenApart Door object could not be found");
		auto const movePlan = moveTarget.planMoveSectorObject(
			doorSectorIndex, movedDoorObjectIndex, 5, 0);
		require(movePlan.valid, "OpenApart Door move plan was rejected");
		require(moveTarget.applyObjectMove(movePlan) != nullptr, "OpenApart Door move failed");
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Moving the OpenApart Door did not preserve its opening style");

		// Clipboard copy/paste mirror: the paste side reads the copied Door's
		// authored options and re-creates through addSectorDoor; the style rides
		// through CreateDoorOptions unchanged.
		require(moveTarget.getSectorDoorOptions(0, 0, 5, 2, readBack),
			"Copied Door options could not be read");
		core::World pasteTarget("OpenApart paste", 8, 3);
		pasteTarget.addRoom("Fore room", 0, 0, 0, 7, 2);
		pasteTarget.addRoom("Back room", 1, 0, 0, 7, 2);
		auto const pasted = pasteTarget.addSectorDoor(0, 0, 1, readBack);
		pasteTarget.finishBuild();
		auto const pastedDoor = findDoor(pasteTarget, pasted.door.sector->getIndex());
		require(pastedDoor && pastedDoor->getOpenStyle() == core::Door::OpenStyle::OpenApart,
			"Pasted Door did not carry the copied OpenApart opening style");
		require(pasteTarget.getSectorDoorOptions(0, 0, 1, 2, readBack)
			&& readBack.openStyle == core::Door::OpenStyle::OpenApart,
			"Pasted Door record lost the OpenApart opening style");
	}

	void liftDoorsDefaultToOpenApartWhileOtherDoorsKeepOpenUp(smoke::Context const&)
	{
		auto doorFromResult = [](core::World::CreateObjectResult const& result)
			-> std::shared_ptr<const core::Door>
		{
			if (!result.sector || result.index == ~0u) return nullptr;
			auto const object = result.sector->getObject(result.index);
			if (!object || object->getObjectType() != core::SectorObjectType::Door) return nullptr;
			return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
		};
		// Every Door of one ownership kind, found through the public ownership
		// predicates rather than by remembering where creation left it.
		auto collectOwnedDoorStyles = [](core::World const& world, bool liftDoors)
		{
			std::vector<core::Door::OpenStyle> styles;
			std::set<core::Door const*> seen;
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u };
					bool const owned = liftDoors
						? world.isLiftOwnedDoor(object, &ownerSector, &stopIndex)
						: world.isShuttleOwnedDoor(object, &ownerSector, &stopIndex, &carriageIndex);
					if (!owned) continue;
					auto const door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
					if (!seen.insert(door.get()).second) continue;
					styles.push_back(door->getOpenStyle());
				}
			}
			return styles;
		};
		auto requireAll = [](std::vector<core::Door::OpenStyle> const& styles,
			core::Door::OpenStyle expected, size_t atLeast, char const* what)
		{
			require(styles.size() >= atLeast,
				(std::string("Too few Doors were found to check: ") + what).c_str());
			for (auto const style : styles)
				require(style == expected, (std::string(what) + " has the wrong default opening style").c_str());
		};

		// An ordinary Door with no style set keeps the OpenUp default.
		core::World ordinary("Ordinary door defaults", 12, 3);
		ordinary.addRoom("Fore", 0, 0, 0, 11, 2);
		ordinary.addRoom("Aft", 1, 0, 0, 11, 2);
		auto const ordinaryDoor = ordinary.addSectorDoor(0, 0, 3, core::World::CreateDoorOptions{});
		ordinary.finishBuild();
		require(doorFromResult(ordinaryDoor.door)
			&& doorFromResult(ordinaryDoor.door)->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"An ordinary Door no longer defaults to OpenUp");

		// Every landing Door a Lift generates is authored OpenApart.
		core::World liftWorld("Lift door defaults", 16, 3);
		auto hall = liftWorld.addRoom("Lift Hall", 0, 0, 0, 16, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 16; ++x)
				liftWorld.addSectorWalkway(hall, level, x);
		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.levelsHigh = 3;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = liftWorld.addLift(1, 0, 8, liftOptions);
		liftWorld.finishBuild();
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		for (size_t i = 0; i < lift.doors.size(); ++i)
			require(doorFromResult(lift.doors[i].door)
				&& doorFromResult(lift.doors[i].door)->getOpenStyle() == core::Door::OpenStyle::OpenApart,
				"A newly created Lift Door does not default to OpenApart");
		requireAll(collectOwnedDoorStyles(liftWorld, true), core::Door::OpenStyle::OpenApart, 3,
			"A Lift-owned Door");

		// The Lift record carries no style of its own, so replaying a legacy map -
		// whose Lift record predates opening styles entirely - still lands on the
		// generated-Door default rather than an explicit override.
		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		liftWorld.serialize(*writer, workData);
		writer->serialize();
		auto const liftYaml = writer->getSerializedString();
		require(liftYaml.find("type: lift") != std::string::npos,
			"The Lift record was not persisted");
		require(liftYaml.find("openStyle") == std::string::npos,
			"A Lift record grew its own opening style, so a legacy Lift map would no "
			"longer reconstruct through the generated-Door default");
		auto legacy = std::make_shared<core::World>("placeholder", 1, 1);
		{
			auto reader = core::YamlSerializer::fromString(liftYaml);
			reader->deserialize();
			legacy->deserialize(*reader, workData);
		}
		requireAll(collectOwnedDoorStyles(*legacy, true), core::Door::OpenStyle::OpenApart, 3,
			"A reconstructed Lift Door");

		// Shuttle-owned Doors keep the OpenUp default: only Lift Doors change.
		core::World shuttleWorld("Shuttle door defaults", 32, 3);
		shuttleWorld.addCorridor(0, 0, 31);
		shuttleWorld.addCorridor(1, 0, 31);
		core::World::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 18 }, 0 };
		shuttleOptions.capacity = 2;
		shuttleOptions.doorMask = 0b101;
		shuttleWorld.addShuttle(1, 0, 0, 27, shuttleOptions);
		shuttleWorld.finishBuild();
		requireAll(collectOwnedDoorStyles(shuttleWorld, false), core::Door::OpenStyle::OpenUp, 2,
			"A Shuttle-owned Door");
	}

	void doorStyleMapsAdvanceTheSchemaVersionAndLegacySixStillLoads(smoke::Context const&)
	{
		auto findDoor = [](core::World const& world, uint32_t sectorIndex)
			-> std::shared_ptr<const core::Door>
		{
			auto sector = world.getSector(sectorIndex);
			if (!sector) return nullptr;
			for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
			{
				auto const object = sector->getObject(i);
				if (object && object->getObjectType() == core::SectorObjectType::Door)
					return static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
			}
			return nullptr;
		};
		// Every Door of one ownership kind, found through the public ownership
		// predicates rather than by remembering where creation left it.
		auto collectOwnedDoorStyles = [](core::World const& world, bool liftDoors)
		{
			std::vector<core::Door::OpenStyle> styles;
			std::set<core::Door const*> seen;
			for (uint32_t sectorIndex = 0; sectorIndex < world.getNumSectors(); ++sectorIndex)
			{
				auto const sector = world.getSector(sectorIndex);
				if (!sector) continue;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
				{
					auto const object = sector->getObject(i);
					if (!object || object->getObjectType() != core::SectorObjectType::Door) continue;
					uint32_t ownerSector{ ~0u }, stopIndex{ ~0u }, carriageIndex{ ~0u };
					bool const owned = liftDoors
						? world.isLiftOwnedDoor(object, &ownerSector, &stopIndex)
						: world.isShuttleOwnedDoor(object, &ownerSector, &stopIndex, &carriageIndex);
					if (!owned) continue;
					auto const door = static_pointer_cast<const core::DoorSectorObject>(object)->getDoor();
					if (!seen.insert(door.get()).second) continue;
					styles.push_back(door->getOpenStyle());
				}
			}
			return styles;
		};
		auto requireAllStyles = [](std::vector<core::Door::OpenStyle> const& styles,
			core::Door::OpenStyle expected, size_t atLeast, char const* what)
		{
			require(styles.size() >= atLeast,
				(std::string("Too few Doors were found to check: ") + what).c_str());
			for (auto const style : styles)
				require(style == expected,
					(std::string(what) + " has the wrong opening style").c_str());
		};
		auto serialize = [](core::World const& world)
		{
			core::SerializationWorkData workData;
			auto writer = core::YamlSerializer::toString();
			world.serialize(*writer, workData);
			writer->serialize();
			return writer->getSerializedString();
		};

		// A stand-in for a pre-Door-style (version 6) build's reader: the version
		// ceiling that build accepted, and its construction-record name table. A
		// style-bearing map has to be refused at the version check rather than
		// have its Door style fields silently ignored.
		uint32_t const preDoorStyleVersionCeiling{ 6 };
		static std::set<std::string> const preDoorStyleRecordNames{
			"corridor", "room", "ladder", "stairwell", "staircase", "lift", "shuttle", "door",
			"window", "bulkheadDoor", "lightSwitch", "forceBridge", "sectorLadder", "platformLift",
			"walkway", "marker", "removeWall", "removeMarker", "objectTombstone", "background",
			"facade" };
		auto preDoorStyleReaderReads = [&](std::string const& yaml)
		{
			auto reader = core::YamlSerializer::fromString(yaml);
			reader->deserialize();
			reader->beginMap("world");
			auto const version = reader->readUint32("version");
			if (version > preDoorStyleVersionCeiling)
				throw core::SerializationException("Unsupported World serialization version");
			reader->beginArray("construction");
			while (reader->nextArrayItem())
			{
				reader->beginMap("");
				auto const type = reader->readString("type");
				if (preDoorStyleRecordNames.find(type) == preDoorStyleRecordNames.end())
					throw core::SerializationException(
						"Unknown World construction record type: " + type);
				reader->endMap();
			}
			reader->endArray();
			reader->endMap();
		};

		// One map exercising every ownership kind and every style field: an
		// ordinary OpenLeft Door, a Lift with a per-stop override, and a Shuttle
		// with a per-Door override.
		core::World authored("Door-style schema", 40, 3);
		authored.addRoom("Fore hall", 0, 0, 0, 8, 3);
		authored.addRoom("Aft room", 1, 0, 0, 8, 3);
		core::World::CreateDoorOptions doorOptions;
		doorOptions.width = 2;
		doorOptions.openStyle = core::Door::OpenStyle::OpenLeft;
		auto const ordinaryDoor = authored.addSectorDoor(0, 0, 3, doorOptions);
		auto const liftHall = authored.addRoom("Lift hall", 0, 0, 10, 8, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 8; ++x)
				authored.addSectorWalkway(liftHall, level, x);
		core::World::CreateLiftOptions liftOptions;
		liftOptions.cellsWide = 1;
		liftOptions.levelsHigh = 3;
		liftOptions.stopOffsets = { 0, 1, 2 };
		auto const lift = authored.addLift(1, 0, 13, liftOptions);
		authored.addCorridor(0, 0, 20, 20, 1);
		core::World::CreateShuttleOptions shuttleOptions{ 2, 3, { 0, 12 }, 0 };
		shuttleOptions.doorOpenStyles = { static_cast<uint32_t>(core::Door::OpenStyle::OpenApart),
			~0u, ~0u, ~0u };
		auto const shuttle = authored.addShuttle(1, 0, 20, 20, shuttleOptions);
		authored.finishBuild();
		require(lift.doors.size() == 3, "The Lift did not generate one Door per stop");
		require(shuttle.doors.size() == 4, "The Shuttle did not generate four landing Doors");
		authored.pauseSimulation();
		std::string diagnostic;
		require(authored.setLiftStopDoorOpenStyle(lift.lift.sector->getIndex(), 1,
				core::Door::OpenStyle::OpenRight, &diagnostic),
			("A Lift stop style override was refused: " + diagnostic).c_str());

		auto const yaml = serialize(authored);
		require(yaml.find("version: 39") != std::string::npos,
			"A map with authored Door styles was not written at the current schema version");
		require(yaml.find("version: 6") == std::string::npos,
			"A map with authored Door styles still carries version 6");
		require(yaml.find("openStyle: openLeft") != std::string::npos
			&& yaml.find("stopDoorOpenStyles") != std::string::npos
			&& yaml.find("doorOpenStyles") != std::string::npos,
			"The Door-style fields were not persisted alongside the new version");

		// A version-6 reader refuses the file at the version check: the refusal
		// is about the schema version, not about record shapes it would have
		// accepted.
		bool refusedVersion{ false };
		try
		{
			preDoorStyleReaderReads(yaml);
		}
		catch (core::SerializationException const& error)
		{
			refusedVersion = true;
			require(std::string(error.what()).find("version") != std::string::npos,
				("A version-6 reader failed for a reason other than the version: "
					+ std::string(error.what())).c_str());
		}
		require(refusedVersion, "A version-6 reader accepted a version-7 Door-style map");

		// A defaults-only map replays exactly like a pre-feature version-6 file:
		// every style field absent.  Reconstruct that legacy shape by rewriting
		// the version and dropping the only style line the writer emitted.
		core::World defaults("Legacy-shaped map", 40, 3);
		defaults.addRoom("Fore hall", 0, 0, 0, 8, 3);
		defaults.addRoom("Aft room", 1, 0, 0, 8, 3);
		auto const defaultDoor = defaults.addSectorDoor(0, 0, 3, core::World::CreateDoorOptions{});
		auto const defaultHall = defaults.addRoom("Lift hall", 0, 0, 10, 8, 3);
		for (uint32_t level = 1; level < 3; ++level)
			for (uint32_t x = 0; x < 8; ++x)
				defaults.addSectorWalkway(defaultHall, level, x);
		core::World::CreateLiftOptions defaultLiftOptions;
		defaultLiftOptions.cellsWide = 1;
		defaultLiftOptions.levelsHigh = 3;
		defaultLiftOptions.stopOffsets = { 0, 1, 2 };
		auto const defaultLift = defaults.addLift(1, 0, 13, defaultLiftOptions);
		defaults.addCorridor(0, 0, 20, 20, 1);
		core::World::CreateShuttleOptions defaultShuttleOptions{ 2, 3, { 0, 12 }, 0 };
		auto const defaultShuttle = defaults.addShuttle(1, 0, 20, 20, defaultShuttleOptions);
		defaults.finishBuild();

		auto defaultsYaml = serialize(defaults);
		require(defaultsYaml.find("stopDoorOpenStyles") == std::string::npos
			&& defaultsYaml.find("doorOpenStyles") == std::string::npos,
			"A defaults-only map persisted transport style overrides");
		std::string const styleLine = "    openStyle: openUp\n";
		require(defaultsYaml.find(styleLine) != std::string::npos,
			"The defaults-only map did not persist the ordinary Door's openStyle line");
		auto const legacyYaml = std::string("version: 6")
			+ defaultsYaml.substr(defaultsYaml.find("\n"));
		auto const strippedYaml = legacyYaml.substr(0, legacyYaml.find(styleLine))
			+ legacyYaml.substr(legacyYaml.find(styleLine) + styleLine.size());
		require(strippedYaml.find("openStyle") == std::string::npos,
			"The reconstructed legacy map still carried a Door style field");

		// The version-6 reader is content with the legacy shape: its refusal of
		// the new map is the version alone.
		preDoorStyleReaderReads(strippedYaml);

		// The current reader loads version 6 and supplies the owner-sensitive
		// defaults: OpenUp for the ordinary and Shuttle-owned Doors, OpenApart
		// for the Lift-owned Doors.
		auto legacy = std::make_shared<core::World>("placeholder", 1, 1);
		{
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(strippedYaml);
			reader->deserialize();
			legacy->deserialize(*reader, workData);
		}
		auto const legacyOrdinary = findDoor(*legacy, defaultDoor.door.sector->getIndex());
		require(legacyOrdinary
			&& legacyOrdinary->getOpenStyle() == core::Door::OpenStyle::OpenUp,
			"A legacy ordinary Door did not replay as OpenUp");
		requireAllStyles(collectOwnedDoorStyles(*legacy, true), core::Door::OpenStyle::OpenApart, 3,
			"A legacy Lift-owned Door");
		requireAllStyles(collectOwnedDoorStyles(*legacy, false), core::Door::OpenStyle::OpenUp, 4,
			"A legacy Shuttle-owned Door");

		// The current reader still refuses anything above its own ceiling.
		auto const futureYaml = std::string("version: 40")
			+ defaultsYaml.substr(defaultsYaml.find("\n"));
		bool refusedFuture{ false };
		try
		{
			core::SerializationWorkData workData;
			auto reader = core::YamlSerializer::fromString(futureYaml);
			reader->deserialize();
			core::World rejected("placeholder", 1, 1);
			rejected.deserialize(*reader, workData);
		}
		catch (core::SerializationException const&)
		{
			refusedFuture = true;
		}
		require(refusedFuture, "A future map was accepted by the current reader");
	}
}
