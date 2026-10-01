#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	routing_smoke::registerPlanning(checks);
	routing_smoke::registerPlanningTime(checks);
	routing_smoke::registerMovement(checks);
	routing_smoke::registerRestoredPaths(checks);
	routing_smoke::registerIsolatedSectors(checks);
	routing_smoke::registerMobility(checks);
	routing_smoke::registerWorkspace(checks);
	routing_smoke::registerThresholdRouteCost(checks);
	routing_smoke::registerStairRouteCost(checks);
	routing_smoke::registerLiftRouteCost(checks);
	routing_smoke::registerShuttleRouteCost(checks);
	routing_smoke::registerLadderForceBridgeRouteCost(checks);
	routing_smoke::registerPathSource(checks);
	return smoke::main("routing", checks, argc, argv);
}
