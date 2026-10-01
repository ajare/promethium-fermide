// Synthetic child for dispatcher contracts; never runs production checks.
#include <cstdlib>
#include <iostream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <csignal>
#endif

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i) std::cout << "ARG " << argv[i] << '\n';
	std::cout << "CHILD stdout\n" << std::flush;
	std::cerr << "CHILD stderr\n";
	if (auto mode = std::getenv("PF_COMPATIBILITY_PROBE"))
	{
		if (std::string(mode) == "abnormal")
		{
#ifdef _WIN32
			TerminateProcess(GetCurrentProcess(), 0xC0000005UL);
#else
			std::raise(SIGKILL);
#endif
		}
		if (std::string(mode) == "failure") return 23;
	}
	return 0;
}
