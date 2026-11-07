#pragma once

#include "Smoke.h"
#include <vector>

namespace routing_smoke
{
	void registerPlanning(std::vector<smoke::Check>& checks);
	void registerPlanningEditor(std::vector<smoke::Check>& checks);
	void registerPlanningTime(std::vector<smoke::Check>& checks);
	void registerPlanningTimeEditor(std::vector<smoke::Check>& checks);
	void registerMovement(std::vector<smoke::Check>& checks);
	void registerPathSource(std::vector<smoke::Check>& checks);
	void registerRestoredPaths(std::vector<smoke::Check>& checks);
	void registerIsolatedSectors(std::vector<smoke::Check>& checks);
	void registerMobility(std::vector<smoke::Check>& checks);
	void registerWorkspace(std::vector<smoke::Check>& checks);
	void registerDepthContinuity(std::vector<smoke::Check>& checks);
	void registerSeatOccupancy(std::vector<smoke::Check>& checks);
	void registerThresholdRouteCost(std::vector<smoke::Check>& checks);
	void registerStairRouteCost(std::vector<smoke::Check>& checks);
	void registerLiftRouteCost(std::vector<smoke::Check>& checks);
	void registerShuttleRouteCost(std::vector<smoke::Check>& checks);
	void registerLadderForceBridgeRouteCost(std::vector<smoke::Check>& checks);
}
