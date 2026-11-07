#pragma once

#include "TemporaryDirectory.h"

#include <fstream>
#include <stdexcept>
#include <string>

namespace behaviour_smoke
{
	// Runtime package fixtures only; no editor state or production runtime objects.
	inline void writeRuntimeText(std::filesystem::path const& path,
		std::string const& source)
	{
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		output.write(source.data(), static_cast<std::streamsize>(source.size()));
		if (!output) throw std::runtime_error("Could not write Lua preflight fixture");
	}
}
