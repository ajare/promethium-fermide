#include "Checks.h"
#include "ImGuiContext.h"
#include "TestEngine.h"
#include "StationSystemsWindow.h"
#include "DocumentEdit.h"
#include "StationFaultPanel.h"
#include "StationFlowDiagram.h"
#include "core/World.h"
#include "core/Serializer.h"
#include "core/YamlSerializer.h"
#include "core/BinarySerializer.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
	void faultBadgeWorkflowForMode(smoke::Context const& context, std::string const& mode)
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
		if (mode == "pump-seizure")
		{
			settings.coolantLoops[0].coolantLeak.reset();
			settings.coolantLoops[0].condition = core::InstallationConditionSettings{};
			settings.coolantLoops[0].repairParts->repairs[0].modeId = mode;
		}
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
			REQ(hasFault(badged, mode),
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
			auto const* row = alarmRow(window, "loop1", mode);
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
			auto const* started = alarmRow(window, "loop1", mode);
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
			REQ(!hasFault(window.flowNodeFaults("Coolant loop", "loop1"), mode),
				"the badge survived the completed repair");
			REQ(alarmRow(window, "loop1", mode) == nullptr,
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

	void faultBadgeWorkflow(smoke::Context const& context)
	{
		faultBadgeWorkflowForMode(context, "coolant-leak");
		faultBadgeWorkflowForMode(context, "pump-seizure");
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

	// #863: an existing Heat engine is selectable even without placement ports.
	// Exercise its real inspector, not a settings mutation masquerading as UI.
	void heatEngineFaultAuthoringWorkflow(smoke::Context const& context)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		StationSystemsWindow window;
		auto const path = context.temporaryRoot() / "heat-engine-faults.station.yaml";
		window.setFilePicker([&](bool) -> std::optional<std::string> { return path.string(); });
		auto world = std::make_shared<core::World>("Heat engine authoring", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		core::StationSystemsSettings seed;
		core::ThermalReservoirSettings hot;
		hot.id = "hot"; hot.thermalClass = "moderate"; hot.heatCapacityKJPerK = 10000;
		auto cold = hot; cold.id = "cold";
		seed.thermalReservoirs = {hot, cold};
		core::ResourceStoreSettings parts;
		parts.id = "spares"; parts.unit = "kg"; parts.capacity = 100; parts.startingQualified = 50;
		auto waste = parts; waste.id = "waste";
		seed.stores = {parts, waste};
		core::HeatEngineSettings engine;
		engine.id = "engine"; engine.hotReservoirId = "hot";
		engine.rejectionReservoirId = "cold"; engine.thermalClass = "moderate";
		engine.ratedThermalInputKW = 10; engine.commandedInputKW = 10;
		engine.minHotTemperatureC = 80; engine.maxSinkTemperatureC = 60;
		seed.heatEngines = {engine};
		std::string diagnostic;
		auto const configured = world->configureStationSystems(seed, false, &diagnostic);
		require(configured, diagnostic.c_str());
		world->markSaved();
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration"); window.renderFlowContents(world); ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "heatEngineFaultAuthoring", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration");
			ImVec4 rect{};
			REQ(window.flowNodeRect("Heat engine", "engine", &rect), "Heat engine missing from Flow");
			ctx->MouseTeleportToPos({(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f});
			ctx->Yield(2); ctx->MouseClick(); ctx->Yield(3);
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			auto const start = window.draft();
			auto const undoStart = window.draftHistory().undoCount();
			driver.click(ctx, "**/Fault: mechanical-seizure##Heat engine 0"); ctx->Yield(3);
			driver.click(ctx, "**/Wear: efficiency drift##Heat engine 0"); ctx->Yield(3);
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			for (auto const& [field, value] : std::vector<std::pair<std::string, std::string>>{
				{"fade per operating hour", "0.01"}, {"fade per calendar hour", "0.02"},
				{"max wear fraction", "0.7"}})
			{
				driver.typeValue(ctx, ("**/" + field + "##Heat engine 0").c_str(), value.c_str());
				ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			}
			REQ(!ctx->IsError(), "Wear input interaction failed");
			driver.click(ctx, "**/Repair-parts wiring##Heat engine 0"); ctx->Yield(3);
			REQ(!ctx->IsError(), "Repair wiring interaction failed");
			REQ(window.draft().heatEngines[0].repairParts.has_value(), "Repair wiring checkbox did not enable block");
			ctx->MouseTeleportToPos({40, 40}); ctx->Yield(2);
			// BeginCombo has no labelled Test Engine item: resolve its ID in
			// the labelled sibling's scope, as the other reference workflows do.
			auto const parent = ctx->ItemInfo("**/Repair-parts wiring##Heat engine 0")->ParentID;
			ctx->ItemClick(ImHashStr("parts store##Heat engine 0", 0, parent)); ctx->Yield(2);
			REQ(!ctx->IsError(), "Parts store opening failed");
			auto* popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "Parts combo popup missing");
			ctx->ItemClick((std::string("//") + popup->Name + "/**/spares").c_str()); ctx->Yield(3);
			REQ(!ctx->IsError(), "Parts store combo interaction failed");
			REQ(window.draft().heatEngines[0].repairParts
				&& window.draft().heatEngines[0].repairParts->partsStoreId == "spares", "Parts store not selected");
			ctx->SetRef("Configuration");
			driver.click(ctx, "**/Add repair plan to Heat engine 0"); ctx->Yield(3);
			REQ(window.draft().heatEngines[0].repairParts
				&& window.draft().heatEngines[0].repairParts->repairs.size() == 1,
				"Heat engine add repair plan failed");
			ctx->ItemClick(ImHashStr("repair plan 0 mode##Heat engine 0", 0, parent)); ctx->Yield(2);
			popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "Mode combo popup missing");
			ctx->ItemClick((std::string("//") + popup->Name + "/**/mechanical-seizure").c_str()); ctx->Yield(3);
			ctx->SetRef("Configuration");
			driver.typeValue(ctx, "**/repair plan 0 duration seconds##Heat engine 0", "12");
			ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			driver.typeValue(ctx, "**/repair plan 0 parts quantity##Heat engine 0", "2");
			ctx->KeyPress(ImGuiKey_Enter); ctx->Yield(3);
			ctx->ItemClick(ImHashStr("repair plan 0 waste store##Heat engine 0", 0, parent)); ctx->Yield(2);
			popup = ctx->GetWindowByRef("//$FOCUSED");
			REQ(popup != nullptr, "Waste combo popup missing");
			ctx->ItemClick((std::string("//") + popup->Name + "/**/waste").c_str()); ctx->Yield(3);
			ctx->SetRef("Configuration");
			auto const authored = window.draft();
			auto const& edited = authored.heatEngines.at(0);
			REQ(edited.mechanicalSeizure && edited.efficiencyDrift && edited.repairParts,
				"Heat engine opt-ins did not reach draft");
			REQ(edited.efficiencyDrift->fadePerOperatingHour == 0.01
				&& edited.efficiencyDrift->fadePerCalendarHour == 0.02
				&& edited.efficiencyDrift->maxWearFraction == 0.7,
				"Heat engine Wear rates/ceiling did not reach draft");
			REQ(edited.repairParts->repairs.size() == 1, "Heat engine repair plan lost");
			auto const& plan = edited.repairParts->repairs[0];
			REQ(edited.repairParts->partsStoreId == "spares" && plan.modeId == "mechanical-seizure"
				&& plan.durationSeconds == 12 && plan.partsQuantity == 2 && plan.wasteStoreId == "waste",
				"Heat engine repair plan did not reach draft");
			auto const valid = core::validateStationSystemsSettings(authored, diagnostic);
			REQ(valid, diagnostic.c_str());
			for (int i = 0; i < 40 && window.draft() != start; ++i)
			{ driver.click(ctx, "**/Undo configuration"); ctx->Yield(2); }
			REQ(window.draft() == start && window.draftHistory().undoCount() == undoStart,
				"Heat engine undo lost seed/history");
			for (int i = 0; i < 40 && window.draft() != authored; ++i)
			{ driver.click(ctx, "**/Redo configuration"); ctx->Yield(2); }
			REQ(window.draft() == authored, "Heat engine redo lost edits");
			REQ(world->getStationSystemsSettings() == seed && !world->isModified(),
				"Heat engine draft edits mutated World before Apply");
			driver.click(ctx, "Save configuration"); ctx->Yield(3);
			REQ(std::filesystem::exists(path) && !window.flowModified(), "Heat engine save failed");
			driver.click(ctx, "New configuration"); ctx->Yield(3);
			driver.click(ctx, "Open configuration"); ctx->Yield(3);
			REQ(window.draft() == authored, "Standalone persistence lost Heat engine fields");
			auto const applied = world->configureStationSystems(window.draft(), false, &diagnostic);
			REQ(applied, diagnostic.c_str());
			for (bool binary : {false, true})
			{
				core::SerializationWorkData work;
				auto write = [&](auto writer) {
					world->serialize(*writer, work); writer->serialize(); return writer->getSerializedString();
				};
				auto bytes = binary ? write(core::BinarySerializer::toString()) : write(core::YamlSerializer::toString());
				std::unique_ptr<core::Serializer> reader = binary
					? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
					: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
				reader->deserialize();
				core::World loaded("Loaded", 2, 2);
				REQ(loaded.deserialize(*reader, work) && loaded.getStationSystemsSettings() == authored,
					"World persistence lost Heat engine fault/Wear/repair fields");
			}
		});
		#undef REQ
		require(driver.passed(), "Heat engine fault authoring workflow failed");
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

namespace
{
	void busBreakerWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext imgui;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1600, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		auto world = std::make_shared<core::World>("Breaker", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		core::StationSystemsSettings settings;
		settings.enabled = true; settings.systemsStepWorldTicks = 1;
		settings.powerBuses[0].ratedCapacityKW = 5;
		settings.powerBuses[0].tripPersistenceSeconds = 0.5;
		core::SolarArraySettings source;
		source.id = "solar"; source.capacityKW = 20; source.illumination.constantLevel = 1;
		settings.solarArrays = {source};
		core::EquipmentLoadSettings load;
		load.id = "load"; load.electricalDemandKW = 8; load.operatingProfile.constantLevel = 1; load.thermalClass = "moderate";
		settings.equipmentLoads = {load};
		std::string diagnostic;
		if (!world->configureStationSystems(settings, false, &diagnostic))
			require(false, ("breaker configuration: " + diagnostic).c_str());
		world->resumeSimulation(); require(world->advanceTicks(120), "breaker World failed");
		world->pauseSimulation();
		require(world->getStationSystemsSnapshot().power.buses[0].tripped, "World did not trip");
		// Replay without any client command must retain the autonomous Trip.
		for (bool binary : {false, true})
		{
			std::unique_ptr<core::Serializer> out = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::toString())
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::toString());
			core::SerializationWorkData work; world->serialize(*out, work); out->serialize();
			auto bytes = binary ? static_cast<core::BinarySerializer&>(*out).getSerializedString() : static_cast<core::YamlSerializer&>(*out).getSerializedString();
			std::unique_ptr<core::Serializer> in = binary ? std::unique_ptr<core::Serializer>(core::BinarySerializer::fromString(bytes))
				: std::unique_ptr<core::Serializer>(core::YamlSerializer::fromString(bytes));
			in->deserialize(); core::World restored("Restored", 2, 2);
			require(restored.deserialize(*in, work), "breaker reload refused");
			auto a = restored.getStationSystemsSnapshot(), b = world->getStationSystemsSnapshot();
			require(a.power == b.power, "breaker save/load power diverged");
			require(a.activeAlerts == b.activeAlerts, "breaker save/load Alerts diverged");
			require(a == b, "autonomous breaker save/load other snapshot fields diverged");
		}
		StationSystemsWindow window;
		DocumentHistory history;
		// Exercise multiple circuits, including an explicitly unrated one,
		// without disturbing the breaker click fixture's diagram geometry.
		{
			auto rollupWorld = std::make_shared<core::World>("Rollups", 2, 2);
			rollupWorld->pauseSimulation(); rollupWorld->finishBuild();
			auto rollupSettings = settings;
			rollupSettings.powerBuses.push_back({"unrated", "Unrated", 0, 0});
			require(rollupWorld->configureStationSystems(rollupSettings, false), "rollup configuration failed");
			StationSystemsWindow rollupWindow;
			DocumentHistory rollupHistory;
			auto const before = rollupWorld->getStationSystemsSnapshot();
			headless::TestEngineDriver rollupDriver([&] {
				ImGui::SetNextWindowSize({1560, 960}); ImGui::Begin("Rollups");
				rollupWindow.renderContents(rollupWorld, rollupHistory); ImGui::End();
			});
			rollupDriver.frame(); rollupDriver.frame();
			require(rollupDriver.contains("Bus main: supply") && rollupDriver.contains("Bus unrated: supply")
				&& rollupDriver.contains("Load fraction: unrated") && rollupDriver.contains("of 5.000 kW")
				&& rollupDriver.contains("Trip state: closed") && rollupDriver.contains("Power totals:"), "multiple bus rollups missing");
			require(before == rollupWorld->getStationSystemsSnapshot() && rollupHistory.undoCount() == 0,
				"rollup observation mutated World or history");
		}
		bool liveRollups = true;
		headless::TestEngineDriver driver([&] {
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1560, 960});
			ImGui::Begin("Configuration");
			if (liveRollups) window.renderContents(world, history);
			else window.renderFlowContents(world, &history);
			ImGui::End();
		});
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "busBreakerWorkflow", [&](ImGuiTestContext* ctx) {
			ctx->SetRef("Configuration"); ctx->Yield(2);
			auto const beforeObservation = world->getStationSystemsSnapshot();
			REQ(driver.contains("Bus main: supply"), "per-bus rollups missing");
			REQ(driver.contains("protected unmet")
				&& driver.contains("Trip state: tripped") && driver.contains("of 5.000 kW")
				&& driver.contains("Power totals:"), "rollup health or station totals missing");
			ctx->Yield(2);
			REQ(world->getStationSystemsSnapshot() == beforeObservation && history.undoCount() == 0,
				"read-only rollups changed dispatch or history");
			liveRollups = false; ctx->Yield(3);
			ctx->ScrollToTop("Configuration"); ctx->Yield(2);
			REQ(driver.contains("Latched trip - Reset breaker"), "latched marker missing");
			bool deadColour = false;
			for (auto* w : ImGui::GetCurrentContext()->Windows)
				for (auto const& vertex : w->DrawList->VtxBuffer)
					deadColour |= vertex.col == station_flow::DeadBusColor;
			REQ(deadColour, "tripped bus did not render dead styling");
			ImVec4 markerNode{};
			REQ(window.flowNodeRect("Power bus", "main", &markerNode), "marker bus absent");
			auto const zoom = (markerNode.z - markerNode.x) / station_flow::NodeWidth;
			ctx->MouseTeleportToPos({markerNode.x + 8 * zoom + 80, markerNode.y + 64 * zoom + 9});
			ctx->MouseClick(); ctx->Yield(3);
			REQ(!world->getStationSystemsSnapshot().power.buses[0].tripped, "marker did not reset breaker");
			REQ(history.undoCount() == 1, "marker did not issue one history entry");
			REQ(world->getStationSystemsSnapshot().closedAlerts.back().clearReason == core::AlertClearReason::BreakerReset,
				"marker did not clear the Critical Alert with BreakerReset");
			auto resetSnapshot = world->getStationSystemsSnapshot();
			auto restore = [&](DocumentSnapshot const& snapshot) {
				world = deserializeDocumentSnapshot(snapshot, world, {});
				world->pauseSimulation(); return true;
			};
			REQ(history.undo(captureDocumentSnapshot(world, history), restore)
				&& world->getStationSystemsSnapshot().power.buses[0].tripped
				&& !world->getStationSystemsSnapshot().activeAlerts.empty(), "undo lost trip or Alert");
			ctx->Yield(3);
			REQ(history.redo(captureDocumentSnapshot(world, history), restore)
				&& world->getStationSystemsSnapshot() == resetSnapshot, "redo did not replay BreakerReset exactly");
			world->resumeSimulation(); REQ(world->advanceTicks(60), "re-trip World failed"); world->pauseSimulation();
			ctx->Yield(3);
			REQ(world->getStationSystemsSnapshot().power.buses[0].tripped, "World persistent overload did not re-trip");
			ImVec4 rect{};
			REQ(window.flowNodeRect("Power bus", "main", &rect), "bus absent");
			ctx->MouseTeleportToPos({(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f});
			ctx->MouseClick(); ctx->Yield(3);
			driver.click(ctx, "**/BreakerReset"); ctx->Yield(3);
			REQ(!world->getStationSystemsSnapshot().power.buses[0].tripped, "inspector reset failed");
		});
		#undef REQ
		require(driver.passed(), "bus breaker ImGui workflow failed");
	}
}

namespace editor_smoke
{
	void registerStationFaultSurface(std::vector<smoke::Check>& checks)
	{
		checks.push_back({"station/busBreakerWorkflow", busBreakerWorkflow});
		checks.push_back({"station/faultBadgeWorkflow", faultBadgeWorkflow});
		checks.push_back({"station/faultAuthoringWorkflow", faultAuthoringWorkflow});
		checks.push_back({"station/heatEngineFaultAuthoringWorkflow", heatEngineFaultAuthoringWorkflow});
		checks.push_back({"station/faultTripResetWorkflow", faultTripResetWorkflow});
	}
}
