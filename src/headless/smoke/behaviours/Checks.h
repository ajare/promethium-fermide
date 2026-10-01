#pragma once

#include "Smoke.h"
#include <vector>

namespace behaviour_smoke
{
	void registerRegistry(std::vector<smoke::Check>& checks);
	void registerAssignment(std::vector<smoke::Check>& checks);
	void registerWorkflow(std::vector<smoke::Check>& checks);
	void registerRegistryEditor(std::vector<smoke::Check>& checks);
	void registerAssignmentEditor(std::vector<smoke::Check>& checks);
	void registerPortabilityEditor(std::vector<smoke::Check>& checks);
	void registerDeleteEditor(std::vector<smoke::Check>& checks);
	void registerSchemaReconciliationEditor(std::vector<smoke::Check>& checks);
	void registerWorkflowEditor(std::vector<smoke::Check>& checks);
}
