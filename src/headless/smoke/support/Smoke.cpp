#include "Smoke.h"

#include <iostream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#include <cstdlib>
#include <crtdbg.h>
#endif
#endif

namespace smoke
{
	void require(bool condition, std::string_view diagnostic)
	{
		if (!condition) throw Failure(std::string(diagnostic));
	}

	void setupProcess()
	{
#if defined(_WIN32)
		SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER)
		_set_error_mode(_OUT_TO_STDERR);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
		_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
		_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
		_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
#endif
	}

	Context::Context()
	{
		auto const parent = std::filesystem::absolute(std::filesystem::temp_directory_path());
		std::random_device random;
		for (unsigned attempt = 0; attempt < 100; ++attempt)
		{
			auto candidate = parent / ("pf-smoke-" + std::to_string(random()) + "-" + std::to_string(random()));
			// Atomic creation, not a check-then-create race, establishes ownership.
			if (std::filesystem::create_directory(candidate))
			{
				temporaryRoot_ = std::move(candidate);
				return;
			}
		}
		throw Failure("Cannot create a unique smoke temporary root");
	}

	Context::~Context()
	{
		std::error_code ignored;
		std::filesystem::remove_all(temporaryRoot_, ignored);
	}

	std::filesystem::path Context::fixture(std::filesystem::path const& relative) const
	{
		require(!relative.empty() && !relative.has_root_path(), "Fixture path must be repository-relative");
		for (auto const& component : relative)
			require(component != "..", "Fixture path must not escape the repository root");
		auto const path = std::filesystem::path(PF_SMOKE_SOURCE_ROOT) / relative;
		require(std::filesystem::is_regular_file(path), "Missing required fixture: " + path.string());
		return path;
	}

	namespace
	{
		std::string singleLine(std::string text)
		{
			for (char& c : text)
				if (c == '\n' || c == '\r') c = ' ';
			return text;
		}
	}

	int run(std::string_view module, std::span<Check const> checks,
		std::span<std::string_view const> arguments, std::ostream& out, std::ostream& err)
	{
		setupProcess();
		std::string_view selected;
		if (arguments.size() == 1 && arguments[0] == "--list")
		{
			for (auto const& check : checks) out << check.name << '\n';
			return 0;
		}
		if (!arguments.empty())
		{
			if (arguments.size() != 2 || arguments[0] != "--check")
			{
				err << "ERROR " << module << ": usage: [--list | --check <name>]\n";
				return 2;
			}
			selected = arguments[1];
			bool found = false;
			for (auto const& check : checks) found = found || check.name == selected;
			if (!found || selected.empty())
			{
				err << "ERROR " << module << ": unknown check: " << singleLine(std::string(selected)) << '\n';
				return 2;
			}
		}

		unsigned passed = 0, failed = 0, skipped = 0;
		try
		{
			Context context;
			for (auto const& check : checks)
			{
				if (!selected.empty() && selected != check.name) continue;
				try
				{
					check.run(context);
					++passed;
					out << "PASS " << module << ' ' << check.name << '\n';
				}
				catch (OptionalCapabilityUnavailable const& error)
				{
					++skipped;
					out << "SKIP " << module << ' ' << check.name << ": " << singleLine(error.what()) << '\n';
				}
				catch (std::exception const& error)
				{
					++failed;
					out << "FAIL " << module << ' ' << check.name << ": " << singleLine(error.what()) << '\n';
				}
				catch (...)
				{
					++failed;
					out << "FAIL " << module << ' ' << check.name << ": unknown exception\n";
				}
			}
		}
		catch (std::exception const& error)
		{
			++failed;
			out << "FAIL " << module << " setup: " << singleLine(error.what()) << '\n';
		}
		out << "SUMMARY " << module << " pass=" << passed << " fail=" << failed << " skip=" << skipped << '\n';
		return failed ? 1 : 0;
	}

	int main(std::string_view module, std::span<Check const> checks, int argc, char** argv)
	{
		setupProcess();
		std::vector<std::string_view> arguments;
		for (int i = 1; i < argc; ++i) arguments.emplace_back(argv[i]);
		return run(module, checks, arguments, std::cout, std::cerr);
	}
}
