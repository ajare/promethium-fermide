#include "StationComponentHelp.h"

#include <stdexcept>
#include <yaml-cpp/yaml.h>
#include <willpower/application/resourcesystem/DataStream.h>
#include <willpower/application/resourcesystem/ResourceExceptions.h>
#include <willpower/application/resourcesystem/ResourceFactory.h>
#include <willpower/application/resourcesystem/ResourceManager.h>

namespace resources = wp::application::resourcesystem;

namespace
{
	std::string requiredText(YAML::Node const& node, char const* field)
	{
		auto value = node[field];
		if (!value.IsScalar()) throw std::runtime_error(std::string("missing text field '") + field + "'");
		auto text = value.as<std::string>();
		if (text.empty() || text.size() > 65536)
			throw std::runtime_error(std::string("invalid text field '") + field + "'");
		return text;
	}
	void requireSequence(YAML::Node const& value)
	{
		if (!value.IsSequence() || value.size() == 0 || value.size() > 256)
			throw std::runtime_error("expected a nonempty bounded sequence");
	}
}

bool parseStationComponentHelp(std::string const& text,
	StationComponentHelpCatalogue& destination, std::string& diagnostic)
{
	try
	{
		if (text.size() > 512 * 1024) throw std::runtime_error("document exceeds 512 KiB");
		auto root = YAML::Load(text);
		if (!root.IsMap() || root["version"].as<int>() != 1)
			throw std::runtime_error("unsupported component help version");
		auto entries = root["components"];
		requireSequence(entries);
		StationComponentHelpCatalogue staged;
		for (auto const& entry : entries)
		{
			auto type = requiredText(entry, "nodeType");
			StationComponentHelp help;
			help.title = requiredText(entry, "title");
			auto blocks = entry["blocks"];
			requireSequence(blocks);
			for (auto const& node : blocks)
			{
				StationHelpBlock block;
				auto kind = requiredText(node, "kind");
				if (kind == "heading" || kind == "paragraph")
				{
					block.kind = kind == "heading" ? StationHelpBlock::Kind::Heading : StationHelpBlock::Kind::Paragraph;
					block.text = requiredText(node, "text");
				}
				else if (kind == "list")
				{
					block.kind = StationHelpBlock::Kind::List;
					requireSequence(node["items"]);
					for (auto const& item : node["items"])
					{
						if (!item.IsScalar() || item.as<std::string>().empty())
							throw std::runtime_error("invalid list item");
						block.items.push_back(item.as<std::string>());
					}
				}
				else if (kind == "settings")
				{
					block.kind = StationHelpBlock::Kind::Settings;
					requireSequence(node["rows"]);
					for (auto const& row : node["rows"])
						block.rows.push_back({requiredText(row, "name"), requiredText(row, "description")});
				}
				else throw std::runtime_error("unknown block kind '" + kind + "'");
				help.blocks.push_back(std::move(block));
			}
			if (!staged.emplace(type, std::move(help)).second)
				throw std::runtime_error("duplicate component '" + type + "'");
		}
		destination = std::move(staged);
		diagnostic.clear();
		return true;
	}
	catch (std::exception const& error)
	{
		diagnostic = std::string("Station component help: ") + error.what();
		return false;
	}
}

StationComponentHelpResource::StationComponentHelpResource(std::string const& name,
	std::string const& namesp, std::string const& source,
	std::map<std::string, std::string> const& tags, resources::ResourceLocation* location)
	: Resource(name, namesp, kTypeName, source, tags, location) {}

StationComponentHelp const* StationComponentHelpResource::find(std::string const& nodeType) const
{
	auto found = mCatalogue.find(nodeType);
	return found == mCatalogue.end() ? nullptr : &found->second;
}

void StationComponentHelpResource::create(resources::DataStreamPtr data, resources::ResourceManager*)
{
	if (!data) throw resources::ResourceException(this, "has no component help document");
	std::string diagnostic;
	if (!parseStationComponentHelp(std::string(reinterpret_cast<char const*>(data->getData()), data->getSize()),
		mCatalogue, diagnostic)) throw resources::ResourceException(this, diagnostic);
}

void StationComponentHelpResource::destroy() { mCatalogue.clear(); }

void registerStationComponentHelpResourceType(resources::ResourceManager& manager)
{
	class Factory final : public resources::ResourceFactory
	{
	public:
		Factory() : ResourceFactory(StationComponentHelpResource::kTypeName) {}
		resources::Resource* createResource(std::string const& name, std::string const& namesp,
			std::string const& source, std::map<std::string, std::string> const& tags,
			resources::ResourceLocation* location) override
		{ return new StationComponentHelpResource(name, namesp, source, tags, location); }
	};
	manager.addResourceFactory(new Factory);
}

std::shared_ptr<StationComponentHelpResource const> loadStationComponentHelpResource(resources::ResourceManager& manager)
{
	std::shared_ptr<StationComponentHelpResource> result;
	for (auto const& entry : manager.getResourcesByType(StationComponentHelpResource::kTypeName))
	{
		if (result) throw resources::ResourceException(entry.get(), "duplicates the component help catalogue");
		result = std::dynamic_pointer_cast<StationComponentHelpResource>(entry);
		if (!result) throw resources::ResourceException(entry.get(), "is not StationComponentHelp");
		manager.createResource(result);
		manager.loadResource(result);
	}
	return result;
}
