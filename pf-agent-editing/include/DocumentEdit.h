#pragma once

// World snapshot integration for the reusable per-document history. The GUI
// currently owns one World document; external editor documents can own
// separate DocumentHistory instances without sharing stacks or saved state.

#include <filesystem>
#include <memory>
#include <optional>

#include "DocumentHistory.h"

namespace core
{
	class World;
}

extern DocumentHistory gWorldDocumentHistory;

std::optional<DocumentSnapshot> captureDocumentSnapshot(
	std::shared_ptr<const core::World> const& world,
	DocumentHistory const& history = gWorldDocumentHistory);

// Rebuild a snapshot into a detached World. A saved document's adjacent
// dependencies are resolved from its directory; unsaved documents retain the
// current World's loaded Furniture catalogue as the only available fallback.
std::shared_ptr<core::World> deserializeDocumentSnapshot(
	DocumentSnapshot const& snapshot,
	std::shared_ptr<const core::World> const& currentWorld,
	std::filesystem::path const& documentPath);

void commitDocumentEdit(std::optional<DocumentSnapshot> snapshot,
	DocumentHistory& history = gWorldDocumentHistory);
