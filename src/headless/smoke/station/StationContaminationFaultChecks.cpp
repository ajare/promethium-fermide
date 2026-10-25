#include "StationRig.h"
#include "Smoke.h"
#include "core/StationSystemsRuntime.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/YamlSerializer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
using namespace smoke::station;

core::StationSystemsSettings contaminationStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	auto store = [&](std::string id, double stock, bool compound, double moisture) {
		core::ResourceStoreSettings v;
		v.id = id; v.unit = "kg"; v.capacity = 100; v.startingQualified = stock;
		if (compound)
		{
			core::ResourceCompositionSettings c;
			c.moisture = moisture; c.otherMinerals = 1.0;
			v.composition = c;
		}
		s.stores.push_back(v);
	};
	store("raw", 50, true, 0.9);
	store("treated", 0, true, 0.9);
	store("concentrate", 0, true, 0.0);
	store("kit", 10, false, 0);
	store("hydrogen", 0, false, 0);
	store("oxygen", 0, false, 0);
	store("backup-hydrogen", 10, true, 0.9);
	store("backup-oxygen", 50, false, 0);
	store("product-water", 0, false, 0);
	core::WaterTreatmentSettings t;
	t.id = "filter"; t.sourceStoreId = "raw"; t.treatedStoreId = "treated";
	t.concentrateStoreId = "concentrate"; t.consumableStoreId = "kit";
	t.throughputKgPerS = 0.1; t.breakthrough = true;
	s.waterTreatments = {t};
	core::ThermalReservoirSettings sink;
	sink.id = "sink"; sink.thermalClass = "warm"; sink.heatCapacityKJPerK = 1e6;
	sink.startingTemperatureC = 20; sink.minTemperatureC = -50; sink.maxTemperatureC = 300;
	s.thermalReservoirs = {sink};
	core::SolarArraySettings sun; sun.id = "sun"; sun.capacityKW = 1000;
	s.solarArrays = {sun};
	core::ElectrolyzerSettings e;
	e.id = "electrolyzer"; e.waterStoreId = "treated"; e.hydrogenStoreId = "hydrogen";
	e.oxygenStoreId = "oxygen"; e.thermalClass = "warm"; e.heatSinkId = "sink";
	e.rateKgPerS = 0.01; e.catalystPoisoning = core::CatalystPoisoningSettings{0, 360};
	s.electrolyzers = {e};
	core::FuelCellSettings f;
	f.id = "backup"; f.hydrogenStoreId = "backup-hydrogen"; f.oxygenStoreId = "backup-oxygen";
	f.waterStoreId = "product-water"; f.thermalClass = "warm"; f.heatSinkId = "sink";
	f.ratedOutputKW = 10; f.commandedOutputKW = 10;
	f.catalystPoisoning = core::CatalystPoisoningSettings{0, 360};
	s.fuelCells = {f};
	return s;
}

core::StationSystemsStoreSnapshot storeOf(core::StationSystemsSnapshot const& s, std::string const& id)
{
	for (auto const& store : s.stores) if (store.id == id) return store;
	throw std::runtime_error("missing store " + id);
}

template<class Serializer>
std::string bytes(core::StationSystemsRuntime const& run)
{
	auto out = Serializer::toString();
	out->beginMap("root"); run.writeFaultReplay(*out); out->endMap(); out->serialize();
	return out->getSerializedString();
}

template<class Serializer>
void replay(core::StationSystemsRuntime& run)
{
	auto const saved = bytes<Serializer>(run);
	auto in = Serializer::fromString(saved); in->deserialize(); in->beginMap("root");
	auto restored = core::StationSystemsRuntime::readFaultReplay(*in, run.effectiveSettings(), 0.25f);
	in->endMap();
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"contamination snapshot/events replay diverged");
	require(bytes<Serializer>(*restored) == saved, "contamination replay bytes diverged");
	auto const until = run.snapshot().elapsedSeconds + 1.0;
	require(restored->advanceTo(until) && run.advanceTo(until), "continuation failed");
	require(restored->snapshot() == run.snapshot() && restored->events() == run.events(),
		"contamination continuation diverged");
}

void contaminationChain(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = contaminationStation();
	core::StationSystemsRuntime run(s, 0.25f);
	if (!run.advanceTo(0.25)) throw std::runtime_error("clean baseline failed: "
		+ (run.diagnostics().empty() ? std::string("no diagnostic") : run.diagnostics().back().message));
	auto before = run.snapshot();
	require(before.electrolyzers[0].activeFaults.empty() && before.electrolyzers[0].lastSplitKg > 0,
		"clean electrolyzer feed poisoned catalyst");
	require(before.fuelCells[0].activeFaults.size() == 1,
		"independently contaminated hydrogen failed to poison fuel cell");
	require(run.injectFault("filter", "breakthrough", {core::FaultCause::Kind::AgentAction, "agent:846"}),
		"breakthrough injection failed");
	require(run.snapshot().waterTreatments[0].activeFaults.empty(), "pending breakthrough published");
	require(!run.injectFault("filter", "breakthrough", {}) && !run.injectFault("backup", "breakthrough", {})
		&& !run.injectFault("electrolyzer", "catalyst-poisoning", {}), "invalid or duplicate injection accepted");
	require(run.advanceTo(0.5), "contamination drive failed");
	auto after = run.snapshot();
	auto const& t = after.waterTreatments[0];
	require(after.healthy && t.activeFaults.size() == 1 && t.activeFaults[0].derivedImpact == 1
		&& t.activeFaults[0].causeDetail == "agent:846", "breakthrough fault attribution/impact missing");
	require(std::abs(t.lastProcessedKg - t.lastTreatedKg) < 1e-12 && t.lastConcentrateKg < 1e-12
		&& !t.qualityAccepted, "breakthrough stream split/qualification wrong");
	require(storeOf(after, "treated").currentUnqualified > storeOf(before, "treated").currentUnqualified
		&& storeOf(after, "concentrate").currentQualified == storeOf(before, "concentrate").currentQualified,
		"concentrate not redirected as held material");
	auto const& e = after.electrolyzers[0];
	require(e.waterQuality.totalDissolved > 0 && e.lastSplitKg == 0 && e.activeFaults.size() == 1
		&& e.activeFaults[0].causeKind == core::FaultCause::Kind::FeedContamination
		&& e.activeFaults[0].causeDetail == "treated", "downstream composition did not close quality gate/poison catalyst");
	require(run.advanceTo(2), "ongoing poisoning drive failed");
	after = run.snapshot();
	require(after.electrolyzers[0].wearFraction > e.wearFraction
		&& after.fuelCells[0].wearFraction > before.fuelCells[0].wearFraction,
		"exposure failed to accumulate permanent Wear");
	require(after.electrolyzers[0].effectiveRateKgPerS < s.electrolyzers[0].rateKgPerS
		&& after.fuelCells[0].effectiveRatedOutputKW < s.fuelCells[0].ratedOutputKW,
		"permanent Wear did not reduce effective performance ratings");
	require(after.electrolyzers[0].activeFaults[0].derivedImpact == after.electrolyzers[0].wearFraction
		&& after.fuelCells[0].activeFaults[0].derivedImpact == after.fuelCells[0].wearFraction,
		"poisoning impact is not derived from Wear");
	auto const events = run.events();
	require(std::count_if(events.begin(), events.end(), [](auto const& event) {
		return event.category == "fault" && (event.message.find("breakthrough") != std::string::npos
			|| event.message.find("catalyst-poisoning") != std::string::npos);
	}) == 3, "fault onsets missing or repeated");
	// Removing exposure stops further wear; existing fade and Faults remain.
	require(run.updateNode<core::ElectrolyzerSettings>("electrolyzer", [](auto& v) {
		v.catalystPoisoning->triggerConcentration = 0.9;
	}) && run.updateNode<core::FuelCellSettings>("backup", [](auto& v) {
		v.commandedOutputKW = 0;
	}), "exposure retune failed");
	auto const ew = after.electrolyzers[0].wearFraction, fw = after.fuelCells[0].wearFraction;
	require(run.advanceTo(3) && run.snapshot().electrolyzers[0].wearFraction == ew
		&& run.snapshot().fuelCells[0].wearFraction == fw, "Wear healed or continued without exposure");
	replay<core::YamlSerializer>(run); replay<core::BinarySerializer>(run);
}

void contaminationContracts(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = contaminationStation();
	std::string diagnostic;
	auto const valid = core::validateStationSystemsSettings(s, diagnostic);
	require(valid, diagnostic.c_str());
	for (double rate : {0.0, -1.0, std::numeric_limits<double>::infinity()})
	{
		auto bad = s; bad.electrolyzers[0].catalystPoisoning->wearPerHour = rate;
		require(!core::validateStationSystemsSettings(bad, diagnostic), "invalid poison Wear rate accepted");
	}
	for (bool fuel : {false, true})
	{
		core::StationSystemsRuntime tainted(s, 0.25f);
		require(tainted.advanceTo(0.25), "invalid live-rate setup failed");
		auto const before = tainted.snapshot();
		auto const events = tainted.events();
		require(tainted.injectFault("filter", "breakthrough", {}), "rollback breakthrough setup failed");
		require(fuel ? tainted.updateNode<core::FuelCellSettings>("backup", [](auto& c) {
			c.catalystPoisoning->wearPerHour = -360;
		}) : tainted.updateNode<core::ElectrolyzerSettings>("electrolyzer", [](auto& c) {
			c.catalystPoisoning->triggerConcentration = std::numeric_limits<double>::quiet_NaN();
		}), "invalid live-rate edit refused before audited boundary");
		require(!tainted.advanceTo(0.5) && !tainted.snapshot().healthy
			&& tainted.snapshot().electrolyzers[0].wearFraction == before.electrolyzers[0].wearFraction
			&& tainted.snapshot().fuelCells[0].wearFraction == before.fuelCells[0].wearFraction,
			"invalid live poison rate healed Wear or committed a partial tick");
		// A failed boundary may append its diagnostic event, but no second
		// onset or partial booking from that boundary may be published.
		require(tainted.snapshot().waterTreatments[0] == before.waterTreatments[0],
			"failed poison audit did not roll back treatment state");
		auto const afterEvents = tainted.events();
		auto const isFault = [](auto const& event) { return event.category == "fault"; };
		require(std::count_if(events.begin(), events.end(), isFault)
			== std::count_if(afterEvents.begin(), afterEvents.end(), isFault), "failed tick published a Fault event");
	}
	auto bad = s; bad.stores[1].composition.reset();
	require(!core::validateStationSystemsSettings(bad, diagnostic), "breakthrough into untracked species accepted");
	bad = s; bad.waterTreatments[0].id = "sun";
	require(!core::validateStationSystemsSettings(bad, diagnostic), "ambiguous fault id accepted");
	core::StationSystemsRuntime run(s, 0.25f);
	if (!run.advanceTo(0.25)) throw std::runtime_error("autonomous drive failed: "
		+ (run.diagnostics().empty() ? std::string("no diagnostic") : run.diagnostics().back().message));
	require(run.hasFaultReplayState(), "autonomous poisoning replay state absent");
	replay<core::YamlSerializer>(run); replay<core::BinarySerializer>(run);
	auto changed = s; changed.waterTreatments[0].breakthrough = false;
	require(!run.updateSettings(changed, &diagnostic), "whole settings changed frozen opt-in");
	require(!run.updateNode<core::FuelCellSettings>("backup", [](auto& c) { c.catalystPoisoning.reset(); }),
		"targeted edit changed frozen poison opt-in");
	core::StationSystemsRuntime pending(s, 0.25f);
	require(pending.injectFault("filter", "breakthrough", {}), "pending replay setup failed");
	replay<core::YamlSerializer>(pending); replay<core::BinarySerializer>(pending);
	core::StationConfigurationDocument document; document.settings = s;
	auto yaml = core::serializeStationConfiguration(document);
	require(yaml.find("schemaVersion: 119") != std::string::npos, "contamination schema tier absent");
	core::StationConfigurationDocument parsed;
	require(core::parseStationConfiguration(yaml, parsed, &diagnostic) && parsed.settings == s,
		"contamination configuration round trip failed");
	// New settings round-trip and older schema guard through the public map seam.
	auto out = core::YamlSerializer::toString();
	out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
	for (auto version : {118u, 119u})
	{
		auto in = core::YamlSerializer::fromString(out->getSerializedString());
		in->deserialize(); in->beginMap("root");
		core::StationSystemsSettings result;
		bool refused = false;
		try { core::readStationSystemsSettingsMap(*in, version, result); }
		catch (core::SerializationException const&) { refused = true; }
		require(version == 118 ? refused : !refused && result == s, "contamination schema contract failed");
	}
}

void contaminationOptIn(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto enabled = contaminationStation();
	enabled.stores[0].composition.reset(); // pure raw water
	enabled.stores[6].composition.reset(); // pure hydrogen
	auto absent = enabled;
	absent.waterTreatments[0].breakthrough = false;
	absent.electrolyzers[0].catalystPoisoning.reset();
	absent.fuelCells[0].catalystPoisoning.reset();
	core::StationSystemsRuntime a(enabled, 0.25f), b(absent, 0.25f);
	require(!b.injectFault("filter", "breakthrough", {}), "absent breakthrough opt-in accepted injection");
	for (double until : {0.25, 1.0, 2.0})
	{
		require(a.advanceTo(until) && b.advanceTo(until), "opt-in baseline drive failed");
		require(a.snapshot() == b.snapshot() && a.events() == b.events(),
			"unused contamination opt-ins changed legacy behavior");
	}
	require(!b.hasFaultReplayState(), "absent opt-ins emitted replay state");
	core::StationConfigurationDocument doc; doc.settings = absent;
	auto const yaml = core::serializeStationConfiguration(doc);
	require(yaml.find("schemaVersion: 111") != std::string::npos
		&& yaml.find("breakthrough") == std::string::npos
		&& yaml.find("catalystPoisoning") == std::string::npos, "absent opt-ins changed legacy wire tier/fields");
	core::StationConfigurationDocument restored;
	std::string diagnostic;
	require(core::parseStationConfiguration(yaml, restored, &diagnostic)
		&& core::serializeStationConfiguration(restored) == yaml, "legacy configuration bytes changed on round trip");
	for (auto ratio : {1u, 4u, 6u})
	{
		auto s = contaminationStation(); s.systemsStepWorldTicks = ratio;
		core::StationSystemsRuntime run(s, 0.25f);
		require(run.injectFault("filter", "breakthrough", {}) && run.advanceTo(3),
			"contamination failed at supported step ratio");
		require(run.snapshot().electrolyzers[0].wearFraction > 0
			&& run.snapshot().fuelCells[0].wearFraction > 0, "poisoning missing at supported step ratio");
	}
}

void contaminationWearPerformance(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto s = contaminationStation();
	core::ResourceStoreSettings clean;
	clean.id = "clean"; clean.unit = "kg"; clean.capacity = 100; clean.startingQualified = 50;
	s.stores.push_back(clean);
	core::ResourceTransferSettings flush;
	flush.id = "flush"; flush.sourceId = "clean"; flush.destinationId = "treated"; flush.unit = "kg";
	flush.ratePerSecond = 1; flush.mode = core::ResourceTransferMode::Scheduled;
	flush.quantity = 1; flush.firstOccurrenceSeconds = 1.0;
	s.transfers = {flush};
	core::WasteEjectionSettings purge;
	purge.id = "purge"; purge.storeId = "treated"; purge.unit = "kg"; purge.quantity = 100;
	purge.firstOccurrenceSeconds = 0.75;
	purge.eligibility = core::WasteEjectionEligibility::QualifiedAndHeld;
	s.wasteEjections = {purge};
	core::StationSystemsRuntime run(s, 0.25f);
	require(run.advanceTo(0.25) && run.injectFault("filter", "breakthrough", {})
		&& run.advanceTo(0.5), "Wear/flush setup failed");
	auto const worn = run.snapshot().electrolyzers[0].wearFraction;
	require(worn > 0 && worn < 1, "poisoning did not leave a partial fade");
	require(run.updateNode<core::WaterTreatmentSettings>("filter", [](auto& t) { t.throughputKgPerS = 0; })
		&& run.advanceTo(1.25), "clean flush failed");
	auto const cell = run.snapshot().electrolyzers[0];
	require(cell.waterQuality.totalDissolved == 0 && cell.lastSplitKg > 0,
		"quality did not recover after conservative purge/clean refill");
	require(cell.wearFraction >= worn && cell.activeFaults.size() == 1
		&& std::abs(cell.lastSplitKg - 0.25 * cell.effectiveRateKgPerS) < 1e-12
		&& cell.lastSplitKg < 0.25 * cell.rateKgPerS,
		"clean feed healed Wear or bypassed faded throughput");
	replay<core::YamlSerializer>(run); replay<core::BinarySerializer>(run);
}
}
void registerStationContaminationFaultChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"contaminationChain", contaminationChain});
	checks.push_back({"contaminationContracts", contaminationContracts});
	checks.push_back({"contaminationWearPerformance", contaminationWearPerformance});
	checks.push_back({"contaminationOptIn", contaminationOptIn});
}
