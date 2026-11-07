#include "Checks.h"

#include <string>

#include "game/WorldRuntime.h"

namespace
{
	using smoke::require;
}

// The point of the module is that the game reaches WorldRuntime through a real
// shared library, not through a header-only stand-in. On Linux dladdr reports
// which image a resolved symbol actually lives in, which turns that from a
// linking assumption into an observed fact. Windows module attribution is left
// to the separate Windows validation track rather than being asserted here.
#if defined(__linux__)
#include <dlfcn.h>
#endif

void registerGameRuntimeSharedLibrary(std::vector<smoke::Check>& checks)
{
#if defined(__linux__)
	checks.push_back({ "worldRuntimeCodeLivesInTheSharedLibrary", [](smoke::Context const&)
		{
			Dl_info info{};
			require(dladdr(reinterpret_cast<void*>(&game::WorldRuntime::getFixedTimestep), &info) != 0
				&& info.dli_fname != nullptr,
				"dladdr could not locate WorldRuntime::getFixedTimestep");
			std::string const location = info.dli_fname;
			require(location.find("pf-game-runtime") != std::string::npos,
				"WorldRuntime resolved outside the shared runtime library: " + location);
		} });
#else
	(void)checks;
#endif
}
