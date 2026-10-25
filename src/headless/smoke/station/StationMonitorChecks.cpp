#include "StationRig.h"
#include "Smoke.h"
#include "core/StationConfigurationDocument.h"
#include "core/WorldDocument.h"
#include "core/BinarySerializer.h"

#include <format>

namespace
{
using namespace smoke::station;
core::StationSystemsSettings monitorStation()
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	core::AtmosphereSettings room;
	room.id = "room"; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
	room.startingOxygenKg = 20; room.startingNitrogenKg = 80;
	room.minPressureKPa = 1; room.maxPressureKPa = 200;
	s.atmospheres = {room};
	s.monitors = {{"oxygen-low", "atmosphere", "room", "oxygenPartialPressureKPa",
		{17.5, core::AlertDirection::Below, 30, 60}, core::AlertSeverity::Critical, true}};
	return s;
}
void monitorTiming(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	for (auto const dt : {0.25f, 1.0f})
	{
		auto settings = monitorStation();
		core::StationSystemsRuntime run(settings, dt);
		require(run.advanceTo(30 - dt) && run.snapshot().activeAlerts.empty(), "Monitor fired before full on time");
		require(run.advanceTo(30), "fire step failed");
		auto const snap = run.snapshot();
		require(snap.activeAlerts.size() == 1 && snap.closedAlerts.empty(), "Monitor did not fire at on boundary");
		auto const& alert = snap.activeAlerts[0];
		require(alert.firedAtSeconds == 30 && alert.monitorId == "oxygen-low" && alert.subjectType == "atmosphere"
			&& alert.subjectId == "room" && alert.metric == "oxygenPartialPressureKPa"
			&& alert.threshold == 17.5 && alert.severity == core::AlertSeverity::Critical
			&& alert.currentValue == snap.atmospheres[0].oxygenPartialPressureKPa,
			"active ledger did not publish observed value, subject, severity or timestamp");
		// Perturb only a live authored parameter; never poke the Monitor or snapshot.
		settings.atmospheres[0].volumeM3 = 50;
		require(run.updateSettings(settings) && run.advanceTo(90 - dt)
			&& run.snapshot().activeAlerts.size() == 1, "Monitor cleared before full recovery time");
		require(run.advanceTo(90), "recovery step failed");
		auto const cleared = run.snapshot();
		require(cleared.activeAlerts.empty() && cleared.closedAlerts.size() == 1
			&& cleared.closedAlerts[0].firedAtSeconds == 30 && cleared.closedAlerts[0].clearedAtSeconds == 90
			&& cleared.closedAlerts[0].clearReason == core::AlertClearReason::AutoClear, "clear boundary/ledger wrong");
		require(run.advanceTo(100) && run.snapshot().closedAlerts.size() == 1, "clear repeated");
		require(std::count_if(run.events().begin(), run.events().end(), [](auto const& e) { return e.category == "alert"; }) == 2,
			"fire/clear event stream did not announce both transitions exactly once");
	}
	// Zero-time, strict inequality (equality is inside), both directions,
	// and interrupted persistence in both states.
	auto settings = monitorStation();
	settings.monitors[0].rule.triggerOnSeconds = 0;
	settings.monitors[0].rule.triggerOffSeconds = 0;
	core::StationSystemsRuntime instant(settings, 1);
	require(instant.snapshot().activeAlerts.empty() && instant.advanceTo(1)
		&& instant.snapshot().activeAlerts.size() == 1, "zero-on did not fire on evaluation");
	settings.atmospheres[0].volumeM3 = 50;
	require(instant.updateSettings(settings) && instant.advanceTo(2) && instant.snapshot().activeAlerts.empty(), "zero-off not instant");
	settings = monitorStation();
	settings.monitors[0].rule = {0, core::AlertDirection::Above, 2, 2};
	settings.atmospheres[0].startingOxygenKg = 0;
	core::StationSystemsRuntime equal(settings, 1);
	require(equal.advanceTo(10) && equal.snapshot().activeAlerts.empty(), "equality fired Above");
	settings.monitors[0].rule.direction = core::AlertDirection::Below;
	core::StationSystemsRuntime equalBelow(settings, 1);
	require(equalBelow.advanceTo(10) && equalBelow.snapshot().activeAlerts.empty(), "equality fired Below");
	settings = monitorStation(); settings.monitors[0].rule = {10, core::AlertDirection::Above, 2, 0};
	core::StationSystemsRuntime above(settings, 1);
	require(above.advanceTo(1) && above.snapshot().activeAlerts.empty() && above.advanceTo(2)
		&& above.snapshot().activeAlerts.size() == 1, "Above direction did not fire at boundary");
	settings = monitorStation(); settings.monitors[0].rule.triggerOnSeconds = 2; settings.monitors[0].rule.triggerOffSeconds = 2;
	core::StationSystemsRuntime oscillate(settings, 1);
	require(oscillate.advanceTo(1), "oscillation start failed");
	settings.atmospheres[0].volumeM3 = 50;
	require(oscillate.updateSettings(settings) && oscillate.advanceTo(2), "inside sample failed");
	settings.atmospheres[0].volumeM3 = 100;
	require(oscillate.updateSettings(settings) && oscillate.advanceTo(3) && oscillate.snapshot().activeAlerts.empty(), "on persistence was not reset");
	require(oscillate.advanceTo(4) && oscillate.snapshot().activeAlerts.size() == 1, "second on boundary failed");
	settings.atmospheres[0].volumeM3 = 50;
	require(oscillate.updateSettings(settings) && oscillate.advanceTo(5), "first recovery failed");
	settings.atmospheres[0].volumeM3 = 100;
	require(oscillate.updateSettings(settings) && oscillate.advanceTo(6), "outside sample failed");
	settings.atmospheres[0].volumeM3 = 50;
	require(oscillate.updateSettings(settings) && oscillate.advanceTo(7) && oscillate.snapshot().activeAlerts.size() == 1,
		"off persistence was not reset");
	require(oscillate.advanceTo(8) && oscillate.snapshot().closedAlerts[0].clearedAtSeconds == 8, "second off boundary failed");
}
void monitorCommands(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = monitorStation(); settings.monitors[0].enabled = false;
	core::StationSystemsRuntime run(settings, 1);
	require(run.advanceTo(100) && run.snapshot().activeAlerts.empty() && run.snapshot().closedAlerts.empty(), "disabled Monitor produced alerts");
	settings.monitors[0].enabled = true;
	require(run.updateSettings(settings) && run.advanceTo(129) && run.snapshot().activeAlerts.empty(), "enable backfilled history");
	require(run.advanceTo(130) && run.snapshot().activeAlerts[0].firedAtSeconds == 130, "enable did not start fresh");
	settings.monitors[0].enabled = false;
	require(run.updateSettings(settings) && run.snapshot().activeAlerts.empty()
		&& run.snapshot().closedAlerts[0].clearedAtSeconds == 130
		&& run.snapshot().closedAlerts[0].clearReason == core::AlertClearReason::MonitorDisabled, "disable did not close active Alert");
	require(run.advanceTo(200), "disabled drive failed");
	settings.monitors[0].enabled = true;
	require(run.updateSettings(settings) && run.advanceTo(229) && run.snapshot().activeAlerts.empty(), "re-enable retained active persistence");
	settings.monitors[0].enabled = false;
	require(run.updateSettings(settings), "pending disable failed");
	settings.monitors[0].enabled = true;
	require(run.updateSettings(settings) && run.advanceTo(258) && run.snapshot().activeAlerts.empty(), "pending persistence survived disable");
	require(run.advanceTo(259) && run.snapshot().activeAlerts.size() == 1, "fresh evaluation failed");
	// Real validated World facade, not a test-specific command path.
	core::World world("Monitor commands", 4, 4);
	world.pauseSimulation(); world.finishBuild();
	settings.monitors[0].enabled = false;
	require(world.configureStationSystems(settings, false), "World configure failed");
	world.resumeSimulation();
	require(advanceSeconds(world, 1), "World setup failed");
	settings.monitors[0].enabled = true;
	require(world.updateStationSystemsSettings(settings) && advanceSeconds(world, 30)
		&& world.getStationSystemsSnapshot().activeAlerts.size() == 1, "World enable path failed");
	settings.monitors[0].enabled = false;
	require(world.updateStationSystemsSettings(settings) && world.getStationSystemsSnapshot().activeAlerts.empty(), "World disable path failed");
}
void monitorValidation(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = monitorStation(); core::StationSystemsRuntime run(settings, 1);
	require(run.advanceTo(10), "validation setup failed");
	auto const before = run.snapshot(); auto const events = run.events();
	std::vector<core::StationSystemsSettings> invalid;
	auto add = [&](auto mutate) { auto copy = settings; mutate(copy); invalid.push_back(copy); };
	add([](auto& s) { s.monitors.push_back(s.monitors[0]); });
	add([](auto& s) { s.monitors[0].id.clear(); });
	add([](auto& s) { s.monitors[0].subjectType = "pathway"; });
	add([](auto& s) { s.monitors[0].subjectId = "missing"; });
	add([](auto& s) { s.monitors[0].metric = "unknown"; });
	add([](auto& s) { s.monitors[0].rule.triggerOnSeconds = -1; });
	add([](auto& s) { s.monitors[0].rule.triggerOffSeconds = -1; });
	add([](auto& s) { s.monitors[0].rule.direction = static_cast<core::AlertDirection>(99); });
	add([](auto& s) { s.monitors[0].severity = static_cast<core::AlertSeverity>(99); });
	for (auto v : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
	{
		add([&](auto& s) { s.monitors[0].rule.threshold = v; });
		add([&](auto& s) { s.monitors[0].rule.triggerOnSeconds = v; });
		add([&](auto& s) { s.monitors[0].rule.triggerOffSeconds = v; });
	}
	for (auto& bad : invalid)
	{
		bad.atmospheres[0].volumeM3 = 50; // mixed offer must not partially land
		std::string diagnostic;
		require(!run.updateSettings(bad, &diagnostic) && !diagnostic.empty(), "invalid Monitor accepted/unexplained");
		require(run.effectiveSettings() == settings && run.snapshot() == before && run.events() == events, "refusal left partial runtime state");
		// Invalid C++ enum representations have no wire spelling; exercise
		// malformed enum strings separately below rather than normalize them.
		if (static_cast<int>(bad.monitors[0].rule.direction) == 99
			|| static_cast<int>(bad.monitors[0].severity) == 99) continue;
		core::StationConfigurationDocument document; document.settings = bad;
		auto yaml = core::serializeStationConfiguration(document);
		core::StationConfigurationDocument target; target.settings = settings;
		require(!core::parseStationConfiguration(yaml, target, &diagnostic) && target.settings == settings, "invalid document mutated destination");
	}
	for (auto const& [from, to] : std::vector<std::pair<std::string, std::string>>{
		{"Below", "Sideways"}, {"Critical", "Urgent"}, {"schemaVersion: 122", "schemaVersion: 121"}})
	{
		core::StationConfigurationDocument doc; doc.settings = settings;
		auto yaml = core::serializeStationConfiguration(doc);
		yaml.replace(yaml.find(from), from.size(), to);
		auto target = doc; std::string diagnostic;
		require(!core::parseStationConfiguration(yaml, target, &diagnostic) && target.settings == settings
			&& !diagnostic.empty(), "malformed Monitor wire value accepted");
	}
	require(run.advanceTo(30) && run.snapshot().activeAlerts.size() == 1, "refused edits reset pending persistence");
}
void monitorPersistence(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = monitorStation();
	// Even a physical-state replay must not persist Alert history/timers.
	core::StationSystemsRuntime run(settings, 1);
	require(run.advanceTo(30) && run.snapshot().activeAlerts.size() == 1, "replay setup failed");
	auto replayOut = core::BinarySerializer::toString();
	replayOut->beginMap("root"); run.writeFaultReplay(*replayOut); replayOut->endMap(); replayOut->serialize();
	auto replayIn = core::BinarySerializer::fromString(replayOut->getSerializedString());
	replayIn->deserialize(); replayIn->beginMap("root");
	auto loaded = core::StationSystemsRuntime::readFaultReplay(*replayIn, settings, 1); replayIn->endMap();
	require(loaded->snapshot().activeAlerts.empty() && loaded->snapshot().closedAlerts.empty()
		&& loaded->advanceTo(59) && loaded->snapshot().activeAlerts.empty()
		&& loaded->advanceTo(60) && loaded->snapshot().activeAlerts[0].firedAtSeconds == 60,
		"load retained Alert history or backfilled persistence");
	for (auto severity : {core::AlertSeverity::Advisory, core::AlertSeverity::Warning, core::AlertSeverity::Critical})
	for (auto direction : {core::AlertDirection::Above, core::AlertDirection::Below})
	{
		settings.monitors[0].severity = severity; settings.monitors[0].rule.direction = direction;
		for (bool binary : {false, true})
		{
			std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
			out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, settings); out->endMap(); out->serialize();
			auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
				: static_cast<core::YamlSerializer&>(*out).getSerializedString();
			std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
			in->deserialize(); in->beginMap("root"); core::StationSystemsSettings restored;
			core::readStationSystemsSettingsMap(*in, core::kWorldSchemaVersion, restored); in->endMap();
			require(restored == settings, "Monitor fields did not round-trip");
		}
		core::StationConfigurationDocument doc, parsed; doc.settings = settings;
		require(core::parseStationConfiguration(core::serializeStationConfiguration(doc), parsed) && parsed.settings == settings,
			"Station configuration lost Monitor fields");
	}
}
void monitorDemo(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto settings = monitorStation();
	auto supply = settings.atmospheres[0]; supply.id = "supply"; supply.startingOxygenKg = 80;
	settings.atmospheres.push_back(supply);
	settings.atmospherePathways = {{"recovery", "room", "supply", 25, 0}};
	core::StationConfigurationDocument authored, loaded;
	authored.settings = settings;
	require(core::parseStationConfiguration(core::serializeStationConfiguration(authored), loaded), "demo document failed to load");
	core::StationSystemsRuntime run(loaded.settings, 1);
	require(run.advanceTo(29) && run.snapshot().activeAlerts.empty(), "demo fired early");
	require(run.advanceTo(30) && run.snapshot().activeAlerts.size() == 1, "demo low pO2 did not fire");
	settings.atmospherePathways[0].openFraction = 1;
	require(run.updateSettings(settings), "demo recovery hatch refused");
	double firstInside = 0;
	for (double time = 31; time <= 180; ++time)
	{
		require(run.advanceTo(time), "demo recovery failed");
		auto snap = run.snapshot();
		if (!firstInside && snap.atmospheres[0].oxygenPartialPressureKPa >= 17.5) firstInside = time;
		if (firstInside && time < firstInside + 59) require(snap.activeAlerts.size() == 1, "demo cleared before 60 recovery samples");
		if (!snap.closedAlerts.empty())
		{
			require(firstInside && snap.closedAlerts[0].clearedAtSeconds == firstInside + 59
				&& snap.closedAlerts[0].clearReason == core::AlertClearReason::AutoClear, "demo clear time wrong");
			return;
		}
	}
	throw std::runtime_error("demo did not recover and clear");
}

// ---------------------------------------------------------------------------
// #867: the spec §4.1 atmosphere habitability default table.
// ---------------------------------------------------------------------------

struct ExpectedDefault
{
	char const* key, *metric, *tag;
	double threshold;
	core::AlertDirection direction;
	double onSeconds, offSeconds;
	core::AlertSeverity severity;
};

// The spec §4.1 table transcribed independently of the implementation, so the
// check fails if the shipped defaults drift from the documented numbers.
std::vector<ExpectedDefault> specHabitabilityTable()
{
	using enum core::AlertDirection;
	using enum core::AlertSeverity;
	return {
		{"po2-low", "oxygenPartialPressureKPa", "iss", 17.5, Below, 30.0, 60.0, Critical},
		{"po2-high", "oxygenPartialPressureKPa", "iss", 24.0, Above, 60.0, 60.0, Warning},
		{"pco2-high", "carbonDioxidePartialPressureKPa", "iss", 0.7, Above, 300.0, 120.0, Warning},
		{"pco2-critical", "carbonDioxidePartialPressureKPa", "iss", 1.0, Above, 60.0, 120.0, Critical},
		{"pressure-low", "pressureKPa", "iss", 90.0, Below, 10.0, 60.0, Critical},
		{"pressure-high", "pressureKPa", "prov", 110.0, Above, 60.0, 60.0, Warning},
		{"temperature-high", "temperatureC", "iss", 27.0, Above, 600.0, 300.0, Advisory},
		{"temperature-low", "temperatureC", "iss", 18.0, Below, 600.0, 300.0, Advisory},
		{"humidity-high", "relativeHumidity", "prov", 0.75, Above, 600.0, 300.0, Advisory},
		{"toxic-contaminant", "toxicConcentration", "prov", 0.1, Above, 60.0, 120.0, Critical},
		{"oxygen-supply-unmet", "oxygenUnmetKg", "rated", 0.0, Above, 10.0, 30.0, Critical},
		{"compartment-leak", "leakRateKgPerS", "prov", 0.01, Above, 60.0, 120.0, Warning},
	};
}

core::StationSystemsSettings habitabilityStation(std::vector<std::string> const& compartments)
{
	core::StationSystemsSettings s;
	s.enabled = true; s.systemsStepWorldTicks = 1;
	for (auto const& id : compartments)
	{
		core::AtmosphereSettings room;
		room.id = id; room.volumeM3 = 100; room.thermalCapacityKJPerK = 10000;
		room.startingOxygenKg = 20; room.startingNitrogenKg = 80;
		room.minPressureKPa = 1; room.maxPressureKPa = 500;
		s.atmospheres.push_back(room);
	}
	return s;
}

core::StationMonitorSettings const* findMonitor(core::StationSystemsSettings const& s, std::string_view id)
{
	auto const found = std::find_if(s.monitors.begin(), s.monitors.end(),
		[&](auto const& m) { return m.id == id; });
	return found == s.monitors.end() ? nullptr : &*found;
}

void monitorDefaultTable(smoke::Context const&)
{
	auto const table = specHabitabilityTable();
	require(core::stationAtmosphereHabitabilityTemplates().size() == table.size(),
		"the shipped table is not the twelve spec 4.1 habitability defaults");
	// Every row watches a Metric the atmosphere subject actually publishes.
	for (auto const& row : table)
		require(core::findStationAtmosphereMetric(row.metric) != nullptr,
			"a spec 4.1 default names a Metric outside the atmosphere catalogue");

	auto settings = habitabilityStation({"room", "hub", "lab"});
	core::applyStationAtmosphereDefaultMonitors(settings);
	require(settings.monitors.size() == table.size() * 3,
		"the default set did not land on every atmosphere compartment");
	for (auto const* subject : {"room", "hub", "lab"})
		for (auto const& expected : table)
		{
			auto const* monitor = findMonitor(settings, std::string(subject) + "-" + expected.key);
			require(monitor != nullptr, "default Monitor id is not <subject>-<key>");
			require(monitor->subjectType == "atmosphere" && monitor->subjectId == subject,
				"default Monitor does not watch the compartment it was expanded for");
			require(monitor->metric == expected.metric
				&& monitor->rule.threshold == expected.threshold
				&& monitor->rule.direction == expected.direction
				&& monitor->rule.triggerOnSeconds == expected.onSeconds
				&& monitor->rule.triggerOffSeconds == expected.offSeconds
				&& monitor->severity == expected.severity,
				"default Monitor values drifted from the spec 4.1 table");
			require(!monitor->enabled, "a habitability default shipped enabled");
			require(core::stationMonitorProvenanceTag(monitor->provenance) == std::string(expected.tag),
				"default Monitor provenance tag is wrong");
		}
	// Re-applying the table is a no-op on authored data: a designer's own
	// threshold and enable survive, and nothing is duplicated.
	auto tuned = settings;
	tuned.monitors[0].enabled = true;
	tuned.monitors[0].rule.threshold = 19.0;
	core::applyStationAtmosphereDefaultMonitors(tuned);
	require(tuned.monitors.size() == settings.monitors.size(), "re-applying the table duplicated Monitors");
	require(tuned.monitors[0].enabled && tuned.monitors[0].rule.threshold == 19.0,
		"a per-Monitor adjustment did not survive re-application");
	require(tuned.monitors[1] == settings.monitors[1], "re-applying the table rewrote an untouched Monitor");
	// A configuration with no compartments expands to nothing: the table
	// never invents a subject.
	core::StationSystemsSettings empty;
	core::applyStationAtmosphereDefaultMonitors(empty);
	require(empty.monitors.empty(), "defaults invented a subject with no compartment");
	// The expanded set is ordinary authored configuration: it validates and
	// round-trips through both wire formats unchanged.
	{
		core::ScopedStationStepRatioForTesting clock(std::nullopt);
		core::StationSystemsRuntime run(settings, 1);
		require(run.effectiveSettings() == settings, "expanded defaults were refused or rewritten");
	}
	for (bool binary : {false, true})
	{
		std::unique_ptr<core::Serializer> out = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
		out->beginMap("root"); core::writeStationSystemsSettingsMap(*out, settings); out->endMap(); out->serialize();
		auto const bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString()
			: static_cast<core::YamlSerializer&>(*out).getSerializedString();
		std::unique_ptr<core::Serializer> in = binary
			? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
			: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
		in->deserialize(); in->beginMap("root"); core::StationSystemsSettings restored;
		core::readStationSystemsSettingsMap(*in, core::kWorldSchemaVersion, restored); in->endMap();
		require(restored == settings, "expanded habitability defaults did not round-trip");
	}
	core::StationConfigurationDocument document, parsed;
	document.settings = settings;
	require(core::parseStationConfiguration(core::serializeStationConfiguration(document), parsed)
		&& parsed.settings == settings, "Station configuration lost the habitability default set");
}

// A large sink the compartment can be coupled to later. It is authored from
// the start: adding a reservoir mid-run is a topology change the live
// settings contract refuses, while the exchanger conductance is an ordinary
// live parameter. Stiff enough that the compartment crosses the bound on the
// first coupled sample, so the trigger-off clock is driven by the Monitor
// rather than by how fast the physics happens to move.
void addSink(core::StationSystemsSettings& s, double sinkC)
{
	core::ThermalReservoirSettings sink;
	sink.id = "sink"; sink.thermalClass = "warm";
	sink.heatCapacityKJPerK = 1e6; sink.startingTemperatureC = sinkC;
	sink.minTemperatureC = 0; sink.maxTemperatureC = 60;
	s.thermalReservoirs = {sink};
	s.atmospheres[0].reservoirId = "sink";
}

// Each probe authors the watched Metric outside its bound at construction -
// starting conditions are never pushed live - and brings it back inside with
// a live authored lever, so both the fire and the clear are driven by values
// the Station actually published rather than by poking the snapshot.
struct DefaultProbe
{
	char const* key;
	void (*outside)(core::StationSystemsSettings&);
	void (*inside)(core::StationSystemsSettings&);
	void (*inject)(core::StationSystemsRuntime&) = nullptr;
};

std::vector<DefaultProbe> habitabilityProbes()
{
	return {
		// 15.2 kPa out, 30.5 kPa back inside.
		{"po2-low",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].startingOxygenKg = 20; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 50; }},
		// 26.7 kPa out, 13.3 kPa inside.
		{"po2-high",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].startingOxygenKg = 35; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 200; }},
		// 1.11 kPa out, 0.28 kPa inside.
		{"pco2-high",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].startingCarbonDioxideKg = 2; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 400; }},
		// Same excursion, the tighter Critical bound.
		{"pco2-critical",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].startingCarbonDioxideKg = 2; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 400; }},
		// 59.8 kPa out, 119.6 kPa inside.
		{"pressure-low",
			[](core::StationSystemsSettings& s) {
				s.atmospheres[0].startingNitrogenKg = 60; s.atmospheres[0].startingOxygenKg = 10;
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 50; }},
		// 118.6 kPa out, 59.3 kPa inside.
		{"pressure-high",
			[](core::StationSystemsSettings& s) {
				s.atmospheres[0].startingNitrogenKg = 110; s.atmospheres[0].startingOxygenKg = 30;
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 200; }},
		// Uncoupled at 30 C; closing the exchanger to a 20 C sink pulls it
		// under the 27 C bound.
		{"temperature-high",
			[](core::StationSystemsSettings& s) {
				addSink(s, 20);
				s.atmospheres[0].startingTemperatureC = 30;
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].exchangerConductanceKWPerK = 20000; }},
		// Uncoupled at 15 C; a 25 C sink lifts it over the 18 C bound.
		{"temperature-low",
			[](core::StationSystemsSettings& s) {
				addSink(s, 25);
				s.atmospheres[0].startingTemperatureC = 15;
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].exchangerConductanceKWPerK = 20000; }},
		// 1.4 kg of vapour is RH 0.81; four times the volume is 0.20.
		{"humidity-high",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].startingWaterVapourKg = 1.4; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 400; }},
		// The toxic species has no authored lever: a real battery
		// thermal-runaway Fault books 20 g into the 100 m3 compartment
		// (0.2 g/m3), and the compartment volume dilutes it to 0.05.
		{"toxic-contaminant",
			[](core::StationSystemsSettings& s) {
				core::BatterySettings bank;
				bank.id = "bank";
				bank.nameplateKWh = 100; bank.usableKWh = 90; bank.startingStoredKWh = 50;
				bank.maxChargeKW = 50; bank.maxDischargeKW = 50;
				bank.thermalRunaway = core::BatteryThermalRunawaySettings{"room", 500.0, 0.02};
				s.batteries = {bank};
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].volumeM3 = 400; },
			[](core::StationSystemsRuntime& run) {
				require(run.injectFault("bank", "thermal-runaway",
					{core::FaultCause::Kind::ScenarioEvent, "cell failure"}), "toxic booking setup failed");
			}},
		// Two occupants wanting 1 kg/s against 0.5 kg present leaves the
		// supply short from the first tick; emptying the compartment ends it.
		{"oxygen-supply-unmet",
			[](core::StationSystemsSettings& s) {
				s.atmospheres[0].startingOxygenKg = 0.5;
				s.atmospheres[0].occupants = 2;
				s.atmospheres[0].oxygenUsePerOccupantKgPerS = 1.0;
			},
			[](core::StationSystemsSettings& s) { s.atmospheres[0].occupants = 0; }},
		// An authored 0.02 kg/s constant leak, then sealed.
		{"compartment-leak",
			[](core::StationSystemsSettings& s) { s.atmospheres[0].leakRateKgPerS = 0.02; },
			[](core::StationSystemsSettings& s) { s.atmospheres[0].leakRateKgPerS = 0.0; }},
	};
}

double observedMetric(core::StationSystemsSnapshot const& snap, std::string_view metric)
{
	auto const* entry = core::findStationAtmosphereMetric(metric);
	require(entry != nullptr, "atmosphere Metric vanished from the catalogue");
	return entry->read(snap.atmospheres[0]);
}

bool beyond(core::AlertRule const& rule, double value)
{
	return rule.direction == core::AlertDirection::Above ? value > rule.threshold : value < rule.threshold;
}

void monitorDefaultBoundaries(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	auto const table = specHabitabilityTable();
	for (auto const& probe : habitabilityProbes())
	{
		auto const expected = std::find_if(table.begin(), table.end(),
			[&](auto const& row) { return std::string(row.key) == probe.key; });
		require(expected != table.end(), "probe has no spec 4.1 row to check against");
		auto settings = habitabilityStation({"room"});
		probe.outside(settings);
		core::applyStationAtmosphereDefaultMonitors(settings);
		require(settings.monitors.size() == table.size(), "single-compartment expansion is wrong");
		// One default at a time, so no neighbouring Monitor confuses the ledger.
		std::vector<core::StationMonitorSettings> solo;
		for (auto const& monitor : settings.monitors)
			if (monitor.id == std::string("room-") + probe.key) solo.push_back(monitor);
		require(solo.size() == 1, "the default under test is not uniquely addressable");
		solo[0].enabled = true;
		settings.monitors = solo;
		core::StationSystemsRuntime run(settings, 1);
		if (probe.inject) probe.inject(run);
		auto const& rule = solo[0].rule;
		// Let the first audited boundary land: an injected one-shot is not
		// in the compartment before it, and that sample is the first one the
		// Monitor can count toward trigger-on persistence.
		require(run.advanceTo(1), (std::string(probe.key) + " first sample failed").c_str());
		require(beyond(rule, observedMetric(run.snapshot(), solo[0].metric)),
			(std::string(probe.key) + " probe did not start beyond its bound").c_str());
		// Fire only after the full trigger-on persistence.
		require(run.advanceTo(rule.triggerOnSeconds - 1) && run.snapshot().activeAlerts.empty(),
			(std::string(probe.key) + " fired before full trigger-on persistence").c_str());
		require(run.advanceTo(rule.triggerOnSeconds) && run.snapshot().activeAlerts.size() == 1,
			(std::string(probe.key) + " did not fire at the trigger-on boundary").c_str());
		auto const fired = run.snapshot();
		auto const& alert = fired.activeAlerts[0];
		require(alert.monitorId == solo[0].id && alert.metric == solo[0].metric
			&& alert.severity == expected->severity
			&& alert.provenance == solo[0].provenance
			&& core::stationMonitorProvenanceTag(alert.provenance) == std::string(expected->tag),
			(std::string(probe.key) + " Alert did not publish its severity and provenance to surfaces").c_str());
		// Bring it back inside, then require the full trigger-off persistence
		// counted from the first sample that is actually inside.
		probe.inside(settings);
		require(run.updateSettings(settings), (std::string(probe.key) + " recovery edit refused").c_str());
		double firstInside = 0;
		bool cleared = false;
		for (double time = rule.triggerOnSeconds + 1; time <= rule.triggerOnSeconds + rule.triggerOffSeconds + 600; ++time)
		{
			require(run.advanceTo(time), (std::string(probe.key) + " recovery drive failed").c_str());
			if (!firstInside && !beyond(rule, observedMetric(run.snapshot(), solo[0].metric))) firstInside = time;
			if (!firstInside) continue;
			if (time < firstInside + rule.triggerOffSeconds - 1)
			{
				require(run.snapshot().activeAlerts.size() == 1,
					(std::string(probe.key) + " cleared before full trigger-off persistence").c_str());
				continue;
			}
			auto const snap = run.snapshot();
			require(snap.activeAlerts.empty() && snap.closedAlerts.size() == 1
				&& snap.closedAlerts[0].clearReason == core::AlertClearReason::AutoClear
				&& snap.closedAlerts[0].firedAtSeconds == rule.triggerOnSeconds
				&& snap.closedAlerts[0].clearedAtSeconds == time,
				(std::string(probe.key) + " did not auto-clear at the trigger-off boundary").c_str());
			cleared = true;
			break;
		}
		require(cleared, (std::string(probe.key) + " never came back inside its bound and cleared").c_str());
	}
}

}

// The Alert announcements, in the order the run produced them.
std::vector<core::StationSystemsEvent> alertEvents(core::StationSystemsRuntime& run)
{
	std::vector<core::StationSystemsEvent> announced;
	for (auto const& event : run.events())
		if (event.category == "alert") announced.push_back(event);
	return announced;
}
int countAnnouncements(std::vector<core::StationSystemsEvent> const& events, std::string_view transition)
{
	return static_cast<int>(std::count_if(events.begin(), events.end(), [&](auto const& event) {
		return event.message.find("Alert " + std::string(transition)) == 0;
	}));
}
void monitorCallbackStream(smoke::Context const&)
{
	core::ScopedStationStepRatioForTesting clock(std::nullopt);
	// The registry resolves every alert type to a bound pair. The alert type is
	// the Metric that raised the Alert, and an unbound type still lands on the
	// v1 handler rather than dropping the Alert on the floor.
	auto const& oxygen = core::stationAlertCallbacksFor("oxygenPartialPressureKPa");
	require(oxygen.onFire != nullptr && oxygen.onClear != nullptr,
		"the oxygen Metric has no bound Alert handler pair");
	require(core::stationAlertCallbacksFor("noSuchAlertType").onFire == oxygen.onFire
		&& core::stationAlertCallbacksFor("noSuchAlertType").onClear == oxygen.onClear,
		"an unbound alert type does not fall back to the v1 handler");

	auto settings = monitorStation();
	core::StationSystemsRuntime run(settings, 1);
	require(run.advanceTo(29) && run.snapshot().activeAlerts.empty(), "Alert fired before its on time");
	require(run.advanceTo(30) && run.snapshot().activeAlerts.size() == 1, "Alert did not fire on boundary");

	// The fire is announced on the stream carrying the whole payload, so a
	// scenario author reads the oversight event without querying the ledger.
	auto const fired = run.snapshot().activeAlerts[0];
	auto const events = alertEvents(run);
	require(events.size() == 1 && countAnnouncements(events, "fired") == 1,
		"the fire was not announced on the event stream exactly once");
	auto const& fire = events[0].message;
	for (auto const* field : {"Alert fired 'oxygen-low'", "on 'atmosphere/room'", "oxygenPartialPressureKPa",
		"threshold 17.5", "severity Critical", "provenance custom", "sim time 30.000s"})
		require(fire.find(field) != std::string::npos,
			(std::string("the fire announcement omits a payload field: ") + field).c_str());
	require(fire.find("value " + std::format("{}", fired.currentValue)) != std::string::npos,
		"the fire announcement's value disagrees with the ledger entry it came from");

	// A live reading refreshes the published Alert but is not a transition: the
	// ledger is written only when the callback fires, so no extra announcement
	// appears, and the value published is the Monitor's current one.
	settings.atmospheres[0].volumeM3 = 50;
	require(run.updateSettings(settings) && run.advanceTo(31), "live refresh drive failed");
	auto const live = run.snapshot();
	require(live.activeAlerts.size() == 1, "a live refresh dropped the active Alert");
	require(live.activeAlerts[0].currentValue == live.atmospheres[0].oxygenPartialPressureKPa
		&& live.activeAlerts[0].currentValue != fired.currentValue,
		"the published active Alert does not carry the Monitor's live reading");
	require(alertEvents(run).size() == 1, "a live value refresh announced an extra Alert event");

	// Auto-clear announces with its reason, and its payload matches the
	// closed-out ledger entry the same callback wrote.
	require(run.advanceTo(90) && run.snapshot().activeAlerts.empty(), "Alert did not auto-clear");
	auto const cleared = run.snapshot().closedAlerts[0];
	auto const afterClear = alertEvents(run);
	require(afterClear.size() == 2 && countAnnouncements(afterClear, "cleared") == 1,
		"the clear was not announced on the event stream exactly once");
	auto const& clear = afterClear[1].message;
	for (auto const* field : {"Alert cleared 'oxygen-low'", "on 'atmosphere/room'", "oxygenPartialPressureKPa",
		"threshold 17.5", "severity Critical", "sim time 90.000s", "reason AutoClear"})
		require(clear.find(field) != std::string::npos,
			(std::string("the clear announcement omits a payload field: ") + field).c_str());
	require(clear.find("value " + std::format("{}", cleared.currentValue)) != std::string::npos,
		"the clear announcement's value disagrees with the closed-out ledger entry");

	// Fire again, then close out by disabling: the reason on the stream is the
	// reason in the ledger, for a close the Monitor did not earn by recovering.
	settings.atmospheres[0].volumeM3 = 100;
	require(run.updateSettings(settings) && run.advanceTo(120) && run.snapshot().activeAlerts.size() == 1,
		"the Monitor did not re-fire after recovery");
	settings.monitors[0].enabled = false;
	require(run.updateSettings(settings) && run.snapshot().activeAlerts.empty(),
		"disabling did not close the active Alert");
	auto const final = alertEvents(run);
	require(final.size() == 4 && final[3].message.find("Alert cleared 'oxygen-low'") != std::string::npos
		&& final[3].message.find("reason MonitorDisabled") != std::string::npos,
		"a disabled Monitor's close-out is not explained on the event stream");

	// Lockstep: every Alert the ledger holds was announced, and nothing was
	// recorded without a matching announcement. A direct-write bypass would
	// leave a ledger entry the stream never saw.
	auto const snap = run.snapshot();
	require(countAnnouncements(final, "fired")
		== static_cast<int>(snap.activeAlerts.size() + snap.closedAlerts.size()),
		"the Alert ledger and the event stream are not in lockstep");
	require(snap.activeAlerts.empty() && snap.closedAlerts.size() == 2, "unexpected ledger shape at the end");
}
void registerStationMonitorChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"monitorTiming", monitorTiming});
	checks.push_back({"monitorCommands", monitorCommands});
	checks.push_back({"monitorValidation", monitorValidation});
	checks.push_back({"monitorPersistence", monitorPersistence});
	checks.push_back({"monitorDemo", monitorDemo});
	checks.push_back({"monitorDefaultTable", monitorDefaultTable});
	checks.push_back({"monitorDefaultBoundaries", monitorDefaultBoundaries});
	checks.push_back({"monitorCallbackStream", monitorCallbackStream});
}
