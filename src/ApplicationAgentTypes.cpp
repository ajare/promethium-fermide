#include "ApplicationAgentTypes.h"
#include "ApplicationResources.h"
#include "core/AgentTypeRuntime.h"
#include "core/WorldDocument.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <willpower/application/resourcesystem/DataStream.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace resources = wp::application::resourcesystem;

AgentTypeResource::AgentTypeResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags, resources::ResourceLocation* location)
	: Resource(name, namesp, "AgentType", source, tags, location) {}

void AgentTypeResource::create(resources::DataStreamPtr data, resources::ResourceManager*)
{
	std::string source;
	if (std::filesystem::path(getSource()).is_absolute())
	{
		// Like other programmatic document resources, an explicit external
		// Resource has no manifest ResourceLocation. Read only its declared file.
		if (std::filesystem::file_size(getSource()) > 1024u * 1024u)
			throw resources::ResourceException(this, "Agent type source exceeds 1 MiB");
		std::ifstream input(getSource(), std::ios::binary);
		if (!input) throw resources::ResourceException(this, "Could not read Agent type source");
		source.assign(std::istreambuf_iterator<char>(input), {});
		if (!input.good() && !input.eof())
			throw resources::ResourceException(this, "Could not read Agent type source");
	}
	else
	{
		if (!data) throw resources::ResourceException(this, "has no Agent type source");
		source.assign(reinterpret_cast<char const*>(data->getData()), data->getSize());
	}
	auto preflight = core::AgentTypeRuntimeAdapter::preflightType(getName(), source);
	if (!preflight.loaded || !core::agentTypeIdIsValid(preflight.typeId)
		|| !core::agentTypeDisplayNameIsValid(preflight.displayName))
		throw resources::ResourceException(this,
			preflight.loaded ? "Agent type identity is invalid" : preflight.diagnostic);
	core::AgentTypeRuntimeAdapter preview;
	auto result = preview.construct(preflight.typeId, source, preflight.displayName);
	if (!result.succeeded) throw resources::ResourceException(this, result.diagnostic);
	mSource = std::move(source);
	mTypeId = std::move(preflight.typeId);
	mDisplayName = std::move(preflight.displayName);
}

void AgentTypeResource::destroy()
{
	mSource.clear();
	mTypeId.clear();
	mDisplayName.clear();
}

ApplicationAgentTypes::ApplicationAgentTypes(resources::ResourceManager& manager)
	: mManager(manager)
{
	for (auto const& resource : manager.getResourcesByType("AgentType"))
	{
		if (!resource) continue;
		try
		{
			manager.acquireResource(resource);
			manager.createResource(resource);
			manager.loadResource(resource);
			mResources.emplace(resource->getName(), resource);
		}
		catch (std::exception const&) { manager.releaseResource(resource); }
	}
}

ApplicationAgentTypes::~ApplicationAgentTypes()
{
	for (auto const& [name, resource] : mResources)
	{
		(void)name;
		mManager.releaseResource(resource);
	}
}

bool ApplicationAgentTypes::importFile(std::filesystem::path const& path,
	ApplicationAgentType& imported, std::string& diagnostic)
{
	imported = {};
	diagnostic.clear();
	try
	{
		if (!path.filename().string().ends_with(".agent.lua"))
			throw std::runtime_error("Select a .agent.lua Agent type file");
		auto const canonical = std::filesystem::canonical(path);
		if (!std::filesystem::is_regular_file(canonical)
			|| !canonical.filename().string().ends_with(".agent.lua"))
			throw std::runtime_error("Agent type source must be a regular .agent.lua file");
		auto const name = core::externalAgentTypeResourceName(canonical);
		if (auto found = mResources.find(name); found != mResources.end())
		{
			auto typed = std::dynamic_pointer_cast<AgentTypeResource>(found->second);
			imported = { name, typed->typeId(), typed->displayName() };
			return true; // same source: no reread or hot reload
		}
		auto resource = std::make_shared<AgentTypeResource>(name, "", canonical.string(),
			std::map<std::string, std::string>{}, nullptr);
		struct CandidateCleanup
		{
			resources::ResourceManager& manager;
			resources::ResourcePtr resource;
			bool published{ false };
			~CandidateCleanup()
			{
				if (!published)
				{
					manager.releaseResource(resource);
					manager.destroyResources({ resource });
				}
			}
		} candidate{ mManager, resource };
		// Construct and validate BEFORE publishing to Willpower or the selector.
		// A failed candidate has no registration to compensate.
		mManager.createResource(resource);
		mManager.loadResource(resource);
		for (auto const& type : types())
			if (type.typeId == resource->typeId())
				throw std::runtime_error("Duplicate Agent type ID '" + type.typeId
					+ "' is already provided by resource '" + type.resourceName + "'");
		// Finish allocations before the final Willpower publication. If its
		// duplicate/name checks refuse, erase our candidate index as well.
		ApplicationAgentType selection{ name, resource->typeId(), resource->displayName() };
		mResources.emplace(name, resource);
		try { mManager.addResource(resource); }
		catch (...)
		{
			mResources.erase(name);
			throw;
		}
		candidate.published = true;
		mManager.acquireResource(resource);
		imported = std::move(selection);
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = "Could not import Agent type '" + path.string() + "': " + error.what();
		return false;
	}
}

std::optional<core::AgentTypeDefinition> ApplicationAgentTypes::resolve(std::string const& name)
{
	auto found = mResources.find(name);
	if (found == mResources.end())
	{
		auto const path = core::externalAgentTypeResourcePath(name);
		if (path.empty()) return std::nullopt;
		if (core::externalAgentTypeResourceName(path) != name)
			throw std::runtime_error("Agent type source identity changed: " + path.string());
		ApplicationAgentType imported;
		std::string diagnostic;
		if (!importFile(path, imported, diagnostic)) throw std::runtime_error(diagnostic);
		found = mResources.find(name);
	}
	auto typed = std::dynamic_pointer_cast<AgentTypeResource>(found->second);
	if (!typed) return std::nullopt;
	// Dependency resolution is an intentional reconstruction boundary. Read
	// the selected Resource's current source into a private candidate, without
	// replacing the startup/import resource or any surviving World instance.
	// Ordinary import remains idempotent; this is not hot reload.
	auto candidate = std::make_shared<AgentTypeResource>(name, typed->getNamespace(),
		typed->getSource(), typed->getTags(), typed->mwLocation);
	struct Cleanup
	{
		resources::ResourceManager& manager;
		resources::ResourcePtr resource;
		~Cleanup() { manager.destroyResources({ resource }); }
	} cleanup{ mManager, candidate };
	mManager.createResource(candidate);
	return core::AgentTypeDefinition{ candidate->typeId(), candidate->displayName(),
		name, candidate->source() };
}

std::vector<ApplicationAgentType> ApplicationAgentTypes::types() const
{
	std::vector<ApplicationAgentType> result;
	for (auto const& [name, resource] : mResources)
	{
		auto typed = std::dynamic_pointer_cast<AgentTypeResource>(resource);
		if (typed) result.push_back({ name, typed->typeId(), typed->displayName() });
	}
	std::sort(result.begin(), result.end(), [](auto const& left, auto const& right) {
		return left.displayName != right.displayName ? left.displayName < right.displayName
			: left.resourceName < right.resourceName;
	});
	return result;
}
