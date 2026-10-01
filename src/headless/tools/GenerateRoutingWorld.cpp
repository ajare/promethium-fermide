#include "ToolSupport.h"
#include "RoutingPopulation.h"

int main(int argc, char** argv)
{
    constexpr auto usage = "Usage: pf-generate-routing-world <new.world.yaml>";
    return tool::main("pf-generate-routing-world", usage, [&]
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << usage << '\n';
            return 0;
        }
        if (argc != 2) throw tool::UsageError("Expected one output World path");
        tool::pathArgument(argv[1]);
        (void)routing_support::populationRoutingRun(argv[1], true, &std::cout, tool::workingSetBytes);
        std::cout << "PASS: wrote routing stress World and adjacent tag registry\n";
        return 0;
    });
}
