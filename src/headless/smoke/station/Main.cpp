#include "StationChecks.h"

#include "core/StationSystemsRuntime.h"

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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
	registerStationPathwayChecks(checks);
	registerStationThermalLinkChecks(checks);
	registerStationHeatEngineChecks(checks);
	registerStationBottomingCycleChecks(checks);
	registerStationAdvectionChecks(checks);
	registerStationWaterChecks(checks);
	registerCompoundWaterChecks(checks);
	registerStationCompoundPowerChecks(checks);
	registerStationBatteryChecks(checks);
	registerStationLogisticsChecks(checks);
	registerStationCoreChecks(checks);
	registerStationFaultChecks(checks);
	registerStationMonitorChecks(checks);
	registerStationStoreConnectionMonitorChecks(checks);
	registerStationEclssBiologyMonitorChecks(checks);
	registerStationConditionChecks(checks);
	registerStationWearChecks(checks);
	registerStationWearCatalogueChecks(checks);
	registerStationThermalPlantFaultChecks(checks);
	registerStationPowerSourceFaultChecks(checks);
	registerStationContaminationFaultChecks(checks);
	registerStationClockChecks(checks);
	registerStationRuntimeRegressionChecks(checks);
	registerStationInvarianceChecks(checks);
	registerStationPhysicalFidelityChecks(checks);
	registerStationProtectionChecks(checks);
	registerStationEventSplitChecks(checks);
	registerStationPortChecks(checks);
	registerStationSeamChecks(checks);
	registerStationPrefabChecks(checks);
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

	std::vector<std::string_view> arguments;
	uint32_t ratio = core::StationSystemsSettings{}.systemsStepWorldTicks;
	bool ratioRequested = false;
	for (int index = 1; index < argc; ++index)
	{
		std::string_view argument(argv[index]);
		if (argument != "--step-ratio") { arguments.push_back(argument); continue; }
		if (++index == argc)
		{
			std::cerr << "ERROR station: usage: [--step-ratio <1..60>] [--list | --check <name>]\n";
			return 2;
		}
		ratioRequested = true;
		auto const first = argv[index];
		auto const last = first + std::char_traits<char>::length(first);
		auto const parsed = std::from_chars(first, last, ratio);
		if (parsed.ec != std::errc{} || parsed.ptr != last || ratio < 1
			|| ratio > core::CORE_MAX_SYSTEMS_STEP_WORLD_TICKS)
		{
			std::cerr << "ERROR station: usage: [--step-ratio <1..60>] [--list | --check <name>]\n";
			return 2;
		}
	}
	core::setStationSystemsRuntimeStepRatioOverrideForTesting(
		ratioRequested ? std::optional<uint32_t>{ratio} : std::nullopt);
	std::cout << "RUN station requested-step-ratio=" << (ratioRequested ? std::to_string(ratio) : "authored")
		<< " (explicit-ratio comparisons own their clock)\n";
	// The runner has completed Context cleanup before it prints SUMMARY. Do not
	// enter static destruction afterwards: long Station runs previously printed
	// a passing summary and then could stall during process teardown.
	auto const status = smoke::run("station", checks, arguments, std::cout, std::cerr);
	std::cout.flush();
	std::cerr.flush();
	std::_Exit(status);
}
