#include "Checks.h"
#include "EditorState.h"
#include "TemporaryDirectory.h"
// External Agent tag registry document workflow checks for #128 and #129.

#include "TagsPanel.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "core/AgentTagRegistry.h"
#include "core/AgentTagRegistryDocument.h"
#include "core/World.h"
#include "core/TransactionalFileWriter.h"
#include "core/YamlSerializer.h"

namespace
{
	using tag_smoke::TemporaryDirectory;
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

	std::string serializeRegistry(core::AgentTagRegistry const& registry)
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData workData;
		workData.markSerializedUnmodified = false;
		registry.serialize(*writer, workData);
		writer->serialize();
		return writer->getSerializedString();
	}

	std::shared_ptr<core::World> loadWorld(std::filesystem::path const& path)
	{
		auto loaded = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromFile(path.string());
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "The World did not reload");
		return loaded;
	}

	void savedWorldCreatesAndReopensAdjacentRegistry(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto world = std::make_shared<core::World>("Station", 4, 2);
		auto const worldPath = temporary.path / "station.world.yaml";

		std::string diagnostic;
		require(!canCreateAgentTagRegistry(world, "", &diagnostic),
			"The Tags panel enabled registry creation before the World was saved");
		require(diagnostic.find("Save") != std::string::npos,
			"The unsaved-registry diagnostic did not explain the saved-location requirement");

		world->saveTo(worldPath.string());
		require(canCreateAgentTagRegistry(world, worldPath.string(), &diagnostic),
			"The Tags panel did not enable registry creation for a saved World");
		auto registry = core::createAndAttachAgentTagRegistry(*world, worldPath);
		auto const registryPath = temporary.path / "station.tags.yaml";
		require(std::filesystem::is_regular_file(registryPath),
			"The adjacent .tags.yaml registry was not created");
		require(world->hasAgentTagRegistryReference()
			&& world->hasAttachedAgentTagRegistry(),
			"The new registry was not attached to the World");
		require(world->getAgentTagRegistryResourceName() == "station.tags.yaml",
			"The World did not retain a basename-only registry reference");
		require(world->getExpectedAgentTagRegistryUuid() == registry->getUuid()
			&& core::AgentTagRegistry::uuidIsValid(registry->getUuid()),
			"The World did not retain the registry's stable UUID");
		require(registry->getNextAgentTagId() == 1
			&& registry->getNextPropertyRevision() == 1,
			"A new registry did not start both non-reused allocators at one");

		auto const registryYaml = readText(registryPath);
		require(registryYaml.find("version: 14") != std::string::npos
			&& registryYaml.find("uuid: " + registry->getUuid()) != std::string::npos
			&& registryYaml.find("nextAgentTagId: 1") != std::string::npos
			&& registryYaml.find("nextPropertyRevision: 1") != std::string::npos
			&& registryYaml.find("tags:") != std::string::npos,
			"The empty registry omitted its schema, UUID, allocators, or tag collection");

		// Persist the attachment, then model closing both documents and reopening
		// through the same core workflow used by the GUI.
		world->saveTo(worldPath.string());
		auto const worldYaml = readText(worldPath);
		require(worldYaml.find("version: 57") != std::string::npos
			&& worldYaml.find("resource: station.tags.yaml") != std::string::npos
			&& worldYaml.find("expectedUuid: " + registry->getUuid()) != std::string::npos,
			"The World did not persist its version-10 registry reference");

		auto reopened = loadWorld(worldPath);
		require(reopened->hasAgentTagRegistryReference()
			&& !reopened->hasAttachedAgentTagRegistry(),
			"World deserialization did not retain an unresolved registry reference");
		auto reopenedRegistry = core::loadAndAttachAgentTagRegistry(*reopened, worldPath);
		require(reopened->hasAttachedAgentTagRegistry()
			&& reopenedRegistry->getUuid() == registry->getUuid(),
			"The referenced empty registry did not survive close and reopen");

		// Replacing the adjacent file must not silently reinterpret the reference.
		auto replacement = core::AgentTagRegistry::create();
		replacement->saveTo(registryPath.string());
		auto substituted = loadWorld(worldPath);
		bool mismatchRefused{ false };
		try
		{
			(void)core::loadAndAttachAgentTagRegistry(*substituted, worldPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && !substituted->hasAttachedAgentTagRegistry(),
			"A substituted registry with a different UUID was attached");
	}

	void selectionEnforcesBasenameExtensionAndDirectory(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const project = temporary.path / "project";
		auto const otherProject = temporary.path / "other";
		std::filesystem::create_directories(project);
		std::filesystem::create_directories(otherProject);

		auto registry = core::AgentTagRegistry::create();
		auto const registryPath = project / "shared.tags.yaml";
		registry->saveTo(registryPath.string());

		auto world = std::make_shared<core::World>("Selection", 4, 2);
		auto const worldPath = project / "selection.world.yaml";
		world->saveTo(worldPath.string());
		std::string diagnostic;
		require(canSelectAgentTagRegistry(world, worldPath.string(), &diagnostic),
			"The Tags panel did not enable selection for a saved World");
		auto selected = core::selectAndAttachAgentTagRegistry(
			*world, worldPath, registryPath);
		require(world->getAgentTagRegistryResourceName() == "shared.tags.yaml",
			"Selection did not store only the registry basename");
		require(selected->getUuid() == registry->getUuid(),
			"Selection attached a registry with the wrong UUID");

		auto refused = std::make_shared<core::World>("Refused selection", 4, 2);
		auto const refusedPath = project / "refused.world.yaml";
		refused->saveTo(refusedPath.string());
		auto wrongExtension = core::AgentTagRegistry::create();
		auto const wrongExtensionPath = project / "registry.yaml";
		wrongExtension->saveTo(wrongExtensionPath.string());
		bool extensionRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*refused, refusedPath, wrongExtensionPath);
		}
		catch (std::exception const& error)
		{
			extensionRefused = std::string(error.what()).find(".tags.yaml")
				!= std::string::npos;
		}
		require(extensionRefused && !refused->hasAgentTagRegistryReference()
			&& !refused->isModified(),
			"A registry with the wrong extension disturbed the World");

		auto outside = core::AgentTagRegistry::create();
		auto const outsidePath = otherProject / "outside.tags.yaml";
		outside->saveTo(outsidePath.string());
		bool directoryRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*refused, refusedPath, outsidePath);
		}
		catch (std::exception const& error)
		{
			directoryRefused = std::string(error.what()).find("same directory")
				!= std::string::npos;
		}
		require(directoryRefused && !refused->hasAgentTagRegistryReference()
			&& !refused->isModified(),
			"A registry outside the World directory disturbed the World");
	}

	void tagNamesIdentityOrderingAndNoOpEdits(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto registry = core::AgentTagRegistry::create();
		auto const path = temporary.path / "names.tags.yaml";
		registry->saveTo(path.string());
		auto& history = agentTagRegistryDocumentHistory(registry);
		require(!history.isModified() && !registry->isModified(),
			"A newly saved registry did not start clean");

		std::string diagnostic;
		require(core::AgentTag::nameIsValid("abcdefghijkl", &diagnostic),
			"A valid twelve-character Agent tag name was refused");
		auto const cleanYaml = serializeRegistry(*registry);
		require(!commitAgentTagAdd(registry, "#invalid", diagnostic)
			&& serializeRegistry(*registry) == cleanYaml
			&& !history.canUndo() && !history.isModified() && !registry->isModified(),
			"A refused tag submission dirtied a clean registry or created history");

		auto const crew = commitAgentTagAdd(registry, "crew", diagnostic);
		auto const night = commitAgentTagAdd(registry, "night-shift", diagnostic);
		require(crew.value == 1 && night.value == 2
			&& registry->getNextAgentTagId() == 3,
			"Agent tags did not receive monotonic non-zero IDs");
		require(history.undoCount() == 2 && agentTagRegistryIsModified(registry),
			"Accepted tag additions did not dirty only the registry history");

		auto const beforeRefusals = serializeRegistry(*registry);
		auto const undoBeforeRefusals = history.undoCount();
		for (auto const* invalid : { "", "#crew", "Crew", "crew_2", "-crew",
			"crew-", "night--crew", "abcdefghijklm" })
		{
			require(!commitAgentTagAdd(registry, invalid, diagnostic),
				"An invalid Agent tag name was accepted");
		}
		require(!commitAgentTagAdd(registry, "crew", diagnostic),
			"A duplicate Agent tag name was accepted");
		require(!commitAgentTagRename(registry, crew, "crew", diagnostic),
			"An unchanged Agent tag rename was accepted");
		require(!commitAgentTagRename(registry, crew, "#crew", diagnostic),
			"An invalid Agent tag rename was accepted");
		require(!commitAgentTagRename(registry, crew, "night-shift", diagnostic),
			"A duplicate Agent tag rename was accepted");
		require(!commitAgentTagRename(registry, core::AgentTagId{ 99 }, "ghost", diagnostic),
			"An unknown Agent tag was renamed");
		require(serializeRegistry(*registry) == beforeRefusals
			&& history.undoCount() == undoBeforeRefusals,
			"A refused or unchanged tag submission mutated state or history");

		require(commitAgentTagRename(registry, crew, "zulu", diagnostic),
			"A valid Agent tag rename was refused");
		require(registry->getAgentTagName(crew) == "zulu"
			&& registry->getNextAgentTagId() == 3,
			"Rename changed an Agent tag's identity or allocator");
		auto const alphabetical = registry->getAgentTagIdsAlphabetically();
		require(alphabetical.size() == 2 && alphabetical[0] == night
			&& alphabetical[1] == crew,
			"Agent tags were not presented alphabetically");

		auto const yaml = serializeRegistry(*registry);
		// Match the sequence items, not the bare text: a UUID can begin with
		// hex digits that make "id: 2" appear inside the uuid line first.
		auto const idOne = yaml.find("- id: 1");
		auto const idTwo = yaml.find("- id: 2");
		require(idOne != std::string::npos && idTwo != std::string::npos && idOne < idTwo,
			"Registry serialization followed display order instead of identity order");

		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic)
			&& registry->getAgentTagName(crew) == "crew",
			"Registry undo did not restore the pre-rename name and identity");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic)
			&& registry->getAgentTagName(crew) == "zulu",
			"Registry redo did not restore the renamed tag");
		forgetAgentTagRegistryDocument(registry);
	}

	void tagsPersistAndDeletedIdsAreNeverReused(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const path = temporary.path / "manual.tags.yaml";
		auto registry = core::AgentTagRegistry::create();
		registry->saveTo(path.string());
		(void)agentTagRegistryDocumentHistory(registry);

		std::string diagnostic;
		auto const crew = commitAgentTagAdd(registry, "crew", diagnostic);
		auto const night = commitAgentTagAdd(registry, "night-shift", diagnostic);
		require(commitAgentTagRename(registry, crew, "day-crew", diagnostic),
			"The manual-validation rename was refused");
		require(saveAgentTagRegistry(registry, path.string(), &diagnostic),
			"The edited Agent tag registry did not save");
		require(!agentTagRegistryIsModified(registry),
			"Saving a registry did not clear its independent dirty state");

		auto reopened = core::AgentTagRegistry::loadFrom(path.string());
		require(reopened->getAgentTagName(crew) == "day-crew"
			&& reopened->getAgentTagName(night) == "night-shift"
			&& reopened->getNextAgentTagId() == 3,
			"Tag names, identities, or allocator did not survive save/load");
		auto const alphabetical = reopened->getAgentTagIdsAlphabetically();
		require(alphabetical.size() == 2 && alphabetical[0] == crew
			&& alphabetical[1] == night,
			"Reopened tags were not alphabetically presented");

		require(commitAgentTagDelete(reopened, crew, diagnostic),
			"The deletion used to test ID non-reuse was refused");
		require(saveAgentTagRegistry(reopened, path.string(), &diagnostic),
			"The registry did not save after deletion");
		auto afterDelete = core::AgentTagRegistry::loadFrom(path.string());
		require(afterDelete->getNextAgentTagId() == 3,
			"Deleting and reopening moved the tag allocator backwards");
		auto const replacement = afterDelete->addAgentTag("reserve");
		require(replacement.value == 3 && replacement != crew,
			"A deleted AgentTagId was reused");

		forgetAgentTagRegistryDocument(registry);
		forgetAgentTagRegistryDocument(reopened);
	}

	void registryDirtyStateAndCloseWarningStayIndependent(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto world = std::make_shared<core::World>("Independent", 4, 2);
		auto const worldPath = temporary.path / "independent.world.yaml";
		world->saveTo(worldPath.string());
		auto registry = core::createAndAttachAgentTagRegistry(*world, worldPath);
		world->saveTo(worldPath.string());
		world->pauseSimulation();
		auto& history = agentTagRegistryDocumentHistory(registry);
		require(!history.isModified() && !world->isModified(),
			"Saved World and registry documents did not start independently clean");

		std::string diagnostic;
		auto const tag = commitAgentTagAdd(registry, "crew", diagnostic);
		require(tag && attachedAgentTagRegistryIsModified(world)
			&& !world->isModified(),
			"A registry edit dirtied the World or failed to arm its close warning");
		require(restoreAgentTagRegistrySnapshot(registry, false, &diagnostic)
			&& !attachedAgentTagRegistryIsModified(world)
			&& !world->isModified(),
			"Registry undo did not return independently to its saved state");
		require(restoreAgentTagRegistrySnapshot(registry, true, &diagnostic)
			&& attachedAgentTagRegistryIsModified(world)
			&& !world->isModified(),
			"Registry redo leaked dirty state into the World");

		auto const registryPath = temporary.path / "independent.tags.yaml";
		require(saveAgentTagRegistry(registry, registryPath.string(), &diagnostic)
			&& !attachedAgentTagRegistryIsModified(world)
			&& !world->isModified(),
			"Registry Save did not operate independently from the World");
		forgetAgentTagRegistryDocument(registry);
	}
}

void tag_smoke::registerRegistryEditor(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "tags/savedWorldCreatesAndReopensAdjacentRegistry",
		[](smoke::Context const& context)
		{
			EditorState state;
			savedWorldCreatesAndReopensAdjacentRegistry(context);
		} });
	checks.push_back({ "tags/selectionEnforcesBasenameExtensionAndDirectory",
		[](smoke::Context const& context)
		{
			EditorState state;
			selectionEnforcesBasenameExtensionAndDirectory(context);
		} });
	checks.push_back({ "tags/tagNamesIdentityOrderingAndNoOpEdits",
		[](smoke::Context const& context)
		{
			EditorState state;
			tagNamesIdentityOrderingAndNoOpEdits(context);
		} });
	checks.push_back({ "tags/tagsPersistAndDeletedIdsAreNeverReused",
		[](smoke::Context const& context)
		{
			EditorState state;
			tagsPersistAndDeletedIdsAreNeverReused(context);
		} });
	checks.push_back({ "tags/registryDirtyStateAndCloseWarningStayIndependent",
		[](smoke::Context const& context)
		{
			EditorState state;
			registryDirtyStateAndCloseWarningStayIndependent(context);
		} });
}
