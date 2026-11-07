#include "core/TransactionalFileWriter.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

#include "core/SerializationException.h"

#if defined(_WIN32)
#	include <process.h>
#else
#	include <unistd.h>
#endif

namespace core
{
	namespace
	{
		std::atomic<size_t> gWriteFailureAfterBytes{ 0 };

		void removeTemporaryFile(std::filesystem::path const& path)
		{
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
		}

		class TemporaryFileCleanup
		{
			std::filesystem::path mPath;
			bool mActive{ true };

		public:
			explicit TemporaryFileCleanup(std::filesystem::path path)
				: mPath(std::move(path))
			{
			}

			~TemporaryFileCleanup()
			{
				if (mActive) removeTemporaryFile(mPath);
			}

			void release() { mActive = false; }
		};

		std::uint64_t currentProcessId()
		{
#if defined(_WIN32)
			return static_cast<std::uint64_t>(::_getpid());
#else
			return static_cast<std::uint64_t>(::getpid());
#endif
		}

		std::filesystem::path temporaryPath(std::filesystem::path const& destination)
		{
			auto directory = destination.parent_path();
			if (directory.empty()) directory = ".";
			static std::atomic<std::uint64_t> sequence{ 0 };
			return directory / std::format("{}.saving.{}.{}.tmp",
				destination.filename().string(), currentProcessId(),
				sequence.fetch_add(1, std::memory_order_relaxed));
		}

		std::FILE* openExclusive(std::filesystem::path const& path, int& errorCode)
		{
#if defined(_WIN32)
			std::FILE* file = nullptr;
			errorCode = ::_wfopen_s(&file, path.c_str(), L"wbx");
			return file;
#else
			std::FILE* file = std::fopen(path.c_str(), "wbx");
			errorCode = file == nullptr ? errno : 0;
			return file;
#endif
		}

		[[noreturn]] void fail(std::string const& operation,
			std::filesystem::path const& destination)
		{
			throw SerializationException(std::format("Could not {} file: {}",
				operation, destination.string()));
		}
	}

	void setTransactionalWriteFailureAfterBytesForTesting(size_t bytes)
	{
		gWriteFailureAfterBytes.store(bytes, std::memory_order_relaxed);
	}

	void writeFileTransactionally(std::filesystem::path const& requestedDestination,
		std::string_view bytes)
	{
		// Preserve the established behaviour of saves addressed through a symbolic
		// link: replace its target rather than the link itself.
		std::error_code error;
		auto destination = requestedDestination;
		auto const resolvedDestination = std::filesystem::weakly_canonical(destination, error);
		if (!error) destination = resolvedDestination;

		auto const existingStatus = std::filesystem::status(destination, error);
		bool const preservePermissions = !error
			&& std::filesystem::is_regular_file(existingStatus);

		std::FILE* output = nullptr;
		std::filesystem::path temporary;
		int openError = 0;
		constexpr int maxOpenAttempts = 64;
		for (int attempt = 0; attempt < maxOpenAttempts; ++attempt)
		{
			temporary = temporaryPath(destination);
			output = openExclusive(temporary, openError);
			if (output != nullptr || openError != EEXIST) break;
		}
		if (output == nullptr) fail("open temporary", requestedDestination);
		TemporaryFileCleanup cleanup(temporary);

		auto cleanupAndFail = [&](std::string const& operation) -> void
		{
			fail(operation, requestedDestination);
		};

		constexpr size_t chunkSize = 64 * 1024;
		size_t written = 0;
		while (written < bytes.size())
		{
			auto chunk = std::min(chunkSize, bytes.size() - written);
			auto const failureAfter
				= gWriteFailureAfterBytes.load(std::memory_order_relaxed);
			if (failureAfter != 0 && written < failureAfter)
			{
				chunk = std::min(chunk, failureAfter - written);
			}
			if (std::fwrite(bytes.data() + written, 1, chunk, output) != chunk)
			{
				std::fclose(output);
				cleanupAndFail("write");
			}
			written += chunk;
			if (failureAfter != 0 && written >= failureAfter)
			{
				std::fclose(output);
				cleanupAndFail("write");
			}
		}

		if (std::fflush(output) != 0)
		{
			std::fclose(output);
			cleanupAndFail("flush");
		}
		if (std::fclose(output) != 0) cleanupAndFail("close");

		if (preservePermissions)
		{
			std::filesystem::permissions(temporary, existingStatus.permissions(),
				std::filesystem::perm_options::replace, error);
			if (error) cleanupAndFail("preserve permissions of");
		}

		std::filesystem::rename(temporary, destination, error);
		if (error)
		{
			throw SerializationException(std::format("Could not replace file: {} ({})",
				requestedDestination.string(), error.message()));
		}
		cleanup.release();
	}
}
