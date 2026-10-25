#include "StationRig.h"
#include "Smoke.h"
#include "core/StationConfigurationDocument.h"
#include "core/WorldDocument.h"
#include "core/BinarySerializer.h"

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
}
void registerStationMonitorChecks(std::vector<smoke::Check>& checks)
{
	checks.push_back({"monitorTiming", monitorTiming});
	checks.push_back({"monitorCommands", monitorCommands});
	checks.push_back({"monitorValidation", monitorValidation});
	checks.push_back({"monitorPersistence", monitorPersistence});
	checks.push_back({"monitorDemo", monitorDemo});
}
