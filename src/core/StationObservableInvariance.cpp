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
			constexpr std::array<std::string_view, 45> names{
				"enabled", "healthy", "systemsStepWorldTicks", "systemsTimestepSeconds",
				"worldTick", "elapsedSeconds", "completedTicks", "stores", "transfers",
				"connections", "conversions", "balances", "constituentBalances", "deliveries",
				"wasteEjections", "outages", "maintenanceJobs", "power", "thermal", "coolantLoops",
				"chillers", "heatEngines", "thermalBuffers", "atmospheres", "atmospherePathways",
				"thermalLinks", "airlockCycles", "crews",
				"waterCollectors", "waterTreatments", "electrolyzers", "co2Captures",
				"sabatierReactors", "methaneProcessors", "microbialProcessors", "cropPopulations",
				"algaeCultures", "fuelCells", "hasNextDelivery", "nextDeliverySeconds",
				"hasNextWasteEjection", "nextWasteEjectionSeconds", "endurance", "forecast", "balanceTolerance"};
			std::array<StationObservableInvariance, 63> result{};
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
			constexpr std::array<std::string_view, 4> boundedFields{
				"thermal.reservoirs[].temperatureC", "atmospheres[].temperatureC",
				"coolantLoops[].loopTemperatureC", "algaeCultures[].livingBiomassKg"};
			constexpr std::array<double, 4> bounds{0.06, 0.036, 0.036, 0.00176};
			for (size_t i = 0; i < boundedFields.size(); ++i)
			{
				auto& entry = result[names.size() + verified.size() + generatorFields.size() + i];
				entry.name = boundedFields[i];
				entry.classification = StationObservableInvarianceClass::Bound;
				entry.convergenceOrder = 1;
				entry.bound = bounds[i];
				entry.verifiedThroughRatio = 60;
				entry.basis = "70 s physicalCoupling fixtures only, ratios 1,6,7,30,60; analytical ODE truncation bounds for configured model coefficients, not physical parameter calibration or arbitrary configurations";
				entry.verifiedBy = "station/physicalCoupling";
			}
			auto& cycle = result[58];
			cycle.name = "fuelCells[].electricalKJ";
			cycle.classification = StationObservableInvarianceClass::Bound;
			cycle.convergenceOrder = 1;
			cycle.bound = 8.1;
			cycle.verifiedThroughRatio = 60;
			cycle.basis = "70 s solar-funded 0.001 kg/s electrolysis/fuel-cell cycle only; error is one Station interval of reactant production; ratios 1,6,7,30,60, not a global guarantee";
			cycle.verifiedBy = "station/physicalCoupling";
			auto& decay = result[59];
			decay.name = "power.generators[].decayHeatKJ";
			decay.classification = StationObservableInvarianceClass::Exact;
			decay.absoluteTolerance = 1e-8;
			decay.verifiedThroughRatio = 60;
			decay.basis = "manual single-generator finite-fuel/outage fixtures with qualified sink; ratios 1,6,7,30,60; no global guarantee";
			decay.verifiedBy = "station/generatorExhaustion;station/generatorOutage;station/generatorPortCapacity";
			constexpr std::array<std::string_view, 3> commandFields{
				"atmospherePathways[].openFraction", "atmospherePathways[].lastCommandId",
				"atmospherePathways[].lastCommandSeconds"};
			for (size_t i = 0; i < commandFields.size(); ++i)
			{
				auto& entry = result[60 + i];
				entry.name = commandFields[i];
				entry.classification = StationObservableInvarianceClass::Exact;
				entry.verifiedThroughRatio = 60;
				entry.basis = "authored one-off command landing and provenance at ratios 1,7,60; compare matching elapsed instants, not tick counts";
				entry.verifiedBy = "station/pathwayCommands";
			}
			return result;
		}();
		return entries;
	}
}
