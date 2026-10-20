#include "StationChecks.h"

#include "core/StationSystemsRuntime.h"

#include <charconv>
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
	registerStationWaterChecks(checks);
	registerCompoundWaterChecks(checks);
	registerStationCompoundPowerChecks(checks);
	registerStationBatteryChecks(checks);
	registerStationLogisticsChecks(checks);
	registerStationCoreChecks(checks);
	registerStationClockChecks(checks);
	registerStationInvarianceChecks(checks);
	registerStationProtectionChecks(checks);
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

	std::vector<std::string_view> arguments;
	uint32_t ratio = 1;
	for (int index = 1; index < argc; ++index)
	{
		std::string_view argument(argv[index]);
		if (argument != "--step-ratio") { arguments.push_back(argument); continue; }
		if (++index == argc)
		{
			std::cerr << "ERROR station: usage: [--step-ratio <1..600>] [--list | --check <name>]\n";
			return 2;
		}
		auto const first = argv[index];
		auto const last = first + std::char_traits<char>::length(first);
		auto const parsed = std::from_chars(first, last, ratio);
		if (parsed.ec != std::errc{} || parsed.ptr != last || ratio < 1
			|| ratio > core::CORE_MAX_SYSTEMS_STEP_WORLD_TICKS)
		{
			std::cerr << "ERROR station: usage: [--step-ratio <1..600>] [--list | --check <name>]\n";
			return 2;
		}
	}
	core::setStationSystemsRuntimeStepRatioOverrideForTesting(ratio);
	std::cout << "RUN station step-ratio=" << ratio << '\n';
	return smoke::run("station", checks, arguments, std::cout, std::cerr);
}
