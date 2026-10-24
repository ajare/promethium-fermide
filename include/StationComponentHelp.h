#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <willpower/application/resourcesystem/Resource.h>

namespace wp::application::resourcesystem { class ResourceManager; }

// Presentation data only: never used for validation or simulation decisions.
struct StationHelpRow { std::string name, description; };
struct StationHelpBlock
{
	enum class Kind { Heading, Paragraph, List, Settings };
	Kind kind{Kind::Paragraph};
	std::string text;
	std::vector<std::string> items;
	std::vector<StationHelpRow> rows;
};
struct StationComponentHelp
{
	std::string title;
	std::vector<StationHelpBlock> blocks;
};
using StationComponentHelpCatalogue = std::map<std::string, StationComponentHelp>;

// Accepts the structured JSON resource (parsed using the existing YAML/JSON
// reader). Failure leaves the destination untouched and supplies a diagnostic.
bool parseStationComponentHelp(std::string const& text,
	StationComponentHelpCatalogue& destination, std::string& diagnostic);

class StationComponentHelpResource final : public wp::application::resourcesystem::Resource
{
public:
	static constexpr char const* kTypeName = "StationComponentHelp";
	StationComponentHelpResource(std::string const& name, std::string const& namesp,
		std::string const& source, std::map<std::string, std::string> const& tags,
		wp::application::resourcesystem::ResourceLocation* location);
	StationComponentHelp const* find(std::string const& nodeType) const;
private:
	void create(wp::application::resourcesystem::DataStreamPtr data,
		wp::application::resourcesystem::ResourceManager*) override;
	void destroy() override;
	StationComponentHelpCatalogue mCatalogue;
};

void registerStationComponentHelpResourceType(wp::application::resourcesystem::ResourceManager& manager);
std::shared_ptr<StationComponentHelpResource const> loadStationComponentHelpResource(
	wp::application::resourcesystem::ResourceManager& manager);
