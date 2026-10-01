#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include <memory>
#include <stdexcept>
#include <string>

#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/SerializationException.h"
#include "core/Agent.h"
#include "core/DoorSectorObject.h"
#include "core/WalkwaySectorObject.h"
#include "core/LiftSectorObject.h"
#include "core/BulkheadDoorSectorObject.h"
#include "core/Exceptions.h"
#include "core/Graph.h"
#include "core/Edge.h"
#include "core/Path.h"
#include "core/Vertex.h"
#include "imgui/imgui.h"
#include "imgui_internal.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"

namespace
{
	void require(bool condition, std::string const& message)
	{ if (!condition) throw std::runtime_error(message); }

	std::string save(core::World& world)
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		world.serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> load(std::string const& yaml)
	{
		auto world = std::make_shared<core::World>("target", 1, 1);
		core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(yaml); reader->deserialize();
		require(world->deserialize(*reader, work), "permission World did not load");
		return world;
	}

	core::World::CreateLiftResult permissionLift(core::World& world, bool platform,
		uint32_t room, uint32_t x, core::World::CreateLiftOptions options)
	{
		if (!platform) return world.addLift(1, 0, x, options);
		options.cellsWide = 1;
		options.platformStopDurationSeconds = 20.0f;
		auto created = world.addSectorPlatformLift(room, 0, x, options);
		return { created.lift, {}, created.traversalResource, created.interiorSelector };
	}

	void liftDestinationAuthoring(bool platform = false, bool shuttle = false)
	{
		auto world = std::make_shared<core::World>("destination authoring", shuttle ? 64 : 10, 3);
		auto unrelated = world->addRoom("Unrelated", 1, 0, 0, 2, 1);
		uint32_t room = 0;
		uint32_t shuttleMiddle = ~0u;
		if (platform)
		{
			room = world->addRoom("Platform room", 0, 0, 0, 10, 3);
			for (uint32_t level = 1; level < 3; ++level)
				for (uint32_t x = 0; x < 10; ++x) world->addSectorWalkway(room, level, x);
		}
		else if (shuttle)
		{
			world->addCorridor(0, 0, 16);
			shuttleMiddle = world->addCorridor(0, 24, 16);
			world->addCorridor(0, 48, 16);
		}
		else for (uint32_t level = 0; level < 3; ++level) world->addCorridor(level, 0, 10);
		core::World::CreateLiftOptions options;
		options.cellsWide = platform ? 1 : 2; options.stopOffsets = { 0, 1, 2 };
		core::World::CreateLiftResult lift;
		if (shuttle)
		{
			auto created = world->addShuttle(1, 0, 4, 60, { 2, 4, { 0, 24, 48 }, 0 });
			lift = { created.shuttle, created.doors, created.traversalResource, created.interiorSelector };
		}
		else lift = permissionLift(*world, platform, room, 8, options);
		auto sector = lift.lift.sector->getIndex();
		auto object = platform ? lift.lift.index : ~0u;
		world->finishBuild(); world->pauseSimulation();
		auto red = world->addAccessPermission("Red key");
		auto blue = world->addAccessPermission("Blue key");
		require(world->getLiftDestinationLevels(sector, object) == (shuttle ? std::vector<uint32_t>{ 4, 28, 52 } : std::vector<uint32_t>{ 0, 1, 2 }), "Destination Levels missing");
		for (uint32_t stop = 0; stop < 3; ++stop)
			require(world->getLiftDestinationPermissionRequirement(sector, stop, object).empty(), "New Stop is restricted");
		std::string diagnostic;
		auto unrestricted = save(*world);
		require(world->setLiftDestinationPermissionRequirement(sector, 0, {}, &diagnostic, object)
			&& save(*world) == unrestricted, "Empty no-op changed authored data");
		gWorldDocumentHistory.clear();
		require(commitLiftDestinationPermissionRequirement(world, sector, 2, red, true, diagnostic, object), diagnostic);
		require(commitLiftDestinationPermissionRequirement(world, sector, 2, blue, true, diagnostic, object), diagnostic);
		require(gWorldDocumentHistory.undoCount() == 2, "Destination edits bypass document history");
		auto restore = [&](DocumentSnapshot const& snapshot) { world = load(snapshot.yaml); world->pauseSimulation(); return true; };
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination undo failed");
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == std::vector<core::AccessPermissionId>{ red }, "Undo lost destination requirement");
		require(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination redo failed");
		auto expected = std::vector<core::AccessPermissionId>{ red, blue };
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Redo lost destination requirement");
		require(commitClearLiftDestinationPermissionRequirement(world, sector, 2, diagnostic, object), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object).empty(), "Clear retained destination permissions");
		require(gWorldDocumentHistory.undoCount() == 3, "Destination clear bypassed document history");
		require(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(save(*world)), restore), "Destination clear undo failed");
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination clear undo lost requirements");
		require(world->renameAccessPermission(red, "Renamed key", &diagnostic), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Rename changed identity");
		require(world->getAccessPermissionUsage(red).liftDestinationRequirements == 1, "Usage omitted destination");
		auto before = save(*world);
		require(!world->setLiftDestinationPermissionRequirement(sector, 2, { red, red }, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(sector, 2, { core::AccessPermissionId{ 256 } }, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(sector, 3, {}, &diagnostic, object)
			&& !world->setLiftDestinationPermissionRequirement(0, 0, {}, &diagnostic)
			&& save(*world) == before, "Invalid destination edit was not transactional");
		require(world->resumeSimulation(), "Destination fixture did not resume");
		require(!world->setLiftDestinationPermissionRequirement(sector, 2, {}, &diagnostic, object), "Running destination edit accepted");
		world->pauseSimulation();

		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.DisplaySize = ImVec2(1000, 700);
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		ImGui::NewFrame(); ImGui::SetNextWindowSize(ImVec2(950, 650)); ImGui::Begin("Lift Selection");
		ImGui::LogToBuffer();
		renderLiftDestinationPermissions(world, sector, object);
		std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
		require(text.find("Destination permissions") != std::string::npos
			&& text.find("Accepted shared journeys") != std::string::npos
			&& text.find("Intermediate feature") == std::string::npos
			&& text.find("Dynamic authorization is not yet complete") == std::string::npos
			&& text.find("NOT YET ENFORCED") == std::string::npos
			&& text.find("None") != std::string::npos
			&& text.find("Add / remove permissions") != std::string::npos
			&& text.find("Clear") != std::string::npos
			&& text.find("Renamed key") != std::string::npos
			&& text.find("Blue key") != std::string::npos
			&& (!shuttle || (text.find("Stop 0: x 4") != std::string::npos
				&& text.find("Stop 1: x 28") != std::string::npos
				&& text.find("Stop 2: x 52") != std::string::npos)), "Selection omitted requirements or destination positions");
		ImGui::LogFinish(); ImGui::End(); ImGui::Render();

		auto yaml = save(*world);
		auto restored = load(yaml);
		require(restored->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination YAML round trip failed");
		core::SerializationWorkData binaryWork;
		auto binaryWriter = core::BinarySerializer::toString();
		world->serialize(*binaryWriter, binaryWork); binaryWriter->serialize();
		auto binaryReader = core::BinarySerializer::fromString(binaryWriter->getSerializedString());
		binaryReader->deserialize();
		core::World binaryWorld("binary target", 1, 1);
		require(binaryWorld.deserialize(*binaryReader, binaryWork)
			&& binaryWorld.getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Destination binary round trip failed");
		// Old Worlds have no destination field and remain unrestricted.
		auto legacy = YAML::Load(yaml);
		legacy["version"] = 29;
		for (auto record : legacy["construction"]) record.remove("destinationPermissionRequirements");
		require(load(YAML::Dump(legacy))->getLiftDestinationPermissionRequirement(sector, 2, object).empty(), "Older World is restricted");

		for (auto replacement : {
			"[{permissions: []}, {permissions: []}, {permissions: [999]}]",
			"[{permissions: []}, {permissions: []}, {permissions: [0]}]",
			"[{permissions: []}, {permissions: []}, {permissions: [1, 1]}]",
			"[{permissions: []}, {permissions: []}, {permissions: broken}]",
			"[{permissions: [1]}]", "broken", "{}" })
		{
			auto malformed = YAML::Load(yaml);
			for (auto record : malformed["construction"])
				if (record["destinationPermissionRequirements"])
					record["destinationPermissionRequirements"] = YAML::Load(replacement);
			auto unchanged = save(*restored);
			bool refused = false;
			try
			{
				core::SerializationWorkData work;
				auto reader = core::YamlSerializer::fromString(YAML::Dump(malformed)); reader->deserialize();
				restored->deserialize(*reader, work);
			}
			catch (core::SerializationException const&) { refused = true; }
			require(refused && save(*restored) == unchanged, "Malformed destination mutated target World");
		}

		if (shuttle)
		{
			auto apply = [&](core::World::ShuttleEditPlan const& plan)
			{
				require(plan.valid, plan.diagnostic);
				sector = world->applyShuttleEdit(plan);
			};
			auto locationPlan = world->planRemoveLocation(unrelated);
			require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan); --sector;
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle reindex lost requirement");
			apply(world->planEditShuttleVehicle(sector, 2, 4, 1u << 2));
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle Door edit lost requirement");
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
			apply(world->planRemoveShuttleStop(sector, 1));
			require(world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Shuttle retained Stop lost requirement");
			apply(world->planAddShuttleStop(sector, 24));
			require(world->getLiftDestinationPermissionRequirement(sector, 1).empty()
				&& world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle recreated Stop inherited requirement");
			require(load(save(*world))->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Shuttle edit round trip failed");
			// Removing the supporting Location also removes its Stop, without
			// shifting another destination's permissions onto a different Stop.
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
			auto removePlatform = world->planRemoveLocation(shuttleMiddle - 1);
			require(removePlatform.valid, removePlatform.diagnostic);
			world->applyLocationEdit(removePlatform);
			for (uint32_t i = 0; i < world->getNumSectors(); ++i)
				if (world->getSector(i)->getType() == core::SectorType::Shuttle) sector = i;
			require(world->getLiftDestinationLevels(sector).size() == 2, "Supporting Location removal did not retain two Shuttle Stops");
			require(world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Platform removal lost retained Shuttle destination");
			world->addCorridor(0, 24, 16);
			apply(world->planAddShuttleStop(sector, 24));
			require(world->getLiftDestinationLevels(sector).size() == 3, "Recreated platform did not restore three Shuttle Stops");
			require(world->getLiftDestinationPermissionRequirement(sector, 1).empty(), "Recreated Shuttle platform inherited requirement");
			require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
			require(world->getLiftDestinationPermissionRequirement(sector, 2) == std::vector<core::AccessPermissionId>{ blue }, "Shuttle permission cleanup failed");
			apply(world->planRemoveShuttle(sector));
			require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Removed Shuttle counted in usage");
			auto recreated = world->addShuttle(1, 0, 4, 60, { 2, 4, { 0, 24, 48 }, 0 });
			require(world->getLiftDestinationPermissionRequirement(recreated.shuttle.sector->getIndex(), 2).empty(), "Recreated Shuttle inherited requirement");
			gWorldDocumentHistory.clear();
			return;
		}

		if (platform)
		{
			auto locatePlatform = [&]
			{
				for (uint32_t i = 0; i < world->getSector(sector)->getNumObjects(); ++i)
					if (std::dynamic_pointer_cast<const core::LiftSectorObject>(world->getSector(sector)->getObject(i))) return i;
				throw std::runtime_error("Platform object missing");
			};
			// Ground is a destination too, independently of the landing call.
			require(world->setLiftDestinationPermissionRequirement(sector, 0, { blue }, &diagnostic, object), diagnostic);
			require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic, object), diagnostic);
			auto locationPlan = world->planRemoveLocation(unrelated);
			require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan); --sector;
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Platform sector reindex lost requirement");
			core::World::CreateLiftOptions settings;
			require(world->getPlatformLiftOptions(sector, object, settings), "Platform options missing");
			settings.capacity = 1; settings.platformStopDurationSeconds = 4.0f;
			auto edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Platform settings lost requirement");
			settings.stopOffsets = { 0, 2 };
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object) == expected, "Platform retained Stop lost requirement");
			settings.stopOffsets = { 0, 1, 2 };
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object).empty(), "Recreated Platform Stop inherited requirement");
			// Removing a supporting Walkway removes only that optional Stop.
			uint32_t walkway = ~0u;
			for (uint32_t i = 0; i < world->getSector(sector)->getNumObjects(); ++i)
			{
				auto candidate = world->getSector(sector)->getObject(i);
				if (std::dynamic_pointer_cast<const core::WalkwaySectorObject>(candidate)
					&& candidate->getCellX() == 8 && candidate->getCellY() == 1) walkway = i;
			}
			require(walkway != ~0u, "Supporting Walkway missing");
			require(world->removeSectorWalkway(sector, walkway), "Walkway removal failed");
			require(world->getLiftDestinationLevels(sector, object) == std::vector<uint32_t>{ 0, 2 }
				&& world->getLiftDestinationPermissionRequirement(sector, 1, object) == expected, "Walkway deletion lost retained destination");
			world->addSectorWalkway(sector, 1, 8);
			object = locatePlatform();
			edit = world->planPlatformLiftEdit(sector, object, settings);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getLiftDestinationPermissionRequirement(sector, 1, object).empty()
				&& world->getLiftDestinationPermissionRequirement(sector, 0, object) == std::vector<core::AccessPermissionId>{ blue },
				"Walkway recreation inherited requirement or lost mandatory ground requirement");
			require(load(save(*world))->getLiftDestinationPermissionRequirement(sector, 2, object) == expected, "Structural edit persistence failed");
			require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
			require(world->getLiftDestinationPermissionRequirement(sector, 2, object) == std::vector<core::AccessPermissionId>{ blue }, "Platform deletion cleanup failed");
			edit = world->planRemovePlatformLift(sector, object);
			require(edit.valid, edit.diagnostic); world->applyPlatformLiftEdit(edit);
			require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Deleted Platform retained usage");
			auto recreated = world->addSectorPlatformLift(sector, 0, 8, options);
			world->finishBuild(); world->pauseSimulation();
			for (uint32_t stop = 0; stop < 3; ++stop)
				require(world->getLiftDestinationPermissionRequirement(sector, stop, recreated.lift.index).empty(), "Recreated Platform is restricted");
			gWorldDocumentHistory.clear();
			return;
		}

		// Deleting a landing Location also removes its Stop and remaps retained requirements.
		restored->pauseSimulation();
		require(restored->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
		auto landingPlan = restored->planRemoveLocation(2);
		require(landingPlan.valid, landingPlan.diagnostic); restored->applyLocationEdit(landingPlan);
		require(restored->getLiftDestinationLevels(sector - 1) == std::vector<uint32_t>{ 0, 2 }
			&& restored->getLiftDestinationPermissionRequirement(sector - 1, 1) == expected
			&& restored->getAccessPermissionUsage(red).liftDestinationRequirements == 1,
			"Landing deletion lost retained requirements or retained a deleted Stop requirement");
		require(load(save(*restored))->getLiftDestinationPermissionRequirement(sector - 1, 1) == expected,
			"Landing deletion produced malformed destination data");

		// Unrelated deletion reindexes Sectors without changing destination identity.
		auto locationPlan = world->planRemoveLocation(unrelated);
		require(locationPlan.valid, locationPlan.diagnostic); world->applyLocationEdit(locationPlan);
		--sector;
		require(world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Sector reindexing lost requirements");
		// Removing a restricted earlier Stop reindexes the retained destination; recreation is unrestricted.
		require(world->setLiftDestinationPermissionRequirement(sector, 1, { red }, &diagnostic), diagnostic);
		auto plan = world->planRemoveLiftStop(sector, 1);
		require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getLiftDestinationLevels(sector) == std::vector<uint32_t>{ 0, 2 }
			&& world->getLiftDestinationPermissionRequirement(sector, 1) == expected, "Retained Stop lost requirement after reindexing");
		plan = world->planResizeLift(sector, 8, 0, 2, 3);
		require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getLiftDestinationLevels(sector) == std::vector<uint32_t>{ 0, 1, 2 }
			&& world->getLiftDestinationPermissionRequirement(sector, 1).empty()
			&& world->getLiftDestinationPermissionRequirement(sector, 2) == expected, "Recreated Stop inherited a requirement");
		require(world->deleteAccessPermission(red, &diagnostic), diagnostic);
		require(world->getLiftDestinationPermissionRequirement(sector, 2) == std::vector<core::AccessPermissionId>{ blue }, "Deletion left destination reference");
		plan = world->planRemoveLift(sector); require(plan.valid, plan.diagnostic); world->applyLiftEdit(plan);
		require(world->getAccessPermissionUsage(blue).liftDestinationRequirements == 0, "Deleted transport retained usage");
		lift = world->addLift(1, 0, 8, options); world->finishBuild(); world->pauseSimulation();
		require(world->getLiftDestinationPermissionRequirement(lift.lift.sector->getIndex(), 2).empty(), "Recreated Lift inherited requirements");
		gWorldDocumentHistory.clear();
	}
}

void permission_smoke::registerDestinationEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "permissions/destinationAuthoringLift",
		[](smoke::Context const&)
		{
			EditorState state;
			liftDestinationAuthoring(false, false);
		} });
	checks.push_back({ "permissions/destinationAuthoringPlatform",
		[](smoke::Context const&)
		{
			EditorState state;
			liftDestinationAuthoring(true, false);
		} });
	checks.push_back({ "permissions/destinationAuthoringShuttle",
		[](smoke::Context const&)
		{
			EditorState state;
			liftDestinationAuthoring(false, true);
		} });
}
