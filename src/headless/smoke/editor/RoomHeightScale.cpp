#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "core/Location.h"
#include "core/World.h"
#include <memory>
#include <string>

namespace
{
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
	}
}
