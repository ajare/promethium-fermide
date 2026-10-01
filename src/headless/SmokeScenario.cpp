#define NOMINMAX
#if defined(_WIN32)
#include <Windows.h>
#if defined(_MSC_VER)
#include <crtdbg.h>
#include <cstdlib>
#endif
#endif

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include "PausePosition.h"
#include "core/Coordination.h"

void runLiftCrossingRepro(char const* filename);
void runLiftBoardingRepro(char const* filename);
void runMarkerIdentitySmokeChecks();
void runDoorTwoSidedButtonSmokeChecks();

static_assert(!std::is_convertible_v<core::DeviceOperationId, core::TraversalResourceId>);

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
	try
	{
		if (argc > 1 && std::string(argv[1]) == "--restoration-benchmark")
		{
			std::cerr << "Use pf-restoration-benchmark <world> [cycles].\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]) == "--write-routing-scale-world")
		{
			std::cerr << "Use pf-generate-routing-world <new.world.yaml>.\n";
			return 2;
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
		if (argc > 1 && std::string(argv[1]) == "--metrics-checks")
		{
			std::cerr << "Metrics checks moved to smoke-metrics; use CTest.\n";
			return 2;
		}
		if (argc > 1 && std::string(argv[1]).starts_with("--metrics"))
		{
			std::cerr << "Use pf-metrics-server --help.\n";
			return 2;
		}
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
		if (argc > 1 && std::string(argv[1]) == "--graphics-startup-smoke")
		{
			std::cerr << "Graphics startup checks moved to smoke-startup; use CTest.\n";
			return 2;
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
