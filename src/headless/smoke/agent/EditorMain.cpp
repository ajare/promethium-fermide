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
	return smoke::main("agent-editor", checks, argc, argv);
}
