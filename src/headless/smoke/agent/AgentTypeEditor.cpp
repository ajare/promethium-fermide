// Editor placement seam for script-backed Human (#504). The same
// commitAgentPlacement the GUI palette and paste paths call must resolve the
// managed Agent type resource and create a retained script-backed instance
// through the public World construction path, refusing atomically when the
// resource is unavailable or mismatched.

#include <fstream>
#include <memory>
#include <willpower/common/Logger.h>
#include <willpower/application/resourcesystem/ResourceManager.h>
#include "ApplicationAgentTypes.h"
#include "ApplicationResources.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/AgentTagRegistry.h"
#include "core/SerializationException.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "AgentClipboard.h"
#include "AgentDropTargets.h"
#include "DocumentEdit.h"
#include "DocumentHistory.h"
#include "core/Agent.h"
#include "core/AgentType.h"
#include "core/Sector.h"
#include "core/World.h"

#include "Checks.h"
#include "MobilityLifecycle.h"

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
		explicit AgentTypeLoaderScope(core::AgentTypeResourceLoader loader,
			core::AgentTypePreviewLoader preview = {})
		{
			core::setAgentTypeResourceLoader(std::move(loader), std::move(preview));
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
			// Human is already World-registered; placement uses that frozen source.
			// Refusal must exercise an unresolved selected resource instead.
			require(!commitAgentPlacement(fixture.world, scoutPayload("Broken"),
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
			require(!commitAgentPlacement(fixture.world, scoutPayload("Broken"),
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

	void previewQueriesReuseValidatedResource(smoke::Context const&)
	{
		auto fixture = buildWorld("Repeated Agent preview");
		auto definition = core::bundledHumanAgentType();
		unsigned resolutions = 0;
		AgentTypeLoaderScope scope{ [&](std::string const&) {
			++resolutions;
			return std::optional{ definition };
		} };
		auto payload = humanPayload("Preview");
		auto initial = agentClipboardPlacementDimensions(payload);
		require(initial.x > 0.f, "Initial preview did not resolve");
		for (unsigned frame = 0; frame < 64; ++frame)
		{
			payload.individualHeightModifier = frame % 2 ? 0.9f : 1.f;
			auto const dimensions = agentClipboardPlacementDimensions(payload);
			require(dimensions.x == initial.x
				&& dimensions.y == initial.y * *payload.individualHeightModifier,
				"Cached preview ignored current individual properties");
			require(fixture.world->canPlaceAgentInLocation(fixture.corridor, {}, {}),
				"Repeated placement permission query failed");
		}
		require(resolutions == 1, "Per-frame previews re-resolved the Agent resource "
			+ std::to_string(resolutions) + " times instead of once");
		require(agentCount(*fixture.world) == 0, "Preview published an Agent");

		// Loader replacement is an explicit cache-lifetime boundary. Failed
		// snapshots must also be cached so a bad selection cannot spin up Lua
		// (or retry I/O) on every frame.
		unsigned failures = 0;
		core::setAgentTypeResourceLoader([&](std::string const&)
			-> std::optional<core::AgentTypeDefinition> {
			++failures;
			throw std::runtime_error("Unavailable preview dependency");
		});
		std::string diagnostic;
		for (unsigned frame = 0; frame < 64; ++frame)
			require(agentClipboardPlacementDimensions(payload, &diagnostic).x == 0.f
				&& diagnostic == "Unavailable preview dependency",
				"Loader replacement retained a stale successful preview");
		require(failures == 1, "Failed preview retried dependency resolution per frame");
		core::setAgentTypeResourceLoader([&](std::string const&) { return std::optional{ definition }; });
		require(agentClipboardPlacementDimensions(payload, &diagnostic).x == initial.x
			&& diagnostic.empty(), "Failed preview survived resource-loader replacement");
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
		replace(definition.source, "door = \"can_use\"", "door = \"cannot_use\"");
		auto const initialSource = definition.source;
		AgentTypeLoaderScope scope{ [&definition](std::string const& name)
			-> std::optional<core::AgentTypeDefinition> {
			if (name == definition.resourceName) return definition;
			if (name == "human.agent.lua") return core::bundledHumanAgentType();
			return std::nullopt;
		} };
		auto world = std::make_shared<core::World>("History survival", 12, 2);
		auto const first = world->addRoom("Survivor room", 0, 0, 0, 5, 1);
		auto const second = world->addRoom("Removed room", 0, 0, 6, 4, 1);
		constexpr uint32_t back = 1; // Layer identity survives structural reindexing.
		world->addCorridor(back, 0, 0, 12, 1);
		world->addSectorDoor(0, 0, 2);
		world->addSectorDoor(0, 0, 8);
		world->finishBuild();
		world->pauseSimulation();
		std::string diagnostic;
		require(world->attachAgentType(definition.resourceName, definition.source, &diagnostic), diagnostic);
		auto const survivor = world->createAgent(definition.typeId, "Survivor", first, 0, 2.0f);
		auto const casualty = world->createAgent(definition.typeId, "Deleted", second, 0, 2.0f);
		require(world->setAgentIndividualWalkSpeedModifier(survivor, 1.2f),
			"Could not author the survivor's property");
		core::MobilityProfile override;
		override.set(core::TraversalKind::Door, core::MobilityUse::OnlyIfNoOtherOption);
		require(world->setAgentIndividualMobilityProfile(casualty, override, &diagnostic), diagnostic);
		agent_smoke::requireDoorRoute(*world, survivor, back, false);
		agent_smoke::requireDoorRoute(*world, casualty, back, true);
		// Change the resolved revision before the ordinary edit, not only replay.
		replace(definition.source, "door = \"cannot_use\"", "door = \"can_use\"");
		replace(definition.source, "lying = { image_tile = \"human-lying\", height_ratio = 26 / 72, width_ratio = 72 / 26 },", "");
		replace(definition.source, "height_ratio = 0.3", "height_ratio = 0.4");
		replace(definition.source, "image_tile = \"human-standing\"", "image_tile = \"marker\"");
		DocumentHistory history;
		auto before = captureDocumentSnapshot(world, history);
		require(before.has_value(), "Could not capture a structural history entry");
		auto resize = world->planResizeLocation(first, 0, 0, 6, 1);
		require(resize.valid, "History fixture resize was not valid");
		world->applyLocationEdit(resize);
		agent_smoke::requireDoorRoute(*world, survivor, back, false);
		commitDocumentEdit(std::move(before), history);
		definition.source = "return { api_version = 2, type_id = 'HistoryFixture', "
			"display_name = 'Changed display', new = function() error('fresh constructor') end }";
		auto restore = [&](DocumentSnapshot const& target) {
			try
			{
				auto loaded = deserializeDocumentSnapshot(target, world, {});
				if (!loaded) return false;
				world = std::move(loaded); // destroys old World, but not survivor Lua state
				return true;
			}
			catch (std::exception const& error) { diagnostic = error.what(); return false; }
		};
		auto verifySurvivor = [&] {
			auto const* agent = world->lookupAgent(survivor).entity;
			require(agent && agent->getTypeId() == definition.typeId
				&& std::string(agent->getTypeName()) == "History fixture display"
				&& agent->getPhysicalBaseline().width == 0.4f
				&& agent->getIndividualWalkSpeedModifier() == 1.2f,
				"History reconstructed or changed a surviving Agent");
			require(agent->supportsPose(core::Pose::Lying)
				&& agent->getPoseEnvelope(core::Pose::Crawling)->y == agent->getStandingHeight() * 0.3f
				&& agent->getPhysicalBaseline().doorCrossing.size() == 3
				&& agent->getPoseImageTile() == "human-standing",
				"History replaced a survivor's frozen pose declarations");
			require(!agent->getIndividualMobilityProfile()
				&& agent->getEffectiveMobilityProfile().value.get(core::TraversalKind::Door)
					== core::MobilityUse::CannotUse,
				"Structural history changed frozen Mobility or authored an override");
			agent_smoke::requireDoorRoute(*world, survivor, back, false);
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
		replace(definition.source, "door = \"cannot_use\"", "door = \"bad\"");
		auto const deletedYaml = captureDocumentSnapshot(world, history)->yaml;
		require(!history.undo(captureDocumentSnapshot(world, history), restore)
			&& diagnostic.find(definition.resourceName) != std::string::npos
			&& diagnostic.find("door") != std::string::npos
			&& history.undoCount() == undoCount && !history.canRedo()
			&& captureDocumentSnapshot(world, history)->yaml == deletedYaml,
			"Invalid fresh Mobility restoration mutated World/history or lacked dependency diagnostics");
		verifySurvivor();

		definition.source = initialSource;
		replace(definition.source, "height_ratio = 0.3", "height_ratio = '0.3'");
		require(!history.undo(captureDocumentSnapshot(world, history), restore)
			&& diagnostic.find("poses.crawling.height_ratio") != std::string::npos
			&& history.undoCount() == undoCount && !history.canRedo()
			&& captureDocumentSnapshot(world, history)->yaml == deletedYaml,
			"Invalid pose restoration changed World/history or lacked a field diagnostic");
		verifySurvivor();
		definition.source = initialSource;
		replace(definition.source, "door = \"cannot_use\"", "door = \"can_use\"");
		replace(definition.source, "lying = { image_tile = \"human-lying\", height_ratio = 26 / 72, width_ratio = 72 / 26 },", "");
		replace(definition.source, "height_ratio = 0.3", "height_ratio = 0.4");
		replace(definition.source, "image_tile = \"human-standing\"", "image_tile = \"marker\"");
		replace(definition.source, "width = 0.4", "width = 0.7");
		require(history.undo(captureDocumentSnapshot(world, history), restore),
			"Deletion undo did not recover after constructor failure");
		verifySurvivor();
		require(world->lookupAgent(casualty).entity->getPhysicalBaseline().width == 0.7f
			&& world->lookupAgent(casualty).entity->getPoseImageTile() == "marker",
			"Deletion undo did not use a fresh constructor from the resolved resource");
		auto const* restored = world->lookupAgent(casualty).entity;
		require(!restored->supportsPose(core::Pose::Lying)
			&& restored->getPoseEnvelope(core::Pose::Crawling)->y == restored->getStandingHeight() * 0.4f,
			"Deleted-Agent restoration reused frozen pose definitions");
		require(restored->getIndividualMobilityProfile() == override
			&& restored->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door) == core::MobilityUse::CanUse,
			"Deleted-Agent restoration lost authored Mobility or reused its frozen default");
		agent_smoke::requireDoorRoute(*world, casualty, back, true);
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
		agent_smoke::requireDoorRoute(*loaded, survivor, back, true);
		require(!loaded->lookupAgent(survivor).entity->getIndividualMobilityProfile(),
			"Load materialised script Mobility as an authored override");
		world->resetSimulation();
		require(world->lookupAgent(survivor).entity->getPhysicalBaseline().width == 0.7f,
			"Reset incorrectly preserved a surviving instance from structural history");
		agent_smoke::requireDoorRoute(*world, survivor, back, true);
	}

	std::string externalSource(std::string const& id)
	{
		auto source = core::bundledHumanAgentType().source;
		auto at = source.find("type_id = \"Human\"");
		source.replace(at, std::string("type_id = \"Human\"").size(), "type_id = '" + id + "'");
		return source;
	}

	void writeSource(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream out(path, std::ios::binary);
		out << source;
		require(bool(out), "Could not write an external Agent fixture");
	}

	struct ResourceFixture
	{
		wp::Logger logger;
		wp::application::resourcesystem::ResourceManager manager{ nullptr, nullptr, nullptr, &logger };
		std::unique_ptr<ApplicationAgentTypes> types;
		explicit ResourceFixture(std::filesystem::path const& root)
		{
			logger.open((root / "agent-resource-log.html").string());
			auto const human = root / "human.agent.lua";
			writeSource(human, core::bundledHumanAgentType().source);
			manager.addResource(std::make_shared<AgentTypeResource>("human.agent.lua", "",
				human.string(), std::map<std::string, std::string>{}, nullptr));
			types = std::make_unique<ApplicationAgentTypes>(manager);
		}
	};

	void managedPreviewIsReadOnly(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const path = root / "preview.agent.lua";
		writeSource(path, externalSource("Preview"));
		ResourceFixture resources(root);
		unsigned freshResolutions = 0;
		AgentTypeLoaderScope scope{
			[&](auto const& name) { ++freshResolutions; return resources.types->resolve(name); },
			[&](auto const& name) { return resources.types->preview(name); } };
		ApplicationAgentType selected;
		std::string diagnostic;
		require(resources.types->importFile(path, selected, diagnostic), diagnostic);
		auto payload = humanPayload("Preview");
		payload.type = selected.typeId;
		payload.resource = selected.resourceName;
		auto fixture = buildWorld("Managed preview");
		// Even the FIRST preview after import must use the validated resource,
		// not reread source or invoke the fresh reconstruction loader.
		std::filesystem::remove(path);
		auto const previousLayer = gUISettings.visibleLayer;
		struct RestoreLayer { int value; ~RestoreLayer() { gUISettings.visibleLayer = value; } }
			restoreLayer{ previousLayer };
		gUISettings.visibleLayer = 0;
		for (unsigned frame = 0; frame < 64; ++frame)
		{
			auto dimensions = agentClipboardPlacementDimensions(payload, &diagnostic);
			require(dimensions.x == 0.4f && diagnostic.empty(),
				"Imported preview reread an unavailable file: " + diagnostic);
			require(!!pegmanAgentTargetAtWorld(fixture.world, { 0.01f, 0.1f }, dimensions.x),
				"Cached Agent dimensions did not produce a drop target");
			auto mismatched = payload;
			mismatched.type = "Other";
			require(agentClipboardPlacementDimensions(mismatched, &diagnostic).x == 0.f
				&& !diagnostic.empty(), "Cached preview accepted a mismatched identity");
			auto missing = payload;
			missing.resource = "missing.agent.lua";
			require(agentClipboardPlacementDimensions(missing, &diagnostic).x == 0.f,
				"Missing preview acquired a Human envelope");
		}
		require(freshResolutions == 0 && agentCount(*fixture.world) == 0,
			"Per-frame preview performed fresh resolution or published an Agent");
		core::AgentId placed;
		require(!commitAgentPlacement(fixture.world, payload,
			fixture.world->getSector(fixture.corridor), 0, 1.f, placed, diagnostic)
			&& !placed && !diagnostic.empty(),
			"Cached preview incorrectly masked a missing placement dependency");
		require(freshResolutions == 1, "Placement did not revalidate its dependency");

		// A clipboard resource not yet imported is resolved at paste arming,
		// before the UI asks for dimensions. A prior preview miss must not poison
		// the newly imported resource, and subsequent fall frames remain reads.
		auto const pastedPath = root / "pasted.agent.lua";
		writeSource(pastedPath, externalSource("PastedPreview"));
		payload.type = "PastedPreview";
		payload.resource = core::externalAgentTypeResourceName(pastedPath);
		require(agentClipboardPlacementDimensions(payload, &diagnostic).x == 0.f,
			"Preview implicitly imported an unknown resource");
		PendingAgentPlacement pending;
		require(armAgentPlacement(pending, *fixture.world, payload,
			fixture.world->getSector(fixture.corridor), 0, 1.f, diagnostic), diagnostic);
		auto const afterArming = freshResolutions;
		std::filesystem::remove(pastedPath);
		require(agentClipboardPlacementDimensions(pending.payload, &diagnostic).x == 0.4f
			&& diagnostic.empty() && freshResolutions == afterArming,
			"Armed paste did not acquire its imported preview snapshot");
		pending.cancel();
		require(agentCount(*fixture.world) == 0, "Arming/cancelling a preview published an Agent");
	}

	void managedResourceRevisionOnResetAndLoad(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const path = root / "revision.agent.lua";
		auto source = externalSource("Revision");
		writeSource(path, source);
		auto fixture = buildWorld("Managed revision");
		auto world = fixture.world;
		core::AgentId id;
		std::string resourceName;
		{
			ResourceFixture resources(root);
			AgentTypeLoaderScope scope{ [&resources](auto const& name) { return resources.types->resolve(name); } };
			ApplicationAgentType selected;
			std::string diagnostic;
			require(resources.types->importFile(path, selected, diagnostic), diagnostic);
			resourceName = selected.resourceName;
			auto payload = humanPayload("Revision instance");
			payload.type = selected.typeId;
			payload.resource = selected.resourceName;
			require(commitAgentPlacement(world, payload, world->getSector(fixture.corridor),
				0, 1.f, id, diagnostic), diagnostic);
			world->saveTo((root / "revision.world.yaml").string());
			world->saveTo((root / "revision.world").string());
			auto const at = source.find("width = 0.4");
			source.replace(at, std::string("width = 0.4").size(), "width = 0.7");
			writeSource(path, source);
			require(world->lookupAgent(id).entity->getPhysicalBaseline().width == 0.4f,
				"A managed resource edit hot-reloaded an existing Agent");
			// Placement and every preview of this already registered type must use
			// the World's frozen source, not the edited managed-resource revision.
			require(agentClipboardPlacementDimensions(payload, *world).x == 0.4f,
				"World-bound placement preview used the edited resource revision");
			PendingAgentPlacement pending;
			require(armAgentPlacement(pending, *world, payload,
				world->getSector(fixture.corridor), 0, 2.f, diagnostic), diagnostic);
			require(agentClipboardPlacementDimensions(pending).x == 0.4f,
				"Armed placement preview used the edited resource revision");
			core::AgentId repeated;
			require(commitPendingAgentPlacement(pending, world, repeated, diagnostic), diagnostic);
			require(world->lookupAgent(repeated).entity->getPhysicalBaseline().width == 0.4f,
				"Placement used the edited resource revision instead of the registered definition");
			for (auto const* filename : { "revision.world.yaml", "revision.world" })
			{
				auto loaded = core::loadWorldDocument(root / filename);
				require(loaded->lookupAgent(id).entity->getPhysicalBaseline().width == 0.7f,
					"Same-session load used the cached startup/import revision");
			}
			world->resetSimulation();
			require(world->lookupAgent(id).entity->getPhysicalBaseline().width == 0.7f,
				"Managed Reset used the cached startup/import revision");
			auto const* previous = world->lookupAgent(id).entity;
			writeSource(path, "invalid revised resource");
			bool refused = false;
			try { world->resetSimulation(); }
			catch (std::exception const& error) {
				refused = std::string(error.what()).find(resourceName) != std::string::npos;
			}
			require(refused && world->lookupAgent(id).entity == previous,
				"Invalid managed Reset replaced a live instance or lacked a dependency diagnostic");
			require(resources.types->types().size() == 2,
				"Revision resolution changed the managed selector inventory");
			writeSource(path, source);
		}
		// Destroying Willpower resources cannot invalidate World-owned source or
		// Lua handles. A later intentional reconstruction can resolve the file.
		require(world->lookupAgent(id).entity->getPhysicalBaseline().width == 0.7f,
			"Resource destruction invalidated the retained World instance");
		world->resetSimulation();
		require(world->lookupAgent(id).entity->getTypeResourceName() == resourceName,
			"Resource teardown left Reset with dangling definition references");
	}

	void externalImportPlacesAndReopens(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const external = root / "outside world with spaces";
		auto const documents = root / "documents";
		std::filesystem::create_directories(external);
		std::filesystem::create_directories(documents);
		auto const path = external / "visitor.agent.lua";
		auto source = externalSource("Visitor");
		auto at = source.find("width = 0.4");
		source.replace(at, std::string("width = 0.4").size(), "width = 0.7");
		writeSource(path, source);
		writeSource(external / "unrequested.agent.lua", externalSource("Unrequested"));
		ApplicationAgentType selected;
		core::AgentId placed{};
		{
			ResourceFixture resources(root);
			AgentTypeLoaderScope scope{ [&resources](auto const& name) { return resources.types->resolve(name); } };
			std::string diagnostic;
			require(resources.types->importFile(path, selected, diagnostic), diagnostic);
			require(selected.typeId == "Visitor" && selected.displayName == "Human",
				"Import did not retain independent type ID and display name");
			auto const inventory = resources.types->types();
			require(inventory.size() == 2 && resources.manager.getResourcesByType("AgentType").size() == 2,
				"Import scanned unrequested files or did not publish to Willpower and the selector");
			auto fixture = buildWorld("Imported Visitor");
			auto payload = humanPayload("Visitor instance");
			payload.type = selected.typeId;
			payload.resource = selected.resourceName;
			auto const preview = agentClipboardPlacementDimensions(payload);
			auto const previousLayer = gUISettings.visibleLayer;
			gUISettings.visibleLayer = 0;
			auto const target = pegmanAgentTargetAtWorld(fixture.world, { 0.01f, 0.1f }, preview.x);
			auto const invalidTarget = pegmanAgentTargetAtWorld(fixture.world, { 0.01f, 0.1f }, 0.f);
			gUISettings.visibleLayer = previousLayer;
			require(target && target.localX == preview.x * 0.5f && !invalidTarget,
				"Creation drop clamping used Human width or admitted an invalid preview");
			require(agentCount(*fixture.world) == 0 && preview.x == 0.7f,
				"Import preview created an Agent or used Human dimensions");
			require(commitAgentPlacement(fixture.world, payload,
				fixture.world->getSector(fixture.corridor), 0, 1.0f, placed, diagnostic), diagnostic);
			auto const* agent = fixture.world->lookupAgent(placed).entity;
			require(agent && agent->getTypeId() == selected.typeId
				&& agent->getTypeResourceName() == selected.resourceName
				&& agent->getPhysicalBaseline().width == preview.x,
				"Placement disagreed with the imported identity or preview");
			for (auto const* suffix : { "imported.world.yaml", "imported.world" })
				fixture.world->saveTo((documents / suffix).string());
			// Reimport is idempotent, not hot reload, and leaves existing physical
			// identity unchanged even if the author edits the file.
			writeSource(path, "invalid revised source");
			ApplicationAgentType repeated;
			require(resources.types->importFile(path, repeated, diagnostic)
				&& repeated.resourceName == selected.resourceName && resources.types->types().size() == 2,
				"Repeat import reloaded the script or registered another identity");
			require(fixture.world->lookupAgent(placed).entity->getPhysicalBaseline().width == 0.7f,
				"On-disk edits changed an existing frozen baseline");
			writeSource(path, source);
		}
		// New application session: the Resource name resolves the declared
		// external file without a rewritten manifest or process-local path cache.
		{
			ResourceFixture resources(root);
			AgentTypeLoaderScope scope{ [&resources](auto const& name) { return resources.types->resolve(name); } };
			for (auto const* suffix : { "imported.world.yaml", "imported.world" })
			{
				auto loaded = core::loadWorldDocument(documents / suffix);
				auto const* agent = loaded->lookupAgent(placed).entity;
				require(agent && agent->getTypeId() == selected.typeId
					&& agent->getTypeResourceName() == selected.resourceName
					&& agent->getPhysicalBaseline().width == 0.7f,
					"Reopen did not reconstruct the external Agent");
			}
			require(resources.types->types().size() == 2, "Reopen did not register its managed dependency");
		}
		// GPU-less public document loading uses the same resource-reference seam.
		auto loaded = core::loadWorldDocument(documents / "imported.world.yaml");
		require(loaded->lookupAgent(placed).entity->getTypeId() == "Visitor", "Headless reopen failed");
		writeSource(path, "malformed revised source");
		{
			ResourceFixture resources(root);
			AgentTypeLoaderScope scope{ [&resources](auto const& name) { return resources.types->resolve(name); } };
			bool refused = false;
			try { (void)core::loadWorldDocument(documents / "imported.world.yaml"); }
			catch (std::exception const& error)
			{
				refused = true;
				require(std::string(error.what()).find(path.string()) != std::string::npos,
					"Invalid external dependency diagnostic omitted its source path");
			}
			require(refused && resources.types->types().size() == 1
				&& resources.manager.getResourcesByType("AgentType").size() == 1,
				"Refused reopen left a partial imported resource");
		}
		require(loaded->lookupAgent(placed).entity->getPhysicalBaseline().width == 0.7f,
			"Refused reopen mutated the previously loaded World");
		std::filesystem::remove(path);
		// The application loader must diagnose the missing external dependency
		// before canonicalizing it, just as the headless resolver does.
		{
			ResourceFixture resources(root);
			AgentTypeLoaderScope scope{ [&resources](auto const& name) {
				return resources.types->resolve(name);
			} };
			try { (void)core::loadWorldDocument(documents / "imported.world.yaml");
				throw std::runtime_error("Editor accepted a missing external dependency"); }
			catch (core::SerializationException const& error)
			{
				auto const diagnostic = std::string(error.what());
				require(diagnostic.find("Agent type resource '" + selected.resourceName + "'")
					!= std::string::npos && diagnostic.find(path.string()) != std::string::npos
					&& diagnostic.find("Source file is unavailable") != std::string::npos,
					"Editor missing import diagnostic was not actionable: " + diagnostic);
			}
			require(resources.types->types().size() == 1
				&& resources.manager.getResourcesByType("AgentType").size() == 1,
				"Missing editor dependency left a partial imported resource");
		}
		try { (void)core::loadWorldDocument(documents / "imported.world.yaml");
			throw std::runtime_error("Missing external dependency was accepted"); }
		catch (core::SerializationException const& error)
		{ require(std::string(error.what()).find(selected.resourceName) != std::string::npos,
			"Missing import diagnostic did not identify the resource"); }
	}

	void externalImportRefusesWithoutRegistration(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		{
			ResourceFixture startup(root);
			startup.types.reset();
			auto const invalid = root / "invalid-startup.agent.lua";
			writeSource(invalid, "not lua");
			startup.manager.addResource(std::make_shared<AgentTypeResource>("invalid-startup.agent.lua", "",
				invalid.string(), std::map<std::string, std::string>{}, nullptr));
			startup.types = std::make_unique<ApplicationAgentTypes>(startup.manager);
			auto const inventory = startup.types->types();
			require(inventory.size() == 1 && inventory.front().typeId == "Human"
				&& !startup.types->resolve("invalid-startup.agent.lua"),
				"Startup published an invalid resource in the Agent-type selector");
			std::ifstream input(invalid);
			require(std::string(std::istreambuf_iterator<char>(input), {}) == "not lua",
				"Startup rewrote an invalid authored script");
		}
		ResourceFixture resources(root);
		ApplicationAgentType selected;
		std::string diagnostic;
		auto invalidBaseline = externalSource("BadBaseline");
		auto const width = invalidBaseline.find("width = 0.4");
		invalidBaseline.replace(width, std::string("width = 0.4").size(), "width = -1");
		auto missingBaseline = externalSource("MissingBaseline");
		auto const reach = missingBaseline.find("reach = 0.25,");
		missingBaseline.erase(reach, std::string("reach = 0.25,").size());
		auto invalidPose = externalSource("InvalidPose");
		agent_smoke::replaceSource(invalidPose, "height_ratio = 0.3", "height_ratio = '0.3'");
		auto v1 = externalSource("OldVersion");
		agent_smoke::replaceSource(v1, "api_version = 2", "api_version = 1");
		std::vector<std::pair<std::string, std::string>> cases{
			{ invalidPose, "poses.crawling.height_ratio" },
			{ v1, "migrate" },
			{ invalidBaseline, "width" },
			{ missingBaseline, "reach" },
			{ "not lua", "" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=7 }", "constructor" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=function() local blob=string.rep('x',128*1024*1024) end }", "" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad' }", "constructor" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=function() return 7 end }", "instance table" },
			{ "return { type_id='Bad', display_name='Bad', new=function() return {} end }", "" },
			{ externalSource("Human"), "Duplicate" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=function() return io.open('unsafe') end }", "" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=function() while true do end end }", "" },
			{ "return { api_version=2, type_id='Bad', display_name='Bad', new=function() error('constructor refused') end }", "constructor refused" },
		};
		for (std::size_t i = 0; i < cases.size(); ++i)
		{
			auto const path = root / ("bad-" + std::to_string(i) + ".agent.lua");
			writeSource(path, cases[i].first);
			require(!resources.types->importFile(path, selected, diagnostic)
				&& !diagnostic.empty() && selected.resourceName.empty(), "Invalid import was accepted or undiagnosed");
			require(diagnostic.find(path.string()) != std::string::npos
				&& diagnostic.find(cases[i].second) != std::string::npos, "Import diagnostic was not actionable: " + diagnostic);
			require(resources.types->types().size() == 1
				&& resources.manager.getResourcesByType("AgentType").size() == 1,
				"Failed import left partial registration");
			std::ifstream input(path);
			std::string actual{ std::istreambuf_iterator<char>(input), {} };
			require(actual == cases[i].first, "Import overwrote the source");
		}
		auto const path = root / "repaired.agent.lua";
		writeSource(path, "not lua");
		require(!resources.types->importFile(path, selected, diagnostic), "Malformed source accepted");
		writeSource(path, externalSource("Repaired"));
		require(resources.types->importFile(path, selected, diagnostic), "Failed import prevented a corrected retry: " + diagnostic);
		require(!resources.types->importFile(root / "absent.agent.lua", selected, diagnostic)
			&& !diagnostic.empty(), "Missing import source was accepted or undiagnosed");
		writeSource(root / "wrong.lua", externalSource("Wrong"));
		require(!resources.types->importFile(root / "wrong.lua", selected, diagnostic), "Wrong file suffix accepted");
	}

	void externalPlacementRollsBackRegistration(smoke::Context const& context)
	{
		auto const root = context.temporaryRoot();
		auto const path = root / "placement.agent.lua";
		writeSource(path, externalSource("Placement"));
		ResourceFixture resources(root);
		AgentTypeLoaderScope scope{ [&resources](auto const& name) { return resources.types->resolve(name); } };
		ApplicationAgentType selected;
		std::string diagnostic;
		require(resources.types->importFile(path, selected, diagnostic), diagnostic);
		core::AgentTypeRuntimeLimits limits;
		limits.instructionsPerCall = 1;
		auto world = std::make_shared<core::World>("Refused placement", 10, 2,
			core::AgentBehaviourRuntimeLimits{}, limits);
		auto corridor = world->addCorridor(0, 0, 10);
		world->finishBuild();
		auto payload = humanPayload("Refused");
		payload.type = selected.typeId;
		payload.resource = selected.resourceName;
		auto const before = captureDocumentSnapshot(world);
		core::AgentId placed{};
		require(!commitAgentPlacement(world, payload, world->getSector(corridor), 0, 1.0f, placed, diagnostic)
			&& !placed && !diagnostic.empty(), "Failing constructor published an Agent");
		require(!world->hasAgentType(selected.typeId) && agentCount(*world) == 0
			&& captureDocumentSnapshot(world)->yaml == before->yaml,
			"Failed placement left registration or authored mutation");
		require(world->attachAgentType("competing.agent.lua", externalSource("Placement"), &diagnostic), diagnostic);
		require(!commitAgentPlacement(world, payload, world->getSector(corridor), 0, 1.0f, placed, diagnostic)
			&& diagnostic.find("competing resources") != std::string::npos,
			"Placement silently used a competing definition");
		require(world->agentTypeResourceName(selected.typeId) == "competing.agent.lua" && agentCount(*world) == 0,
			"Duplicate refusal changed the original definition");
	}

	void scriptedClipboardAndDeletionHistory(smoke::Context const& context)
	{
		gWorldDocumentHistory.clear();
		auto definition = *core::resolveAgentTypeResource("scout.agent.lua");
		agent_smoke::replaceSource(definition.source, "door = \"can_use\"", "door = \"cannot_use\"");
		auto const initialSource = definition.source;
		AgentTypeLoaderScope scope{ [&definition](std::string const& name)
			-> std::optional<core::AgentTypeDefinition> {
			if (name == definition.resourceName) return definition;
			if (name == "human.agent.lua") return core::bundledHumanAgentType();
			return std::nullopt;
		} };
		auto fixture = buildWorld("Scripted clipboard history");
		auto& world = fixture.world;
		world->pauseSimulation();
		constexpr uint32_t back = 1;
		world->addCorridor(back, 0, 0, 10, 1);
		world->addSectorDoor(0, 0, 2);
		world->finishBuild();
		auto registry = core::AgentTagRegistry::create();
		auto const tag = registry->addAgentTag("physical");
		std::string diagnostic;
		require(registry->addAgentTagHeightModifier(tag, &diagnostic), diagnostic);
		require(registry->setAgentTagHeightModifier(tag, { 0.7f, 0.9f }, &diagnostic), diagnostic);
		require(registry->addAgentTagWalkSpeedModifier(tag, &diagnostic), diagnostic);
		auto const registryPath = context.temporaryRoot() / "clipboard.tags.yaml";
		registry->saveTo(registryPath.string());
		world->attachAgentTagRegistry(registryPath.filename().string(), registry);
		world->saveTo((context.temporaryRoot() / "clipboard.world.yaml").string());
		core::AgentId original;
		require(commitAgentPlacement(world, scoutPayload("Original"), world->getSector(fixture.corridor),
			0, 1.f, original, diagnostic), diagnostic);
		require(world->assignAgentTag(original, tag, &diagnostic), diagnostic);
		auto authored = makeAgentClipboardPayload(*world, original, "Copy");
		authored.active = false;
		authored.group = "Researchers";
		authored.individualColour = core::AgentColour{ 12, 34, 56 };
		authored.individualEscalatorWalkingChance = 0.4f;
		authored.individualWalkSpeedModifier = 1.2f;
		authored.individualHeightModifier = 0.95f;
		authored.individualStairSpeedModifier = 1.3f;
		authored.individualLadderSpeedModifier = 0.9f;
		authored.individualInteractionAversion = 0.8f;
		authored.individualEffortAversion = 1.2f;
		authored.individualWaitingAversion = 1.3f;
		authored.individualCrowdAversion = 1.4f;
		authored.individualRiskAversion = 1.5f;
		authored.individualRouteFamiliarity = 0.6f;
		authored.individualRoutePersistence = 0.2f;
		authored.individualMinimumRoutePlanningTime = 2.f;
		authored.individualMaximumRoutePlanningTime = 4.f;
		authored.individualPermissionAdherence = false;
		core::MobilityProfile mobility;
		mobility.set(core::TraversalKind::Ladder, core::MobilityUse::CannotUse);
		authored.individualMobilityProfile = mobility;
		auto const text = makeAgentClipboardText(authored, false);
		AgentClipboardPayload read;
		require(readAgentClipboardObject(YAML::Load(text)["promethiumClipboard"]["object"], read, diagnostic), diagnostic);
		require(makeAgentClipboardText(read, false) == text, "Wire round trip lost authored Agent data");
		PendingAgentPlacement pending;
		auto const before = captureDocumentSnapshot(world);
		auto const preview = agentClipboardPlacementDimensions(read, &diagnostic);
		require(diagnostic.empty() && preview.x == 0.3f && preview.y == 0.35f * 0.95f,
			"Preview lost Scout identity or individual-over-tag Height precedence");
		require(armAgentPlacement(pending, *world, read, world->getSector(fixture.corridor), 0, 4.f, diagnostic), diagnostic);
		require(captureDocumentSnapshot(world)->yaml == before->yaml, "Arming mutated the World");
		core::AgentId pasted;
		require(commitPendingAgentPlacement(pending, world, pasted, diagnostic), diagnostic);
		auto verify = [&](bool fresh = false) {
			require(makeAgentClipboardText(makeAgentClipboardPayload(*world, pasted, "Copy"), false) == text,
				"Paste/history lost identity, individual properties, tags, samples or group");
			auto const* agent = world->lookupAgent(pasted).entity;
			require(agent->getIndividualMobilityProfile() == mobility
				&& agent->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door)
					== (fresh ? core::MobilityUse::CanUse : core::MobilityUse::CannotUse),
				"Paste/restoration lost authored Mobility or reconstructed the wrong revision");
			agent_smoke::requireDoorRoute(*world, pasted, back, true);
			agent_smoke::requireDoorRoute(*world, original, back, false);
			// Revealing the default proves it was reconstructed even when masked.
			world->pauseSimulation();
			require(world->setAgentIndividualMobilityProfile(pasted, std::nullopt, &diagnostic), diagnostic);
			agent_smoke::requireDoorRoute(*world, pasted, back, fresh);
			require(world->setAgentIndividualMobilityProfile(pasted, mobility, &diagnostic), diagnostic);
		};
		verify();
		require(world->lookupAgent(pasted).entity->getStandingHeight() == preview.y,
			"Pasted bounds disagree with preview");
		auto restore = [&](DocumentSnapshot const& snapshot) {
			try {
				auto loaded = deserializeDocumentSnapshot(snapshot, world, context.temporaryRoot() / "clipboard.world.yaml");
				if (!loaded) return false;
				core::loadAndAttachAgentTagRegistry(*loaded, context.temporaryRoot() / "clipboard.world.yaml");
				world = std::move(loaded);
				return true;
			} catch (std::exception const& error) { diagnostic = error.what(); return false; }
		};
		auto const pasteUndone = gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore);
		require(pasteUndone, "Paste undo failed: " + diagnostic);
		require(!world->lookupAgent(pasted).entity && agentCount(*world) == 1,
			"Paste undo kept the pasted Agent or removed its source");
		definition.source = "return { api_version=2, type_id='Scout', display_name='Scout', new=function() error('redo constructor') end }";
		auto const undone = captureDocumentSnapshot(world);
		auto const redoCount = gWorldDocumentHistory.redoCount();
		require(!gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore)
			&& gWorldDocumentHistory.redoCount() == redoCount
			&& captureDocumentSnapshot(world)->yaml == undone->yaml,
			"Paste redo reused a removed instance or changed state after failure");
		definition.source = initialSource;
		require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Paste redo failed to recover");
		verify();
		// Public copy of the placed Agent must produce the same portable data.
		gWorldDocumentHistory.clear();
		auto const beforeCut = captureDocumentSnapshot(world);
		require(cutAgent(world, pasted, diagnostic), diagnostic);
		commitDocumentEdit(beforeCut);
		require(!world->lookupAgent(pasted).entity, "Cut kept the deleted Agent alive in the World");
		auto const deleted = captureDocumentSnapshot(world);
		auto const count = gWorldDocumentHistory.undoCount();
		require(count == 1, "Cut was not captured as one history edit");
		auto const undo = [&] { return gWorldDocumentHistory.undo(captureDocumentSnapshot(world), restore); };
		for (int failure = 0; failure != 4; ++failure)
		{
			if (failure == 0) definition.resourceName = "missing.agent.lua";
			if (failure == 1) definition.typeId = "Mismatched";
			if (failure == 2) definition.source = "return { api_version=2, type_id='Scout', display_name='Scout', new=function() error('fresh lifetime') end }";
			if (failure == 3) agent_smoke::replaceSource(definition.source, "door = \"cannot_use\"", "door = \"bad\"");
			require(!undo(), "Deletion undo accepted an invalid dependency or reused the deleted instance");
			require(captureDocumentSnapshot(world)->yaml == deleted->yaml
				&& gWorldDocumentHistory.undoCount() == count && !gWorldDocumentHistory.canRedo(),
				"Failed deletion restoration changed the World/history");
			if (failure == 3) require(diagnostic.find("scout.agent.lua") != std::string::npos
				&& diagnostic.find("door") != std::string::npos, "Invalid Mobility restoration lacked field/resource diagnostics");
			agent_smoke::requireDoorRoute(*world, original, back, false);
			definition = { "Scout", "Scout", "scout.agent.lua", initialSource };
		}
		auto const at = definition.source.find("width = 0.3");
		require(at != std::string::npos, "Scout fixture width not found");
		definition.source.replace(at, std::string("width = 0.3").size(), "width = 0.5");
		agent_smoke::replaceSource(definition.source, "door = \"cannot_use\"", "door = \"can_use\"");
		auto const deletionUndone = undo();
		require(deletionUndone, "Deleted-Agent undo did not recover: " + diagnostic);
		verify(true);
		require(world->lookupAgent(pasted).entity->getWidth() == 0.5f
			&& world->lookupAgent(original).entity->getWidth() == 0.3f,
			"Deletion restoration was not fresh or discarded the surviving instance");
		require(gWorldDocumentHistory.redo(captureDocumentSnapshot(world), restore), "Deletion redo failed");
		require(!world->lookupAgent(pasted).entity, "Deletion redo retained its Agent");
		require(undo(), "Repeated deletion undo failed");
		verify(true);
		gWorldDocumentHistory.clear();
		// Copy the restored, placed Agent through the public editor seam into a
		// new World. This exercises an actual copy, not just payload encoding.
		auto copyFixture = buildWorld("Fresh copied lifetime");
		copyFixture.world->pauseSimulation();
		constexpr uint32_t copyBack = 1;
		copyFixture.world->addCorridor(copyBack, 0, 0, 10, 1);
		copyFixture.world->addSectorDoor(0, 0, 2);
		copyFixture.world->finishBuild();
		copyFixture.world->attachAgentTagRegistry(registryPath.filename().string(), registry);
		auto copiedPayload = makeAgentClipboardPayload(*world, pasted, "Copy");
		core::AgentId copied;
		require(commitAgentPlacement(copyFixture.world, copiedPayload,
			copyFixture.world->getSector(copyFixture.corridor), 0, 4.f, copied, diagnostic), diagnostic);
		auto const* copy = copyFixture.world->lookupAgent(copied).entity;
		require(copy->getIndividualMobilityProfile() == mobility && copy->getAgentTagIds().contains(tag)
			&& copy->getTypeId() == "Scout" && copy->getTypeResourceName() == "scout.agent.lua"
			&& copy->getScriptDefaultMobilityProfile().get(core::TraversalKind::Door) == core::MobilityUse::CanUse,
			"Copied Agent did not retain overrides/identity with fresh script defaults");
		agent_smoke::requireDoorRoute(*copyFixture.world, copied, copyBack, true);
		require(copyFixture.world->setAgentIndividualMobilityProfile(copied, std::nullopt, &diagnostic), diagnostic);
		agent_smoke::requireDoorRoute(*copyFixture.world, copied, copyBack, true);
		gWorldDocumentHistory.clear();
	}

	void scriptedClipboardRefusalAndLegacy(smoke::Context const&)
	{
		gWorldDocumentHistory.clear();
		auto fixture = buildWorld("Clipboard refusals");
		auto const validDefinition = *core::resolveAgentTypeResource("scout.agent.lua");
		auto definition = validDefinition;
		auto loader = [&definition](std::string const& name)
			-> std::optional<core::AgentTypeDefinition> {
			if (name == "scout.agent.lua") return definition;
			if (name == "human.agent.lua") return core::bundledHumanAgentType();
			return std::nullopt;
		};
		AgentTypeLoaderScope scope{ loader };
		std::string diagnostic;
		auto const before = captureDocumentSnapshot(fixture.world);
		for (int failure = 0; failure != 5; ++failure)
		{
			auto payload = scoutPayload("Refused");
			if (failure == 0) payload.resource = "absent.agent.lua";
			if (failure == 1) payload.type = "NotScout";
			if (failure == 2) payload.resource.clear();
			if (failure == 3) definition.source = "return { api_version=2, type_id='Scout', display_name='Scout', new=function() error('preview constructor') end }";
			if (failure == 4) payload.individualHeightModifier = -1.f;
			// Each case is a newly selected/validated resource session, not an
			// implicit hot reload of an already accepted preview snapshot.
			core::setAgentTypeResourceLoader(loader);
			if (failure < 4)
			{
				auto dimensions = agentClipboardPlacementDimensions(payload, &diagnostic);
				require(dimensions.x == 0.f && dimensions.y == 0.f && !diagnostic.empty(),
					"Invalid clipboard preview silently became Human");
			}
			PendingAgentPlacement pending;
			require(!armAgentPlacement(pending, *fixture.world, payload,
				fixture.world->getSector(fixture.corridor), 0, 1.f, diagnostic)
				&& !pending.armed() && !diagnostic.empty(), "Invalid paste was armed");
			core::AgentId placed;
			require(!commitAgentPlacement(fixture.world, payload, fixture.world->getSector(fixture.corridor),
				0, 1.f, placed, diagnostic) && !placed && !diagnostic.empty(), "Invalid paste was published");
			require(captureDocumentSnapshot(fixture.world)->yaml == before->yaml
				&& !gWorldDocumentHistory.canUndo(), "Refused paste changed World/history");
			definition = validDefinition;
		}
		require(fixture.world->attachAgentType("competing.agent.lua", validDefinition.source, &diagnostic), diagnostic);
		auto const conflictBefore = captureDocumentSnapshot(fixture.world);
		PendingAgentPlacement conflict;
		require(!armAgentPlacement(conflict, *fixture.world, scoutPayload("Conflict"),
			fixture.world->getSector(fixture.corridor), 0, 1.f, diagnostic)
			&& diagnostic.find("competing resources") != std::string::npos,
			"Competing clipboard identity was armed");
		core::AgentId refused;
		require(!commitAgentPlacement(fixture.world, scoutPayload("Conflict"), fixture.world->getSector(fixture.corridor),
			0, 1.f, refused, diagnostic) && !refused
			&& captureDocumentSnapshot(fixture.world)->yaml == conflictBefore->yaml,
			"Conflicting paste changed the World");
		for (auto const* field : { "colour", "escalatorWalkingChance", "walkSpeedModifier", "heightModifier", "mobilityProfile" })
		{
			auto object = YAML::Load("name: Invalid property\nflags: 0\n");
			object[field] = YAML::Load("[-1, 999]");
			AgentClipboardPayload invalid;
			require(!readAgentClipboardObject(object, invalid, diagnostic) && !diagnostic.empty(),
				"Malformed individual clipboard property was accepted");
		}
		for (bool omitType : { false, true })
		{
			auto object = YAML::Load("name: Legacy\nflags: 0\n");
			if (!omitType) object["type"] = "Human";
			AgentClipboardPayload legacy;
			require(readAgentClipboardObject(object, legacy, diagnostic), diagnostic);
			core::AgentId placed;
			require(commitAgentPlacement(fixture.world, legacy, fixture.world->getSector(fixture.corridor),
				0, 1.f, placed, diagnostic), diagnostic);
			require(fixture.world->lookupAgent(placed).entity->getTypeResourceName() == "human.agent.lua",
				"Legacy clipboard did not resolve bundled Human");
		}
		gWorldDocumentHistory.clear();
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
	checks.push_back({ "agentTypesPreviewQueriesReuseValidatedResource", previewQueriesReuseValidatedResource });
	checks.push_back({ "agentTypesManagedPreviewIsReadOnly", managedPreviewIsReadOnly });
	checks.push_back({ "agentTypesScriptedClipboardAndDeletionHistory", scriptedClipboardAndDeletionHistory });
	checks.push_back({ "agentTypesScriptedClipboardRefusalAndLegacy", scriptedClipboardRefusalAndLegacy });
	checks.push_back({ "agentTypesExternalImportPlacesAndReopens", externalImportPlacesAndReopens });
	checks.push_back({ "agentTypesManagedResourceRevisionOnResetAndLoad", managedResourceRevisionOnResetAndLoad });
	checks.push_back({ "agentTypesExternalImportRefusesWithoutRegistration", externalImportRefusesWithoutRegistration });
	checks.push_back({ "agentTypesExternalPlacementRollsBackRegistration", externalPlacementRollsBackRegistration });
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
