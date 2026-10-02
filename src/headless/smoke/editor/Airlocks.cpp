#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "UI.h"
#include "PaletteLayout.h"
#include "core/AirlockTransit.h"
#include "core/YamlSerializer.h"

namespace
{
	void commands(smoke::Context const&)
	{
		using smoke::require;
		auto world = std::make_shared<core::World>("Airlock editor commands", 10, 2);
		world->addRoom("Left", 0, 0, 0, 2, 1); world->addCorridor(0, 0, 5, 2, 1);
		world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		// Same creation and property commands as the palette and Selection panel,
		// through the existing snapshot/history seam (no new test-only interface).
		auto before = captureDocumentSnapshot(world, history);
		auto index = world->addAirlock(0, 0, 2, 3); world->finishBuild();
		commitDocumentEdit(std::move(before), history);
		require(history.undoCount() == 1 && isCanvasSelectableSectorType(core::SectorType::Airlock), "Airlock creation not undoable/selectable");
		require(paletteSlotRow(PaletteSlot::Airlock) == 0, "Airlock missing creation palette surface");
		auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(chamber && chamber->getCapacity() == chamber->getCellsWide() && chamber->getCycleSeconds() == 3,
			"Selection readouts missing derived capacity/default timing");
		auto property = captureDocumentSnapshot(world, history);
		require(world->setAirlockCycleSeconds(index, 10), "Editor timing command refused");
		commitDocumentEdit(std::move(property), history);
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; return world->deserialize(*reader, work);
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Airlock property undo failed");
		chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(chamber && chamber->getCycleSeconds() == 3, "Undo did not restore Airlock default timing");
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->getNumSectors() == 2
			&& world->getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::Wall,
			"Undo creation failed to restore original wall");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Airlock creation redo failed");
		chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(chamber && chamber->getControl(2) && chamber->getDoor(0)->isClosed(), "Redo lost generated devices");
		world->resumeSimulation();
		require(!world->setAirlockCycleSeconds(index, 4), "Running Airlock accepted authored property edit");
	}
}

void editor_smoke::registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/editorCommands", commands });
}
