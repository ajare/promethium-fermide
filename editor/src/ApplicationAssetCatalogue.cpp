#include "ApplicationAssetCatalogue.h"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <willpower/application/resourcesystem/ResourceManager.h>

#include "ApplicationResources.h"
#include "WorldRenderStack.h"
#include "core/AgentType.h"
#include "core/Furniture.h"
#include "core/WorldDocument.h"

namespace resources = wp::application::resourcesystem;
namespace
{
	// Every valid, loaded Agent type Resource, keyed by Resource name. The
	// bundled Human and every manifest-registered type are loaded at startup
	// so the creation selector and document loading can resolve them.
	std::unique_ptr<ApplicationAgentTypes> gAgentTypes;
}

void initialiseApplicationAssetCatalogue()
{
	auto* manager = worldRenderResourceManager();
	if (!manager)
		throw std::runtime_error(
			"The application asset catalogue needs the shared World render stack's resource manager");
	gAgentTypes = std::make_unique<ApplicationAgentTypes>(*manager);
	if (!gAgentTypes->resolve("human.agent.lua"))
		throw std::runtime_error("The bundled Human Agent type could not be loaded");
	core::setAgentTypeResourceLoader([](std::string const& name)
	{
		return gAgentTypes->resolve(name);
	}, [](std::string const& name)
	{
		return gAgentTypes->preview(name);
	});
	core::FurnitureCatalogue::setResourceLoader([](std::filesystem::path const& path)
	{
		// Loading is an explicit package preflight, including reload. Never
		// return a path-only cache: external edits must be read and scripts
		// validated into a fresh immutable snapshot. Existing Worlds retain
		// their own accepted catalogue until their transaction commits.
		return FurnitureCatalogueResource::createSnapshot(
			*worldRenderResourceManager(), path)->catalogue();
	});
}

void shutdownApplicationAssetCatalogue()
{
	// Both loaders reach through the shared stack's resource manager, so they
	// are unregistered before the catalogue and the stack go away.
	core::setAgentTypeResourceLoader({});
	core::FurnitureCatalogue::setResourceLoader({});
	gAgentTypes.reset();
}

std::vector<std::string> applicationResourceNames(std::string const& type)
{
	auto* manager = worldRenderResourceManager();
	if (!manager) return {};
	std::vector<std::string> names;
	for (auto const& resource : manager->getResourcesByType(type))
		if (resource) names.push_back(resource->getName());
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	return names;
}

std::vector<ApplicationAgentType> applicationAgentTypes()
{
	if (!gAgentTypes) return {};
	return gAgentTypes->types();
}

bool importApplicationAgentType(std::filesystem::path const& path,
	ApplicationAgentType& imported, std::string& diagnostic)
{
	if (!gAgentTypes)
	{
		diagnostic = "Application resources are unavailable";
		return false;
	}
	return gAgentTypes->importFile(path, imported, diagnostic);
}

std::optional<std::filesystem::path> applicationResourceSource(
	std::string const& type, std::string const& name)
{
	// The manifest catalog resolver is installed with the shared render stack;
	// with no stack there is no manifest to answer from.
	if (!worldRenderStackActive()) return std::nullopt;
	auto source = core::resolveCatalogSource(type, name);
	if (source.empty()) return std::nullopt;
	return source;
}
