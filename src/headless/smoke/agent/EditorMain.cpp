#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	agent_smoke::registerGroupEditor(checks);
	agent_smoke::registerGroupAssignmentEditor(checks);
	agent_smoke::registerGroupCountEditor(checks);
	agent_smoke::registerGroupDeleteEditor(checks);
	agent_smoke::registerGroupIdAllocationEditor(checks);
	agent_smoke::registerGroupTopologyEditor(checks);
	agent_smoke::registerActivationEditor(checks);
	agent_smoke::registerGroupClipboardEditor(checks);
	agent_smoke::registerColourEditor(checks);
	agent_smoke::registerWalkSpeedEditor(checks);
	agent_smoke::registerHeightEditor(checks);
	return smoke::main("agent-editor", checks, argc, argv);
}
