#pragma once

// The open editor document's save and staleness seam (#975).
//
// `isDocumentStale` and the save-target builder used to live in the GUI's
// UI.cpp, where nothing that runs without a window could reach them. They
// moved here because the claim they carry - that the editor's unsaved-changes
// prompt reads the same history the authoring handle reports - is the exact
// claim #975 found broken, and a claim that cannot be exercised cannot be
// pinned. The UI calls through this seam, and a headless check asserts the
// two answers are the same object rather than merely the same shape.
//
// A document is stale when any of the four things that travel with it is
// dirty: the World itself, the authoring history the handle owns, the
// attached Agent tag registry, and the attached Agent behaviour package.
// None of them stands in for another, which is why all four are asked.

#include <string>

#include "EditorDocument.h"
#include "TagsPanel.h"

namespace editor
{
	// Where the open document's attached Agent tag registry is written, and
	// where the attached Agent behaviour package is written. Both resolve a
	// Resource name through the application catalogue first and fall back to
	// the file beside the World document, which is what keeps hand-authored
	// and legacy adjacent-file documents working.
	std::string currentAgentTagRegistryFilepath(DocumentPtr const& document,
		std::string const& savedWorldFilepath);

	std::string currentAgentBehaviourRegistryPackagePath(
		DocumentPtr const& document, std::string const& savedWorldFilepath);

	// The save target for `document` at `worldFilepath`, carrying the
	// document's own saved-state marker - the history its authoring handle
	// owns, not a stack borrowed from anywhere else (#975).
	//
	// `savedWorldFilepath` is where the document lives right now, which is
	// what the adjacent-file fallback resolves against. It is a parameter
	// rather than a read because the editor keeps that in its own state, and
	// because during Save As it is deliberately not the destination: the
	// registry-copy logic keys off the source directory, and #975 moves the
	// seam without disturbing that.
	WorldDocumentSaveTarget currentDocumentSaveTarget(
		DocumentPtr const& document, std::string worldFilepath,
		std::string const& savedWorldFilepath);

	// True when `document` holds work that has not reached disk. This is the
	// question the close, New, Open and Exit prompts ask, and it reads
	// `document->authoring().isModified()` rather than a parallel history,
	// so the prompt and the authoring façade (design pattern) cannot fall out
	// of agreement (#975).
	bool documentIsStale(DocumentPtr const& document);
}
