#include "StationWaterChecks.h"
#include "core/World.h"
#include "core/StationReference.h"
#include "core/StationConfigurationDocument.h"
#include "core/StationConnections.h"
#include "core/AgentTagRegistryDocument.h"
#include <algorithm>
#include <format>
#include <cmath>

namespace
{
using smoke::require;
struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } };
void close(double actual, double expected)
{
	require(std::abs(actual - expected) < 1e-8 * std::max(1.0, std::abs(expected)),
		std::format("compound water numeric mismatch: {:.17g} expected {:.17g}", actual, expected).c_str());
}
core::StationSystemsSettings scenario(smoke::Context const& context, char const* name)
{
	core::StationConfigurationDocument document;
	std::string error;
	auto const loaded = core::loadStationConfiguration(context.fixture(std::string("resources/station-systems/scenarios/")
		+ name).string(), document, &error);
	require(loaded, error.c_str());
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
	auto const settings = scenario(context, "water-greywater.yaml");
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
	checks.push_back({"compoundWaterTracer", tracer});
	checks.push_back({"compoundWaterBlend", blend});
	checks.push_back({"compoundWaterValidation", validation});
	checks.push_back({"compoundWaterRouting", routing});
	checks.push_back({"compoundWaterConnections", connections});
	checks.push_back({"compoundWaterLegacy", legacy});
	checks.push_back({"compoundWaterDocuments", documents});
}
