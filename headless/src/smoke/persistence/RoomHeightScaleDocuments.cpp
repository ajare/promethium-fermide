#include "WorldChecks.h"
#include "core/World.h"
#include "core/Location.h"
#include "core/YamlSerializer.h"
#include <cmath>
#include <optional>

namespace persistence
{
	using smoke::require;

	namespace
	{
		std::shared_ptr<const core::Location> room(core::World const& world, uint32_t index)
		{
			return std::dynamic_pointer_cast<const core::Location>(world.getSector(index));
		}
	}

	void roomHeightScaleRoundTrips(smoke::Context const&)
	{
		core::World original("Low Room", 8, 2);
		auto roomIndex = original.addRoom("Low room", 0, 0, 0, 7, 1);
		original.finishBuild();
		original.pauseSimulation();
		require(original.setRoomHeightScale(roomIndex, 0.4f), "Room height scale was refused");

		core::SerializationWorkData workData;
		auto writer = core::YamlSerializer::toString();
		original.serialize(*writer, workData);
		writer->serialize();
		auto yaml = writer->getSerializedString();
		require(yaml.find("hasHeightScale: true") != std::string::npos
			&& yaml.find("heightScale:") != std::string::npos,
			"World YAML did not persist the Room height scale");

		core::World loaded("placeholder", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		require(loaded.deserialize(*reader, workData), "Low Room world did not round-trip");
		auto loadedRoom = room(loaded, roomIndex);
		require(loadedRoom && loadedRoom->getHeightScale() == std::optional<float>{0.4f},
			"Loaded Room lost its height scale");
		require(std::fabs(loadedRoom->getEffectiveTopLevelHeight() - 0.9f * 0.4f) < 0.001f,
			"Loaded Room did not apply its effective height");

		// Clearing the field persists an absent override, not a defaulted one.
		auto heightAt = yaml.find("hasHeightScale: true");
		require(heightAt != std::string::npos, "No Room height scale marker in YAML");
		yaml.replace(heightAt, std::string("hasHeightScale: true").size(), "hasHeightScale: false");
		auto heightValue = yaml.find("    heightScale:");
		if (heightValue != std::string::npos)
			yaml.erase(heightValue, yaml.find('\n', heightValue) - heightValue + 1);

		core::World cleared("placeholder", 1, 1);
		auto clearedReader = core::YamlSerializer::fromString(yaml);
		clearedReader->deserialize();
		require(cleared.deserialize(*clearedReader, workData), "Cleared Room scale document did not load");
		require(!room(cleared, roomIndex)->getHeightScale(), "Cleared Room scale came back set");
	}
}
