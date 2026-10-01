#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	agent_smoke::registerIdentity(checks);
	agent_smoke::registerGroup(checks);
	agent_smoke::registerGroupAssignment(checks);
	agent_smoke::registerGroupCount(checks);
	agent_smoke::registerGroupDelete(checks);
	agent_smoke::registerGroupIdAllocation(checks);
	agent_smoke::registerGroupTopology(checks);
	agent_smoke::registerActivation(checks);
	agent_smoke::registerColour(checks);
	agent_smoke::registerWalkSpeed(checks);
	agent_smoke::registerHeight(checks);
	agent_smoke::registerIndividualProperties(checks);
	return smoke::main("agent", checks, argc, argv);
}
