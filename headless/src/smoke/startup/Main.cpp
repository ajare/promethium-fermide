#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	startup_smoke::registerChecks(checks);
	return smoke::main("startup", checks, argc, argv);
}
