#include "ToolSupport.h"
#include "Restoration.h"

int main(int argc, char** argv)
{
    constexpr auto usage = "Usage: pf-restoration-benchmark <world> [cycles:1..1000]";
    return tool::main("pf-restoration-benchmark", usage, [&]
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << usage << '\n';
            return 0;
        }
        if (argc != 2 && argc != 3) throw tool::UsageError("Expected a World and optional cycle count");
        tool::pathArgument(argv[1]);
        auto const cycles = argc == 3 ? tool::number(argv[2], 1, 1000, "Cycles") : 5;
        restoration_support::verify(argv[1], cycles, [](unsigned cycle, double reloadMs, double resetMs)
        {
            std::cout << "restoration-cycle=" << cycle << " reload-ms=" << reloadMs
                << " reset-ms=" << resetMs << " working-set-MiB="
                << tool::workingSetBytes() / (1024.0 * 1024.0)
                << " peak-working-set-MiB=" << tool::peakWorkingSetBytes() / (1024.0 * 1024.0) << '\n';
        });
        return 0;
    });
}
