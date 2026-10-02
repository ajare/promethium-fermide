// The GUI must report expected graphics initialisation failure as a controlled
// non-zero process exit, never an abort or another hard crash.

#include "Checks.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PF_STARTUP_GUI_EXECUTABLE
#error "PF_STARTUP_GUI_EXECUTABLE must identify the required GUI child product"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	constexpr int StartupTimeoutMs = 30'000;
	constexpr char UnusableVideoDriver[] = "prometheum-fermide-no-such-video-driver";

	std::string environmentValue(char const* name)
	{
#ifdef _WIN32
		char* rawValue{ nullptr };
		size_t length{ 0 };
		if (_dupenv_s(&rawValue, &length, name) != 0 || rawValue == nullptr)
		{
			return {};
		}

		std::string value(rawValue);
		std::free(rawValue);
		return value;
#else
		char const* rawValue = std::getenv(name);
		return rawValue != nullptr ? std::string(rawValue) : std::string();
#endif
	}

	std::filesystem::path requiredGuiExecutable()
	{
		auto configured = environmentValue("PF_GUI_EXECUTABLE");
		std::filesystem::path executable = configured.empty()
			? std::filesystem::path(PF_STARTUP_GUI_EXECUTABLE)
			: std::filesystem::path(std::move(configured));
		std::error_code error;
		if (!std::filesystem::is_regular_file(executable, error))
		{
			throw smoke::Failure("Missing required GUI executable: " + executable.string());
		}
#ifndef _WIN32
		if (access(executable.c_str(), X_OK) != 0)
		{
			throw smoke::Failure("Required GUI executable is not executable: " + executable.string());
		}
#endif
		return std::filesystem::absolute(executable);
	}

#ifdef _WIN32
	std::vector<wchar_t> childEnvironment()
	{
		auto* inherited = GetEnvironmentStringsW();
		if (!inherited) throw smoke::Failure("Could not read the child environment");
		std::vector<wchar_t> environment;
		try
		{
			std::vector<std::wstring> entries;
			for (auto* entry = inherited; *entry; entry += wcslen(entry) + 1)
			{
				if (_wcsnicmp(entry, L"SDL_VIDEODRIVER=", 16) == 0
					|| _wcsnicmp(entry, L"DISPLAY=", 8) == 0
					|| _wcsnicmp(entry, L"WAYLAND_DISPLAY=", 16) == 0) continue;
				entries.emplace_back(entry);
			}
			entries.emplace_back(L"SDL_VIDEODRIVER=prometheum-fermide-no-such-video-driver");
			std::sort(entries.begin(), entries.end(), [](auto const& left, auto const& right)
			{
				return _wcsicmp(left.c_str(), right.c_str()) < 0;
			});
			for (auto const& entry : entries)
			{
				environment.insert(environment.end(), entry.begin(), entry.end());
				environment.push_back(L'\0');
			}
			environment.push_back(L'\0');
		}
		catch (...)
		{
			FreeEnvironmentStringsW(inherited);
			throw;
		}
		FreeEnvironmentStringsW(inherited);
		return environment;
	}

	void runGuiWithUnusableDriver(std::filesystem::path const& guiExecutable)
	{
		auto environment = childEnvironment();
		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo{};

		std::wstring commandLine = L"\"" + guiExecutable.wstring() + L"\"";
		auto const workingDirectory = guiExecutable.parent_path().wstring();
		if (!CreateProcessW(guiExecutable.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
			workingDirectory.c_str(), &startupInfo, &processInfo))
		{
			throw smoke::Failure("Could not launch required GUI executable: " + guiExecutable.string());
		}

		DWORD const waitResult = WaitForSingleObject(processInfo.hProcess,
			static_cast<DWORD>(StartupTimeoutMs));
		if (waitResult == WAIT_TIMEOUT)
		{
			TerminateProcess(processInfo.hProcess, 1);
			WaitForSingleObject(processInfo.hProcess, INFINITE);
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			throw smoke::Failure("GUI executable did not fail fast with an unusable video driver");
		}
		if (waitResult != WAIT_OBJECT_0)
		{
			TerminateProcess(processInfo.hProcess, 1);
			WaitForSingleObject(processInfo.hProcess, INFINITE);
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			throw smoke::Failure("Could not wait for the GUI smoke child process");
		}

		DWORD exitCode{ 0 };
		if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
		{
			CloseHandle(processInfo.hProcess);
			CloseHandle(processInfo.hThread);
			throw smoke::Failure("Could not read the GUI smoke child exit code");
		}
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);

		if (exitCode == 0)
		{
			throw smoke::Failure("GUI exited with 0 despite an unusable video driver");
		}

		// Exit code 3 is the C runtime abort() code; 0xC0000000-range codes
		// are hard crashes. Both mean the failure bypassed the handler.
		if (exitCode == 3)
		{
			throw smoke::Failure("GUI aborted instead of exiting in a controlled way");
		}
		if ((exitCode & 0xC0000000) == 0xC0000000)
		{
			throw smoke::Failure("GUI crashed with exit code " + std::to_string(exitCode)
				+ " instead of exiting in a controlled way");
		}
		if (exitCode != 1)
		{
			throw smoke::Failure("GUI exited with unexpected status " + std::to_string(exitCode)
				+ "; expected controlled graphics startup failure status 1");
		}
	}
#else
	void runGuiWithUnusableDriver(std::filesystem::path const& guiExecutable)
	{
		pid_t const child = fork();
		if (child == -1)
		{
			throw smoke::Failure("Could not fork the GUI smoke child process");
		}

		if (child == 0)
		{
			setenv("SDL_VIDEODRIVER", UnusableVideoDriver, 1);
			unsetenv("DISPLAY");
			unsetenv("WAYLAND_DISPLAY");
			if (chdir(guiExecutable.parent_path().c_str()) != 0) _exit(126);
			if (std::freopen("/dev/null", "w", stdout) == nullptr) _exit(126);
			if (std::freopen("/dev/null", "w", stderr) == nullptr) _exit(126);
			execl(guiExecutable.c_str(), guiExecutable.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}

		int status = 0;
		for (int waitedMs = 0; ; waitedMs += 100)
		{
			pid_t const result = waitpid(child, &status, WNOHANG);
			if (result == child) break;
			if (result == -1 && errno == EINTR) continue;
			if (result == -1)
			{
				kill(child, SIGKILL);
				waitpid(child, &status, 0);
				throw smoke::Failure("Could not wait for the GUI smoke child process");
			}
			if (waitedMs >= StartupTimeoutMs)
			{
				kill(child, SIGKILL);
				waitpid(child, &status, 0);
				throw smoke::Failure("GUI executable did not fail fast with an unusable video driver");
			}
			usleep(100 * 1000);
		}

		if (WIFSIGNALED(status))
		{
			throw smoke::Failure("GUI terminated by signal " + std::to_string(WTERMSIG(status))
				+ "; graphics startup failure must exit controllably");
		}
		if (!WIFEXITED(status))
		{
			throw smoke::Failure("GUI did not exit normally");
		}
		if (WEXITSTATUS(status) == 0)
		{
			throw smoke::Failure("GUI exited with 0 despite an unusable video driver");
		}
		if (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127)
		{
			throw smoke::Failure("Could not execute the required GUI child product");
		}
		if (WEXITSTATUS(status) != 1)
		{
			throw smoke::Failure("GUI exited with unexpected status " + std::to_string(WEXITSTATUS(status))
				+ "; expected controlled graphics startup failure status 1");
		}
	}
#endif

	void graphicsInitializationFailure(smoke::Context const&)
	{
		runGuiWithUnusableDriver(requiredGuiExecutable());
	}
}

void startup_smoke::registerChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "graphicsInitializationFailure", graphicsInitializationFailure });
}
