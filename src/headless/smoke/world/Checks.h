#pragma once
#include "Smoke.h"
#include <vector>

void registerObjectEditing(std::vector<smoke::Check>& checks);
void registerFloorsAndWalls(std::vector<smoke::Check>& checks);
void registerTopology(std::vector<smoke::Check>& checks);
void registerLayerDeletion(std::vector<smoke::Check>& checks);

void runBackgroundSectorSmokeChecks();
void runBackgroundPaintSmokeChecks();
void runBackgroundPlacementSmokeChecks();
void runBackgroundCascadeDeleteSmokeChecks();
void runEditorLayerSmokeChecks();
void runWindowLayerSmokeChecks();
void runWindowIntoBackgroundSmokeChecks();
void runWindowMultiBackgroundSmokeChecks();
void runFacadeSmokeChecks();
void runZeroSizeLocationSmokeChecks();
void runThresholdLayerOverlapSmokeChecks();
