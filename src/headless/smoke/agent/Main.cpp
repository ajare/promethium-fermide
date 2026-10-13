#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	agent_smoke::registerIdentity(checks);
	agent_smoke::registerPlayerControl(checks);
	agent_smoke::registerPlayerTransports(checks);
	agent_smoke::registerPlayerAirlocks(checks);
	agent_smoke::registerPlayerChambers(checks);
	agent_smoke::registerPlayerBoothWindows(checks);
	agent_smoke::registerPlayerAccessPanels(checks);
	agent_smoke::registerPlayerPoses(checks);
	agent_smoke::registerPlayerBulkheadPoses(checks);
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
	agent_smoke::registerAgentTypes(checks);
	agent_smoke::registerNoneUsage(checks);
	agent_smoke::registerRemoteIntegration(checks);
	return smoke::main("agent", checks, argc, argv);
}
