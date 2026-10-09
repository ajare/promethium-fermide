#include "Checks.h"
#include "TemporaryDirectory.h"
#include "EditorState.h"
#include "ImGuiContext.h"
// External Agent behaviour registry package workflow and atomic reload checks.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include "core/AgentTagRegistryDocument.h"
#include <stdexcept>
#include <string>

#include "BehavioursPanel.h"
#include "DocumentEdit.h"
#include "imgui/imgui.h"
#include "core/AgentBehaviourRegistry.h"
#include "core/AgentBehaviourRegistryDocument.h"
#include "core/World.h"
#include "core/Log.h"
#include "core/TransactionalFileWriter.h"
#include "core/YamlSerializer.h"

namespace
{
	using behaviour_smoke::TemporaryDirectory;
	void require(bool condition, char const* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	std::string readText(std::filesystem::path const& path)
	{
		std::ifstream input(path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
	}

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write behaviour package fixture");
	}

	std::string serializeWorld(core::World const& world)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		world.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::filesystem::path manifestPath(std::filesystem::path const& packageDirectory)
	{
		return core::agentBehaviourRegistryManifestPath(packageDirectory);
	}

	void hotReloadIsAtomicAcrossSourceHelpersAndDependentWorlds(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const package = temporary.path / "atomic.behaviours";
		std::filesystem::create_directories(package);
		auto const uuid = std::string("123e4567-e89b-42d3-a456-426614174157");
		auto const manifest = ""
			"  version: 1\n"
			"  uuid: " + uuid + "\n"
			"  revision: 1\n"
			"  modules:\n"
			"    - name: helpers.reload\n"
			"      source: helper.lua\n"
			"  nextBehaviourId: 2\n"
			"  behaviours:\n"
			"    - id: 1\n"
			"      name: Atomic\n"
			"      revision: 1\n"
			"      source: atomic.lua\n"
			"      schema:\n"
			"        - name: expected\n"
			"          type: integer\n"
			"        - name: code\n"
			"          type: integer\n"
			"        - name: destination\n"
			"          type: marker\n";
		auto behaviourSource = [](std::string const& sourceRevision)
		{
			return "local helper = require('helpers.reload')\n"
				"local source_revision = '" + sourceRevision + "'\n"
				"return { api_version = 3, factory = function(configuration)\n"
				"  if configuration.expected ~= helper.expected then error('wrong expected value') end\n"
				"  return function(context)\n"
				"    context.log('start:' .. source_revision .. ':' .. helper.generation .. ':' .. context.random_integer(1, 1000000))\n"
				"    context.set_timer('preserved', 3)\n"
				"    local moved = context.move_to(configuration.destination)\n"
				"    if not moved.accepted then error(moved.status) end\n"
				"    while true do\n"
				"      local event = wait()\n"
				"      if event.type == 'timer_expired' then\n"
				"        context.log('timer:' .. source_revision .. ':' .. helper.generation .. ':' .. event.name)\n"
				"      end\n"
				"    end\n"
				"  end\n"
				"end }\n";
		};
		writeText(manifestPath(package), manifest);
		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v1' }\n");
		writeText(package / "atomic.lua", behaviourSource("v1"));

		struct Fixture
		{
			std::shared_ptr<core::World> world;
			std::filesystem::path path;
			std::vector<core::AgentId> agents;
		};
		auto makeWorld = [&](std::string name, std::string filename,
			unsigned agentCount)
		{
			Fixture fixture;
			fixture.world = std::make_shared<core::World>(std::move(name), 10, 2);
			auto const room = fixture.world->addRoom("Room", 0, 0, 0, 10, 1);
			fixture.world->addSectorMarker(room, 0, 8.5f, "Destination");
			fixture.world->finishBuild();
			for (unsigned index = 0; index < agentCount; ++index)
				fixture.agents.push_back(fixture.world->createAgent(
					"Agent " + std::to_string(index + 1), room, 0,
					0.5f + static_cast<float>(index)));
			fixture.world->pauseSimulation();
			fixture.path = temporary.path / filename;
			fixture.world->saveTo(fixture.path.string());
			return fixture;
		};
		auto alpha = makeWorld("Alpha", "alpha.world.yaml", 2);
		auto zulu = makeWorld("Zulu", "zulu.world.yaml", 1);

		// Attach in reverse display order. Reload diagnostics must still use stable
		// World-name order and stable Agent-ID order.
		auto registry = core::selectAndAttachAgentBehaviourRegistry(
			*zulu.world, zulu.path, package);
		require(core::selectAndAttachAgentBehaviourRegistry(
				*alpha.world, alpha.path, package) == registry,
			"Atomic reload dependents did not share one registry");
		auto const behaviour = core::AgentBehaviourId{ 1 };
		auto assign = [&](Fixture& fixture, uint64_t& code)
		{
			for (auto agent : fixture.agents)
			{
				require(fixture.world->setAgentBehaviourAssignment(agent, behaviour, 1, {
					{ "expected", int64_t{ 7 } },
					{ "code", static_cast<int64_t>(code++) },
					{ "destination", fixture.world->getMarkerIds().front() }
				}), "Could not assign an atomic reload fixture");
			}
			fixture.world->saveTo(fixture.path.string());
		};
		uint64_t code = 1;
		assign(alpha, code);
		assign(zulu, code);
		require(!registry->isModified() && !alpha.world->isModified()
			&& !zulu.world->isModified(),
			"Atomic reload fixtures did not start with clean documents");

		auto runStartBoundary = [&](Fixture& fixture)
		{
			require(fixture.world->resumeSimulation(),
				"Could not resume an atomic reload fixture");
			require(fixture.world->advanceTick(),
				"A restarted atomic reload instance failed");
			fixture.world->pauseSimulation();
		};
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		(void)core::consumeLogMessages();

		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v2' }\n");
		writeText(package / "atomic.lua", behaviourSource("v2"));
		std::string diagnostic;
		std::vector<core::AgentBehaviourReloadDiagnostic> reloadDiagnostics;

		// One running dependent refuses the transaction before any scratch module
		// executes or any live state changes.
		require(zulu.world->resumeSimulation(),
			"Could not run the dependent used by the reload refusal check");
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& diagnostic.find("Zulu") != std::string::npos,
			"A reload was not refused while a dependent World was running");
		zulu.world->pauseSimulation();

		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.empty(),
			"Valid source/helper reload did not commit atomically");
		auto stopMessages = core::consumeLogMessages();
		auto const stopCount = std::count_if(stopMessages.begin(), stopMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg.starts_with("start:v1:") || message.msg.starts_with("timer:v1:");
			});
		require(stopCount == 0,
			"Successful reload resumed an old suspended coroutine");
		for (auto agent : alpha.agents)
			require(!alpha.world->lookupAgent(agent).entity->getPath(),
				"Successful reload retained old behaviour movement");

		auto collectStarts = []
		{
			auto messages = core::consumeLogMessages();
			std::vector<std::string> starts;
			for (auto const& message : messages)
				if (message.msg.starts_with("start:v2:v2:")) starts.push_back(message.msg);
			return starts;
		};
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		auto const firstRestart = collectStarts();
		require(firstRestart.size() == 3,
			"Reloaded instances were not recreated from all authored configurations");

		auto const historyState = gWorldDocumentHistory.currentStateId();
		auto const undoCount = gWorldDocumentHistory.undoCount();
		auto const redoCount = gWorldDocumentHistory.redoCount();
		auto assertRollback = [&](std::string const& candidate,
			core::AgentBehaviourReloadDiagnosticScope expectedScope)
		{
			writeText(package / "atomic.lua", candidate);
			reloadDiagnostics.clear();
			auto const reloaded = core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics);
			require(!reloaded && !reloadDiagnostics.empty()
				&& reloadDiagnostics.front().scope == expectedScope
				&& registry->lookupAgentBehaviour(behaviour)->getModuleStatus()
					== core::AgentBehaviourModuleStatus::Loaded
				&& !registry->isModified() && !alpha.world->isModified()
				&& !zulu.world->isModified()
				&& gWorldDocumentHistory.currentStateId() == historyState
				&& gWorldDocumentHistory.undoCount() == undoCount
				&& gWorldDocumentHistory.redoCount() == redoCount,
				"A failed reload changed live registry/runtime document state or history");
		};
		assertRollback("return { api_version = 3, factory = function( }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("require('helpers.missing')\nreturn { api_version = 3, factory = function() return function(context) while true do wait() end end end }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("return { api_version = 99, factory = function() return function(context) wait() end end }\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);
		assertRollback("while true do end\n",
			core::AgentBehaviourReloadDiagnosticScope::Module);

		writeText(package / "atomic.lua", behaviourSource("v2"));
		writeText(package / "helper.lua", "return { broken = function( }\n");
		reloadDiagnostics.clear();
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.size() >= 2
			&& std::all_of(reloadDiagnostics.begin(), reloadDiagnostics.end(),
				[](core::AgentBehaviourReloadDiagnostic const& item)
				{
					return item.scope
						== core::AgentBehaviourReloadDiagnosticScope::Module;
				})
			&& !registry->isModified() && !alpha.world->isModified()
			&& !zulu.world->isModified(),
			"Helper-graph failures were not aggregated without mutation");
		writeText(package / "helper.lua",
			"return { expected = 7, generation = 'v2' }\n");

		writeText(package / "atomic.lua", ""
			"return { api_version = 3, factory = function(configuration)\n"
			"  error('factory rejected code ' .. configuration.code)\n"
			"end }\n");
		reloadDiagnostics.clear();
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics)
			&& reloadDiagnostics.size() == 3
			&& reloadDiagnostics[0].scope
				== core::AgentBehaviourReloadDiagnosticScope::Agent
			&& reloadDiagnostics[0].worldName == "Alpha"
			&& reloadDiagnostics[0].agent == alpha.agents[0]
			&& reloadDiagnostics[1].worldName == "Alpha"
			&& reloadDiagnostics[1].agent == alpha.agents[1]
			&& reloadDiagnostics[2].worldName == "Zulu"
			&& reloadDiagnostics[2].agent == zulu.agents[0],
			"Per-configuration factory failures were not aggregated in stable World/Agent order");

		// All failed attempts leave the v2 instances and their timers intact. A
		// restart would emit start again and postpone these timers.
		for (auto* fixture : { &alpha, &zulu })
		{
			require(fixture->world->resumeSimulation(),
				"Could not resume after a refused reload");
			require(fixture->world->advanceTicks(3),
				"The previous runtime failed after a refused reload");
			fixture->world->pauseSimulation();
		}
		auto retainedMessages = core::consumeLogMessages();
		auto retainedTimers = std::count_if(retainedMessages.begin(), retainedMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg == "timer:v2:v2:preserved";
			});
		auto repeatedStarts = std::count_if(retainedMessages.begin(), retainedMessages.end(),
			[](core::LogMessage const& message)
			{
				return message.msg.starts_with("start:");
			});
		require(retainedTimers == 3 && repeatedStarts == 0,
			"Failed reload did not preserve the previous live instance state");

		// Re-adopting the same valid revision restarts deterministic random streams
		// and produces the same per-Agent startup outcomes.
		writeText(package / "atomic.lua", behaviourSource("v2"));
		require(core::reloadAgentBehaviourRegistryDocument(registry, package,
				&diagnostic, &reloadDiagnostics),
			"Could not recover from refused reload candidates");
		(void)core::consumeLogMessages(); // successful-reload teardown
		runStartBoundary(alpha);
		runStartBoundary(zulu);
		auto const secondRestart = collectStarts();
		require(secondRestart == firstRestart,
			"Repeated successful reloads produced nondeterministic restarted outcomes");
	}

	void packageContainmentAndLifecycle(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto world = std::make_shared<core::World>("Containment", 4, 2);
		world->pauseSimulation();
		auto const path = temporary.path / "containment.world.yaml";
		world->saveTo(path.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(*world, path);
		auto const package = core::defaultAgentBehaviourRegistryPackagePath(path);
		writeText(package / "nested" / "source.lua",
			"return {api_version=3,factory=function() return function(context) error('must never execute') end end}\n");
		auto id = registry->addAgentBehaviour("  Schedule  ", "nested/source.lua", {});
		require(registry->getBehaviourName(id) == "Schedule", "Authored names were not trimmed");
		registry->saveTo(manifestPath(package).string());
		auto const valid = readText(manifestPath(package));
		std::string diagnostic;
		require(core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
			&& registry->lookupAgentBehaviour(id)->getModuleStatus()
				== core::AgentBehaviourModuleStatus::Loaded,
			"Nested managed source was refused or an Agent callback executed");
		world->saveTo(path.string());
		world->resetSimulation();
		require(world->getAgentBehaviourRegistry() == registry
			&& registry->hasLoadedWorld(world.get()), "Reset lost the registry attachment");

		// Escaping symlinks are rejected after canonicalization, including the
		// manifest itself. Windows may not grant symlink creation privileges.
		writeText(temporary.path / "outside.lua", "error('outside')\n");
		std::error_code error;
		std::filesystem::remove(package / "nested" / "source.lua");
		std::filesystem::create_symlink(temporary.path / "outside.lua",
			package / "nested" / "source.lua", error);
		if (!error)
		{
			require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
				&& registry->getBehaviourCount() == 1, "A symlink escaped the package");
			std::filesystem::remove(package / "nested" / "source.lua");
		}
		writeText(package / "nested" / "source.lua",
			"return {api_version = 3,factory=function() return function(context) while true do wait() end end end}\n");
		writeText(temporary.path / "outside.yaml", valid);
		std::filesystem::remove(manifestPath(package));
		error.clear();
		std::filesystem::create_symlink(temporary.path / "outside.yaml", manifestPath(package), error);
		if (!error)
		{
			require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic),
				"A manifest symlink escaped the package");
			std::filesystem::remove(manifestPath(package));
		}
		writeText(manifestPath(package), valid);

		auto regressed = valid;
		auto const allocator = regressed.find("nextBehaviourId: 2");
		require(allocator != std::string::npos, "Missing fixture allocator");
		regressed.replace(allocator, std::string("nextBehaviourId: 2").size(), "nextBehaviourId: 1");
		writeText(manifestPath(package), regressed);
		require(!core::reloadAgentBehaviourRegistryDocument(registry, package, &diagnostic)
			&& registry->getNextBehaviourId() == 2, "Malformed allocator changed live state");
		writeText(manifestPath(package), valid);

		world->finishBuild();
		require(world->resumeSimulation(), "Could not resume lifecycle fixture");
		bool refused = false;
		try { world->detachAgentBehaviourRegistry(); }
		catch (std::exception const&) { refused = true; }
		require(refused && world->hasAttachedAgentBehaviourRegistry(), "Running detach succeeded");
		world->pauseSimulation();

		// Exercise the actual extracted panel in a CPU-side ImGui context.
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		ImGui::NewFrame();
		ImGui::Begin("Behaviour registry smoke");
		require(!renderBehavioursPanel(world, path.string()), "Inspection edited the World");
		ImGui::End();
		ImGui::Render();

		world->detachAgentBehaviourRegistry();
		require(!registry->hasLoadedWorlds()
			&& readText(manifestPath(package)) == valid, "Detach changed the package or kept its dependent");
		require(core::unloadAgentBehaviourRegistryDocumentIfUnused(registry), "Unused package did not unload");
	}

	void recoverDetachAndReplaceUsedRegistrySafely(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const worldPath = temporary.path / "recovery.world.yaml";
		auto const package = temporary.path / "recovery.behaviours";
		auto const hiddenPackage = temporary.path / "recovery.hidden";
		auto world = std::make_shared<core::World>("Recovery", 8, 2);
		auto const corridor = world->addCorridor(0, 0, 6);
		world->finishBuild();
		auto const agent = world->createAgent("Assigned", corridor, 0, 1.5f);
		world->pauseSimulation();
		world->saveTo(worldPath.string());
		auto registry = core::createAndAttachAgentBehaviourRegistry(
			*world, worldPath);
		writeText(package / "worker.lua",
			"return {api_version = 3,factory=function(config) return function(context) while true do wait() end end end}\n");
		auto const behaviour = registry->addAgentBehaviour("Worker", "worker.lua", {});
		registry->saveTo(manifestPath(package).string());
		std::string diagnostic;
		require(world->setAgentBehaviourAssignment(agent, behaviour, 1, {},
			&diagnostic), "Could not author the recovery assignment");
		world->saveTo(worldPath.string());
		auto const originalManifest = readText(manifestPath(package));
		auto const originalSource = readText(package / "worker.lua");
		auto const expectedUuid = registry->getUuid();
		world.reset();
		require(core::unloadAgentBehaviourRegistryDocumentIfUnused(registry),
			"Could not release the recovery package before dependency fixtures");
		registry.reset();

		auto assertRecoverableOpen = [&](char const* expectedDiagnostic)
		{
			auto loaded = core::loadWorldDocument(worldPath);
			require(loaded->getName() == "Recovery" && loaded->getNumSectors() == 1
				&& loaded->hasAgentBehaviourRegistryReference()
				&& !loaded->hasAttachedAgentBehaviourRegistry()
				&& loaded->getAgentBehaviourAssignmentCount() == 1
				&& loaded->getAgentBehaviourAssignment(agent)
				&& !loaded->agentBehaviourConfigurationsAreValid()
				&& loaded->getAgentBehaviourDependencyDiagnostic().find(expectedDiagnostic)
					!= std::string::npos
				&& !loaded->resumeSimulation(),
				"A broken package did not preserve a blocked structural World and its assignment");
			return loaded;
		};

		std::filesystem::rename(package, hiddenPackage);
		auto unresolved = assertRecoverableOpen("missing");
		std::filesystem::rename(hiddenPackage, package);
		writeText(manifestPath(package), "agentBehaviourRegistry: [not valid");
		(void)assertRecoverableOpen("Could not load");
		writeText(manifestPath(package), originalManifest);
		auto unsupported = originalManifest;
		auto const version = unsupported.find("version: 1");
		require(version != std::string::npos, "Recovery manifest omitted its version");
		unsupported.replace(version, std::string("version: 1").size(), "version: 2");
		writeText(manifestPath(package), unsupported);
		(void)assertRecoverableOpen("Unsupported");
		writeText(manifestPath(package), originalManifest);
		auto substituted = originalManifest;
		auto const uuidOffset = substituted.find(expectedUuid);
		require(uuidOffset != std::string::npos, "Recovery manifest omitted its UUID");
		auto replacementIdentity = core::AgentBehaviourRegistry::create();
		substituted.replace(uuidOffset, expectedUuid.size(),
			replacementIdentity->getUuid());
		writeText(manifestPath(package), substituted);
		(void)assertRecoverableOpen("UUID mismatch");
		writeText(manifestPath(package), originalManifest);

		// A syntactically valid package whose assigned factory fails is not a
		// repair. Candidate construction happens before reference/runtime adoption.
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		auto const beforeFailedRepair = serializeWorld(*unresolved);
		auto const beforeFailedRepairModified = unresolved->isModified();
		writeText(package / "worker.lua",
			"return {api_version=3,factory=function(config) error('broken repair') end}\n");
		require(!commitAgentBehaviourRegistrySwitch(unresolved,
			worldPath.string(), package.string(), diagnostic)
			&& diagnostic.find("runtime preflight failed") != std::string::npos
			&& serializeWorld(*unresolved) == beforeFailedRepair
			&& unresolved->isModified() == beforeFailedRepairModified
			&& !gWorldDocumentHistory.canUndo(),
			"A failed expected-package repair changed authored state, dirty state, or history");
		writeText(package / "worker.lua", originalSource);

		// Selecting the expected repaired package validates every authored
		// configuration and constructs all factories before making it live. The
		// persisted reference is unchanged, so recovery is not an authored edit.
		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		auto const unresolvedYaml = serializeWorld(*unresolved);
		auto const unresolvedModified = unresolved->isModified();
		require(commitAgentBehaviourRegistrySwitch(unresolved,
			worldPath.string(), package.string(), diagnostic),
			"The repaired expected package did not attach");
		require(unresolved->hasAttachedAgentBehaviourRegistry()
			&& unresolved->agentBehaviourConfigurationsAreValid()
			&& unresolved->getExpectedAgentBehaviourRegistryUuid() == expectedUuid
			&& unresolved->getAgentBehaviourAssignment(agent)
			&& unresolved->getAgentBehaviourAssignment(agent)->behaviour == behaviour
			&& serializeWorld(*unresolved) == unresolvedYaml
			&& unresolved->isModified() == unresolvedModified
			&& !gWorldDocumentHistory.canUndo(),
			"Recovery changed authored state, assignment data, dirty state, or history");

		// Another loaded World proves that switching one dependent never unloads
		// a shared package. A third exercises confirmed destructive detachment.
		auto shared = core::loadWorldDocument(worldPath);
		shared->pauseSimulation();
		require(shared->getAgentBehaviourRegistry()
			== unresolved->getAgentBehaviourRegistry(),
			"Canonical recovery packages did not share one loaded instance");
		auto detacher = core::loadWorldDocument(worldPath);
		detacher->pauseSimulation();
		gWorldDocumentHistory.clear();
		requestAgentBehaviourRegistryDetach(detacher);
		std::string consequence;
		require(agentBehaviourRegistryChangePending(&consequence)
			&& consequence.find("clear all 1 Agent behaviour assignment")
				!= std::string::npos,
			"Used detachment did not require explicit destructive confirmation");
		require(confirmPendingAgentBehaviourRegistryChange(diagnostic)
			&& !detacher->hasAgentBehaviourRegistryReference()
			&& detacher->getAgentBehaviourAssignmentCount() == 0
			&& gWorldDocumentHistory.undoCount() == 1,
			"Confirmed used detachment did not clear assignment/configuration atomically");

		gWorldDocumentHistory.clear();
		gWorldDocumentHistory.markSaved();
		auto const before = serializeWorld(*unresolved);
		auto const sourceRegistry = unresolved->getAgentBehaviourRegistry();
		auto const modified = unresolved->isModified();
		require(!commitAgentBehaviourRegistryDetach(unresolved, diagnostic)
			&& diagnostic.find("confirmed destructive action") != std::string::npos
			&& serializeWorld(*unresolved) == before
			&& unresolved->getAgentBehaviourRegistry() == sourceRegistry
			&& unresolved->isModified() == modified
			&& !gWorldDocumentHistory.canUndo(),
			"Direct used detachment changed state or history");
		requestAgentBehaviourRegistryDetach(unresolved);
		cancelPendingAgentBehaviourRegistryChange();
		require(!agentBehaviourRegistryChangePending()
			&& serializeWorld(*unresolved) == before
			&& unresolved->getAgentBehaviourRegistry() == sourceRegistry
			&& !gWorldDocumentHistory.canUndo(),
			"Cancelling used detachment changed state or history");

		requestAgentBehaviourRegistrySwitch(unresolved, worldPath.string(),
			(temporary.path / "missing.behaviours").string());
		require(!confirmPendingAgentBehaviourRegistryChange(diagnostic)
			&& serializeWorld(*unresolved) == before
			&& unresolved->getAgentBehaviourRegistry() == sourceRegistry
			&& unresolved->isModified() == modified
			&& !gWorldDocumentHistory.canUndo(),
			"A failed destructive replacement cleared data, replaced runtime, or changed history");

		auto const replacementPackage = temporary.path / "replacement.behaviours";
		std::filesystem::create_directories(replacementPackage);
		auto replacement = core::AgentBehaviourRegistry::create();
		replacement->saveTo(manifestPath(replacementPackage).string());
		requestAgentBehaviourRegistrySwitch(unresolved, worldPath.string(),
			replacementPackage.string());
		require(agentBehaviourRegistryChangePending(&consequence)
			&& consequence.find("replacement.behaviours") != std::string::npos,
			"Used replacement did not describe its destructive consequence");
		require(confirmPendingAgentBehaviourRegistryChange(diagnostic)
			&& unresolved->getAgentBehaviourRegistryResourceName()
				== "replacement.behaviours"
			&& unresolved->getAgentBehaviourAssignmentCount() == 0
			&& !unresolved->getAgentBehaviourAssignment(agent)
			&& gWorldDocumentHistory.undoCount() == 1
			&& gWorldDocumentHistory.isModified(),
			"Confirmed replacement did not leave one unsaved, undoable World edit while clearing assignments and changing reference");
		require(sourceRegistry->hasLoadedWorld(shared.get())
			&& !core::unloadAgentBehaviourRegistryDocumentIfUnused(sourceRegistry),
			"Switching one World unloaded a registry still shared by another");

		// Dirty unreferenced package work survives ordinary detach/replacement and
		// is released only by an explicit discard after the final dependent leaves.
		require(sourceRegistry->renameAgentBehaviour(behaviour, "Unsaved worker",
			&diagnostic), "Could not dirty the shared source registry");
		require(commitAgentBehaviourRegistryDetachClearingAssignments(
			shared, diagnostic),
			"The final shared registry did not detach after explicit assignment clearing");
		require(sourceRegistry->isModified()
			&& !core::unloadAgentBehaviourRegistryDocumentIfUnused(sourceRegistry)
			&& core::unloadAgentBehaviourRegistryDocumentIfUnused(sourceRegistry, true),
			"Dirty unreferenced package work was silently discarded or could not be explicitly discarded");
		cancelPendingAgentBehaviourRegistryChange();
		gWorldDocumentHistory.clear();
	}
}

void behaviour_smoke::registerRegistryEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "behaviours/hotReloadIsAtomicAcrossSourceHelpersAndDependentWorlds", [](smoke::Context const& context)
	{
		EditorState state;
		hotReloadIsAtomicAcrossSourceHelpersAndDependentWorlds(context);
	} });
	checks.push_back({ "behaviours/recoverDetachAndReplaceUsedRegistrySafely", [](smoke::Context const& context)
	{
		EditorState state;
		recoverDetachAndReplaceUsedRegistrySafely(context);
	} });
	checks.push_back({ "behaviours/packageContainmentAndLifecycle", [](smoke::Context const& context)
	{
		EditorState state;
		packageContainmentAndLifecycle(context);
	} });
}
