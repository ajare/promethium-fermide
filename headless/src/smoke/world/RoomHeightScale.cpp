#include "Checks.h"
#include "core/World.h"
#include "core/Location.h"
#include <cmath>

namespace
{
	using smoke::require;

	void roomHeightScale(smoke::Context const&)
	{
		core::World world("Room height scale", 12, 4);
		auto room = world.addRoom("Low room", 0, 0, 0, 3, 1);
		auto corridor = world.addCorridor(0u, 0u, 3u, 2u, 1u);
		auto facade = world.addFacade(0, 0, 5, 3, 1, 0.9f);
		world.finishBuild();

		auto roomSector = std::dynamic_pointer_cast<const core::Location>(world.getSector(room));
		require(roomSector && roomSector->isRoom(), "Room was not recognised as a Room");
		require(roomSector->getLevelsHigh() == 1, "Fixture Room is not one cell high");

		// Authored before the simulation is paused, exactly as the editor would.
		world.pauseSimulation();

		require(world.setRoomHeightScale(room, 0.4f), "Room height scale was refused");
		require(roomSector->getHeightScale() == std::optional<float>{0.4f}, "Room scale was not stored");
		require(std::abs(roomSector->getEffectiveTopLevelHeight() - 0.9f * 0.4f) < 0.0001f,
			"Effective height did not scale the standard Room height");
		require(std::abs(roomSector->getLevelHeight(0) - 0.9f * 0.4f) < 0.0001f,
			"Level height did not reflect the effective height");

		// Clearing restores the authored top-level height.
		require(world.setRoomHeightScale(room, std::nullopt), "Clearing the Room scale was refused");
		require(!roomSector->getHeightScale(), "Cleared Room scale was still present");
		require(std::abs(roomSector->getEffectiveTopLevelHeight() - roomSector->getTopLevelHeight()) < 0.0001f,
			"Cleared Room did not restore its authored height");

		// Out-of-range, non-finite, and sub-floor scales are refused.
		std::string diagnostic;
		require(!world.setRoomHeightScale(room, 0.0f, &diagnostic), "Zero Room scale accepted");
		require(!world.setRoomHeightScale(room, 0.19f, &diagnostic), "Sub-floor Room scale accepted");
		require(!world.setRoomHeightScale(room, 1.01f, &diagnostic), "Over-range Room scale accepted");
		require(!world.setRoomHeightScale(room, std::nanf(""), &diagnostic), "NaN Room scale accepted");

		// The override only applies to Rooms.
		require(!world.setRoomHeightScale(corridor, 0.5f, &diagnostic), "Corridor accepted a height scale");
		require(!world.setRoomHeightScale(facade, 0.5f, &diagnostic), "Facade accepted a height scale");

		// A multi-cell Room refuses the override entirely.
		auto tall = world.addRoom("Tall room", 0, 0, 9, 3, 2);
		world.finishBuild();
		require(!world.setRoomHeightScale(tall, 0.5f, &diagnostic), "Multi-level Room accepted a height scale");

		// Resizing a one-cell Room to more than one cell removes the override.
		require(world.setRoomHeightScale(room, 0.4f), "Room scale refused before resize");
		auto plan = world.planResizeLocation(room, 0, 0, 3, 2);
		require(plan.valid, "Room resize plan was refused");
		auto const newRoom = world.applyLocationEdit(plan);
		auto resized = std::dynamic_pointer_cast<const core::Location>(world.getSector(newRoom));
		require(resized && resized->getLevelsHigh() == 2, "Room resize did not take effect");
		require(!resized->getHeightScale(), "Resize to two cells retained the height override");
		require(std::abs(resized->getEffectiveTopLevelHeight() - resized->getTopLevelHeight()) < 0.0001f,
			"Resized Room did not return to its authored height");
	}

	void roomHeightScaleRequiresPause(smoke::Context const&)
	{
		core::World world("Room height scale pause", 8, 2);
		auto room = world.addRoom("Room", 0, 0, 0, 4, 1);
		world.finishBuild();
		std::string diagnostic;
		require(!world.setRoomHeightScale(room, 0.5f, &diagnostic),
			"Height scale was accepted while the simulation was running");
	}
}

void registerRoomHeightScale(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "room-height-scale", roomHeightScale });
	checks.push_back({ "room-height-scale-requires-pause", roomHeightScaleRequiresPause });
}
