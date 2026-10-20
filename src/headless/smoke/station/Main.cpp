#include "StationChecks.h"

int main(int argc, char** argv)
{
	std::vector<smoke::Check> checks;
	registerStationOperationsChecks(checks);
	registerStationResilienceChecks(checks);
	registerStationTrendChecks(checks);
	registerStationForecastChecks(checks);
	registerStationRecoveryChecks(checks);
	registerBioconversionChecks(checks);
	registerStationBiologyChecks(checks);
	registerStationThermalChecks(checks);
	registerStationGenerationChecks(checks);
	registerStationAtmosphereChecks(checks);
	registerStationWaterChecks(checks);
	registerCompoundWaterChecks(checks);
	registerStationCompoundPowerChecks(checks);
	registerStationBatteryChecks(checks);
	registerStationLogisticsChecks(checks);
	registerStationCoreChecks(checks);
	registerStationClockChecks(checks);
	registerStationEventSplitChecks(checks);
	registerStationPortChecks(checks);
	registerStationSeamChecks(checks);
	registerStationPowerChecks(checks);
	registerStationDocumentChecks(checks);
	registerScheduledTransferChecks(checks);
	registerStationConnectionChecks(checks);
	registerBatteryObservationChecks(checks);
	registerObservationIntegrationChecks(checks);
	registerMaterialObservationChecks(checks);
	registerCoupledStationChecks(checks);
	registerReferenceStationChecks(checks);
	registerScenarioChecks(checks);
	registerStabilityChecks(checks);
	return smoke::main("station", checks, argc, argv);
}
