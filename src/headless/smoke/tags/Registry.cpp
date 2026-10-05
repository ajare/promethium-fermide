#include "Checks.h"
#include "TemporaryDirectory.h"
// External Agent tag registry document workflow checks for #128 and #129.

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

	void writeText(std::filesystem::path const& path, std::string const& text)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output << text;
		if (!output) throw std::runtime_error("Could not write registry test fixture");
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

	void olderWorldWithoutReferenceStillLoads()
	{
		core::World source("Legacy", 4, 2);
		auto yaml = serializeWorld(source);
		auto const version = yaml.find("version: 53");
		require(version != std::string::npos, "The current World schema version was missing");
		yaml.replace(version, std::string("version: 53").size(), "version: 9");

		auto loaded = std::make_shared<core::World>("Loading", 1, 1);
		auto reader = core::YamlSerializer::fromString(yaml);
		reader->deserialize();
		core::SerializationWorkData workData;
		require(loaded->deserialize(*reader, workData), "A version-9 World did not load");
		require(!loaded->hasAgentTagRegistryReference()
			&& !loaded->hasAttachedAgentTagRegistry(),
			"An older World invented an Agent tag registry");
	}

	void canonicalFilesShareOneInstanceAndDirectoriesRemainDistinct(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		auto firstDiskRegistry = core::AgentTagRegistry::create();
		auto secondDiskRegistry = core::AgentTagRegistry::create();
		auto const firstRegistryPath = firstDirectory / "shared.tags.yaml";
		auto const secondRegistryPath = secondDirectory / "shared.tags.yaml";
		firstDiskRegistry->saveTo(firstRegistryPath.string());
		secondDiskRegistry->saveTo(secondRegistryPath.string());

		auto first = std::make_shared<core::World>("First", 4, 2);
		auto second = std::make_shared<core::World>("Second", 4, 2);
		auto third = std::make_shared<core::World>("Third", 4, 2);
		auto const firstPath = firstDirectory / "first.world.yaml";
		auto const secondPath = firstDirectory / "second.world.yaml";
		auto const thirdPath = secondDirectory / "third.world.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		third->saveTo(thirdPath.string());

		auto sharedFirst = core::selectAndAttachAgentTagRegistry(
			*first, firstPath, firstRegistryPath);
		auto sharedSecond = core::selectAndAttachAgentTagRegistry(
			*second, secondPath, firstDirectory / "." / "shared.tags.yaml");
		auto distinct = core::selectAndAttachAgentTagRegistry(
			*third, thirdPath, secondRegistryPath);
		require(sharedFirst == sharedSecond,
			"One canonical registry file produced multiple loaded instances");
		require(sharedFirst != distinct,
			"Same-named registries in different directories shared an instance");

		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		first.reset();
		second.reset();
		sharedFirst.reset();
		sharedSecond.reset();

		auto reopenedFirst = core::loadWorldDocument(firstPath);
		auto reopenedSecond = core::loadWorldDocument(secondPath);
		require(reopenedFirst->getAgentTagRegistry()
			== reopenedSecond->getAgentTagRegistry(),
			"Two reopened Worlds did not share their canonical registry instance");
	}

	void duplicateUuidAndInvalidDocumentsAreTransactional(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const firstDirectory = temporary.path / "first";
		auto const secondDirectory = temporary.path / "second";
		std::filesystem::create_directories(firstDirectory);
		std::filesystem::create_directories(secondDirectory);

		auto sourceRegistry = core::AgentTagRegistry::create();
		auto const sourcePath = firstDirectory / "registry.tags.yaml";
		auto const duplicatePath = secondDirectory / "registry.tags.yaml";
		sourceRegistry->saveTo(sourcePath.string());
		std::filesystem::copy_file(sourcePath, duplicatePath);

		auto first = std::make_shared<core::World>("First", 4, 2);
		auto second = std::make_shared<core::World>("Second", 4, 2);
		auto const firstPath = firstDirectory / "first.world.yaml";
		auto const secondPath = secondDirectory / "second.world.yaml";
		first->saveTo(firstPath.string());
		second->saveTo(secondPath.string());
		auto loadedFirst = core::selectAndAttachAgentTagRegistry(
			*first, firstPath, sourcePath);

		bool duplicateRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*second, secondPath, duplicatePath);
		}
		catch (std::exception const& error)
		{
			duplicateRefused = std::string(error.what()).find("already loaded")
				!= std::string::npos;
		}
		require(duplicateRefused && !second->hasAgentTagRegistryReference()
			&& !second->isModified()
			&& first->getAgentTagRegistry() == loadedFirst,
			"A duplicate registry UUID disturbed loaded document state");

		// A refusal must not cache the duplicate path. Replacing that path with a
		// genuinely independent registry makes it immediately selectable.
		auto independent = core::AgentTagRegistry::create();
		independent->saveTo(duplicatePath.string());
		auto loadedSecond = core::selectAndAttachAgentTagRegistry(
			*second, secondPath, duplicatePath);
		require(loadedSecond != loadedFirst,
			"A duplicate-UUID refusal left a stale registry loaded at its path");

		auto malformed = std::make_shared<core::World>("Malformed", 4, 2);
		auto const malformedWorldPath = firstDirectory / "malformed.world.yaml";
		auto const malformedRegistryPath = firstDirectory / "malformed.tags.yaml";
		malformed->saveTo(malformedWorldPath.string());
		writeText(malformedRegistryPath, "agentTagRegistry: [not valid");
		bool malformedRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedWorldPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			malformedRefused = std::string(error.what()).find("Could not load")
				!= std::string::npos;
		}
		require(malformedRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"A malformed registry disturbed the World");

		auto unsupportedRegistry = core::AgentTagRegistry::create();
		unsupportedRegistry->saveTo(malformedRegistryPath.string());
		auto unsupportedYaml = readText(malformedRegistryPath);
		auto const version = unsupportedYaml.find("version: 14");
		require(version != std::string::npos,
			"The registry fixture did not contain the current schema version");
		unsupportedYaml.replace(version, std::string("version: 14").size(), "version: 15");
		writeText(malformedRegistryPath, unsupportedYaml);
		bool unsupportedRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedWorldPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			unsupportedRefused = std::string(error.what()).find("Unsupported")
				!= std::string::npos;
		}
		require(unsupportedRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"An unsupported registry schema disturbed the World");

		std::filesystem::remove(malformedRegistryPath);
		bool missingRefused{ false };
		try
		{
			(void)core::selectAndAttachAgentTagRegistry(
				*malformed, malformedWorldPath, malformedRegistryPath);
		}
		catch (std::exception const& error)
		{
			missingRefused = std::string(error.what()).find("missing")
				!= std::string::npos;
		}
		require(missingRefused && !malformed->hasAgentTagRegistryReference()
			&& !malformed->isModified(),
			"A missing registry disturbed the World");
	}

	void refusedWorldLoadKeepsCurrentStateAndUnloadsCandidateRegistry(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		auto const sourceDirectory = temporary.path / "source";
		auto const destinationDirectory = temporary.path / "destination";
		std::filesystem::create_directories(sourceDirectory);
		std::filesystem::create_directories(destinationDirectory);
		auto const worldPath = sourceDirectory / "referencing.world.yaml";
		auto const registryPath = sourceDirectory / "referencing.tags.yaml";

		{
			auto persisted = std::make_shared<core::World>("Persisted", 4, 2);
			persisted->saveTo(worldPath.string());
			(void)core::createAndAttachAgentTagRegistry(*persisted, worldPath);
			persisted->saveTo(worldPath.string());
		}

		auto replacement = core::AgentTagRegistry::create();
		replacement->saveTo(registryPath.string());
		auto const replacementUuid = replacement->getUuid();
		auto const copiedReplacement = destinationDirectory / "replacement.tags.yaml";
		std::filesystem::copy_file(registryPath, copiedReplacement);
		replacement.reset();

		auto current = std::make_shared<core::World>("Current", 7, 3);
		auto const* currentIdentity = current.get();
		bool mismatchRefused{ false };
		try
		{
			current = core::loadWorldDocument(worldPath);
		}
		catch (std::exception const& error)
		{
			mismatchRefused = std::string(error.what()).find("UUID mismatch")
				!= std::string::npos;
		}
		require(mismatchRefused && current.get() == currentIdentity
			&& current->getName() == "Current" && current->getCellsWide() == 7,
			"A refused World load replaced or changed the current World");

		// The replacement was parsed solely for the failed load above. It must no
		// longer count as loaded, so the same UUID at this independent path is valid.
		auto destination = std::make_shared<core::World>("Destination", 4, 2);
		auto const destinationPath = destinationDirectory / "destination.world.yaml";
		destination->saveTo(destinationPath.string());
		auto selected = core::selectAndAttachAgentTagRegistry(
			*destination, destinationPath, copiedReplacement);
		require(selected->getUuid() == replacementUuid,
			"A registry loaded only for a failed World load remained referenced");
	}

	void failedAtomicCreationLeavesNoReferenceOrFile(smoke::Context const& context)
	{
		TemporaryDirectory temporary{ context };
		core::World world("Atomic", 4, 2);
		auto const worldPath = temporary.path / "atomic.world.yaml";
		world.saveTo(worldPath.string());
		auto const registryPath = core::defaultAgentTagRegistryPath(worldPath);

		core::setTransactionalWriteFailureAfterBytesForTesting(1);
		bool refused{ false };
		try
		{
			(void)core::createAndAttachAgentTagRegistry(world, worldPath);
		}
		catch (std::exception const&)
		{
			refused = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(refused, "The injected registry write failure was not reported");
		require(!std::filesystem::exists(registryPath),
			"A failed registry creation left a partial destination file");
		require(!world.hasAgentTagRegistryReference(),
			"A failed registry creation attached a nonexistent registry");
	}

}

void tag_smoke::registerRegistry(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "olderWorldWithoutReferenceStillLoads",
		[](smoke::Context const&)
		{
			olderWorldWithoutReferenceStillLoads();
		} });
	checks.push_back({ "canonicalFilesShareOneInstanceAndDirectoriesRemainDistinct",
		[](smoke::Context const& context)
		{
			canonicalFilesShareOneInstanceAndDirectoriesRemainDistinct(context);
		} });
	checks.push_back({ "duplicateUuidAndInvalidDocumentsAreTransactional",
		[](smoke::Context const& context)
		{
			duplicateUuidAndInvalidDocumentsAreTransactional(context);
		} });
	checks.push_back({ "refusedWorldLoadKeepsCurrentStateAndUnloadsCandidateRegistry",
		[](smoke::Context const& context)
		{
			refusedWorldLoadKeepsCurrentStateAndUnloadsCandidateRegistry(context);
		} });
	checks.push_back({ "failedAtomicCreationLeavesNoReferenceOrFile",
		[](smoke::Context const& context)
		{
			failedAtomicCreationLeavesNoReferenceOrFile(context);
		} });
}
