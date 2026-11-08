#pragma once

#include "Smoke.h"

#include <vector>

void registerGameRuntimeOwnership(std::vector<smoke::Check>& checks);
void registerGameRuntimeTicking(std::vector<smoke::Check>& checks);
void registerGameRuntimeLifecycle(std::vector<smoke::Check>& checks);
void registerGameRuntimeObservation(std::vector<smoke::Check>& checks);
void registerGameRuntimePlayerInput(std::vector<smoke::Check>& checks);
void registerGameRuntimeRenderView(std::vector<smoke::Check>& checks);
void registerGameRuntimeConstReadSurface(std::vector<smoke::Check>& checks);
void registerGameRuntimeReadSurface(std::vector<smoke::Check>& checks);
void registerGameRuntimeSharedLibrary(std::vector<smoke::Check>& checks);
