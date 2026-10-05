#include "Observation.h"
#include "Checks.h"

namespace
{
	void observation(smoke::Context const&)
	{
		runSimulationObservationSmokeChecks();
	}
}

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks = { { "observation", observation } };
	registerPause(checks);
	registerTiming(checks);
	registerTeardown(checks);
	registerTicking(checks);
	registerTraversal(checks);
	registerInteractions(checks);
	registerDoors(checks);
	registerBrokenExtensibles(checks);
	registerBrokenEscalators(checks);
	registerBrokenLifts(checks);
	registerBrokenPlatformLifts(checks);
	registerBrokenShuttles(checks);
	registerBrokenDoors(checks);
	registerBrokenBulkheadDoors(checks);
	registerDoorQueues(checks);
	registerCrossingBands(checks);
	registerAirlocks(checks);
	registerSecurityScanners(checks);
	registerAccessPanels(checks);
	registerBoothWindows(checks);
	registerDumbwaiters(checks);
	registerFurniture(checks);
	registerScale(checks);
	return smoke::main("simulation", checks, argc, argv);
}
