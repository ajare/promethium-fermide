#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include "core/Platform.h"

namespace core
{
	class AgentTagRegistry;
	class World;

	// The default adjacent filename used by the Tags panel. Both
	// `/project/station.world` and `/project/station.world.yaml` resolve to
	// `/project/station.tags.yaml`.
	PF_CORE_API std::filesystem::path defaultAgentTagRegistryPath(
		std::filesystem::path const& worldFilepath);

	// Creates a new empty registry without overwriting an existing file, writes
	// it atomically, and only then attaches its basename and UUID to the World.
	PF_CORE_API std::shared_ptr<AgentTagRegistry> createAndAttachAgentTagRegistry(
		World& world, std::filesystem::path const& worldFilepath);

	// Creates an independent persisted copy at an unoccupied destination. The
	// returned managed document has a new UUID and definitions exactly equivalent
	// to source. Installation is no-clobber even if another writer races the
	// initial collision check.
	PF_CORE_API std::shared_ptr<AgentTagRegistry> copyAgentTagRegistryDocument(
		AgentTagRegistry const& source,
		std::filesystem::path const& destinationFilepath);

	// Selects an existing registry. Both documents must resolve to regular files
	// in the same canonical directory, and only the registry basename is stored.
	PF_CORE_API std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistry(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& registryFilepath);

	// The explicit destructive switch validates and loads the replacement before
	// atomically clearing every assignment/sample and changing the reference.
	PF_CORE_API std::shared_ptr<AgentTagRegistry> selectAndAttachAgentTagRegistryClearingAssignments(
		World& world, std::filesystem::path const& worldFilepath,
		std::filesystem::path const& registryFilepath);

	// Resolves a persisted World reference beside the World document,
	// verifies the registry UUID, and attaches it. Registries are shared by
	// canonical file identity. Returns null when the World has no reference.
	PF_CORE_API std::shared_ptr<AgentTagRegistry> loadAndAttachAgentTagRegistry(
		World& world, std::filesystem::path const& worldFilepath);

	// Explicitly reloads a clean shared registry from disk. The replacement and
	// every loaded Agent are validated before definitions or samples change; all
	// dependent Worlds must already be paused.
	PF_CORE_API bool reloadAgentTagRegistryDocument(
		std::shared_ptr<AgentTagRegistry> const& registry,
		std::filesystem::path const& registryFilepath,
		std::string* diagnostic = nullptr);

	// Removes a manager-owned registry only after its final World detaches.
	// Dirty registries stay loaded unless discardDirty is the user's explicit
	// discard action.
	PF_CORE_API bool unloadAgentTagRegistryDocumentIfUnused(
		std::shared_ptr<AgentTagRegistry> const& registry,
		bool discardDirty = false);

	// Loads a complete binary or YAML World document and its optional registries
	// into temporary state. No caller-owned World is changed when the World or a
	// fatal dependency is refused.
	PF_CORE_API std::shared_ptr<World> loadWorldDocument(
		std::filesystem::path const& worldFilepath);
}
