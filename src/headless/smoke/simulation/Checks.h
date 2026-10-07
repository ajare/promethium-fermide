#pragma once
#include "Smoke.h"
#include <vector>

void registerPause(std::vector<smoke::Check>& checks);
void registerTiming(std::vector<smoke::Check>& checks);
void registerTeardown(std::vector<smoke::Check>& checks);
void registerTicking(std::vector<smoke::Check>& checks);
void registerTraversal(std::vector<smoke::Check>& checks);
void registerInteractions(std::vector<smoke::Check>& checks);
void registerBrokenDoors(std::vector<smoke::Check>& checks);
void registerBrokenExtensibles(std::vector<smoke::Check>& checks);
void registerBrokenLifts(std::vector<smoke::Check>& checks);
void registerBrokenPlatformLifts(std::vector<smoke::Check>& checks);
void registerBrokenShuttles(std::vector<smoke::Check>& checks);
void registerBrokenEscalators(std::vector<smoke::Check>& checks);
void registerBrokenBulkheadDoors(std::vector<smoke::Check>& checks);
void registerDoors(std::vector<smoke::Check>& checks);
void registerDoorQueues(std::vector<smoke::Check>& checks);
void registerCrossingBands(std::vector<smoke::Check>& checks);
void registerAirlocks(std::vector<smoke::Check>& checks);
void registerSecurityScanners(std::vector<smoke::Check>& checks);
void registerAccessPanels(std::vector<smoke::Check>& checks);
void registerBoothWindows(std::vector<smoke::Check>& checks);
void registerDumbwaiters(std::vector<smoke::Check>& checks);
void registerScale(std::vector<smoke::Check>& checks);
void registerFurniture(std::vector<smoke::Check>& checks);
void registerMarkerActions(std::vector<smoke::Check>& checks);
void registerMixedCrawling(std::vector<smoke::Check>& checks);
