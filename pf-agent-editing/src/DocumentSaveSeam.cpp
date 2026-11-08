// The open document's save-target and staleness seam; see DocumentSaveSeam.h.

#include "DocumentSaveSeam.h"

#include <filesystem>
#include <string>
#include <utility>

#include "BehavioursPanel.h"
#include "core/WorldDocument.h"
#include "core/World.h"

namespace editor
{
	namespace
	{
		// The adjacent-file fallback both companion documents share: a Resource
		// name the application catalogue cannot resolve is read as a file
		// beside the World document the editor is holding.
		std::string adjacentPath(std::string const& savedWorldFilepath,
			std::string const& resourceName)
		{
			if (savedWorldFilepath.empty()) return {};
			return (std::filesystem::path(savedWorldFilepath).parent_path()
				/ resourceName).string();
		}
	}

	std::string currentAgentTagRegistryFilepath(DocumentPtr const& document,
		std::string const& savedWorldFilepath)
	{
		auto const world = document ? document->world() : nullptr;
		if (!world || !world->hasAttachedAgentTagRegistry()) return {};
		auto source = core::resolveCatalogSource("AgentTagRegistry",
			world->getAgentTagRegistryResourceName());
		if (!source.empty()) return source.string();
		return adjacentPath(savedWorldFilepath,
			world->getAgentTagRegistryResourceName());
	}

	std::string currentAgentBehaviourRegistryPackagePath(
		DocumentPtr const& document, std::string const& savedWorldFilepath)
	{
		auto const world = document ? document->world() : nullptr;
		if (!world || !world->hasAttachedAgentBehaviourRegistry()) return {};
		auto source = core::resolveCatalogSource("AgentBehaviourRegistry",
			world->getAgentBehaviourRegistryResourceName());
		if (!source.empty()) return source.string();
		return adjacentPath(savedWorldFilepath,
			world->getAgentBehaviourRegistryResourceName());
	}

	WorldDocumentSaveTarget currentDocumentSaveTarget(
		DocumentPtr const& document, std::string worldFilepath,
		std::string const& savedWorldFilepath)
	{
		auto const world = document ? document->world() : nullptr;
		// The saved-state marker handed to the save path is the history the
		// authoring handle owns - the same one `documentIsStale` below reads -
		// so a save settles exactly what the prompt asked about and nothing
		// else (#975).
		return { world, std::move(worldFilepath),
			currentAgentTagRegistryFilepath(document, savedWorldFilepath),
			document ? &document->authoring().history() : nullptr,
			currentAgentBehaviourRegistryPackagePath(document,
				savedWorldFilepath) };
	}

	bool documentIsStale(DocumentPtr const& document)
	{
		auto const world = document ? document->world() : nullptr;
		if (!world) return false;
		// The authoring handle's own history, not a stack read from beside it.
		// This is the line #975 is about: the prompt used to ask a different
		// `DocumentHistory` than the one the authoring façade (design pattern)
		// reported, which let the two disagree - one reading "clean" while the
		// other read "stale", neither wrong about its own object.
		return world->isModified()
			|| document->authoring().isModified()
			|| attachedAgentTagRegistryIsModified(world)
			|| attachedAgentBehaviourRegistryIsModified(world);
	}
}
