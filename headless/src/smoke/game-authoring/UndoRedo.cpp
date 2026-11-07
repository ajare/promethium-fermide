#include "Checks.h"
#include "Fixture.h"

#include <memory>
#include <optional>
#include <string>

#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;

	struct PairedHandles
	{
		std::unique_ptr<game::WorldRuntime> runtime;
		std::unique_ptr<game::WorldAuthoring> authoring;
	};

	PairedHandles pairedHandles(std::string const& name)
	{
		auto world = std::make_unique<core::World>(name, 16, 4);
		auto runtime = std::make_unique<game::WorldRuntime>(std::move(world));
		auto authoring = std::make_unique<game::WorldAuthoring>(*runtime);
		return PairedHandles{ std::move(runtime), std::move(authoring) };
	}
}

void registerGameAuthoringUndoRedo(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "undoTakesAnEditBackAndRedoPutsItAgain", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Undo and redo");
			auto& authoring = *fixture.authoring;
			require(!authoring.canUndo(),
				"a handle that has never edited anything can still undo");
			require(!authoring.canRedo(),
				"a handle that has never edited anything can still redo");

			auto const before = authoring.captureSnapshot();
			require(before.has_value(), "the initial document could not be captured");
			authoring.addCorridor(0, 0, 4);
			authoring.commitEdit(before);
			require(authoring.world().getNumSectors() == 1, "the edit was not applied");
			require(authoring.canUndo(), "a committed edit did not become undoable");

			require(authoring.undo(), "undo refused a committed edit");
			require(authoring.world().getNumSectors() == 0, "undo did not take the edit back");
			require(authoring.canRedo(), "undo left nothing to redo");

			require(authoring.redo(), "redo refused the edit that was just undone");
			require(authoring.world().getNumSectors() == 1,
				"redo did not put the edit back");
		} });

	checks.push_back({ "undoRestoresTheWorldTheRuntimeOwnsRatherThanReplacingIt", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("In place");
			auto& authoring = *fixture.authoring;
			auto const* const worldBefore = &authoring.world();

			auto const before = authoring.captureSnapshot();
			authoring.addRoom("Room", 0, 0, 4, 2, 1);
			authoring.commitEdit(before);
			require(authoring.undo(), "undo refused a committed edit");

			// The runtime never handed its World away and never got a new one:
			// the document changed underneath it while the object stayed put,
			// which is what lets a running runtime survive an undo (#968).
			require(&authoring.world() == worldBefore,
				"undo replaced the World object instead of restoring it in place");
			require(&authoring.runtime() == fixture.runtime.get(),
				"the handle lost its runtime across an undo");
			require(&authoring.document() == worldBefore,
				"the read view and the restored World are not the same object");
		} });

	checks.push_back({ "theRestoredWorldIsBuiltAndStillDrives", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Drivable after undo");
			auto& authoring = *fixture.authoring;
			auto const before = authoring.captureSnapshot();
			authoring.addCorridor(0, 0, 4);
			authoring.commitEdit(before);
			require(authoring.undo(), "undo refused a committed edit");

			// The construction-record replay a restore performs ends in
			// finishBuild(), so the runtime can pick the World straight back up.
			require(fixture.runtime->advanceTicks(3),
				"the runtime could not drive the World after it was restored");
			require(fixture.runtime->getSimulationTick() == 3,
				"the restored World did not tick as the runtime drove it");
		} });

	checks.push_back({ "undoAndRedoLeaveTheRunStateAsTheyFoundIt", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Run state");
			auto& authoring = *fixture.authoring;
			fixture.runtime->finishBuild();
			fixture.runtime->pauseSimulation();
			require(fixture.runtime->isSimulationPaused(),
				"the fixture could not be paused");

			auto const before = authoring.captureSnapshot();
			authoring.addCorridor(0, 0, 4);
			authoring.commitEdit(before);
			require(authoring.undo(), "undo refused a committed edit");
			require(fixture.runtime->isSimulationPaused(),
				"undo resumed a simulation that was paused before it");
			require(authoring.redo(), "redo refused the undone edit");
			require(fixture.runtime->isSimulationPaused(),
				"redo resumed a simulation that was paused before it");
		} });

	checks.push_back({ "anEmptyHistoryRefusesBothDirections", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Empty history");
			auto& authoring = *fixture.authoring;
			require(!authoring.undo(), "undo succeeded with nothing in the undo stack");
			require(!authoring.redo(), "redo succeeded with nothing in the redo stack");
			require(authoring.undoDepth() == 0, "the undo stack is not empty");
			require(authoring.redoDepth() == 0, "the redo stack is not empty");
			require(authoring.world().getNumSectors() == 0,
				"a refused undo still changed the document");
		} });

	checks.push_back({ "committingNoSnapshotRecordsNoEdit", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Null snapshot");
			auto& authoring = *fixture.authoring;
			// A failed edit leaves nothing to commit; the call site should not
			// need a special case for that (#968).
			authoring.commitEdit(std::nullopt);
			require(!authoring.canUndo(), "committing no snapshot became an edit");
			require(authoring.undoDepth() == 0, "the undo stack grew on an empty commit");
		} });

	checks.push_back({ "eachAuthoringHandleKeepsItsOwnHistory", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Two handles");
			game::WorldAuthoring first(*fixture.runtime);
			game::WorldAuthoring second(*fixture.runtime);

			first.commitEdit(first.captureSnapshot());
			first.addCorridor(0, 0, 4);
			require(first.canUndo(), "the first handle recorded no edit");
			require(!second.canUndo(),
				"the second handle can undo an edit it did not make");

			// Rolling back through the first handle changes the shared document
			// but not the second handle's stack, which is the point of owning
			// the history rather than sharing a process-wide one.
			require(first.undo(), "the first handle could not undo its own edit");
			require(first.world().getNumSectors() == 0,
				"the first handle's undo did not reach the shared document");
			require(second.world().getNumSectors() == 0,
				"the two handles are not looking at one document");
			require(!second.canUndo(), "the second handle gained an undo entry");
		} });

	checks.push_back({ "clearHistoryDropsBothStacksAndTheSavedMarker", [](smoke::Context const&)
		{
			auto fixture = pairedHandles("Clear history");
			auto& authoring = *fixture.authoring;
			authoring.commitEdit(authoring.captureSnapshot());
			authoring.addCorridor(0, 0, 4);
			authoring.markSaved();
			require(!authoring.isModified(), "markSaved left the document modified");
			require(authoring.canUndo(), "nothing was recorded before clearing");

			authoring.clearHistory();
			require(!authoring.canUndo(), "clearHistory left an undo entry");
			require(!authoring.canRedo(), "clearHistory left a redo entry");
			// Clearing drops the saved marker with the stacks, so the document
			// reads as modified until it is saved again. That is the history's
			// contract, not a leftover: nothing is on disk to be even with.
			require(authoring.isModified(),
				"clearHistory left the document reading as saved with nothing saved");
			authoring.markSaved();
			require(!authoring.isModified(),
				"markSaved after clearHistory did not clear the modified flag");
			require(authoring.world().getNumSectors() == 1,
				"clearHistory touched the document as well as the history");
		} });
}
