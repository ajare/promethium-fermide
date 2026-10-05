#pragma once

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace smoke
{
	// Compose native Lua fixture variants without evaluating Lua or exposing VM APIs.
	// Fixtures end in `return catalogue`; edits run before the validated return.
	inline std::string catalogueSource(std::filesystem::path const& path)
	{
		std::ifstream input(path);
		if (!input) throw std::runtime_error("Cannot read catalogue fixture: " + path.string());
		std::string source{std::istreambuf_iterator<char>(input), {}};
		auto at = source.rfind("return catalogue");
		if (at == std::string::npos) throw std::runtime_error("Missing catalogue return: " + path.string());
		source.erase(at);
		return source;
	}

	inline void writeCatalogue(std::filesystem::path const& path, std::string const& source)
	{
		std::ofstream output(path);
		output << source << "\nreturn catalogue\n";
		if (!output) throw std::runtime_error("Cannot write catalogue fixture: " + path.string());
	}
}
