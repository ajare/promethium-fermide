#pragma once

#include <map>
#include <memory>
#include <vector>
#include "core/AgentType.h"

namespace wp::application::resourcesystem { class ResourceManager; class Resource; }

struct ApplicationAgentType
{
	std::string resourceName;
	std::string typeId;
	std::string displayName;
};

// CPU-only application resource seam, shared by startup, explicit editor import,
// and document dependency resolution. Owns acquired resources, never Lua instances.
class ApplicationAgentTypes
{
public:
	explicit ApplicationAgentTypes(wp::application::resourcesystem::ResourceManager& manager);
	~ApplicationAgentTypes();
	ApplicationAgentTypes(ApplicationAgentTypes const&) = delete;
	ApplicationAgentTypes& operator=(ApplicationAgentTypes const&) = delete;
	bool importFile(std::filesystem::path const& path, ApplicationAgentType& imported,
		std::string& diagnostic);
	// Fresh dependency resolution for placement/load/Reset, not rendering.
	std::optional<core::AgentTypeDefinition> resolve(std::string const& name);
	// Pure read of the validated startup/import snapshot. Does not import,
	// reread, or execute a resource, including on missing/invalid names.
	std::optional<core::AgentTypePreview> preview(std::string const& name) const;
	std::vector<ApplicationAgentType> types() const;
private:
	wp::application::resourcesystem::ResourceManager& mManager;
	std::map<std::string, std::shared_ptr<wp::application::resourcesystem::Resource>> mResources;
};
