#pragma once

#include "Smoke.h"
#include <vector>

namespace behaviour_smoke
{
	void registerRegistry(std::vector<smoke::Check>& checks);
	void registerAssignment(std::vector<smoke::Check>& checks);
	void registerWorkflow(std::vector<smoke::Check>& checks);
	void registerRuntimePreflight(std::vector<smoke::Check>& checks);
	void registerRuntimeContainment(std::vector<smoke::Check>& checks);
	void registerRuntimeInstances(std::vector<smoke::Check>& checks);
	void registerRuntimeCoroutines(std::vector<smoke::Check>& checks);
	void registerRuntimeMovement(std::vector<smoke::Check>& checks);
	void registerRuntimeCallbacks(std::vector<smoke::Check>& checks);
	void registerRuntimeScheduling(std::vector<smoke::Check>& checks);
	void registerRuntimeFailures(std::vector<smoke::Check>& checks);
	void registerRuntimeDeterminism(std::vector<smoke::Check>& checks);
	void registerRuntimeScale(std::vector<smoke::Check>& checks);
	void registerRuntimeAuthorization(std::vector<smoke::Check>& checks);
	void registerRegistryEditor(std::vector<smoke::Check>& checks);
	void registerAssignmentEditor(std::vector<smoke::Check>& checks);
	void registerPortabilityEditor(std::vector<smoke::Check>& checks);
	void registerDeleteEditor(std::vector<smoke::Check>& checks);
	void registerSchemaReconciliationEditor(std::vector<smoke::Check>& checks);
	void registerWorkflowEditor(std::vector<smoke::Check>& checks);
}
