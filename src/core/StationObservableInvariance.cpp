#include "core/StationObservableInvariance.h"
#include "core/StationSystems.h"

#include <array>

namespace core
{
	std::span<StationObservableInvariance const> stationObservableInvariances()
	{
		// An aggregate mixes physical state, last-step doses, rates and step
		// stamps. Comparing a few inventories cannot certify the entire object.
		static constexpr auto entries = []
		{
			constexpr std::array<std::string_view, 42> names{
				"enabled", "healthy", "systemsStepWorldTicks", "systemsTimestepSeconds",
				"worldTick", "elapsedSeconds", "completedTicks", "stores", "transfers",
				"connections", "conversions", "balances", "constituentBalances", "deliveries",
				"wasteEjections", "outages", "maintenanceJobs", "power", "thermal", "coolantLoops",
				"chillers", "thermalBuffers", "atmospheres", "airlockCycles", "crews",
				"waterCollectors", "waterTreatments", "electrolyzers", "co2Captures",
				"sabatierReactors", "methaneProcessors", "microbialProcessors", "cropPopulations",
				"algaeCultures", "fuelCells", "hasNextDelivery", "nextDeliverySeconds",
				"hasNextWasteEjection", "nextWasteEjectionSeconds", "endurance", "forecast", "balanceTolerance"};
			std::array<StationObservableInvariance, 51> result{};
			for (size_t i = 0; i < names.size(); ++i)
			{
				auto& entry = result[i];
				entry.name = names[i];
				entry.classification = StationObservableInvarianceClass::Unverified;
				entry.basis = "mixed aggregate or physical observable without complete cross-step evidence";
				if (entry.name == "worldTick" || entry.name == "completedTicks"
					|| entry.name == "systemsStepWorldTicks" || entry.name == "systemsTimestepSeconds"
					|| entry.name == "elapsedSeconds")
				{
					entry.classification = StationObservableInvarianceClass::StepDependent;
					entry.basis = "effective Station clock; compare physical states at matching elapsed instants, not equal counts or arbitrary targets";
					entry.verifiedBy = "station/clock";
					entry.verifiedThroughRatio = CORE_MAX_SYSTEMS_STEP_WORLD_TICKS;
				}
				if (entry.name == "enabled" || entry.name == "balanceTolerance")
				{
					entry.classification = StationObservableInvarianceClass::Exact;
					entry.certifiedRatio = CORE_MAX_SYSTEMS_STEP_WORLD_TICKS;
					entry.verifiedThroughRatio = CORE_MAX_SYSTEMS_STEP_WORLD_TICKS;
					entry.basis = "authored setting or published constant";
					entry.verifiedBy = "station/invarianceRepresentative";
				}
			}
			constexpr std::array<std::string_view, 7> verified{
				"stores[].currentQualified", "stores[].currentUnqualified", "connections[].inputTotal",
				"connections[].deliveredTotal", "connections[].lostTotal", "deliveries[].acceptedTotal", "balances[].residual"};
			for (size_t i = 0; i < verified.size(); ++i)
			{
				auto& entry = result[names.size() + i];
				entry.name = verified[i];
				entry.classification = StationObservableInvarianceClass::Exact;
				entry.absoluteTolerance = 1e-12;
				entry.relativeTolerance = 1e-12;
				entry.verifiedThroughRatio = CORE_MAX_SYSTEMS_STEP_WORLD_TICKS;
				entry.basis = "constant-rate transfer with delivery and finite-store saturation at sampled ratios 1,6,7,30,60; fixture-scoped, not a guarantee for arbitrary coupled configurations";
				entry.verifiedBy = "station/invarianceRepresentative";
			}
			constexpr std::array<std::string_view, 2> generatorFields{
				"power.generators[].electricalKJ", "power.generators[].startupReceivedKJ"};
			for (size_t i = 0; i < generatorFields.size(); ++i)
			{
				auto& entry = result[names.size() + verified.size() + i];
				entry.name = generatorFields[i];
				entry.classification = StationObservableInvarianceClass::Exact;
				entry.absoluteTolerance = 1e-8;
				entry.verifiedThroughRatio = CORE_MAX_SYSTEMS_STEP_WORLD_TICKS;
				entry.basis = "fixture-scoped manual affine ramps at sampled ratios 1,6,7,30,60 with ample fuel, qualified sinks and lossless connections; constant/delayed solar or generator-funded crank, not arbitrary coupled configurations";
				entry.verifiedBy = "station/invarianceGeneratorTiming;station/invarianceGeneratorFunding";
			}
			return result;
		}();
		return entries;
	}
}
