#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include "core/StationConfigurationDocument.h"
#include "core/StationPrefab.h"
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

// The prefab catalogue as a station configuration resource (#812). Like the
// configuration above it is transactional: `create` parses the whole document
// and throws on the first malformed template, so a catalogue is either fully
// loaded or absent - the editor can never show half of a file's units.
class PrefabCatalogueResource final : public wp::application::resourcesystem::Resource
{
public:
	static constexpr char const* kTypeName{"PrefabCatalogue"};
	PrefabCatalogueResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	core::PrefabCatalogue const& catalogue() const { return mCatalogue; }
	std::string const& text() const { return mText; }
private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager* manager) override;
	void destroy() override;
	core::PrefabCatalogue mCatalogue;
	std::string mText;
};

void registerPrefabCatalogueResourceType(wp::application::resourcesystem::ResourceManager& manager);

// Loads manifest-declared prefab catalogues, in resource-name order. A
// malformed catalogue propagates its diagnostic as a resource failure rather
// than being silently omitted.
std::vector<std::shared_ptr<PrefabCatalogueResource>> loadPrefabCatalogueResources(
	wp::application::resourcesystem::ResourceManager& manager);
