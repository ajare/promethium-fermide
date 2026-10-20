#include "StationRecoveryChecks.h"
#include "core/World.h"
#include "core/StationReference.h"
#include "CropObservationFixture.h"
#include "core/StationConfigurationDocument.h"
#include "core/AgentTagRegistryDocument.h"
#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <format>

namespace
{
using namespace smoke::station;
using smoke::require;
struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } };
using Basis = core::StationRateBasis;

void close(double actual, double expected)
{
	require(std::abs(actual - expected) < 2e-7 * std::max(1e-6, std::abs(expected)),
		std::format("bioconversion mismatch: {:.17g} expected {:.17g}", actual, expected).c_str());
}
core::StationSystemsSettings urine(smoke::Context const& context)
{
	core::StationConfigurationDocument doc;
	std::string error;
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/bioconversion-urine.yaml").string(), doc, &error), error.c_str());
	return doc.settings;
}
core::StationSystemsSnapshot run(core::StationSystemsSettings settings, int ticks = 60)
{
	core::World world("Bioconversion", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string error;
	auto const configured = world.configureStationSystems(settings, false, &error);
	require(configured, error.c_str());
	world.resumeSimulation();
	if (!world.advanceTicks(ticks))
	{
		for (auto const& diagnostic : world.getStationSystemsDiagnostics()) error += diagnostic.message;
		require(false, error.c_str());
	}
	auto result = world.getStationSystemsSnapshot();
	require(result.healthy, "bioconversion became unhealthy");
	return result;
}
void tracer(smoke::Context const& context)
{
	auto settings = urine(context);
	// Assert the sandbox's own actuals as well as the host snapshot: merely
	// accepting descriptors would miss a stale fixed-rate Lua mirror.
	core::StationSystemsSnapshot snap;
	{
		SourceScope restoreSource;
		core::setStationSystemsSourceLoader([] {
			auto source = core::bundledStationSystemsSource();
			auto const at = source.find("        self.ticks = self.ticks + 1");
			require(at != std::string::npos, "Lua assertion insertion point missing");
			source.insert(at, R"lua(
        local p = self.microbial_processors[1]
        assert(p.rateBasis == 'CompositionDerived' and p.nitrificationFraction == 1)
        assert(math.abs(p.lastOxygenConsumed - p.lastFeedConsumed * 0.06114) < 1e-12)
        assert(math.abs(p.lastNitrifiedNitrogenKg - p.lastFeedConsumed * 0.012) < 1e-12)
        assert(math.abs(p.nitrifiedNitrogenKg - p.nitrogenRecoveredTotal) < 1e-12)
)lua");
			return source;
		});
		snap = run(settings);
	}
	auto const& p = snap.microbialProcessors.at(0);
	auto const daily = 86400.0 / (60 * core::World::getFixedTimestep());
	close(p.feedConsumedKg * daily, 4.0);
	close(p.nitrogenRecoveredKg * daily, 0.048);
	close(p.nitrifiedNitrogenKg * daily, 0.048);
	close(p.oxygenConsumedKg * daily, 0.24456);
	close(p.co2ProducedKg * daily, 0.0324);
	close(p.reactionHeatKJ * daily, 10.08);
	close(p.recoveredWaterKg * daily, 2.66);
	close(p.residueProducedKg * daily, 1.41206);
	close(p.consumablesUsed * daily, 0.0009);
	close(p.waterConsumedKg, 0.0);
	close(p.lastNitrifiedNitrogenKg * 60, p.nitrifiedNitrogenKg);
	close(p.residualKg, 0.0); close(p.residualKJ, 0.0);
	close(snap.thermal.reservoirs[0].microbialEnergyKJ, p.energySuppliedKJ + p.reactionHeatKJ);
	require(p.rateBasis == Basis::CompositionDerived && p.nitrificationFraction == 1.0,
		"derived contract absent from snapshot");
	// The limiter scales every stream, including nitrification, together.
	auto starved = settings;
	starved.stores[1].startingQualified = p.lastOxygenConsumedKg / 2;
	auto const limited = run(starved, 1).microbialProcessors.at(0);
	require(limited.limiter.find("oxygen") != std::string::npos, "missing oxygen limiter");
	close(limited.lastFeedConsumedKg, p.lastFeedConsumedKg / 2);
	close(limited.lastNitrifiedNitrogenKg, p.lastNitrifiedNitrogenKg / 2);
	close(limited.lastCo2ProducedKg, p.lastCo2ProducedKg / 2);
	close(limited.lastRecoveredWaterKg, p.lastRecoveredWaterKg / 2);
	close(limited.lastResidueProducedKg, p.lastResidueProducedKg / 2);
	// A non-kg consumable uses mass conversion, not store units as kg.
	auto grams = settings;
	grams.stores[3].unit = "g";
	auto const g = run(grams).microbialProcessors.at(0);
	close(g.consumablesUsed / 1000, p.consumablesUsed);
	close(g.residueProducedKg, p.residueProducedKg); close(g.residualKg, 0);
	// Feed with no organic oxidation still pays nitrification on recovered N only.
	auto mineral = settings;
	mineral.microbialProcessors[0].organicOxidationFraction = 0;
	mineral.stores[3].startingQualified = 0;
	auto const m = run(mineral).microbialProcessors.at(0);
	close(m.oxygenConsumedKg, m.nitrifiedNitrogenKg * 4.57);
	close(m.lastCo2ProducedKg, 0); close(m.reactionHeatKJ, 0);
	require(m.lastStressors.empty(), "zero-demand medium caused exhaustion stress");
}
void carryover(smoke::Context const& context)
{
	auto legacy = urine(context);
	auto const baseline = run(legacy).microbialProcessors.at(0);
	auto zero = legacy;
	zero.microbialProcessors[0].recoveredWaterCarryoverFractions = {{"nutrientNitrogen", 0}};
	auto const unchanged = run(zero).microbialProcessors.at(0);
	require(baseline.recoveredWaterKg == unchanged.recoveredWaterKg
		&& baseline.residueProducedKg == unchanged.residueProducedKg
		&& baseline.feedConsumedKg == unchanged.feedConsumedKg, "zero carry-over changed legacy split");
	legacy.stores[5].materialId = "water";
	legacy.stores[5].composition = core::ResourceCompositionSettings{};
	legacy.stores[5].composition->moisture = 0.999;
	legacy.microbialProcessors[0].recoveredWaterCarryoverFractions = {{"nutrientNitrogen", 0.5}};
	core::StationSystemsSnapshot snap;
	{
		SourceScope restoreSource;
		core::setStationSystemsSourceLoader([] {
			auto source = core::bundledStationSystemsSource();
			auto const at = source.find("        self.ticks = self.ticks + 1");
			require(at != std::string::npos, "carry-over Lua insertion point missing");
			source.insert(at, R"lua(
        local p = self.microbial_processors[1]
        assert(math.abs(p.lastCarriedConstituents.nutrientNitrogen - p.lastFeedConsumed * 0.0015) < 1e-12)
        assert(math.abs(p.carriedConstituents.nutrientNitrogen - p.feedConsumedTotal * 0.0015) < 1e-12)
)lua");
			return source;
		});
		snap = run(legacy);
	}
	auto const& p = snap.microbialProcessors.at(0);
	close(p.carriedConstituents.at("nutrientNitrogen"), p.feedConsumedKg * 0.0015);
	close(p.lastCarriedConstituents.at("nutrientNitrogen") * 60, p.carriedConstituents.at("nutrientNitrogen"));
	close(p.recoveredWaterKg, baseline.recoveredWaterKg + p.carriedConstituents.at("nutrientNitrogen"));
	close(p.residueProducedKg, baseline.residueProducedKg - p.carriedConstituents.at("nutrientNitrogen"));
	close(p.residualKg, 0);
	close(snap.stores.at(5).actualConstituents.at("nutrientNitrogen"), p.carriedConstituents.at("nutrientNitrogen"));
	for (auto const& balance : snap.constituentBalances)
		require(std::abs(balance.residual) < 1e-8, "carry-over constituent boundary did not close");
	auto saturated = legacy;
	saturated.stores[5].capacity = p.lastRecoveredWaterKg / 2;
	auto const limited = run(saturated, 1).microbialProcessors.at(0);
	close(limited.lastCarriedConstituents.at("nutrientNitrogen"), p.lastCarriedConstituents.at("nutrientNitrogen") / 2);
	close(limited.lastResidueProducedKg, p.lastResidueProducedKg / 2);
	std::string error;
	for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto bad = legacy; bad.microbialProcessors[0].recoveredWaterCarryoverFractions["nutrientNitrogen"] = invalid;
		require(!core::validateStationSystemsSettings(bad, error), "invalid carry-over accepted");
	}
	auto bad = legacy; bad.microbialProcessors[0].recoveredWaterCarryoverFractions["unknown"] = 0;
	require(!core::validateStationSystemsSettings(bad, error), "unknown carry-over constituent accepted");
	bad = legacy; bad.stores[5].composition.reset();
	require(!core::validateStationSystemsSettings(bad, error) && error.find("nutrientNitrogen") != std::string::npos,
		"loaded recovered water went to plain destination");
	// The same scenario document the editor opens feeds public World ticks.
	core::StationConfigurationDocument doc;
	auto const loaded = core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/water-carryover.yaml").string(), doc, &error);
	require(loaded, error.c_str());
	auto const bridge = run(doc.settings);
	close(bridge.microbialProcessors[0].carriedConstituents.at("nutrientNitrogen"), p.carriedConstituents.at("nutrientNitrogen"));
	require(bridge.cropPopulations[0].lastGrowthKg > 0, "crop refused dilute irrigation");
	close(bridge.cropPopulations[0].waterQuality.scale, 1);
	close(bridge.cropPopulations[0].waterQuality.concentrations.at("nutrientNitrogen"), 0.006 / 2.666);
}
void hotDigestion(smoke::Context const& context)
{
	core::StationConfigurationDocument doc;
	std::string error;
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/bioconversion-foodwaste.yaml").string(), doc, &error), error.c_str());
	auto const snap = run(doc.settings);
	auto const& p = snap.microbialProcessors.at(0);
	auto const daily = 86400.0 / (60 * core::World::getFixedTimestep());
	close(p.feedConsumedKg * daily, 0.5);
	close(p.oxygenConsumedKg * daily, 0.5544);
	close(p.co2ProducedKg * daily, 0.7128);
	close(p.waterConsumedKg * daily, 0.75);
	close(p.consumablesUsed * daily, 0.0198);
	close(p.nutrientProducedKg * daily, 0.047);
	close(p.nitrogenRecoveredKg * daily, 0.020);
	close(p.phosphorusRecoveredKg * daily, 0.008);
	close(p.potassiumRecoveredKg * daily, 0.012);
	close(p.mineralsRecoveredKg * daily, 0.007);
	close(p.recoveredWaterKg, 0); close(p.nitrifiedNitrogenKg, 0);
	close(p.residueProducedKg * daily, 1.0644);
	close(p.reactionHeatKJ * daily, 4712.4);
	close(p.lastReactionHeatKW, 0.05454166666666667);
	close(p.energySuppliedKJ * daily, 3600);
	close(p.heatRoutedKJ * daily, 8312.4);
	close(p.scale, 1); close(p.residualKg, 0); close(p.residualKJ, 0);
	require(p.lastStressors.empty(), "hot digestion suffered unexpected stress");
	close(snap.thermal.reservoirs.at(0).microbialEnergyKJ, p.heatRoutedKJ);
	// A second, unwired reservoir must not receive digestion heat.
	auto settings = doc.settings;
	// Smaller capacity makes a duplicate physical temperature booking visible
	// without cancellation error from subtracting almost-equal temperatures.
	settings.thermalReservoirs[0].heatCapacityKJPerK = 1000;
	auto unwired = settings.thermalReservoirs[0]; unwired.id = "unwired";
	settings.thermalReservoirs.push_back(unwired);
	auto const twoSinks = run(settings);
	close(twoSinks.thermal.reservoirs[0].microbialEnergyKJ, p.energySuppliedKJ + p.reactionHeatKJ);
	close(twoSinks.thermal.reservoirs[1].microbialEnergyKJ, 0);
	auto const& sink = twoSinks.thermal.reservoirs[0];
	close((sink.temperatureC - sink.startingTemperatureC) * sink.heatCapacityKJPerK,
		p.energySuppliedKJ + p.reactionHeatKJ);
	close(twoSinks.thermal.reservoirs[1].temperatureC, unwired.startingTemperatureC);
}
void oxygenStarvation(smoke::Context const& context)
{
	// Include nonzero nitrification AND dry-feed dilution so no derived
	// stream can escape uniform scaling behind a zero-valued assertion.
	auto settings = urine(context);
	settings.stores[0].composition->moisture = 0;
	settings.microbialProcessors[0].processWaterTargetMoisture = 0.6;
	auto const full = run(settings, 1).microbialProcessors.at(0);
	settings.stores[1].startingQualified = full.oxygenConsumedKg / 4;
	auto const snap = run(settings, 1);
	auto const& limited = snap.microbialProcessors.at(0);
	require(limited.limiter.find("oxygen") != std::string::npos
		&& limited.limiter.find("processOxygen") != std::string::npos, "missing named oxygen limiter");
	close(limited.scale, 0.25);
	for (auto field : {&core::StationMicrobialProcessorSnapshot::feedConsumedKg,
		&core::StationMicrobialProcessorSnapshot::oxygenConsumedKg,
		&core::StationMicrobialProcessorSnapshot::waterConsumedKg,
		&core::StationMicrobialProcessorSnapshot::consumablesUsed,
		&core::StationMicrobialProcessorSnapshot::nutrientProducedKg,
		&core::StationMicrobialProcessorSnapshot::co2ProducedKg,
		&core::StationMicrobialProcessorSnapshot::residueProducedKg,
		&core::StationMicrobialProcessorSnapshot::nitrogenRecoveredKg,
		&core::StationMicrobialProcessorSnapshot::phosphorusRecoveredKg,
		&core::StationMicrobialProcessorSnapshot::potassiumRecoveredKg,
		&core::StationMicrobialProcessorSnapshot::mineralsRecoveredKg,
		&core::StationMicrobialProcessorSnapshot::nitrifiedNitrogenKg,
		&core::StationMicrobialProcessorSnapshot::energySuppliedKJ,
		&core::StationMicrobialProcessorSnapshot::reactionHeatKJ,
		&core::StationMicrobialProcessorSnapshot::heatRoutedKJ})
	{
		require(full.*field > 0, "starvation coverage contains a zero stream");
		close(limited.*field, full.*field / 4);
	}
	close(limited.residualKg, 0); close(limited.residualKJ, 0);
	close(snap.thermal.reservoirs[0].microbialEnergyKJ, limited.heatRoutedKJ);
	// Wet feed exercises the remaining nonzero output: recovered water.
	settings.stores[0].composition->moisture = 0.5;
	auto wet = settings; wet.stores[1].startingQualified = 10;
	auto const wetFull = run(wet, 1).microbialProcessors.at(0);
	settings.stores[1].startingQualified = wetFull.oxygenConsumedKg / 4;
	auto const wetLimited = run(settings, 1).microbialProcessors.at(0);
	require(wetFull.recoveredWaterKg > 0, "wet starvation fixture is dry");
	close(wetLimited.recoveredWaterKg, wetFull.recoveredWaterKg / 4);
	close(wetLimited.scale, 0.25);
}
void validation(smoke::Context const& context)
{
	auto settings = urine(context);
	std::string error;
	auto validate = [&](auto const& s) { return core::validateStationSystemsSettings(s, error); };
	require(validate(settings), error.c_str());
	for (auto member : {&core::MicrobialProcessorSettings::organicOxidationFraction,
		&core::MicrobialProcessorSettings::nitrificationFraction,
		&core::MicrobialProcessorSettings::heatReleaseFraction,
		&core::MicrobialProcessorSettings::processWaterTargetMoisture})
		for (double invalid : {-0.1, 1.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			auto bad = settings; bad.microbialProcessors[0].*member = invalid;
			require(!validate(bad), "invalid derived fraction accepted");
		}
	for (auto member : {&core::MicrobialProcessorSettings::o2PerKgOrganicOxidized,
		&core::MicrobialProcessorSettings::o2PerKgNitrifiedN,
		&core::MicrobialProcessorSettings::co2PerKgOrganicOxidized,
		&core::MicrobialProcessorSettings::consumablePerKgOrganicOxidized})
		for (double invalid : {-0.1, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		{
			auto bad = settings; bad.microbialProcessors[0].*member = invalid;
			require(!validate(bad), "invalid derived rate accepted");
		}
	auto bad = settings; bad.microbialProcessors[0].rateBasis = static_cast<Basis>(42);
	require(!validate(bad), "invalid rate basis accepted");
	bad = settings; bad.microbialProcessors[0].processWaterTargetMoisture = 1;
	require(!validate(bad), "unit target moisture accepted");
	bad = settings; bad.stores[0].composition.reset();
	require(!validate(bad), "plain derived feed accepted");
	bad = settings; bad.microbialProcessors[0].thermalClass = "warm";
	require(!validate(bad), "derived mode bypassed thermal class agreement");
	bad = settings; bad.microbialProcessors[0].waterStoreId = bad.microbialProcessors[0].oxygenStoreId;
	require(!validate(bad), "derived mode accepted aliased streams");
	bad = settings; bad.stores[3].unit = "mol";
	require(!validate(bad), "derived medium used a non-mass unit");
	bad = settings; bad.microbialProcessors[0].co2PerKgOrganicOxidized = 1000;
	require(!validate(bad) && error.find("mass") != std::string::npos, "negative derived residue accepted");
	// Hidden fixed coefficients cannot reject or influence derived processing.
	auto ignored = settings;
	ignored.microbialProcessors[0].reactionHeatKJPerKgFeed = 1e100;
	ignored.microbialProcessors[0].oxygenPerKgFeedKg = -1;
	require(validate(ignored), error.c_str());
	close(run(ignored).microbialProcessors[0].oxygenConsumedKg,
		run(settings).microbialProcessors[0].oxygenConsumedKg);
}
void blend(smoke::Context const& context)
{
	auto settings = urine(context);
	// Real biological death deposits nutrient-rich, energy-depleted material
	// into a store whose declaration still contains organics (#601). No
	// illegal generic transfer across a composition boundary is used.
	auto crops = cropObservationFixture(2);
	crops.cropPopulations[0].respirationPerHour = 1e6;
	crops.microbialProcessors = settings.microbialProcessors;
	crops.microbialProcessors[0].feedStoreId = "waste";
	crops.microbialProcessors[0].processWaterTargetMoisture = 0.6;
	for (size_t i = 1; i < settings.stores.size(); ++i) crops.stores.push_back(settings.stores[i]);
	crops.thermalReservoirs = settings.thermalReservoirs;
	auto const snap = run(crops);
	auto const& p = snap.microbialProcessors[0];
	require(p.feedConsumedKg > 0, "drifted feed did not run");
	// All organic mass was respired before the non-respirable 0.8 kg
	// residue entered waste. The remaining N share is 0.5 / 0.8.
	close(p.oxygenConsumedKg, p.feedConsumedKg * (0.5 / 0.8) * 0.8 * 4.57);
	close(p.nitrifiedNitrogenKg, p.feedConsumedKg * (0.5 / 0.8) * 0.8);
	close(p.reactionHeatKJ, 0); close(p.co2ProducedKg, 0);
	close(p.consumablesUsed, 0); close(p.recoveredWaterKg, 0);
	close(p.waterConsumedKg, p.feedConsumedKg * 1.5);
	close(p.nitrogenRecoveredKg, p.nitrifiedNitrogenKg);
	close(p.phosphorusRecoveredKg, p.feedConsumedKg * (0.1 / 0.8) * 0.8);
	close(p.potassiumRecoveredKg, p.feedConsumedKg * (0.1 / 0.8) * 0.8);
	close(p.mineralsRecoveredKg, p.feedConsumedKg * (0.1 / 0.8) * 0.7);
	close(p.nutrientProducedKg, p.nitrogenRecoveredKg + p.phosphorusRecoveredKg
		+ p.potassiumRecoveredKg + p.mineralsRecoveredKg);
	close(p.residueProducedKg, p.feedConsumedKg + p.oxygenConsumedKg
		+ p.waterConsumedKg - p.nutrientProducedKg);
	close(p.heatRoutedKJ, p.energySuppliedKJ);
	close(p.residualKg, 0); close(p.residualKJ, 0);
	// Dry feed dilution uses moisture deficit; no legacy constant water draw.
	settings.stores[0].startingQualified = 10;
	settings.stores[0].composition->moisture = 0;
	settings.microbialProcessors[0].processWaterTargetMoisture = 0.6;
	auto const dry = run(settings).microbialProcessors[0];
	close(dry.waterConsumedKg, dry.feedConsumedKg * 1.5);
	close(dry.recoveredWaterKg, 0); close(dry.residualKg, 0);
}
void degenerate(smoke::Context const& context)
{
	core::StationConfigurationDocument doc;
	std::string error;
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/bioconversion-degenerate.yaml").string(), doc, &error), error.c_str());
	auto settings = doc.settings;
	// No zero-demand stream may require inventory or destination headroom.
	for (auto index : {1, 2, 3}) settings.stores[index].startingQualified = 0;
	for (auto index : {4, 6}) settings.stores[index].startingQualified = settings.stores[index].capacity;
	SourceScope restoreSource;
	core::setStationSystemsSourceLoader([] {
		auto source = core::bundledStationSystemsSource();
		auto const at = source.find("        self.ticks = self.ticks + 1");
		require(at != std::string::npos, "Lua assertion insertion point missing");
		source.insert(at, R"lua(
        local p = self.microbial_processors[1]
        assert(p.lastOxygenConsumed == 0 and p.lastCo2Produced == 0)
        assert(p.lastReactionHeatKW == 0 and p.lastConsumableUsed == 0)
        assert(p.lastNutrientProduced == 0 and p.lastNitrifiedNitrogenKg == 0)
        if p.waterRecovery == 0 then
            assert(p.lastFeedConsumed == 0 and p.lastRecoveredWaterProduced == 0)
            assert(p.lastPowerSupplied == 0 and p.lastResidueProduced == 0)
        else
            assert(p.lastFeedConsumed > 0 and p.lastRecoveredWaterProduced > 0)
        end
        assert(string.find(p.lastLimiter, 'carries no convertible content', 1, true))
)lua");
		return source;
	});
	auto const pass = run(settings).microbialProcessors[0];
	require(pass.feedConsumedKg > 0, "water pass-through stalled");
	close(pass.recoveredWaterKg, pass.feedConsumedKg * 0.999);
	close(pass.residueProducedKg, pass.feedConsumedKg * 0.001);
	close(pass.oxygenConsumedKg, 0); close(pass.co2ProducedKg, 0);
	close(pass.reactionHeatKJ, 0); close(pass.consumablesUsed, 0);
	close(pass.nutrientProducedKg, 0); close(pass.waterConsumedKg, 0);
	close(pass.nitrogenRecoveredKg, 0); close(pass.nitrifiedNitrogenKg, 0);
	close(pass.phosphorusRecoveredKg, 0); close(pass.potassiumRecoveredKg, 0);
	close(pass.mineralsRecoveredKg, 0);
	require(pass.lastStressors.empty(), "zero-demand stream caused stress");
	close(pass.residualKg, 0); close(pass.residualKJ, 0);
	// At a partial recovery fraction, unrecovered water remains in residue.
	settings.microbialProcessors[0].waterRecovery = 0.7;
	auto const partial = run(settings).microbialProcessors[0];
	close(partial.recoveredWaterKg, partial.feedConsumedKg * 0.999 * 0.7);
	close(partial.residueProducedKg, partial.feedConsumedKg - partial.recoveredWaterKg);
	close(partial.residualKg, 0);
	settings.microbialProcessors[0].waterRecovery = 0;
	settings.microbialProcessors[0].specificEnergyKWhPerKgFeed = 2;
	settings.solarArrays.clear();
	auto const idle = run(settings).microbialProcessors[0];
	close(idle.feedConsumedKg, 0); close(idle.recoveredWaterKg, 0);
	close(idle.residueProducedKg, 0); close(idle.energySuppliedKJ, 0);
	close(idle.residualKg, 0);
	require(idle.limiter == "feed 'nearlyWater' carries no convertible content: the stage has nothing to oxidize or recover",
		"zero-throughput no-conversion reason missing");
	settings.stores[0].composition->moisture = 0.999999;
	close(run(settings).microbialProcessors[0].feedConsumedKg, 0);
	settings.stores[0].composition.reset();
	require(!core::validateStationSystemsSettings(settings, error), "undeclared degenerate feed accepted");
	core::setStationSystemsSourceLoader({});
	// Physical scarcity still takes precedence over the explanatory reason
	// on a water pass-through; zero-demand destinations never bind it.
	auto blocked = doc.settings;
	blocked.stores[5].startingQualified = blocked.stores[5].capacity;
	auto const full = run(blocked).microbialProcessors[0];
	close(full.feedConsumedKg, 0);
	require(full.limiter.find("recovered water store") != std::string::npos,
		"no-conversion reason hid a full water destination");
	// Remaining truth-table rows: zero organics but recovered N; zero N;
	// energy-free organics; and moisture below, at and above the target.
	settings = urine(context);
	auto& c = *settings.stores[0].composition;
	c.nutrientNitrogen = 1; c.phosphorus = c.potassium = c.otherMinerals = 0;
	c.energyKJPerKgDry = 0;
	auto const mineral = run(settings).microbialProcessors[0];
	close(mineral.oxygenConsumedKg, mineral.nitrifiedNitrogenKg * 4.57);
	close(mineral.co2ProducedKg, 0); close(mineral.reactionHeatKJ, 0);
	close(mineral.consumablesUsed, 0); close(mineral.residualKg, 0);
	settings.microbialProcessors[0].nitrogenRecovery = 0;
	auto const unrecovered = run(settings).microbialProcessors[0];
	close(unrecovered.nitrifiedNitrogenKg, 0); close(unrecovered.oxygenConsumedKg, 0);
	c.nutrientNitrogen = 0;
	for (double moisture : {0.0, 0.6, 0.9})
	{
		c.moisture = moisture;
		settings.microbialProcessors[0].processWaterTargetMoisture = 0.6;
		auto const p = run(settings).microbialProcessors[0];
		require(p.feedConsumedKg > 0, "truth-table feed stalled");
		close(p.nitrifiedNitrogenKg, 0); close(p.reactionHeatKJ, 0);
		close(p.oxygenConsumedKg, p.feedConsumedKg * (1 - moisture) * 0.9 * 1.4);
		close(p.co2ProducedKg, p.feedConsumedKg * (1 - moisture) * 0.9 * 1.8);
		close(p.waterConsumedKg, p.feedConsumedKg * (moisture == 0 ? 1.5 : 0));
		close(p.recoveredWaterKg, p.feedConsumedKg * moisture * 0.7);
		close(p.residualKg, 0);
	}
}
void documents(smoke::Context const& context)
{
	core::StationConfigurationDocument doc; doc.settings = urine(context);
	auto& authored = doc.settings.microbialProcessors[0];
	authored.organicOxidationFraction = 0.4; authored.o2PerKgOrganicOxidized = 2;
	authored.nitrificationFraction = 0.4; authored.o2PerKgNitrifiedN = 3;
	authored.co2PerKgOrganicOxidized = 1; authored.processWaterTargetMoisture = 0.97;
	authored.heatReleaseFraction = 0.123; authored.consumablePerKgOrganicOxidized = 0.2;
	auto const bytes = core::serializeStationConfiguration(doc);
	core::StationConfigurationDocument restored; std::string error;
	require(core::parseStationConfiguration(bytes, restored, &error), error.c_str());
	require(restored.settings == doc.settings, "derived document round trip changed settings");
	core::World original("Derived files", 2, 2);
	original.pauseSimulation(); original.finishBuild();
	require(original.configureStationSystems(doc.settings, false, &error), error.c_str());
	original.resumeSimulation(); require(original.advanceTicks(60), "derived save rig failed");
	for (auto extension : {"derived.world.yaml", "derived.world"})
	{
		auto const path = context.temporaryRoot() / extension;
		original.saveTo(path.string());
		auto reopened = core::loadWorldDocument(path);
		reopened->pauseSimulation();
		require(reopened->getStationSystemsSettings() == doc.settings, "World derived settings changed on reload");
		close(reopened->getStationSystemsSnapshot().microbialProcessors[0].nitrifiedNitrogenKg, 0);
		reopened->resumeSimulation(); require(reopened->advanceTicks(60), "reloaded derived World failed");
		require(reopened->getStationSystemsSnapshot().microbialProcessors == original.getStationSystemsSnapshot().microbialProcessors,
			"derived World reload did not reproduce tick actuals");
	}
	auto node = YAML::Load(bytes);
	node["stationSystems"]["microbialProcessors"][0]["rateBasis"] = "invalid";
	require(!core::parseStationConfiguration(YAML::Dump(node), restored, &error), "unknown persisted basis accepted");
	// Explicit fixed mode is tick-identical to the same old document with
	// the new fields absent. Also exercise real World serialization/reload.
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/bioconversion-legacy.yaml").string(), doc, &error), error.c_str());
	node = YAML::Load(core::serializeStationConfiguration(doc));
	auto proc = node["stationSystems"]["microbialProcessors"][0];
	for (auto field : {"rateBasis", "organicOxidationFraction", "o2PerKgOrganicOxidized", "nitrificationFraction", "o2PerKgNitrifiedN", "co2PerKgOrganicOxidized", "processWaterTargetMoisture", "heatReleaseFraction", "consumablePerKgOrganicOxidized"}) proc.remove(field);
	require(core::parseStationConfiguration(YAML::Dump(node), restored, &error), error.c_str());
	require(restored.settings == doc.settings, "legacy default did not preserve settings");
	core::World a("LegacyA", 2, 2), b("LegacyB", 2, 2);
	for (auto* w : {&a, &b}) { w->pauseSimulation(); w->finishBuild(); }
	require(a.configureStationSystems(doc.settings, false, &error), error.c_str());
	require(b.configureStationSystems(restored.settings, false, &error), error.c_str());
	a.resumeSimulation(); b.resumeSimulation();
	for (int tick = 0; tick < 60; ++tick)
	{
		require(a.advanceTicks(1) && b.advanceTicks(1), "legacy anchor tick failed");
		require(a.getStationSystemsSnapshot().microbialProcessors == b.getStationSystemsSnapshot().microbialProcessors,
			"legacy anchor changed tick actuals");
	}
}
}

void registerBioconversionChecks(std::vector<smoke::Check>& checks)
{
	checks.insert(checks.end(), {{"bioconversionCarryover", carryover}, {"bioconversionTracer", tracer}, {"bioconversionValidation", validation},
		{"bioconversionBlend", blend}, {"bioconversionDocuments", documents}, {"bioconversionDegenerate", degenerate},
		{"bioconversionHotDigestion", hotDigestion}, {"bioconversionOxygenStarvation", oxygenStarvation}});
}
