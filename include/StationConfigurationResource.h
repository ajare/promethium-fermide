#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include "core/StationConfigurationDocument.h"
#include "core/StationReference.h"
#include <willpower/application/resourcesystem/Resource.h>

namespace wp::application::resourcesystem { class ResourceManager; }

// One managed YAML document. Layout belongs to the configuration; optional
// reference metadata describes the scenario, never a second settings authority.
class StationConfigurationResource final : public wp::application::resourcesystem::Resource
{
public:
	StationConfigurationResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	core::StationConfigurationDocument const& document() const { return mDocument; }
	std::optional<core::StationReferenceDocument> const& reference() const { return mReference; }
	std::string const& text() const { return mText; }
private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	core::StationConfigurationDocument mDocument;
	std::optional<core::StationReferenceDocument> mReference;
	std::string mText;
};

void registerStationConfigurationResourceType(wp::application::resourcesystem::ResourceManager& manager);

// Loads manifest-declared configurations, in resource-name order. Resource
// failures propagate with identity; malformed files are never silently omitted.
std::vector<std::shared_ptr<StationConfigurationResource>> loadStationConfigurationResources(
	wp::application::resourcesystem::ResourceManager& manager);
