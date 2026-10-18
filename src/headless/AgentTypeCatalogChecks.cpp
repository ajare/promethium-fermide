#include <iostream>
#include <stdexcept>
#include <string>

#include "core/Exceptions.h"
#include "core/AgentTypeCatalog.h"
#include "core/SerializationException.h"
#include "core/World.h"

namespace
{
	int checksRun = 0;

	void require(bool condition, std::string const& what)
	{
		if (!condition) throw std::runtime_error("AgentTypeCatalog check failed: " + what);
		++checksRun;
	}

	template<typename Fn>
	void requireThrowsSerialization(Fn&& fn, std::string const& what)
	{
		try
		{
			fn();
		}
		catch (core::SerializationException const&)
		{
			++checksRun;
			return;
		}
		throw std::runtime_error("AgentTypeCatalog check did not throw: " + what);
	}

	std::string typeSource(std::string const& typeId, std::string const& displayName,
		std::string const& newBody)
	{
		return "return {\n"
			"    api_version = 2,\n"
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
			"            poses = { standing = {image_tile='agent'}, sitting = {image_tile='agent',height_ratio=0.5}, lying = {image_tile='agent',height_ratio=5/6,width_ratio=1.2}, crouching = {image_tile='agent',height_ratio=0.6}, crawling = {image_tile='agent',height_ratio=0.4} },\n"
			"            automatic_poses = { room_movement = {{pose='standing',speed_ratio=1},{pose='crouching',speed_ratio=1},{pose='crawling',speed_ratio=1}}, door_crossing = {{pose='standing',speed_ratio=1},{pose='crawling',speed_ratio=0.5}} },\n"
			"            mobility_profile = { staircase = 'can_use', escalator = 'can_use', stairwell = 'can_use', ladder = 'can_use', lift = 'can_use', platform_lift = 'can_use', shuttle = 'can_use', door = 'can_use', buttons = 'can_use' },\n" + overrides
			+ "        }\n";
	}
}

int main()
{
	using namespace core;

	try
	{
		// The catalog is constructed directly against a World it does not
		// own: the ADR 0022 proof-of-pattern is that the Agent-type family
		// is unit-testable without driving the World facade. World keeps the
		// Agent-type registry state (ADR 0001); the collaborator holds only
		// a reference.
		World world("catalog-checks", 8, 4);
		AgentTypeCatalog catalog(world);

		std::string diagnostic;

		// ---------------------------------------------------------------
		// Bundled registration.
		// ---------------------------------------------------------------
		// The World constructor registers the bundled Human type; the
		// catalog sees it, and re-registering is a no-op that neither
		// duplicates nor replaces it.
		require(catalog.hasAgentType("Human"), "bundled Human not registered");
		require(catalog.agentTypeDisplayName("Human") == "Human",
			"bundled Human display name");
		require(catalog.agentTypeResourceName("Human") == "human.agent.lua",
			"bundled Human resource name");
		{
			auto const before = catalog.registeredAgentTypeDefinition("Human", "human.agent.lua");
			catalog.registerBundledAgentTypes();
			auto const after = catalog.registeredAgentTypeDefinition("Human", "human.agent.lua");
			require(before && after && before->source == after->source,
				"re-registering the bundled Human changed the definition");
			++checksRun;
		}
		require(!catalog.hasAgentType("Nope"), "unknown type reported registered");
		require(catalog.agentTypeDisplayName("Nope").empty()
			&& catalog.agentTypeResourceName("Nope").empty(),
			"unknown type name queries were not empty");

		// ---------------------------------------------------------------
		// Attachment.
		// ---------------------------------------------------------------
		require(catalog.attachAgentType("checker.agent.lua",
			typeSource("Checker", "Checker Person", validBaseline()), &diagnostic),
			"valid attach refused: " + diagnostic);
		require(catalog.hasAgentType("Checker"), "attached type not registered");
		require(catalog.agentTypeDisplayName("Checker") == "Checker Person",
			"attached display name");
		require(catalog.agentTypeResourceName("Checker") == "checker.agent.lua",
			"attached resource name");

		// A duplicate type ID is refused without mutating the World.
		require(!catalog.attachAgentType("other-checker.agent.lua",
			typeSource("Checker", "Other", validBaseline()), &diagnostic)
			&& diagnostic.find("already registered") != std::string::npos,
			"duplicate type ID accepted");
		require(catalog.agentTypeDisplayName("Checker") == "Checker Person",
			"refused duplicate changed the registered definition");

		// A resource that re-declares the bundled Human ID is refused.
		require(!catalog.attachAgentType("other-human.agent.lua",
			typeSource("Human", "Not Human", validBaseline()), &diagnostic),
			"a competing Human declaration was accepted");
		require(catalog.agentTypeDisplayName("Human") == "Human",
			"refused Human impostor changed the bundled definition");

		// An empty resource name and an invalid script are refused.
		require(!catalog.attachAgentType("", "return {}", &diagnostic)
			&& !diagnostic.empty(), "empty resource name accepted");
		require(!catalog.attachAgentType("broken.agent.lua", "this is not lua",
			&diagnostic), "invalid script accepted");
		require(!catalog.hasAgentType("Broken"), "invalid script registered a type");

		// ---------------------------------------------------------------
		// Resolution.
		// ---------------------------------------------------------------
		// Legacy record: the stable type ID names a registered type.
		auto human = catalog.resolveAgentType("Human", "");
		require(human && human->displayName == "Human", "legacy Human resolution");

		// An explicit resource reference is authoritative: the registered
		// Checker resolves by resource name without touching the loader.
		auto checker = catalog.resolveAgentType("Checker", "checker.agent.lua");
		require(checker && checker->resourceName == "checker.agent.lua",
			"explicit-resource resolution of a registered type");

		// A saved record whose type ID contradicts the registered resource is
		// refused, never silently honoured.
		requireThrowsSerialization(
			[&] { catalog.resolveAgentType("Impostor", "checker.agent.lua"); },
			"type-ID/resource mismatch did not throw");

		// Unknown records fail clearly; they never fall back to Human.
		requireThrowsSerialization(
			[&] { catalog.resolveAgentType("Nope", ""); },
			"unknown legacy record did not throw");
		requireThrowsSerialization(
			[&] { catalog.resolveAgentType("Ghost", "no-such.agent.lua"); },
			"missing explicit resource did not throw");

		// ---------------------------------------------------------------
		// Definition and preview-baseline queries.
		// ---------------------------------------------------------------
		auto const definition = catalog.registeredAgentTypeDefinition(
			"Checker", "checker.agent.lua");
		require(definition && definition->typeId == "Checker",
			"registeredAgentTypeDefinition round-trip");
		require(!catalog.registeredAgentTypeDefinition("Checker", "other.agent.lua")
			.has_value(),
			"definition lookup ignored the resource-name match requirement");

		auto const baseline = catalog.registeredAgentTypeBaseline(
			"Checker", "checker.agent.lua");
		require(baseline && baseline->width == 0.5f,
			"registeredAgentTypeBaseline did not compute the authored baseline");
		// The cache is keyed by source: a second read returns the same value.
		auto const cached = catalog.registeredAgentTypeBaseline(
			"Checker", "checker.agent.lua");
		require(cached && cached->width == baseline->width
			&& cached->walkSpeed == baseline->walkSpeed,
			"cached baseline disagrees with the first read");
		require(!catalog.registeredAgentTypeBaseline("Checker", "wrong.agent.lua")
			.has_value(),
			"baseline lookup ignored the resource-name match requirement");

		// ---------------------------------------------------------------
		// Detachment.
		// ---------------------------------------------------------------
		require(!catalog.detachUnusedAgentType("Human"),
			"the bundled Human was detached");
		require(!catalog.detachUnusedAgentType("Nope"),
			"detaching an unknown type reported success");

		// A type owned by a live Agent cannot be detached.
		auto const corridor = world.addCorridor(0, 0, 8);
		world.finishBuild();
		auto const agentId = world.createAgent("Checker", "Owned checker", corridor, 0, 1.0f);
		require(world.lookupAgent(agentId).entity->getTypeId() == "Checker",
			"placed Agent did not take the attached type");
		require(!catalog.detachUnusedAgentType("Checker"),
			"a type owned by a live Agent was detached");

		// An unused attachment detaches cleanly.
		require(catalog.attachAgentType("spare.agent.lua",
			typeSource("Spare", "Spare", validBaseline()), &diagnostic),
			"spare attach refused: " + diagnostic);
		require(catalog.detachUnusedAgentType("Spare"),
			"unused spare type was not detached");
		require(!catalog.hasAgentType("Spare"), "detached type still registered");

		// ---------------------------------------------------------------
		// The World facade forwards to the same state the catalog edits.
		// ---------------------------------------------------------------
		require(world.attachAgentType("facade.agent.lua",
			typeSource("FacadeType", "Facade Type", validBaseline()), &diagnostic),
			"facade attach refused: " + diagnostic);
		require(catalog.hasAgentType("FacadeType"),
			"facade attach did not reach the catalog");
		require(catalog.agentTypeDisplayName("FacadeType") == "Facade Type"
			&& world.agentTypeDisplayName("FacadeType") == "Facade Type",
			"facade and catalog display names disagree");
		require(world.hasAgentType("FacadeType")
			&& catalog.agentTypeResourceName("FacadeType") == "facade.agent.lua",
			"facade and catalog resource names disagree");
		require(world.detachUnusedAgentType("FacadeType")
			&& !catalog.hasAgentType("FacadeType"),
			"facade detach did not reach the catalog");

		std::cout << "AgentTypeCatalog checks passed (" << checksRun << " checks)\n";
		return 0;
	}
	catch (std::exception const& error)
	{
		std::cerr << "AgentTypeCatalog checks FAILED: " << error.what() << "\n";
		return 1;
	}
}
