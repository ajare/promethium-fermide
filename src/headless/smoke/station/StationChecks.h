#pragma once

// Registration interfaces for the station smoke runner.
#include "Smoke.h"
#include "Documents.h"
#include "Connections.h"
#include "BatteryObservations.h"
#include "ObservationIntegration.h"
#include "StationThermalChecks.h"
#include "StationGenerationChecks.h"
#include "StationAtmosphereChecks.h"
#include "StationWaterChecks.h"
#include "StationRecoveryChecks.h"
#include "StationBiologyChecks.h"
#include "StationCoreChecks.h"
#include "StationPortChecks.h"
#include "StationSeamChecks.h"
#include "StationPowerChecks.h"
#include "StationCompoundPowerChecks.h"
#include "StationBatteryChecks.h"
#include "StationLogisticsChecks.h"
#include "StationOperationsChecks.h"
#include "StationResilienceChecks.h"
void registerScheduledTransferChecks(std::vector<smoke::Check>& checks);
#include "MaterialObservations.h"
#include "Coupled.h"
#include "Reference.h"
#include "Scenarios.h"
#include "Stability.h"
#include "StationTrendChecks.h"
#include "StationForecastChecks.h"
void registerStationInvarianceChecks(std::vector<smoke::Check>& checks);
void registerStationRuntimeRegressionChecks(std::vector<smoke::Check>& checks);
