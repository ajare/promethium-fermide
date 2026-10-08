#pragma once

#include <map>
#include <memory>
#include <string>
#include "core/AgentType.h"

#include <willpower/application/resourcesystem/Resource.h>
#include <willpower/application/resourcesystem/ResourceFactory.h>

namespace core
{
	class AgentBehaviourRegistry;
	class AgentTagRegistry;
	class World;
	class FurnitureCatalogue;
}

namespace wp::application::resourcesystem
{
	class ResourceManager;
}

class LuaScriptResource final : public wp::application::resourcesystem::Resource
{
public:
	LuaScriptResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::string const& text() const { return mText; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::string mText;
};

// A managed `.agent.lua` Agent type definition (ADR 0019). create() validates
// the source in an isolated scratch sandbox and retains the immutable text and
// stable type identity and validated preview baseline. Rendering reads that
// snapshot without I/O or Lua construction. No World or live runtime is touched;
// destroy() releases the snapshot, never a live Lua object.
class AgentTypeResource final : public wp::application::resourcesystem::Resource
{
	friend class ApplicationAgentTypes;
public:
	AgentTypeResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::string const& source() const { return mSource; }
	std::string const& typeId() const { return mTypeId; }
	std::string const& displayName() const { return mDisplayName; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::string mSource;
	std::string mTypeId;
	std::string mDisplayName;
	std::optional<core::AgentTypePreview> mPreview;
};

class AgentTagRegistryResource final : public wp::application::resourcesystem::Resource
{
public:
	AgentTagRegistryResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::AgentTagRegistry> const& registry() const { return mRegistry; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::AgentTagRegistry> mRegistry;
};

class AgentBehaviourRegistryResource final : public wp::application::resourcesystem::Resource
{
public:
	AgentBehaviourRegistryResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::AgentBehaviourRegistry> const& registry() const { return mRegistry; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::AgentBehaviourRegistry> mRegistry;
};

class FurnitureCatalogueResource final : public wp::application::resourcesystem::Resource
{
public:
	FurnitureCatalogueResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<const core::FurnitureCatalogue> const& catalogue() const { return mCatalogue; }
	void setArtwork(wp::application::resourcesystem::ResourcePtr artwork) { addDependentResource("Artwork", std::move(artwork)); }
private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<const core::FurnitureCatalogue> mCatalogue;
};

class WorldResource final : public wp::application::resourcesystem::Resource
{
public:
	WorldResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<core::World> const& world() const { return mWorld; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<core::World> mWorld;
};

void registerApplicationResourceTypes(
	wp::application::resourcesystem::ResourceManager& manager);
