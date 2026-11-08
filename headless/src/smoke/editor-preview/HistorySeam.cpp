#include "Checks.h"
#include "../tags/TemporaryDirectory.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "DocumentEdit.h"
#include "DocumentSaveSeam.h"
#include "EditorDocument.h"
#include "TagsPanel.h"
#include "core/World.h"
#include "game/WorldAuthoring.h"

// #975: the authoring handle says it owns the document history, and the
// editor's unsaved-changes prompt has to be asking that same history. The
// bug this pins was not a wrong answer from one of them - it was two
// correct answers from two different objects, with the façade's being the
// one a caller was told to trust. These checks assert object identity, not
// matching behaviour, because matching behaviour is exactly what two
// divergent stacks show right up until they do not.
namespace
{
	using tag_smoke::TemporaryDirectory;
	using smoke::require;

	// Each check opens the document it needs and closes it again, so the
	// single-document invariant is left as it was found.
	struct CloseDocument
	{
		~CloseDocument() { editor::closeDocument(); }
	};

	// The editor's own edit transaction: capture the state the edit is made
	// from, make the edit, commit what was captured. Every editor panel runs
	// this shape, so this is what has to leave both the façade and the prompt
	// dirty together.
	void commitAnEdit(editor::DocumentPtr const& document,
		std::string const& roomName, uint32_t x)
	{
		auto& authoring = document->authoring();
		auto const before = captureDocumentSnapshot(document->world(),
			authoring.history());
		require(before.has_value(),
			"the state an edit is made from could not be captured");
		authoring.world().pauseSimulation();
		authoring.addRoom(roomName, 0, 0, x, 2, 1);
		authoring.finishBuild();
		commitDocumentEdit(before, authoring.history());
	}
}

void registerEditorPreviewHistorySeam(std::vector<smoke::Check>& checks)
{
	checks.push_back({
		"theUnsavedChangesPromptReadsTheAuthoringFaadesHistory",
		[](smoke::Context const& context)
		{
			CloseDocument const closer;
			TemporaryDirectory const temporary(context, "history-seam");
			auto const worldPath = temporary.path / "seam.world.yaml";

			auto const world = editor::openNewWorld("Seam", 8, 2);
			auto const document = editor::currentDocument();
			require(document != nullptr,
				"opening a World left the editor with no document");
			auto& authoring = document->authoring();

			// A document at its saved point, reached the way the editor
			// reaches it: the World written, then the saved marker set.
			authoring.saveTo(worldPath.string());
			authoring.markSaved();
			require(!world->isModified(),
				"saving left the World's own modified flag set");
			require(!authoring.isModified(),
				"a freshly saved document still reported unsaved work");
			require(!editor::documentIsStale(document),
				"a freshly saved document read stale to the prompt");

			// The target the prompt is built from carries the façade's own
			// history. Identity, not equivalence: a second stack that
			// behaved identically for a while is precisely the shape of the
			// bug this check exists to keep out.
			auto const target = editor::currentDocumentSaveTarget(document,
				worldPath.string(), worldPath.string());
			require(target.worldHistory == &authoring.history(),
				"the save target carries a history other than the one the "
				"authoring handle owns");

			// An edit committed through the façade has to be visible to the
			// prompt without anything telling the two about each other.
			commitAnEdit(document, "Added through the façade", 0);
			require(authoring.canUndo(),
				"an edit committed through the façade left nothing to take back");
			require(authoring.isModified(),
				"an edit committed through the façade left it reporting no "
				"unsaved work");
			require(editor::documentIsStale(document),
				"the unsaved-changes prompt read clean while the façade "
				"reported unsaved work");
			require(unsavedDocumentPromptText(target).find("World:")
				!= std::string::npos,
				"the unsaved-changes prompt did not list the dirty World "
				"document");

			// Settling the save settles both, because they were never two
			// separate markers to settle: the World's own flag and the
			// history's saved marker are read off the same target and both
			// come back clean.
			authoring.saveTo(worldPath.string());
			authoring.markSaved();
			require(!authoring.isModified(),
				"markSaved left the façade reporting unsaved work");
			require(!editor::documentIsStale(document),
				"the prompt still read stale after the saved marker was set");
			require(unsavedDocumentPromptText(target).find("World:")
				== std::string::npos,
				"the saved document still listed as unsaved in the prompt");
		} });

	checks.push_back({
		"theHistoryCrossesTheUndoAdoptionWithTheDocument",
		[](smoke::Context const&)
		{
			CloseDocument const closer;
			auto const world = editor::openNewWorld("Carry", 8, 2);
			auto document = editor::currentDocument();
			require(document != nullptr,
				"opening a World left the editor with no document");
			// The handle being taken over is held by its own shared_ptr for the
			// whole check: the adoption below replaces the current document,
			// and the history is moved out of the handle the old document
			// owns, so that document has to outlive the handover.
			auto const discarded = document;
			auto& authoring = discarded->authoring();
			authoring.markSaved();

			// Two edits, so that taking one back still leaves the document
			// away from its saved point and there is something to redo.
			commitAnEdit(document, "First edit", 0);
			commitAnEdit(document, "Second edit", 3);
			require(authoring.undoDepth() == 2,
				"two committed edits did not put two steps on the undo stack");
			require(editor::documentIsStale(document),
				"the prompt read clean with committed edits outstanding");

			// The editor's undo re-materialises the document over a rebuilt
			// World - a new runtime, and so a new authoring handle - because
			// a runtime owns the World it was built with for its whole life
			// (#971). The history belongs to the document, so it has to
			// arrive on the far side with it rather than stay behind with
			// the handle being discarded.
			auto const current = captureDocumentSnapshot(discarded->world(),
				authoring.history());
			require(current.has_value(),
				"the state being left could not be captured for the undo");
			std::unique_ptr<core::World> rebuilt;
			auto restore = [&](DocumentSnapshot const& snapshot)
			{
				rebuilt = deserializeDocumentSnapshot(snapshot,
					discarded->world(), {});
				return rebuilt != nullptr;
			};
			require(authoring.undo(current, restore),
				"the undo was refused");
			editor::adoptWorld(std::move(rebuilt));
			document = editor::currentDocument();
			require(document != nullptr,
				"the adoption left the editor with no document");
			document->authoring().adoptHistoryFrom(authoring);

			require(&document->authoring() != &authoring,
				"the adopted document answered with the discarded handle");
			require(document->authoring().undoDepth() == 1,
				"the undo stack did not cross the adoption with the document");
			require(document->authoring().redoDepth() == 1,
				"the redo stack did not cross the adoption with the document");
			require(document->authoring().isModified(),
				"the carried history lost the unsaved marker at the adoption");
			require(editor::documentIsStale(document),
				"the prompt lost the unsaved work at the adoption");
			require(authoring.undoDepth() == 0
				&& authoring.redoDepth() == 0,
				"the discarded handle kept the document's history");

			// And the carried stack is live on the new handle, not a copy of
			// a picture of the old one: redoing through the new document
			// moves its own World and settles its own marker.
			auto const beforeRedo = captureDocumentSnapshot(
				document->world(), document->authoring().history());
			require(beforeRedo.has_value(),
				"the state being left could not be captured for the redo");
			std::unique_ptr<core::World> redone;
			auto redoRestore = [&](DocumentSnapshot const& snapshot)
			{
				redone = deserializeDocumentSnapshot(snapshot,
					document->world(), {});
				return redone != nullptr;
			};
			require(document->authoring().redo(beforeRedo, redoRestore),
				"the redo was refused on the carried history");
			auto const handedOff = document;
			editor::adoptWorld(std::move(redone));
			document = editor::currentDocument();
			document->authoring().adoptHistoryFrom(handedOff->authoring());
			require(document->authoring().undoDepth() == 2,
				"the redo did not put the step back on the new handle's "
				"undo stack");
			require(document->authoring().redoDepth() == 0,
				"the redo left a redo step behind on the new handle");
			require(document->authoring().isModified(),
				"redoing to an unsaved state reported the document saved");
			require(editor::documentIsStale(document),
				"the prompt read clean after the redo brought unsaved work back");
		} });
}
