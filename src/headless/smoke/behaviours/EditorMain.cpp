#include "Checks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	behaviour_smoke::registerRegistryEditor(checks);
	behaviour_smoke::registerAssignmentEditor(checks);
	behaviour_smoke::registerPortabilityEditor(checks);
	behaviour_smoke::registerDeleteEditor(checks);
	behaviour_smoke::registerSchemaReconciliationEditor(checks);
	behaviour_smoke::registerWorkflowEditor(checks);
	return smoke::main("behaviours-editor", checks, argc, argv);
}
