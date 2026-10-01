#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	permission_smoke::registerAccess(checks);
	permission_smoke::registerDestinations(checks);
	permission_smoke::registerAdherence(checks);
	permission_smoke::registerLandingAdherence(checks);
	permission_smoke::registerInteractionMobility(checks);
	permission_smoke::registerInteractionGeometry(checks);
	permission_smoke::registerPreflight(checks);
	permission_smoke::registerRefusal(checks);
	return smoke::main("permissions", checks, argc, argv);
}
