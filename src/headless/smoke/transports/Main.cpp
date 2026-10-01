#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerStairs(checks);
	registerOccupants(checks);
	registerPlatformLifts(checks);
	registerLifts(checks);
	registerShuttles(checks);
	registerEscalators(checks);
	registerDeletion(checks);
	registerDoorQueries(checks);
	registerBoarding(checks);
	return smoke::main("transports", checks, argc, argv);
}
