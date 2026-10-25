#include "Checks.h"
#include "ImGuiContext.h"
#include "TestEngine.h"
#include "StationSystemsWindow.h"
#include "StationFaultPanel.h"
#include "StationFlowDiagram.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>

// #849: the editor surface for the degradation-and-failure framework.
// These checks drive the real Flow canvas, the real inspector and the real
// alarm panel through the ImGui Test Engine, and assert on what the operator
// would see: badges on the node where the machine is drawn, the alarm list
// ordered by derived impact rather than by authored severity, and the
// opt-in authoring controls round-tripping through undo, redo and save.
//
// Every fault command is issued through the World facade - the same
// validated surface a headless client drives - so a check that the editor
// bypassed it would have to fail these assertions.
namespace
{
	// A coolant-loop rig whose leak, Condition, Wear and repair-parts wiring
	// are all authored in, so the badge, the alarm and the repair tier all
	// have a real machine to act on.
	core::StationSystemsSettings faultLoopRig()
	{
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		auto store = [](std::string const& id, std::string const& unit, double capacity,
			double qualified) {
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = unit;
			entry.capacity = capacity; entry.startingQualified = qualified;
			return entry;
		};
		settings.stores = {
			store("parts", "kg", 100.0, 100.0),
			store("waste", "kg", 100.0, 0.0),
		};
		core::AtmosphereSettings compartment;
		compartment.id = "comp";
		compartment.volumeM3 = 500.0;
		compartment.startingOxygenKg = 100.0;
		compartment.startingNitrogenKg = 300.0;
		compartment.thermalCapacityKJPerK = 500.0;
		compartment.minPressureKPa = 1.0;
		compartment.maxPressureKPa = 200.0;
		compartment.minOxygenPartialPressureKPa = 1.0;
		compartment.maxCarbonDioxidePartialPressureKPa = 20.0;
		settings.atmospheres = {compartment};
		core::ThermalReservoirSettings host;
		host.id = "host"; host.thermalClass = "low";
		host.heatCapacityKJPerK = 10000.0; host.startingTemperatureC = 40.0;
		settings.thermalReservoirs = {host};
		core::CoolantLoopSettings loop;
		loop.id = "loop1"; loop.reservoirId = "host";
		loop.coolantCapacityKg = 100.0; loop.startingCoolantKg = 100.0;
		loop.specificHeatKJPerKgK = 4.0; loop.ratedFlowKgPerS = 1.0;
		loop.pumpPowerKW = 1.0;
		loop.exchangerConductanceKWPerK = 5.0;
		core::CoolantLeakSettings leak;
		leak.compartmentId = "comp"; leak.releasedKg = 25.0;
		loop.coolantLeak = leak;
		core::RepairPartsSettings parts;
		parts.partsStoreId = "parts";
		core::FaultRepairSettings plan;
		plan.modeId = "coolant-leak"; plan.durationSeconds = 5.0;
		plan.partsQuantity = 1.0; plan.wasteStoreId = "waste";
		parts.repairs = {plan};
		loop.repairParts = parts;
		settings.coolantLoops = {loop};
		return settings;
	}

	bool hasFault(std::vector<station_flow::FaultBadge> const& badges, std::string const& mode)
	{
		return std::any_of(badges.begin(), badges.end(),
			[&](station_flow::FaultBadge const& badge) { return badge.modeId == mode; });
	}

	// The alarm panel row for one (installation, mode), if it is listed.
	StationFaultPanel::Row const* alarmRow(StationSystemsWindow const& window,
		std::string const& installationId, std::string const& mode)
	{
		for (auto const& row : window.faultPanelRows())
			if (row.fault.installationId == installationId && row.fault.modeId == mode) return &row;
		return nullptr;
	}

	// --- Badges, live injection through the inspector, and the repair tier.
	void faultBadgeWorkflow(smoke::Context const& context)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto const path = context.temporaryRoot() / "fault-badge.station.yaml";
		window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
		auto world = std::make_shared<core::World>("Fault badge", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		auto settings = faultLoopRig();
		settings.atmospheres[0].fire = core::AtmosphereFireSettings{1, 0.1, 1, 20, 99.9};
		require(world->configureStationSystems(settings, false, &diagnostic),
			("configure fault rig: " + diagnostic).c_str());
		world->resumeSimulation();
		require(world->advanceTicks(120), "the fault fixture did not tick");
		world->markSaved();
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			window.renderFlowContents(world);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "faultBadge", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			ImVec4 loopRect{};
			REQ(window.flowNodeRect("Coolant loop", "loop1", &loopRect),
				"the coolant loop node is not on the canvas");
			// Settle the pointer on empty space first, then land on the node
			// and move off it again: a hover tooltip would otherwise stand in
			// for the inspector text in the frame capture.
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			ctx->MouseTeleportToPos({(loopRect.x + loopRect.z) * 0.5f,
				(loopRect.y + loopRect.w) * 0.5f}); ctx->Yield(2);
			ctx->MouseClick(); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			REQ(driver.contains("Node: Coolant loop 'loop1'"),
				"selecting the coolant loop did not name it in the inspector");
			// The machine opted into a fault mode, so the fault section is
			// there - but nothing is broken yet, and it says so.
			REQ(driver.contains("Faults")
				&& driver.contains("No active Fault on this Installation."),
				"the inspector's fault section did not open clean");
			REQ(window.flowNodeFaults("Coolant loop", "loop1").empty(),
				"a badge was drawn with no active Fault");
			// Inject through the inspector. The editor never reaches the
			// station runtime directly: this is the World command surface.
			REQ(driver.contains("Inject fault mode"),
				"the injectable-mode control is missing for an opted-in machine");
			driver.click(ctx, "**/Inject"); ctx->Yield(2);
			REQ(!window.flowNodeFaults("Coolant loop", "loop1").empty()
				|| !world->getStationSystemsSnapshot().coolantLoops.empty(),
				"the inject control did not reach the World");
			// The command lands at the next Station boundary; pump frames so
			// the view's 4 Hz poll sees the new tick.
			for (int i = 0; i < 40; ++i)
			{
				world->advanceTicks(6);
				ctx->Yield(1);
			}
			auto const badged = window.flowNodeFaults("Coolant loop", "loop1");
			REQ(hasFault(badged, "coolant-leak"),
				"the node did not badge the injected Fault");
			REQ(badged.front().nodeType == "Coolant loop"
				&& badged.front().installationId == "loop1"
				&& badged.front().causeKind == core::FaultCause::Kind::Editor,
				"the badge lost the node identity or the Editor cause attribution");
			// The toolbar button carries the count, so an alarm is visible
			// with the panel closed.
			ctx->Yield(2);
			REQ(driver.contains("Fault alarms (1)"),
				"the alarm button did not count the active Fault");
			float alarmX = 0.0f, alarmY = 0.0f;
			REQ(driver.itemCenter(ctx, "**/Fault alarms (1)", &alarmX, &alarmY),
				"the alarm button could not be resolved by its counted label");
			ctx->MouseTeleportToPos({alarmX, alarmY}); ctx->Yield(2);
			ctx->MouseClick(); ctx->Yield(3);
			REQ(window.faultPanelVisible(),
				"the alarm button did not open the panel");
			ctx->SetRef("Station faults");
			REQ(driver.contains("Active alarms"), "the alarm panel did not open");
			auto const* row = alarmRow(window, "loop1", "coolant-leak");
			REQ(row != nullptr, "the alarm panel did not list the active Fault");
			REQ(row->repairable && !row->resettable,
				"a plain Fault is not repairable, or a non-Trip was offered as a Trip");
			// The repair tier: a funded, job-shaped repair started from the
			// panel, through the same command a headless client issues. The
			// row is re-read after the click: the panel rebuilds its rows from
			// the snapshot every frame, so a pointer held across a frame is
			// not a live row.
			driver.click(ctx, "**/Repair"); ctx->Yield(3);
			REQ(window.faultPanelNotice().empty(),
				("the panel's repair command was refused: " + window.faultPanelNotice()).c_str());
			auto const* started = alarmRow(window, "loop1", "coolant-leak");
			REQ(started != nullptr && !started->repairJobId.empty(),
				"the repair did not report the job id it started");
			// Let the five-second repair run. The Fault clears when it
			// completes, and the badge goes with it without the panel being
			// told anything.
			for (int i = 0; i < 60; ++i)
			{
				world->advanceTicks(12);
				ctx->Yield(1);
			}
			REQ(!hasFault(window.flowNodeFaults("Coolant loop", "loop1"), "coolant-leak"),
				"the badge survived the completed repair");
			REQ(alarmRow(window, "loop1", "coolant-leak") == nullptr,
				"the alarm panel still lists the repaired Fault");
			// The transitions the panel lists are the framework's own
			// categorised events, newest first.
			auto const& transitions = window.faultPanelTransitions();
			REQ(std::any_of(transitions.begin(), transitions.end(),
				[](std::string const& line) { return line.find("[fault]") != std::string::npos; }),
				"the panel listed no fault transition after the onset");
			REQ(std::any_of(transitions.begin(), transitions.end(),
				[](std::string const& line) { return line.find("[repair]") != std::string::npos; }),
				"the panel listed no repair transition after the repair");
			REQ(world->injectStationFault("comp", "fire", {}), "fire injection refused");
			for (int i = 0; i < 40; ++i)
			{
				world->advanceTicks(6);
				ctx->Yield(1);
			}
			REQ(alarmRow(window, "comp", "fire") == nullptr,
				"the alarm panel retained an extinguished fire");
			REQ(std::any_of(window.faultPanelTransitions().begin(), window.faultPanelTransitions().end(),
				[](std::string const& line) { return line.find("[fault-extinguished]") != std::string::npos; }),
				"the panel hid the fire extinction transition");
		});
		#undef REQ
		require(driver.passed(), "the fault badge and repair workflow failed on the Test Engine");
	}

	// --- Authoring the opt-ins through the real widgets, with history.
	void faultAuthoringWorkflow(smoke::Context const& context)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto const path = context.temporaryRoot() / "fault-authoring.station.yaml";
		window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
		auto world = std::make_shared<core::World>("Fault authoring", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		// A bare loop: no Condition, no Wear, no fault mode, no spares. The
		// whole point is that every block below is added through the editor.
		core::StationSystemsSettings seed;
		seed.enabled = false;
		core::ThermalReservoirSettings host;
		host.id = "host"; host.thermalClass = "low"; host.heatCapacityKJPerK = 10000.0;
		seed.thermalReservoirs = {host};
		core::CoolantLoopSettings loop;
		loop.id = "loop1"; loop.reservoirId = "host";
		loop.coolantCapacityKg = 100.0; loop.startingCoolantKg = 100.0;
		loop.specificHeatKJPerKgK = 4.0; loop.ratedFlowKgPerS = 1.0;
		seed.coolantLoops = {loop};
		std::string diagnostic;
		require(world->configureStationSystems(seed, false, &diagnostic),
			("configure seed: " + diagnostic).c_str());
		world->markSaved();
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			window.renderFlowContents(world);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "faultAuthoring", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			ImVec4 loopRect{};
			REQ(window.flowNodeRect("Coolant loop", "loop1", &loopRect),
				"the coolant loop node is not on the canvas");
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			ctx->MouseTeleportToPos({(loopRect.x + loopRect.z) * 0.5f,
				(loopRect.y + loopRect.w) * 0.5f}); ctx->Yield(2);
			ctx->MouseClick(); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			REQ(window.draft().coolantLoops.size() == 1
				&& !window.draft().coolantLoops[0].condition
				&& !window.draft().coolantLoops[0].wear
				&& !window.draft().coolantLoops[0].coolantLeak
				&& !window.draft().coolantLoops[0].repairParts,
				"the seed loop should start with no fault opt-ins");
			auto const start = window.draft();
			auto const undoStart = window.draftHistory().undoCount();
			// Enabling a block seeds the core's declared defaults, so the
			// block is valid the moment it exists.
			driver.click(ctx, "**/Fault: coolant-leak##Coolant loop 0"); ctx->Yield(3);
			// The opt-in carries a hover tooltip; parked on the box it hides
			// the fields it just revealed from the frame capture.
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			REQ(window.draft().coolantLoops[0].coolantLeak.has_value()
				&& window.draft().coolantLoops[0].coolantLeak->compartmentId.empty(),
				"enabling the leak did not add a default block");
			driver.typeValue(ctx, "**/leak released kg##Coolant loop 0", "18");
			ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			REQ(window.draft().coolantLoops[0].coolantLeak->releasedKg == 18.0,
				"the leak size did not reach the draft");
			driver.click(ctx, "**/Condition model##Coolant loop 0"); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			driver.typeValue(ctx, "**/seizure threshold##Coolant loop 0", "0.25");
			ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			REQ(window.draft().coolantLoops[0].condition.has_value()
				&& window.draft().coolantLoops[0].condition->seizureThreshold == 0.25,
				"the Condition spawn threshold did not reach the draft");
			driver.click(ctx, "**/Wear: pump wear##Coolant loop 0"); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			driver.typeValue(ctx, "**/fade per operating hour##Coolant loop 0", "0.01");
			ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			REQ(window.draft().coolantLoops[0].wear.has_value()
				&& window.draft().coolantLoops[0].wear->fadePerOperatingHour == 0.01,
				"the wear rate did not reach the draft");
			// The repair-parts wiring, then a plan aimed at the mode that is
			// now injectable on this machine.
			driver.click(ctx, "**/Repair-parts wiring##Coolant loop 0"); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			REQ(window.draft().coolantLoops[0].repairParts.has_value()
				&& window.draft().coolantLoops[0].repairParts->repairs.empty(),
				"enabling the repair wiring did not add an empty plan set");
			driver.click(ctx, "**/Add repair plan to Coolant loop 0"); ctx->Yield(3);
			REQ(window.draft().coolantLoops[0].repairParts->repairs.size() == 1,
				"the add-plan control did not add a plan");
			// Undo every authored field, then redo it all: the new fields are
			// document state like any other, not a side channel. Seven edits
			// went in, so the walk back is a loop rather than one press.
			auto const authored = window.draft();
			for (int i = 0; i < 40 && window.draft() != start; ++i)
			{
				driver.click(ctx, "**/Undo configuration"); ctx->Yield(2);
			}
			REQ(window.draft() == start,
				"undo did not walk the authored fault opt-ins back to the seed");
			REQ(window.draftHistory().undoCount() == undoStart,
				"undo left the draft history out of step with the seed");
			for (int i = 0; i < 40 && window.draft() != authored; ++i)
			{
				driver.click(ctx, "**/Redo configuration"); ctx->Yield(2);
			}
			REQ(window.draft() == authored,
				"redo did not restore the authored fault opt-ins");
			REQ(authored.coolantLoops[0].coolantLeak.has_value()
				&& authored.coolantLoops[0].condition.has_value()
				&& authored.coolantLoops[0].wear.has_value()
				&& authored.coolantLoops[0].repairParts.has_value()
				&& authored.coolantLoops[0].repairParts->repairs.size() == 1,
				"the redrafted configuration lost a fault opt-in block");
			// The plan's mode list is the machine's own injectable set: the
			// leak enabled above is offered, a mode this machine never enabled
			// is not. This runs last because opening the combo carries the
			// test context's item scope away with it.
			// test context's item scope away with it, so it comes after every
			// other click in this check.
			// Draft editing never mutates the World.
			REQ(world->getStationSystemsSettings() == seed && !world->isModified(),
				"fault authoring mutated the running World");
			// Save and reload: the opt-ins travel in the document.
			ctx->SetRef("Configuration");
			driver.click(ctx, "Save configuration"); ctx->Yield(3);
			REQ(std::filesystem::exists(path) && !window.flowModified(),
				"the authored fault opt-ins did not save");
			ctx->SetRef("Configuration");
			driver.click(ctx, "New configuration"); ctx->Yield(3);
			REQ(window.draft().coolantLoops.empty(), "New did not clear the draft");
			ctx->SetRef("Configuration");
			driver.click(ctx, "Open configuration"); ctx->Yield(3);
			REQ(window.draft() == authored,
				"the save/load round trip lost the authored fault opt-ins");
		});
		#undef REQ
		require(driver.passed(), "the fault opt-in authoring workflow failed on the Test Engine");
	}

	// --- A latching Trip reads differently from a plain Fault, and the
	// free reset is offered on the Trip and on nothing else.
	void faultTripResetWorkflow(smoke::Context const& context)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Fault trip", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		auto store = [](std::string const& id) {
			core::ResourceStoreSettings entry;
			entry.id = id; entry.unit = "kg";
			entry.capacity = 1000.0; entry.startingQualified = 500.0;
			return entry;
		};
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.stores = {store("co2"), store("h2"), store("methane"), store("rawWater")};
		core::ThermalReservoirSettings sink;
		sink.id = "sink"; sink.thermalClass = "low";
		sink.heatCapacityKJPerK = 10000.0; sink.startingTemperatureC = 10.0;
		settings.thermalReservoirs = {sink};
		core::SabatierReactorSettings reactor;
		reactor.id = "sabatier";
		reactor.co2StoreId = "co2"; reactor.hydrogenStoreId = "h2";
		reactor.methaneStoreId = "methane"; reactor.waterStoreId = "rawWater";
		reactor.thermalClass = "low"; reactor.heatSinkId = "sink";
		reactor.rateKgPerS = 0.001; reactor.specificEnergyKWhPerKgCO2 = 1.0;
		reactor.maxSinkTemperatureC = 60.0;
		core::OvertempRunawaySettings runaway;
		runaway.triggerTemperatureC = 80.0; runaway.heatSpikeKJ = 1.0;
		reactor.overtempRunaway = runaway;
		settings.sabatierReactors = {reactor};
		std::string diagnostic;
		require(world->configureStationSystems(settings, false, &diagnostic),
			("configure trip rig: " + diagnostic).c_str());
		world->resumeSimulation();
		require(world->advanceTicks(120), "the trip fixture did not tick");
		world->markSaved();
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			window.renderFlowContents(world);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "faultTripReset", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			ImVec4 reactorRect{};
			REQ(window.flowNodeRect("Sabatier", "sabatier", &reactorRect),
				"the Sabatier node is not on the canvas");
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			ctx->MouseTeleportToPos({(reactorRect.x + reactorRect.z) * 0.5f,
				(reactorRect.y + reactorRect.w) * 0.5f}); ctx->Yield(2);
			ctx->MouseClick(); ctx->Yield(3);
			ctx->MouseTeleportToPos({40.0f, 40.0f}); ctx->Yield(2);
			driver.click(ctx, "**/Inject"); ctx->Yield(2);
			for (int i = 0; i < 40; ++i)
			{
				world->advanceTicks(6);
				ctx->Yield(1);
			}
			auto const badged = window.flowNodeFaults("Sabatier", "sabatier");
			REQ(hasFault(badged, "overtemp-runaway"),
				"the Sabatier node did not badge the injected Trip");
			REQ(!badged.empty() && badged.front().latched,
				"overtemp-runaway did not read as a latched Trip");
			float tripAlarmX = 0.0f, tripAlarmY = 0.0f;
			REQ(driver.itemCenter(ctx, "**/Fault alarms (1)", &tripAlarmX, &tripAlarmY),
				"the trip alarm button could not be resolved by its counted label");
			ctx->MouseTeleportToPos({tripAlarmX, tripAlarmY}); ctx->Yield(2);
			ctx->MouseClick(); ctx->Yield(3);
			REQ(window.faultPanelVisible(), "the alarm panel did not open for the Trip");
			ctx->SetRef("Station faults");
			auto const* row = alarmRow(window, "sabatier", "overtemp-runaway");
			REQ(row != nullptr, "the alarm panel did not list the latched Trip");
			REQ(row->resettable,
				"the alarm panel did not offer the free reset on a latched Trip");
			driver.click(ctx, "**/Reset trip"); ctx->Yield(3);
			REQ(window.faultPanelNotice().empty(),
				("the free trip reset was refused: " + window.faultPanelNotice()).c_str());
			for (int i = 0; i < 20; ++i)
			{
				world->advanceTicks(6);
				ctx->Yield(1);
			}
			REQ(!hasFault(window.flowNodeFaults("Sabatier", "sabatier"), "overtemp-runaway"),
				"the badge survived the trip reset");
			REQ(std::any_of(window.faultPanelTransitions().begin(),
				window.faultPanelTransitions().end(),
				[](std::string const& line) { return line.find("[trip]") != std::string::npos; }),
				"the panel listed no trip transition after the reset");
		});
		#undef REQ
		require(driver.passed(), "the latched trip reset workflow failed on the Test Engine");
	}
}

namespace editor_smoke
{
	void registerStationFaultSurface(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"station/faultBadgeWorkflow", faultBadgeWorkflow});
		checks.push_back({"station/faultAuthoringWorkflow", faultAuthoringWorkflow});
		checks.push_back({"station/faultTripResetWorkflow", faultTripResetWorkflow});
	}
}
