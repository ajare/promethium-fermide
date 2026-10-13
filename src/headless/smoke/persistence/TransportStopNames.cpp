#include "WorldChecks.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/LiftTransit.h"
#include "core/ShuttleTransit.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void transportStopShortNames(smoke::Context const&)
	{
		using smoke::require;
		for (auto const& valid : {"A", "AB", "A9Z", "000", "999"})
			require(core::World::isValidTransportStopShortName(valid), "Valid uppercase alphanumeric label refused");
		for (bool shuttle : {false, true})
		{
			core::World world("Stop short names", 32, 3);
			for (uint32_t level = 0; level < 3; ++level) world.addCorridor(level, 0, 32);
			uint32_t sector;
			core::TraversalResourceId resource;
			if (shuttle)
			{
				core::World::CreateShuttleOptions options{1, 3, {0, 8, 16}, 0};
				auto created = world.addShuttle(1, 0, 0, 24, options);
				sector = created.shuttle.sector->getIndex(); resource = created.traversalResource;
			}
			else
			{
				auto created = world.addLift(1, 0, 2, 1, 3);
				sector = created.lift.sector->getIndex(); resource = created.traversalResource;
			}
			world.finishBuild(); world.pauseSimulation();
			for (uint32_t stop = 0; stop < 3; ++stop)
				require(world.getTransportStopShortName(sector, stop) == std::to_string(stop)
					&& world.getTransportStopShortName(resource, stop) == std::to_string(stop), "Stop names must default to zero-based indices");
			auto serialize = [&](bool binary) {
				auto encode = [&](auto writer) {
					core::SerializationWorkData data; data.markSerializedUnmodified = false;
					world.serialize(*writer, data); writer->serialize(); return writer->getSerializedString();
				};
				return binary ? encode(core::BinarySerializer::toString()) : encode(core::YamlSerializer::toString());
			};
			auto load = [](core::World& target, std::string const& bytes, bool binary) {
				std::unique_ptr<core::Serializer> reader = binary
					? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
				reader->deserialize(); core::SerializationWorkData data;
				return target.deserialize(*reader, data);
			};
			std::string diagnostic;
			for (auto const& invalid : {"", "abcd", "abc", "A B", "A-1", "A_1", "é"})
			{
				auto before = serialize(false);
				require(!world.setTransportStopShortName(sector, 1, invalid, &diagnostic)
					&& !diagnostic.empty() && serialize(false) == before, "Invalid labels must be rejected without mutation");
			}
			require(!world.setTransportStopShortName(sector, 3, "XYZ"), "Unknown Stop must be refused");
			for (uint32_t stop = 0; stop < 3; ++stop)
				require(world.setTransportStopShortName(sector, stop, "B" + std::to_string(stop)), "Valid Stop label refused");
			world.resumeSimulation();
			require(!world.setTransportStopShortName(sector, 0, "RUN"), "Running simulation must refuse authored Stop-name edits");
			world.pauseSimulation();
			for (bool binary : {false, true})
			{
				core::World restored("Placeholder", 1, 1);
				require(load(restored, serialize(binary), binary), "Stop names failed to round-trip");
				for (uint32_t stop = 0; stop < 3; ++stop)
					require(restored.getTransportStopShortName(sector, stop) == "B" + std::to_string(stop), "Round-trip lost a Stop name");
				restored.resetSimulation();
				require(restored.getTransportStopShortName(sector, 1) == "B1", "Reset lost authored Stop names");
			}
			auto legacy = YAML::Load(serialize(false)); legacy["version"] = 64;
			for (auto record : legacy["construction"]) record.remove("stopShortNames");
			core::World restored("Legacy", 1, 1);
			require(load(restored, YAML::Dump(legacy), false)
				&& restored.getTransportStopShortName(sector, 2) == "2", "Legacy Worlds must receive default Stop names");
			for (auto const& invalid : {"bad", "TOOLONG", "", "A!"})
			{
				auto corrupt = YAML::Load(serialize(false));
				for (auto record : corrupt["construction"])
					if (record["stopShortNames"]) record["stopShortNames"][1] = invalid;
				bool rejected = false;
				try { rejected = !load(restored, YAML::Dump(corrupt), false); } catch (std::exception const&) { rejected = true; }
				require(rejected && restored.getTransportStopShortName(sector, 2) == "2", "Malformed label load must leave the previous World intact");
			}
			for (bool extra : {false, true})
			{
				auto corrupt = YAML::Load(serialize(false));
				for (auto record : corrupt["construction"])
					if (record["stopShortNames"])
					{
						if (extra) record["stopShortNames"].push_back("EX");
						else record["stopShortNames"].remove(2);
					}
				bool rejected = false;
				try { rejected = !load(restored, YAML::Dump(corrupt), false); } catch (std::exception const&) { rejected = true; }
				require(rejected && restored.getTransportStopShortName(sector, 2) == "2", "Mismatched Stop-name list must be rejected atomically");
			}
			if (shuttle)
			{
				auto plan = world.planResizeShuttle(sector, 8, 0, 16);
				require(plan.valid, plan.diagnostic); sector = world.applyShuttleEdit(plan);
			}
			else
			{
				auto plan = world.planResizeLift(sector, 2, 1, 1, 2);
				require(plan.valid, plan.diagnostic); sector = world.applyLiftEdit(plan);
			}
			require(world.getTransportStopShortName(sector, 0) == "B1"
				&& world.getTransportStopShortName(sector, 1) == "B2", "Surviving names must follow Stop identity when indices change");
			if (!shuttle)
			{
				auto plan = world.planResizeLift(sector, 2, 0, 1, 3);
				require(plan.valid, plan.diagnostic); sector = world.applyLiftEdit(plan);
				require(world.getTransportStopShortName(sector, 0) == "0"
					&& world.getTransportStopShortName(sector, 1) == "B1", "Inserted Stop must get a numeric default without shifting surviving names");
			}
		}
		// Creation options and open Platform lift Stop labels use the same contract.
		for (bool shuttle : {false, true})
		for (bool invalid : {false, true})
		{
			core::World world("Authored Stop labels", 12, 3);
			for (uint32_t y = 0; y < 3; ++y) world.addCorridor(y, 0, 12);
			core::World::CreateLiftOptions options; options.stopOffsets = {0, 1, 2};
			options.stopShortNames = {"G", invalid ? "bad" : "L1", "TOP"};
			bool rejected = false;
			try
			{
				uint32_t sector;
				if (shuttle)
				{
					core::World::CreateShuttleOptions shuttleOptions{1, 3, {0, 4, 8}, 0};
					shuttleOptions.stopShortNames = options.stopShortNames;
					sector = world.addShuttle(1, 0, 0, 12, shuttleOptions).shuttle.sector->getIndex();
				}
				else sector = world.addLift(1, 0, 2, options).lift.sector->getIndex();
				require(world.getTransportStopShortName(sector, 2) == "TOP", "Creation options lost authored Stop names");
			}
			catch (std::exception const&) { rejected = true; }
			require(rejected == invalid, "Transport creation must apply shared Stop-name validation");
			if (invalid) require(world.getNumSectors() == 3, "Rejected Stop names must not partially create a transport");
		}
		{
			core::World world("Platform Stop labels", 12, 3);
			auto room = world.addRoom("Room", 0, 0, 0, 12, 3);
			world.addSectorWalkway(room, 2, 4); world.addSectorWalkway(room, 2, 5);
			core::World::CreateLiftOptions options; options.stopOffsets = {0, 2}; options.stopShortNames = {"G", "TOP"};
			auto result = world.addSectorPlatformLift(room, 0, 4, options);
			world.finishBuild(); world.pauseSimulation();
			require(world.getTransportStopShortName(room, 1, result.lift.index) == "TOP"
				&& world.getTransportStopShortName(result.traversalResource, 0) == "G", "Platform lift name lookup must support Stop and resource identity");
			require(world.setTransportStopShortName(room, 1, "L2", nullptr, result.lift.index), "Platform lift Stop-name edit refused");
			world.resetSimulation();
			require(world.getTransportStopShortName(room, 1, result.lift.index) == "L2", "Platform lift Reset lost Stop names");
		}
	}
}
