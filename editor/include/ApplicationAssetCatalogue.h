#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ApplicationAgentTypes.h"

//
// The editor's application asset-catalogue selectors (#1001).
//
// The World panel's catalogue feeds live here and nowhere else: the declared
// Resource names of one type, every valid Agent type, an explicit Agent type
// import, and the manifest source behind a named Resource. They return editor
// types and feed editor widgets, which is exactly why they are not part of the
// shared renderer - the renderer must stay free of the editor's catalogue
// vocabulary.
//
// Everything here reads the Willpower application resource manager the shared
// render stack created (`worldRenderResourceManager()`), so the catalogue
// exists only while that stack does. It never constructs a manager of its own:
// one manifest, one manager, one atlas authority.
//

// Attaches the editor's catalogue to the shared render stack's resource
// manager. Loads every valid manifest-registered Agent type, verifies the
// bundled Human, and registers the Agent-type and Furniture-catalogue
// loaders through which core resolves World dependencies.
//
// Throws when there is no render stack to attach to, or when the bundled
// Human Agent type cannot be loaded - an install without it cannot open a
// World, and that is reported at startup rather than at first use.
void initialiseApplicationAssetCatalogue();

// Detaches the catalogue. Must run before the shared render stack is
// destroyed: the loaders registered here reach into the stack's resource
// manager, and a stale loader outliving it would dangle.
void shutdownApplicationAssetCatalogue();

// Returns the declared Resource names of one type, alphabetically.
// `type` is "FurnitureCatalogue", "AgentTagRegistry", or
// "AgentBehaviourRegistry".
std::vector<std::string> applicationResourceNames(std::string const& type);

// Every valid manifest-registered or explicitly imported Agent type, ordered
// by display name (then resource name). Invalid resources are excluded; the
// bundled Human is always present.
std::vector<ApplicationAgentType> applicationAgentTypes();

// Imports one `.agent.lua` file as a new selectable Agent type. Returns false
// and fills `diagnostic` when the file is not a valid Agent type.
bool importApplicationAgentType(std::filesystem::path const& path,
	ApplicationAgentType& imported, std::string& diagnostic);

// Returns the manifest source (file or package directory) for a named
// Resource of one type, or nullopt when the Resource is unknown.
std::optional<std::filesystem::path> applicationResourceSource(
	std::string const& type, std::string const& name);
