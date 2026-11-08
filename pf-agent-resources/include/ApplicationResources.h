#pragma once

#include <filesystem>
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

namespace game
{
	class WorldRuntime;
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
	static std::shared_ptr<FurnitureCatalogueResource> createSnapshot(
		wp::application::resourcesystem::ResourceManager& manager, std::filesystem::path const& path);
private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<const core::FurnitureCatalogue> mCatalogue;
};

// The application's World resource (#970, spec #960). It holds the
// `game::WorldRuntime` the game drives rather than a raw `core::World`.
//
// create() still deserializes a World exactly as before, but the moment that
// World exists it is handed to a runtime, which owns it exclusively from
// there on. The only thing this resource hands out is the runtime, so every
// consumer reaches the loaded World through the same façade (design pattern)
// the shipped game runs - and none of them can reach a mutable World without
// the authoring surface, which lives above the runtime and is not part of the
// game.
//
// Before create() the resource holds nothing: an unloaded World resource has
// no runtime, which is how callers tell "not created yet" apart from
// "created".
class WorldResource final : public wp::application::resourcesystem::Resource
{
public:
	WorldResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	std::shared_ptr<game::WorldRuntime> const& runtime() const { return mRuntime; }

private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	std::shared_ptr<game::WorldRuntime> mRuntime;
};

void registerApplicationResourceTypes(
	wp::application::resourcesystem::ResourceManager& manager);
