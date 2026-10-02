#include "Smoke.h"

#include <fstream>
#include <iostream>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace
{
	void pass(smoke::Context const& context)
	{
		std::ifstream input(context.fixture("src/headless/smoke/tests/fixture.txt"));
		std::string line;
		std::getline(input, line);
		smoke::require(line == "smoke fixture", "Required fixture content differs");
	}

	void fail(smoke::Context const&)
	{
		smoke::require(false, "deliberate\nfailure");
	}

	void skip(smoke::Context const&)
	{
		throw smoke::OptionalCapabilityUnavailable("optional external capability unavailable (synthetic)");
	}

	void unknown(smoke::Context const&)
	{
		throw 42;
	}

	void missing(smoke::Context const& context)
	{
		(void)context.fixture("src/headless/smoke/tests/absent-required-fixture");
	}

	void paths(smoke::Context const& context)
	{
		smoke::require(std::filesystem::is_directory(context.temporaryRoot()), "Temporary root absent");
		std::filesystem::path nestedRoot;
		try
		{
			smoke::Context nested;
			nestedRoot = nested.temporaryRoot();
			smoke::require(nestedRoot != context.temporaryRoot(), "Temporary roots collide");
			std::ofstream(nestedRoot / "artifact.txt") << "nested";
			throw smoke::Failure("unwind");
		}
		catch (smoke::Failure const& error)
		{
			smoke::require(std::string(error.what()) == "unwind", error.what());
		}
		smoke::require(!std::filesystem::exists(nestedRoot), "Unwound temporary root leaked");
		std::ofstream(context.temporaryRoot() / "artifact.txt") << "owned";
		std::cout << "ROOT " << context.temporaryRoot().string() << '\n';
	}

#if defined(_WIN32)
	void leakHandle(smoke::Context const& context)
	{
		auto const path = context.temporaryRoot() / "open-handle.txt";
		// Intentionally retained until process exit: Windows must refuse cleanup.
		auto const handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		smoke::require(handle != INVALID_HANDLE_VALUE, "Cannot create cleanup-failure fixture");
		std::cout << "ROOT " << context.temporaryRoot().string() << '\n';
	}
#endif

	void failedPaths(smoke::Context const& context)
	{
		paths(context);
		fail(context);
	}

	constexpr smoke::Check checks[] = {
		{ "pass", pass },
		{ "fail", fail },
		{ "skip", skip },
		{ "unknown-exception", unknown },
		{ "missing-fixture", missing },
		{ "after", pass },
		{ "paths", paths },
		{ "failed-paths", failedPaths },
	};
}

int main(int argc, char** argv)
{
#if defined(_WIN32)
	if (argc == 2 && std::string_view(argv[1]) == "--cleanup-failure")
	{
		constexpr smoke::Check cleanupChecks[] = { { "leaked-handle", leakHandle } };
		return smoke::main("harness", cleanupChecks, 1, argv);
	}
#endif
	return smoke::main("harness", checks, argc, argv);
}
