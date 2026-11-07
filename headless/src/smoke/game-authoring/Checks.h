#pragma once

#include "Smoke.h"

#include <vector>

void registerGameAuthoringWrapping(std::vector<smoke::Check>& checks);
void registerGameAuthoringMutationThroughRuntime(std::vector<smoke::Check>& checks);
void registerGameAuthoringSharedLibrary(std::vector<smoke::Check>& checks);
