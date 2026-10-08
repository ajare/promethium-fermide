#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/SerializationException.h"

namespace core
{
	namespace
	{
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
			AgentTypeRuntimeAdapter runtime;
			auto result = runtime.construct("Human",
				bundledHumanAgentType().source, "Human");
			if (!result.succeeded)
				throw SerializationException(
					"Bundled Human type failed: " + result.diagnostic);
			return result.baseline;
		}();
		return baseline;
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
}
