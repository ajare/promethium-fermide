#include "StationRig.h"
#include "Smoke.h"
#include "core/StationConnections.h"
#include "core/StationPorts.h"
#include "core/StationConfigurationDocument.h"
#include "core/BinarySerializer.h"
#include "core/WorldDocument.h"

namespace
{
using smoke::require;
core::StationSystemsSettings scene()
{
	core::StationSystemsSettings s; s.enabled = true; s.systemsStepWorldTicks = 1;
	s.stores = {{"source", "kg", 10000, 9000}, {"sink", "kg", 10000, 0}};
	s.transfers = {{"pipe", "source", "sink", "kg", 1}};
	return s;
}
auto connectionKey(core::StationSystemsSettings const& s, std::string_view owner, std::string_view role)
{
	for (auto const& link : core::station_connections::build(s).links)
		if (link.designKey.ownerType == owner && link.designKey.relation == role) return link.designKey;
	require(false, "missing connection in Monitor fixture");
	return core::StationConnectionKey{};
}
void enable(core::StationSystemsSettings& s, std::string_view type, std::string_view id, std::string_view metric)
{
	for (auto& m : s.monitors)
		if (m.subjectType == type && m.subjectId == id && m.metric == metric) { m.enabled = true; return; }
	require(false, "missing default Monitor");
}
void boundary(core::StationSystemsRuntime& run, double at, float dt, core::AlertSeverity severity)
{
	require(run.advanceTo(at - dt) && run.snapshot().activeAlerts.empty(), "default fired before on boundary");
	require(run.advanceTo(at), "default boundary drive failed");
	auto const snapshot = run.snapshot();
	require(snapshot.activeAlerts.size() == 1 && snapshot.activeAlerts[0].firedAtSeconds == at
		&& snapshot.activeAlerts[0].severity == severity, "default fire timestamp/severity wrong");
}
void clearBoundary(core::StationSystemsRuntime& run, double at, float dt)
{
	require(run.advanceTo(at - dt) && run.snapshot().activeAlerts.size() == 1, "default cleared early");
	require(run.advanceTo(at) && run.snapshot().activeAlerts.empty() && run.snapshot().closedAlerts.size() == 1
		&& run.snapshot().closedAlerts[0].clearedAtSeconds == at
		&& run.snapshot().closedAlerts[0].clearReason == core::AlertClearReason::AutoClear, "default clear boundary wrong");
}
void table(smoke::Context const&)
{
	auto s = scene(); s.stores[0].materialId = "water";
	core::applyStationStoreConnectionDefaultMonitors(s);
	require(core::stationStoreMonitorTemplates().size() == 4 && core::stationConnectionMonitorTemplates().size() == 4,
		"store/connection template registry incomplete");
	require(s.monitors.size() == 16, "water/non-water default expansion coverage wrong");
	require(std::all_of(s.monitors.begin(), s.monitors.end(), [](auto const& m) { return !m.enabled; }), "defaults not disabled");
	using enum core::AlertSeverity; using enum core::AlertDirection; using enum core::MonitorProvenance;
	std::vector<core::StationMonitorTemplate> const expectedStores{
		{"inventory-low", "usableFraction", {0.25, Below, 120, 60}, Warning, Rated},
		{"inventory-critical", "usableFraction", {0.10, Below, 60, 60}, Critical, Rated},
		{"water-total-dissolved", "totalDissolved", {500, Above, 60, 60}, Critical, Who},
		{"water-unexpected-species", "unexpectedSpecies", {0, Above, 60, 60}, Warning, Provisional}};
	std::vector<core::StationMonitorTemplate> const expectedConnections{
		{"loss-fraction", "lossFraction", {0.05, Above, 120, 60}, Warning, Rated},
		{"absolute-loss", "lossRate", {0, Above, 120, 60}, Warning, Custom},
		{"reversed-flow", "reversed", {0, Above, 1, 1}, Critical, Provisional},
		{"near-capacity", "utilizationFraction", {0.9, Above, 300, 120}, Advisory, Rated}};
	auto exact = [](auto const& actual, auto const& expected) {
		for (size_t i = 0; i < expected.size(); ++i)
			require(actual[i].key == expected[i].key && actual[i].metric == expected[i].metric
				&& actual[i].rule == expected[i].rule && actual[i].severity == expected[i].severity
				&& actual[i].provenance == expected[i].provenance, "default table differs from spec");
	};
	exact(core::stationStoreMonitorTemplates(), expectedStores);
	exact(core::stationConnectionMonitorTemplates(), expectedConnections);
	std::string error;
	require(core::validateStationSystemsSettings(s, error), error.c_str());
	auto original = s.monitors;
	core::applyStationStoreConnectionDefaultMonitors(s);
	require(s.monitors == original, "default expansion not idempotent");
	s.monitors[0].rule.threshold = 0.17; s.monitors[0].enabled = true;
	original = s.monitors; core::applyStationStoreConnectionDefaultMonitors(s);
	require(s.monitors == original, "re-expansion overwrote author tuning");
	core::StationConfigurationDocument doc; doc.settings = s;
	core::StationConfigurationDocument parsed;
	require(core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed, &error)
		&& parsed.settings == s, "store/connection Monitor YAML round-trip failed");
	auto out = core::BinarySerializer::toString();
	out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, s); out->endMap(); out->serialize();
	auto in = core::BinarySerializer::fromString(out->getSerializedString());
	in->deserialize(); in->beginMap("root"); core::StationSystemsSettings restored;
	core::readStationSystemsSettingsMap(*in, core::kWorldSchemaVersion, restored); in->endMap();
	require(restored == s, "store/connection Monitor binary round-trip failed");
	for (auto const& type : {"store", "connection"})
	{
		auto bad = s;
		auto m = std::find_if(bad.monitors.begin(), bad.monitors.end(), [&](auto const& m) { return m.subjectType == type; });
		m->subjectId = "missing";
		require(!core::validateStationSystemsSettings(bad, error), "missing subject accepted");
		bad = s;
		m = std::find_if(bad.monitors.begin(), bad.monitors.end(), [&](auto const& m) { return m.subjectType == type; });
		m->metric = "pressureKPa";
		require(!core::validateStationSystemsSettings(bad, error), "wrong subject metric accepted");
	}
	core::StationNodeRename rename;
	require(core::planStationNodeRename(s, "Store", "source", "renamed", rename, &error)
		&& core::renameStationNode(s, rename, &error) && core::validateStationSystemsSettings(s, error),
		"store/connection Monitor rename cascade failed");
	core::StationNodeRemoval removal;
	require(core::planStationNodeRemoval(s, "Transfer", "pipe", removal)
		&& !removal.monitorIds.empty() && core::removeStationNode(s, removal, &error), "connection Monitor removal failed");
	require(std::none_of(s.monitors.begin(), s.monitors.end(), [](auto const& m) { return m.subjectType == "connection"; }),
		"connection deletion orphaned a Monitor");
}
void stores(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (float dt : {0.25f, 1.0f})
	{
		// No species is named in this Monitor: trace contamination is detected
		// directly from the same live constituent inventory clients see.
		auto s = scene(); s.transfers.clear();
		s.stores = {{"potable", "kg", 400, 100}, {"drained", "kg", 400, 0}};
		core::ResourceCompositionSettings water; water.moisture = 0.999998;
		s.stores[0].composition = water; s.stores[0].materialId = "water";
		s.stores[1].composition = water;
		s.transfers = {{"drain", "potable", "drained", "kg", 0.000001}};
		core::applyStationStoreConnectionDefaultMonitors(s);
		enable(s, "store", "potable", "unexpectedSpecies");
		core::StationSystemsRuntime run(s, dt);
		boundary(run, 60, dt, core::AlertSeverity::Warning);
		s.transfers[0].ratePerSecond = 100 / dt;
		require(run.updateSettings(s), "contaminated water drain update refused");
		clearBoundary(run, 120, dt);
		for (auto const [metric, threshold, on, severity] : {
			std::tuple{"usableFraction", 0.25, 120.0, core::AlertSeverity::Warning},
			std::tuple{"usableFraction", 0.10, 60.0, core::AlertSeverity::Critical},
			std::tuple{"totalDissolved", 500.0, 60.0, core::AlertSeverity::Critical}})
		{
			auto low = scene(); low.transfers.clear(); low.stores.resize(1);
			low.stores[0].capacity = 1000; low.stores[0].startingQualified = 50;
			water.moisture = 0.999; low.stores[0].composition = water; low.stores[0].materialId = "water";
			core::applyStationStoreConnectionDefaultMonitors(low);
			for (auto& m : low.monitors) m.enabled = m.subjectId == "source" && m.metric == metric && m.rule.threshold == threshold;
			core::StationSystemsRuntime r(low, dt); boundary(r, on, dt, severity);
		}
	}
}
void connections(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (float dt : {0.25f, 1.0f})
	{
		for (bool absolute : {false, true})
		{
			auto s = scene(); auto key = connectionKey(s, "Transfer", "Source");
			s.connections[key] = {core::StationPhysicalConnection::Automatic, 1, 0.9};
			core::applyStationStoreConnectionDefaultMonitors(s);
			auto id = core::stationConnectionMonitorSubjectId(key);
			enable(s, "connection", id, absolute ? "lossRate" : "lossFraction");
			if (absolute) for (auto& m : s.monitors) if (m.enabled) m.rule.threshold = 0.05;
			core::StationSystemsRuntime run(s, dt);
			boundary(run, 120, dt, core::AlertSeverity::Warning);
			s.connections[key].efficiency = 1;
			require(run.updateSettings(s), "connection recovery refused"); clearBoundary(run, 180, dt);
		}
		auto s = scene(); auto key = connectionKey(s, "Transfer", "Source");
		s.connections[key] = {core::StationPhysicalConnection::Automatic, 1, 1};
		core::applyStationStoreConnectionDefaultMonitors(s);
		enable(s, "connection", core::stationConnectionMonitorSubjectId(key), "utilizationFraction");
		core::StationSystemsRuntime run(s, dt); boundary(run, 300, dt, core::AlertSeverity::Advisory);
		s.transfers[0].ratePerSecond = 0.5;
		require(run.updateSettings(s), "near-capacity recovery refused"); clearBoundary(run, 420, dt);
		// A real radiator carries heat backwards when its sink is hotter.
		core::StationSystemsSettings heat; heat.enabled = true; heat.systemsStepWorldTicks = 1;
		core::ThermalReservoirSettings reservoir; reservoir.id = "body";
		reservoir.heatCapacityKJPerK = 10000; reservoir.thermalClass = "low"; reservoir.startingTemperatureC = 20;
		heat.thermalReservoirs = {reservoir};
		core::RadiatorSettings radiator; radiator.id = "radiator"; radiator.reservoirId = "body";
		radiator.areaM2 = 1; radiator.sinkTemperatureC = 30; heat.radiators = {radiator};
		key = connectionKey(heat, "Radiator", "Heat rejection to environment");
		core::applyStationStoreConnectionDefaultMonitors(heat);
		enable(heat, "connection", core::stationConnectionMonitorSubjectId(key), "reversed");
		core::StationSystemsRuntime reverse(heat, dt); boundary(reverse, 1, dt, core::AlertSeverity::Critical);
		heat.radiators[0].sinkTemperatureC = 0;
		require(reverse.updateSettings(heat), "reverse-flow recovery refused"); clearBoundary(reverse, 2, dt);
	}
}
void stream(smoke::Context const& context)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	core::StationConfigurationDocument doc; std::string error;
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/water-greywater.yaml").string(), doc, &error), error.c_str());
	auto& s = doc.settings; s.systemsStepWorldTicks = 1;
	core::applyStationStoreConnectionDefaultMonitors(s);
	auto const key = connectionKey(s, "Water treatment", "Raw water");
	enable(s, "connection", core::stationConnectionMonitorSubjectId(key), "totalDissolved");
	core::StationSystemsRuntime run(s, 1);
	boundary(run, 60, 1, core::AlertSeverity::Critical);
	require(std::abs(run.snapshot().activeAlerts[0].currentValue - 2000) < 1e-6,
		"in-transit Monitor did not read carried water contaminant fraction in mg/L");
	for (auto& monitor : s.monitors) monitor.enabled = false;
	enable(s, "connection", core::stationConnectionMonitorSubjectId(key), "unexpectedSpecies");
	core::StationSystemsRuntime trace(s, 1);
	boundary(trace, 60, 1, core::AlertSeverity::Warning);
	require(trace.snapshot().activeAlerts[0].currentValue == 1, "stream trace missed unnamed species");
	auto const maximum = core::readStationMonitorMetric(s, run.snapshot(), "connection",
		core::stationConnectionMonitorSubjectId(key), "maxContaminantConcentration");
	require(maximum && std::abs(*maximum - 2000) < 1e-6, "stream maximum concentration units wrong");
	core::StationConfigurationDocument carry;
	require(core::loadStationConfiguration(context.fixture("resources/station-systems/scenarios/water-carryover.yaml").string(), carry, &error), error.c_str());
	carry.settings.systemsStepWorldTicks = 1;
	auto const recovered = connectionKey(carry.settings, "Microbial processor", "Recovered water");
	core::applyStationStoreConnectionDefaultMonitors(carry.settings);
	enable(carry.settings, "connection", core::stationConnectionMonitorSubjectId(recovered), "totalDissolved");
	core::StationSystemsRuntime producer(carry.settings, 1);
	boundary(producer, 60, 1, core::AlertSeverity::Critical);
	require(std::abs(producer.snapshot().activeAlerts[0].currentValue - 0.006 / 2.666 * 1e6) < 1e-5,
		"recovered-water connection lost carried nutrient contamination");
}
}
void registerStationStoreConnectionMonitorChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"monitorStoreConnectionTable", table});
	checks.push_back({"monitorStoreBoundaries", stores});
	checks.push_back({"monitorConnectionBoundaries", connections});
	checks.push_back({"monitorWaterStream", stream});
}
