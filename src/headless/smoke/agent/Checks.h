#pragma once

#include "Smoke.h"
#include <vector>

namespace agent_smoke
{
	void registerIdentity(std::vector<smoke::Check>& checks);
	void registerGroup(std::vector<smoke::Check>& checks);
	void registerGroupAssignment(std::vector<smoke::Check>& checks);
	void registerGroupCount(std::vector<smoke::Check>& checks);
	void registerGroupDelete(std::vector<smoke::Check>& checks);
	void registerGroupIdAllocation(std::vector<smoke::Check>& checks);
	void registerGroupTopology(std::vector<smoke::Check>& checks);
	void registerActivation(std::vector<smoke::Check>& checks);
	void registerColour(std::vector<smoke::Check>& checks);
	void registerWalkSpeed(std::vector<smoke::Check>& checks);
	void registerHeight(std::vector<smoke::Check>& checks);
	void registerIndividualProperties(std::vector<smoke::Check>& checks);
	void registerAgentTypes(std::vector<smoke::Check>& checks);
	void registerNoneUsage(std::vector<smoke::Check>& checks);
	void registerRemoteIntegration(std::vector<smoke::Check>& checks);
	void registerAgentTypeEditor(std::vector<smoke::Check>& checks);
	void registerGroupEditor(std::vector<smoke::Check>& checks);
	void registerGroupAssignmentEditor(std::vector<smoke::Check>& checks);
	void registerGroupCountEditor(std::vector<smoke::Check>& checks);
	void registerGroupDeleteEditor(std::vector<smoke::Check>& checks);
	void registerGroupIdAllocationEditor(std::vector<smoke::Check>& checks);
	void registerGroupTopologyEditor(std::vector<smoke::Check>& checks);
	void registerActivationEditor(std::vector<smoke::Check>& checks);
	void registerGroupClipboardEditor(std::vector<smoke::Check>& checks);
	void registerColourEditor(std::vector<smoke::Check>& checks);
	void registerPoseEditor(std::vector<smoke::Check>& checks);
	void registerWalkSpeedEditor(std::vector<smoke::Check>& checks);
	void registerHeightEditor(std::vector<smoke::Check>& checks);
	void registerMobilityProfileEditor(std::vector<smoke::Check>& checks);
}
