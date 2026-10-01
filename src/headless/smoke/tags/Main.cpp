#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	tag_smoke::registerRegistry(checks);
	tag_smoke::registerAssignment(checks);
	tag_smoke::registerReconciliation(checks);
	tag_smoke::registerMobilityProfile(checks);
	return smoke::main("agent-tags", checks, argc, argv);
}
