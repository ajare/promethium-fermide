#include "ToolSupport.h"
#include "LiftBoarding.h"

int main(int argc, char** argv)
{
    constexpr auto usage = "Usage: pf-lift-repro <crossing|boarding> <world>";
    return tool::main("pf-lift-repro", usage, [&]
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << usage << '\n';
            return 0;
        }
        if (argc != 3) throw tool::UsageError("Expected a reproduction kind and World path");
        std::string_view const kind(argv[1]);
        if (kind != "crossing" && kind != "boarding")
            throw tool::UsageError("Reproduction kind must be crossing or boarding");
        tool::pathArgument(argv[2]);
        if (kind == "crossing")
        {
            headless::checkLiftCrossings(argv[2], false);
            headless::checkLiftCrossings(argv[2], true);
            std::cout << "PASS: Lift boarding stays within the landing doorway in full and two-Agent runs\n";
        }
        else
        {
            headless::checkLiftBoarding(argv[2], false);
            headless::checkLiftBoarding(argv[2], true);
            std::cout << "PASS: Lift demand drained in full and reduced boarding regressions\n";
        }
        return 0;
    });
}
