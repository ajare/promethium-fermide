#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	using smoke::require;

	std::string readFile(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input) throw std::runtime_error("Cannot read fixture: " + path.string());
		return std::string{ std::istreambuf_iterator<char>(input), {} };
	}

	std::string serializeWorld(core::World const& world, bool binary)
	{
		std::unique_ptr<core::Serializer> writer = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		core::SerializationWorkData work;
		work.markSerializedUnmodified = false;
		world.serialize(*writer, work);
		writer->serialize();
		return binary
			? static_cast<core::BinarySerializer*>(writer.get())->getSerializedString()
			: static_cast<core::YamlSerializer*>(writer.get())->getSerializedString();
	}

	bool loadWorld(std::string const& bytes, bool binary, core::World& target)
	{
		std::unique_ptr<core::Serializer> reader = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		reader->deserialize();
		core::SerializationWorkData work;
		return target.deserialize(*reader, work);
	}

	std::string typeSource(std::string const& typeId, std::string const& displayName,
		std::string const& newBody)
	{
		return "return {\n"
			"    api_version = 1,\n"
			"    type_id = \"" + typeId + "\",\n"
			"    display_name = \"" + displayName + "\",\n"
			"    new = function()\n" + newBody + "\n"
			"    end,\n"
			"}\n";
	}

	std::string validBaseline(std::string const& overrides = {})
	{
		return "        return {\n"
			"            width = 0.5,\n"
			"            standing_height = 0.6,\n"
			"            reach = 0.3,\n"
			"            walk_speed = 0.4,\n"
			"            climb_speed = 0.2,\n"
			"            stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35,\n"
			"            sitting_height_ratio = 0.5,\n"
			"            crouching_height_ratio = 0.6,\n"
			"            crawling_height_ratio = 0.4,\n"
			"            crawling_speed_ratio = 0.5,\n" + overrides
			+ "        }\n";
	}

	void scriptedHumanIdentity(smoke::Context const&)
	{
		core::World world("Scripted Human", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		require(world.hasAgentType("Human"), "The bundled Human type was not registered");
		require(world.agentTypeDisplayName("Human") == "Human",
			"The bundled Human type lost its display name");
		auto const id = world.createAgent("Created human", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && std::string(agent->getTypeName()) == "Human"
			&& agent->getTypeId() == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"Scripted Human identity did not resolve");
		auto const& physical = agent->getPhysicalBaseline();
		require(physical.width == 0.4f && physical.standingHeight == 0.45f
			&& physical.reach == 0.25f && physical.walkSpeed == 0.5f
			&& physical.climbSpeed == 0.25f && physical.stairAscentSpeed == 0.35f
			&& physical.stairDescentSpeed == 0.45f && physical.sittingHeightRatio == 0.6f
			&& physical.crouchingHeightRatio == 0.6f && physical.crawlingHeightRatio == 0.3f
			&& physical.crawlingSpeedRatio == 0.5f,
			"Scripted Human baseline did not match the bundled definition");
	}

	void bundledDefinitionMatchesResource(smoke::Context const& context)
	{
		auto const resource = readFile(context.fixture("resources/test-worlds/human.agent.lua"));
		require(resource == core::bundledHumanAgentType().source,
			"The bundled Human resource drifted from the embedded definition");
		auto const definition = core::bundledHumanAgentType();
		auto preflight = core::AgentTypeRuntimeAdapter::preflightType(
			definition.resourceName, definition.source);
		require(preflight.loaded && preflight.typeId == "Human"
			&& preflight.displayName == "Human",
			"The bundled Human definition did not preflight");
	}

	void genericScriptBackedType(smoke::Context const&)
	{
		core::World world("Generic type", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()), &diagnostic),
			diagnostic.c_str());
		require(world.hasAgentType("Tall")
			&& world.agentTypeDisplayName("Tall") == "Tall Person",
			"The attached type identity did not resolve");
		auto const id = world.createAgent("Tall", "Tall one", corridor, 0, 2.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Tall"
			&& std::string(agent->getTypeName()) == "Tall Person"
			&& agent->getTypeResourceName() == "tall.agent.lua",
			"The generic script-backed Agent identity did not round-trip");
		auto const& physical = agent->getPhysicalBaseline();
		require(physical.width == 0.5f && physical.standingHeight == 0.6f
			&& physical.reach == 0.3f && physical.walkSpeed == 0.4f,
			"The generic type's frozen baseline was not applied");
	}

	void invalidBaselinesAreRejected(smoke::Context const&)
	{
		// Sources refused at preflight (attach) time: the type object itself is
		// malformed before any instance is constructed.
		struct AttachCase { std::string name; std::string source; };
		std::vector<AttachCase> attachCases;
		attachCases.push_back({ "malformed Lua", "this is not valid lua" });
		attachCases.push_back({ "missing constructor",
			"return { api_version = 1, type_id = \"NoCtor\", display_name = \"No Ctor\" }\n" });
		attachCases.push_back({ "invalid type id",
			"return { api_version = 1, type_id = \"bad id!\", display_name = \"Bad\", new = function() return {} end }\n" });
		for (auto const& test : attachCases)
		{
			core::World world("Reject", 4, 2);
			std::string diagnostic;
			require(!world.attachAgentType(test.name + ".agent.lua", test.source, &diagnostic),
				("Invalid Agent type was accepted at attach: " + test.name).c_str());
		}

		// Sources with a valid type object whose new() returns an invalid
		// baseline: attachment succeeds, but creation must refuse atomically.
		struct ConstructCase { std::string name; std::string source; };
		std::vector<ConstructCase> constructCases;
		constructCases.push_back({ "omitted field", typeSource("Missing", "Missing",
			"        return { width = 0.5, standing_height = 0.6, reach = 0.3,\n"
			"            walk_speed = 0.4, climb_speed = 0.2, stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35, sitting_height_ratio = 0.5,\n"
			"            crouching_height_ratio = 0.6, crawling_height_ratio = 0.4,\n"
			"            crawling_speed_ratio = 0.5 }\n") });
		constructCases.push_back({ "zero width", typeSource("Zero", "Zero",
			validBaseline("            width = 0.0,\n")) });
		constructCases.push_back({ "NaN reach", typeSource("NaN", "NaN",
			validBaseline("            reach = (0.0 / 0.0),\n")) });
		constructCases.push_back({ "ratio above one", typeSource("Ratio", "Ratio",
			validBaseline("            sitting_height_ratio = 1.5,\n")) });
		constructCases.push_back({ "non-table constructor", typeSource("NotTable", "Not Table",
			"        return 42\n") });
		constructCases.push_back({ "forbidden capability", typeSource("Forbidden", "Forbidden",
			"        local x = setmetatable({}, {})\n" + validBaseline()) });
		for (auto const& test : constructCases)
		{
			core::World world("Reject", 6, 2);
			auto const corridor = world.addCorridor(0, 0, 6);
			world.finishBuild();
			std::string diagnostic;
			require(world.attachAgentType(test.name + ".agent.lua", test.source, &diagnostic),
				("Valid type object was refused at attach: " + test.name + " (" + diagnostic + ")").c_str());
			bool threw = false;
			try
			{
				(void)world.createAgent(test.name, "Broken", corridor, 0, 1.0f);
			}
			catch (std::exception const&)
			{
				threw = true;
			}
			require(threw, ("Invalid baseline was accepted at creation: " + test.name).c_str());
			require(world.getSimulationSnapshot().agents.empty()
				&& world.getSector(corridor)->getAgents().empty(),
				("Invalid baseline left a partial Agent: " + test.name).c_str());
		}
	}

	void constructorFailureLeavesNoPartialAgent(smoke::Context const&)
	{
		core::World world("Constructor failure", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("boom.agent.lua",
			typeSource("Boom", "Boom", "        error(\"constructor failed\")\n"),
			&diagnostic), diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Boom", "Broken", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "A throwing constructor published an Agent");
		require(world.getSimulationSnapshot().agents.empty()
			&& world.getSector(corridor)->getAgents().empty(),
			"A throwing constructor left a partial Agent");
	}

	void executionBudgetIsEnforced(smoke::Context const&)
	{
		core::World world("Budget", 4, 2, {},
			core::AgentTypeRuntimeLimits{ 64u * 1024u * 1024u, 2000u });
		auto const corridor = world.addCorridor(0, 0, 4);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("loop.agent.lua",
			typeSource("Loop", "Loop", "        while true do end\n"), &diagnostic),
			diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Loop", "Spinner", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "An unbounded constructor was not stopped by the instruction budget");
		require(world.getSimulationSnapshot().agents.empty(),
			"A budgeted failure left a partial Agent");
	}

	void allocationBudgetIsEnforced(smoke::Context const&)
	{
		core::World world("Memory budget", 4, 2, {},
			core::AgentTypeRuntimeLimits{ 512u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 4);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("hungry.agent.lua",
			typeSource("Hungry", "Hungry",
				"        local blob = string.rep(\"x\", 16 * 1024 * 1024)\n"
				+ validBaseline()), &diagnostic), diagnostic.c_str());
		bool threw = false;
		try
		{
			(void)world.createAgent("Hungry", "Hungry", corridor, 0, 1.0f);
		}
		catch (std::exception const&)
		{
			threw = true;
		}
		require(threw, "A huge allocation was not stopped by the memory budget");
		require(world.getSimulationSnapshot().agents.empty(),
			"A memory-budget failure left a partial Agent");
	}

	void instancesAreIsolated(smoke::Context const&)
	{
		core::World world("Isolation", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		// A module-level counter proves isolation: each new() must start from a
		// fresh environment, so every instance observes width == 1.0 rather than
		// a shared, incrementing counter.
		std::string const counter = "        local count = 0\n"
			"        count = count + 1\n"
			"        return {\n"
			"            width = count,\n"
			"            standing_height = 0.6, reach = 0.3, walk_speed = 0.4,\n"
			"            climb_speed = 0.2, stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35, sitting_height_ratio = 0.5,\n"
			"            crouching_height_ratio = 0.6, crawling_height_ratio = 0.4,\n"
			"            crawling_speed_ratio = 0.5 }\n";
		std::string diagnostic;
		require(world.attachAgentType("counter.agent.lua",
			typeSource("Counter", "Counter", counter), &diagnostic), diagnostic.c_str());
		auto const first = world.createAgent("Counter", "First", corridor, 0, 1.0f);
		auto const second = world.createAgent("Counter", "Second", corridor, 0, 2.0f);
		require(world.lookupAgent(first).entity->getPhysicalBaseline().width == 1.0f
			&& world.lookupAgent(second).entity->getPhysicalBaseline().width == 1.0f,
			"Mutable module state leaked between Agent instances");
	}

	void legacyAndScriptedHumanLoading(smoke::Context const&)
	{
		core::World original("Document identity", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		auto const id = original.createAgent("Saved human", sector, 0, 1.0f);
		(void)id;
		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Scripted Human World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getTypeId() == "Human"
				&& std::string(agent->getTypeName()) == "Human",
				"Scripted Human identity was lost on load");
		}
		// An explicit missing resource is refused, never silently Human.
		auto yaml = serializeWorld(original, false);
		auto const resourceAt = yaml.find("resource: human.agent.lua");
		require(resourceAt != std::string::npos, "Saved Agent omitted its resource reference");
		yaml.replace(resourceAt, std::string("resource: human.agent.lua").size(),
			"resource: missing.agent.lua");
		core::World refused("refused", 2, 2);
		bool rejected = false;
		try
		{
			(void)loadWorld(yaml, false, refused);
		}
		catch (core::SerializationException const& error)
		{
			rejected = std::string(error.what()).find("missing.agent.lua") != std::string::npos;
		}
		require(rejected, "A missing Agent type resource was not refused clearly");
	}

	void duplicateTypeIdIsRejected(smoke::Context const&)
	{
		core::World world("Duplicate", 4, 2);
		std::string diagnostic;
		require(!world.attachAgentType("other-human.agent.lua",
			typeSource("Human", "Other Human", validBaseline()), &diagnostic),
			"A duplicate Human type ID was accepted");
		require(world.agentTypeDisplayName("Human") == "Human",
			"A refused duplicate replaced the bundled Human");
	}

	void resetReconstructsScriptedInstances(smoke::Context const&)
	{
		core::World world("Reset", 6, 2);
		auto const sector = world.addCorridor(0, 0, 6);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()), &diagnostic),
			diagnostic.c_str());
		auto const id = world.createAgent("Tall", "Tall one", sector, 0, 1.0f);
		auto const before = world.lookupAgent(id).entity->getPhysicalBaseline().width;
		world.resetSimulation();
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Tall"
			&& agent->getPhysicalBaseline().width == before,
			"Reset lost or changed the scripted Agent's frozen baseline");
	}
}

void agent_smoke::registerAgentTypes(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentTypesScriptedHumanIdentity", scriptedHumanIdentity });
	checks.push_back({ "agentTypesBundledDefinitionMatchesResource", bundledDefinitionMatchesResource });
	checks.push_back({ "agentTypesGenericScriptBackedType", genericScriptBackedType });
	checks.push_back({ "agentTypesInvalidBaselinesRejected", invalidBaselinesAreRejected });
	checks.push_back({ "agentTypesConstructorFailureLeavesNoPartialAgent", constructorFailureLeavesNoPartialAgent });
	checks.push_back({ "agentTypesExecutionBudgetEnforced", executionBudgetIsEnforced });
	checks.push_back({ "agentTypesAllocationBudgetEnforced", allocationBudgetIsEnforced });
	checks.push_back({ "agentTypesInstancesAreIsolated", instancesAreIsolated });
	checks.push_back({ "agentTypesLegacyAndScriptedLoading", legacyAndScriptedHumanLoading });
	checks.push_back({ "agentTypesDuplicateTypeIdRejected", duplicateTypeIdIsRejected });
	checks.push_back({ "agentTypesResetReconstructsScriptedInstances", resetReconstructsScriptedInstances });
}
