#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "core/Location.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include <memory>
#include <string>

namespace
{
	void occupiedPoseHistory(smoke::Context const& context)
	{
		editor_smoke::State state;
		using smoke::require;
		auto world = std::make_shared<core::World>("Occupied edit", 8, 2);
		auto room = world->addRoom("Robot room", 0, 0, 0, 7, 1);
		world->finishBuild();
		world->pauseSimulation();
		auto robot = core::resolveAgentTypeResource("standing-robot.agent.lua");
		require(robot && world->attachAgentType(robot->resourceName, robot->source), "History robot resource refused");
		auto id = world->createAgent("StandingRobot", "Robot", room, 0, 2.5f);
		require(world->setRoomHeightScale(room, .4f), "Robot exact placement fit refused");
		world->saveTo((context.temporaryRoot() / "occupied-height.world.yaml").string());
		gWorldDocumentHistory.clear();
		auto before = captureDocumentSnapshot(world);
		require(before.has_value(), "Occupied history snapshot unavailable");
		std::string diagnostic;
		if (world->setRoomHeightScale(room, .3f, &diagnostic)) commitDocumentEdit(before);
		require(!diagnostic.empty() && !world->isModified() && gWorldDocumentHistory.undoCount() == 0
			&& captureDocumentSnapshot(world)->yaml == before->yaml
			&& world->lookupAgent(id).entity->getPose() == core::Pose::Standing,
			"Refused occupied edit changed geometry, history, dirty state or pose");
		require(world->setRoomHeightScale(room, .5f), "Fitting occupied edit refused");
		commitDocumentEdit(std::move(before));
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml);
			reader->deserialize();
			core::SerializationWorkData work;
			auto ok = world->deserialize(*reader, work);
			world->pauseSimulation();
			return ok;
		};
		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
			&& world->lookupAgent(id).entity->getPose() == core::Pose::Standing, "Occupied height undo failed");
		require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore)
			&& world->lookupAgent(id).entity->getPose() == core::Pose::Standing, "Occupied height redo failed");
	}

	void roomHeightScaleHistory(smoke::Context const&)
	{
		editor_smoke::State state;
		using smoke::require;

		auto world = std::make_shared<core::World>("Room height scale", 8, 2);
		auto room = world->addRoom("Low room", 0, 0, 0, 7, 1);
		world->finishBuild();
		world->pauseSimulation();
		gWorldDocumentHistory.clear();

		auto before = captureDocumentSnapshot(world);
		require(world->setRoomHeightScale(room, 0.4f), "Room height scale edit was refused");
		commitDocumentEdit(std::move(before));
		require(gWorldDocumentHistory.undoCount() == 1, "Room height scale edit missing history");

		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto reader = core::YamlSerializer::fromString(snapshot.yaml);
			reader->deserialize();
			core::SerializationWorkData work;
			bool ok = world->deserialize(*reader, work);
			world->pauseSimulation();
			return ok;
		};

		require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore),
			"Room height scale undo failed");
		require(!std::dynamic_pointer_cast<const core::Location>(world->getSector(room))->getHeightScale(),
			"Undo retained the Room height scale");

		require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore),
			"Room height scale redo failed");
		auto roomLocation = std::dynamic_pointer_cast<const core::Location>(world->getSector(room));
		require(roomLocation->getHeightScale() == std::optional<float>{0.4f},
			"Redo lost the Room height scale");
	}
}

namespace editor_smoke
{
	void registerRoomHeightScale(std::vector<smoke::Check>& checks)
	{
		checks.push_back({ "roomHeightScale/history", roomHeightScaleHistory });
		checks.push_back({ "roomHeightScale/occupiedPoseHistory", occupiedPoseHistory });
	}
}
