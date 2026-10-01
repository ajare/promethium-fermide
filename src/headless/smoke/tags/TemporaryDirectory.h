#pragma once

#include "Smoke.h"

namespace tag_smoke
{
	// Every fixture lives below the invocation's atomically reserved Context root.
	// Keep separate directories even when a check creates two fixtures at once.
	// Context cleans everything on both success and exception.
	struct TemporaryDirectory
	{
		std::filesystem::path path;

		explicit TemporaryDirectory(smoke::Context const& context,
			std::string const& purpose = "fixture")
		{
			for (unsigned index = 0;; ++index)
			{
				path = context.temporaryRoot() / (purpose + "-" + std::to_string(index));
				if (std::filesystem::create_directory(path)) return;
			}
		}
	};
}
