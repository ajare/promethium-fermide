#pragma once

// World snapshot integration for the reusable per-document history.
//
// A history belongs to a document, and every document owns exactly one: the
// editor's World document keeps its on the `game::WorldAuthoring` handle
// that changes the World, which is what makes the handle's claim to own the
// document history a description rather than an aspiration (#975).
//
// What this header adds is the seam the shared editor panels use to *find*
// that history, because a panel is handed a `core::World` and nothing else.
// Until #975 the seam was a process-wide `DocumentHistory` that every
// capture and commit defaulted to, and that single default is what made the
// authoring handle's documented `isModified()` a lie: the editor dirtied
// the global stack while the handle's own stack sat empty, so a caller who
// trusted the authoring façade (design pattern) was told there was no
// unsaved work while the author was hours deep in it. The global is gone. The
// seam resolves now, and a default that has to look something up cannot
// quietly be a different document's stack.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "DocumentHistory.h"

namespace core
{
	class World;
}

// What `currentDocumentHistory()` answers.
using DocumentHistoryResolver = std::function<DocumentHistory& ()>;

// The history the edit being made right now belongs to.
//
// In the editor that is the open document's authoring history, which is the
// same object `WorldAuthoring::isModified()`, `markSaved()` and
// `clearHistory()` act on. There is no fallback: an edit with no document
// open has no history to belong to, and saying so is worth more than
// writing it somewhere nobody will ever read back.
//
// Throws std::logic_error when nothing can answer for the edit.
DocumentHistory& currentDocumentHistory();

// Replaces what `currentDocumentHistory()` answers and hands back what was
// installed before it. Passing nullptr restores the production lookup.
//
// Production never calls this. The headless editor smoke harness does, once
// per process, over a history the harness owns: those checks drive the same
// panels the GUI drives but against a bare `core::World` with no
// `editor::EditorDocument` open, and they need somewhere to commit that is
// not a production document. A shipped build is never pointed at a stack
// nobody owns, because nothing in it installs a resolver.
DocumentHistoryResolver setDocumentHistoryResolver(
	DocumentHistoryResolver resolver);

std::optional<DocumentSnapshot> captureDocumentSnapshot(
	std::shared_ptr<const core::World> const& world,
	DocumentHistory const& history = currentDocumentHistory());

// Rebuild a snapshot into a detached World. A saved document's adjacent
// dependencies are resolved from its directory; unsaved documents retain
// the current World's loaded Furniture catalogue as the only available
// fallback.
//
// The rebuilt World is handed back exclusively owned (#973): the editor puts
// every restored World into a `WorldRuntime` of its own, and a runtime can
// only take a World it will own alone. A caller that wants to share the
// result can still move it into a `std::shared_ptr<core::World>`.
std::unique_ptr<core::World> deserializeDocumentSnapshot(
	DocumentSnapshot const& snapshot,
	std::shared_ptr<const core::World> const& currentWorld,
	std::filesystem::path const& documentPath);

void commitDocumentEdit(std::optional<DocumentSnapshot> snapshot,
	DocumentHistory& history = currentDocumentHistory());
