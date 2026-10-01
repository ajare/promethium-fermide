#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	behaviour_smoke::registerRegistry(checks);
	behaviour_smoke::registerAssignment(checks);
	behaviour_smoke::registerWorkflow(checks);
	behaviour_smoke::registerRuntimePreflight(checks);
	behaviour_smoke::registerRuntimeContainment(checks);
	behaviour_smoke::registerRuntimeInstances(checks);
	behaviour_smoke::registerRuntimeMovement(checks);
	behaviour_smoke::registerRuntimeCallbacks(checks);
	behaviour_smoke::registerRuntimeScheduling(checks);
	behaviour_smoke::registerRuntimeFailures(checks);
	behaviour_smoke::registerRuntimeDeterminism(checks);
	behaviour_smoke::registerRuntimeScale(checks);
	behaviour_smoke::registerRuntimeAuthorization(checks);
	return smoke::main("behaviours", checks, argc, argv);
}
