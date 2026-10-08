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
#include "DocumentEdit.h"
#include "DocumentHistory.h"
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

	void historyPreservesSurvivorsAndReconstructsDeletedAgents(smoke::Context const&)
	{
		// The managed resource can change between reconstructions. A throwing
		// new() distinguishes preservation from fresh construction at the public
		// history seam without inspecting VM pointers or arbitrary private state.
		auto definition = core::bundledHumanAgentType();
		definition.typeId = "HistoryFixture";
		definition.displayName = "History fixture display";
		definition.resourceName = "history-fixture.agent.lua";
		auto replace = [](std::string& source, std::string const& from, std::string const& to) {
			auto const at = source.find(from);
			require(at != std::string::npos, "Fixture source replacement was not found");
			source.replace(at, from.size(), to);
		};
		replace(definition.source, "type_id = \"Human\"", "type_id = \"HistoryFixture\"");
		replace(definition.source, "display_name = \"Human\"",
			"display_name = \"History fixture display\"");
		auto const initialSource = definition.source;
		AgentTypeLoaderScope scope{ [&definition](std::string const& name)
			-> std::optional<core::AgentTypeDefinition> {
			if (name == definition.resourceName) return definition;
			return std::nullopt;
		} };
		auto world = std::make_shared<core::World>("History survival", 12, 2);
		auto const first = world->addRoom("Survivor room", 0, 0, 0, 5, 1);
		auto const second = world->addRoom("Removed room", 0, 0, 6, 4, 1);
		world->finishBuild();
		world->pauseSimulation();
		std::string diagnostic;
		require(world->attachAgentType(definition.resourceName, definition.source, &diagnostic), diagnostic);
		auto const survivor = world->createAgent(definition.typeId, "Survivor", first, 0, 2.0f);
		auto const casualty = world->createAgent(definition.typeId, "Deleted", second, 0, 2.0f);
		require(world->setAgentIndividualWalkSpeedModifier(survivor, 1.2f),
			"Could not author the survivor's property");
		DocumentHistory history;
		auto before = captureDocumentSnapshot(world, history);
		require(before.has_value(), "Could not capture a structural history entry");
		auto resize = world->planResizeLocation(first, 0, 0, 6, 1);
		require(resize.valid, "History fixture resize was not valid");
		world->applyLocationEdit(resize);
		commitDocumentEdit(std::move(before), history);
		definition.source = "return { api_version = 1, type_id = 'HistoryFixture', "
			"display_name = 'Changed display', new = function() error('fresh constructor') end }";
		auto restore = [&](DocumentSnapshot const& target) {
			try
			{
				auto loaded = deserializeDocumentSnapshot(target, world, {});
				if (!loaded) return false;
				world = std::move(loaded); // destroys old World, but not survivor Lua state
				return true;
			}
			catch (std::exception const&) { return false; }
		};
		auto verifySurvivor = [&] {
			auto const* agent = world->lookupAgent(survivor).entity;
			require(agent && agent->getTypeId() == definition.typeId
				&& std::string(agent->getTypeName()) == "History fixture display"
				&& agent->getPhysicalBaseline().width == 0.4f
				&& agent->getIndividualWalkSpeedModifier() == 1.2f,
				"History reconstructed or changed a surviving Agent");
		};
		require(history.undo(captureDocumentSnapshot(world, history), restore),
			"Structural undo ran a surviving Agent's throwing constructor");
		verifySurvivor();
		require(history.redo(captureDocumentSnapshot(world, history), restore),
			"Structural redo ran a surviving Agent's throwing constructor");
		verifySurvivor();

		before = captureDocumentSnapshot(world, history);
		auto remove = world->planRemoveLocation(second);
		require(remove.valid, "History fixture Location deletion was not valid");
		world->applyLocationEdit(remove);
		commitDocumentEdit(std::move(before), history);
		require(!world->lookupAgent(casualty).entity, "Topology deletion kept a casualty");
		auto const undoCount = history.undoCount();
		require(!history.undo(captureDocumentSnapshot(world, history), restore),
			"Undo of deletion reused a deleted Agent instead of constructing fresh");
		require(history.undoCount() == undoCount && !history.canRedo()
			&& !world->lookupAgent(casualty).entity,
			"Failed reconstruction changed the live World or document history");
		verifySurvivor(); // failed candidate released borrowed references safely

		definition.source = initialSource;
		replace(definition.source, "width = 0.4", "width = 0.7");
		require(history.undo(captureDocumentSnapshot(world, history), restore),
			"Deletion undo did not recover after constructor failure");
		verifySurvivor();
		require(world->lookupAgent(casualty).entity->getPhysicalBaseline().width == 0.7f,
			"Deletion undo did not use a fresh constructor from the resolved resource");
		require(history.redo(captureDocumentSnapshot(world, history), restore),
			"Deletion redo failed");
		verifySurvivor();
		require(!world->lookupAgent(casualty).entity, "Deletion redo kept the casualty");
		auto const newId = world->createAgent("New lifetime", first, 0, 4.0f);
		require(newId.value > casualty.value,
			"History replacement reissued a deleted Agent's authored identity");
		// Loading without a current World remains a fresh lifetime boundary.
		auto const snapshot = captureDocumentSnapshot(world, history);
		auto loaded = deserializeDocumentSnapshot(*snapshot, {}, {});
		require(loaded && loaded->lookupAgent(survivor).entity->getPhysicalBaseline().width == 0.7f,
			"An ordinary document load incorrectly preserved the old baseline");
		world->resetSimulation();
		require(world->lookupAgent(survivor).entity->getPhysicalBaseline().width == 0.7f,
			"Reset incorrectly preserved a surviving instance from structural history");
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
	checks.push_back({ "agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents",
		historyPreservesSurvivorsAndReconstructsDeletedAgents });
}
