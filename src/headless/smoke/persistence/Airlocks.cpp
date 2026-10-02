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
		core::World world("Persisted chambers", 12, 3);
		world.addRoom("Left", 0, 0, 0, 2, 1); world.addCorridor(0, 0, 5, 2, 1);
		auto index = world.addAirlock(0, 0, 2, 3, 7.5f);
		world.addRoom("Ordinary", 1, 0, 0, 7, 1);
		world.finishBuild(); world.pauseSimulation();
		auto chamberIn = [&](core::World const& source) {
			return std::dynamic_pointer_cast<const core::AirlockTransit>(source.getSector(index));
		};
		auto assertAuthored = [&](core::World const& source) {
			auto chamber = chamberIn(source);
			require(chamber && chamber->getCellX() == 2 && chamber->getCellY() == 0
				&& chamber->getCellsWide() == 3 && chamber->getLayerIndex() == 0
				&& chamber->getCycleSeconds() == 7.5f && chamber->getCapacity() == 3, "Airlock identity/geometry/timing lost");
			auto state = source.getSimulationSnapshot();
			require(state.airlocks.size() == 1 && state.airlocks[0].sector.value == index + 1
				&& state.airlocks[0].occupants.empty() && state.airlocks[0].cycleComplete
				&& state.airlocks[0].traversalAvailable && state.airlocks[0].remainingCycleSeconds == 0
				&& state.airlocks[0].doors[0] == core::DoorSnapshotState::Closed
				&& state.airlocks[0].doors[1] == core::DoorSnapshotState::Closed,
				"Load/reset must be empty, closed, initially cycled, ready");
			for (uint32_t i = 0; i < 3; ++i)
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
	}
}
