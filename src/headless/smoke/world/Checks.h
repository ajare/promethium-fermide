#pragma once
#include "Smoke.h"
#include <vector>

void registerBoothWindows(std::vector<smoke::Check>& checks);
void registerAirlocks(std::vector<smoke::Check>& checks);
void registerSecurityScanners(std::vector<smoke::Check>& checks);
void registerObjectEditing(std::vector<smoke::Check>& checks);
void registerFloorsAndWalls(std::vector<smoke::Check>& checks);
void registerTopology(std::vector<smoke::Check>& checks);
void registerLayerDeletion(std::vector<smoke::Check>& checks);

void runMarkerIdentitySmokeChecks();
void runDoorTwoSidedButtonSmokeChecks();
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

void registerFurniture(std::vector<smoke::Check>& checks);
