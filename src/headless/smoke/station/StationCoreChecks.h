#pragma once
#include "Smoke.h"
#include <vector>
void registerStationProtectionChecks(std::vector<smoke::Check>& checks);
void registerStationClockChecks(std::vector<smoke::Check>& checks);
void registerStationEventSplitChecks(std::vector<smoke::Check>& checks);
void registerStationCoreChecks(std::vector<smoke::Check>& checks);
