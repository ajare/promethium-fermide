#pragma once

#include "Smoke.h"

#include <vector>

void registerGameRuntimeOwnership(std::vector<smoke::Check>& checks);
void registerGameRuntimeTicking(std::vector<smoke::Check>& checks);
void registerGameRuntimeLifecycle(std::vector<smoke::Check>& checks);
void registerGameRuntimeObservation(std::vector<smoke::Check>& checks);
void registerGameRuntimeRenderView(std::vector<smoke::Check>& checks);
void registerGameRuntimeSharedLibrary(std::vector<smoke::Check>& checks);
