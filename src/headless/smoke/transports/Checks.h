#pragma once
#include "Smoke.h"
#include <vector>
#include <cstdint>

inline constexpr uint64_t MaximumSimulationTicks = 1000;

void registerStairs(std::vector<smoke::Check>& checks);
void registerOccupants(std::vector<smoke::Check>& checks);
void registerPlatformLifts(std::vector<smoke::Check>& checks);
void registerLifts(std::vector<smoke::Check>& checks);
void registerShuttles(std::vector<smoke::Check>& checks);
void registerEscalators(std::vector<smoke::Check>& checks);
void registerDeletion(std::vector<smoke::Check>& checks);
void registerDoorQueries(std::vector<smoke::Check>& checks);
void registerBoarding(std::vector<smoke::Check>& checks);
