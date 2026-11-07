#include "Checks.h"
#include "DocumentEdit.h"
#include "PermissionsPanel.h"
#include "core/World.h"
#include "core/YamlSerializer.h"

namespace editor_smoke
{
	void registerTransportStopNames(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"transportStopNames/editorHistory", [](smoke::Context const&) {
			using smoke::require;
			for (bool shuttle : {false, true})
			{
				auto world = std::make_shared<core::World>("Stop-name history", 32, 3);
				for (uint32_t level = 0; level < 3; ++level) world->addCorridor(level, 0, 32);
				auto sector = shuttle ? world->addShuttle(1, 0, 0, 24, {1, 3, {0, 8, 16}, 0}).shuttle.sector->getIndex()
					: world->addLift(1, 0, 2, 1, 3).lift.sector->getIndex();
				world->finishBuild(); world->pauseSimulation(); gWorldDocumentHistory.clear();
				std::string diagnostic;
				require(commitTransportStopShortName(world, sector, 1, "B2", diagnostic), diagnostic);
				require(world->getTransportStopShortName(sector, 1) == "B2" && gWorldDocumentHistory.undoCount() == 1,
					"Editor Stop-name command must create an undoable edit");
				for (auto const& invalid : {"", "abcd", "abc", "A-1"})
					require(!commitTransportStopShortName(world, sector, 1, invalid, diagnostic)
						&& gWorldDocumentHistory.undoCount() == 1 && world->getTransportStopShortName(sector, 1) == "B2",
						"Invalid Stop-name edit must not mutate the document/history");
				require(!commitTransportStopShortName(world, sector, 1, "B2", diagnostic)
					&& gWorldDocumentHistory.undoCount() == 1, "Unchanged Stop name must not create history");
				auto restore = [&](DocumentSnapshot const& snapshot) {
					auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
					core::SerializationWorkData data; return world->deserialize(*reader, data);
				};
				require(gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore)
					&& world->getTransportStopShortName(sector, 1) == "1", "Undo must restore the default Stop label");
				require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore)
					&& world->getTransportStopShortName(sector, 1) == "B2", "Redo must restore the authored Stop label");
				world->resumeSimulation();
				require(!commitTransportStopShortName(world, sector, 1, "NEW", diagnostic)
					&& gWorldDocumentHistory.undoCount() == 1, "Running Stop-name edit must be refused without history");
			}
			gWorldDocumentHistory.clear();
		}});
	}
}
