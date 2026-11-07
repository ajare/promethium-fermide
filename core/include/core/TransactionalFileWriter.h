#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace core
{
	// Commits opaque bytes to a file without exposing a partially written
	// destination. The temporary file is exclusively created beside the resolved
	// destination, then installed only after write, flush, and close succeed.
	void writeFileTransactionally(std::filesystem::path const& destination,
		std::string_view bytes);

	// Regression-test seam: forces transactional writes to fail after reaching
	// the given byte count. Zero disables failure injection.
	void setTransactionalWriteFailureAfterBytesForTesting(size_t bytes);
}
