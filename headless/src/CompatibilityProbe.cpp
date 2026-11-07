// Synthetic child for dispatcher contracts; never runs production checks.
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <csignal>
#endif

namespace
{
	std::optional<std::string> environment(char const* name)
	{
#ifdef _WIN32
		auto size = GetEnvironmentVariableA(name, nullptr, 0);
		if (!size) return std::nullopt;
		std::string value(size, '\0');
		auto length = GetEnvironmentVariableA(name, value.data(), size);
		if (!length || length >= size) return std::nullopt;
		value.resize(length);
		return value;
#else
		if (auto value = std::getenv(name)) return std::string(value);
		return std::nullopt;
#endif
	}
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) std::cout << "ARG " << argv[i] << '\n';
	std::cout << "CHILD stdout\n" << std::flush;
	std::cerr << "CHILD stderr\n";
	if (auto value = environment("PF_COMPATIBILITY_ENVIRONMENT"))
		std::cout << "ENV " << *value << '\n';
#ifdef _WIN32
	if (environment("PF_COMPATIBILITY_ERROR_MODE"))
		std::cout << "ERROR_MODE " << GetErrorMode() << '\n';
#endif
	// Allow different outcomes in the same dispatch to verify first-failure order.
	auto key = "PF_COMPATIBILITY_EXIT_" + std::filesystem::path(argv[0]).stem().string();
	if (auto value = environment(key.c_str()))
	{
#ifdef _WIN32
		std::cout.flush();
		ExitProcess(static_cast<UINT>(std::strtoul(value->c_str(), nullptr, 10)));
#else
		return std::atoi(value->c_str());
#endif
	}
	if (auto mode = environment("PF_COMPATIBILITY_PROBE"))
	{
		if (*mode == "abnormal")
		{
#ifdef _WIN32
			// A real unhandled exception, relying on the dispatcher's inherited
			// error mode to prevent Windows Error Reporting dialogs.
			RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
			std::raise(SIGKILL);
#endif
		}
		if (*mode == "failure") return 23;
	}
	return 0;
}
