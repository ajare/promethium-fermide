#include "Checks.h"
#include "State.h"
#include "DocumentEdit.h"
#include "UI.h"
#include "PaletteLayout.h"
#include "PermissionsPanel.h"
#include "core/AirlockTransit.h"
#include "core/YamlSerializer.h"

namespace
{
	void structuralHistory(smoke::Context const&)
	{
		using smoke::require;
		auto world = std::make_shared<core::World>("Airlock edit history", 12, 3);
		for (uint32_t y = 0; y < 3; ++y)
		{
			world->addRoom("Left", 0, y, 0, 2, 1);
			world->addCorridor(0, y, y == 2 ? 4 : 5, 2, 1);
		}
		auto index = world->addAirlock(0, 0, 2, 3, 8); world->finishBuild(); world->pauseSimulation();
		DocumentHistory history;
		auto edit = [&](core::World::AirlockEditPlan const& plan) {
			require(plan.valid, "Editor structural command refused");
			auto before = captureDocumentSnapshot(world, history);
			index = world->applyAirlockEdit(plan);
			commitDocumentEdit(std::move(before), history);
		};
		edit(world->planResizeAirlock(index, 2, 1, 3));
		edit(world->planResizeAirlock(index, 2, 2, 2));
		edit(world->planRemoveAirlock(index));
		require(history.undoCount() == 3 && world->getNumSectors() == 6, "Structural edits missing history/deletion");
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; return world->deserialize(*reader, work);
		};
		for (uint32_t row : { 2u, 1u, 0u })
		{
			require(history.undo(captureDocumentSnapshot(world, history), restore), "Structural undo failed");
			auto chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(6));
			require(chamber && chamber->getCellY() == row && chamber->getCycleSeconds() == 8
				&& chamber->getCapacity() == (row == 2 ? 2u : 3u), "Structural undo geometry/configuration mismatch");
		}
		for (unsigned count = 0; count < 3; ++count)
			require(history.redo(captureDocumentSnapshot(world, history), restore), "Structural redo failed");
		require(world->getNumSectors() == 6 && world->getSimulationSnapshot().interactionPoints.empty(), "Redo orphaned Airlock controls");
	}

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
		auto key = world->addAccessPermission("Outside operator");
		auto property = captureDocumentSnapshot(world, history);
		require(world->setAirlockCycleSeconds(index, 10), "Editor timing command refused");
		std::string diagnostic;
		require(commitInteractionPermissionRequirement(world, chamber->getControl(0), key, true, diagnostic)
			&& commitInteractionPermissionRequirement(world, chamber->getControl(1), key, true, diagnostic), "Outside permission editor command refused");
		require(world->getSimulationSnapshot().interactionPoints.size() == 2, "Editor retained internal button");
		commitDocumentEdit(std::move(property), history);
		auto restore = [&](DocumentSnapshot const& snapshot) {
			auto reader = core::YamlSerializer::fromString(snapshot.yaml); reader->deserialize();
			core::SerializationWorkData work; return world->deserialize(*reader, work);
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Airlock property undo failed");
		chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(chamber && chamber->getCycleSeconds() == 3
			&& world->getInteractionPointPermissionRequirement(chamber->getControl(0)).empty()
			&& world->getInteractionPointPermissionRequirement(chamber->getControl(1)).empty(), "Undo did not restore Airlock properties");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Permission redo failed");
		chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(world->getInteractionPointPermissionRequirement(chamber->getControl(0)) == std::vector<core::AccessPermissionId>{ key }
			&& world->getInteractionPointPermissionRequirement(chamber->getControl(1)) == std::vector<core::AccessPermissionId>{ key }, "Redo lost outside requirements");
		require(history.undo(captureDocumentSnapshot(world, history), restore), "Second property undo failed");
		require(history.undo(captureDocumentSnapshot(world, history), restore) && world->getNumSectors() == 2
			&& world->getSector(0)->getEndType(0, CORE_SIDE_RIGHT) == core::SectorEndType::Wall,
			"Undo creation failed to restore original wall");
		require(history.redo(captureDocumentSnapshot(world, history), restore), "Airlock creation redo failed");
		chamber = std::dynamic_pointer_cast<const core::AirlockTransit>(world->getSector(index));
		require(chamber && chamber->getControl(0) && chamber->getControl(1)
			&& chamber->getNumObjects() == 2 && chamber->getDoor(0)->isClosed(), "Redo lost generated devices");
		world->resumeSimulation();
		require(!world->setAirlockCycleSeconds(index, 4), "Running Airlock accepted authored property edit");
	}
}

void editor_smoke::registerAirlocks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "airlocks/structuralHistory", structuralHistory });
	checks.push_back({ "airlocks/editorCommands", commands });
}
