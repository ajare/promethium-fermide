#include "WorldChecks.h"
#include "core/AirlockTransit.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/World.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void airlocks(smoke::Context const&)
	{
		using smoke::require;
		// Pre-automation documents allocated three point IDs per chamber.
		// Removing the physical internal button must not retarget permissions
		// on a subsequent chamber or an unrelated authored control.
		{
			core::World legacySource("Legacy control identities", 20, 2);
			legacySource.addRoom("Left", 0, 0, 0, 2, 1);
			legacySource.addRoom("Middle", 0, 0, 5, 2, 1);
			legacySource.addRoom("Right", 0, 0, 10, 3, 1);
			legacySource.addAirlock(0, 0, 2, 3);
			legacySource.addAirlock(0, 0, 7, 3);
			legacySource.addSectorLightSwitch(2, 2);
			legacySource.finishBuild(); legacySource.pauseSimulation();
			auto key = legacySource.addAccessPermission("Legacy control protection");
			auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
			work.markSerializedUnmodified = false; legacySource.serialize(*writer, work); writer->serialize();
			auto legacy = YAML::Load(writer->getSerializedString());
			legacy["version"] = 42;
			for (auto id : { 4, 7 })
			{
				YAML::Node requirement;
				requirement["interactionPoint"] = id;
				requirement["permissions"].push_back(key.value);
				legacy["interactionPermissionRequirements"].push_back(requirement);
			}
			core::World migrated("Migrated", 1, 1);
			auto input = core::YamlSerializer::fromString(YAML::Dump(legacy)); input->deserialize();
			require(migrated.deserialize(*input, work), "Legacy Airlock control identities refused");
			auto check = [&](core::World const& source) {
				auto state = source.getSimulationSnapshot();
				require(state.airlocks.size() == 2 && state.interactionPoints.size() == 5
					&& state.airlocks[0].controls[0].value == 1 && state.airlocks[0].controls[1].value == 2
					&& state.airlocks[1].controls[0].value == 4 && state.airlocks[1].controls[1].value == 5
					&& !source.lookupInteractionPoint(core::InteractionPointId{ 3 })
					&& !source.lookupInteractionPoint(core::InteractionPointId{ 6 }), "Removed internal buttons shifted later controls or survived migration");
				for (uint64_t id : { 4u, 7u })
					require(source.getInteractionPointPermissionRequirement(core::InteractionPointId{ id }) == std::vector<core::AccessPermissionId>{ key },
						"Legacy permission reference retargeted after internal button removal");
			};
			check(migrated); migrated.resetSimulation(); check(migrated);
			for (bool binary : { false, true })
			{
				auto save = [&](auto output) {
					migrated.serialize(*output, work); output->serialize(); return output->getSerializedString();
				};
				auto data = binary ? save(core::BinarySerializer::toString()) : save(core::YamlSerializer::toString());
				std::unique_ptr<core::Serializer> reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
				reader->deserialize(); core::World reloaded("Reloaded", 1, 1);
				require(reloaded.deserialize(*reader, work), "Migrated control references failed round trip"); check(reloaded);
			}
		}
		core::World world("Persisted chambers", 12, 3);
		world.addRoom("Left", 0, 0, 0, 2, 1); world.addCorridor(0, 0, 5, 2, 1);
		auto index = world.addAirlock(0, 0, 2, 3, 7.5f);
		world.addRoom("Ordinary", 1, 0, 0, 7, 1);
		world.finishBuild(); world.pauseSimulation();
		auto chamberIn = [&](core::World const& source) {
			return std::dynamic_pointer_cast<const core::AirlockTransit>(source.getSector(index));
		};
		auto leftKey = world.addAccessPermission("Left operation");
		auto rightKey = world.addAccessPermission("Right operation");
		require(world.setInteractionPointPermissionRequirement(chamberIn(world)->getControl(0), { leftKey })
			&& world.setInteractionPointPermissionRequirement(chamberIn(world)->getControl(1), { rightKey }), "Outside requirements refused");
		require(world.getSimulationSnapshot().interactionPoints.size() == 2, "Internal button was generated");
		auto assertAuthored = [&](core::World const& source) {
			auto chamber = chamberIn(source);
			require(chamber && chamber->getCellX() == 2 && chamber->getCellY() == 0
				&& chamber->getCellsWide() == 3 && chamber->getLayerIndex() == 0
				&& chamber->getCycleSeconds() == 7.5f && chamber->getCapacity() == 3, "Airlock identity/geometry/timing lost");
			require(source.getInteractionPointPermissionRequirement(chamber->getControl(0)) == std::vector<core::AccessPermissionId>{ leftKey }
				&& source.getInteractionPointPermissionRequirement(chamber->getControl(1)) == std::vector<core::AccessPermissionId>{ rightKey }, "Independent requirements lost on load/replay/reset");
			auto state = source.getSimulationSnapshot();
			require(state.airlocks.size() == 1 && state.airlocks[0].sector.value == index + 1
				&& state.airlocks[0].occupants.empty() && state.airlocks[0].cycleComplete
				&& state.airlocks[0].traversalAvailable && state.airlocks[0].remainingCycleSeconds == 0
				&& state.airlocks[0].doors[0] == core::DoorSnapshotState::Closed
				&& state.airlocks[0].doors[1] == core::DoorSnapshotState::Closed,
				"Load/reset must be empty, closed, initially cycled, ready");
			require(state.interactionPoints.size() == 2 && chamber->getNumObjects() == 2, "Load/reset regenerated an internal button");
			for (uint32_t i = 0; i < 2; ++i)
				require(chamber->getControl(i) == chamberIn(world)->getControl(i)
					&& source.lookupInteractionPoint(chamber->getControl(i)), "Generated control identity lost");
			require(source.getSector(3)->getName() == "Ordinary" && source.getSector(3)->getNumObjects() == 0,
				"Airlock persistence changed ordinary objects");
		};
		auto write = [&](bool binary) {
			auto serialize = [&](auto writer) {
				core::SerializationWorkData work; work.markSerializedUnmodified = false;
				world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
			};
			return binary ? serialize(core::BinarySerializer::toString()) : serialize(core::YamlSerializer::toString());
		};
		for (bool binary : { false, true })
		{
			auto data = write(binary);
			std::unique_ptr<core::Serializer> reader = binary
				? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work), "Airlock document refused"); assertAuthored(loaded);
			require(chamberIn(loaded)->getPreviousEnd(0) == core::SectorEndType::Wall
				&& chamberIn(loaded)->getPreviousEnd(1) == core::SectorEndType::Wall, "Closed wall restoration lost");
			loaded.resetSimulation(); assertAuthored(loaded);
		}
		world.resetSimulation(); assertAuthored(world);
		world.pauseSimulation();
		// Existing structural replay, not a low-level Airlock test interface.
		world.addLayer(); assertAuthored(world);
		auto node = YAML::Load(write(false));
		for (auto record : node["construction"])
			if (record["type"].as<std::string>() == "airlock") record["leftWasOpen"] = true;
		core::World loaded("Open wall restoration", 1, 1); core::SerializationWorkData work;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
		require(loaded.deserialize(*reader, work) && chamberIn(loaded)->getPreviousEnd(0) == core::SectorEndType::None
			&& chamberIn(loaded)->getPreviousEnd(1) == core::SectorEndType::Wall, "Originally open wall restoration lost");
		loaded.resetSimulation();
		require(chamberIn(loaded)->getPreviousEnd(0) == core::SectorEndType::None, "Replay closed an originally open wall");
		// Legacy chambers default to unrestricted controls; protection cannot be
		// smuggled into an older schema or silently dropped during atomic load.
		auto protectedNode = node;
		for (auto value : { "[1, 1]", "[0]", "[256]", "[-1]", "broken" })
		{
			auto invalid = YAML::Clone(protectedNode);
			for (auto record : invalid["construction"])
				if (record["type"].as<std::string>() == "airlock")
					record["leftControlPermissionRequirement"] = YAML::Load(value);
			bool rejected = false;
			try { auto input = core::YamlSerializer::fromString(YAML::Dump(invalid)); input->deserialize(); loaded.deserialize(*input, work); }
			catch (std::exception const&) { rejected = true; }
			require(rejected && loaded.getInteractionPointPermissionRequirement(chamberIn(loaded)->getControl(0)) == std::vector<core::AccessPermissionId>{ leftKey },
				"Malformed outside requirement loaded or changed World");
		}
		auto legacy = YAML::Clone(protectedNode); legacy["version"] = 40;
		bool rejected = false;
		try { auto input = core::YamlSerializer::fromString(YAML::Dump(legacy)); input->deserialize(); loaded.deserialize(*input, work); }
		catch (std::exception const&) { rejected = true; }
		require(rejected, "Old schema silently accepted outside requirements");
		for (auto record : legacy["construction"])
			if (record["type"].as<std::string>() == "airlock")
			{ record.remove("leftControlPermissionRequirement"); record.remove("rightControlPermissionRequirement"); }
		legacy.remove("interactionPermissionRequirements");
		core::World unrestricted("Legacy Airlock", 1, 1);
		auto input = core::YamlSerializer::fromString(YAML::Dump(legacy)); input->deserialize();
		require(unrestricted.deserialize(*input, work)
			&& unrestricted.getInteractionPointPermissionRequirement(chamberIn(unrestricted)->getControl(0)).empty()
			&& unrestricted.getInteractionPointPermissionRequirement(chamberIn(unrestricted)->getControl(1)).empty(), "Legacy outside controls restricted");
		auto baseline = loaded.getNumSectors();
		for (auto record : node["construction"])
			if (record["type"].as<std::string>() == "airlock") record["cycleSeconds"] = 11;
		bool refused = false;
		try { reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize(); loaded.deserialize(*reader, work); }
		catch (std::exception const&) { refused = true; }
		require(refused && loaded.getNumSectors() == baseline && chamberIn(loaded)->getCycleSeconds() == 7.5f, "Malformed Airlock load changed target World");
		for (auto record : node["construction"])
			if (record["type"].as<std::string>() == "airlock") { record["cycleSeconds"] = 3; record["initiallyBroken"] = true; }
		refused = false;
		try { reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize(); loaded.deserialize(*reader, work); }
		catch (std::exception const&) { refused = true; }
		require(refused && chamberIn(loaded)->getCycleSeconds() == 7.5f, "Broken Airlock authored or load not atomic");
		loaded.pauseSimulation();
		require(loaded.planRemoveAirlock(index).valid, "Loaded originally open chamber delete refused");
		loaded.applyAirlockEdit(loaded.planRemoveAirlock(index));
		require(loaded.getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::None
			&& loaded.getSector(1)->getEndType(0, CORE_SIDE_LEFT) == core::SectorEndType::Wall,
			"Deletion ignored authoritative saved wall restoration flags");
		loaded.resetSimulation();
		require(loaded.getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::None,
			"Replay lost detached originally open wall");
	}
}
