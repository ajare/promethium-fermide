#include "Checks.h"
#include "TestEngine.h"
#include "State.h"
#include "StationSystemsWindow.h"
#include "DocumentEdit.h"
#include "../CoupledRig.h"
#include "core/World.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

namespace
{
	void storeWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Store editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems({true, 1.0f, {
			{"o2", "kg", 1000.0, 800.0, 100.0}, {"water", "L", 500.0, 0.0, 250.0}}},
			false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "storeWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			// The separate window shows units, usable versus held inventory,
			// capacity and initial versus current state.
			REQ(driver.contains("o2 (kg) [normal]: usable 800.000, held 100.000 of capacity 1000.000 | initial usable 800.000, initial held 100.000"),
				"Store live display missing identity, units, usable/held or capacity");
			REQ(driver.contains("water (L) [normal]: usable 0.000, held 250.000 of capacity 500.000 | initial usable 0.000, initial held 250.000"),
				"Empty-usable store display wrong");
			REQ(driver.contains("2 resource store(s)"), "Authored store count missing");
			// Author one more store through the real controls, then edit it.
			ctx->ItemClick("Add store");
			REQ(driver.contains("Draft differs from the authored start conditions"),
				"Added draft store not reported as a change");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores.size() == 3 && history.undoCount() == 1,
				"Added store did not apply or commit history");
			ctx->ItemInputValue("Store 2 starting usable", "30");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores[2].startingQualified == 30.0
				&& world->getStationSystemsSnapshot().stores[2].currentQualified == 30.0
				&& history.undoCount() == 2, "Edited starting usable did not reach the live store");
			// Invalid values are refused before running, with a visible diagnostic
			// and no mutation of the authored document or history.
			ctx->ItemInputValue("Store 2 capacity", "0");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("capacity must be finite and positive"),
				"Invalid store capacity diagnostic not shown");
			REQ(world->getStationSystemsSettings().stores[2].capacity == 100.0
				&& history.undoCount() == 2, "Invalid store edit mutated state");
			ctx->ItemInputValue("Store 2 capacity", "75");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores[2].capacity == 75.0
				&& history.undoCount() == 3, "Recovery capacity edit did not apply");
			// The authored hazard class (#412) is editable through the real
			// controls and reaches the live store table.
			driver.typeValue(ctx, "Store 2 hazard class", "hazardous");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores[2].hazardClass == "hazardous",
				"Store hazard class edit did not apply");
			// An unknown hazard class is refused with a diagnostic and no
			// mutation of the authored settings.
			driver.typeValue(ctx, "Store 2 hazard class", "toxic");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("unknown hazard class 'toxic'"),
				"Unknown store hazard class diagnostic not shown");
			REQ(world->getStationSystemsSettings().stores[2].hazardClass == "hazardous"
				&& history.undoCount() == 4,
				"Invalid store hazard class draft mutated the authored settings");
			driver.click(ctx, "Remove store 2");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().stores.size() == 2
				&& world->getStationSystemsSnapshot().stores.size() == 2
				&& history.undoCount() == 5, "Removed store did not apply");
			// Running keeps the live display and never edits the authored document.
			world->markSaved();
			world->resumeSimulation();
			REQ(world->advanceTicks(60), "Store editor run failed");
			ctx->Sleep(1);
			REQ(driver.contains("o2 (kg) [normal]: usable 800.000")
				&& !world->isModified(), "Run changed the display source or dirtied the document");
			// Undo restores the authored store set without resurrecting live state.
			world->pauseSimulation();
			auto restore = [&](DocumentSnapshot const& snapshot)
			{ world = deserializeDocumentSnapshot(snapshot, world, {}); world->pauseSimulation(); return true; };
			REQ(history.undo(captureDocumentSnapshot(world, history), restore)
				&& world->getStationSystemsSettings().stores.size() == 3,
				"Undo did not restore the authored store set");
			REQ(world->getStationSystemsSnapshot().worldTick == 0
				&& world->getStationSystemsSnapshot().stores.size() == 3,
				"History undo resurrected live station state instead of reconstructing stores");
		});
		#undef REQ
		require(driver.passed(), "Store workflow failed on the Test Engine");
	}

	void workflow(smoke::Context const&)
	{
		using smoke::require;
		editor_smoke::State state;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Station editor", 2, 2); world->pauseSimulation(); world->finishBuild();
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 850});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "windowWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(ImGui::FindWindowByName("Station Systems") != nullptr
				&& driver.contains("Active World: Station editor"),
				"Separate top-level station window missing active World");
			REQ(driver.contains("Draft matches the authored start conditions"),
				"Unchanged draft state not reported");
			ctx->ItemClick("Enable Station systems");
			REQ(driver.contains("Draft differs from the authored start conditions"),
				"Draft change not reported");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().enabled && history.undoCount() == 1,
				"Enable did not commit document history");
			REQ(driver.contains("Document: unsaved changes"), "Dirty state not shown after applying settings");
			world->markSaved(); ctx->Sleep(1);
			REQ(driver.contains("Document: saved"), "Saved state not shown after markSaved");
			world->resumeSimulation();
			REQ(world->advanceTicks(60), "Editor station did not tick");
			ctx->Sleep(1);
			REQ(driver.contains("Live station state: Enabled") && driver.contains("60 completed ticks"),
				"Station live results not observable");
			auto const before = world->getStationSystemsSnapshot();
			// Close via the real top-level ImGui close control, not disablement:
			// a real press at the close-button coordinate, letting ImGui handle it.
			auto* imguiWindow = ImGui::FindWindowByName("Station Systems");
			ctx->MouseTeleportToPos(ImVec2(imguiWindow->Pos.x + imguiWindow->Size.x - 15,
				imguiWindow->Pos.y + 10));
			ctx->MouseClick();
			ctx->Sleep(1);
			REQ(!window.visible && world->getStationSystemsSettings().enabled
				&& world->getStationSystemsSnapshot() == before,
				"Closing window changed station simulation");
			REQ(world->advanceTick(), "Hidden station stopped ticking");
			window.visible = true; world->pauseSimulation(); ctx->Sleep(1);
			ctx->ItemClick("Enable Station systems"); ctx->ItemClick("Cancel edits");
			REQ(world->getStationSystemsSettings().enabled && history.undoCount() == 1,
				"Cancel changed live/authored state");
			ctx->ItemClick("Enable Station systems"); ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().enabled && world->getSimulationTick() == 61
				&& driver.contains("Confirm?"), "Apply bypassed reset confirmation");
			ctx->ItemClick("Cancel reset");
			REQ(world->getSimulationTick() == 61 && history.undoCount() == 1,
				"Reset cancellation mutated state");
			ctx->ItemClick("Apply settings"); ctx->ItemClick("Confirm World reset");
			REQ(!world->getStationSystemsSettings().enabled && world->getSimulationTick() == 0
				&& history.undoCount() == 2,
				"Confirmed setting did not reset World and commit history");
			auto restore = [&](DocumentSnapshot const& snapshot)
			{ world = deserializeDocumentSnapshot(snapshot, world, {}); world->pauseSimulation(); return true; };
			REQ(history.undo(captureDocumentSnapshot(world, history), restore)
				&& world->getStationSystemsSettings().enabled, "Station undo lost configuration");
			REQ(world->getStationSystemsSnapshot().worldTick == 0, "History resurrected live station Lua");
			REQ(history.redo(captureDocumentSnapshot(world, history), restore)
				&& !world->getStationSystemsSettings().enabled, "Station redo failed");
			world = std::make_shared<core::World>("Second World", 2, 2); world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("Active World: Second World")
				&& driver.contains("Live station state: Disabled"),
				"Window leaked state across Worlds");
			struct SourceScope { ~SourceScope() { core::setStationSystemsSourceLoader({}); } } sourceScope;
			core::setStationSystemsSourceLoader([] {
				return "return function(name) return {advance=function() return 0/0 end} end";
			});
			world->finishBuild();
			REQ(world->configureStationSystems({true, 1}), "Could not configure failure fixture");
			world->resumeSimulation();
			REQ(!world->advanceTick() && !world->getStationSystemsSnapshot().healthy,
				"Failure fixture did not reach station failure");
			ctx->Sleep(1);
			REQ(driver.contains("Live station state: Failed")
				&& driver.contains("World status: Paused")
				&& driver.contains("Invalid output/accounting")
				&& driver.contains("non-finite")
				&& driver.contains("Reset the World"),
				"Station window omitted failure category, pause, diagnostic or recovery");
			core::setStationSystemsSourceLoader({}); world->resetSimulation(); ctx->Sleep(1);
			REQ(driver.contains("Live station state: Enabled")
				&& !driver.contains("Software failure at tick"), "Window retained reset failure");
			world.reset(); ctx->Sleep(1);
			REQ(driver.contains("No World is open."), "No active World status missing");
		});
		#undef REQ
		require(driver.passed(), "Station window workflow failed on the Test Engine");
	}

	void transferWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		// The transfer draft extends past the default display height; ImGui
		// clamps the mouse to the display, so give the headless test enough.
		io.DisplaySize = {1200, 1700};
		auto world = std::make_shared<core::World>("Transfer editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems({true, 1.0f,
			{{"src", "kg", 100.0, 100.0, 0.0, 20.0}, {"dst", "kg", 40.0, 0.0, 0.0},
			{"aux", "kg", 50.0, 0.0, 0.0}},
			{{"pump", "src", "dst", "kg", 60.0, 0}}}, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
			#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

			driver.run("station", "transferWorkflow", [&](ImGuiTestContext* ctx)
			{
				ctx->SetRef("Station Systems");
			REQ(driver.contains("src (kg) [normal]: usable 100.000, held 0.000 of capacity 100.000 | initial usable 100.000, initial held 0.000 | protected 20.000"), "Protected reserve not shown in the live store table");
			REQ(driver.contains("pump: src -> dst (kg) idle | requested 0.000, completed 0.000, unmet 0.000 (rate 60.000/s, priority 0)"), "Authored transfer not shown in the live transfer table");
			// Run until the destination fills: the window shows the rejection and
			// the conserved boundary balance.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(41), "Transfer editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("pump: src -> dst (kg) rejected | requested 1.000, completed 0.000, unmet 1.000 (rate 60.000/s, priority 0)"), "Blocked transfer rejection not observable in the window");
			REQ(driver.contains("Balance kg: starting 100.000, current 100.000, lost 0.000, delivered 0.000, residual 0.000000000"), "Boundary balance not observable in the window");
			REQ(!world->isModified(), "Transfer ticking dirtied the authored document");
			// Draft a second transfer through the real controls; an unknown
			// source is refused with a visible diagnostic and no mutation.
			driver.click(ctx, "Add transfer");
			driver.typeValue(ctx, "Transfer 1 source", "nope");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("references unknown source store 'nope'"),
				"Invalid transfer source diagnostic not shown");
			REQ(world->getStationSystemsSettings().transfers.size() == 1 && history.undoCount() == 0
				&& world->getSimulationTick() == 41,
				"Invalid transfer draft mutated the authored settings");
			// Repair the drafted source (the invalid 'nope') to the aux store and
			// confirm again: the valid transfer applies with the required reset.
			driver.typeValue(ctx, "Transfer 1 source", "aux");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().transfers.size() == 2 && history.undoCount() == 1
				&& world->getSimulationTick() == 0,
				"Repaired transfer draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 transfer(s)")
				&& driver.contains("aux -> dst (kg) idle"),
				"Applied transfer not reflected in the window");
			// Remove the drafted transfer again (tick 0: no reset confirmation needed).
			driver.click(ctx, "Remove transfer 1");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().transfers.size() == 1 && history.undoCount() == 2,
				"Transfer removal did not apply");
				});
		#undef REQ
		require(driver.passed(), "transferWorkflow failed on the Test Engine");
	}

	void compoundWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Compound editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		auto const food = core::ResourceCompositionSettings{0.8, 0.05, 0.0, 0.01, 0.02, 0.02, 17000.0};
		auto const biomass = core::ResourceCompositionSettings{0.0, 0.04, 0.0, 0.005, 0.01, 0.005, 18000.0};
		auto const waste = core::ResourceCompositionSettings{0.8, 0.08, 0.0, 0.02, 0.04, 0.04, 8000.0};
		std::string diagnostic;
		require(world->configureStationSystems({true, 1.0f, {
			{"food", "kg", 100.0, 50.0, 0.0, 0.0, food},
			{"biomass", "kg", 10.0, 1.0, 0.0, 0.0, biomass},
			{"waste", "kg", 20.0, 0.0, 0.0, 0.0, waste}}},
			false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "compoundWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			// The live view shows the declared composition on the dry-mass
			// basis and the constituent balance ledger.
			REQ(driver.contains("food (kg) [normal]: usable 50.000")
				&& driver.contains("compound (dry-mass basis): moisture 0.8000 | nutrient N 0.0500, atmospheric N 0.0000, P 0.0100, K 0.0200, other minerals 0.0200 | 17000.0 kJ/kg dry"),
				"Compound store display missing composition terms");
			REQ(driver.contains("Constituent balances")
				&& driver.contains("Constituent nutrientNitrogen (kg): starting 0.540000"),
				"Constituent balance ledger missing from the window");
			// Author a compound store through the real controls.
			ctx->ItemClick("Add store");
			ctx->ItemClick("Store 3 declared composition");
			ctx->ItemInputValue("Store 3 moisture fraction", "0.5");
			ctx->ItemInputValue("Store 3 nutrient nitrogen fraction", "0.1");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores.size() == 4
				&& world->getStationSystemsSettings().stores[3].composition.has_value()
				&& world->getStationSystemsSettings().stores[3].composition->moisture == 0.5
				&& world->getStationSystemsSettings().stores[3].composition->nutrientNitrogen == 0.1
				&& history.undoCount() == 1, "Authored composition did not apply");
			// An impossible dry-mass total is refused with a visible diagnostic
			// and no mutation.
			ctx->ItemInputValue("Store 3 phosphorus fraction", "0.95");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("dry-mass fractions sum to"),
				"Impossible dry-mass total diagnostic not shown");
			REQ(world->getStationSystemsSettings().stores[3].composition->phosphorus == 0.0
				&& history.undoCount() == 1, "Impossible composition mutated state");
			ctx->ItemInputValue("Store 3 phosphorus fraction", "0.05");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().stores[3].composition->phosphorus == 0.05
				&& history.undoCount() == 2, "Recovery composition edit did not apply");
			// Author the harvest conversion through the real controls.
			ctx->ItemClick("Add conversion");
			ctx->ItemInputValue("Conversion 0 id", "harvest");
			ctx->ItemInputValue("Conversion 0 input 0 store", "food");
			ctx->ItemInputValue("Conversion 0 input 0 rate", "60");
			ctx->ItemInputValue("Conversion 0 output 0 store", "biomass");
			ctx->ItemInputValue("Conversion 0 output 0 yield", "4.8");
			ctx->ItemClick("Add output to conversion 0");
			ctx->ItemInputValue("Conversion 0 output 1 store", "waste");
			ctx->ItemInputValue("Conversion 0 output 1 yield", "24");
			ctx->ItemClick("Apply settings");
			REQ(world->getStationSystemsSettings().conversions.size() == 1
				&& world->getStationSystemsSettings().conversions[0].id == "harvest"
				&& world->getStationSystemsSettings().conversions[0].outputs.size() == 2
				&& history.undoCount() == 3, "Authored conversion did not apply");
			// A yield that would create nutrient nitrogen is refused before the
			// run, with the offending constituent named in the window.
			ctx->ItemInputValue("Conversion 0 output 1 yield", "50");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("would create constituent")
				&& driver.contains("exceeds declared input"),
				"Yield-creating conversion diagnostic not shown in the window");
			REQ(world->getStationSystemsSettings().conversions[0].outputs[1].yieldPerSecond == 24.0
				&& history.undoCount() == 3, "Impossible yield edit mutated state");
			ctx->ItemInputValue("Conversion 0 output 1 yield", "24");
			ctx->Sleep(1);
			REQ(driver.contains("Draft matches the authored start conditions")
				&& world->getStationSystemsSettings().conversions[0].outputs[1].yieldPerSecond == 24.0
				&& history.undoCount() == 3, "Recovery edit did not restore the authored match");
			// Run: the live view shows the full-throughput conversion and the
			// closing constituent ledger, and ticking never dirties the document.
			world->markSaved();
			world->resumeSimulation();
			REQ(world->advanceTicks(30), "Compound editor run failed");
			ctx->Sleep(1);
			REQ(driver.contains("harvest: full (scale 1.000, priority 0)")
				&& driver.contains("consumes food (kg): requested 1.000, completed 1.000")
				&& driver.contains("produces waste (kg): requested 0.400, completed 0.400"),
				"Live conversion status not displayed");
			REQ(!world->isModified(), "Compound ticking dirtied the authored document");
			// Refusal is visible too: fill the biomass store and watch the
			// conversion reject through the window.
			world->pauseSimulation();
			auto settings = world->getStationSystemsSettings();
			settings.stores[1].capacity = 2.0;
			settings.stores[1].startingQualified = 2.0;
			REQ(world->configureStationSystems(settings, true), "Capacity edit failed");
			world->resumeSimulation();
			REQ(world->advanceTicks(5), "Blocked run failed");
			ctx->Sleep(1);
			REQ(driver.contains("harvest: rejected (scale 0.000, priority 0)")
				&& driver.contains("consumes food (kg): requested 1.000, completed 0.000"),
				"Conversion refusal not displayed");
			bool sawRefusalReason = false;
			for (auto const& event : world->getStationSystemsEvents())
				if (event.message.find("Conversion 'harvest' rejected: output store 'biomass' headroom limits throughput")
					!= std::string::npos) sawRefusalReason = true;
			REQ(sawRefusalReason, "Conversion refusal reason missing from the event log");
			for (auto const& entry : world->getStationSystemsSnapshot().constituentBalances)
				REQ(std::abs(entry.residual)
					<= world->getStationSystemsSnapshot().balanceTolerance
						* std::max(1.0, std::abs(entry.starting)),
					"Editor compound run exceeded constituent tolerance");
		});
		#undef REQ
		require(driver.passed(), "Compound workflow failed on the Test Engine");
	}

	void powerWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		// The power draft extends past the default display height; give the
		// headless test enough room and scroll as the compound tests do.
		io.DisplaySize = {1200, 1700};
		auto world = std::make_shared<core::World>("Power editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 1.0f;
		initial.stores.push_back({"water", "kg", 100.0, 50.0, 0.0});
		core::ProfileSettings fullSun;
		fullSun.constantLevel = 1.0;
		initial.solarArrays.push_back({"field", 10.0, fullSun});
		core::EquipmentLoadSettings shop;
		shop.id = "shop";
		shop.electricalDemandKW = 6.0;
		shop.thermalClass = "moderate";
		initial.equipmentLoads.push_back(shop);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
			#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

			driver.run("station", "powerWorkflow", [&](ImGuiTestContext* ctx)
			{
				ctx->SetRef("Station Systems");
			REQ(driver.contains("1 solar array(s), 1 external load(s)"),
				"Authored solar/load counts missing from the window");
			REQ(driver.contains("field: capacity 10.000 kW"),
				"Solar source display missing installed capacity");
			// Run under full sun: the live view shows the supplied service,
			// the surplus and the power totals, and ticking never dirties the
			// authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(60), "Power editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("field: capacity 10.000 kW, illumination 1.000, available 10.000 kW"), "Live solar output not displayed");
			REQ(driver.contains("shop: full | requested 6.000 kW, supplied 6.000 kW, unmet 0.000 kW"), "Live load service not displayed");
			REQ(driver.contains("Power totals: generation 10.000 kW, requested 6.000, supplied 6.000, unmet 0.000, unmet critical 0.000, surplus 4.000"), "Power totals not displayed");
			REQ(!world->isModified(), "Power ticking dirtied the authored document");
			// Draft a second load through the real controls; an unknown thermal
			// class is refused with a visible diagnostic and no mutation.
			driver.click(ctx, "Add load");
			driver.typeValue(ctx, "Load 1 thermal class", "cryogenic");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("unknown thermal service class 'cryogenic'"),
				"Invalid thermal class diagnostic not shown");
			REQ(world->getStationSystemsSettings().equipmentLoads.size() == 1
				&& history.undoCount() == 0 && world->getSimulationTick() == 60,
				"Invalid load draft mutated the authored settings");
			// Repair the class and author the protected floor and curtailment;
			// the confirmed reset applies the new contract.
			driver.typeValue(ctx, "Load 1 thermal class", "warm");
			driver.typeValue(ctx, "Load 1 critical floor kW", "2");
			driver.typeValue(ctx, "Load 1 max curtailment", "0.5");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().equipmentLoads.size() == 2
				&& world->getStationSystemsSettings().equipmentLoads[1].criticalFloorKW == 2.0
				&& world->getStationSystemsSettings().equipmentLoads[1].maxCurtailment == 0.5
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired load draft did not apply through the reset-confirmed flow");
			// A load that requires a cooling service is shown as not admitted,
			// with its heat need visible as unmet cooling — and its protected
			// critical floor reported whole as unmet critical demand, per load
			// and in the station totals, despite never being allocated (#550).
			driver.click(ctx, "Add load");
			driver.click(ctx, "Load 2 requires cooling service");
			driver.typeValue(ctx, "Load 2 electrical demand kW", "3");
			driver.typeValue(ctx, "Load 2 thermal class", "low");
			driver.typeValue(ctx, "Load 2 heat output kW", "1.5");
			driver.typeValue(ctx, "Load 2 critical floor kW", "1");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().equipmentLoads.size() == 3
				&& world->getStationSystemsSettings().equipmentLoads[2].coolingDisposition
				== core::StationCoolingDisposition::CoolingService
				&& history.undoCount() == 2, "Cooling-service load did not apply");
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(60), "Cooling-blocked run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("not admitted: no cooling service of class 'low' is available (unmet cooling 1.500 kW)"), "Unmet cooling not displayed in the window");
			REQ(driver.contains("floor 1.000, unmet critical 1.000"),
				"Cooling-blocked critical floor not displayed per load in the window");
			REQ(driver.contains("unmet critical 1.000, surplus"),
				"Cooling-blocked unmet critical demand not displayed in the station power totals");
			// Turn the solar array periodic with an eclipse leg and watch the
			// live illumination fall to zero through the eclipse.
			driver.click(ctx, "Solar 0 periodic illumination");
			driver.click(ctx, "Add segment to solar 0");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().solarArrays[0].illumination.periodSeconds == 120.0
				&& world->getStationSystemsSettings().solarArrays[0].illumination.segments.size() == 2
				&& world->getStationSystemsSettings().solarArrays[0].illumination.segments[1].startSeconds == 60.0
				&& world->getStationSystemsSettings().solarArrays[0].illumination.segments[1].level == 0.0
				&& history.undoCount() == 3, "Periodic illumination did not apply");
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(3660), "Eclipse editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("field: capacity 10.000 kW, illumination 0.000, available 0.000 kW"), "Eclipse illumination not displayed live");
			REQ(driver.contains("shop: rejected | requested 6.000 kW, supplied 0.000 kW, unmet 6.000 kW"), "Eclipse shortfall not displayed live");
			REQ(!world->isModified(), "Eclipse run dirtied the authored document");
			bool sawEclipse = false;
			for (auto const& event : world->getStationSystemsEvents())
				if (event.message.find("Solar array 'field' entered eclipse") != std::string::npos)
				sawEclipse = true;
				REQ(sawEclipse, "Eclipse transition missing from the window's event log");
				});
		#undef REQ
		require(driver.passed(), "powerWorkflow failed on the Test Engine");
	}

	void batteryWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		// The battery draft extends past the default display height; give
		// the headless test enough room and scroll as the power tests do.
		io.DisplaySize = {1200, 1700};
		auto world = std::make_shared<core::World>("Battery editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 1.0f;
		core::ProfileSettings fullSun;
		fullSun.constantLevel = 1.0;
		initial.solarArrays.push_back({"field", 10.0, fullSun});
		core::EquipmentLoadSettings shop;
		shop.id = "shop";
		shop.electricalDemandKW = 6.0;
		shop.thermalClass = "moderate";
		initial.equipmentLoads.push_back(shop);
		initial.batteries.push_back({"bank", 10.0, 8.0, 8.0, 2.0, 4.0, 1.0, 1.0});
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
			#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

			driver.run("station", "batteryWorkflow", [&](ImGuiTestContext* ctx)
			{
				ctx->SetRef("Station Systems");
			REQ(driver.contains("1 battery(ies)"),
				"Authored battery count missing from the window");
			REQ(driver.contains("bank: idle | stored 8.000 of nameplate 10.000 kWh (usable 8.000, reserve 2.000 never dischargeable)"), "Authored battery reserve policy not displayed");
			// Charge from surplus under full sun: the live view shows the
			// charging power and the document stays clean while ticking.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(60), "Battery editor charge run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("battery discharge 0.000 kW, battery charge 2.000 kW, battery shortfall 0.000 kW"), "Live battery charging totals not displayed");
			REQ(!world->isModified(), "Battery charging dirtied the authored document");
			// Turn the array periodic with an eclipse leg and watch the live
			// discharge carry the deficit.
			driver.click(ctx, "Solar 0 periodic illumination");
			driver.click(ctx, "Add segment to solar 0");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().solarArrays[0].illumination.segments.size() == 2
				&& history.undoCount() == 1, "Periodic illumination did not apply");
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(3660), "Battery editor eclipse run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("bank: discharge-limited |")
				&& driver.contains("battery discharge 4.000 kW")
				&& driver.contains("battery shortfall 2.000 kW"),
				"Live battery discharge during eclipse not displayed");
			REQ(!world->isModified(), "Battery eclipse run dirtied the authored document");
			bool sawDeficitSupport = false;
			for (auto const& event : world->getStationSystemsEvents())
				if (event.message.find("Battery 'bank' discharge limited: supplied 4.000000 of 6.000000 kW")
				!= std::string::npos)
				sawDeficitSupport = true;
			REQ(sawDeficitSupport, "Battery discharge transition missing from the window's event log");
			// Draft a second battery through the real controls; usable energy
			// above the nameplate is refused with a visible diagnostic and no
			// mutation of the authored settings or history.
			driver.click(ctx, "Add battery");
			driver.typeValue(ctx, "Battery 1 usable kWh", "15");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("usable energy exceeds the nameplate energy"),
				"Invalid battery usable energy diagnostic not shown");
			REQ(world->getStationSystemsSettings().batteries.size() == 1
				&& history.undoCount() == 1 && world->getSimulationTick() == 3660,
				"Invalid battery draft mutated the authored settings");
			// Repair the drafted value; the confirmed reset applies the new
			// contract and the live view reconstructs from authored state.
			driver.typeValue(ctx, "Battery 1 usable kWh", "6");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().batteries.size() == 2
				&& world->getStationSystemsSettings().batteries[1].usableKWh == 6.0
				&& history.undoCount() == 2 && world->getSimulationTick() == 0,
				"Repaired battery draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 battery(ies)")
				&& driver.contains("battery2: idle | stored 8.000 of nameplate 10.000 kWh (usable 6.000, reserve 4.000 never dischargeable)"),
				"Applied battery not reflected in the window");
				});
		#undef REQ
		require(driver.passed(), "batteryWorkflow failed on the Test Engine");
	}

	// #557: removing draft rows must never touch the erased element or its
	// nested flows after the erase. Exercise first/middle/last conversion
	// removal over populated flows and compound stores, with cancellation
	// and applied removal, under the real window controls.
	void removalWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO(); io.DisplaySize = {1200, 1000};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Removal editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		auto const food = core::ResourceCompositionSettings{0.8, 0.05, 0.0, 0.01, 0.02, 0.02, 17000.0};
		auto const spare = core::ResourceCompositionSettings{0.5, 0.1, 0.0, 0.02, 0.04, 0.04, 15000.0};
		auto const biomass = core::ResourceCompositionSettings{0.0, 0.04, 0.0, 0.005, 0.01, 0.005, 18000.0};
		auto const waste = core::ResourceCompositionSettings{0.8, 0.08, 0.0, 0.02, 0.04, 0.04, 8000.0};
		core::StationSystemsSettings settings;
		settings.enabled = true;
		settings.observationIntervalSeconds = 1.0f;
		settings.stores = {
			{"food", "kg", 100.0, 50.0, 0.0, 0.0, food},
			{"spare", "kg", 100.0, 50.0, 0.0, 0.0, spare},
			{"biomass", "kg", 100.0, 1.0, 0.0, 0.0, biomass},
			{"waste", "kg", 100.0, 0.0, 0.0, 0.0, waste}};
		for (size_t variant = 0; variant < 3; ++variant)
		{
			core::ResourceConversionSettings conversion;
			conversion.id = "c" + std::to_string(variant);
			conversion.inputs.push_back({"food", 60.0});
			conversion.outputs.push_back({"biomass", 4.8});
			conversion.outputs.push_back({"waste", 24.0});
			settings.conversions.push_back(std::move(conversion));
		}
		std::string diagnostic;
		require(world->configureStationSystems(settings, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 980});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
			#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

			driver.run("station", "removalWorkflow", [&](ImGuiTestContext* ctx)
			{
				ctx->SetRef("Station Systems");
			REQ(driver.contains("4 resource store(s)")
				&& driver.contains("3 conversion(s)"),
				"Removal fixture stores or conversions not shown in the window");
			// A removal cancelled before Apply never reaches the document.
			driver.click(ctx, "Remove conversion 1");
			REQ(driver.contains("Draft differs from the authored start conditions"),
				"Cancelled conversion removal draft not reported");
			driver.click(ctx, "Cancel edits"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().conversions.size() == 3
				&& history.undoCount() == 0
				&& driver.contains("Draft matches the authored start conditions"),
				"Cancel edits did not restore the authored conversion set");
			// Middle conversion removal applies with the neighbours intact.
			driver.click(ctx, "Remove conversion 1");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().conversions.size() == 2
				&& world->getStationSystemsSettings().conversions[0].id == "c0"
				&& world->getStationSystemsSettings().conversions[1].id == "c2"
				&& history.undoCount() == 1, "Middle conversion removal did not apply");
			// First conversion removal shifts the survivor to index 0.
			driver.click(ctx, "Remove conversion 0");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().conversions.size() == 1
				&& world->getStationSystemsSettings().conversions[0].id == "c2"
				&& history.undoCount() == 2, "First conversion removal did not apply");
			// Nested flow removal: dropping the first output of the survivor
			// keeps the remaining flow and its declared yield.
			driver.click(ctx, "Remove Conversion 0 output 0");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().conversions[0].outputs.size() == 1
				&& world->getStationSystemsSettings().conversions[0].outputs[0].storeId == "waste"
				&& world->getStationSystemsSettings().conversions[0].outputs[0].yieldPerSecond == 24.0
				&& history.undoCount() == 3, "Nested output flow removal did not apply");
			// Compound store removal: the mid-list store with a declared
			// composition disappears without disturbing its neighbours.
			driver.click(ctx, "Remove store 1");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().stores.size() == 3
				&& world->getStationSystemsSettings().stores[0].id == "food"
				&& world->getStationSystemsSettings().stores[1].id == "biomass"
				&& world->getStationSystemsSettings().stores[2].id == "waste"
				&& history.undoCount() == 4, "Compound store removal did not apply");
			// Last conversion removal on a running World needs the reset
			// confirmation; cancelling it mutates nothing.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(30), "Removal editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			driver.click(ctx, "Remove conversion 0");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("Confirm?"),
				"Last conversion removal bypassed the World reset confirmation");
			driver.click(ctx, "Cancel reset");
			REQ(world->getStationSystemsSettings().conversions.size() == 1
				&& world->getSimulationTick() == 30 && history.undoCount() == 4,
				"Reset cancellation for the last conversion mutated state");
			driver.click(ctx, "Apply settings"); driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().conversions.empty()
				&& world->getSimulationTick() == 0 && history.undoCount() == 5,
				"Confirmed last conversion removal did not apply");
			ctx->Sleep(1);
			REQ(driver.contains("0 conversion(s)")
				&& driver.contains("Draft matches the authored start conditions"),
				"Applied removal not reflected in the window");
				});
		#undef REQ
		require(driver.passed(), "removalWorkflow failed on the Test Engine");
	}

	// #392: resupply delivery schedules are drafted, validated and applied
	// through the real Station Systems window controls, and the live view
	// reports the next arrival together with accepted and refused amounts.
	void deliveryWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Delivery editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ResourceStoreSettings tank;
		tank.id = "tank";
		tank.unit = "kg";
		tank.capacity = 30.0;
		tank.startingQualified = 10.0;
		initial.stores.push_back(tank);
		core::ResourceDeliverySettings pulse;
		pulse.id = "pulse";
		pulse.storeId = "tank";
		pulse.unit = "kg";
		pulse.quantity = 12.0;
		pulse.firstOccurrenceSeconds = 1.0;
		pulse.recurrenceSeconds = 2.0;
		initial.deliveries.push_back(pulse);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		window.visible = true;
		// The Test Engine drives the interaction; the driver renders the real
		// Station window each frame and captures its text through the buffer
		// rather than the clipboard, which the engine owns during a run.
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "deliveryWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 resupply delivery(ies)"),
				"Authored delivery count missing from the window");
			REQ(driver.contains("pulse -> tank (kg)")
				&& driver.contains("next arrival at 1.000 s"),
				"Authored next-arrival schedule not displayed");
			// Run into the scenario: the 30 kg store starts with 10 kg and each
			// arrival brings 12, so it fills at the second occurrence and the
			// third is refused whole. The live view shows both, and ticking
			// never dirties the authored document.
			world->markSaved();
			world->resumeSimulation();
			if (!world->advanceTicks(300))
			{ REQ(false, "Delivery editor run failed"); }
			world->pauseSimulation();
			ctx->Sleep(1);
			REQ(driver.contains("pulse -> tank (kg): refused | last at 5.000 s: 0.000 of 12.000 accepted, 12.000 rejected | total accepted 20.000, rejected 16.000 | 3 occurrence(s)"),
				"Live delivery accepted/refused accounting not displayed");
			REQ(driver.contains("tank (kg) [normal]: usable 30.000, held 0.000 of capacity 30.000"),
				"Live destination saturation not displayed");
			REQ(driver.contains("next arrival at 7.000 s"),
				"Next arrival not advanced past the fired occurrences");
			REQ(!world->isModified(), "Delivery ticking dirtied the authored document");
			// An invalid drafted delivery is refused with a visible diagnostic
			// and never reaches the authored settings or the history.
			ctx->ItemInputValue("Delivery 0 quantity", "0");
			ctx->ItemClick("Apply settings");
			ctx->ItemClick("Confirm World reset");
			ctx->Sleep(1);
			REQ(driver.contains("quantity must be finite and positive"),
				"Invalid delivery quantity diagnostic not shown");
			REQ(world->getStationSystemsSettings().deliveries[0].quantity == 12.0
				&& history.undoCount() == 0 && world->getSimulationTick() == 300,
				"Invalid delivery draft mutated the authored settings");
			// Repair the draft; the confirmed reset applies it and the live
			// view restarts the schedule from the authored beginning.
			ctx->ItemInputValue("Delivery 0 quantity", "5");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("Confirm?"),
				"Delivery edit bypassed the World reset confirmation");
			ctx->ItemClick("Confirm World reset");
			REQ(world->getStationSystemsSettings().deliveries[0].quantity == 5.0
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired delivery draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("0 occurrence(s)") && driver.contains("next arrival at 1.000 s"),
				"Applied delivery did not restart from authored state");
			// A drafted sub-tick recurrence is refused: the schedule can never
			// ask one tick to absorb an unbounded catch-up burst.
			ctx->ItemInputValue("Delivery 0 recurrence seconds", "0.005");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("at least one World fixed tick"),
				"Sub-tick delivery recurrence was accepted");
			ctx->ItemClick("Cancel edits");
			REQ(world->getStationSystemsSettings().deliveries[0].recurrenceSeconds == 2.0,
				"Cancelled recurrence edit did not restore the authored schedule");
		});
		#undef REQ
		require(driver.passed(), "Delivery workflow failed on the Test Engine");
	}

	// #403: terminal-waste ejection schedules are drafted, validated and
	// applied through the real Station Systems window controls, and the
	// live view reports the next ejection together with the ejected and
	// not-ejectable amounts, while held waste stays visibly behind.
	void wasteEjectionWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Waste ejection editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ResourceStoreSettings dump;
		dump.id = "dump";
		dump.unit = "kg";
		dump.capacity = 60.0;
		dump.startingQualified = 8.0;
		dump.startingUnqualified = 5.0;
		initial.stores.push_back(dump);
		core::WasteEjectionSettings haul;
		haul.id = "haul";
		haul.storeId = "dump";
		haul.unit = "kg";
		haul.quantity = 5.0;
		haul.firstOccurrenceSeconds = 1.0;
		haul.recurrenceSeconds = 2.0;
		initial.wasteEjections.push_back(haul);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		window.visible = true;
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "wasteEjectionWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 terminal-waste ejection(s)"),
				"Authored waste ejection count missing from the window");
			REQ(driver.contains("haul <- dump (kg)")
				&& driver.contains("next ejection at 1.000 s"),
				"Authored next-ejection schedule not displayed");
			// Run into the scenario: the store starts with 8 kg disposal-ready
			// and 5 kg held. Occurrences at 1, 3 and 5 seconds eject 5, then
			// the remaining 3, then find nothing eligible - the held waste is
			// still visibly behind, and ticking never dirties the document.
			world->markSaved();
			world->resumeSimulation();
			if (!world->advanceTicks(300))
			{ REQ(false, "Waste ejection editor run failed"); }
			world->pauseSimulation();
			ctx->Sleep(1);
			REQ(driver.contains("haul <- dump (kg): nothing-ejectable | last at 5.000 s: 0.000 of 5.000 ejected, 5.000 not ejectable | total ejected 8.000, unmet 7.000 | 3 occurrence(s)"),
				"Live ejection ejected/unmet accounting not displayed");
			REQ(driver.contains("dump (kg) [normal]: usable 0.000, held 5.000 of capacity 60.000"),
				"Held waste not displayed as retained in the store");
			REQ(driver.contains("next ejection at 7.000 s"),
				"Next ejection not advanced past the fired occurrences");
			REQ(!world->isModified(), "Ejection ticking dirtied the authored document");
			// An invalid drafted ejection is refused with a visible diagnostic
			// and never reaches the authored settings or the history.
			ctx->ItemInputValue("Waste ejection 0 quantity", "0");
			ctx->ItemClick("Apply settings");
			ctx->ItemClick("Confirm World reset");
			ctx->Sleep(1);
			REQ(driver.contains("quantity must be finite and positive"),
				"Invalid ejection quantity diagnostic not shown");
			REQ(world->getStationSystemsSettings().wasteEjections[0].quantity == 5.0
				&& history.undoCount() == 0 && world->getSimulationTick() == 300,
				"Invalid ejection draft mutated the authored settings");
			// Repair the draft; the confirmed reset applies it and the live
			// view restarts the schedule from the authored beginning.
			ctx->ItemInputValue("Waste ejection 0 quantity", "4");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("Confirm?"),
				"Ejection edit bypassed the World reset confirmation");
			ctx->ItemClick("Confirm World reset");
			REQ(world->getStationSystemsSettings().wasteEjections[0].quantity == 4.0
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired ejection draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("0 occurrence(s)") && driver.contains("next ejection at 1.000 s"),
				"Applied ejection did not restart from authored state");
			// A drafted sub-tick recurrence is refused: the schedule can
			// never ask one tick to absorb an unbounded catch-up burst.
			ctx->ItemInputValue("Waste ejection 0 recurrence seconds", "0.005");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("at least one World fixed tick"),
				"Sub-tick ejection recurrence was accepted");
			ctx->ItemClick("Cancel edits");
			REQ(world->getStationSystemsSettings().wasteEjections[0].recurrenceSeconds == 2.0,
				"Cancelled recurrence edit did not restore the authored schedule");
		});
		#undef REQ
		require(driver.passed(), "Waste ejection workflow failed on the Test Engine");
	}

	// #412: the store hazard class and the hazardous treatment's
	// release-clearance contract are drafted, validated and applied
	// through the real Station Systems window controls, and the live
	// view reports the three quality gates and the clearance ledger.
	void containmentWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Containment editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		auto hazStore = [](std::string const& id, double capacity, double qualified)
		{
			core::ResourceStoreSettings settings;
			settings.id = id;
			settings.unit = "kg";
			settings.hazardClass = "hazardous";
			settings.capacity = capacity;
			settings.startingQualified = qualified;
			return settings;
		};
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		initial.stores = {
			hazStore("hazRaw", 200.0, 100.0),
			hazStore("hazTreated", 100.0, 0.0),
			hazStore("hazConcentrate", 100.0, 0.0),
			hazStore("hazConsumable", 100.0, 10.0),
			hazStore("hazClearance", 100.0, 5.0) };
		core::WaterTreatmentSettings plant;
		plant.id = "plant";
		plant.hazardClass = "hazardous";
		plant.sourceStoreId = "hazRaw";
		plant.treatedStoreId = "hazTreated";
		plant.concentrateStoreId = "hazConcentrate";
		plant.consumableStoreId = "hazConsumable";
		plant.clearanceStoreId = "hazClearance";
		plant.clearancePerKg = 0.002;
		plant.throughputKgPerS = 0.005;
		plant.recoveryRatio = 0.8;
		plant.specificEnergyKWhPerKg = 0.5;
		plant.consumablePerKg = 0.01;
		initial.waterTreatments = { plant };
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 20.0;
		initial.solarArrays = { field };
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		window.visible = true;
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "containmentWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			// The store hazard class is shown in the live store table.
			REQ(driver.contains("hazRaw (kg) [hazardous]"),
				"Hazardous store class not shown in the live store table");
			// Run the cleared scenario: the live treatment view reports the
			// three gates cleared and the clearance ledger.
			world->markSaved();
			world->resumeSimulation();
			if (!world->advanceTicks(60))
			{ REQ(false, "Containment editor run failed"); }
			world->pauseSimulation();
			ctx->Sleep(1);
			REQ(driver.contains("release clearance CLEARED"),
				"Cleared release state not shown in the live treatment view");
			REQ(driver.contains("treatment evidence complete"),
				"Treatment evidence state not shown");
			REQ(!world->isModified(), "Containment ticking dirtied the authored document");
			// A hazardous treatment drafted with a clearance store that
			// does not exist is refused with a visible diagnostic and no
			// mutation.
			driver.typeValue(ctx, "Treatment 0 release-clearance store", "nowhere");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			ctx->ItemClick("Confirm World reset");
			ctx->Sleep(1);
			REQ(driver.contains("release-clearance consumable store 'nowhere' does not exist"),
				"Unknown clearance store diagnostic not shown");
			REQ(world->getStationSystemsSettings().waterTreatments[0].clearanceStoreId == "hazClearance"
				&& history.undoCount() == 0 && world->getSimulationTick() == 60,
				"Invalid clearance draft mutated the authored settings");
			// Repair the draft; the confirmed reset applies it and the live
			// view restarts the scenario from the authored beginning.
			driver.typeValue(ctx, "Treatment 0 release-clearance store", "hazClearance");
			ctx->ItemInputValue("Treatment 0 clearance per kg", "0.003");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("Confirm?"),
				"Clearance edit bypassed the World reset confirmation");
			ctx->ItemClick("Confirm World reset");
			REQ(world->getStationSystemsSettings().waterTreatments[0].clearancePerKg == 0.003
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired clearance draft did not apply through the reset-confirmed flow");
			// A hazardous treatment drafted with a zero clearance dose is
			// refused.
			ctx->ItemInputValue("Treatment 0 clearance per kg", "0");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			REQ(driver.contains("clearance per kg must be finite and positive"),
				"Zero clearance dose was accepted");
			ctx->ItemClick("Cancel edits");
			REQ(world->getStationSystemsSettings().waterTreatments[0].clearancePerKg == 0.003,
				"Cancelled clearance edit did not restore the authored contract");
		});
		#undef REQ
		require(driver.passed(), "Containment workflow failed on the Test Engine");
	}

	// #395: thermal reservoirs and radiators are drafted, validated and
	// applied through the real Station Systems window controls, and the
	// live view reports temperatures, heat flows, limits and the closing
	// energy ledger.
	void thermalWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Thermal editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ThermalReservoirSettings cabin;
		cabin.id = "cabin";
		cabin.thermalClass = "moderate";
		cabin.heatCapacityKJPerK = 100.0;
		cabin.startingTemperatureC = 20.0;
		cabin.minTemperatureC = 10.0;
		cabin.maxTemperatureC = 60.0;
		cabin.absorbedHeatKW = 0.5;
		initial.thermalReservoirs.push_back(cabin);
		core::RadiatorSettings panel;
		panel.id = "panel";
		panel.reservoirId = "cabin";
		panel.areaM2 = 5000.0;
		panel.emissivity = 0.9;
		panel.viewFactor = 1.0;
		panel.sinkTemperatureC = -270.0;
		initial.radiators.push_back(panel);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 10.0;
		initial.solarArrays.push_back(field);
		core::EquipmentLoadSettings shop;
		shop.id = "shop";
		shop.electricalDemandKW = 5.0;
		shop.thermalClass = "moderate";
		shop.heatOutputKW = 2.0;
		shop.heatSinkId = "cabin";
		initial.equipmentLoads.push_back(shop);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "thermalWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 thermal reservoir(s), 1 radiator(s)"),
				"Authored thermal counts missing from the window");
			REQ(driver.contains("cabin (moderate): 20.000 C | limits 10.000..60.000 C | capacity 100.000 kJ/K"),
				"Authored reservoir display missing temperature, limits or capacity");
			REQ(driver.contains("panel -> cabin: area 5000.000 m2, emissivity 0.900, view 1.000, sink -270.000 C"),
				"Authored radiator display missing");
			// Run: the live view shows the warming reservoir, the routed
			// heat on the load, the radiator rejection and the closing
			// ledger, and ticking never dirties the authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(600), "Thermal editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("heat routed to reservoir: 2.000 kW (not also exported)"),
				"Routed load heat not displayed");
			REQ(!driver.contains("ABOVE the declared maximum")
				&& !driver.contains("BELOW the declared minimum"),
				"Reservoir limit flags wrong for a nominal run");
			REQ(driver.contains("energy ledger (kJ)") && driver.contains("residual"),
				"Thermal energy ledger not displayed");
			REQ(driver.contains("Thermal totals: absorbed"),
				"Thermal totals not displayed");
			REQ(!world->isModified(), "Thermal ticking dirtied the authored document");
			// An invalid drafted reservoir is refused with a visible
			// diagnostic and never reaches the authored settings.
			driver.click(ctx, "Add thermal reservoir");
			driver.typeValue(ctx, "Reservoir 1 min temperature C", "50");
			driver.typeValue(ctx, "Reservoir 1 max temperature C", "40");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("min below max"),
				"Inverted drafted temperature limits diagnostic not shown");
			REQ(world->getStationSystemsSettings().thermalReservoirs.size() == 1
				&& history.undoCount() == 0 && world->getSimulationTick() == 600,
				"Invalid reservoir draft mutated the authored settings");
			// Repair the drafted limits and give the new reservoir a warm
			// service class; the confirmed reset applies the contract and
			// the live view restarts from authored state.
			driver.typeValue(ctx, "Reservoir 1 min temperature C", "0");
			driver.typeValue(ctx, "Reservoir 1 thermal class", "warm");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().thermalReservoirs.size() == 2
				&& world->getStationSystemsSettings().thermalReservoirs[1].minTemperatureC == 0.0
				&& world->getStationSystemsSettings().thermalReservoirs[1].thermalClass == "warm"
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired reservoir draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 thermal reservoir(s)")
				&& driver.contains("reservoir2 (warm): 20.000 C"),
				"Applied reservoir not reflected in the window");
			// A radiator with an emissivity above one is refused.
			// The World is back at tick 0 after the confirmed reset, so
			// these applies need no reset confirmation.
			driver.click(ctx, "Add radiator");
			driver.typeValue(ctx, "Radiator 1 emissivity", "2");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("emissivity must be finite and greater than 0 up to 1"),
				"Invalid radiator emissivity diagnostic not shown");
			REQ(world->getStationSystemsSettings().radiators.size() == 1
				&& history.undoCount() == 1,
				"Invalid radiator draft mutated the authored settings");
			driver.typeValue(ctx, "Radiator 1 emissivity", "0.8");
			driver.click(ctx, "Apply settings");
			REQ(world->getStationSystemsSettings().radiators.size() == 2
				&& history.undoCount() == 2 && world->getSimulationTick() == 0,
				"Repaired radiator draft did not apply");
			// A load heat sink of an incompatible class is refused with
			// both classes named; the authored routing stays untouched.
			driver.typeValue(ctx, "Load 0 heat sink", "reservoir2");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("incompatible with heat sink reservoir 'reservoir2' of class 'warm'"),
				"Cross-class heat sink routing diagnostic not shown");
			REQ(world->getStationSystemsSettings().equipmentLoads[0].heatSinkId == "cabin"
				&& history.undoCount() == 2 && world->getSimulationTick() == 0,
				"Invalid heat sink draft mutated the authored routing");
			driver.click(ctx, "Cancel edits");
			REQ(world->getStationSystemsSettings().equipmentLoads[0].heatSinkId == "cabin",
				"Cancelled heat sink edit did not restore the authored routing");
		});
		#undef REQ
		require(driver.passed(), "Thermal workflow failed on the Test Engine");
	}
	// #396: coolant loops are drafted, validated and applied through the
	// real Station Systems window controls, and the live view reports the
	// finite inventory, circulation, freeze-protection service and the
	// closing loop energy ledger.
	void coolantWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Coolant editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ThermalReservoirSettings bus;
		bus.id = "bus";
		bus.thermalClass = "moderate";
		bus.heatCapacityKJPerK = 1000.0;
		bus.startingTemperatureC = 20.0;
		bus.minTemperatureC = -50.0;
		bus.maxTemperatureC = 100.0;
		initial.thermalReservoirs.push_back(bus);
		core::CoolantLoopSettings itcs;
		itcs.id = "itcs";
		itcs.hazardClass = "normal";
		itcs.reservoirId = "bus";
		itcs.coolantCapacityKg = 100.0;
		itcs.startingCoolantKg = 100.0;
		itcs.specificHeatKJPerKgK = 4.0;
		itcs.startingTemperatureC = 20.0;
		itcs.freezeProtectionSetpointC = 5.0;
		itcs.pumpPowerKW = 1.2;
		itcs.ratedFlowKgPerS = 3.0;
		itcs.exchangerConductanceKWPerK = 6.0;
		itcs.heaterCapacityKW = 2.0;
		initial.coolantLoops.push_back(itcs);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 10.0;
		initial.solarArrays.push_back(field);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "coolantWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 coolant loop(s)"),
				"Authored coolant loop count missing from the window");
			// Run: the live view shows the circulating loop at full rated
			// flow, the closing loop ledger and the station coolant totals,
			// and ticking never dirties the authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(600), "Coolant editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("itcs [normal] -> bus: full service")
				&& driver.contains("flow 3.000 of 3.000 kg/s (circulation 100.0%)")
				&& driver.contains("pump 1.200 of 1.200 kW"),
				"Live coolant circulation service not displayed");
			REQ(driver.contains("energy ledger (kJ)") && driver.contains("pump work"),
				"Coolant loop energy ledger not displayed");
			REQ(driver.contains("circulation 3.000 kg/s"),
				"Station coolant circulation total not displayed");
			REQ(!driver.contains("BELOW the freeze-protection setpoint")
				&& !driver.contains("DISABLED"),
				"Freeze or disabled flags wrong for a nominal run");
			REQ(!world->isModified(), "Coolant ticking dirtied the authored document");
			// An invalid drafted loop is refused with a visible diagnostic
			// and never reaches the authored settings.
			driver.click(ctx, "Add coolant loop");
			driver.typeValue(ctx, "Coolant loop 1 starting coolant kg", "150");
			driver.click(ctx, "Apply settings");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("exceeds the loop capacity"),
				"Over-filled drafted coolant diagnostic not shown");
			REQ(world->getStationSystemsSettings().coolantLoops.size() == 1
				&& history.undoCount() == 0 && world->getSimulationTick() == 600,
				"Invalid coolant draft mutated the authored settings");
			// Repair the drafted charge and give the new loop the hazardous
			// class; the confirmed reset applies the contract and the live
			// view restarts from authored state.
			driver.typeValue(ctx, "Coolant loop 1 starting coolant kg", "50");
			driver.typeValue(ctx, "Coolant loop 1 hazard class", "hazardous");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().coolantLoops.size() == 2
				&& world->getStationSystemsSettings().coolantLoops[1].startingCoolantKg == 50.0
				&& world->getStationSystemsSettings().coolantLoops[1].hazardClass == "hazardous"
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired coolant draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 coolant loop(s)")
				&& driver.contains("coolantLoop2 [hazardous] -> bus"),
				"Applied coolant loop not reflected in the window");
			// An unknown hazard class is refused.
			driver.typeValue(ctx, "Coolant loop 1 hazard class", "toxic");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("unknown coolant hazard class 'toxic'"),
				"Unknown hazard class diagnostic not shown");
			REQ(world->getStationSystemsSettings().coolantLoops[1].hazardClass == "hazardous"
				&& history.undoCount() == 1,
				"Invalid hazard class draft mutated the authored settings");
			// A loop coupled to an unknown reservoir is refused.
			driver.typeValue(ctx, "Coolant loop 1 hazard class", "hazardous");
			driver.typeValue(ctx, "Coolant loop 1 reservoir", "ghost");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("coupled to unknown thermal reservoir 'ghost'"),
				"Unknown loop host diagnostic not shown");
			driver.click(ctx, "Cancel edits");
			REQ(world->getStationSystemsSettings().coolantLoops[1].reservoirId == "bus",
				"Cancelled loop edit did not restore the authored coupling");
		});
		#undef REQ
		require(driver.passed(), "Coolant workflow failed on the Test Engine");
	}
	// #397: chillers are drafted, validated and applied through the real
	// Station Systems window controls, and the live view reports the cold
	// duty, circulation-capped deliverable, COP work draw, condenser
	// heat, per-class rollup and the closing chiller ledger.
	void chillerWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Chiller editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ThermalReservoirSettings bus;
		bus.id = "bus";
		bus.thermalClass = "moderate";
		bus.heatCapacityKJPerK = 1000.0;
		bus.startingTemperatureC = 20.0;
		bus.minTemperatureC = -50.0;
		bus.maxTemperatureC = 100.0;
		initial.thermalReservoirs.push_back(bus);
		core::ThermalReservoirSettings sink;
		sink.id = "sink";
		sink.thermalClass = "moderate";
		sink.heatCapacityKJPerK = 1000.0;
		sink.startingTemperatureC = 20.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 100.0;
		initial.thermalReservoirs.push_back(sink);
		core::CoolantLoopSettings chilled;
		chilled.id = "chilled";
		chilled.hazardClass = "normal";
		chilled.reservoirId = "bus";
		chilled.coolantCapacityKg = 100.0;
		chilled.startingCoolantKg = 100.0;
		chilled.specificHeatKJPerKgK = 4.0;
		chilled.startingTemperatureC = 20.0;
		chilled.freezeProtectionSetpointC = 0.0;
		chilled.pumpPowerKW = 0.5;
		chilled.ratedFlowKgPerS = 2.0;
		chilled.exchangerConductanceKWPerK = 1.0;
		chilled.heaterCapacityKW = 0.0;
		initial.coolantLoops.push_back(chilled);
		core::CoolantLoopSettings chilled2;
		chilled2.id = "chilled2";
		chilled2.hazardClass = "normal";
		chilled2.reservoirId = "bus";
		chilled2.coolantCapacityKg = 100.0;
		chilled2.startingCoolantKg = 100.0;
		chilled2.specificHeatKJPerKgK = 4.0;
		chilled2.startingTemperatureC = 20.0;
		chilled2.freezeProtectionSetpointC = 0.0;
		chilled2.pumpPowerKW = 0.5;
		chilled2.ratedFlowKgPerS = 2.0;
		chilled2.exchangerConductanceKWPerK = 1.0;
		chilled2.heaterCapacityKW = 0.0;
		initial.coolantLoops.push_back(chilled2);
		core::ChillerSettings ics;
		ics.id = "ics";
		ics.thermalClass = "moderate";
		ics.coldLoopId = "chilled";
		ics.condenserReservoirId = "sink";
		ics.ratedCoolingCapacityKW = 6.0;
		ics.cop = 3.0;
		ics.auxiliaryPowerKW = 0.5;
		ics.chilledSetpointC = 5.0;
		ics.maxCondensingTemperatureC = 45.0;
		initial.chillers.push_back(ics);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 10.0;
		initial.solarArrays.push_back(field);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "chillerWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 chiller(s)"),
				"Authored chiller count missing from the window");
			// Run: the live view shows the chiller at full cold duty with
			// the COP work draw, the condenser heat, the closing ledger
			// and the per-class rollup, and ticking never dirties the
			// authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(600), "Chiller editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("ics (moderate) chilled -> condenser sink: full service")
				&& driver.contains("cold duty 6.000 of 6.000 kW (unmet 0.000)")
				&& driver.contains("deliverable 6.000 kW at 100.0% circulation")
				&& driver.contains("power 2.500 of 2.500 kW (COP 3.00, aux 0.500 kW)")
				&& driver.contains("condenser heat 8.500 kW"),
				"Live chiller cold-service, COP draw or condenser heat not displayed");
			REQ(driver.contains("energy ledger (kJ)") && driver.contains("extracted"),
				"Chiller energy ledger not displayed");
			REQ(driver.contains("moderate: capacity 6.000 kW, provided 6.000")
				&& driver.contains("low: capacity 0.000 kW, provided 0.000"),
				"Per-class cold service rollup not displayed");
			REQ(!driver.contains("SINK OVER") && !driver.contains("DISABLED")
				&& !driver.contains("at its chilled setpoint"),
				"Chiller condition flags wrong for a nominal run");
			REQ(!world->isModified(), "Chiller ticking dirtied the authored document");
			// An invalid drafted chiller is refused with a visible
			// diagnostic and never reaches the authored settings.
			driver.click(ctx, "Add chiller");
			driver.typeValue(ctx, "Chiller 1 cold loop", "chilled2");
			driver.typeValue(ctx, "Chiller 1 COP", "0");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("COP must be finite and positive"),
				"Zero-COP drafted chiller diagnostic not shown");
			REQ(world->getStationSystemsSettings().chillers.size() == 1
				&& history.undoCount() == 0 && world->getSimulationTick() == 600,
				"Invalid chiller draft mutated the authored settings");
			// Repair the COP but cross the classes: the drafted chiller
			// may not serve a class its cold-loop reservoir does not.
			// The World is still at tick 600 (the refused apply reset
			// nothing), so every apply below still routes through the
			// reset-confirmation gate.
			driver.typeValue(ctx, "Chiller 1 COP", "3");
			driver.typeValue(ctx, "Chiller 1 thermal class", "warm");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("incompatible with cold loop 'chilled2'"),
				"Cross-class cold-side coupling diagnostic not shown");
			REQ(world->getStationSystemsSettings().chillers.size() == 1
				&& history.undoCount() == 0,
				"Cross-class chiller draft mutated the authored settings");
			// A chiller coupled to an unknown cold loop is refused.
			driver.typeValue(ctx, "Chiller 1 thermal class", "moderate");
			driver.typeValue(ctx, "Chiller 1 cold loop", "ghost");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("unknown coolant loop 'ghost'"),
				"Unknown cold-loop coupling diagnostic not shown");
			// A chilled setpoint at or above the maximum condensing
			// temperature is refused.
			driver.typeValue(ctx, "Chiller 1 cold loop", "chilled2");
			driver.typeValue(ctx, "Chiller 1 chilled setpoint C", "45");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("chilled setpoint must be below the maximum condensing"),
				"Setpoint-above-max-condensing diagnostic not shown");
			// Repair the drafted limits; the confirmed reset applies the
			// contract and the live view restarts from authored state.
			driver.typeValue(ctx, "Chiller 1 chilled setpoint C", "5");
			driver.typeValue(ctx, "Chiller 1 cold loop", "chilled2");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().chillers.size() == 2
				&& world->getStationSystemsSettings().chillers[1].cop == 3.0
				&& world->getStationSystemsSettings().chillers[1].coldLoopId == "chilled2"
				&& world->getStationSystemsSettings().chillers[1].condenserReservoirId == "sink"
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired chiller draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 chiller(s)")
				&& driver.contains("chiller2 (moderate) chilled2 -> condenser sink"),
				"Applied chiller not reflected in the window");
			// Two chillers cannot share one evaporator loop.
			driver.click(ctx, "Add chiller");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("shares cold loop 'chilled'"),
				"Shared-evaporator diagnostic not shown");
			driver.click(ctx, "Cancel edits");
			REQ(world->getStationSystemsSettings().chillers.size() == 2,
				"Cancelled chiller edit did not restore the authored contracts");
		});
		#undef REQ
		require(driver.passed(), "Chiller workflow failed on the Test Engine");
	}

	void generatorWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Generator editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 1000.0;
		diesel.startingQualified = 1000.0;
		core::ThermalReservoirSettings hot;
		hot.id = "hot";
		hot.thermalClass = "generation";
		hot.heatCapacityKJPerK = 10000.0;
		hot.startingTemperatureC = 20.0;
		hot.minTemperatureC = -50.0;
		hot.maxTemperatureC = 200.0;
		core::GeneratorSettings gen;
		gen.id = "gen";
		gen.fuelStoreId = "diesel";
		gen.thermalClass = "generation";
		gen.heatSinkId = "hot";
		gen.ratedOutputKW = 10.0;
		gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 5.0;
		gen.commandedOutputKW = 10.0;
		gen.startupPowerKW = 1.0;
		gen.startupSeconds = 6.0;
		gen.shutdownHeatKW = 1.0;
		gen.shutdownSeconds = 10.0;
		gen.maxSinkTemperatureC = 90.0;
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 20.0;
		initial.stores.push_back(diesel);
		initial.thermalReservoirs.push_back(hot);
		initial.generators.push_back(gen);
		initial.solarArrays.push_back(field);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "generatorWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 generator(s)"),
				"Authored generator count missing from the window");
			// Run: the live view shows the generator at full output with
			// its fuel draw, waste heat, completed startup and the closing
			// fuel ledger, and ticking never dirties the authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(600), "Generator editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("gen (generation) fuel diesel -> sink hot: running / full")
				&& driver.contains("output 10.000 of commanded 10.000 kW")
				&& driver.contains("waste 10.000 kW")
				&& driver.contains("startup 0.000 of 1.000 kW (6.0 of 6.0 kJ)"),
				"Live generator output, waste heat or completed startup not displayed");
			REQ(driver.contains("fuel ledger (kJ)") && driver.contains("residual"),
				"Generator fuel ledger not displayed");
			REQ(!driver.contains("DISABLED") && !driver.contains("cooling REQUIRED"),
				"Generator condition flags wrong for a nominal run");
			REQ(!world->isModified(), "Generator ticking dirtied the authored document");
			// An invalid drafted generator is refused with a visible
			// diagnostic and never reaches the authored settings.
			driver.click(ctx, "Add generator");
			driver.typeValue(ctx, "Generator 1 efficiency", "0");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("efficiency must be finite and greater than 0"),
				"Zero-efficiency drafted generator diagnostic not shown");
			REQ(world->getStationSystemsSettings().generators.size() == 1
				&& history.undoCount() == 0 && world->getSimulationTick() == 600,
				"Invalid generator draft mutated the authored settings");
			// A fuel store that is not an energy inventory is refused.
			driver.typeValue(ctx, "Generator 1 efficiency", "0.5");
			driver.typeValue(ctx, "Generator 1 fuel store", "ghost");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("fuel inventory references unknown Resource store 'ghost'"),
				"Unknown fuel-store coupling diagnostic not shown");
			// Commanding above the installed rating is refused.
			driver.typeValue(ctx, "Generator 1 fuel store", "diesel");
			driver.typeValue(ctx, "Generator 1 commanded output kW", "11");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("commanded output exceeds the installed capacity"),
				"Commanded-above-rating diagnostic not shown");
			// A generator whose waste-heat class does not match its sink
			// reservoir is refused: heat routes only to the same class.
			driver.typeValue(ctx, "Generator 1 commanded output kW", "10");
			driver.typeValue(ctx, "Generator 1 thermal class", "warm");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("incompatible with heat sink reservoir 'hot'"),
				"Cross-class heat-sink coupling diagnostic not shown");
			// Repair the drafted limits; the confirmed reset applies the
			// contract and the live view restarts from authored state.
			driver.typeValue(ctx, "Generator 1 thermal class", "generation");
			driver.click(ctx, "Confirm World reset");
			REQ(world->getStationSystemsSettings().generators.size() == 2
				&& world->getStationSystemsSettings().generators[1].efficiency == 0.5
				&& world->getStationSystemsSettings().generators[1].fuelStoreId == "diesel"
				&& world->getStationSystemsSettings().generators[1].heatSinkId == "hot"
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired generator draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			REQ(driver.contains("2 generator(s)")
				&& driver.contains("generator2 (generation) fuel diesel -> sink hot"),
				"Applied generator not reflected in the window");
			driver.click(ctx, "Add generator");
			driver.click(ctx, "Cancel edits");
			REQ(world->getStationSystemsSettings().generators.size() == 2,
				"Cancelled generator edit did not restore the authored contracts");
		});
		#undef REQ
		require(driver.passed(), "Generator workflow failed on the Test Engine");
	}
}
void bufferWorkflow(smoke::Context const&)
{
	using smoke::require;
	headless::ScopedImGuiContext context;
	auto& io = ImGui::GetIO();
	io.DisplaySize = {1200, 1700};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	DocumentHistory history;
	StationSystemsWindow window;
	auto world = std::make_shared<core::World>("Buffer editor", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	std::string diagnostic;
	core::StationSystemsSettings initial;
	initial.enabled = true;
	initial.observationIntervalSeconds = 2.0f;
	core::ThermalReservoirSettings host;
	host.id = "host";
	host.thermalClass = "moderate";
	host.heatCapacityKJPerK = 100.0;
	host.startingTemperatureC = 50.0;
	host.minTemperatureC = 0.0;
	host.maxTemperatureC = 200.0;
	host.absorbedHeatKW = 10.0;
	core::ThermalBufferSettings buf;
	buf.id = "buf";
	buf.reservoirId = "host";
	buf.accessMode = "passive";
	buf.capacityKJ = 1000.0;
	buf.startingEnergyKJ = 0.0;
	buf.minUsableTemperatureC = 0.0;
	buf.maxUsableTemperatureC = 40.0;
	buf.maxChargeKW = 20.0;
	buf.maxDischargeKW = 20.0;
	buf.transferConductanceKWPerK = 1e6;
	initial.thermalReservoirs.push_back(host);
	initial.thermalBuffers.push_back(buf);
	require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
	headless::TestEngineDriver driver([&]
	{
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
		window.render(world, history);
	});
	window.visible = true;
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

	driver.run("station", "bufferWorkflow", [&](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		REQ(driver.contains("1 thermal buffer(s)"),
			"Authored thermal buffer count missing from the window");
		// Run: the live view shows the buffer absorbing the host's cooling
		// deficit, its stored energy growing, and the closing storage ledger;
		// ticking never dirties the authored document.
		world->markSaved();
		world->resumeSimulation(); REQ(world->advanceTicks(600), "Buffer editor run failed");
		world->pauseSimulation(); ctx->Sleep(1);
		REQ(driver.contains("buf -> host [passive]")
			&& driver.contains("stored") && driver.contains("kJ")
			&& driver.contains("energy ledger (kJ)") && driver.contains("residual"),
			"Live buffer charge state or storage ledger not displayed");
		REQ(!world->isModified(), "Buffer ticking dirtied the authored document");
		// An invalid drafted buffer is refused with a visible diagnostic and
		// never reaches the authored settings: a circulation buffer with no
		// declared loop cannot be applied.
		driver.click(ctx, "Add thermal buffer");
		driver.click(ctx, "Apply settings"); ctx->Sleep(1);
		driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
		REQ(driver.contains("circulation access references unknown coolant loop"),
			"Circulation buffer with no loop diagnostic not shown");
		REQ(world->getStationSystemsSettings().thermalBuffers.size() == 1
			&& history.undoCount() == 0 && world->getSimulationTick() == 600,
			"Invalid buffer draft mutated the authored settings");
		// Repair the drafted buffer to a valid passive store; the confirmed
		// reset applies the contract and the live view restarts from authored state.
		driver.typeValue(ctx, "Buffer 1 access mode (circulation|passive)", "passive");
		driver.click(ctx, "Confirm World reset");
		REQ(world->getStationSystemsSettings().thermalBuffers.size() == 2
			&& world->getStationSystemsSettings().thermalBuffers[1].accessMode == "passive"
			&& world->getStationSystemsSettings().thermalBuffers[1].reservoirId == "host"
			&& history.undoCount() == 1 && world->getSimulationTick() == 0,
			"Repaired buffer draft did not apply through the reset-confirmed flow");
		ctx->Sleep(1);
		REQ(driver.contains("2 thermal buffer(s)")
			&& driver.contains("buffer2 -> host [passive]"),
			"Applied buffer not reflected in the window");
		// A starting energy above capacity is refused. The tick is back at
		// zero after the applied reset, so Apply runs directly without the
		// World-reset confirmation.
		driver.click(ctx, "Add thermal buffer");
		driver.typeValue(ctx, "Buffer 2 access mode (circulation|passive)", "passive");
		driver.typeValue(ctx, "Buffer 2 starting energy kJ", "5000");
		driver.click(ctx, "Apply settings"); ctx->Sleep(1);
		REQ(driver.contains("starting energy must be finite and within capacity"),
			"Buffer starting-above-capacity diagnostic not shown");
		// Cancelling a further edit restores the authored contracts.
		driver.click(ctx, "Cancel edits");
		REQ(world->getStationSystemsSettings().thermalBuffers.size() == 2,
			"Cancelled buffer edit did not restore the authored contracts");
	});
	#undef REQ
	require(driver.passed(), "Buffer workflow failed on the Test Engine");
}
	void microbialConditionWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200.0f, 2400.0f};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Microbial condition editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		auto plain = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings store;
			store.id = id;
			store.unit = "kg";
			store.capacity = capacity;
			store.startingQualified = starting;
			return store;
		};
		auto feed = plain("waste", 100.0, 10.0);
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.9;
		composition.nutrientNitrogen = 0.05;
		composition.atmosphericNitrogen = 0.02;
		composition.phosphorus = 0.02;
		composition.potassium = 0.02;
		composition.otherMinerals = 0.03;
		composition.energyKJPerKgDry = 8000.0;
		feed.composition = composition;
		initial.stores.push_back(feed);
		initial.stores.push_back(plain("oxygen", 100.0, 10.0));
		initial.stores.push_back(plain("processWater", 100.0, 10.0));
		initial.stores.push_back(plain("media", 100.0, 10.0));
		initial.stores.push_back(plain("nutrients", 100.0, 0.0));
		initial.stores.push_back(plain("recoveredWater", 100.0, 0.0));
		initial.stores.push_back(plain("co2", 100.0, 0.0));
		initial.stores.push_back(plain("residue", 100.0, 0.0));
		initial.stores.push_back(plain("stock", 10.0, 1.0));
		core::ThermalReservoirSettings sink;
		sink.id = "sink";
		sink.thermalClass = "low";
		sink.heatCapacityKJPerK = 10000.0;
		sink.startingTemperatureC = 20.0;
		sink.minTemperatureC = -50.0;
		sink.maxTemperatureC = 200.0;
		initial.thermalReservoirs.push_back(sink);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		initial.solarArrays.push_back(field);
		core::MicrobialProcessorSettings digester;
		digester.id = "digester";
		digester.hazardClass = "normal";
		digester.feedStoreId = "waste";
		digester.oxygenStoreId = "oxygen";
		digester.waterStoreId = "processWater";
		digester.consumableStoreId = "media";
		digester.nutrientStoreId = "nutrients";
		digester.recoveredWaterStoreId = "recoveredWater";
		digester.co2StoreId = "co2";
		digester.residueStoreId = "residue";
		digester.thermalClass = "low";
		digester.heatSinkId = "sink";
		digester.rateKgPerS = 0.001;
		digester.reactionHeatKJPerKgFeed = 600.0;
		digester.initialCondition = 0.5;
		digester.recoveryPerHour = 72.0;
		digester.stockStoreId = "stock";
		digester.stockUnitsPerConditionPoint = 10.0;
		initial.microbialProcessors.push_back(digester);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 2350});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "microbialConditionWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 microbial processor(s)"),
				"Authored microbial processor count missing from the window");
			// Run: the live view shows the degraded culture recovering,
			// funded by the replacement stock, with the condition and its
			// change visible. Ticking never dirties the authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(60), "Editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("condition 0.520 | benign, recovering | last change +0.000333 | replacement stock: drawn from 'stock', consumed 0.003333 units this tick (0.2000 total)"),
				"Live culture condition, recovery state or stock funding not displayed");
			REQ(!world->isModified(), "Microbial condition ticking dirtied the authored document");
			// An invalid drafted condition is refused with a visible
			// diagnostic and never reaches the authored settings.
			driver.typeValue(ctx, "Microbial processor 0 initial condition", "2");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("initial culture condition must be finite and from 0 to 1"),
				"Invalid drafted initial condition diagnostic not shown");
			REQ(world->getStationSystemsSettings().microbialProcessors[0].initialCondition == 0.5
				&& world->getSimulationTick() == 60,
				"Invalid drafted initial condition mutated the authored settings");
			// A valid drafted recovery rate applies through the World-reset
			// confirmation and restarts the culture from its authored start.
			driver.typeValue(ctx, "Microbial processor 0 initial condition", "0.25");
			driver.typeValue(ctx, "Microbial processor 0 recovery per hour", "36");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().microbialProcessors[0].initialCondition == 0.25
				&& world->getStationSystemsSettings().microbialProcessors[0].recoveryPerHour == 36.0
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired microbial condition draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			world->resumeSimulation(); REQ(world->advanceTicks(60), "Post-apply run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			// 0.25 + 1 s at 36/h = 0.26: the live view follows the new rate.
			REQ(driver.contains("condition 0.260")
				&& driver.contains("1 microbial processor(s)"),
				"Applied recovery rate not reflected in the live condition display");

			// Render real World snapshots for every description branch and
			// both configured and absent replacement stock. Check the entire
			// line so displaced printf arguments cannot pass on substrings.
			auto checkReadout = [&](core::StationSystemsSettings const& settings,
				char const* expected)
			{
				if (!driver.require(world->configureStationSystems(settings, true, &diagnostic),
					diagnostic.c_str())) return false;
				world->resumeSimulation();
				if (!driver.require(world->advanceTicks(60), "Readout run failed")) return false;
				world->pauseSimulation(); ctx->Sleep(1);
				return driver.require(driver.contains(expected), "Complete microbial condition readout incorrect");
			};
			auto benign = initial;
			benign.microbialProcessors[0].initialCondition = 1.0;
			benign.microbialProcessors[0].stockStoreId.clear();
			REQ(checkReadout(benign,
				"condition 1.000 | benign, at full condition | last change +0.000000 | replacement stock: none configured, consumed 0.000000 units this tick (0.0000 total)"),
				"Benign readout failed");
			auto recovering = initial;
			recovering.microbialProcessors[0].stockStoreId.clear();
			REQ(checkReadout(recovering,
				"condition 0.520 | benign, recovering | last change +0.000333 | replacement stock: none configured, consumed 0.000000 units this tick (0.0000 total)"),
				"Recovery without configured stock readout failed");
			auto stressed = benign;
			stressed.stores[0].startingQualified = 0.0;
			stressed.microbialProcessors[0].starvationDegradePerHour = 72.0;
			REQ(checkReadout(stressed,
				"condition 0.980 | stressed by starvation | last change -0.000333 | replacement stock: none configured, consumed 0.000000 units this tick (0.0000 total)"),
				"Stressed readout failed");
		});
		#undef REQ
		require(driver.passed(), "Microbial condition workflow failed on the Test Engine");
	}
// #421: the coupled rig's live picture is observable in the separate
// Station Systems window. The rig is run with the hydrogen feed drained
// and the electrolyzer off, so the Sabatier stage carries a named
// limiter, and the window must surface the coupled subsystems and the
// limiting process beside their live numbers.
void coupledLiveViewWorkflow(smoke::Context const&)
{
	using smoke::require;
	headless::ScopedImGuiContext context;
	auto& io = ImGui::GetIO();
	io.DisplaySize = {1200.0f, 6400.0f};
	io.Fonts->AddFontDefault(); io.Fonts->Build();
	DocumentHistory history;
	StationSystemsWindow window;
	auto world = std::make_shared<core::World>("Coupled live view", 2, 2);
	world->pauseSimulation(); world->finishBuild();
	auto rig = coupled_rig::nominal();
	for (auto& store : rig.stores)
		if (store.id == "h2") store.startingQualified = 0.0;
	rig.electrolyzers.front().rateKgPerS = 0.0;
	std::string diagnostic;
	require(world->configureStationSystems(rig, false, &diagnostic), diagnostic.c_str());
	world->resumeSimulation();
	require(world->advanceTicks(600), "the coupled live-view station did not tick");
	auto const& snap = world->getStationSystemsSnapshot();
	require(snap.healthy
		&& !snap.sabatierReactors.empty()
		&& snap.sabatierReactors.front().blockReason.find("hydrogen feed store 'h2'")
			!= std::string::npos,
		"the coupled fixture did not reach the hydrogen-starved state");
	headless::TestEngineDriver driver([&]
	{
		ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 6300});
		window.render(world, history);
	});
	window.visible = true;
	driver.frame(); driver.frame();
	#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
	driver.run("station", "coupledLiveView", [&](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		REQ(driver.contains("Active World: Coupled live view"),
			"station window missing the coupled World");
		REQ(driver.contains("Sabatier reactors")
			&& driver.contains("hydrogen feed store 'h2' has nothing drawable"),
			"window did not surface the Sabatier limiting process");
		REQ(driver.contains("Methane processors"),
			"window missing the methane processing stage");
		REQ(driver.contains("Microbial processors"),
			"window missing the microbial nutrient stage");
		REQ(driver.contains("Crop populations") && driver.contains("Algae cultures"),
			"window missing the coupled biology stages");
		REQ(driver.contains("CO2 capture"),
			"window missing the CO2 capture stage");
	});
	#undef REQ
	require(driver.passed(), "coupled live view failed on the Test Engine");
}

	// #418: the separate Station Systems window shows bounded trends and
	// the categorized incident history; toggling visibility and switching
	// the active World never leak state, and the authored retention is
	// editable through the real paused-only controls.
	void trendsWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200.0f, 3200.0f};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto const makeRig = [](std::string const& loadId)
		{
			core::StationSystemsSettings settings;
			settings.enabled = true;
			settings.observationIntervalSeconds = 1.0f;
			settings.trendRetentionSeconds = 300.0f;
			core::ResourceStoreSettings source;
			source.id = "source"; source.unit = "kg";
			source.capacity = 10000.0; source.startingQualified = 8000.0;
			core::ResourceStoreSettings sink;
			sink.id = "sink"; sink.unit = "kg";
			sink.capacity = 10000.0; sink.startingQualified = 0.0;
			settings.stores = { source, sink };
			core::ResourceTransferSettings pump;
			pump.id = "pump"; pump.sourceId = "source"; pump.destinationId = "sink";
			pump.unit = "kg"; pump.ratePerSecond = 1.0; pump.priority = 5;
			settings.transfers = { pump };
			core::ProfileSettings sunLight;
			sunLight.constantLevel = 1.0;
			core::SolarArraySettings sun;
			sun.id = "sun"; sun.capacityKW = 10.0; sun.illumination = sunLight;
			settings.solarArrays = { sun };
			core::EquipmentLoadSettings load;
			load.id = loadId; load.electricalDemandKW = 6.0;
			load.operatingProfile = sunLight;
			load.heatOutputKW = 0.0; load.thermalClass = "low";
			load.coolingDisposition = core::StationCoolingDisposition::Exported;
			load.criticalFloorKW = 0.0; load.priority = 10; load.maxCurtailment = 1.0;
			settings.equipmentLoads = { load };
			return settings;
		};
		auto world = std::make_shared<core::World>("Trends editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		require(world->configureStationSystems(makeRig("shop"), false, &diagnostic),
			diagnostic.c_str());
		world->resumeSimulation();
		require(world->advanceTicks(300), "trends editor pre-run failed"); // t = 5 s
		world->markSaved(); // the configure edit is expected dirt; rendering must add none
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({1150, 3100});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)
		driver.run("station", "trendsWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("Active World: Trends editor"),
				"station window missing the trends World");
			REQ(driver.contains("Trend retention: 300 s"),
				"authored trend retention missing from the window");
			// The bounded trends section is open by default and presents
			// the series identity, units and effective stride.
			REQ(driver.contains("power (station-wide) (kW)"),
				"station-wide power trend series missing from the window");
			REQ(driver.contains("equipment-load 'shop' (kW)"),
				"equipment load trend series missing from the window");
			REQ(driver.contains("transfer 'pump' (kg)"),
				"transfer trend series missing from the window");
			REQ(driver.contains("effective stride 1.00 s"),
				"trend effective sample stride missing");
			// The incident history shows simulation timestamps and the
			// stable presentation categories.
			REQ(driver.contains("[observation]"),
				"observation category missing from the incident history");
			REQ(driver.contains("[station]"),
				"station category missing from the incident history");
			// Visibility toggling never mutates state: hide the window, keep
			// ticking, show it again. The trends advanced only with World
			// ticks, and rendering dirtied nothing.
			window.visible = false;
			ctx->Sleep(2);
			require(world->advanceTicks(120), "trends run during hidden window failed");
			window.visible = true;
			ctx->Sleep(2);
			REQ(world->getStationSystemsTrends().size() == 3
				&& world->getStationSystemsTrends()[0].samples.size() == 7,
				"trends did not advance with the World ticks while the window was hidden");
			REQ(driver.contains("power (station-wide) (kW)"),
				"trend series vanished after toggling window visibility");
			REQ(!world->isModified(), "rendering the trends view dirtied the document");
			// Paused-only retention editing: invalid values are refused
			// visibly with no mutation; a valid value applies through the
			// reset-confirmation path and commits document history.
			world->pauseSimulation();
			ctx->Sleep(1);
			ctx->ItemInputValue("Trend retention (seconds)", "0");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			ctx->ItemClick("Confirm World reset");
			ctx->Sleep(1);
			REQ(driver.contains("trend retention must be finite"),
				"invalid trend retention was not refused visibly");
			REQ(world->getStationSystemsSettings().trendRetentionSeconds == 300.0f,
				"invalid trend retention mutated the authored settings");
			ctx->ItemClick("Cancel edits");
			ctx->Sleep(1);
			ctx->ItemInputValue("Trend retention (seconds)", "120");
			ctx->ItemClick("Apply settings");
			ctx->Sleep(1);
			ctx->ItemClick("Confirm World reset");
			ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().trendRetentionSeconds == 120.0f
				&& history.undoCount() >= 1,
				"valid trend retention edit did not apply through the reset confirmation");
			REQ(world->getSimulationTick() == 0,
				"the confirmed retention change did not reset the World");
			// Active-World switch: the window rebinds to the new World and
			// shows only that World's series - nothing of the old one leaks.
			world = std::make_shared<core::World>("Trends second", 2, 2);
			world->pauseSimulation(); world->finishBuild();
			require(world->configureStationSystems(makeRig("lab"), false, &diagnostic),
				diagnostic.c_str());
			world->resumeSimulation();
			require(world->advanceTicks(180), "second trends world run failed"); // t = 3 s
			ctx->Sleep(2);
			REQ(driver.contains("Active World: Trends second"),
				"window did not follow the active-World switch");
			REQ(driver.contains("equipment-load 'lab' (kW)"),
				"switched World's load trend series missing");
			REQ(!driver.contains("equipment-load 'shop'"),
				"previous World's load trend series leaked into the switched view");
			REQ(world->getStationSystemsTrends()[1].samples.size() == 3,
				"switched World's trend history is not its own");
		});
		#undef REQ
		require(driver.passed(), "trends workflow failed on the Test Engine");
	}

void cropWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200.0f, 4200.0f};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Crop workflow editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 1.0f;
		auto plain = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings store;
			store.id = id;
			store.unit = "kg";
			store.capacity = capacity;
			store.startingQualified = starting;
			return store;
		};
		initial.stores.push_back(plain("irrigation", 200.0, 150.0));
		initial.stores.push_back(plain("fertN", 10.0, 5.0));
		initial.stores.push_back(plain("fertP", 10.0, 5.0));
		initial.stores.push_back(plain("fertK", 10.0, 5.0));
		initial.stores.push_back(plain("fertMin", 10.0, 5.0));
		initial.stores.push_back(plain("food", 10.0, 0.0));
		initial.stores.push_back(plain("waste", 10.0, 0.0));
		core::AtmosphereSettings habitat;
		habitat.id = "habitat";
		habitat.hazardClass = "normal";
		habitat.volumeM3 = 100.0;
		habitat.startingOxygenKg = 40.0;
		habitat.startingNitrogenKg = 80.0;
		habitat.startingCarbonDioxideKg = 5.0;
		habitat.startingWaterVapourKg = 1.0;
		habitat.startingTemperatureC = 22.0;
		habitat.thermalCapacityKJPerK = 20000.0;
		habitat.minPressureKPa = 1.0;
		habitat.maxPressureKPa = 200.0;
		initial.atmospheres.push_back(habitat);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		initial.solarArrays.push_back(field);
		core::CropPopulationSettings canopy;
		canopy.id = "canopy";
		canopy.atmosphereId = "habitat";
		canopy.waterStoreId = "irrigation";
		canopy.nitrogenStoreId = "fertN";
		canopy.phosphorusStoreId = "fertP";
		canopy.potassiumStoreId = "fertK";
		canopy.mineralsStoreId = "fertMin";
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.0;
		composition.nutrientNitrogen = 0.05;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.03;
		composition.otherMinerals = 0.05;
		composition.energyKJPerKgDry = 1700.0;
		canopy.composition = composition;
		canopy.growingAreaM2 = 10.0;
		canopy.maxBiomassDensityKgPerM2 = 2.0; // 20 kg Growing capacity
		canopy.initialBiomassKg = 1.0;
		canopy.lampRatedKW = 3.6;
		canopy.illuminationProfile.constantLevel = 1.0;
		canopy.circulationPowerKW = 0.1;
		canopy.growthYieldKgPerKWh = 2.0;
		canopy.respirationPerHour = 0.5;
		canopy.transpirationKgPerKgGrowth = 5.0;
		canopy.minGrowingTemperatureC = 10.0;
		canopy.maxGrowingTemperatureC = 35.0;
		canopy.foodStoreId = "food";
		canopy.wasteStoreId = "waste";
		canopy.edibleFraction = 0.6;
		canopy.harvestTriggerKg = 0.3;
		canopy.harvestFraction = 1.0;
		initial.cropPopulations.push_back(canopy);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 4100});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "cropWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 crop population(s)"),
				"Authored crop population count missing from the window");
			// Run: the live view shows the canopy growing inside its declared
			// capacity, funded by delivered light, with the growth scale and
			// the living-biomass picture visible. Ticking never dirties the
			// authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(120), "Editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("canopy [habitat]")
				&& driver.contains("capacity 20.0000 kg")
				&& driver.contains("growing"),
				"Live crop canopy, declared capacity or growth not displayed");
			REQ(driver.contains("harvest: trigger 0.3000 kg")
				&& driver.contains("food 'food'")
				&& driver.contains("waste 'waste'"),
				"Harvest rule and destinations not displayed in the live crop view");
			REQ(!world->isModified(), "Crop growth ticking dirtied the authored document");
			// An invalid drafted initial biomass (fuller than the installation
			// can support) is refused with a visible diagnostic and never
			// reaches the authored settings.
			driver.typeValue(ctx, "Crop 0 initial dry biomass kg", "999");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("exceeds the declared Growing capacity"),
				"Invalid drafted initial biomass diagnostic not shown");
			REQ(world->getStationSystemsSettings().cropPopulations[0].initialBiomassKg == 1.0
				&& world->getSimulationTick() == 120,
				"Invalid drafted initial biomass mutated the authored settings");
			// A valid drafted growing area applies through the World-reset
			// confirmation and restarts the population from its authored start.
			driver.typeValue(ctx, "Crop 0 initial dry biomass kg", "2");
			driver.typeValue(ctx, "Crop 0 growing area m2", "20");
			driver.typeValue(ctx, "Crop 0 harvest trigger kg (0 disables harvest)", "0.5");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().cropPopulations[0].growingAreaM2 == 20.0
				&& world->getStationSystemsSettings().cropPopulations[0].initialBiomassKg == 2.0
				&& world->getStationSystemsSettings().cropPopulations[0].harvestTriggerKg == 0.5
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired crop draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			world->resumeSimulation(); REQ(world->advanceTicks(120), "Post-apply run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			// The doubled area doubles the declared capacity to 40 kg.
			REQ(driver.contains("capacity 40.0000 kg")
				&& driver.contains("1 crop population(s)"),
				"Applied growing area not reflected in the live capacity display");
			// #581: a drafted finite growing area whose product with the
			// authored density overflows the derived Growing capacity is
			// refused with a visible diagnostic and never reaches the
			// authored settings. Repairing the area applies through the
			// reset-confirmed flow.
			driver.typeValue(ctx, "Crop 0 growing area m2", "1e308");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("overflows"),
				"Overflowed derived Growing capacity diagnostic not shown");
			REQ(world->getStationSystemsSettings().cropPopulations[0].growingAreaM2 == 20.0
				&& world->getSimulationTick() == 120,
				"Refused overflowed crop draft mutated the authored settings or the run");
			driver.typeValue(ctx, "Crop 0 growing area m2", "30");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().cropPopulations[0].growingAreaM2 == 30.0
				&& history.undoCount() == 2 && world->getSimulationTick() == 0,
				"Repaired crop capacity factor did not apply through the reset-confirmed flow");
		});
		#undef REQ
		require(driver.passed(), "Crop workflow failed on the Test Engine");
	}
	void algaeWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200.0f, 4200.0f};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Algae workflow editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 1.0f;
		auto plain = [](std::string const& id, double capacity, double starting)
		{
			core::ResourceStoreSettings store;
			store.id = id;
			store.unit = "kg";
			store.capacity = capacity;
			store.startingQualified = starting;
			return store;
		};
		initial.stores.push_back(plain("makeup", 200.0, 150.0));
		initial.stores.push_back(plain("fertN", 10.0, 5.0));
		initial.stores.push_back(plain("fertP", 10.0, 5.0));
		initial.stores.push_back(plain("fertK", 10.0, 5.0));
		initial.stores.push_back(plain("fertMin", 10.0, 5.0));
		initial.stores.push_back(plain("afood", 10.0, 0.0));
		initial.stores.push_back(plain("arecovery", 10.0, 0.0));
		initial.stores.push_back(plain("awaste", 10.0, 0.0));
		core::AtmosphereSettings habitat;
		habitat.id = "habitat";
		habitat.hazardClass = "normal";
		habitat.volumeM3 = 100.0;
		habitat.startingOxygenKg = 40.0;
		habitat.startingNitrogenKg = 80.0;
		habitat.startingCarbonDioxideKg = 5.0;
		habitat.startingWaterVapourKg = 1.0;
		habitat.startingTemperatureC = 22.0;
		habitat.thermalCapacityKJPerK = 20000.0;
		habitat.minPressureKPa = 1.0;
		habitat.maxPressureKPa = 200.0;
		initial.atmospheres.push_back(habitat);
		core::SolarArraySettings field;
		field.id = "field";
		field.capacityKW = 100.0;
		initial.solarArrays.push_back(field);
		core::AlgaeCultureSettings pond;
		pond.id = "pond";
		pond.atmosphereId = "habitat";
		pond.waterStoreId = "makeup";
		pond.nitrogenStoreId = "fertN";
		pond.phosphorusStoreId = "fertP";
		pond.potassiumStoreId = "fertK";
		pond.mineralsStoreId = "fertMin";
		core::ResourceCompositionSettings composition;
		composition.moisture = 0.0;
		composition.nutrientNitrogen = 0.06;
		composition.atmosphericNitrogen = 0.0;
		composition.phosphorus = 0.02;
		composition.potassium = 0.03;
		composition.otherMinerals = 0.04;
		composition.energyKJPerKgDry = 1700.0;
		pond.composition = composition;
		pond.cultureVolumeL = 1000.0;
		pond.maxBiomassDensityKgPerL = 0.002; // 2 kg culture capacity
		pond.initialBiomassKg = 0.5;
		pond.lampRatedKW = 3.6;
		pond.illuminationProfile.constantLevel = 1.0;
		pond.circulationPowerKW = 0.1;
		pond.growthYieldKgPerKWh = 2.0;
		pond.respirationPerHour = 0.5;
		pond.evaporationKgPerKgGrowth = 1.0;
		pond.minGrowingTemperatureC = 10.0;
		pond.maxGrowingTemperatureC = 35.0;
		pond.foodStoreId = "afood";
		pond.recoveryStoreId = "arecovery";
		pond.wasteStoreId = "awaste";
		pond.foodFraction = 0.4;
		pond.recoveryFraction = 0.3;
		pond.harvestTriggerKg = 0.3;
		pond.harvestFraction = 1.0;
		initial.algaeCultures.push_back(pond);
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 4100});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "algaeWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("1 algae culture(s)"),
				"Authored algae culture count missing from the window");
			// Run: the live view shows the culture growing inside its own
			// culture-volume capacity, funded by delivered light, with the
			// three-way harvest rule displayed. Ticking never dirties the
			// authored document.
			world->markSaved();
			world->resumeSimulation(); REQ(world->advanceTicks(120), "Editor run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			REQ(driver.contains("pond [habitat]")
				&& driver.contains("culture capacity 2.0000 kg")
				&& driver.contains("growing"),
				"Live algae culture, declared culture capacity or growth not displayed");
			REQ(driver.contains("harvest: trigger 0.3000 kg")
				&& driver.contains("food 'afood'")
				&& driver.contains("recovery 'arecovery'")
				&& driver.contains("waste 'awaste'"),
				"Harvest partition rule and destinations not displayed in the live culture view");
			REQ(!world->isModified(), "Algae growth ticking dirtied the authored document");
			// An invalid drafted partition (food + recovery above the
			// harvest) is refused with a visible diagnostic and never
			// reaches the authored settings.
			driver.typeValue(ctx, "Algae 0 recovery fraction of harvest", "0.9");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(driver.contains("food and recovery fractions together exceed 1"),
				"Invalid drafted harvest partition diagnostic not shown");
			REQ(world->getStationSystemsSettings().algaeCultures[0].recoveryFraction == 0.3
				&& world->getSimulationTick() == 120,
				"Invalid drafted harvest partition mutated the authored settings");
			// A valid drafted culture volume applies through the
			// reset-confirmed flow and restarts the population from its
			// authored start.
			driver.typeValue(ctx, "Algae 0 recovery fraction of harvest", "0.3");
			driver.typeValue(ctx, "Algae 0 active culture volume L", "2000");
			driver.typeValue(ctx, "Algae 0 initial dry biomass kg", "1");
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().algaeCultures[0].cultureVolumeL == 2000.0
				&& world->getStationSystemsSettings().algaeCultures[0].initialBiomassKg == 1.0
				&& history.undoCount() == 1 && world->getSimulationTick() == 0,
				"Repaired algae draft did not apply through the reset-confirmed flow");
			ctx->Sleep(1);
			world->resumeSimulation(); REQ(world->advanceTicks(120), "Post-apply run failed");
			world->pauseSimulation(); ctx->Sleep(1);
			// The doubled volume doubles the declared capacity to 4 kg.
			REQ(driver.contains("culture capacity 4.0000 kg")
				&& driver.contains("1 algae culture(s)"),
				"Applied culture volume not reflected in the live capacity display");
		});
		#undef REQ
		require(driver.passed(), "Algae workflow failed on the Test Engine");
	}
	// #414: the real Station Systems window drafts, validates, refuses
	// and applies resource-constrained maintenance edits, and the live
	// view reports the phase, isolation, reservation and spend through
	// the headless ImGui seam.
	void maintenanceWorkflow(smoke::Context const&)
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		auto& io = ImGui::GetIO();
		io.DisplaySize = {1200, 1700};
		io.Fonts->AddFontDefault(); io.Fonts->Build();
		DocumentHistory history;
		StationSystemsWindow window;
		auto world = std::make_shared<core::World>("Maintenance editor", 2, 2);
		world->pauseSimulation(); world->finishBuild();
		std::string diagnostic;
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 1.0f;
		core::ResourceStoreSettings tools;
		tools.id = "tools";
		tools.unit = "kg";
		tools.capacity = 50.0;
		tools.startingQualified = 10.0;
		core::ResourceStoreSettings parts;
		parts.id = "parts";
		parts.unit = "kg";
		parts.capacity = 50.0;
		parts.startingQualified = 5.0;
		core::ResourceStoreSettings waste;
		waste.id = "waste";
		waste.unit = "kg";
		waste.capacity = 20.0;
		waste.startingQualified = 0.0;
		core::ResourceStoreSettings diesel;
		diesel.id = "diesel";
		diesel.unit = "kWh";
		diesel.capacity = 1000.0;
		diesel.startingQualified = 1000.0;
		core::ThermalReservoirSettings hot;
		hot.id = "hot";
		hot.thermalClass = "generation";
		hot.heatCapacityKJPerK = 10000.0;
		hot.startingTemperatureC = 20.0;
		hot.minTemperatureC = -50.0;
		hot.maxTemperatureC = 200.0;
		core::GeneratorSettings gen;
		gen.id = "gen";
		gen.fuelStoreId = "diesel";
		gen.thermalClass = "generation";
		gen.heatSinkId = "hot";
		gen.ratedOutputKW = 10.0;
		gen.efficiency = 0.5;
		gen.maxRampKWPerSecond = 100.0;
		gen.commandedOutputKW = 10.0;
		gen.startupPowerKW = 1.0;
		gen.startupSeconds = 1.0;
		gen.maxSinkTemperatureC = 90.0;
		initial.stores = {tools, parts, waste, diesel};
		initial.thermalReservoirs = {hot};
		initial.generators = {gen};
		require(world->configureStationSystems(initial, false, &diagnostic), diagnostic.c_str());
		headless::TestEngineDriver driver([&]
		{
			ImGui::SetNextWindowPos({10, 10}); ImGui::SetNextWindowSize({950, 1650});
			window.render(world, history);
		});
		window.visible = true;
		driver.frame(); driver.frame();
		#define REQ(cond, msg) do { if (!driver.require((cond), (msg))) return; } while (0)

		driver.run("station", "maintenanceWorkflow", [&](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Station Systems");
			REQ(driver.contains("0 maintenance job(s)"),
				"Authored maintenance count missing from the window");
			// Draft a job through the real controls. The freshly added
			// job points at the existing generator and store.
			driver.click(ctx, "Add maintenance job");
			REQ(driver.contains("Draft differs from the authored start conditions"),
				"Added maintenance draft not reported as a change");
			driver.typeValue(ctx, "Maintenance job 0 first occurrence seconds", "5");
			driver.typeValue(ctx, "Maintenance job 0 duration seconds", "4");
			driver.typeValue(ctx, "Maintenance job 0 waste store id", "waste");
			driver.typeValue(ctx, "Maintenance requirement 0.0 store id", "tools");
			driver.typeValue(ctx, "Maintenance requirement 0.0 quantity", "2");
			driver.click(ctx, "Add requirement to maintenance job 0");
			driver.typeValue(ctx, "Maintenance requirement 0.1 store id", "parts");
			driver.typeValue(ctx, "Maintenance requirement 0.1 quantity", "1");
			driver.click(ctx, "Maintenance requirement 0.1 consumed (unchecked = reusable tool)");
			// An invalid draft - unknown installation - is refused with a
			// visible diagnostic and never reaches the authored settings.
			driver.typeValue(ctx, "Maintenance job 0 installation id", "ghost");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(driver.contains("targets unknown installation 'ghost'"),
				"Unknown-installation maintenance draft diagnostic not shown");
			REQ(world->getStationSystemsSettings().maintenanceJobs.empty()
				&& history.undoCount() == 0,
				"Invalid maintenance draft mutated the authored settings");
			// Repair the draft and apply through the document seam.
			driver.typeValue(ctx, "Maintenance job 0 installation id", "gen");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().maintenanceJobs.size() == 1
				&& world->getStationSystemsSettings().maintenanceJobs[0].requirements.size() == 2
				&& world->getStationSystemsSettings().maintenanceJobs[0].wasteStoreId == "waste"
				&& history.undoCount() == 1,
				"Repaired maintenance draft did not apply");
			// Run through the window: the job starts, isolates the
			// generator, completes, and the live view reports the spend.
			world->markSaved();
			world->resumeSimulation();
			REQ(world->advanceTicks(8 * 60), "Maintenance editor run failed mid-job");
			ctx->Sleep(1);
			REQ(driver.contains("maint1 on generator 'gen': active [ISOLATED]"),
				"Active isolated maintenance not shown in the live view");
			REQ(driver.contains("tools 2.000000 kg reusable: reserved")
				&& driver.contains("parts 1.000000 kg consumed: reserved"),
				"Live reservation state not displayed");
			REQ(world->advanceTicks(8 * 60), "Maintenance editor run failed past completion");
			ctx->Sleep(1);
			REQ(driver.contains("maint1 on generator 'gen': completed")
				&& !driver.contains("maint1 on generator 'gen': active"),
				"Completed maintenance not shown in the live view");
			REQ(driver.contains("last spent 1.000000")
				&& driver.contains("waste (kg) [normal]: usable 1.000"),
				"Completed spend into the waste store not displayed");
			REQ(!world->isModified(), "Maintenance ticking dirtied the authored document");
			// Removing the job applies through the same seam.
			world->pauseSimulation();
			driver.click(ctx, "Remove maintenance job 0");
			driver.click(ctx, "Apply settings"); ctx->Sleep(1);
			driver.click(ctx, "Confirm World reset"); ctx->Sleep(1);
			REQ(world->getStationSystemsSettings().maintenanceJobs.empty()
				&& history.undoCount() == 2,
				"Removed maintenance job did not apply");
		});
		#undef REQ
		require(driver.passed(), "Maintenance workflow failed on the Test Engine");
	}
	void editor_smoke::registerStationSystems(std::vector<smoke::Check>& checks)
{
	checks.push_back({"station/windowWorkflow", workflow});
	checks.push_back({"station/removalWorkflow", removalWorkflow});
	checks.push_back({"station/storeWorkflow", storeWorkflow});
	checks.push_back({"station/transferWorkflow", transferWorkflow});
	checks.push_back({"station/compoundWorkflow", compoundWorkflow});
	checks.push_back({"station/powerWorkflow", powerWorkflow});
	checks.push_back({"station/batteryWorkflow", batteryWorkflow});
	checks.push_back({"station/deliveryWorkflow", deliveryWorkflow});
	checks.push_back({"station/wasteEjectionWorkflow", wasteEjectionWorkflow});
	checks.push_back({"station/containmentWorkflow", containmentWorkflow});
	checks.push_back({"station/thermalWorkflow", thermalWorkflow});
	checks.push_back({"station/coolantWorkflow", coolantWorkflow});
	checks.push_back({"station/chillerWorkflow", chillerWorkflow});
	checks.push_back({"station/generatorWorkflow", generatorWorkflow});
	checks.push_back({"station/bufferWorkflow", bufferWorkflow});
	checks.push_back({"station/microbialConditionWorkflow", microbialConditionWorkflow});
	checks.push_back({"station/cropWorkflow", cropWorkflow});
	checks.push_back({"station/algaeWorkflow", algaeWorkflow});
	checks.push_back({"station/maintenanceWorkflow", maintenanceWorkflow});
	checks.push_back({"station/coupledLiveViewWorkflow", coupledLiveViewWorkflow});
	checks.push_back({"station/trendsWorkflow", trendsWorkflow});
}
