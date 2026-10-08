// Editor placement seam for script-backed Human (#504). The same
// commitAgentPlacement the GUI palette and paste paths call must resolve the
// managed Agent type resource and create a retained script-backed instance
// through the public World construction path, refusing atomically when the
// resource is unavailable or mismatched.

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "AgentClipboard.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/Sector.h"
#include "core/World.h"

#include "Checks.h"

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// Installs a resource loader for one check and restores file/catalog
	// resolution on scope exit so later checks are unaffected.
	struct AgentTypeLoaderScope
	{
		explicit AgentTypeLoaderScope(core::AgentTypeResourceLoader loader)
		{
			core::setAgentTypeResourceLoader(std::move(loader));
		}
		~AgentTypeLoaderScope() { core::setAgentTypeResourceLoader({}); }
	};

	struct Fixture
	{
		std::shared_ptr<core::World> world;
		uint32_t corridor{ 0 };
	};

	Fixture buildWorld(std::string const& name)
	{
		Fixture fixture;
		fixture.world = std::make_shared<core::World>(name, 10, 2);
		fixture.corridor = fixture.world->addCorridor(0, 0, 10);
		fixture.world->finishBuild();
		return fixture;
	}

	AgentClipboardPayload humanPayload(std::string const& name)
	{
		return AgentClipboardPayload{ name, 0, true, std::nullopt };
	}

	AgentClipboardPayload scoutPayload(std::string const& name)
	{
		AgentClipboardPayload payload{ name, 0, true, std::nullopt };
		payload.type = "Scout";
		payload.resource = "scout.agent.lua";
		return payload;
	}

	std::size_t agentCount(core::World const& world)
	{
		return world.getSimulationSnapshot().agents.size();
	}

	void editorPlacementCreatesScriptedHuman(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor scripted Human");
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(fixture.world, humanPayload("Alice"),
			fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
			"Human placement failed: " + diagnostic);
		require(!!placed, "Placement reported success without naming an Agent");
		auto const* agent = fixture.world->lookupAgent(placed).entity;
		require(agent != nullptr, "The placed Agent could not be found");
		require(agent->getTypeId() == "Human"
			&& std::string(agent->getTypeName()) == "Human"
			&& agent->getTypeResourceName() == "human.agent.lua",
			"Editor Human placement did not produce script-backed Human identity");
		auto const& physical = agent->getPhysicalBaseline();
		auto const expected = core::bundledHumanBaseline();
		require(physical.width == expected.width
			&& physical.standingHeight == expected.standingHeight
			&& physical.reach == expected.reach
			&& physical.walkSpeed == expected.walkSpeed,
			"Editor Human placement baseline did not match the scripted baseline");
	}

	void editorPlacementFailureIsAtomic(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor scripted failure");
		auto const before = agentCount(*fixture.world);
		{
			AgentTypeLoaderScope scope{ [](std::string const&)
				-> std::optional<core::AgentTypeDefinition> { return std::nullopt; } };
			core::AgentId placed{};
			std::string diagnostic;
			require(!commitAgentPlacement(fixture.world, humanPayload("Broken"),
				fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
				"Placement with an unavailable resource was accepted");
			require(!placed, "Failed placement named an Agent");
			require(!diagnostic.empty(), "Failed placement reported no diagnostic");
		}
		require(agentCount(*fixture.world) == before,
			"Failed placement left an Agent behind");
	}

	void editorPlacementMismatchIsRefused(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor scripted mismatch");
		auto const before = agentCount(*fixture.world);
		{
			AgentTypeLoaderScope scope{ [](std::string const& name)
				-> std::optional<core::AgentTypeDefinition>
			{
				core::AgentTypeDefinition definition;
				definition.typeId = "NotHuman";
				definition.displayName = "Not Human";
				definition.resourceName = name;
				definition.source = core::bundledHumanAgentType().source;
				return definition;
			} };
			core::AgentId placed{};
			std::string diagnostic;
			require(!commitAgentPlacement(fixture.world, humanPayload("Broken"),
				fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
				"Placement with a mismatched type ID was accepted");
			require(diagnostic.find("type ID") != std::string::npos,
				"The mismatch diagnostic did not name the type ID: " + diagnostic);
		}
		require(agentCount(*fixture.world) == before,
			"Mismatched placement left an Agent behind");
	}

	void editorSelectionPlacesFixtureType(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor Scout");
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(fixture.world, scoutPayload("Runner"),
			fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
			"Scout placement failed: " + diagnostic);
		require(!!placed, "Scout placement reported success without naming an Agent");
		auto const* agent = fixture.world->lookupAgent(placed).entity;
		require(agent != nullptr, "The placed Scout could not be found");
		require(agent->getTypeId() == "Scout"
			&& std::string(agent->getTypeName()) == "Scout"
			&& agent->getTypeResourceName() == "scout.agent.lua",
			"Editor Scout placement did not produce Scout identity");
		require(agent->getPhysicalBaseline().walkSpeed == 0.9f
			&& agent->getPhysicalBaseline().width == 0.3f,
			"Editor Scout placement did not use the Scout baseline");
	}

	void editorSelectionPreviewAgreesWithPlacement(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor Scout preview");
		auto const preview = agentClipboardPlacementDimensions(scoutPayload("Runner"));
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(fixture.world, scoutPayload("Runner"),
			fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
			"Scout placement failed: " + diagnostic);
		auto const* agent = fixture.world->lookupAgent(placed).entity;
		auto const& physical = agent->getPhysicalBaseline();
		require(preview.x == physical.width && preview.y == physical.standingHeight,
			"Scout preview dimensions did not agree with placement");
	}

	void editorSelectionDependencyRefusedAtomically(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor Scout refusal");
		auto const before = agentCount(*fixture.world);
		{
			AgentClipboardPayload missing = scoutPayload("Broken");
			missing.resource = "missing.agent.lua";
			core::AgentId placed{};
			std::string diagnostic;
			require(!commitAgentPlacement(fixture.world, missing,
				fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
				"Placement with a missing Scout resource was accepted");
			require(!placed && !diagnostic.empty(),
				"Failed Scout placement did not report or named an Agent");
		}
		{
			AgentClipboardPayload mismatched = scoutPayload("Broken");
			mismatched.type = "NotScout";
			core::AgentId placed{};
			std::string diagnostic;
			require(!commitAgentPlacement(fixture.world, mismatched,
				fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
				"Placement with a mismatched Scout type ID was accepted");
			require(!placed, "Mismatched Scout placement named an Agent");
		}
		require(agentCount(*fixture.world) == before,
			"Failed Scout placement left an Agent behind");
	}

	void editorCopiedFixturePreservesTypeIdentity(smoke::Context const&)
	{
		auto fixture = buildWorld("Editor Scout copy");
		core::AgentId placed{};
		std::string diagnostic;
		require(commitAgentPlacement(fixture.world, scoutPayload("Original"),
			fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic),
			"Scout placement failed: " + diagnostic);
		// A copy carries the immutable type identity, never a changed type: the
		// editor has no type-change control, so copying preserves what was placed.
		auto const payload = makeAgentClipboardPayload(*fixture.world, placed, "Copy");
		require(payload.type == "Scout" && payload.resource == "scout.agent.lua",
			"A copied Scout did not carry its type ID and resource reference");
		require(std::string(fixture.world->lookupAgent(placed).entity->getTypeId()) == "Scout",
			"The placed Scout's type identity changed");
	}
}

void agent_smoke::registerAgentTypeEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "agentTypesEditorPlacementCreatesScriptedHuman",
		editorPlacementCreatesScriptedHuman });
	checks.push_back({ "agentTypesEditorPlacementFailureIsAtomic",
		editorPlacementFailureIsAtomic });
	checks.push_back({ "agentTypesEditorPlacementMismatchIsRefused",
		editorPlacementMismatchIsRefused });
	checks.push_back({ "agentTypesEditorSelectionPlacesFixtureType",
		editorSelectionPlacesFixtureType });
	checks.push_back({ "agentTypesEditorSelectionPreviewAgreesWithPlacement",
		editorSelectionPreviewAgreesWithPlacement });
	checks.push_back({ "agentTypesEditorSelectionDependencyRefusedAtomically",
		editorSelectionDependencyRefusedAtomically });
	checks.push_back({ "agentTypesEditorCopiedFixturePreservesTypeIdentity",
		editorCopiedFixturePreservesTypeIdentity });
}
