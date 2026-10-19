#include "StationWaterChecks.h"
#include "core/World.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationConnections.h"
#include "core/AgentTagRegistryDocument.h"
#include <algorithm>
#include <format>
#include <cmath>
#include <limits>

namespace
{
using smoke::require;
struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } };
void close(double actual, double expected)
{
	require(std::abs(actual - expected) < 1e-8 * std::max(1.0, std::abs(expected)),
		std::format("compound water numeric mismatch: {:.17g} expected {:.17g}", actual, expected).c_str());
}
core::StationSystemsSettings scenario(smoke::Context const& context, char const* name, bool partial = false)
{
	core::StationConfigurationDocument document;
	std::string error;
	auto const loaded = core::loadStationConfiguration(context.fixture(std::string("resources/station-systems/scenarios/")
		+ name).string(), document, &error);
	require(loaded, error.c_str());
	if (!partial && std::string(name) == "water-greywater.yaml")
	{
		document.settings.waterTreatments[0].removalFractions.clear();
		document.settings.stores[1].composition.reset();
		document.settings.crews.clear(); // legacy/full-removal rig has no potable probe
	}
	return document.settings;
}
core::StationSystemsSnapshot run(core::StationSystemsSettings const& settings, int ticks = 60)
{
	core::World world("Compound water", 2, 2);
	world.pauseSimulation(); world.finishBuild();
	std::string error;
	auto const configured = world.configureStationSystems(settings, false, &error);
	require(configured, error.c_str());
	world.resumeSimulation();
	if (!world.advanceTicks(ticks))
	{
		for (auto const& d : world.getStationSystemsDiagnostics()) error += d.message;
		require(false, error.c_str());
	}
	auto snapshot = world.getStationSystemsSnapshot();
	require(snapshot.healthy, "compound water became unhealthy");
	for (auto const& balance : snapshot.constituentBalances) close(balance.residual, 0);
	return snapshot;
}
auto const& store(core::StationSystemsSnapshot const& snapshot, char const* id)
{
	auto const found = std::find_if(snapshot.stores.begin(), snapshot.stores.end(),
		[&](auto const& s) { return s.id == id; });
	require(found != snapshot.stores.end(), "missing water store");
	return *found;
}
void tracer(smoke::Context const& context)
{
	auto settings = scenario(context, "water-greywater.yaml");
	SourceScope restore;
	core::setStationSystemsSourceLoader([] {
		auto source = core::bundledStationSystemsSource();
		auto at = source.find("        self.ticks = self.ticks + 1");
		require(at != std::string::npos, "Lua water assertion point missing");
		source.insert(at, R"lua(
        local s, c = self.stores[1], self.stores[3]
        assert(math.abs(s.concentrations.dryOrganics - 0.002) < 1e-10)
        assert(math.abs(c.actualConstituents.dryOrganics
            - self.water_treatments[1].processedTotal * 0.002) < 1e-10)
        assert(math.abs(c.concentrations.dryOrganics - 0.002 / 0.1517) < 1e-10)
)lua");
		return source;
	});
	auto const snapshot = run(settings);
	auto const& t = snapshot.waterTreatments.at(0);
	auto const daily = 86400.0 / (60.0 * core::World::getFixedTimestep());
	close(t.processedKg * daily, 100);
	close(t.treatedKg * daily, 84.83);
	close(t.concentrateKg * daily, 15.17);
	close(store(snapshot, "concentrate").actualConstituents.at("dryOrganics") * daily, 0.2);
	close(store(snapshot, "concentrate").concentrations.at("dryOrganics"), 0.2 / 15.17);
}
void partialRemoval(smoke::Context const& context)
{
	auto settings = scenario(context, "water-greywater.yaml", true);
	settings.crews.clear(); // isolate treatment inventory conservation
	SourceScope restore;
	core::setStationSystemsSourceLoader([] {
		auto source = core::bundledStationSystemsSource();
		auto at = source.find("        self.ticks = self.ticks + 1");
		require(at != std::string::npos, "Lua partial assertion point missing");
		source.insert(at, R"lua(
        local t = self.water_treatments[1]
        assert(t.removalFractions.dryOrganics == 0.95)
        assert(math.abs(t.removedConstituents.dryOrganics - t.processedTotal * 0.0019) < 1e-10)
        assert(math.abs(t.passedConstituents.dryOrganics - t.processedTotal * 0.0001) < 1e-10)
)lua");
		return source;
	});
	auto const snapshot = run(settings);
	auto const daily = 86400.0 / (60.0 * core::World::getFixedTimestep());
	auto const& t = snapshot.waterTreatments[0];
	close(t.treatedKg * daily, 84.84); close(t.concentrateKg * daily, 15.16);
	close(store(snapshot, "treated").concentrations.at("dryOrganics"), 0.01 / 84.84);
	close(store(snapshot, "concentrate").concentrations.at("dryOrganics"), 0.19 / 15.16);
	require(t.removalFractions == settings.waterTreatments[0].removalFractions, "snapshot lost removals");
	core::setStationSystemsSourceLoader({});
	// Lower removal or greater incoming load raises effluent concentration.
	auto lighter = settings; lighter.waterTreatments[0].removalFractions["dryOrganics"] = 0.5;
	require(store(run(lighter), "treated").concentrations.at("dryOrganics")
		> store(snapshot, "treated").concentrations.at("dryOrganics"), "lighter removal did not overwhelm effluent");
	auto heavier = settings; heavier.stores[0].composition->moisture = 0.99;
	require(store(run(heavier), "treated").concentrations.at("dryOrganics")
		> store(snapshot, "treated").concentrations.at("dryOrganics"), "heavier load did not overwhelm effluent");
	// Plain inflow changes the actual blend: partial selectivity must follow it.
	auto diluted = settings;
	auto fresh = diluted.stores[1]; fresh.id = "fresh"; fresh.composition.reset();
	fresh.startingQualified = 100; diluted.stores.push_back(fresh);
	core::ResourceTransferSettings inflow;
	inflow.id = "dilute"; inflow.sourceId = "fresh"; inflow.destinationId = "greywater";
	inflow.unit = "kg"; inflow.ratePerSecond = 10; diluted.transfers = {inflow};
	require(store(run(diluted), "treated").concentrations.at("dryOrganics")
		< store(snapshot, "treated").concentrations.at("dryOrganics"), "partial removal ignored live dilution");
	// Arbitrary species, gram units and chemical energy: every tick closes.
	auto& c = *settings.stores[0].composition;
	c.moisture = 0.8; c.nutrientNitrogen = 0.1; c.atmosphericNitrogen = 0.1;
	c.phosphorus = 0.1; c.potassium = 0.1; c.otherMinerals = 0.1;
	settings.stores[0].unit = "g"; settings.stores[0].capacity *= 1000;
	settings.stores[0].startingQualified *= 1000;
	for (double fraction : {0.0, 0.13, 0.5, 0.99, 1.0})
	{
		for (size_t i = 0; i < core::StationWaterDryConstituents.size(); ++i)
			settings.waterTreatments[0].removalFractions[core::StationWaterDryConstituents[i]] =
				fraction == 0 || fraction == 1 ? fraction : std::fmod(fraction + i * 0.17, 1.0);
		core::World world("Partial conservation", 2, 2);
		world.pauseSimulation(); world.finishBuild(); std::string error;
		require(world.configureStationSystems(settings, false, &error), error.c_str());
		world.resumeSimulation();
		auto before = world.getStationSystemsSnapshot();
		for (int tick = 0; tick < 60; ++tick)
		{
			require(world.advanceTicks(1), "partial treatment tick failed");
			auto after = world.getStationSystemsSnapshot();
			auto const& treatment = after.waterTreatments[0];
			for (auto const& [name, initial] : store(before, "greywater").actualConstituents)
			{
				auto const drawn = initial - store(after, "greywater").actualConstituents.at(name);
				auto const passed = treatment.lastPassedConstituents.at(name);
				auto const removed = treatment.lastRemovedConstituents.at(name);
				close(drawn, passed + removed);
				close(store(after, "treated").actualConstituents.at(name)
					- store(before, "treated").actualConstituents.at(name), passed);
				close(store(after, "concentrate").actualConstituents.at(name)
					- store(before, "concentrate").actualConstituents.at(name), removed);
				if (fraction == 0 && name != "water") close(removed, 0);
			}
			for (auto const& balance : after.constituentBalances) close(balance.residual, 0);
			before = std::move(after);
		}
	}
}
void partialValidation(smoke::Context const& context)
{
	auto base = scenario(context, "water-greywater.yaml", true);
	base.crews.clear(); // isolate treatment headroom and constituent paths
	std::string reason;
	for (double value : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN()})
	{
		auto settings = base; settings.waterTreatments[0].removalFractions["dryOrganics"] = value;
		require(!core::validateStationSystemsSettings(settings, reason)
			&& reason.find("dryOrganics") != std::string::npos, "invalid removal fraction accepted");
	}
	for (std::string const name : {"unobtainium", "water", "energy"})
	{
		auto settings = base; settings.waterTreatments[0].removalFractions[name] = 0.5;
		require(!core::validateStationSystemsSettings(settings, reason)
			&& reason.find(name) != std::string::npos, "unknown removal key accepted");
		core::StationConfigurationDocument doc, restored; doc.settings = settings;
		require(!core::parseStationConfiguration(core::serializeStationConfiguration(doc), restored, &reason)
			&& reason.find(name) != std::string::npos, "YAML silently dropped unknown removal key");
	}
	auto plain = base; plain.stores[1].composition.reset();
	require(!core::validateStationSystemsSettings(plain, reason)
		&& reason.find("dryOrganics") != std::string::npos, "partial load entered plain treated store");
	plain.waterTreatments[0].removalFractions["dryOrganics"] = 0;
	require(!core::validateStationSystemsSettings(plain, reason), "all-zero removal bypassed treated boundary");
	plain.stores[1].composition = base.stores[1].composition;
	plain.stores[2].composition.reset();
	require(core::validateStationSystemsSettings(plain, reason), reason.c_str());
	auto const passed = run(plain);
	close(store(passed, "treated").actualConstituents.at("dryOrganics"), passed.waterTreatments[0].processedKg * 0.002);
	// Headroom must use loaded effluent mass rather than recovered water alone.
	auto limited = base; limited.stores[1].capacity = 0.0001;
	auto const result = run(limited);
	close(store(result, "treated").currentQualified, limited.stores[1].capacity);
	require(result.waterTreatments[0].limiter.find("treated water") != std::string::npos,
		"loaded effluent headroom did not throttle");
}
void blend(smoke::Context const& context)
{
	auto settings = scenario(context, "water-greywater.yaml");
	// Exercise every existing species and chemical energy, with a destination
	// declaration deliberately unrelated to the actual concentrated blend.
	auto& c = *settings.stores[0].composition;
	c.moisture = 0.8; c.nutrientNitrogen = 0.1; c.atmosphericNitrogen = 0.1;
	c.phosphorus = 0.1; c.potassium = 0.1; c.otherMinerals = 0.1;
	settings.stores[1].composition = c;
	auto const before = run(settings, 0);
	auto const after = run(settings);
	for (auto const& [name, initial] : store(before, "greywater").actualConstituents)
	{
		auto const drawn = initial - store(after, "greywater").actualConstituents.at(name);
		auto const treated = store(after, "treated").actualConstituents.at(name);
		auto const concentrate = store(after, "concentrate").actualConstituents.at(name);
		close(drawn, treated + concentrate);
		if (name != "water") close(treated, 0);
	}
	auto grams = settings;
	grams.stores[0].unit = "g"; grams.stores[0].capacity *= 1000;
	grams.stores[0].startingQualified *= 1000;
	auto const g = run(grams);
	for (auto const& [name, mass] : store(after, "concentrate").actualConstituents)
		close(store(g, "concentrate").actualConstituents.at(name), mass);
	// Plain inflow dilutes without creating dry mass or chemical energy.
	settings.waterTreatments[0].throughputKgPerS = 0;
	auto fresh = settings.stores[1]; fresh.id = "fresh"; fresh.composition.reset();
	fresh.startingQualified = 100; settings.stores.push_back(fresh);
	core::ResourceTransferSettings transfer;
	transfer.id = "dilute"; transfer.sourceId = "fresh";
	transfer.destinationId = "greywater"; transfer.unit = "kg"; transfer.ratePerSecond = 1;
	settings.transfers = {transfer};
	for (bool scheduled : {false, true})
	{
		settings.transfers[0].mode = scheduled ? core::ResourceTransferMode::Scheduled : core::ResourceTransferMode::Continuous;
		settings.transfers[0].quantity = 1;
		auto const diluted = run(settings);
		for (auto const& [name, initial] : store(before, "greywater").actualConstituents)
			if (name != "water") close(store(diluted, "greywater").actualConstituents.at(name), initial);
		close(store(diluted, "greywater").concentrations.at("nutrientNitrogen"), 4.0 / 201.0);
	}
	// Re-enable treatment: its output must use that diluted blend, not c.
	settings.transfers[0].mode = core::ResourceTransferMode::Continuous;
	settings.waterTreatments[0].throughputKgPerS = 1;
	auto const mixed = run(settings, 120);
	require(store(mixed, "greywater").concentrations.at("nutrientNitrogen") < 0.02,
		"water draw ignored dilution");
}
void routing(smoke::Context const& context)
{
	auto settings = scenario(context, "water-greywater.yaml");
	auto buffer = settings.stores[2]; buffer.id = "disposal";
	settings.stores.push_back(buffer);
	core::ResourceTransferSettings transfer;
	transfer.id = "routeConcentrate"; transfer.sourceId = "concentrate";
	transfer.destinationId = "disposal"; transfer.unit = "kg"; transfer.ratePerSecond = 1;
	settings.transfers = {transfer};
	core::WasteEjectionSettings ejection;
	ejection.id = "eject"; ejection.storeId = "disposal"; ejection.unit = "kg";
	ejection.quantity = 1; ejection.firstOccurrenceSeconds = 0.5;
	settings.wasteEjections = {ejection};
	auto const routed = run(settings);
	close(store(routed, "concentrate").currentQualified, 0);
	require(store(routed, "disposal").currentQualified > 0
		&& routed.wasteEjections[0].ejectedTotal > 0, "compound concentrate not routable/disposable");
	close(store(routed, "disposal").concentrations.at("dryOrganics"), 0.2 / 15.17);
	// Plain-source deposits into compound outlets must never synthesize
	// dry matter from either destination's seeded declaration.
	settings.transfers.clear(); settings.wasteEjections.clear();
	settings.stores[0].composition.reset();
	settings.stores[1].composition = buffer.composition;
	auto const pure = run(settings);
	close(store(pure, "treated").concentrations.at("dryOrganics"), 0);
	close(store(pure, "concentrate").concentrations.at("dryOrganics"), 0);
	close(store(pure, "treated").concentrations.at("water"), 1);
}
void connections(smoke::Context const& context)
{
	for (std::string const role : {"Raw water", "Treated water", "Concentrate"})
	{
		auto settings = scenario(context, "water-greywater.yaml");
		settings.stores[1].composition = settings.stores[0].composition;
		bool found = false;
		for (auto const& link : core::station_connections::build(settings).links)
			if (link.designKey.ownerType == "Water treatment" && link.designKey.relation == role)
			{
				settings.connections[link.designKey].efficiency = 0.5;
				found = true;
			}
		require(found, "missing treatment connection");
		auto const snapshot = run(settings);
		close(store(snapshot, "concentrate").concentrations.at("dryOrganics"), 0.2 / 15.17);
		close(store(snapshot, "treated").concentrations.at("dryOrganics"), 0);
	}
}
void validation(smoke::Context const& context)
{
	auto const base = scenario(context, "water-greywater.yaml");
	std::string reason;
	for (bool missing : {false, true})
	{
		auto settings = base;
		if (missing) settings.waterTreatments[0].concentrateStoreId.clear();
		else settings.stores[2].composition.reset();
		require(!core::validateStationSystemsSettings(settings, reason), "loaded treatment lost its concentrate");
		require(reason.find("dryOrganics") != std::string::npos, "refusal did not name constituent");
	}
	for (bool reverse : {false, true})
	{
		auto settings = base;
		core::ResourceTransferSettings transfer;
		transfer.id = "launder"; transfer.sourceId = reverse ? "treated" : "greywater";
		transfer.destinationId = reverse ? "greywater" : "treated";
		transfer.unit = "kg"; transfer.ratePerSecond = 1;
		settings.transfers = {transfer};
		if (!reverse) require(!core::validateStationSystemsSettings(settings, reason)
			&& reason.find("compound-material boundary") != std::string::npos, "compound to plain laundering accepted");
		else require(core::validateStationSystemsSettings(settings, reason), reason.c_str());
	}
	auto mismatch = base; mismatch.stores[2].hazardClass = "hazardous";
	require(!core::validateStationSystemsSettings(mismatch, reason)
		&& reason.find("hazard") != std::string::npos, "compound treatment crossed hazard classes");
	auto dry = base; dry.stores[0].composition->moisture = 0;
	auto const stopped = run(dry, 1);
	close(stopped.waterTreatments[0].processedKg, 0);
	require(stopped.waterTreatments[0].limiter == "stream carries no water", "missing dry-stream limiter");
	auto full = base; full.stores[2].startingUnqualified = full.stores[2].capacity;
	close(run(full, 1).waterTreatments[0].processedKg, 0);
	// Pure scalar feed at 100% recovery needs no concentrate headroom.
	auto pure = scenario(context, "water-legacy-anchor.yaml");
	pure.waterTreatments[0].recoveryRatio = 1;
	pure.stores[2].startingUnqualified = pure.stores[2].capacity;
	auto const result = run(pure);
	close(result.waterTreatments[0].concentrateKg, 0);
	require(result.waterTreatments[0].processedKg > 0, "pure treatment stalled");
}
void documents(smoke::Context const& context)
{
	auto const settings = scenario(context, "water-greywater.yaml", true);
	core::StationConfigurationDocument document; document.settings = settings;
	core::StationConfigurationDocument restored; std::string error;
	auto const parsed = core::parseStationConfiguration(core::serializeStationConfiguration(document), restored, &error);
	require(parsed, error.c_str());
	require(restored.settings == settings, "compound water document changed on round trip");
	core::World original("Compound water files", 2, 2);
	original.pauseSimulation(); original.finishBuild();
	auto const configured = original.configureStationSystems(settings, false, &error);
	require(configured, error.c_str());
	original.resumeSimulation(); require(original.advanceTicks(60), "compound save rig failed");
	auto const expected = original.getStationSystemsSnapshot();
	for (auto const* name : {"compound-water.world.yaml", "compound-water.world"})
	{
		auto const path = context.temporaryRoot() / name;
		original.saveTo(path.string());
		auto reopened = core::loadWorldDocument(path);
		require(reopened->getStationSystemsSettings() == settings, "water World settings changed on reload");
		reopened->resumeSimulation(); require(reopened->advanceTicks(60), "reloaded water World failed");
		auto const snapshot = reopened->getStationSystemsSnapshot();
		require(snapshot.stores == expected.stores && snapshot.waterTreatments == expected.waterTreatments,
			"reloaded water World did not reproduce exact tick actuals");
	}
}
void qualityGates(smoke::Context const& context)
{
	auto settings = scenario(context, "water-greywater.yaml", true);
	auto const treated = run(settings);
	close(treated.crews[0].waterQuality.scale, 1);
	require(treated.crews[0].lastWaterActual > 0, "potable probe did not accept treated water");
	settings.crews[0].waterStoreId = "greywater";
	SourceScope restore;
	core::setStationSystemsSourceLoader([] {
		auto source = core::bundledStationSystemsSource();
		auto at = source.find("        self.ticks = self.ticks + 1");
		source.insert(at, R"lua(
        local q = self.crews[1].waterQuality
        assert(q.enabled and math.abs(q.scale - 0.25) < 1e-10)
        assert(q.limiter == 'water quality: dryOrganics above crew tolerance')
        assert(math.abs(q.bindingTolerance - 0.0005) < 1e-10)
        assert(math.abs(q.permittedKgPerS / q.demandKgPerS - 0.25) < 1e-10)
)lua");
		return source;
	});
	auto const raw = run(settings);
	close(raw.crews[0].metabolicScale, 0.25);
	close(raw.crews[0].lastWaterActual, raw.crews[0].lastWaterRequested * 0.25);
	require(raw.crews[0].limiter.find("water quality: dryOrganics") != std::string::npos, "organic limiter absent");
	core::setStationSystemsSourceLoader({});
	settings.crews[0].waterQuality.maxTotalDissolved = 0.0002;
	auto const total = run(settings);
	close(total.crews[0].metabolicScale, 0.1);
	require(total.crews[0].limiter.find("total dissolved") != std::string::npos, "total limiter absent");
	settings.crews[0].waterQuality.maxTotalDissolved = 0;
	auto const stopped = run(settings);
	close(stopped.crews[0].lastWaterActual, 0);
	close(stopped.crews[0].metabolicScale, 0);
	// A refused unit must not request material merely to lose it in a
	// zero-efficiency inlet. Quality caps connection demand as well as use.
	auto lossy = settings; lossy.waterTreatments.clear();
	bool found = false;
	for (auto const& link : core::station_connections::build(lossy).links)
		if (link.designKey.ownerType == "Crew" && link.designKey.peerId == "greywater")
		{ lossy.connections[link.designKey].efficiency = 0; found = true; }
	require(found, "missing crew water connection");
	close(store(run(lossy), "greywater").currentQualified, lossy.stores[0].startingQualified);
	settings.crews[0].waterQuality.maxTotalDissolved.reset();
	settings.crews[0].waterQuality.maxConcentrations["dryOrganics"] = 0;
	close(run(settings).crews[0].lastWaterActual, 0);
	settings.crews[0].waterQuality = {};
	close(run(settings).crews[0].metabolicScale, 1);
	// Gate uses actual ledger dilution, not authored 0.2% organics.
	settings.crews[0].waterQuality.maxConcentrations["dryOrganics"] = 0.0005;
	auto fresh = settings.stores[0]; fresh.id = "fresh"; fresh.composition.reset();
	fresh.startingQualified = 500; settings.stores.push_back(fresh);
	core::ResourceTransferSettings dilution;
	dilution.id = "dilution"; dilution.sourceId = "fresh"; dilution.destinationId = "greywater";
	dilution.unit = "kg"; dilution.ratePerSecond = 20; settings.transfers = {dilution};
	require(run(settings).crews[0].waterQuality.scale > raw.crews[0].waterQuality.scale,
		"quality gate ignored live dilution");
}
void qualityValidation(smoke::Context const& context)
{
	auto const base = scenario(context, "water-greywater.yaml", true);
	std::string error;
	for (auto value : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		auto s = base; s.crews[0].waterQuality.maxConcentrations["dryOrganics"] = value;
		require(!core::validateStationSystemsSettings(s, error), "invalid species tolerance accepted");
		s = base; s.crews[0].waterQuality.maxTotalDissolved = value;
		require(!core::validateStationSystemsSettings(s, error), "invalid total tolerance accepted");
	}
	auto s = base; s.crews[0].waterQuality.maxConcentrations["water"] = 0;
	require(!core::validateStationSystemsSettings(s, error), "non-dry species gate accepted");
	core::StationConfigurationDocument document; document.settings = base;
	core::StationConfigurationDocument restored;
	auto text = core::serializeStationConfiguration(document);
	auto at = text.find("throughputKgPerS:");
	require(at != std::string::npos, "serialized treatment missing");
	auto const line = text.rfind('\n', at) + 1;
	text.insert(line, std::string(at - line, ' ') + "waterQuality: {maxTotalDissolved: 0}\n");
	require(!core::parseStationConfiguration(text, restored, &error)
		&& error.find("no configurable water quality gate") != std::string::npos, "producer gate silently discarded");
}
void qualityConsumers(smoke::Context const&)
{
	auto settings = core::bundledStationReference().settings;
	// Built-in purity is not schema rejection. Declare a loaded feed and
	// ensure both mirrors reject operation before spending power or feed.
	auto const id = settings.electrolyzers[0].waterStoreId;
	for (auto& s : settings.stores) if (s.id == id)
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.99; composition.otherMinerals = 1;
		s.composition = composition;
	}
	auto const snapshot = run(settings, 3);
	require(snapshot.electrolyzers[0].waterQuality.enabled, "built-in gate not exposed");
	close(snapshot.electrolyzers[0].lastSplitKg, 0);
	close(snapshot.electrolyzers[0].lastPowerSuppliedKW, 0);
	require(snapshot.electrolyzers[0].limiter.find("water quality") != std::string::npos, "electrolyzer purity limiter absent");
	// Every optional consumer surface round-trips, and zero tolerances
	// participate in the same uniform operation scale, not a water-only cap.
	settings = core::bundledStationReference().settings;
	settings.crews[0].waterQuality.maxTotalDissolved = 0;
	settings.cropPopulations[0].waterQuality.maxTotalDissolved = 0;
	settings.algaeCultures[0].waterQuality.maxTotalDissolved = 0;
	settings.microbialProcessors[0].waterQuality.maxTotalDissolved = 0;
	core::StationConfigurationDocument doc; doc.settings = settings;
	core::StationConfigurationDocument restored; std::string error;
	require(core::parseStationConfiguration(core::serializeStationConfiguration(doc), restored, &error), error.c_str());
	require(restored.settings == settings, "consumer gate fields did not round-trip");
	auto const pure = run(settings, 3);
	require(pure.electrolyzers[0].lastSplitKg > 0, "pure electrolyzer feed changed");
	close(pure.cropPopulations[0].waterQuality.scale, 1);
	close(pure.algaeCultures[0].waterQuality.scale, 1);
	close(pure.microbialProcessors[0].waterQuality.scale, 1);
	for (auto& s : settings.stores) if (s.id == "fresh")
	{
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.99; composition.otherMinerals = 1;
		s.composition = composition;
	}
	auto const loaded = run(settings, 3);
	close(loaded.crews[0].lastWaterActual, 0);
	close(loaded.cropPopulations[0].waterQuality.scale, 0);
	close(loaded.algaeCultures[0].waterQuality.scale, 0);
	close(loaded.microbialProcessors[0].waterQuality.scale, 0);
	close(loaded.cropPopulations[0].lastGrowthKg, 0);
	close(loaded.algaeCultures[0].lastGrowthKg, 0);
	close(loaded.microbialProcessors[0].lastWaterConsumedKg, 0);
	settings.crews[0].waterQuality = {};
	settings.cropPopulations[0].waterQuality = {};
	settings.algaeCultures[0].waterQuality = {};
	settings.microbialProcessors[0].waterQuality = {};
	auto const ungated = run(settings, 3);
	require(!ungated.cropPopulations[0].waterQuality.enabled
		&& ungated.cropPopulations[0].lastGrowthKg > 0, "ungated loaded irrigation was refused");
	require(ungated.algaeCultures[0].lastGrowthKg > 0, "ungated loaded makeup was refused");
	require(ungated.microbialProcessors[0].lastWaterConsumedKg > 0, "ungated loaded process water was refused");
}
void legacy(smoke::Context const& context)
{
	auto settings = scenario(context, "water-legacy-anchor.yaml");
	auto const snapshot = run(settings);
	auto const& t = snapshot.waterTreatments[0];
	auto const daily = 86400.0 / (60.0 * core::World::getFixedTimestep());
	close(t.treatedKg * daily, 85); close(t.concentrateKg * daily, 15);
	for (auto const& s : snapshot.stores) require(s.actualConstituents.empty(), "legacy gained constituents");
	// The old scalar path's exact per-tick arithmetic, not an approximate
	// greywater comparison (compound water recovery intentionally differs).
	auto const full = settings.waterTreatments[0].throughputKgPerS * core::World::getFixedTimestep();
	require(t.lastTreatedKg == full * settings.waterTreatments[0].recoveryRatio,
		"legacy scalar arithmetic changed");
}
}
void registerCompoundWaterChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"waterQualityGates", qualityGates});
	checks.push_back({"waterQualityValidation", qualityValidation});
	checks.push_back({"waterQualityConsumers", qualityConsumers});
	checks.push_back({"compoundWaterPartial", partialRemoval});
	checks.push_back({"compoundWaterPartialValidation", partialValidation});
	checks.push_back({"compoundWaterTracer", tracer});
	checks.push_back({"compoundWaterBlend", blend});
	checks.push_back({"compoundWaterValidation", validation});
	checks.push_back({"compoundWaterRouting", routing});
	checks.push_back({"compoundWaterConnections", connections});
	checks.push_back({"compoundWaterLegacy", legacy});
	checks.push_back({"compoundWaterDocuments", documents});
}
