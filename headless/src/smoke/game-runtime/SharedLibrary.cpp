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
			// Probed on a member that is defined out of line in the shared
			// library. It used to be `getFixedTimestep`, and #983 made that
			// `constexpr`: a `constexpr` function is implicitly `inline`, so
			// taking its address here emits the caller's own copy and dladdr
			// reports this executable rather than the runtime image - which is
			// exactly the header-only stand-in this check exists to rule out,
			// arriving as a false positive. `isValidTransportStopShortName` is
			// still a plain exported member, so the attribution it reports is
			// the runtime's.
			Dl_info info{};
			require(dladdr(reinterpret_cast<void*>(&game::WorldRuntime::isValidTransportStopShortName), &info) != 0
				&& info.dli_fname != nullptr,
				"dladdr could not locate WorldRuntime::isValidTransportStopShortName");
			std::string const location = info.dli_fname;
			require(location.find("pf-game-runtime") != std::string::npos,
				"WorldRuntime resolved outside the shared runtime library: " + location);
		} });
#else
	(void)checks;
#endif
}
