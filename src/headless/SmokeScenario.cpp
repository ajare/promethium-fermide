#define NOMINMAX
#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#else
#error "Unsupported platform"
#endif

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "PausePosition.h"
#include "core/Coordination.h"

#ifdef _MSC_VER
#pragma comment(lib, "Psapi.lib")
#endif

void runLiftCrossingRepro(char const* filename);
void runLiftBoardingRepro(char const* filename);
void runMarkerIdentitySmokeChecks();
void runDoorTwoSidedButtonSmokeChecks();
void writeRoutingScaleWorld(std::filesystem::path const& output);
void runRestorationBenchmark(std::filesystem::path const& input, unsigned cycles);
void runGraphicsStartupSmokeChecks();

static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);

namespace
{
	size_t currentWorkingSetBytes()
	{
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS_EX counters{};
		counters.cb = sizeof(counters);
		return GetProcessMemoryInfo(GetCurrentProcess(),
			reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))
			? counters.WorkingSetSize : 0;
#elif defined(__linux__)
		long totalPages = 0;
		long residentPages = 0;
		std::ifstream statm("/proc/self/statm");
		if (!(statm >> totalPages >> residentPages)) return 0;
		auto const pageSize = sysconf(_SC_PAGESIZE);
		return pageSize > 0 ? static_cast<size_t>(residentPages) * static_cast<size_t>(pageSize) : 0;
#else
#error "Unsupported platform"
#endif
	}
}

void runMetricsChecks();
int runMetricsEndpoint(int argc, char** argv);

size_t getHeadlessWorkingSetBytes()
{
	return currentWorkingSetBytes();
}

size_t getHeadlessPeakWorkingSetBytes()
{
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS counters{};
	counters.cb = sizeof(counters);
	return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))
		? counters.PeakWorkingSetSize : 0;
#else
	return 0; // Unavailable; not a zero-memory claim.
#endif
}

int main(int argc, char** argv)
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
	if (argc > 1 && (std::string(argv[1]) == "--viewport-checks"
		|| std::string(argv[1]) == "--render-checks"))
	{
		std::cerr << "Render and viewport checks moved to smoke-render; use CTest.\n";
		return 2;
	}
	bool const graphicsStartupOnly = argc > 1
		&& std::string(argv[1]) == "--graphics-startup-smoke";

	try
	{
		if (argc > 1 && std::string(argv[1]) == "--restoration-benchmark")
		{
			if (argc != 3 && argc != 4)
				throw std::invalid_argument("Usage: --restoration-benchmark <world> [cycles]");
			unsigned cycles = 5;
			if (argc == 4)
			{
				std::string argument = argv[3];
				size_t consumed = 0;
				auto const count = std::stoul(argument, &consumed);
				if (consumed != argument.size() || count == 0 || count > 1000)
					throw std::invalid_argument("Restoration cycles must be between 1 and 1000");
				cycles = static_cast<unsigned>(count);
			}
			runRestorationBenchmark(argv[2], cycles);
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--write-routing-scale-world")
		{
			if (argc != 3) throw std::invalid_argument("Usage: --write-routing-scale-world <new.world.yaml>");
			writeRoutingScaleWorld(argv[2]);
			std::cout << "PASS: wrote routing stress World and adjacent tag registry\n";
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--agent-behaviour-checks")
		{
			std::cerr << "Agent behaviour checks moved to smoke-behaviours and smoke-behaviours-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--access-permission-checks")
		{
			std::cerr << "Access permission checks moved to smoke-permissions and smoke-permissions-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--route-planning-checks")
		{
			std::cerr << "Route planning checks moved to smoke-routing, smoke-routing-editor and smoke-render; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--route-planning-time-checks")
		{
			std::cerr << "Route planning checks moved to smoke-routing, smoke-routing-editor and smoke-render; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--routing-scale-checks")
		{
			std::cerr << "Routing scale checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--shuttle-route-checks")
		{
			std::cerr << "Shuttle route-cost checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--restored-path-checks")
		{
			std::cerr << "Restored Path checks moved to smoke-routing; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--serialization-checks")
		{
			std::cerr << "Serialization checks moved to smoke-persistence and smoke-render; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--coordinated-document-checks")
		{
			std::cerr << "Coordinated document checks moved to smoke-agent-tags-editor and smoke-behaviours-editor; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--metrics-checks") { runMetricsChecks(); return 0; }
		if (argc > 1 && std::string(argv[1]).starts_with("--metrics")) return runMetricsEndpoint(argc, argv);
		if (argc == 3 && std::string(argv[1]) == "--pause-position-repro")
		{
			if (std::string(argv[2]) == "minimal") pause_position::runAll();
			else pause_position::runRepro(argv[2]);
			return 0;
		}
		if (argc == 3 && std::string(argv[1]) == "--lift-crossing-repro")
		{
			runLiftCrossingRepro(argv[2]);
			return 0;
		}
		if (argc == 3 && std::string(argv[1]) == "--lift-stall-repro")
		{
			runLiftBoardingRepro(argv[2]);
			return 0;
		}
		if (argc > 1 && std::string(argv[1]) == "--world-teardown-smoke")
		{
			std::cerr << "World teardown checks moved to smoke-simulation; use CTest.\n";
			return 2;
		}
		if (graphicsStartupOnly)
		{
			runGraphicsStartupSmokeChecks();
			return 0;
		}

		// Only unmigrated external suites remain here. Domain scenarios are
		// registered directly with their owning modules, not called twice.
		runMarkerIdentitySmokeChecks();
		runDoorTwoSidedButtonSmokeChecks();

		std::cout << "PASS: remaining legacy checks; migrated scenarios run through domain CTests\n";
		return 0;
	}
	catch (std::exception const& exception)
	{
		std::cerr << "FAIL: headless smoke scenario threw: " << exception.what() << '\n';
		return 1;
	}
	catch (...)
	{
		std::cerr << "FAIL: headless smoke scenario threw an unknown exception\n";
		return 1;
	}
}
