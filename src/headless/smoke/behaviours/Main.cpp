#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	behaviour_smoke::registerRegistry(checks);
	behaviour_smoke::registerAssignment(checks);
	behaviour_smoke::registerWorkflow(checks);
	return smoke::main("behaviours", checks, argc, argv);
}
