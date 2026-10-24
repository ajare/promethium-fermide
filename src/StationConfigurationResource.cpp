#include "StationConfigurationResource.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <willpower/application/resourcesystem/DataStream.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/application/resourcesystem/ResourceFactory.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace resources = wp::application::resourcesystem;

StationConfigurationResource::StationConfigurationResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags, resources::ResourceLocation* location)
	: Resource(name, namesp, "StationConfiguration", source, tags, location) {}

void StationConfigurationResource::create(resources::DataStreamPtr data, resources::ResourceManager*)
{
	if (!data) throw resources::ResourceException(this, "has no Station configuration document");
	std::string text(reinterpret_cast<char const*>(data->getData()), data->getSize());
	core::StationConfigurationDocument document;
	std::string diagnostic;
	if (!core::parseStationConfiguration(text, document, &diagnostic))
		throw resources::ResourceException(this, diagnostic);
	std::optional<core::StationReferenceDocument> reference;
	auto yaml = core::YamlSerializer::fromString(text);
	yaml->deserialize();
	if (yaml->hasField("name"))
	{
		reference.emplace();
		if (!core::parseStationReference(text, *reference, &diagnostic))
			throw resources::ResourceException(this, diagnostic);
	}
	mDocument = std::move(document);
	mReference = std::move(reference);
	mText = std::move(text);
}

void StationConfigurationResource::destroy()
{
	mDocument = {}; mReference.reset(); mText.clear();
}

void registerStationConfigurationResourceType(resources::ResourceManager& manager)
{
	class Factory final : public resources::ResourceFactory
	{
	public:
		Factory() : ResourceFactory("StationConfiguration") {}
		resources::Resource* createResource(std::string const& name, std::string const& namesp,
			std::string const& source, std::map<std::string, std::string> const& tags,
			resources::ResourceLocation* location) override
		{ return new StationConfigurationResource(name, namesp, source, tags, location); }
	};
	manager.addResourceFactory(new Factory);
}

std::vector<std::shared_ptr<StationConfigurationResource>> loadStationConfigurationResources(
	resources::ResourceManager& manager)
{
	std::vector<std::shared_ptr<StationConfigurationResource>> result;
	for (auto const& entry : manager.getResourcesByType("StationConfiguration"))
	{
		auto resource = std::dynamic_pointer_cast<StationConfigurationResource>(entry);
		if (!resource) throw resources::ResourceException(entry.get(), "is not a StationConfiguration");
		manager.createResource(resource);
		manager.loadResource(resource);
		result.push_back(std::move(resource));
	}
	std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
		return a->getQualifiedName() < b->getQualifiedName();
	});
	return result;
}

PrefabCatalogueResource::PrefabCatalogueResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags, resources::ResourceLocation* location)
	: Resource(name, namesp, kTypeName, source, tags, location) {}

void PrefabCatalogueResource::create(resources::DataStreamPtr data, resources::ResourceManager*)
{
	if (!data) throw resources::ResourceException(this, "has no prefab catalogue document");
	std::string text(reinterpret_cast<char const*>(data->getData()), data->getSize());
	// Transactional: the parser refuses the whole catalogue on the first
	// malformed template, and the refusal surfaces here as a resource failure
	// carrying the parser's diagnostic. Nothing partial is kept.
	core::PrefabCatalogue catalogue;
	std::string diagnostic;
	if (!core::parsePrefabCatalogue(text, catalogue, &diagnostic))
		throw resources::ResourceException(this, diagnostic);
	mCatalogue = std::move(catalogue);
	mText = std::move(text);
}

void PrefabCatalogueResource::destroy()
{
	mCatalogue = {}; mText.clear();
}

void registerPrefabCatalogueResourceType(resources::ResourceManager& manager)
{
	class Factory final : public resources::ResourceFactory
	{
	public:
		Factory() : ResourceFactory(PrefabCatalogueResource::kTypeName) {}
		resources::Resource* createResource(std::string const& name, std::string const& namesp,
			std::string const& source, std::map<std::string, std::string> const& tags,
			resources::ResourceLocation* location) override
		{ return new PrefabCatalogueResource(name, namesp, source, tags, location); }
	};
	manager.addResourceFactory(new Factory);
}

std::vector<std::shared_ptr<PrefabCatalogueResource>> loadPrefabCatalogueResources(
	resources::ResourceManager& manager)
{
	std::vector<std::shared_ptr<PrefabCatalogueResource>> result;
	for (auto const& entry : manager.getResourcesByType(PrefabCatalogueResource::kTypeName))
	{
		auto resource = std::dynamic_pointer_cast<PrefabCatalogueResource>(entry);
		if (!resource) throw resources::ResourceException(entry.get(), "is not a PrefabCatalogue");
		manager.createResource(resource);
		manager.loadResource(resource);
		result.push_back(std::move(resource));
	}
	std::sort(result.begin(), result.end(), [](auto const& a, auto const& b) {
		return a->getQualifiedName() < b->getQualifiedName();
	});
	return result;
}
