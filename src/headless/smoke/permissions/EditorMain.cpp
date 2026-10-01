#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	permission_smoke::registerDestinationEditor(checks);
	permission_smoke::registerAccessEditor(checks);
	permission_smoke::registerAdherenceEditor(checks);
	permission_smoke::registerLandingAdherenceEditor(checks);
	return smoke::main("permissions-editor", checks, argc, argv);
}
