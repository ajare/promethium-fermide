#include "WorldChecks.h"
#include "core/World.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <yaml-cpp/yaml.h>

namespace persistence
{
	void boothWindows(smoke::Context const&)
	{
		using smoke::require;
		core::World world("BoothWindow document", 10, 3); world.addLayer();
		for (uint32_t layer = 0; layer < 3; ++layer) world.addRoom("Room", layer, 0, 0, 9, 2);
		world.addBoothWindow(0, 0, 2); world.addBoothWindow(1, 0, 4, core::Window::State::Open); world.finishBuild();
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
						require(object->getObjectType() == core::SectorObjectType::BoothWindow && booth->getFrontLayer() == p[0]
							&& booth->getBackLayer() == p[0] + 1 && static_cast<uint32_t>(booth->getState()) == p[2]
							&& !booth->isTraversalConfigured(), "Authored BoothWindow lost kind/pair/state");
					}
				require(found, "Authored BoothWindow missing");
			}
			require(source.getSimulationSnapshot().traversalResources.empty() && source.getSimulationSnapshot().interactionPoints.empty(), "Document created crossing/panel resources");
		};
		for (bool binary : {false,true})
		{
			auto data = write(world, binary);
			std::unique_ptr<core::Serializer> reader = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(data))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(data));
			reader->deserialize(); core::SerializationWorkData work; core::World loaded("Loaded", 1, 1);
			require(loaded.deserialize(*reader, work), "BoothWindow round trip refused"); assertAuthored(loaded);
			loaded.resetSimulation(); assertAuthored(loaded); loaded.pauseSimulation(); loaded.addLayer(); assertAuthored(loaded);
		}
		auto node = YAML::Load(write(world, false));
		require(node["version"].as<int>() == 43, "BoothWindow schema not allocated");
		core::SerializationWorkData work;
		for (auto change : {"width", "height", "state", "glass", "traversal", "broken", "layer", "position", "legacy", "unknown"})
		{
			auto invalid = YAML::Clone(node);
			for (auto record : invalid["construction"]) if (record["type"].as<std::string>() == "boothWindow")
			{
				std::string c = change;
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
		auto malformedBinary = write(world,true);
		auto stateOffset = malformedBinary.find("closed");
		require(stateOffset != std::string::npos, "Binary fixture missing shutter state");
		malformedBinary.replace(stateOffset, 6, "broken");
		bool binaryRefused = false; auto baseline = write(world,false);
		try { auto input = core::BinarySerializer::fromString(malformedBinary); input->deserialize(); world.deserialize(*input, work); }
		catch (std::exception const&) { binaryRefused = true; }
		require(binaryRefused && write(world,false) == baseline, "Malformed binary BoothWindow accepted or mutated target");
		auto legacy = YAML::Clone(node); legacy["version"] = 42;
		YAML::Node records(YAML::NodeType::Sequence);
		for (auto record : legacy["construction"]) if (record["type"].as<std::string>() != "boothWindow") records.push_back(record);
		legacy["construction"] = records;
		auto reader = core::YamlSerializer::fromString(YAML::Dump(legacy)); reader->deserialize();
		core::World loaded("Legacy",1,1); require(loaded.deserialize(*reader,work), "Legacy document no longer compatible");
	}
}
