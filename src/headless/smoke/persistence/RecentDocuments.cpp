#include "Infrastructure.h"

#include "RecentFiles.h"

#include <filesystem>
#include <fstream>

namespace persistence
{
	using smoke::require;

	void recentFilesPersistAcrossStartup(smoke::Context const& context)
	{
		auto directory = context.temporaryRoot() / "prometheum-fermide-recent-files-smoke";
		std::filesystem::create_directories(directory);
		auto file = directory / "recent-files.txt";
		RecentFiles first(3);
		first.initialize(file);
		require(std::filesystem::exists(file), "Recent-file storage was not created on first startup");
		first.add((directory / "alpha.world.yaml").string());
		first.add((directory / "beta.world").string());
		first.add((directory / "alpha.world.yaml").string());
		first.add((directory / "beta.world").string());
		RecentFiles restarted(3);
		restarted.initialize(file);
		require(restarted.entries().size() == 2,
			"Recent files were not restored after startup");
		require(restarted.entries()[0] == (directory / "beta.world").string()
			&& restarted.entries()[1] == (directory / "alpha.world.yaml").string(),
			"Recent binary and YAML Worlds did not retain order or deduplication");
	}

	void missingRecentFilesCanBeRemovedPersistently(smoke::Context const& context)
	{
		auto directory = context.temporaryRoot()
			/ "prometheum-fermide-missing-recent-file-smoke";
		std::filesystem::create_directories(directory);
		auto const storage = directory / "recent-files.txt";
		auto const missing = (directory / "moved.world").string();

		RecentFiles recent(3);
		recent.initialize(storage);
		recent.add(missing);
		require(recent.removeUnavailable(missing),
			"A selected missing World was not removed from recent files");
		require(recent.empty(),
			"A removed missing World remained in the in-memory recent files");

		auto const available = directory / "available.world";
		std::ofstream(available, std::ios::binary) << "binary";
		recent.add(available.string());
		require(!recent.removeUnavailable(available.string())
			&& recent.entries().size() == 1,
			"An available recent World was removed");
		std::filesystem::remove(available);
		require(recent.removeUnavailable(available.string()),
			"A recently removed World was not pruned after becoming unavailable");

		RecentFiles restarted(3);
		restarted.initialize(storage);
		require(restarted.empty(),
			"A removed missing World returned after recent files were reloaded");
	}
}
