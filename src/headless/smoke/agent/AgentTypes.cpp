#include "Checks.h"
#include "core/World.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/AgentTypeRuntime.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "core/WorldDocument.h"
#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"

#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
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
			"            crawling_speed_ratio = 0.5,\n"
			"            mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' },\n" + overrides
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
		// The former Human-only query factory now accepts any resolved resource,
		// retains an isolated instance, and does not publish to the World.
		core::AgentTypeDefinition definition{ "Tall", "Tall Person", "tall.agent.lua",
			typeSource("Tall", "Tall Person", validBaseline()) };
		auto query = core::Agent::create(definition, "Query");
		require(query->getTypeId() == "Tall" && query->getWidth() == agent->getWidth()
			&& query->getWalkSpeed() == agent->getWalkSpeed()
			&& world.getSimulationSnapshot().agents.size() == 1,
			"Resource-backed query factory refused an arbitrary type or published an Agent");
		require(world.attachAgentType("unit.agent.lua", typeSource("Unit", "Unit",
			validBaseline("sitting_height_ratio=1, crouching_height_ratio=1,\n"
				"crawling_height_ratio=1, crawling_speed_ratio=1,\n")), &diagnostic), diagnostic);
		auto unit = world.lookupAgent(world.createAgent("Unit", "Unit", corridor, 0, 4.f)).entity;
		require(unit->getPhysicalBaseline().crawlingSpeedRatio == 1.f
			&& unit->getTraversalCrawlingDoorClearanceExtent(true) == unit->getStandingHeight(),
			"The inclusive ratio upper boundary was refused or changed");
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
		attachCases.push_back({ "invalid constructor",
			"return { api_version = 1, type_id = 'NoCtor', display_name = 'No Ctor', new = 42 }" });
		attachCases.push_back({ "module exception", "error('module refused')" });
		attachCases.push_back({ "invalid type id",
			"return { api_version = 1, type_id = \"bad id!\", display_name = \"Bad\", new = function() return {} end }\n" });
		for (auto const& test : attachCases)
		{
			core::World world("Reject", 4, 2);
			std::string diagnostic;
			require(!world.attachAgentType(test.name + ".agent.lua", test.source, &diagnostic)
				&& !diagnostic.empty(),
				("Invalid Agent type was accepted or undiagnosed at attach: " + test.name).c_str());
		}

		// Sources with a valid type object whose new() returns an invalid
		// baseline: attachment succeeds, but creation must refuse atomically.
		struct ConstructCase { std::string name; std::string body; std::string expected; };
		std::vector<ConstructCase> constructCases;
		// Every field is tested, and every failure names the offending field.
		for (auto const* field : { "width", "standing_height", "reach", "walk_speed",
			"climb_speed", "stair_ascent_speed", "stair_descent_speed",
			"sitting_height_ratio", "crouching_height_ratio", "crawling_height_ratio",
			"crawling_speed_ratio" })
		{
			for (auto const* value : { "nil", "0", "-1", "0/0", "math.huge",
				"-math.huge", "false", "'0.5'", "{}", "1e-300" })
				constructCases.push_back({ std::string(field) + " = " + value,
					validBaseline(std::string(field) + " = " + value + ",\n"), field });
			constructCases.push_back({ std::string(field) + " overflow",
				validBaseline(std::string(field) + " = 1e300,\n"), field });
			if (std::string_view(field).ends_with("ratio"))
				constructCases.push_back({ std::string(field) + " above one",
					validBaseline(std::string(field) + " = 1.01,\n"), field });
		}
		for (auto const* value : { "42", "nil", "false", "function() end", "'instance'" })
			constructCases.push_back({ std::string("non-table ") + value,
				std::string("return ") + value, "instance table" });
		constructCases.push_back({ "constructor exception", "error('constructor refused')",
			"constructor refused" });
		for (auto const* capability : { "io.open('unsafe')", "os.execute('unsafe')",
			"debug.getregistry()", "package.loadlib('unsafe', 'unsafe')",
			"require('unsafe')", "dofile('unsafe')", "loadfile('unsafe')",
			"load('unsafe')", "setmetatable({}, {})", "collectgarbage()" })
			constructCases.push_back({ capability,
				std::string("local forbidden = ") + capability + "\n" + validBaseline(), "" });
		for (auto const& test : constructCases)
		{
			core::World world("Reject", 6, 2);
			auto const corridor = world.addCorridor(0, 0, 6);
			world.finishBuild();
			std::string diagnostic;
			require(world.attachAgentType("invalid.agent.lua",
				typeSource("Invalid", "Invalid", test.body), &diagnostic),
				("Valid type object was refused at attach: " + test.name + " (" + diagnostic + ")").c_str());
			bool threw = false;
			try
			{
				(void)world.createAgent("Invalid", "Broken", corridor, 0, 1.0f);
			}
			catch (std::exception const& error)
			{
				diagnostic = error.what();
				threw = diagnostic.find("invalid.agent.lua") != std::string::npos
					&& diagnostic.find(test.expected) != std::string::npos;
			}
			require(threw, ("Invalid construction was not diagnosed: " + test.name + ": " + diagnostic).c_str());
			require(world.getSimulationSnapshot().agents.empty()
				&& world.getSector(corridor)->getAgents().empty(),
				("Invalid baseline left a partial Agent: " + test.name).c_str());
		}
	}

	void scriptedMobilityProfiles(smoke::Context const&)
	{
		core::World world("Script defaults", 6, 2);
		auto const corridor = world.addCorridor(0, 0, 6);
		world.finishBuild();
		std::string diagnostic;
		std::string scriptProfile = "{ staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'cannot_use', buttons = 'only_if_no_other_option' }";
		require(world.attachAgentType("limited.agent.lua", typeSource("Limited", "Limited",
			validBaseline("mobility_profile = " + scriptProfile + ",\n")), &diagnostic), diagnostic);
		core::AgentId id;
		try { id = world.createAgent("Limited", "Limited", corridor, 0, 1.f); }
		catch (std::exception const& error) { require(false, error.what()); }
		auto* agent = world.lookupAgent(id).entity;
		require(agent && agent->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door)
			== core::MobilityUse::CannotUse && agent->getEffectiveMobilityProfile().value
			== agent->getScriptDefaultMobilityProfile(),
			"A script Mobility default was not frozen or used without a tag registry");
		world.pauseSimulation();
		core::MobilityProfile individual;
		individual.set(core::TraversalKind::Lift, core::MobilityUse::CannotUse);
		require(world.setAgentIndividualMobilityProfile(id, individual, &diagnostic)
			&& agent->getEffectiveMobilityProfile().value == individual, diagnostic);
		require(world.setAgentIndividualMobilityProfile(id, std::nullopt, &diagnostic)
			&& agent->getEffectiveMobilityProfile().value == agent->getScriptDefaultMobilityProfile(), diagnostic);

		uint32_t invalidIndex = 0;
		for (auto const& invalid : std::vector<std::pair<std::string, std::string>>{
			{ "missing", "nil" }, { "malformed", "42" },
			{ "unknown", "{ staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use', jetpack = 'can_use' }" },
			{ "invalid-use", "{ staircase = 'bad', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' }" } })
		{
			auto const typeId = "Bad" + std::to_string(++invalidIndex);
			core::World rejected("Reject profile", 4, 2);
			auto const location = rejected.addCorridor(0, 0, 4);
			rejected.finishBuild();
			require(rejected.attachAgentType(invalid.first + ".agent.lua", typeSource(typeId,
				"Bad", validBaseline("mobility_profile = " + invalid.second + ",\n")), &diagnostic), diagnostic);
			bool threw = false;
			try { (void)rejected.createAgent(typeId, "Bad", location, 0, 1.f); }
			catch (std::exception const& error) { threw = std::string(error.what()).find(invalid.first + ".agent.lua") != std::string::npos; }
			require(threw && rejected.getSimulationSnapshot().agents.empty(),
				"Invalid script Mobility profile was published or lacked resource diagnostics");
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
		for (auto const* guard : { "pcall", "xpcall" })
		{
			auto const body = std::string(guard) + "(function() while true do end end"
				+ (std::string_view(guard) == "xpcall" ? ", function(e) return e end" : "")
				+ ")\n" + validBaseline();
			require(world.attachAgentType(std::string(guard) + ".agent.lua",
				typeSource(guard, guard, body), &diagnostic), diagnostic);
			bool refused = false;
			try { world.createAgent(guard, "Caught budget", corridor, 0, 1.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.empty(),
				"A protected call swallowed terminal instruction-budget exhaustion");
		}
		require(!!world.createAgent("Recovery", corridor, 0, 1.f),
			"Instruction-budget failure poisoned subsequent creation");
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
		for (auto const* guard : { "pcall", "xpcall" })
		{
			auto const body = std::string(guard)
				+ "(function() local blob = string.rep('x', 16 * 1024 * 1024) end"
				+ (std::string_view(guard) == "xpcall" ? ", function(e) return e end" : "")
				+ ")\n" + validBaseline();
			require(world.attachAgentType(std::string(guard) + ".agent.lua",
				typeSource(guard, guard, body), &diagnostic), diagnostic);
			bool refused = false;
			try { world.createAgent(guard, "Caught budget", corridor, 0, 1.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.empty(),
				"A protected call swallowed terminal allocation-budget exhaustion");
		}
		require(!!world.createAgent("Recovery", corridor, 0, 1.f),
			"Allocation-budget failure poisoned subsequent creation");
	}

	void instancesAreIsolated(smoke::Context const&)
	{
		core::World world("Isolation", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		// A module-level counter proves isolation: each new() must start from a
		// fresh environment, so every instance observes width == 1.0 rather than
		// a shared, incrementing counter.
		std::string const counter = "        count = count + 1\n"
			"        return {\n"
			"            width = count,\n"
			"            standing_height = 0.6, reach = 0.3, walk_speed = 0.4,\n"
			"            climb_speed = 0.2, stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35, sitting_height_ratio = 0.5,\n"
			"            crouching_height_ratio = 0.6, crawling_height_ratio = 0.4,\n"
			"            crawling_speed_ratio = 0.5,\n"
			"            mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' } }\n";
		std::string diagnostic;
		require(world.attachAgentType("counter.agent.lua",
			"local count = 0\n" + typeSource("Counter", "Counter", counter), &diagnostic), diagnostic.c_str());
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

	void resetRevisionAndAuthoredData(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const path = root / "revision.agent.lua";
		auto write = [&](float width) {
			std::ofstream out(path);
			// Both module upvalues and instance-private data are fresh each time.
			out << "local count = 0\n" << typeSource("Revision",
				width > 0.5f ? "Revised display" : "Original display",
				"count = count + 1\n" + validBaseline(
					"width = " + std::to_string(width) + " * count,\n"
					"private_state = { count = count },\n"));
			require(bool(out), "Could not write revision fixture");
		};
		write(0.5f);
		auto const resource = core::externalAgentTypeResourceName(path);
		core::World world("Revision", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		world.pauseSimulation();
		auto definition = core::resolveAgentTypeResource(resource);
		std::string diagnostic;
		require(definition && world.attachAgentType(resource, definition->source, &diagnostic), diagnostic);
		auto first = world.createAgent("Revision", "First", corridor, 0, 1.f);
		auto second = world.createAgent("Revision", "Second", corridor, 0, 3.f);
		auto registry = core::AgentTagRegistry::create();
		auto tag = registry->addAgentTag("physical");
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(tag, { 0.7f, 0.9f }, &diagnostic), diagnostic);
		auto const registryPath = root / "revision.tags.yaml";
		registry->saveTo(registryPath.string());
		world.attachAgentTagRegistry(registryPath.filename().string(), registry);
		require(world.assignAgentTag(first, tag, &diagnostic), diagnostic);
		require(world.setAgentIndividualWalkSpeedModifier(first, 1.2f), "Could not author speed");
		require(world.setAgentIndividualHeightModifier(first, 0.95f), "Could not author height");
		auto const sample = world.lookupAgent(first).entity->getHeightModifierSample();
		world.saveTo((root / "revision.world.yaml").string());
		world.saveTo((root / "revision.world").string());
		auto const authored = serializeWorld(world, false);
		write(0.75f);
		require(world.lookupAgent(first).entity->getPhysicalBaseline().width == 0.5f
			&& world.lookupAgent(second).entity->getPhysicalBaseline().width == 0.5f,
			"On-disk edits changed a live frozen baseline");
		for (auto const* filename : { "revision.world.yaml", "revision.world" })
		{
			auto loaded = core::loadWorldDocument(root / filename);
			require(loaded->lookupAgent(first).entity->getPhysicalBaseline().width == 0.75f
				&& loaded->lookupAgent(second).entity->getPhysicalBaseline().width == 0.75f
				&& std::string(loaded->lookupAgent(first).entity->getTypeName()) == "Revised display",
				"Load reused a baseline snapshot, refused revised display, or shared mutable module state");
		}
		world.resetSimulation();
		for (auto id : { first, second })
		{
			auto const* agent = world.lookupAgent(id).entity;
			require(agent && agent->getTypeId() == "Revision"
				&& agent->getTypeResourceName() == resource
				&& agent->getPhysicalBaseline().width == 0.75f,
				"Reset did not reconstruct isolated instances from the current revision");
		}
		auto const* agent = world.lookupAgent(first).entity;
		require(agent->getHeightModifierSample() == sample
			&& agent->getIndividualHeightModifier() == 0.95f
			&& agent->getIndividualWalkSpeedModifier() == 1.2f
			&& agent->getAgentTagIds().contains(tag), "Reset changed authored properties or tag samples");
		require(world.isSimulationPaused() && !world.isModified()
			&& serializeWorld(world, false) == authored,
			"Reset changed authored document data, pause or dirty state");
		require(authored.find("private_state") == std::string::npos
			&& authored.find("standing_height") == std::string::npos,
			"Document serialized private state or a baseline snapshot");
	}

	void resetFailureIsAtomic(smoke::Context const& context)
	{
		auto const path = context.temporaryRoot() / "failure.agent.lua";
		auto const valid = typeSource("Failure", "Failure", validBaseline());
		auto write = [&](std::string const& source) {
			std::ofstream out(path); out << source;
			require(bool(out), "Could not write failure fixture");
		};
		write(valid);
		auto const resource = core::externalAgentTypeResourceName(path);
		core::World world("Atomic reset", 8, 2, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		// Human preconstruction succeeds before the second type is refused.
		auto const first = world.createAgent("First", corridor, 0, 1.f);
		std::string diagnostic;
		require(world.attachAgentType(resource, valid, &diagnostic), diagnostic);
		auto const second = world.createAgent("Failure", "Second", corridor, 0, 3.f);
		auto const third = world.createAgent("Failure", "Third", corridor, 0, 5.f);
		world.update(0.1);
		auto const* originalFirst = world.lookupAgent(first).entity;
		auto const* originalSecond = world.lookupAgent(second).entity;
		auto const* originalThird = world.lookupAgent(third).entity;
		auto const authored = serializeWorld(world, false);
		auto const tick = world.getSimulationSnapshot().tick;
		auto const paused = world.isSimulationPaused();
		auto const modified = world.isModified();
		for (auto const& source : {
			std::string("invalid Lua"),
			typeSource("Failure", "Failure", "error('Reset constructor refused')"),
			typeSource("Failure", "Failure", validBaseline("width = 0,\n")),
			typeSource("WrongIdentity", "Wrong", validBaseline()),
			typeSource("Failure", "Failure", "while true do end"),
			typeSource("Failure", "Failure", validBaseline("private_blob = string.rep('x', 128 * 1024 * 1024),\n")),
			// One revised constructor fits; the next instance exceeds the total
			// runtime budget. Failure must discard the whole candidate set.
			typeSource("Failure", "Failure", validBaseline("private_blob = string.rep('x', 800 * 1024),\n")),
			std::string{} })
		{
			write(source);
			if (source.empty()) std::filesystem::remove(path);
			bool refused = false;
			try { world.resetSimulation(); }
			catch (std::exception const& error) {
				auto const message = std::string(error.what());
				refused = message.find(resource) != std::string::npos;
			}
			require(refused, "Reset failure did not diagnose its resource");
			require(world.lookupAgent(first).entity == originalFirst
				&& world.lookupAgent(second).entity == originalSecond
				&& world.lookupAgent(third).entity == originalThird
				&& world.getSimulationSnapshot().tick == tick
				&& world.isSimulationPaused() == paused && world.isModified() == modified
				&& serializeWorld(world, false) == authored,
				"A refused Reset partially reconstructed or rewound the World");
		}
		write(valid);
		world.resetSimulation();
		require(world.lookupAgent(second).entity->getPhysicalBaseline().width == 0.5f,
			"A failed Reset poisoned subsequent reconstruction");
	}

	void resetReleasesReplacedInstances(smoke::Context const&)
	{
		core::World world("Reset lifetime", 8, 2, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("private.agent.lua", typeSource("Private", "Private",
			validBaseline("private_blob = string.rep('x', 800 * 1024),\n")), &diagnostic), diagnostic);
		auto const id = world.createAgent("Private", "Instance", corridor, 0, 1.f);
		for (int cycle = 0; cycle < 4; ++cycle)
		{
			world.resetSimulation(); // Must not charge both old and fresh instances to one runtime.
			require(world.lookupAgent(id).entity->getPhysicalBaseline().width == 0.5f,
				"Reset lost its private instance's baseline");
			bool refused = false;
			try { world.createAgent("Private", "Duplicate", corridor, 0, 3.f); }
			catch (std::exception const&) { refused = true; }
			require(refused && world.getSimulationSnapshot().agents.size() == 1,
				"Reset discarded the private instance or failed to preserve its runtime budget");
		}
	}

	void resolvedResourceIdentity(smoke::Context const&)
	{
		auto const definition = core::resolveAgentTypeResource("human.agent.lua");
		require(definition.has_value(),
			"The bundled Human Agent type resource did not resolve");
		require(definition->typeId == "Human" && definition->displayName == "Human"
			&& definition->resourceName == "human.agent.lua",
			"The resolved bundled Human resource lost its identity");
		require(definition->source == core::bundledHumanAgentType().source,
			"The resolved bundled Human resource drifted from the embedded definition");
	}

	void resolvedResourceUnavailable(smoke::Context const&)
	{
		require(!core::resolveAgentTypeResource("no-such.agent.lua").has_value(),
			"An unknown Agent type resource resolved");
	}

	void previewMatchesPlacement(smoke::Context const&)
	{
		core::World world("Preview agreement", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const id = world.createAgent("Human", "Placed", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent != nullptr, "The placed Human was not found");
		auto const& baseline = agent->getPhysicalBaseline();
		auto const preview = core::Agent::placementDimensions(core::bundledHumanBaseline());
		require(preview.x == baseline.width && preview.y == baseline.standingHeight,
			"The preview dimensions did not match the scripted placement baseline");
		auto const modified = core::Agent::placementDimensions(core::bundledHumanBaseline(), 1.5f);
		require(modified.x == baseline.width
			&& modified.y == baseline.standingHeight * 1.5f,
			"The preview did not apply the height modifier to the scripted baseline");
	}

	void scriptedAuthorizationPlacement(smoke::Context const&)
	{
		core::World world("Authorization", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const id = world.createAgent("Human", "Granted", corridor, 0, 1.0f,
			std::set<core::AccessPermissionId>{}, std::set<core::PermissionSetId>{});
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"The scripted authorization placement lost Human identity");
		require(agent->getPhysicalBaseline().walkSpeed
				== core::bundledHumanBaseline().walkSpeed,
			"The scripted authorization placement did not use the scripted baseline");
	}

	std::string removeLineContaining(std::string text, std::string const& needle)
	{
		auto const pos = text.find(needle);
		if (pos == std::string::npos) return text;
		auto lineStart = text.rfind('\n', pos);
		lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
		auto lineEnd = text.find('\n', pos);
		lineEnd = lineEnd == std::string::npos ? text.size() : lineEnd + 1;
		text.erase(lineStart, lineEnd - lineStart);
		return text;
	}

	// Loads every valid manifest-registered Agent type resource and returns the
	// Scout fixture definition the same way the editor does at startup.
	std::optional<core::AgentTypeDefinition> scoutDefinition(
		smoke::Context const& context)
	{
		auto const source = readFile(context.fixture("resources/test-worlds/scout.agent.lua"));
		auto const resolved = core::resolveAgentTypeResource("scout.agent.lua");
		if (!resolved || resolved->typeId != "Scout" || resolved->displayName != "Scout"
			|| resolved->resourceName != "scout.agent.lua" || resolved->source != source)
			return std::nullopt;
		return resolved;
	}

	void fixtureTypePhysicalOutcomes(smoke::Context const& context)
	{
		auto const resolved = scoutDefinition(context);
		require(resolved.has_value(), "The Scout Agent type resource did not resolve");

		core::World world("Scout physical", 8, 2);
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		std::string diagnostic;
		require(world.attachAgentType("scout.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		require(world.hasAgentType("Scout")
			&& world.agentTypeDisplayName("Scout") == "Scout",
			"The Scout type identity did not resolve");
		auto const id = world.createAgent("Scout", "Runner", corridor, 0, 1.0f);
		auto const* agent = world.lookupAgent(id).entity;
		require(agent && agent->getTypeId() == "Scout"
			&& std::string(agent->getTypeName()) == "Scout"
			&& agent->getTypeResourceName() == "scout.agent.lua",
			"The placed Scout identity did not resolve");

		auto const& physical = agent->getPhysicalBaseline();
		auto const& human = core::bundledHumanBaseline();
		require(physical.width == 0.3f && physical.standingHeight == 0.35f
			&& physical.reach == 0.4f && physical.walkSpeed == 0.9f
			&& physical.climbSpeed == 0.5f && physical.stairAscentSpeed == 0.6f
			&& physical.stairDescentSpeed == 0.7f && physical.sittingHeightRatio == 0.5f
			&& physical.crouchingHeightRatio == 0.5f && physical.crawlingHeightRatio == 0.25f
			&& physical.crawlingSpeedRatio == 0.75f,
			"Scout's frozen baseline did not match the fixture");
		require(physical.width != human.width && physical.standingHeight != human.standingHeight
			&& physical.reach != human.reach && physical.walkSpeed != human.walkSpeed,
			"Scout is not distinctly shaped and speeded from Human");

		// The highest available public consumers read the frozen baseline.
		require(agent->getWalkSpeed() == physical.walkSpeed
			&& agent->getClimbSpeed() == physical.climbSpeed,
			"Scout movement did not use its frozen baseline");
		auto const bounds = agent->getBounds().getSize();
		auto near = [](float left, float right)
		{
			return std::fabs(left - right) < 1e-5f;
		};
		require(near(bounds.x, physical.width) && near(bounds.y, physical.standingHeight),
			"Scout bounds did not use its frozen baseline");
		require(agent->getTraversalCrawlingDoorClearanceExtent(true)
				== physical.standingHeight * physical.crawlingHeightRatio,
			"Scout Crawling clearance did not use its frozen baseline");
	}

	void fixturePersistenceRoundTrip(smoke::Context const& context)
	{
		auto const resolved = scoutDefinition(context);
		require(resolved.has_value(), "The Scout Agent type resource did not resolve");

		core::World original("Scout document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("scout.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		auto const id = original.createAgent("Scout", "Saved scout", sector, 0, 1.0f);
		(void)id;

		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Scout World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getTypeId() == "Scout"
				&& agent->getTypeResourceName() == "scout.agent.lua"
				&& std::string(agent->getTypeName()) == "Scout",
				"Scout type/resource identity was not preserved");
			auto const& physical = agent->getPhysicalBaseline();
			require(physical.width == 0.3f && physical.walkSpeed == 0.9f
				&& physical.reach == 0.4f,
				"Scout's frozen baseline was not reconstructed on load");
		}

		auto const yaml = serializeWorld(original, false);
		require(yaml.find("typeId: Scout") != std::string::npos
			&& yaml.find("resource: scout.agent.lua") != std::string::npos,
			"The saved document omitted the Scout type ID or resource reference");
		require(yaml.find("walk_speed") == std::string::npos
			&& yaml.find("standing_height") == std::string::npos
			&& yaml.find("crawling_height_ratio") == std::string::npos,
			"The saved document embedded a physical-baseline snapshot");
	}

	void loadingRefusesMismatchedOrMissingResource(smoke::Context const& context)
	{
		auto const resolved = scoutDefinition(context);
		require(resolved.has_value(), "The Scout Agent type resource did not resolve");

		core::World original("Refusal document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("scout.agent.lua", resolved->source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Scout", "Scout", sector, 0, 1.0f);

		auto const yaml = serializeWorld(original, false);

		// A mismatched type ID is refused and publishes no partial Agent.
		auto mismatched = yaml;
		auto const typeIdAt = mismatched.find("typeId: Scout");
		require(typeIdAt != std::string::npos, "Saved Scout omitted its type ID");
		mismatched.replace(typeIdAt, std::string("typeId: Scout").size(), "typeId: NotScout");
		core::World refusedType("refused type", 2, 2);
		bool typeRejected = false;
		try
		{
			(void)loadWorld(mismatched, false, refusedType);
		}
		catch (core::SerializationException const& error)
		{
			typeRejected = std::string(error.what()).find("NotScout") != std::string::npos;
		}
		require(typeRejected, "A mismatched Agent type ID was not refused clearly");
		require(refusedType.getSimulationSnapshot().agents.empty(),
			"A mismatched type ID left a partial Agent");

		// A missing resource reference is refused and publishes no partial Agent.
		auto missing = yaml;
		auto const resourceAt = missing.find("resource: scout.agent.lua");
		require(resourceAt != std::string::npos, "Saved Scout omitted its resource reference");
		missing.replace(resourceAt, std::string("resource: scout.agent.lua").size(),
			"resource: missing.agent.lua");
		core::World refusedResource("refused resource", 2, 2);
		bool resourceRejected = false;
		try
		{
			(void)loadWorld(missing, false, refusedResource);
		}
		catch (core::SerializationException const& error)
		{
			resourceRejected = std::string(error.what()).find("missing.agent.lua") != std::string::npos;
		}
		require(resourceRejected, "A missing Agent type resource was not refused clearly");
		require(refusedResource.getSimulationSnapshot().agents.empty(),
			"A missing resource left a partial Agent");
	}

	void competingTypeIdsRejectedOnLoad(smoke::Context const& context)
	{
		auto const scout = scoutDefinition(context);
		require(scout.has_value(), "The Scout Agent type resource did not resolve");

		core::World original("Competing", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("scout.agent.lua", scout->source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Scout", "First", sector, 0, 1.0f);
		(void)original.createAgent("Scout", "Second", sector, 0, 2.0f);

		auto yaml = serializeWorld(original, false);
		auto const needle = std::string("resource: scout.agent.lua");
		auto const pos = yaml.rfind(needle);
		require(pos != std::string::npos, "The second Scout omitted its resource reference");
		yaml.replace(pos, needle.size(), "resource: rival.agent.lua");

		// A rival resource declaring the same type ID must be rejected when it
		// would compete with the already-resolved resource inside one World.
		core::AgentTypeDefinition rival = *scout;
		rival.resourceName = "rival.agent.lua";
		rival.displayName = "Scout";
		rival.source = scout->source;
		struct LoaderScope
		{
			explicit LoaderScope(core::AgentTypeDefinition scoutDef,
				core::AgentTypeDefinition rivalDef)
			{
				core::setAgentTypeResourceLoader(
					[scoutDef = std::move(scoutDef), rivalDef = std::move(rivalDef)](
						std::string const& name) -> std::optional<core::AgentTypeDefinition>
					{
						if (name == "scout.agent.lua") return scoutDef;
						if (name == "rival.agent.lua") return rivalDef;
						return std::nullopt;
					});
			}
			~LoaderScope() { core::setAgentTypeResourceLoader({}); }
		};

		core::World restored("restored", 2, 2);
		bool rejected = false;
		{
			LoaderScope scope{ *scout, rival };
			try
			{
				(void)loadWorld(yaml, false, restored);
			}
			catch (core::SerializationException const& error)
			{
				rejected = std::string(error.what()).find("competing") != std::string::npos;
			}
		}
		require(rejected,
			"Competing resources with the same type ID were not rejected on load");
	}

	void legacyHumanWithoutResourceResolves(smoke::Context const&)
	{
		core::World original("Legacy identity", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		auto const id = original.createAgent("Saved human", sector, 0, 1.0f);
		require(original.setAgentIndividualWalkSpeedModifier(id, 1.2f),
			"Could not author an individual Walk speed modifier");

		auto yaml = removeLineContaining(serializeWorld(original, false),
			"resource: human.agent.lua");
		yaml = removeLineContaining(yaml, "typeId: Human");
		yaml = removeLineContaining(yaml, "type: Human");
		require(yaml.find("resource: human.agent.lua") == std::string::npos,
			"The resource reference was not stripped");

		core::World restored("restored", 2, 2);
		require(loadWorld(yaml, false, restored), "The legacy Human World did not load");
		auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
		require(agent && agent->getTypeId() == "Human"
			&& std::string(agent->getTypeName()) == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"The legacy Human did not resolve to the bundled Human");
		require(agent->getPhysicalBaseline().walkSpeed == core::bundledHumanBaseline().walkSpeed,
			"The legacy Human did not resolve to the bundled baseline");
		require(agent->getIndividualWalkSpeedModifier()
			&& *agent->getIndividualWalkSpeedModifier() == 1.2f,
			"The legacy Human lost an authored individual property");
	}

	void topologyReplayPreservesLiveInstances(smoke::Context const&)
	{
		// The public allocation budget makes a second live construction fail.
		// Replay must carry the existing private object, not duplicate it (nor
		// drop it before construction). No VM inspection or method API is needed.
		core::World world("Retained instance", 12, 3, {},
			core::AgentTypeRuntimeLimits{ 2u * 1024u * 1024u, 100'000u });
		auto corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		world.pauseSimulation();
		std::string diagnostic;
		require(world.attachAgentType("retained.agent.lua",
			typeSource("Retained", "Distinct display name", validBaseline(
				"            private_blob = string.rep('x', 800 * 1024),\n")),
			&diagnostic), diagnostic.c_str());
		core::AgentId id;
		try { id = world.createAgent("Retained", "Survivor", corridor, 0, 2.0f); }
		catch (std::exception const& error) { throw std::runtime_error(std::string("Initial construction: ") + error.what()); }
		require(world.setAgentIndividualWalkSpeedModifier(id, 1.2f),
			"Could not author a survivor property");
		require(world.setAgentActive(id, false), "Could not deactivate survivor");
		auto refuseDuplicate = [&] {
			bool refused = false;
			try { (void)world.createAgent("Retained", "Duplicate", corridor, 0, 4.0f); }
			catch (std::exception const&) { refused = true; }
			require(refused, "The retained private object no longer consumes its instance budget");
		};
		refuseDuplicate();
		auto verify = [&] {
			auto const* agent = world.lookupAgent(id).entity;
			require(agent && agent->getTypeId() == "Retained"
				&& std::string(agent->getTypeName()) == "Distinct display name"
				&& agent->getTypeResourceName() == "retained.agent.lua"
				&& agent->getPhysicalBaseline().width == 0.5f
				&& agent->getIndividualWalkSpeedModifier() == 1.2f && !agent->isActive(),
				"Replay lost survivor identity, baseline, or authored/runtime state");
		};
		world.addLevel();
		verify();
		auto resize = world.planResizeLocation(corridor, 0, 0, 9, 1);
		require(resize.valid, "Survivor Location resize did not validate");
		corridor = world.applyLocationEdit(resize);
		verify();
		// Refused topology changes are true no-ops, including private lifetime.
		auto invalid = world.planResizeLocation(corridor, 11, 0, 9, 1);
		require(!invalid.valid, "An out-of-bounds Location resize was accepted");
		verify();
		refuseDuplicate(); // carrying only a baseline would have discarded the blob
		auto remove = world.planRemoveLocation(corridor);
		require(remove.valid, "Location removal did not validate");
		world.applyLocationEdit(remove);
		require(!world.lookupAgent(id).entity, "A deleted Agent survived Location removal");
		auto replacementSector = world.addCorridor(0, 0, 9);
		world.finishBuild();
		// A new lightweight construction and teardown remain safe after releasing
		// the large instance (its unreferenced Lua storage is collected lazily).
		auto const replacement = world.createAgent("Fresh", replacementSector, 0, 2.0f);
		require(!!replacement && replacement != id,
			"Deletion did not permit fresh construction with a new Agent identity");
	}

	void loadingConstructsFreshInstances(smoke::Context const&)
	{
		// A module-level counter is mutable state that must never survive into
		// a document or across load: loading reconstructs from source, so the
		// reloaded World's instance observes width == 1 rather than a persisted
		// count.
		std::string const counter = "        count = count + 1\n"
			"        return {\n"
			"            width = count,\n"
			"            standing_height = 0.6, reach = 0.3, walk_speed = 0.4,\n"
			"            climb_speed = 0.2, stair_ascent_speed = 0.3,\n"
			"            stair_descent_speed = 0.35, sitting_height_ratio = 0.5,\n"
			"            crouching_height_ratio = 0.6, crawling_height_ratio = 0.4,\n"
			"            crawling_speed_ratio = 0.5,\n"
			"            mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' } }\n";
		core::AgentTypeDefinition counterType;
		counterType.typeId = "Counter";
		counterType.displayName = "Counter";
		counterType.resourceName = "counter.agent.lua";
		counterType.source = "local count = 0\n" + typeSource("Counter", "Counter", counter);
		core::setAgentTypeResourceLoader(
			[counterType](std::string const& name) -> std::optional<core::AgentTypeDefinition>
			{
				if (name == counterType.resourceName) return counterType;
				return std::nullopt;
			});

		core::World original("Counter document", 6, 2);
		auto const sector = original.addCorridor(0, 0, 6);
		original.finishBuild();
		original.pauseSimulation();
		std::string diagnostic;
		require(original.attachAgentType("counter.agent.lua", counterType.source, &diagnostic),
			diagnostic.c_str());
		(void)original.createAgent("Counter", "Counter", sector, 0, 1.0f);

		for (bool binary : { false, true })
		{
			auto const text = serializeWorld(original, binary);
			core::World restored("restored", 2, 2);
			require(loadWorld(text, binary, restored), "Counter World did not load");
			auto const* agent = restored.lookupAgent(core::AgentId{ 1 }).entity;
			require(agent && agent->getPhysicalBaseline().width == 1.0f,
				"Loading preserved mutable Lua state instead of constructing fresh instances");
		}
		core::setAgentTypeResourceLoader({});
	}
}

void agent_smoke::registerAgentTypes(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentTypesScriptedHumanIdentity", scriptedHumanIdentity });
	checks.push_back({ "agentTypesBundledDefinitionMatchesResource", bundledDefinitionMatchesResource });
	checks.push_back({ "agentTypesGenericScriptBackedType", genericScriptBackedType });
	checks.push_back({ "agentTypesInvalidBaselinesRejected", invalidBaselinesAreRejected });
	checks.push_back({ "agentTypesScriptedMobilityProfiles", scriptedMobilityProfiles });
	checks.push_back({ "agentTypesConstructorFailureLeavesNoPartialAgent", constructorFailureLeavesNoPartialAgent });
	checks.push_back({ "agentTypesExecutionBudgetEnforced", executionBudgetIsEnforced });
	checks.push_back({ "agentTypesAllocationBudgetEnforced", allocationBudgetIsEnforced });
	checks.push_back({ "agentTypesInstancesAreIsolated", instancesAreIsolated });
	checks.push_back({ "agentTypesLegacyAndScriptedLoading", legacyAndScriptedHumanLoading });
	checks.push_back({ "agentTypesDuplicateTypeIdRejected", duplicateTypeIdIsRejected });
	checks.push_back({ "agentTypesResetReconstructsScriptedInstances", resetReconstructsScriptedInstances });
	checks.push_back({ "agentTypesResetRevisionAndAuthoredData", resetRevisionAndAuthoredData });
	checks.push_back({ "agentTypesResetFailureIsAtomic", resetFailureIsAtomic });
	checks.push_back({ "agentTypesResetReleasesReplacedInstances", resetReleasesReplacedInstances });
	checks.push_back({ "agentTypesResolvedResourceIdentity", resolvedResourceIdentity });
	checks.push_back({ "agentTypesResolvedResourceUnavailable", resolvedResourceUnavailable });
	checks.push_back({ "agentTypesPreviewMatchesPlacement", previewMatchesPlacement });
	checks.push_back({ "agentTypesScriptedAuthorizationPlacement", scriptedAuthorizationPlacement });
	checks.push_back({ "agentTypesFixturePhysicalOutcomes", fixtureTypePhysicalOutcomes });
	checks.push_back({ "agentTypesFixturePersistenceRoundTrip", fixturePersistenceRoundTrip });
	checks.push_back({ "agentTypesLoadingRefusesMismatchedOrMissingResource", loadingRefusesMismatchedOrMissingResource });
	checks.push_back({ "agentTypesCompetingTypeIdsRejectedOnLoad", competingTypeIdsRejectedOnLoad });
	checks.push_back({ "agentTypesLegacyHumanWithoutResource", legacyHumanWithoutResourceResolves });
	checks.push_back({ "agentTypesLoadingConstructsFreshInstances", loadingConstructsFreshInstances });
	checks.push_back({ "agentTypesTopologyReplayPreservesLiveInstances", topologyReplayPreservesLiveInstances });
}
