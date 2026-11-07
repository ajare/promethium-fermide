#include "Formats.h"
#include "Infrastructure.h"

#include "WriteFailure.h"
#include "core/SerializationException.h"
#include "core/YamlSerializer.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace persistence
{
	using smoke::require;

	// #62: a late save write failure must report failure and leave the previous
	// save file intact, with no temporary file left behind.
	void lateWriteFailurePreservesThePreviousSaveFile(smoke::Context const& context)
	{
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-save-transaction-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";

		auto const originalContents = std::string("original save contents\n");
		{
			std::ofstream original(destination, std::ios::binary);
			original << originalContents;
		}

		auto makeWriter = [&destination]()
		{
			auto writer = core::YamlSerializer::toFile(destination.string());
			writer->beginMap("");
			writer->writeString("payload", std::string(256 * 1024, 'x'));
			writer->endMap();
			return writer;
		};
		auto readFile = [&destination]()
		{
			std::ifstream in(destination, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		};
		auto countRegularFiles = [&directory]()
		{
			int count = 0;
			for (auto const& entry : filesystem::directory_iterator(directory))
			{
				if (entry.is_regular_file()) ++count;
			}
			return count;
		};

		core::setTransactionalWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			makeWriter()->serialize();
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);

		require(reportedFailure, "injected late write failure did not report a save error");
		require(readFile() == originalContents, "failed save destroyed the previous save file");
		require(countRegularFiles() == 1, "failed save left a temporary file behind");

		// A successful save installs the new contents and also leaves no
		// temporary file behind.
		makeWriter()->serialize();
		require(readFile().find(std::string(1024, 'x')) != std::string::npos,
			"successful save did not install the new contents");
		require(countRegularFiles() == 1, "successful save left a temporary file behind");
	}

	// #190: a save must never write through a predictable, pre-existing
	// temporary path. Whatever already sits at the legacy name is an unrelated
	// file and must survive both failed and successful saves.
	void saveNeverTouchesAPredictableTemporaryPath(smoke::Context const& context)
	{
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		filesystem::path const directory
			= context.temporaryRoot() / "pf-save-predictable-temp-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";
		filesystem::path const legacyTemp = directory / "world.world.yaml.saving.tmp";
		auto const originalContents = std::string("original save contents\n");
		auto const bystanderContents = std::string("unrelated bystander contents\n");
		{
			std::ofstream(destination, std::ios::binary) << originalContents;
			std::ofstream(legacyTemp, std::ios::binary) << bystanderContents;
		}
		auto readFile = [](filesystem::path const& path)
		{
			std::ifstream in(path, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		};
		auto makeWriter = [&destination]()
		{
			auto writer = core::YamlSerializer::toFile(destination.string());
			writer->beginMap("");
			writer->writeString("payload", std::string(256 * 1024, 'x'));
			writer->endMap();
			return writer;
		};

		core::setTransactionalWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			makeWriter()->serialize();
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(reportedFailure, "injected late write failure did not report a save error");
		require(readFile(destination) == originalContents,
			"a failed save followed a predictable temporary path and destroyed the destination");
		require(filesystem::exists(legacyTemp) && readFile(legacyTemp) == bystanderContents,
			"a failed save truncated or removed a file at the legacy temporary path");

		makeWriter()->serialize();
		require(readFile(destination).find(std::string(1024, 'x')) != std::string::npos,
			"successful save did not install the new contents");
		require(filesystem::exists(legacyTemp) && readFile(legacyTemp) == bystanderContents,
			"a successful save truncated or removed a file at the legacy temporary path");
	}

	// #92: a save addressed at a symbolic link must update the link's target and
	// leave the link itself in place, as the pre-#62 std::ofstream save did.
	void saveThroughSymlinkUpdatesItsTarget(smoke::Context const& context)
	{
#if defined(_WIN32)
		(void)context;
		throw smoke::OptionalCapabilityUnavailable("POSIX symlink/permission semantics unavailable");
#else
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-save-symlink-smoke";
		filesystem::create_directories(directory);
		filesystem::path const target = directory / "target.yaml";
		filesystem::path const link = directory / "save.yaml";
		{
			std::ofstream original(target, std::ios::binary);
			original << "old-target\n";
		}
		// A relative link, as a user would typically create one.
		filesystem::create_symlink(filesystem::path("target.yaml"), link);

		auto writer = core::YamlSerializer::toFile(link.string());
		writer->beginMap("");
		writer->writeString("payload", std::string("via symlink"));
		writer->endMap();
		writer->serialize();

		require(filesystem::is_symlink(filesystem::symlink_status(link)),
			"save through a symlink destroyed the symlink");
		require(filesystem::read_symlink(link) == filesystem::path("target.yaml"),
			"save through a symlink repointed the symlink");
		std::ifstream in(target, std::ios::binary);
		std::string const contents((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		require(contents.find("via symlink") != std::string::npos,
			"save through a symlink did not update its target");
		bool tempFileLeftBehind = false;
		for (auto const& entry : filesystem::directory_iterator(directory))
		{
			if (entry.path().filename().string().find(".saving") != std::string::npos)
				tempFileLeftBehind = true;
		}
		require(!tempFileLeftBehind, "save through a symlink left a temporary file behind");
#endif
	}

	// #190: the legacy predictable temporary path could be a symbolic link to
	// the destination. A save must not resolve, truncate, or replace that link;
	// it writes to its own uniquely named temporary file instead.
	void saveNeverFollowsASymlinkedTemporaryPath(smoke::Context const& context)
	{
#if defined(_WIN32)
		(void)context;
		throw smoke::OptionalCapabilityUnavailable("POSIX symlink/permission semantics unavailable");
#else
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-save-temp-symlink-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";
		filesystem::path const legacyTemp = directory / "world.world.yaml.saving.tmp";
		auto const originalContents = std::string("original save contents\n");
		{
			std::ofstream original(destination, std::ios::binary);
			original << originalContents;
		}
		// A relative link aimed at the destination, as the audit reproduced it.
		filesystem::create_symlink(filesystem::path("world.world.yaml"), legacyTemp);

		auto writer = core::YamlSerializer::toFile(destination.string());
		writer->beginMap("");
		writer->writeString("payload", std::string(256 * 1024, 'x'));
		writer->endMap();
		core::setTransactionalWriteFailureAfterBytesForTesting(4096);
		bool reportedFailure = false;
		try
		{
			writer->serialize();
		}
		catch (core::SerializationException const&)
		{
			reportedFailure = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);

		require(reportedFailure, "injected late write failure did not report a save error");
		require(filesystem::is_symlink(filesystem::symlink_status(legacyTemp)),
			"a save replaced a symbolic link at the legacy temporary path");
		std::ifstream in(destination, std::ios::binary);
		std::string const contents((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		require(contents == originalContents,
			"a failed save followed a symlinked temporary path and destroyed the destination");
#endif
	}

	// #92: replacing an existing save file must keep its permission mode,
	// including a restrictive 0600, instead of resetting it to the umask mode.
	void savePreservesExistingFilePermissions(smoke::Context const& context)
	{
#if defined(_WIN32)
		(void)context;
		throw smoke::OptionalCapabilityUnavailable("POSIX symlink/permission semantics unavailable");
#else
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-save-permissions-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";
		{
			std::ofstream original(destination, std::ios::binary);
			original << "old\n";
		}
		auto const privateMode = filesystem::perms::owner_read | filesystem::perms::owner_write;
		filesystem::permissions(destination, privateMode, filesystem::perm_options::replace);

		auto writer = core::YamlSerializer::toFile(destination.string());
		writer->beginMap("");
		writer->writeString("payload", std::string("permissions"));
		writer->endMap();
		writer->serialize();

		require(filesystem::status(destination).permissions() == privateMode,
			"save reset a restrictive permission mode to the umask mode");
		auto reader = core::YamlSerializer::fromFile(destination.string());
		reader->deserialize();
		require(reader->readString("payload") == "permissions",
			"permission-preserving save did not install the new contents");
#endif
	}

	// #190: concurrent saves must not share a temporary file. Each save either
	// commits a complete document or reports failure, and no save may truncate
	// another save's temporary file.
	void concurrentSavesCommitOnlyCompleteDocuments(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		filesystem::path const directory = context.temporaryRoot() / "pf-save-concurrent-smoke";
		filesystem::create_directories(directory);
		filesystem::path const destination = directory / "world.world.yaml";

		constexpr int threadCount = 4;
		constexpr int savesPerThread = 8;
		constexpr size_t paddingSize = 64 * 1024;
		std::vector<std::thread> threads;
		std::vector<int> failures(static_cast<size_t>(threadCount), 0);
		for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex)
		{
			threads.emplace_back([&, threadIndex]()
			{
				for (int saveIndex = 0; saveIndex < savesPerThread; ++saveIndex)
				{
					try
					{
						auto writer = core::YamlSerializer::toFile(destination.string());
						writer->beginMap("");
						writer->writeString("writer",
							std::to_string(threadIndex) + "-" + std::to_string(saveIndex));
						writer->writeString("padding", std::string(paddingSize, 'p'));
						writer->endMap();
						writer->serialize();
					}
					catch (core::SerializationException const&)
					{
						++failures[static_cast<size_t>(threadIndex)];
					}
				}
			});
		}
		for (auto& thread : threads)
		{
			thread.join();
		}

		// The committed document must be one writer's complete save, not a blend
		// of two saves that shared a temporary file.
		auto reader = core::YamlSerializer::fromFile(destination.string());
		reader->deserialize();
		require(reader->readString("padding") == std::string(paddingSize, 'p'),
			"a concurrent save committed a truncated or interleaved document");
		std::string const writer = reader->readString("writer");
		bool matchesAWriter = false;
		for (int threadIndex = 0; threadIndex < threadCount && !matchesAWriter; ++threadIndex)
		{
			for (int saveIndex = 0; saveIndex < savesPerThread; ++saveIndex)
			{
				if (writer == std::to_string(threadIndex) + "-" + std::to_string(saveIndex))
				{
					matchesAWriter = true;
					break;
				}
			}
		}
		require(matchesAWriter, "a concurrent save committed an unrecognised document");
		int totalFailures = 0;
		for (int const failureCount : failures)
		{
			totalFailures += failureCount;
		}
		require(totalFailures < threadCount * savesPerThread, "every concurrent save failed");
		bool tempFileLeftBehind = false;
		for (auto const& entry : filesystem::directory_iterator(directory))
		{
			if (entry.path().filename().string().find(".saving") != std::string::npos)
			{
				tempFileLeftBehind = true;
			}
		}
		require(!tempFileLeftBehind, "concurrent saves left a temporary file behind");
	}

	void transactionalWriterPreservesOpaqueBytes(smoke::Context const& context)
	{
		ResetWriteFailure resetWriteFailure;
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "pf-transactional-byte-writer-smoke";
		filesystem::create_directories(directory);

		auto const destination = directory / "opaque.bin";
		std::string const bytes{ '\x01', '\0', '\x02', '\0', static_cast<char>(0xff) };
		core::writeFileTransactionally(destination, bytes);

		auto readSavedBytes = [&destination]()
		{
			std::ifstream input(destination, std::ios::binary);
			return std::string((std::istreambuf_iterator<char>(input)),
				std::istreambuf_iterator<char>());
		};
		require(readSavedBytes() == bytes,
			"transactional byte writer truncated content at an embedded NUL byte");

		core::setTransactionalWriteFailureAfterBytesForTesting(2);
		bool failed = false;
		try
		{
			core::writeFileTransactionally(destination, std::string(1024, 'x'));
		}
		catch (core::SerializationException const&)
		{
			failed = true;
		}
		core::setTransactionalWriteFailureAfterBytesForTesting(0);
		require(failed, "shared transactional writer failure seam did not fail the write");
		require(readSavedBytes() == bytes,
			"failed opaque byte write changed the existing destination");
		require(std::distance(filesystem::directory_iterator(directory),
			filesystem::directory_iterator()) == 1,
			"failed opaque byte write left a temporary file behind");
	}
}
