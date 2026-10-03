#include "ApplicationResources.h"

#include <filesystem>
#include <stdexcept>
#include <type_traits>

#include <willpower/application/resourcesystem/DataStream.h>
#include <willpower/application/resourcesystem/DirectoryResourceLocation.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

#include "core/AgentBehaviourRegistry.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationException.h"
#include "core/SerializationWorkData.h"
#include "core/World.h"
#include "core/Furniture.h"
#include <willpower/application/resourcesystem/ImageSetResource.h>
#include "core/YamlSerializer.h"

namespace resources = wp::application::resourcesystem;
namespace
{
	std::filesystem::path sourcePath(resources::Resource const& resource)
	{
		auto source = std::filesystem::path(resource.getSource());
		if (source.is_absolute()) return source;
		return std::filesystem::path(resource.getDefinitionFile()).parent_path() / source;
	}

	template <typename ResourceType>
	class Factory final : public resources::ResourceFactory
	{
	public:
		explicit Factory(std::string type) : ResourceFactory(std::move(type)) {}

		resources::Resource* createResource(std::string const& name,
			std::string const& namesp, std::string const& source,
			std::map<std::string, std::string> const& tags,
			resources::ResourceLocation* location) override
		{
			return new ResourceType(name, namesp, source, tags, location);
		}
	};
}

LuaScriptResource::LuaScriptResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags,
	resources::ResourceLocation* location)
	: Resource(name, namesp, "LuaScript", source, tags, location)
{
}

void LuaScriptResource::create(resources::DataStreamPtr data,
	resources::ResourceManager*)
{
	if (!data) throw resources::ResourceException(this, "has no Lua source");
	mText.assign(reinterpret_cast<char const*>(data->getData()), data->getSize());
}

void LuaScriptResource::destroy()
{
	mText.clear();
}

AgentTagRegistryResource::AgentTagRegistryResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags,
	resources::ResourceLocation* location)
	: Resource(name, namesp, "AgentTagRegistry", source, tags, location)
{
}

void AgentTagRegistryResource::create(resources::DataStreamPtr,
	resources::ResourceManager*)
{
	mRegistry = core::AgentTagRegistry::loadFrom(sourcePath(*this).string());
}

void AgentTagRegistryResource::destroy()
{
	mRegistry.reset();
}

AgentBehaviourRegistryResource::AgentBehaviourRegistryResource(
	std::string const& name, std::string const& namesp,
	std::string const& source, std::map<std::string, std::string> const& tags,
	resources::ResourceLocation* location)
	: Resource(name, namesp, "AgentBehaviourRegistry", source, tags, location)
{
}

void AgentBehaviourRegistryResource::create(resources::DataStreamPtr,
	resources::ResourceManager*)
{
	mRegistry = core::AgentBehaviourRegistry::loadFrom(sourcePath(*this).string());
}

void AgentBehaviourRegistryResource::destroy()
{
	mRegistry.reset();
}

FurnitureCatalogueResource::FurnitureCatalogueResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags, resources::ResourceLocation* location)
	: Resource(name, namesp, "FurnitureCatalogue", source, tags, location) {}

void FurnitureCatalogueResource::create(resources::DataStreamPtr, resources::ResourceManager*)
{
	auto catalogue = core::FurnitureCatalogue::readFile(sourcePath(*this));
	for (auto const& [key, definition] : catalogue->definitions())
	{
		auto set = hasDependentResource("Artwork")
			? std::dynamic_pointer_cast<resources::ImageSetResource>(getDependentResource("Artwork")) : nullptr;
		if (!set || set->getName() != definition.imageSet || !set->getImageDefinitions().contains(definition.image))
			throw resources::ResourceException(this, "Missing Furniture Image-set region for " + key
				+ ": " + definition.imageSet + "/" + definition.image);
	}
	mCatalogue = std::move(catalogue);
}
void FurnitureCatalogueResource::destroy() { mCatalogue.reset(); }

WorldResource::WorldResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags,
	resources::ResourceLocation* location)
	: Resource(name, namesp, "World", source, tags, location)
{
}

void WorldResource::create(resources::DataStreamPtr data,
	resources::ResourceManager*)
{
	if (!data) throw resources::ResourceException(this, "has no World document");
	std::string yaml(reinterpret_cast<char const*>(data->getData()), data->getSize());
	auto serializer = core::YamlSerializer::fromString(yaml);
	serializer->deserialize();
	auto world = std::make_shared<core::World>("Loading", 1, 1);
	core::SerializationWorkData workData;
	if (hasDependentResource("Furniture"))
	{
		auto dependency = std::dynamic_pointer_cast<FurnitureCatalogueResource>(getDependentResource("Furniture"));
		if (!dependency || !dependency->catalogue())
			throw resources::ResourceException(this, "Furniture dependency is not a loaded FurnitureCatalogue");
		workData.furnitureCatalogue = dependency->catalogue();
	}
	if (!world->deserialize(*serializer, workData))
		throw core::SerializationException("Could not deserialize World resource");

	if (world->hasAgentTagRegistryReference())
	{
		if (!hasDependentResource("AgentTags"))
			throw resources::ResourceException(this,
				"references an Agent tag registry but has no AgentTags dependency");
		auto dependency = std::dynamic_pointer_cast<AgentTagRegistryResource>(
			getDependentResource("AgentTags"));
		if (!dependency || !dependency->registry())
			throw resources::ResourceException(this,
				"AgentTags dependency is not a loaded AgentTagRegistry");
		world->resolveAgentTagRegistry(dependency->registry());
	}

	if (world->hasAgentBehaviourRegistryReference())
	{
		try
		{
			if (!hasDependentResource("AgentBehaviours"))
				throw std::runtime_error(
					"World has no AgentBehaviours resource dependency");
			auto dependency = std::dynamic_pointer_cast<AgentBehaviourRegistryResource>(
				getDependentResource("AgentBehaviours"));
			if (!dependency || !dependency->registry())
				throw std::runtime_error(
					"AgentBehaviours dependency is not a loaded AgentBehaviourRegistry");
			world->resolveAgentBehaviourRegistry(dependency->registry());
		}
		catch (std::exception const& error)
		{
			// ADR 0008: a behaviour-package refusal is recoverable dependency
			// state; the World structure and authored assignments remain loaded.
			world->markAgentBehaviourRegistryUnavailable(error.what());
		}
	}
	mWorld = std::move(world);
}

void WorldResource::destroy()
{
	mWorld.reset();
}

void registerApplicationResourceTypes(resources::ResourceManager& manager)
{
	manager.addResourceFactory(new Factory<LuaScriptResource>("LuaScript"));
	manager.addResourceFactory(new Factory<AgentTagRegistryResource>(
		"AgentTagRegistry"));
	manager.addResourceFactory(new Factory<AgentBehaviourRegistryResource>(
		"AgentBehaviourRegistry"));
	manager.addResourceFactory(new Factory<FurnitureCatalogueResource>("FurnitureCatalogue"));
	manager.addResourceFactory(new Factory<WorldResource>("World"));
}
