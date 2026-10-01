#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	routing_smoke::registerPlanningEditor(checks);
	routing_smoke::registerPlanningTimeEditor(checks);
	return smoke::main("routing-editor", checks, argc, argv);
}
