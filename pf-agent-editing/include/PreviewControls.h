#pragma once

// The editor's simulation controls, spelled without ImGui (#979).
//
// The toolbar's Start/Stop and Reset buttons and the global S and R
// shortcuts all drive the same two operations on the open document's
// preview. Each one used to resolve a World for itself and open with a
// ternary of the shape
//
//	auto const world = document ? document->world() : nullptr;
//	auto& runtime = editor::previewRuntime(*world);
//
// which is a guard that manufactures the null it appears to be guarding
// against: `previewRuntime` takes a reference, so with no document open
// the ternary hands it `*nullptr` and the reference binds to nothing
// before any check the function itself makes can run.
//
// "No document open" is decided once here instead, and the code says it:
// there is nothing to control, so the request is refused and nothing
// changes. `handled` reports whether there was a live document to act on,
// which is how the caller knows to leave its own paused indicator alone
// rather than record a state that was never applied. A handle to a
// document the editor has since replaced is refused the same way - it is
// the same nothing-to-control answer, not a different failure - and
// neither case touches a null pointer.
//
// This lives in pf-agent-editing rather than in the editor executable so
// the headless editor tier exercises the same control path the GUI's
// shortcuts drive, the same reason the document handle itself lives here.

#include <string>

#include "EditorDocument.h"

namespace editor
{
	// What a simulation-control request left behind.
	struct SimulationControl
	{
		// False when there was no live document to control: nothing was
		// paused, started or reset, and no diagnostic was produced.
		bool handled{ false };
		// The paused state the editor should show afterwards. Meaningful
		// only when `handled` is true.
		bool paused{ false };
		// Non-empty when a resume was refused: the preview stays paused,
		// and this says why.
		std::string diagnostic;
	};

	// Whether `document` names the document whose preview may be driven.
	// A null handle, and a handle to a document the editor has already
	// replaced, both answer no.
	bool previewIsControllable(
		std::shared_ptr<EditorDocument> const& document) noexcept;

	// Pause or resume the preview `document` drives. Pausing always
	// applies; a resume the World cannot support leaves the preview paused
	// and reports the reason it was refused.
	SimulationControl setPreviewPaused(
		std::shared_ptr<EditorDocument> const& document, bool paused);

	// Reset the simulation `document` drives and report the paused state
	// it settles in.
	SimulationControl resetPreview(
		std::shared_ptr<EditorDocument> const& document);
}
