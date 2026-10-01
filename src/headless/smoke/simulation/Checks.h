#pragma once
#include "Smoke.h"
#include <vector>

void registerTicking(std::vector<smoke::Check>& checks);
void registerTraversal(std::vector<smoke::Check>& checks);
void registerInteractions(std::vector<smoke::Check>& checks);
void registerDoors(std::vector<smoke::Check>& checks);
void registerDoorQueues(std::vector<smoke::Check>& checks);
void registerCrossingBands(std::vector<smoke::Check>& checks);
void registerScale(std::vector<smoke::Check>& checks);
