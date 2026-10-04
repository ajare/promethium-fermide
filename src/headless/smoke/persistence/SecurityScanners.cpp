#include "WorldChecks.h"
#include "core/ChamberTransit.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include "core/World.h"
#include "core/Graph.h"
#include "core/Agent.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void securityScanners(smoke::Context const&)
	{
		using smoke::require;
		for (uint32_t width : { 1u, 4u })
			for (bool direction : { false, true })
			{
				core::World world("Persisted decontamination", 12, 2);
				world.addRoom("Left", 0, 0, 0, 2, 1); world.addCorridor(0, 0, 2 + width, 3, 1);
				auto index = world.addChamber(0, 0, 2, width, direction, core::ChamberSubtype::Decontamination);
				world.finishBuild(); world.pauseSimulation();
				require(world.setChamberConfiguration(index, 7, 3, 4, 5), "Decontamination configuration refused");
				for (bool binary : { false, true })
				{
					core::SerializationWorkData work;
					auto write = [&](auto writer) {
						world.serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
					};
					auto bytes = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
					std::unique_ptr<core::Serializer> reader = binary
						? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
						: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
					reader->deserialize(); core::World loaded("Loaded decontamination", 1, 1);
					require(loaded.deserialize(*reader, work), "Decontamination round trip refused");
					auto assertData = [&] {
						auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(loaded.getSector(index));
						require(chamber && chamber->getSubtype() == core::ChamberSubtype::Decontamination
							&& chamber->getCapacity() == width && chamber->isLeftToRight() == direction
							&& chamber->getSensorDistance() == 7 && chamber->getPreDelaySeconds() == 3
							&& chamber->getScanSeconds() == 4 && chamber->getPostPauseSeconds() == 5,
							"Decontamination replay lost configuration");
					};
					assertData(); loaded.resetSimulation(); assertData(); loaded.pauseSimulation();
					index = loaded.applyChamberEdit(loaded.planResizeChamber(index, 2, 0, width, direction)); assertData();
					index = loaded.applyChamberEdit(loaded.planSetChamberSubtype(index, core::ChamberSubtype::SecurityScanner));
					require(loaded.getSector(index)->getCapacity() == 1, "Scanner subtype switch retained batch capacity");
					index = loaded.applyChamberEdit(loaded.planSetChamberSubtype(index, core::ChamberSubtype::Decontamination)); assertData();
					if (!binary)
					{
						for (bool oldSchema : { false, true })
						{
							auto node = YAML::Load(bytes);
							if (oldSchema) node["version"] = 45;
							else for (auto record : node["construction"])
								if (record["type"].as<std::string>() == "chamber") record["capacity"] = width + 1;
							auto invalid = core::YamlSerializer::fromString(YAML::Dump(node)); invalid->deserialize();
							bool refused = false;
							try { refused = !loaded.deserialize(*invalid, work); } catch (std::exception const&) { refused = true; }
							require(refused, "Invalid decontamination schema/capacity accepted"); assertData();
						}
					}
				}
			}
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
					auto index = world.addChamber(1, 1, 2, width, direction);
					world.finishBuild(); world.pauseSimulation();
					require(world.setChamberConfiguration(index, 12345, 0, 0.1f, 10), "Scanner configuration refused");
					auto assertAuthored = [&](core::World const& source) {
						auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(source.getSector(index));
						require(chamber && chamber->getType() == core::SectorType::Chamber
							&& chamber->getSubtype() == core::ChamberSubtype::SecurityScanner && chamber->getLayerIndex() == 1 && chamber->getCellY() == 1
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
					auto journey = [&](core::World& loaded) {
						loaded.pauseSimulation();
						auto source = direction ? 0u : 1u, destination = 1 - source;
						auto marker = loaded.addSectorMarker(destination, 0, 1.0f);
						loaded.finishBuild();
						auto id = loaded.createAgent("Loaded traveller", source, 0, 1.0f);
						auto actor = loaded.lookupAgent(id).entity;
						auto path = loaded.getGraph()->calculatePath(actor,
							loaded.getGraph()->getVertexForObject(marker.sector->getObject(marker.index)));
						require(bool(path), "Loaded Chamber did not route its authored direction");
						actor->setPath(path, true); loaded.resumeSimulation();
						bool scanned = false, exited = false;
						// Four six-second Door movements plus ten-second post-pause and walking.
						for (unsigned tick = 0; tick < 4800; ++tick)
						{
							loaded.advanceTick();
							auto chamber = std::dynamic_pointer_cast<const core::ChamberTransit>(loaded.getSector(index));
							require(chamber->getAgents().size() <= 1
								&& (chamber->getDoor(0)->isClosed() || chamber->getDoor(1)->isClosed()), "Loaded Chamber capacity/interlock lost");
							scanned = scanned || chamber->getPhase() == core::SecurityScannerPhase::Scanning;
							if (actor->getSector()->getIndex() == destination && chamber->getPhase() == core::SecurityScannerPhase::Idle)
							{ exited = true; break; }
						}
						require(scanned && exited, "Loaded Chamber did not complete an automatic scanner journey: width="
							+ std::to_string(width) + " direction=" + std::to_string(direction) + " phase="
							+ std::dynamic_pointer_cast<const core::ChamberTransit>(loaded.getSector(index))->getPhaseName()
							+ " sector=" + std::to_string(actor->getSector()->getIndex()) + " scanned=" + std::to_string(scanned));
						loaded.pauseSimulation(); require(loaded.removeAgent(id).removed, "Journey cleanup refused");
					};
					for (bool binary : { false, true })
					{
						std::unique_ptr<core::Serializer> reader = binary
							? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(write(true)))
							: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(write(false)));
						reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
						require(loaded.deserialize(*reader, work), "Scanner round trip refused"); assertAuthored(loaded);
						journey(loaded);
						loaded.resetSimulation(); assertAuthored(loaded);
						loaded.pauseSimulation(); loaded.addLayer(); assertAuthored(loaded);
						// Same-Layer relationships survive deleting the Layer in front.
						require(loaded.applyDeleteLayer(loaded.planDeleteLayer(0)), "Scanner Layer deletion refused");
						auto shifted = std::dynamic_pointer_cast<const core::ChamberTransit>(loaded.getSector(index));
						require(shifted && shifted->getLayerIndex() == 0 && shifted->getCapacity() == 1
							&& shifted->getDoor(0)->isClosed(), "Scanner Layer compaction lost ownership");
					}
					world.resetSimulation(); assertAuthored(world);
					auto baseline = write(false);
					auto node = YAML::Load(baseline);
					for (auto record : node["construction"])
						if (record["type"].as<std::string>() == "chamber")
							require(record["subtype"].as<std::string>() == "securityScanner", "Chamber subtype not persisted explicitly");
					for (unsigned version : { 43u, 44u, 45u })
					{
						auto legacyNode = YAML::Clone(node); legacyNode["version"] = version;
						// Pre-merge scanner documents did not allocate Furniture IDs.
						legacyNode.remove("nextFurnitureId");
						for (auto record : legacyNode["construction"])
							if (version < 45 && record["type"].as<std::string>() == "chamber")
							{ record["type"] = "securityScanner"; record.remove("subtype"); }
						auto input = core::YamlSerializer::fromString(YAML::Dump(legacyNode)); input->deserialize();
						core::SerializationWorkData work; core::World migrated("Migrated", 1, 1);
						require(migrated.deserialize(*input, work), "Legacy scanner migration refused");
						assertAuthored(migrated);
						// Migrated records must support the same Chamber editing/replay commands.
						migrated.pauseSimulation();
						index = migrated.applyChamberEdit(migrated.planResizeChamber(index, 2, 1, width, !direction));
						require(std::dynamic_pointer_cast<const core::ChamberTransit>(migrated.getSector(index))->isLeftToRight() == !direction,
							"Migrated Chamber reversal refused");
						index = migrated.applyChamberEdit(migrated.planResizeChamber(index, 2, 1, width, direction));
						assertAuthored(migrated); journey(migrated);
						auto output = core::YamlSerializer::toString(); migrated.serialize(*output, work); output->serialize();
						auto savedNode = YAML::Load(output->getSerializedString());
						bool foundChamber = false;
						for (auto record : savedNode["construction"])
						{
							require(record["type"].as<std::string>() != "securityScanner", "Migration saved legacy scanner identity");
							if (record["type"].as<std::string>() == "chamber")
							{
								foundChamber = true;
								require(record["subtype"].as<std::string>() == "securityScanner", "Migrated Chamber lost subtype on save");
							}
						}
						require(foundChamber, "Migration did not save Chamber identity");
						// Reopen the new representation after migration and authored replay,
						// including the binary format; no scanner identity alias is needed.
						for (bool binary : { false, true })
						{
							auto binaryOutput = core::BinarySerializer::toString();
							migrated.serialize(*binaryOutput, work); binaryOutput->serialize();
							std::unique_ptr<core::Serializer> savedInput = binary
								? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(binaryOutput->getSerializedString()))
								: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(output->getSerializedString()));
							savedInput->deserialize(); core::World reopened("Reopened migrated Chamber", 1, 1);
							require(reopened.deserialize(*savedInput, work), "Migrated Chamber new-format round trip refused");
							assertAuthored(reopened);
						}
					}
					for (auto field : { "subtype", "capacity", "preDelaySeconds", "scanSeconds", "postPauseSeconds", "sensorDistance", "cellsWide", "levelsHigh", "layer", "x", "y", "leftToRight", "leftWasOpen" })
						for (bool missing : { false, true })
						{
							auto invalid = YAML::Clone(node);
							for (auto record : invalid["construction"])
								if (record["type"].as<std::string>() == "chamber")
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
								if (record["type"].as<std::string>() == "chamber") record[field] = YAML::Load(value);
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
