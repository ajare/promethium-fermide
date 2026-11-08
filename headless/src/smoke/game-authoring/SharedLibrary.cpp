#include "Checks.h"

#include <cstring>
#include <string>

#include "game/WorldAuthoring.h"
#include "game/WorldRuntime.h"

// The point of the module is that the editor reaches WorldAuthoring through a
// real shared library, not through a header-only stand-in. On Linux dladdr
// reports which image a resolved symbol actually lives in, which turns that
// from a linking assumption into an observed fact - and shows the authoring
// library is a separate image from the runtime it wraps, rather than the same
// code in a different wrapper. Windows module attribution is left to the
// separate Windows validation track rather than being asserted here.
#if defined(__linux__)
#include <dlfcn.h>
#endif

namespace
{
#if defined(__linux__)
	using smoke::require;

	// dladdr wants a plain code address. On the Itanium ABI this platform
	// uses, a pointer-to-member-function to a non-virtual member is the entry
	// address plus an adjustment, so the address half is copied out.
	void* entryOf(decltype(&game::WorldAuthoring::world) method)
	{
		static_assert(sizeof(method) == 2 * sizeof(void*),
			"the pointer-to-member layout this check relies on has changed");
		void* address = nullptr;
		std::memcpy(&address, &method, sizeof(address));
		return address;
	}

	std::string imageOf(void* address)
	{
		Dl_info info{};
		require(dladdr(address, &info) != 0 && info.dli_fname != nullptr,
			"dladdr could not locate the address under test");
		return std::string(info.dli_fname);
	}
#endif
}

void registerGameAuthoringSharedLibrary(std::vector<smoke::Check>& checks)
{
#if defined(__linux__)
	checks.push_back({ "worldAuthoringCodeLivesInTheSharedLibrary", [](smoke::Context const&)
		{
			std::string const location = imageOf(entryOf(&game::WorldAuthoring::world));
			require(location.find("pf-game-authoring") != std::string::npos,
				"WorldAuthoring resolved outside the shared authoring library: " + location);
		} });

	checks.push_back({ "authoringLibraryIsSeparateFromTheRuntimeItWraps", [](smoke::Context const&)
		{
			std::string const authoringImage = imageOf(entryOf(&game::WorldAuthoring::world));
			// Same probe as the runtime module's: a member that stays out of
			// line in the shared runtime library. `getFixedTimestep` cannot be
			// used here since #983 made it `constexpr` - implicitly `inline`,
			// so its address is this translation unit's own copy and the
			// "runtime" image would have resolved to this executable instead.
			std::string const runtimeImage = imageOf(
				reinterpret_cast<void*>(&game::WorldRuntime::isValidTransportStopShortName));
			require(runtimeImage.find("pf-game-runtime") != std::string::npos,
				"WorldRuntime resolved outside the shared runtime library: " + runtimeImage);
			require(authoringImage != runtimeImage,
				"WorldAuthoring and WorldRuntime resolved into the same image: " + authoringImage);
		} });
#else
	(void)checks;
#endif
}
