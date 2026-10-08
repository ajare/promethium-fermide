#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/SerializationException.h"
#include "core/WorldDocument.h"

#include <fstream>
#include <iterator>

namespace core
{
	namespace
	{
		AgentTypeResourceLoader gAgentTypeResourceLoader;

		// The bundled Human Agent type. This source is the single authority for
		// Human physical outcomes in this slice; resources/test-worlds/human.agent.lua
		// must remain byte-for-byte identical (a smoke check asserts this).
		constexpr std::string_view kHumanAgentLua = R"(-- Bundled Human Agent type definition.
-- One .agent.lua source defines exactly one type object with a stable type ID,
-- a display name, and a new() constructor returning a fresh instance carrying
-- the complete physical baseline.
return {
    api_version = 1,
    type_id = "Human",
    display_name = "Human",
    new = function()
        return {
            width = 0.4,
            standing_height = (0.7 - 0.2) - 0.05,
            reach = 0.25,
            walk_speed = 0.5,
            climb_speed = 0.25,
            stair_ascent_speed = 0.35,
            stair_descent_speed = 0.45,
            sitting_height_ratio = 0.6,
            crouching_height_ratio = 0.6,
            crawling_height_ratio = 0.3,
            crawling_speed_ratio = 0.5,
        }
    end,
}
)";
	}

	AgentTypeDefinition bundledHumanAgentType()
	{
		AgentTypeDefinition definition;
		definition.typeId = "Human";
		definition.displayName = "Human";
		definition.resourceName = "human.agent.lua";
		definition.source = std::string(kHumanAgentLua);
		return definition;
	}

	AgentPhysicalBaseline const& bundledHumanBaseline()
	{
		static AgentPhysicalBaseline const baseline = []
		{
			auto computed = agentTypeDefinitionBaseline(bundledHumanAgentType());
			if (!computed)
				throw SerializationException(
					"Bundled Human type failed to construct a baseline");
			return *computed;
		}();
		return baseline;
	}

	std::optional<AgentPhysicalBaseline> agentTypeDefinitionBaseline(
		AgentTypeDefinition const& definition)
	{
		try
		{
			AgentTypeRuntimeAdapter runtime;
			auto result = runtime.construct(definition.typeId,
				definition.source, definition.displayName);
			if (!result.succeeded) return std::nullopt;
			return result.baseline;
		}
		catch (std::exception const&)
		{
			return std::nullopt;
		}
	}

	bool agentTypeIdIsValid(std::string_view typeId)
	{
		if (typeId.empty() || typeId.size() > 128) return false;
		for (auto const character : typeId)
		{
			auto const isAlpha = (character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z');
			auto const isDigit = character >= '0' && character <= '9';
			if (!isAlpha && !isDigit && character != '_' && character != '-')
				return false;
		}
		return true;
	}

	bool agentTypeDisplayNameIsValid(std::string_view name)
	{
		if (name.empty() || name.size() > 128) return false;
		for (auto const character : name)
		{
			if (character == '\0' || (static_cast<unsigned char>(character) < 0x20))
				return false;
		}
		return true;
	}

	std::string externalAgentTypeResourceName(std::filesystem::path const& path)
	{
		auto const source = std::filesystem::canonical(path).generic_string();
		constexpr char hex[] = "0123456789abcdef";
		std::string name = "external-agent-";
		for (unsigned char byte : source)
		{
			name += hex[byte >> 4];
			name += hex[byte & 15];
		}
		return name;
	}

	std::filesystem::path externalAgentTypeResourcePath(std::string const& name)
	{
		constexpr std::string_view prefix = "external-agent-";
		if (!name.starts_with(prefix)) return {};
		auto const encoded = std::string_view(name).substr(prefix.size());
		if (encoded.empty() || encoded.size() % 2 || encoded.size() > 32768) return {};
		auto digit = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			return -1;
		};
		std::string decoded;
		for (std::size_t i = 0; i < encoded.size(); i += 2)
		{
			auto const high = digit(encoded[i]), low = digit(encoded[i + 1]);
			if (high < 0 || low < 0 || (high == 0 && low == 0)) return {};
			decoded += static_cast<char>((high << 4) | low);
		}
		std::filesystem::path path(decoded);
		if (!path.is_absolute() || path.lexically_normal() != path
			|| !decoded.ends_with(".agent.lua")) return {};
		return path;
	}

	std::optional<AgentTypeDefinition> resolveAgentTypeResource(
		std::string const& resourceName)
	{
		if (gAgentTypeResourceLoader) return gAgentTypeResourceLoader(resourceName);
		auto const path = resolveCatalogSource("AgentType", resourceName);
		if (path.empty()) return std::nullopt;
		auto const external = !externalAgentTypeResourcePath(resourceName).empty();
		auto refuse = [&](std::string const& reason) -> std::optional<AgentTypeDefinition>
		{
			if (external) throw SerializationException("Agent type resource '" + resourceName
				+ "' (" + path.string() + "): " + reason);
			return std::nullopt;
		};
		std::error_code error;
		auto const size = std::filesystem::file_size(path, error);
		if (error) return refuse("Source file is unavailable");
		if (size > 1024u * 1024u) return refuse("Source exceeds 1 MiB");
		std::ifstream input(path, std::ios::binary);
		if (!input) return refuse("Could not read source file");
		std::string source{
			std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
		if (!input.good() && !input.eof()) return refuse("Could not read source file");
		auto preflight = AgentTypeRuntimeAdapter::preflightType(resourceName, source);
		if (!preflight.loaded || !agentTypeIdIsValid(preflight.typeId)
			|| !agentTypeDisplayNameIsValid(preflight.displayName))
			return refuse(preflight.loaded ? "Agent type identity is invalid" : preflight.diagnostic);
		AgentTypeDefinition definition;
		definition.typeId = std::move(preflight.typeId);
		definition.displayName = std::move(preflight.displayName);
		definition.resourceName = resourceName;
		definition.source = std::move(source);
		return definition;
	}

	void setAgentTypeResourceLoader(AgentTypeResourceLoader loader)
	{
		gAgentTypeResourceLoader = std::move(loader);
	}
}
