#include "WorldChecks.h"
#include "core/SecurityScannerTransit.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/World.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void securityScanners(smoke::Context const&)
	{
		using smoke::require;
		for (uint32_t width : { 1u, 4u })
			for (bool direction : { false, true })
				for (bool open : { false, true })
				{
					core::World world("Persisted scanner", 12, 3);
					world.addRoom("Left", 1, 1, 0, 2, 1); world.addCorridor(1, 1, 2 + width, 2, 1);
					if (open)
					{
						// Detached open ends are supported persisted wall-restoration state.
						auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work;
						world.serialize(*writer, work); writer->serialize(); auto node = YAML::Load(writer->getSerializedString());
						YAML::Node wall; wall["type"] = "removeWall"; wall["sectorIndex"] = 0;
						wall["levelIndex"] = 0; wall["side"] = "right"; wall["airlockWallRestoration"] = true;
						node["construction"].push_back(wall);
						auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
						require(world.deserialize(*reader, work), "Open-end fixture refused"); world.pauseSimulation();
					}
					auto index = world.addSecurityScanner(1, 1, 2, width, direction);
					world.finishBuild(); world.pauseSimulation();
					require(world.setSecurityScannerConfiguration(index, 12345, 0, 0.1f, 10), "Scanner configuration refused");
					auto assertAuthored = [&](core::World const& source) {
						auto chamber = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(source.getSector(index));
						require(chamber && chamber->getLayerIndex() == 1 && chamber->getCellY() == 1
							&& chamber->getCellX() == 2 && chamber->getCellsWide() == width && chamber->getCapacity() == 1
							&& chamber->isLeftToRight() == direction && chamber->getPreDelaySeconds() == 0
							&& chamber->getScanSeconds() == 0.1f && chamber->getPostPauseSeconds() == 10
							&& chamber->getSensorDistance() == 12345, "Scanner authored data lost");
						require(chamber->getPreviousEnd(0) == (open ? core::SectorEndType::None : core::SectorEndType::Wall)
							&& chamber->getPreviousEnd(1) == core::SectorEndType::Wall, "Scanner wall restoration lost");
						for (int side = 0; side < 2; ++side)
							require(chamber->getDoor(side)->isSecurityScannerOwned() && chamber->getDoor(side)->isClosed()
								&& chamber->getDoor(side)->getSideSector(side == 0 ? 1 : 0)->getIndex() == index,
								"Scanner generated owned Door relationship lost");
						require(source.getSimulationSnapshot().interactionPoints.empty() && chamber->getAgents().empty(), "Scanner loaded transient work/controls");
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
						std::unique_ptr<core::Serializer> reader = binary
							? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(write(true)))
							: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(write(false)));
						reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
						require(loaded.deserialize(*reader, work), "Scanner round trip refused"); assertAuthored(loaded);
						loaded.resetSimulation(); assertAuthored(loaded);
						loaded.pauseSimulation(); loaded.addLayer(); assertAuthored(loaded);
						// Same-Layer relationships survive deleting the Layer in front.
						require(loaded.applyDeleteLayer(loaded.planDeleteLayer(0)), "Scanner Layer deletion refused");
						auto shifted = std::dynamic_pointer_cast<const core::SecurityScannerTransit>(loaded.getSector(index));
						require(shifted && shifted->getLayerIndex() == 0 && shifted->getCapacity() == 1
							&& shifted->getDoor(0)->isClosed(), "Scanner Layer compaction lost ownership");
					}
					world.resetSimulation(); assertAuthored(world);
					auto baseline = write(false);
					auto node = YAML::Load(baseline);
					for (auto field : { "capacity", "preDelaySeconds", "scanSeconds", "postPauseSeconds", "sensorDistance", "cellsWide", "levelsHigh", "layer", "x", "y", "leftToRight", "leftWasOpen" })
						for (bool missing : { false, true })
						{
							auto invalid = YAML::Clone(node);
							for (auto record : invalid["construction"])
								if (record["type"].as<std::string>() == "securityScanner")
								{
									if (missing) record.remove(field);
									else if (std::string(field) == "leftWasOpen") record[field] = !open;
									else if (std::string(field) == "leftToRight") record[field] = "not-a-bool";
									else record[field] = -10;
								}
							bool refused = false;
							try { auto reader = core::YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); core::SerializationWorkData work; world.deserialize(*reader, work); }
							catch (std::exception const&) { refused = true; }
							require(refused && write(false) == baseline, "Malformed scanner accepted or changed target World");
							assertAuthored(world);
						}
					for (auto field : { "sensorDistance", "preDelaySeconds", "scanSeconds", "postPauseSeconds" })
						for (auto value : { ".nan", ".inf", "-.inf", "11", "0.099" })
						{
							if (std::string(field) == "sensorDistance" && (std::string(value) == "11" || std::string(value) == "0.099")) continue;
							if (std::string(field) != "scanSeconds" && std::string(value) == "0.099") continue;
							auto invalid = YAML::Clone(node);
							for (auto record : invalid["construction"])
								if (record["type"].as<std::string>() == "securityScanner") record[field] = YAML::Load(value);
							bool refused = false;
							try { auto reader = core::YamlSerializer::fromString(YAML::Dump(invalid)); reader->deserialize(); core::SerializationWorkData work; world.deserialize(*reader, work); }
							catch (std::exception const&) { refused = true; }
							require(refused && write(false) == baseline, "Invalid loaded timing mutated World");
						}
					// A scanner cannot be smuggled into an older schema.
					node["version"] = 42; bool refused = false;
					try { auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize(); core::SerializationWorkData work; world.deserialize(*reader, work); }
					catch (std::exception const&) { refused = true; }
					require(refused, "Old schema accepted scanner");
				}
		core::World legacy("Old document", 8, 2); legacy.addCorridor(0, 0, 0, 4, 1); legacy.finishBuild();
		auto writer = core::YamlSerializer::toString(); core::SerializationWorkData work; legacy.serialize(*writer, work); writer->serialize();
		auto node = YAML::Load(writer->getSerializedString()); node["version"] = 42;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(node)); reader->deserialize();
		require(legacy.deserialize(*reader, work) && legacy.getNumSectors() == 1, "Older scanner-free documents no longer load");
	}
}
