#include "NonInteractiveProcess.h"
// Compatibility only: no production or smoke-check linkage.
#include "CompatibilityModules.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace
{
	using Command = std::vector<std::string>;
	using Commands = std::vector<Command>;

	std::filesystem::path executableDirectory()
	{
#ifdef _WIN32
		std::vector<wchar_t> buffer(32768);
		auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (!size || size == buffer.size()) throw std::runtime_error("Cannot locate compatibility executable");
		return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
#else
		return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
	}

#ifdef _WIN32
	// CommandLineToArgv/CRT quoting, including embedded quotes and trailing slashes.
	std::wstring quote(std::wstring const& value)
	{
		std::wstring result = L"\"";
		size_t slashes = 0;
		for (auto ch : value)
		{
			if (ch == L'\\') { ++slashes; continue; }
			result.append(ch == L'"' ? 2 * slashes + 1 : slashes, L'\\');
			slashes = 0;
			result += ch;
		}
		result.append(2 * slashes, L'\\');
		return result + L'"';
	}
#endif

	int run(std::filesystem::path const& directory, Command const& command)
	{
		auto path = directory / command.front();
#ifdef _WIN32
		path += ".exe";
#endif
		std::cout << "RUN " << command.front() << std::endl;
		std::error_code discoveryError;
		auto exists = std::filesystem::is_regular_file(path, discoveryError);
		if (discoveryError && discoveryError != std::errc::no_such_file_or_directory)
		{
			std::cerr << "ERROR " << command.front() << ": executable discovery failed: " << discoveryError.message() << '\n';
			return 126;
		}
		if (!exists)
		{
			std::cerr << "ERROR " << command.front() << ": missing executable " << path << '\n';
			return 127;
		}
#ifdef _WIN32
		std::wstring line = quote(path.wstring());
		for (size_t i = 1; i < command.size(); ++i)
			line += L" " + quote(std::filesystem::path(command[i]).wstring());
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(path.c_str(), line.data(), nullptr, nullptr, TRUE, 0,
			nullptr, nullptr, &startup, &process))
		{
			auto error = GetLastError(); // Stream operations can overwrite thread last-error.
			std::cerr << "ERROR " << command.front() << ": launch failed, Windows error " << error << '\n';
			return 126;
		}
		CloseHandle(process.hThread);
		auto waited = WaitForSingleObject(process.hProcess, INFINITE);
		DWORD status = 0;
		bool queried = GetExitCodeProcess(process.hProcess, &status) != 0;
		CloseHandle(process.hProcess);
		if (waited != WAIT_OBJECT_0 || !queried)
		{
			std::cerr << "ERROR " << command.front() << ": cannot retrieve child outcome\n";
			return 1;
		}
		if (status >= 0x80000000UL)
		{
			std::cerr << "ERROR " << command.front() << ": abnormal termination, Windows status " << status << '\n';
			return 1;
		}
		if (status > 255)
			std::cerr << "ERROR " << command.front() << ": Windows exit " << status << " normalized to 1\n";
		int code = status <= 255 ? static_cast<int>(status) : 1;
#else
		std::string executable = path.string();
		std::vector<char*> args{ executable.data() };
		for (size_t i = 1; i < command.size(); ++i) args.push_back(const_cast<char*>(command[i].c_str()));
		args.push_back(nullptr);
		pid_t child;
		int error = posix_spawn(&child, executable.c_str(), nullptr, nullptr, args.data(), environ);
		if (error)
		{
			std::cerr << "ERROR " << command.front() << ": launch failed: " << std::strerror(error) << '\n';
			return error == ENOENT ? 127 : 126;
		}
		int status;
		pid_t waited;
		do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
		if (waited < 0) throw std::runtime_error("Cannot wait for child");
		if (WIFSIGNALED(status))
		{
			std::cerr << "ERROR " << command.front() << ": abnormal termination, signal " << WTERMSIG(status) << '\n';
			return 128 + WTERMSIG(status);
		}
		if (!WIFEXITED(status)) throw std::runtime_error("Unexpected child status");
		int code = WEXITSTATUS(status);
#endif
		if (code) std::cerr << "FAIL " << command.front() << ": exit " << code << '\n';
		else std::cout << "PASS " << command.front() << std::endl;
		return code;
	}

	Commands select(int argc, char** argv)
	{
		if (argc == 1)
		{
			Commands commands;
			for (auto name : compatibilityModules) commands.push_back({ name });
			return commands;
		}
		std::string option = argv[1];
		static std::map<std::string, Commands> const selections{
			{ "--viewport-checks", {{ "pf-smoke-render" }} },
			{ "--render-checks", {{ "pf-smoke-render" }} },
			{ "--agent-behaviour-checks", {{ "pf-smoke-behaviours" }, { "pf-smoke-editor" }} },
			{ "--access-permission-checks", {{ "pf-smoke-permissions" }, { "pf-smoke-editor" }} },
			{ "--route-planning-checks", {{ "pf-smoke-routing" }, { "pf-smoke-editor" }, { "pf-smoke-render" }} },
			{ "--route-planning-time-checks", {{ "pf-smoke-routing" }, { "pf-smoke-editor" }, { "pf-smoke-render" }} },
			{ "--routing-scale-checks", {{ "pf-smoke-routing" }} },
			{ "--shuttle-route-checks", {{ "pf-smoke-routing" }} },
			{ "--restored-path-checks", {{ "pf-smoke-routing" }} },
			{ "--serialization-checks", {{ "pf-smoke-persistence" }, { "pf-smoke-render" }} },
			{ "--coordinated-document-checks", {{ "pf-smoke-editor" }} },
			{ "--metrics-checks", {{ "pf-smoke-metrics" }} },
			{ "--world-teardown-smoke", {{ "pf-smoke-simulation" }} },
			{ "--graphics-startup-smoke", {{ "pf-smoke-startup" }} }
		};
		if (auto found = selections.find(option); found != selections.end())
		{
			if (argc != 2) throw std::invalid_argument("Smoke selections accept no extra arguments");
			return found->second;
		}
		Command tool;
		if (option == "--restoration-benchmark") tool = { "pf-restoration-benchmark" };
		else if (option == "--write-routing-scale-world") tool = { "pf-generate-routing-world" };
		else if (option == "--pause-position-repro") tool = { "pf-pause-position-repro" };
		else if (option == "--lift-crossing-repro") tool = { "pf-lift-repro", "crossing" };
		else if (option == "--lift-stall-repro") tool = { "pf-lift-repro", "boarding" };
		else if (option.starts_with("--metrics"))
		{
			tool = { "pf-metrics-server" };
			for (int i = 1; i < argc; ++i)
			{
				std::string arg = argv[i];
				if (arg == "--metrics") continue;
				if (arg == "--metrics-port" || arg == "--metrics-world")
				{
					tool.push_back(arg == "--metrics-port" ? "--port" : "--world");
					if (++i == argc) throw std::invalid_argument("Missing metrics option value");
					tool.push_back(argv[i]);
				}
				else if (arg == "--metrics-detail=sector,queue") tool.push_back("--detail=sector,queue");
				else tool.push_back(arg); // Dedicated tool validates remaining arguments.
			}
			return { tool };
		}
		else throw std::invalid_argument("Unknown legacy option: " + option);
		for (int i = 2; i < argc; ++i) tool.push_back(argv[i]);
		return { tool };
	}
}

int main(int argc, char** argv)
{
	pf::setupNonInteractiveProcess();
	try
	{
		if (argc == 2 && std::string(argv[1]) == "--help")
		{
			std::cout << "Usage: prometheum-fermide-headless [legacy-option [tool arguments]]\n"
				"Deprecated compatibility dispatcher. Prefer pf-smoke-* modules, dedicated tools, or CTest.\n"
				"No arguments runs all configured smoke modules sequentially.\n";
			return 0;
		}
		auto commands = select(argc, argv);
		std::cerr << "DEPRECATED: use";
		for (auto const& command : commands) std::cerr << ' ' << command.front();
		std::cerr << " directly (or CTest for smoke coverage).\n";
		auto directory = executableDirectory();
		int result = 0;
		for (auto const& command : commands)
		{
			int code = run(directory, command);
			if (!result) result = code; // Continue coverage, preserve first failure in stable order.
		}
		return result;
	}
	catch (std::invalid_argument const& error)
	{
		std::cerr << "ERROR compatibility: " << error.what() << "; use --help\n";
		return 2;
	}
	catch (std::exception const& error)
	{
		std::cerr << "ERROR compatibility: " << error.what() << '\n';
		return 1;
	}
}
