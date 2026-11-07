#include "Infrastructure.h"

#include "core/SerializationException.h"
#include "core/WorldDocument.h"

#include <filesystem>
#include <string>

namespace persistence
{
	using smoke::require;

	void worldDocumentPathsUseExactSuffixes(smoke::Context const& context)
	{
		namespace filesystem = std::filesystem;
		auto const directory = context.temporaryRoot() / "pf-world-document-filename-smoke";
		filesystem::create_directories(directory);

		for (auto const* refused : { ".world", ".world.yaml", "filename.yaml",
			"filename.bin", "filename.WORLD", "filename.world.YAML",
			"filename.world.yaml.bak", "filename.world.bin" })
		{
			require(!core::isWorldDocumentPath(directory / refused),
				"An unsupported World document name was accepted");
		}
		require(core::isWorldDocumentPath(directory / "filename.world")
			&& core::isWorldDocumentPath(directory / "filename.world.yaml"),
			"A supported World document suffix was refused");
		require(core::WorldDocumentFilenameSuffix
			== core::BinaryWorldDocumentFilenameSuffix,
			"New World documents do not default to the binary suffix");
		require(core::worldDocumentSavePath(directory / "untitled")
			== directory / "untitled.world"
			&& core::worldDocumentSavePath(directory / "binary.world")
			== directory / "binary.world"
			&& core::worldDocumentSavePath(directory / "yaml.world.yaml")
			== directory / "yaml.world.yaml",
			"Save As did not default to binary or retain a supported suffix");
		for (auto const* refused : { "filename.yaml", "filename.bin", ".world",
			"filename.WORLD", "filename.world.YAML" })
		{
			bool rejected{ false };
			try { (void)core::worldDocumentSavePath(directory / refused); }
			catch (core::SerializationException const& exception)
			{
				rejected = std::string(exception.what()).find("World document")
					!= std::string::npos;
			}
			require(rejected, "Save As accepted an unsupported suffix or gave an unclear diagnostic");
		}
		require(core::worldDocumentBasePath(directory / "filename.world")
			== directory / "filename"
			&& core::worldDocumentBasePath(directory / "filename.world.yaml")
			== directory / "filename",
			"World document formats produced different base paths");

	}
}
