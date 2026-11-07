// Synthetic editor child / in-process environment probe. No graphics linkage.
#include "../startup/Checks.h"
#include "NonInteractiveProcess.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif

namespace
{
	std::string environment(char const* name)
	{
#ifdef _WIN32
		auto size = GetEnvironmentVariableA(name, nullptr, 0);
		if (!size) return {};
		std::string value(size, '\0');
		value.resize(GetEnvironmentVariableA(name, value.data(), size));
		return value;
#else
		auto value = std::getenv(name);
		return value ? value : "";
#endif
	}
}

int main(int argc, char** argv)
{
	if (argc == 2 && std::string(argv[1]) == "--verify-timeout")
	{
		// Exercise the real platform-specific timeout/kill/reap path without
		// spending the real GUI's 30-second allowance on a synthetic sleeper.
		std::vector<smoke::Check> checks;
		startup_smoke::registerSyntheticTimeoutCheck(checks);
		return smoke::main("startup", checks, 1, argv);
	}

	if (argc == 2 && std::string(argv[1]) == "--verify-environment")
	{
		auto driver = environment("SDL_VIDEODRIVER");
		auto display = environment("DISPLAY");
		auto wayland = environment("WAYLAND_DISPLAY");
#ifdef _WIN32
		SetErrorMode(GetErrorMode() | SEM_NOALIGNMENTFAULTEXCEPT);
#endif
		std::vector<smoke::Check> checks;
		startup_smoke::registerChecks(checks);
		int result = smoke::main("startup", checks, 1, argv);
		if (driver != environment("SDL_VIDEODRIVER") || display != environment("DISPLAY")
			|| wayland != environment("WAYLAND_DISPLAY")) return 91;
#ifdef _WIN32
		if (!(GetErrorMode() & SEM_NOALIGNMENTFAULTEXCEPT)) return 92;
#endif
		return result;
	}

	// Inspect inherited OS flags before doing our own CRT setup: exceptions
	// below really test the runner's inherited suppression of Windows dialogs.
#ifdef _WIN32
	constexpr DWORD flags = SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX;
	if ((GetErrorMode() & flags) != flags) return 93;
#endif
	pf::setupNonInteractiveProcess();
	if (environment("SDL_VIDEODRIVER") != "promethium-fermide-no-such-video-driver"
		|| !environment("DISPLAY").empty() || !environment("WAYLAND_DISPLAY").empty()) return 94;
	if (environment("PF_STARTUP_INHERITED") != "value with spaces") return 95;
	auto mode = environment("PF_STARTUP_PROBE");
	if (mode == "success") return 0;
	if (mode == "wrong") return 23;
	if (mode == "abort") std::abort();
	if (mode == "exception")
	{
#ifdef _WIN32
		RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
		std::raise(SIGKILL);
#endif
	}
	if (mode == "timeout") std::this_thread::sleep_for(std::chrono::seconds(60));
	return 1;
}
