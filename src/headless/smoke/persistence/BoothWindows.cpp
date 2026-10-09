#include "WorldChecks.h"
#include "core/World.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <yaml-cpp/yaml.h>
#include "../support/DumbwaiterFixture.h"

namespace persistence
{
	void dumbwaiters(smoke::Context const&)
	{
		using smoke::require;
		auto world = dumbwaiter_fixture::make();
		auto id = world->addDumbwaiter(1, 0, 2, {1, 0.1f}); world->finishBuild(); world->pauseSimulation();
		auto a = world->addAccessPermission("Lower A"), b = world->addAccessPermission("Lower B");
		auto upper = world->addAccessPermission("Upper");
		auto device = world->lookupDumbwaiter(id);
		require(world->setInteractionPointPermissionRequirement(device->getLandingButton(0), {a,b})
			&& world->setInteractionPointPermissionRequirement(device->getLandingButton(1), {upper}), "Landing requirements fixture failed");
		auto write = [](core::World const& source, bool binary)
		{
			auto serialize = [&](auto writer) {
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				source.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
			};
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		for (unsigned edit = 0; edit < 7; ++edit)
		{
			auto source = dumbwaiter_fixture::make(0, true, 2); source->addLayer();
			auto deviceId = source->addDumbwaiter(2, 0, 2, {1, 0.37f}); source->finishBuild();
			auto lowerPermission = source->addAccessPermission("Lower"), upperPermission = source->addAccessPermission("Upper");
			auto unit = source->lookupDumbwaiter(deviceId);
			source->setInteractionPointPermissionRequirement(unit->getLandingButton(0), {lowerPermission});
			source->setInteractionPointPermissionRequirement(unit->getLandingButton(1), {upperPermission});
			source->pressDumbwaiterLanding(deviceId, 0); source->resumeSimulation();
			require(source->advanceTicks(60), "Edited document cycle fixture failed"); source->pauseSimulation();
			if (edit == 0) source->applyWalkwayEdit(source->planRemoveSectorWalkway(0, 0));
			if (edit == 1) source->applyLocationEdit(source->planRemoveLocation(0));
			if (edit == 2) source->applyLocationEdit(source->planResizeLocation(0, 2, 0, 2, 2));
			if (edit == 3) source->applyDeleteLayer(source->planDeleteLayer(0));
			if (edit == 4) source->applyDeleteLayer(source->planDeleteLayer(2));
			if (edit == 5) source->applyDeleteLevel(source->planDeleteLevel(0));
			if (edit == 6) source->applyDeleteLevel(source->planDeleteLevel(3));
			bool survives = edit == 2 || edit == 3 || edit == 6;
			for (bool binary : {false, true})
			{
				auto data = write(*source, binary);
				std::unique_ptr<core::Serializer> input = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
				input->deserialize(); core::SerializationWorkData work; core::World loaded("Edited", 1, 1);
				require(loaded.deserialize(*input, work), "Edited document refused"); loaded.pauseSimulation();
				unit = loaded.lookupDumbwaiter(deviceId);
				require(bool(unit) == survives && loaded.getSimulationSnapshot().interactionPoints.size() == (survives ? 2u : 0u),
					"Edited document persisted orphan children or control relationships");
				if (!unit) continue;
				require(unit->getInitialStop() == 1 && unit->getTravelSeconds() == 0.37f && !unit->isBusy()
					&& unit->getAperture(1)->getProgress() == 1 && unit->getAperture(0)->getProgress() == 0
					&& loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(0)) == std::vector<core::AccessPermissionId>{lowerPermission}
					&& loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(1)) == std::vector<core::AccessPermissionId>{upperPermission},
					"Edited roundtrip lost coherent authored configuration");
				auto actor = loaded.createAgent("Restored operator", unit->getStop(1).sector->getIndex(),
					float(unit->getCellY() + 1 - unit->getStop(1).sector->getCellY()), 0.0f);
				loaded.grantAgentAccessPermission(actor, upperPermission);
				require(bool(loaded.requestDumbwaiterLanding(deviceId, 1, actor)), "Restored Agent landing refused");
				loaded.resumeSimulation(); require(loaded.advanceTicks(130), "Restored Agent operation failed");
				require(!unit->isBusy() && unit->getCarPosition().y == 0, "Restored Agent control not operable");
				auto operation = loaded.pressDumbwaiterLanding(deviceId, 0);
				require(loaded.advanceTicks(119) && loaded.lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Succeeded,
					"Restored user control not operable");
			}
		}
		for (uint32_t initial : {0u, 1u}) for (unsigned ticks : {0u, 12u, 80u, 180u})
		{
			auto running = dumbwaiter_fixture::make();
			auto device = running->addDumbwaiter(1, 0, 2, {initial, 2}); running->finishBuild();
			auto idleYaml = write(*running, false), idleBinary = write(*running, true);
			running->resumeSimulation(); running->pressDumbwaiterLanding(device, initial);
			require(running->advanceTicks(ticks), "Mid-cycle save setup failed");
			for (bool binary : {false, true})
			{
				auto data = write(*running, binary);
				require(data == (binary ? idleBinary : idleYaml), "Mid-cycle document contains runtime motion or requests");
				std::unique_ptr<core::Serializer> reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
				reader->deserialize(); core::SerializationWorkData work; core::World restored("Restored", 1, 1);
				require(restored.deserialize(*reader, work), "Mid-cycle document refused");
				auto unit = restored.lookupDumbwaiter(device);
				require(unit && !unit->isBusy() && unit->getCarPosition().y == float(initial)
					&& unit->getAperture(initial)->getProgress() == 1 && unit->getAperture(1 - initial)->getProgress() == 0
					&& restored.getSimulationSnapshot().deviceOperations.empty(), "Load restored in-flight state instead of authored initial state");
				auto operation = restored.pressDumbwaiterLanding(device, 1 - initial);
				require(restored.advanceTicks(216) && restored.lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Succeeded,
					"Mid-cycle document did not restore an operable unit");
			}
		}
		{
			auto legacyWorld = dumbwaiter_fixture::make();
			auto legacyId = legacyWorld->addDumbwaiter(1, 0, 2);
			auto marker = legacyWorld->addSectorMarker(0, 0, 0.5f);
			require(legacyWorld->removeSectorMarker(0, marker.index), "Legacy layout removal fixture failed");
			auto legacyNode = YAML::Load(write(*legacyWorld, false));
			legacyNode.remove("dumbwaiterPhysicalButtons");
			for (auto record : legacyNode["construction"])
				if (record["type"].as<std::string>() == "removeMarker")
					record["objectIndex"] = record["objectIndex"].as<unsigned>() - 2;
			auto reader = core::YamlSerializer::fromString(YAML::Dump(legacyNode)); reader->deserialize();
			core::World migrated("Old landing controls", 1, 1); core::SerializationWorkData data;
			require(migrated.deserialize(*reader, data) && migrated.lookupDumbwaiter(legacyId),
				"Legacy landing layout did not remap later Marker removals by identity");
			require(YAML::Load(write(migrated, false))["dumbwaiterPhysicalButtons"].as<bool>(),
				"Migrated landing layout was not recorded on save");
		}
		auto baseline = write(*world, false);
		for (bool binary : {false, true})
		{
			auto data = write(*world, binary);
			std::unique_ptr<core::Serializer> input = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			input->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*input, work), "Dumbwaiter document refused");
			auto unit = loaded.lookupDumbwaiter(id);
			require(unit && unit->getInitialStop() == 1 && unit->getTravelSeconds() == 0.1f
				&& unit->getCellX() == 2 && unit->getCellY() == 0 && unit->getLayerIndex() == 1
				&& unit->getAperture(0)->getProgress() == 0 && unit->getAperture(1)->getProgress() == 1,
				"Round trip lost placement/configuration/initial shutters");
			require(loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(0)) == std::vector<core::AccessPermissionId>{a,b}
				&& loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(1)) == std::vector<core::AccessPermissionId>{upper},
				"YAML/binary lost independent landing requirements");
			loaded.resetSimulation(); loaded.pauseSimulation();
			unit = loaded.lookupDumbwaiter(id);
			require(unit->getAperture(1)->getProgress() == 1
				&& loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(0)) == std::vector<core::AccessPermissionId>{a,b}, "Reset lost authored state/requirements");
			require(loaded.deleteAccessPermission(a), "Permission deletion failed");
			loaded.resetSimulation(); loaded.pauseSimulation(); unit = loaded.lookupDumbwaiter(id);
			require(loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(0)) == std::vector<core::AccessPermissionId>{b}
				&& loaded.getInteractionPointPermissionRequirement(unit->getLandingButton(1)) == std::vector<core::AccessPermissionId>{upper},
				"Permission deletion not retained by construction replay");
			require(loaded.removeDumbwaiter(id), "Loaded deletion failed");
			auto next = loaded.addDumbwaiter(1, 0, 2); require(next.value > id.value, "Loaded identity reused");
		}
		auto node = YAML::Load(baseline);
		// Master schema 49 predates the merged Furniture identity field.
		auto masterLegacy = YAML::Clone(node); masterLegacy["version"] = 49;
		masterLegacy.remove("nextFurnitureId");
		auto masterReader = core::YamlSerializer::fromString(YAML::Dump(masterLegacy)); masterReader->deserialize();
		core::SerializationWorkData masterData; core::World masterWorld("Schema 49", 1, 1);
		require(masterWorld.deserialize(*masterReader, masterData) && masterWorld.lookupDumbwaiter(id)
			&& masterWorld.getInteractionPointPermissionRequirement(masterWorld.lookupDumbwaiter(id)->getLandingButton(0))
				== std::vector<core::AccessPermissionId>{a,b}, "Pre-merge master schema lost Dumbwaiter state");
		for (auto change : {"zeroId", "duplicate", "stop", "nan", "timingLow", "timingHigh", "bounds", "layer", "width", "stops", "legacy", "nextId", "support", "unknownPermission", "zeroPermission", "duplicatePermission", "malformedPermission", "legacyPermission"})
		{
			auto invalid = YAML::Clone(node); std::string c = change;
			for (auto record : invalid["construction"])
			{
				if (c == "support" && record["type"].as<std::string>() == "walkway") record["xOffset"] = 1;
				if (record["type"].as<std::string>() != "dumbwaiter") continue;
				if (c == "zeroId") record["id"] = 0;
				if (c == "stop") record["initialStop"] = 2;
				if (c == "nan") record["travelSeconds"] = ".nan";
				if (c == "timingLow") record["travelSeconds"] = 0.09;
				if (c == "timingHigh") record["travelSeconds"] = 60.01;
				if (c == "bounds") record["y"] = 3;
				if (c == "layer") record["layer"] = 0;
				if (c == "width") record["cellsWide"] = 2;
				if (c == "stops") record["stopOffsets"] = std::vector<unsigned>{0, 1, 2};
				if (c == "unknownPermission") record["lowerLandingPermissionRequirement"] = std::vector<unsigned>{255};
				if (c == "zeroPermission") record["upperLandingPermissionRequirement"] = std::vector<unsigned>{0};
				if (c == "duplicatePermission") record["lowerLandingPermissionRequirement"] = std::vector<unsigned>{unsigned(a.value),unsigned(a.value)};
				if (c == "malformedPermission") record["upperLandingPermissionRequirement"] = "not an array";
			}
			if (c == "duplicate") invalid["construction"].push_back(YAML::Clone(invalid["construction"][2]));
			if (c == "legacy") invalid["version"] = 46;
			if (c == "legacyPermission") invalid["version"] = 47;
			if (c == "nextId") invalid["nextDumbwaiterId"] = id.value;
			bool refused = false;
			try { auto input = core::YamlSerializer::fromString(YAML::Dump(invalid)); input->deserialize(); core::SerializationWorkData work; world->deserialize(*input, work); }
			catch (std::exception const&) { refused = true; }
			require(refused && write(*world, false) == baseline, "Malformed Dumbwaiter YAML mutated target: " + c);
		}
		auto bytes = write(*world, true);
		auto offset = bytes.find("travelSeconds"); require(offset != std::string::npos, "Missing binary timing field");
		offset += std::string("travelSeconds").size() + 1;
		for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = 0;
		bool refused = false;
		try { auto input = core::BinarySerializer::fromString(bytes); input->deserialize(); core::SerializationWorkData work; world->deserialize(*input, work); }
		catch (std::exception const&) { refused = true; }
		require(refused && write(*world, false) == baseline, "Malformed binary timing mutated target");
		for (auto field : {"lowerLandingPermissionRequirement", "upperLandingPermissionRequirement"})
		{
			auto corrupt = write(*world, true); auto start = corrupt.find(field);
			require(start != std::string::npos, "Missing binary requirement field");
			// Array tag/count, first uint32 item tag, then its little-endian value.
			start += std::string(field).size() + 1 + 8 + 1;
			for (unsigned i = 0; i < 4; ++i) corrupt[start + i] = static_cast<char>(0xff);
			bool rejected = false;
			try { auto input = core::BinarySerializer::fromString(corrupt); input->deserialize(); core::SerializationWorkData data; world->deserialize(*input, data); }
			catch (std::exception const&) { rejected = true; }
			require(rejected && write(*world, false) == baseline, "Malformed binary permission reference mutated target");
		}
		// Physical children add object slots, but legacy stable Marker removals
		// and landing requirements must continue to name their original owners.
		{
			auto source = dumbwaiter_fixture::make();
			auto device = source->addDumbwaiter(1, 0, 2); source->finishBuild();
			auto permission = source->addAccessPermission("Legacy lower");
			source->setInteractionPointPermissionRequirement(source->lookupDumbwaiter(device)->getLandingButton(0), {permission});
			auto marker = source->addSectorMarker(0, 0, 0.5f); source->finishBuild(); source->pauseSimulation();
			require(source->removeSectorMarker(0, marker.index), "Legacy Marker removal fixture failed");
			auto legacy = YAML::Load(write(*source, false)); legacy["version"] = 49;
			for (auto record : legacy["construction"])
				if (record["type"].as<std::string>() == "removeMarker") record["objectIndex"] = 3;
			auto input = core::YamlSerializer::fromString(YAML::Dump(legacy)); input->deserialize();
			core::World restored("Legacy children", 1, 1); core::SerializationWorkData data;
			require(restored.deserialize(*input, data) && restored.getMarkerIds().empty()
				&& restored.getInteractionPointPermissionRequirement(restored.lookupDumbwaiter(device)->getLandingButton(0)) == std::vector{permission}
				&& dumbwaiter_fixture::control(restored, device, 0), "Legacy child slots lost Marker or permission identity");
			auto invalid = YAML::Clone(legacy);
			auto neighbour = YAML::Clone(invalid["construction"][0]);
			neighbour["name"] = "Retained left wall"; neighbour["x"] = 1;
			invalid["construction"].push_back(neighbour);
			auto before = write(restored, false); auto graph = restored.getGraph(); bool rejected = false;
			try
			{
				auto reader = core::YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize();
				restored.deserialize(*reader, data);
			}
			catch (std::exception const&) { rejected = true; }
			require(rejected && write(restored, false) == before && restored.getGraph() == graph,
				"Legacy layout with no valid landing candidate was not refused transactionally");
		}
		// Schema 47 Dumbwaiters default to empty requirements.
		auto old = YAML::Clone(node); old["version"] = 47;
		for (auto record : old["construction"]) if (record["type"].as<std::string>() == "dumbwaiter")
		{ record.remove("lowerLandingPermissionRequirement"); record.remove("upperLandingPermissionRequirement"); }
		auto oldReader = core::YamlSerializer::fromString(YAML::Dump(old)); oldReader->deserialize();
		core::SerializationWorkData oldData; core::World oldWorld("Schema 47", 1, 1);
		require(oldWorld.deserialize(*oldReader, oldData), "Schema-47 Dumbwaiter refused");
		for (uint32_t stop : {0u,1u}) require(oldWorld.getInteractionPointPermissionRequirement(oldWorld.lookupDumbwaiter(id)->getLandingButton(stop)).empty(),
			"Legacy landing requirements did not default empty");
		// The implementation-time baseline remains loadable with no new authored objects.
		auto legacyWorld = dumbwaiter_fixture::make(); auto legacy = YAML::Load(write(*legacyWorld, false));
		legacy["version"] = 46; legacy.remove("nextDumbwaiterId"); legacy.remove("nextFurnitureId");
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize(); core::SerializationWorkData work;
		core::World loaded("Baseline", 1, 1); require(loaded.deserialize(*reader, work) && !loaded.hasDumbwaiters(), "Schema-46 compatibility lost");

		// Move into landings authored after the unit; replay must retain chronological
		// object slots, including when a new unit subsequently occupies the old site.
		dumbwaiter_fixture::addLandings(*world, 3, 0, 2); world->finishBuild();
		require(world->applyDumbwaiterMove(world->planMoveDumbwaiter(id, 3, 2, 0)), "Document movement failed");
		core::World::CreateDumbwaiterOptions copied{0, 0.5f};
		copied.landingPermissionRequirements = {{{a,b}, {upper}}};
		auto copy = world->addDumbwaiter(1, 0, 2, copied); world->finishBuild();
		for (bool binary : {false,true})
		{
			auto data = write(*world, binary);
			std::unique_ptr<core::Serializer> input = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			input->deserialize(); core::SerializationWorkData restoreData; core::World restored("Moved/pasted",1,1);
			require(restored.deserialize(*input,restoreData) && write(restored,binary) == data, "Moved/pasted canonical YAML/binary round trip failed");
			auto moved = restored.lookupDumbwaiter(id), pasted = restored.lookupDumbwaiter(copy);
			require(moved->getLayerIndex() == 3 && moved->getCellX() == 0 && moved->getCellY() == 2
				&& moved->getCarPosition().y == 3 && moved->getTravelSeconds() == 0.1f
				&& pasted->getLayerIndex() == 1 && pasted->getCarPosition().y == 0
				&& restored.getSimulationSnapshot().interactionPoints.size() == 4, "Restoration lost placement/ownership/configuration");
			for (auto unit : {moved,pasted})
			{
				require(restored.getInteractionPointPermissionRequirement(unit->getLandingButton(0)) == std::vector<core::AccessPermissionId>{a,b}
					&& restored.getInteractionPointPermissionRequirement(unit->getLandingButton(1)) == std::vector<core::AccessPermissionId>{upper}, "Restored move/copy requirements lost");
				auto operation = restored.pressDumbwaiterLanding(unit->getId(),0);
				require(restored.advanceTicks(60) && write(restored,binary) == data, "Moved/pasted mid-cycle save leaked runtime references/progress");
				require(restored.advanceTicks(unit->getId() == id ? 42 : 66)
					&& restored.lookupDeviceOperation(operation).entity->getState() == core::DeviceOperationState::Succeeded, "Restored moved/pasted unit cannot operate");
			}
		}
		auto movedDocument = write(*world,false);
		for (auto field : {"id", "layer", "x", "y"})
		{
			auto invalid = YAML::Load(movedDocument);
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() == "moveDumbwaiter") record[field] = 99;
			bool rejected = false;
			try { auto input = core::YamlSerializer::fromString(YAML::Dump(invalid)); input->deserialize(); core::SerializationWorkData data; world->deserialize(*input,data); }
			catch (std::exception const&) { rejected = true; }
			require(rejected && write(*world,false) == movedDocument, "Malformed movement record partially loaded");
		}
	}
	void boothWindows(smoke::Context const&)
	{
		using smoke::require;
		core::World world("BoothWindow document", 10, 3); world.addLayer();
		for (uint32_t layer = 0; layer < 3; ++layer) world.addRoom("Room", layer, 0, 0, 9, 2);
		auto closed = std::static_pointer_cast<const core::BoothWindow>(world.addBoothWindow(0, 0, 2).object);
		auto open = std::static_pointer_cast<const core::BoothWindow>(world.addBoothWindow(1, 0, 4, core::Window::State::Open).object);
		world.finishBuild();
		world.pauseSimulation();
		auto a = world.addAccessPermission("Panel A"), b = world.addAccessPermission("Panel B");
		require(world.setInteractionPointPermissionRequirement(closed->getPanel(), {a,b}), "Panel authoring failed");
		world.resumeSimulation();
		auto write = [](core::World const& source, bool binary) {
			auto serialize = [&](auto writer) {
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				source.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
			};
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		auto assertAuthored = [&](core::World const& source) {
			for (auto p : {std::array<uint32_t,3>{0,2,2}, {1,4,0}})
			{
				auto sector = source.getSectorAtPosition(p[0], float(p[1]), 0);
				bool found = false;
				for (uint32_t i = 0; i < sector->getNumObjects(); ++i)
					if (auto object = std::dynamic_pointer_cast<const core::WindowSectorObject>(sector->getObject(i));
						object && object->getCellX() == p[1])
					{
						auto booth = object->getWindow(); found = booth->isBoothWindow();
						auto device = std::static_pointer_cast<const core::BoothWindow>(booth);
						auto panel = source.lookupInteractionPoint(device->getPanel()).entity;
						require(panel && panel->getPosition().x == float(p[1]) + 0.5f
							&& panel->getPosition().y == 0 && panel->getSector().value == booth->getBackSector()->getIndex() + 1,
							"Round trip did not reconstruct correct owned panel");
						require(source.getInteractionPointPermissionRequirement(device->getPanel())
							== (p[0] == 0 ? std::vector<core::AccessPermissionId>{a,b} : std::vector<core::AccessPermissionId>{}),
							"Document/reset/replay lost or duplicated authored protection");
						require(object->getObjectType() == core::SectorObjectType::BoothWindow && booth->getFrontLayer() == p[0]
							&& booth->getBackLayer() == p[0] + 1 && static_cast<uint32_t>(booth->getState()) == p[2]
							&& !booth->isTraversalConfigured(), "Authored BoothWindow lost kind/pair/state");
					}
				require(found, "Authored BoothWindow missing");
			}
			require(source.getSimulationSnapshot().traversalResources.empty() && source.getSimulationSnapshot().interactionPoints.size() == 2
				&& source.getSimulationSnapshot().interactionRequests.empty()
				&& source.getSimulationSnapshot().deviceOperations.empty(), "Document persisted transient work or lost/duplicated owned panels");
		};
		auto baselineYaml = write(world, false), baselineBinary = write(world, true);
		for (auto device : {closed, open})
		{
			core::DeviceCommand command; command.type = core::DeviceCommandType::ToggleBoothWindow;
			command.boothWindow = device->getDeviceId(); require(bool(world.submitDeviceCommand(command)), "Runtime command refused");
		}
		require(write(world, false) == baselineYaml && write(world, true) == baselineBinary,
			"Saving pending commands persisted transient operations");
		require(world.advanceTicks(12), "Mid-motion save setup failed");
		require(closed->getState() == core::Window::State::Opening && open->getState() == core::Window::State::Closing,
			"Save did not exercise both moving states");
		require(write(world, false) == baselineYaml && write(world, true) == baselineBinary,
			"Saving mid-motion persisted progress, target, or pending operation");
		for (bool binary : {false,true})
		{
			auto data = write(world, binary);
			std::unique_ptr<core::Serializer> reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work), "BoothWindow round trip refused"); assertAuthored(loaded);
			loaded.resetSimulation(); assertAuthored(loaded); loaded.pauseSimulation(); loaded.addLayer(); assertAuthored(loaded);
		}
		// Save a real Agent's pending panel press, not just editor commands.
		world.resetSimulation();
		auto actor = world.createAgent("Pending panel operator", 1, 0, 2.35f);
		world.pauseSimulation();
		require(world.grantAgentAccessPermission(actor, a) && world.grantAgentAccessPermission(actor, b), "Pending operator grants failed");
		world.resumeSimulation();
		auto panelId = world.getSimulationSnapshot().interactionPoints.front().id;
		require(bool(world.requestInteraction(panelId, actor)), "Pending panel save fixture refused");
		for (bool binary : {false, true})
		{
			auto data = write(world, binary);
			std::unique_ptr<core::Serializer> reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Pending load", 1, 1);
			require(loaded.deserialize(*reader, work), "Pending panel document refused"); assertAuthored(loaded);
			require(loaded.advanceTicks(3), "Loaded panel tick failed"); assertAuthored(loaded);
		}
		auto node = YAML::Load(write(world, false));
		require(node["version"].as<int>() == 63, "BoothWindow schema not allocated");
		core::SerializationWorkData work;
		for (auto change : {"width", "height", "state", "glass", "traversal", "broken", "layer", "position", "legacy", "unknown", "permissionZero", "permissionUnknown", "permissionDuplicate", "permissionShape", "permissionLegacy"})
		{
			auto invalid = YAML::Clone(node);
			for (auto record : invalid["construction"]) if (record["type"].as<std::string>() == "boothWindow")
			{
				std::string c = change;
				if (c == "permissionZero") record["panelPermissionRequirement"] = std::vector<unsigned>{0};
				if (c == "permissionUnknown") record["panelPermissionRequirement"] = std::vector<unsigned>{256};
				if (c == "permissionDuplicate") record["panelPermissionRequirement"] = std::vector<unsigned>{1,1};
				if (c == "permissionShape") record["panelPermissionRequirement"] = "not an array";
				if (c == "permissionLegacy") invalid["version"] = 43;
				if (c == "width") record["cellsWide"] = 2;
				if (c == "height") record["levelsHigh"] = 0;
				if (c == "state") record["initialState"] = "broken";
				if (c == "glass") record["style"] = "clear";
				if (c == "traversal") record["traversable"] = true;
				if (c == "broken") record["initiallyBroken"] = false;
				if (c == "layer") record["layer"] = 2;
				if (c == "position") record["y"] = 1;
				if (c == "legacy") invalid["version"] = 42;
				if (c == "unknown") record["type"] = "boothWindowUnsupported";
			}
			auto baseline = write(world,false); bool refused = false;
			try { auto reader = core::YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); world.deserialize(*reader, work); }
			catch (std::exception const&) { refused = true; }
			require(refused && write(world,false) == baseline, std::string("Malformed BoothWindow loaded or mutated target: ") + change);
		}
		for (unsigned invalidId : {0u, 256u, 2u})
		{
			auto bytes = write(world, true);
			auto offset = bytes.find("panelPermissionRequirement");
			require(offset != std::string::npos, "Binary panel requirement field missing");
			offset += std::string("panelPermissionRequirement").size() + 1 + 8 + 1;
			// Array tag, uint64 count, uint32 tag precede the first reference.
			for (unsigned byte = 0; byte < 4; ++byte) bytes[offset + byte] = static_cast<char>((invalidId >> (8 * byte)) & 255);
			bool refused = false; auto before = write(world, false);
			try { auto input = core::BinarySerializer::fromString(bytes); input->deserialize(); world.deserialize(*input, work); }
			catch (std::exception const&) { refused = true; }
			require(refused && write(world, false) == before, "Malformed binary panel reference partially mutated World");
		}
		auto malformedBinary = write(world,true);
		auto stateOffset = malformedBinary.find("closed");
		require(stateOffset != std::string::npos, "Binary fixture missing shutter state");
		malformedBinary.replace(stateOffset, 6, "broken");
		bool binaryRefused = false; auto baseline = write(world,false);
		try { auto input = core::BinarySerializer::fromString(malformedBinary); input->deserialize(); world.deserialize(*input, work); }
		catch (std::exception const&) { binaryRefused = true; }
		require(binaryRefused && write(world,false) == baseline, "Malformed binary BoothWindow accepted or mutated target");
		auto previous = YAML::Clone(node); previous["version"] = 43;
		for (auto record : previous["construction"]) if (record["type"].as<std::string>() == "boothWindow")
			record.remove("panelPermissionRequirement");
		auto previousReader = core::YamlSerializer::fromString(YAML::Dump(previous)); previousReader->deserialize();
		core::World unrestricted("Previous schema",1,1);
		require(unrestricted.deserialize(*previousReader,work), "Schema-43 BoothWindow compatibility lost");
		for (auto const& point : unrestricted.getSimulationSnapshot().interactionPoints)
			require(unrestricted.getInteractionPointPermissionRequirement(point.id).empty(), "Legacy panel did not default unrestricted");
		auto legacy = YAML::Clone(node); legacy["version"] = 42;
		YAML::Node records(YAML::NodeType::Sequence);
		for (auto record : legacy["construction"]) if (record["type"].as<std::string>() != "boothWindow") records.push_back(record);
		legacy["construction"] = records;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
		core::World loaded("Legacy",1,1); require(loaded.deserialize(*reader,work), "Legacy document no longer compatible");
	}
}
