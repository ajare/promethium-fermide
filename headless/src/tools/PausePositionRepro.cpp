#include "ToolSupport.h"
#include "PausePosition.h"

int main(int argc, char** argv)
{
    constexpr auto usage = "Usage: pf-pause-position-repro <minimal|world>";
    return tool::main("pf-pause-position-repro", usage, [&]
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help")
        {
            std::cout << usage << '\n';
            return 0;
        }
        if (argc != 2) throw tool::UsageError("Expected minimal or one World path");
        std::string_view const input(argv[1]);
        if (input == "minimal")
        {
            pause_position::runAll();
            std::cout << "PASS: minimal pause-position reproductions\n";
        }
        else
        {
            tool::pathArgument(input);
            pause_position::runRepro(argv[1]);
            std::cout << "PASS: file-backed pause-position reproduction\n";
        }
        return 0;
    });
}
