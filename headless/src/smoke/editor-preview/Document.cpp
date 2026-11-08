#include "Checks.h"
#include "../game-runtime/Fixture.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include "DocumentEdit.h"
#include "EditorDocument.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

namespace
{
	using game_runtime_smoke::makeWalkingWorld;
	using smoke::require;

	// Each check opens the document it needs and closes it again, so the
	// single-document invariant is left as it was found.
	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};

	// The restore the editor's Undo / Redo performs (#973): rebuild the
	// selected snapshot into a detached World and adopt it as the editor's
	// document, because the runtime owns the World it was built with for its
	// whole life. Both the view the UI holds and the pointer the rebuilt World
	// had are reported, so a check can require the adopted World to be the
	// live rebuilt object rather than memory the loader had already freed.
	struct RestoredAdoption
	{
		std::shared_ptr<core::World> view;
		core::World const* rebuilt{ nullptr };
	};

	RestoredAdoption restoreIntoDocument(DocumentHistory& history,
		std::shared_ptr<core::World> const& current, bool redo)
	{
		auto const snapshot = captureDocumentSnapshot(current, history);
		if (!snapshot)
			throw smoke::Failure(
				"the current document state could not be captured");
		std::unique_ptr<core::World> rebuilt;
		auto restore = [&](DocumentSnapshot const& target)
		{
			rebuilt = deserializeDocumentSnapshot(target, current, {});
			return rebuilt != nullptr;
		};
		bool const applied = redo ? history.redo(snapshot, restore)
			: history.undo(snapshot, restore);
		if (!applied)
			throw smoke::Failure(redo ? "the redo was refused" : "the undo was refused");
		auto const* const rebuiltPointer = rebuilt.get();
		return RestoredAdoption{ editor::adoptWorld(std::move(rebuilt)),
			rebuiltPointer };
	}
}

void registerEditorPreviewDocument(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "theEditorHoldsNoWorldOutsideItsRuntime", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Preview", 8, 2);
			auto const document = editor::currentDocument();
			require(document != nullptr,
				"opening a World left the editor with no document");
			// The World the UI holds and the World the runtime drives are one
			// object. There is no second World anywhere in the arrangement.
			require(world.get() == &document->runtime().world(),
				"the World the editor holds is not the World its runtime drives");
			require(&document->authoring().world() == &document->runtime().world(),
				"the authoring handle reaches a different World than the runtime drives");
		} });

	checks.push_back({ "theRuntimeIsTakenFromTheAuthoringHandle", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Authoring route", 8, 2);
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document after opening a World");
			require(&document->runtime() == &document->authoring().runtime(),
				"the document's runtime is not the one its authoring handle wraps");
			require(&editor::previewRuntime(*world) == &document->runtime(),
				"previewRuntime does not resolve the open document's runtime");
		} });

	checks.push_back({ "previewRuntimeRefusesAWorldItDoesNotDrive", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Open", 8, 2);
			require(world != nullptr, "no World after opening a document");
			core::World foreign("Foreign", 4, 1);
			bool refused = false;
			try
			{
				(void)editor::previewRuntime(foreign);
			}
			catch (std::logic_error const&)
			{
				refused = true;
			}
			require(refused,
				"a World the editor does not own was accepted as its preview");
		} });

	checks.push_back({ "previewRuntimeRefusesWhenNothingIsOpen", [](smoke::Context const&)
		{
			editor::closeDocument();
			core::World loose("Loose", 4, 1);
			bool refused = false;
			try
			{
				(void)editor::previewRuntime(loose);
			}
			catch (std::logic_error const&)
			{
				refused = true;
			}
			require(refused,
				"a preview was driven with no document open");
		} });

	// The accepting path of the editor's adoption (#973). Every loaded-World
	// action the editor has - File -> Open World and Undo / Redo restore -
	// hands over a World nobody else holds, and that sole-owner handover is
	// what the deleted `shared_ptr` overload destroyed: it built a
	// `unique_ptr` from the `shared_ptr`'s raw pointer and then released the
	// caller's reference, so the last-owner release ran the destructor and
	// the runtime adopted freed memory. The suite covered only the refusal.
	checks.push_back({ "adoptingASoleOwnedWorldLeavesItLive", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Adopted");
			auto const* const handedOver = fixture.world.get();
			auto const view = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "adopting a World left no document open");
			// Same object, still alive, still holding what was built into it.
			// These reads are the liveness proof: under a poisoning allocator a
			// freed World answers with filler instead, so passing them here is
			// not a matter of the block not having been reused yet.
			require(view.get() == handedOver,
				"the adopted view is not the World that was handed over");
			require(&document->runtime().world() == handedOver,
				"the runtime does not drive the World that was adopted");
			require(view->getName() == "Adopted",
				"the adopted World no longer names the World handed over");
			require(view->getNumSectors() == 1,
				"the adopted World lost the topology it was built with");
			require(document->runtime().world().getName() == "Adopted",
				"the runtime's own read of the adopted World is not live");
			require(&editor::previewRuntime(*view) == &document->runtime(),
				"the adopted World does not resolve the runtime that owns it");
		} });

	// File -> Open World goes through the real loader, so the accepting path is
	// exercised with a document on disk rather than a hand-built stand-in.
	checks.push_back({ "openingALoadedWorldAdoptsTheLoadedWorldLive",
		[](smoke::Context const& context)
		{
			CloseDocument const closer;
			auto const documentPath = context.temporaryRoot() / "opened.world.yaml";
			{
				auto authored = std::make_unique<core::World>("Opened", 7, 2);
				auto const corridor = authored->addCorridor(0, 0, 6);
				authored->addSectorMarker(corridor, 0, 0.5f);
				authored->finishBuild();
				authored->saveTo(documentPath.string());
			}
			auto loaded = core::loadWorldDocument(documentPath);
			require(loaded != nullptr, "the World document could not be loaded");
			auto const* const loadedPointer = loaded.get();
			auto const view = editor::adoptWorld(std::move(loaded));
			require(view.get() == loadedPointer,
				"the adopted World is not the World that was loaded");
			require(view->getName() == "Opened",
				"the adopted World does not read back as the document it came from");
			auto const document = editor::currentDocument();
			require(&document->runtime().world() == loadedPointer,
				"the editor's runtime does not drive the loaded World");
			// Freed memory cannot be driven. This is the read the preview makes
			// every frame after an open.
			document->runtime().update(1.0f);
			require(document->runtime().getSimulationTick() == 60,
				"the adopted World's runtime could not be driven");
		} });

	// Undo / Redo restore, end to end: author an edit through the document's
	// handle, undo into a rebuilt World adopted as a new document, then redo
	// the same way. Both rebuilds arrive sole-owned and both have to come out
	// the other side live.
	checks.push_back({ "undoAndRedoRestoreAdoptTheRestoredWorldLive", [](smoke::Context const&)
		{
			CloseDocument const closer;
			DocumentHistory history;
			history.markSaved();
			auto fixture = makeWalkingWorld("Restorable");
			auto const opened = editor::adoptWorld(std::move(fixture.world));
			auto const document = editor::currentDocument();
			require(document != nullptr, "no document before the edit");
			auto const sectorsBefore = opened->getNumSectors();

			// The editor's own transaction: capture the state the edit is made
			// from, make the edit, then commit what was captured.
			auto const preEdit = captureDocumentSnapshot(opened, history);
			require(preEdit.has_value(),
				"the pre-edit state could not be captured");
			document->runtime().pauseSimulation();
			document->authoring().addRoom("Added after opening", 1, 0, 0, 2, 1);
			document->authoring().finishBuild();
			commitDocumentEdit(preEdit, history);
			require(opened->getNumSectors() == sectorsBefore + 1,
				"the authored Room did not reach the open World");
			require(history.canUndo(), "an authored edit left nothing to undo");

			auto const undone = restoreIntoDocument(history, opened, false);
			require(undone.rebuilt != nullptr && undone.view != nullptr,
				"the undo adopted nothing");
			require(undone.view.get() == undone.rebuilt,
				"the World adopted after the undo is not the World that was rebuilt");
			require(undone.view->getName() == "Restorable",
				"the restored World does not read back as the snapshot it came from");
			require(undone.view->getNumSectors() == sectorsBefore,
				"the undo did not restore the pre-edit topology");
			auto const afterUndo = editor::currentDocument();
			require(&afterUndo->runtime().world() == undone.rebuilt,
				"the document's runtime does not drive the restored World");
			afterUndo->runtime().update(1.0f);
			require(afterUndo->runtime().getSimulationTick() == 60,
				"the restored World's runtime could not be driven");

			require(history.canRedo(), "the undo left nothing to redo");
			auto const redone = restoreIntoDocument(history, undone.view, true);
			require(redone.view.get() == redone.rebuilt,
				"the World adopted after the redo is not the World that was rebuilt");
			require(redone.view->getNumSectors() == sectorsBefore + 1,
				"the redo lost the Room the undo had removed");
			require(&editor::currentDocument()->runtime().world() == redone.rebuilt,
				"the document's runtime does not drive the redone World");
			require(redone.view->getName() == "Restorable",
				"the redone World does not read back as the snapshot it came from");
		} });

	// Teardown after adoption (#973). The adopted World is deleted once, when
	// the last holder of the runtime lets go. The deleted overload produced a
	// second delete here; the allocator aborts on that before the last line of
	// this check is ever reached, so arriving is the assertion.
	checks.push_back({ "adoptionTeardownFreesTheAdoptedWorldOnce", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto fixture = makeWalkingWorld("Teardown");
			auto const* const handedOver = fixture.world.get();
			auto view = editor::adoptWorld(std::move(fixture.world));
			require(view.get() == handedOver,
				"the adopted view drifted from the World handed over");
			editor::closeDocument();
			require(editor::currentDocument() == nullptr,
				"the document is still open after closeDocument()");
			// The view shares the runtime's control block rather than the
			// World's ownership, so the World it names outlives the document
			// and still is that World.
			require(view->getName() == "Teardown",
				"the held World stopped naming the closed document's World");
			require(view.get() == handedOver,
				"the held view stopped pointing at the adopted World");
			view.reset();
			require(editor::currentDocument() == nullptr,
				"letting go of the last World view reopened a document");
		} });

	checks.push_back({ "theWorldViewKeepsTheRuntimeAliveWhenTheDocumentCloses",
		[](smoke::Context const&)
		{
			auto world = editor::openNewWorld("Closing", 4, 1);
			auto& driven = editor::previewRuntime(*world);
			driven.update(1.0f);
			require(driven.getSimulationTick() == 60,
				"the preview did not advance before the document closed");
			editor::closeDocument();
			require(editor::currentDocument() == nullptr,
				"the document is still open after closeDocument()");
			// The view shares the runtime's control block rather than the
			// World's ownership, so the World it names is still alive and
			// still the one that was driven.
			require(world->getName() == "Closing",
				"the held World view stopped naming the closed document's World");
			require(world.get() != nullptr, "the held World view went null");
			world.reset();
		} });

	checks.push_back({ "replacingTheDocumentReplacesTheRuntime", [](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const first = editor::openNewWorld("First", 4, 1);
			auto const* const firstRuntime = &editor::previewRuntime(*first);
			auto const second = editor::openNewWorld("Second", 4, 1);
			auto const* const secondRuntime = &editor::previewRuntime(*second);
			require(firstRuntime != secondRuntime,
				"opening a second document kept driving the first document's runtime");
			require(editor::previewRuntime(*second).getSimulationTick() == 0,
				"the new document's runtime carried the old document's tick");
			bool staleRefused = false;
			try
			{
				(void)editor::previewRuntime(*first);
			}
			catch (std::logic_error const&)
			{
				staleRefused = true;
			}
			require(staleRefused,
				"a stale World view still resolved a runtime to drive");
		} });
}
