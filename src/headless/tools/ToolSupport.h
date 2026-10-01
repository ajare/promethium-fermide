#pragma once

#include <charconv>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#include <cstdlib>
#include <crtdbg.h>
#endif

namespace tool
{
class UsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

inline unsigned number(std::string_view value, unsigned minimum, unsigned maximum, char const* name)
{
    unsigned result = 0;
    auto const parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || result < minimum || result > maximum)
        throw UsageError(std::string(name) + " must be an integer between "
            + std::to_string(minimum) + " and " + std::to_string(maximum));
    return result;
}

inline void pathArgument(std::string_view value)
{
    if (value.empty() || value.starts_with("-"))
        throw UsageError("Expected a non-empty file path (prefix option-like paths with ./)");
}

// Never open OS/CRT error dialogs in unattended tools, including Debug builds.
template<class Run>
int main(char const* name, char const* usage, Run run)
{
#if defined(_WIN32)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
#endif
    try { return run(); }
    catch (UsageError const& error)
    {
        std::cerr << name << ": " << error.what() << '\n' << usage << '\n';
        return 2;
    }
    catch (std::exception const& error)
    {
        std::cerr << name << ": " << error.what() << '\n';
        return 1;
    }
}

size_t workingSetBytes();
size_t peakWorkingSetBytes();
}
