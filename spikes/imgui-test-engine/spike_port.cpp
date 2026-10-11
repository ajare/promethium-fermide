// PILOT: port of station/deliveryWorkflow from spatial-scan clicking to the
// Dear ImGui Test Engine. Same scenario, same assertions, no coordinates.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include "core/World.h"
#include "StationSystemsWindow.h"
#include "DocumentHistory.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
	// Shared scenario the test body drives.
	StationSystemsWindow g_window;
	DocumentHistory g_history;
	std::shared_ptr<core::World> g_world;
	std::string g_text;
	int g_checks{ 0 };
	int g_failures{ 0 };

	void renderFrame()
	{
		ImGui::NewFrame();
		// The test engine replaces io.SetClipboardTextFn/ClipboardUserData while a
		// test runs (imgui_te_engine.cpp:1405) to implement its own
		// "copy log to clipboard" feature. This project's assertions read the
		// captured window text through that same hook, so re-assert ours every
		// frame. The engine's clipboard feature is unused here.
		ImGuiIO& fio = ImGui::GetIO();
		fio.SetClipboardTextFn = [](void* data, char const* value) {
			*static_cast<std::string*>(data) = value; };
		fio.ClipboardUserData = &g_text;
		g_text.clear();
		ImGui::LogToClipboard();
		ImGui::SetNextWindowPos({ 10, 10 });
		ImGui::SetNextWindowSize({ 950, 1650 });
		g_window.render(g_world, g_history);
		ImGui::LogFinish();
		ImGui::Render();
	}

	// Mirrors smoke::require, but reports through the engine instead of throwing.
	#define PILOT_REQUIRE(cond, msg) \
		do { ++g_checks; if (!(cond)) { ++g_failures; \
			std::printf("    FAIL: %s\n", (msg)); return; } } while (0)

	bool contains(char const* needle) { return g_text.find(needle) != std::string::npos; }

	void scenario()
	{
		g_world = std::make_shared<core::World>("Delivery editor", 2, 2);
		g_world->pauseSimulation();
		g_world->finishBuild();
		core::StationSystemsSettings initial;
		initial.enabled = true;
		initial.observationIntervalSeconds = 2.0f;
		core::ResourceStoreSettings tank;
		tank.id = "tank"; tank.unit = "kg";
		tank.capacity = 30.0; tank.startingQualified = 10.0;
		initial.stores.push_back(tank);
		core::ResourceDeliverySettings pulse;
		pulse.id = "pulse"; pulse.storeId = "tank"; pulse.unit = "kg";
		pulse.quantity = 12.0; pulse.firstOccurrenceSeconds = 1.0;
		pulse.recurrenceSeconds = 2.0;
		initial.deliveries.push_back(pulse);
		std::string diagnostic;
		if (!g_world->configureStationSystems(initial, false, &diagnostic))
			std::printf("    scenario refused: %s\n", diagnostic.c_str());
	}

	// ---- The ported test ----
	void deliveryWorkflowPort(ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");

		std::printf("    [dbg] text len=%zu deliveries=%zu enabled=%d\n",
			g_text.size(), g_world->getStationSystemsSettings().deliveries.size(),
			(int)g_world->getStationSystemsSettings().enabled);
		std::printf("    [dbg] first 300: %s\n", g_text.substr(0, 300).c_str());
		PILOT_REQUIRE(contains("1 resupply delivery(ies)"),
			"Authored delivery count missing from the window");
		PILOT_REQUIRE(contains("pulse -> tank (kg)") && contains("next arrival at 1.000 s"),
			"Authored next-arrival schedule not displayed");

		// Run into the scenario: the 30 kg store starts with 10 kg and each
		// arrival brings 12, so it fills at the second occurrence and the
		// third is refused whole.
		g_world->markSaved();
		g_world->resumeSimulation();
		if (!g_world->advanceTicks(300)) { PILOT_REQUIRE(false, "Delivery editor run failed"); }
		g_world->pauseSimulation();
		ctx->Sleep(1);
		PILOT_REQUIRE(contains("pulse -> tank (kg): refused | last at 5.000 s: 0.000 of 12.000 accepted, 12.000 rejected | total accepted 20.000, rejected 16.000 | 3 occurrence(s)"),
			"Live delivery accepted/refused accounting not displayed");
		PILOT_REQUIRE(contains("tank (kg): usable 30.000, held 0.000 of capacity 30.000"),
			"Live destination saturation not displayed");
		PILOT_REQUIRE(contains("next arrival at 7.000 s"),
			"Next arrival not advanced past the fired occurrences");
		PILOT_REQUIRE(!g_world->isModified(),
			"Delivery ticking dirtied the authored document");

		// An invalid drafted delivery is refused with a visible diagnostic.
		ctx->ItemInputValue("Delivery 0 quantity", "0");
		ctx->ItemClick("Apply settings");
		ctx->ItemClick("Confirm World reset");
		ctx->Sleep(1);
		PILOT_REQUIRE(contains("quantity must be finite and positive"),
			"Invalid delivery quantity diagnostic not shown");
		PILOT_REQUIRE(g_world->getStationSystemsSettings().deliveries[0].quantity == 12.0
			&& g_history.undoCount() == 0 && g_world->getSimulationTick() == 300,
			"Invalid delivery draft mutated the authored settings");

		// Repair the draft; the confirmed reset applies it.
		ctx->ItemInputValue("Delivery 0 quantity", "5");
		ctx->ItemClick("Apply settings");
		ctx->Sleep(1);
		PILOT_REQUIRE(contains("Confirm?"),
			"Delivery edit bypassed the World reset confirmation");
		ctx->ItemClick("Confirm World reset");
		PILOT_REQUIRE(g_world->getStationSystemsSettings().deliveries[0].quantity == 5.0
			&& g_history.undoCount() == 1 && g_world->getSimulationTick() == 0,
			"Repaired delivery draft did not apply through the reset-confirmed flow");
		ctx->Sleep(1);
		PILOT_REQUIRE(contains("0 occurrence(s)") && contains("next arrival at 1.000 s"),
			"Applied delivery did not restart from authored state");

		// A drafted sub-tick recurrence is refused.
		ctx->ItemInputValue("Delivery 0 recurrence seconds", "0.005");
		ctx->ItemClick("Apply settings");
		ctx->Sleep(1);
		PILOT_REQUIRE(contains("at least one World fixed tick"),
			"Sub-tick delivery recurrence was accepted");
		ctx->ItemClick("Cancel edits");
		PILOT_REQUIRE(g_world->getStationSystemsSettings().deliveries[0].recurrenceSeconds == 2.0,
			"Cancelled recurrence edit did not restore the authored schedule");
	}
}

int main()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(1200, 1700);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();
	io.SetClipboardTextFn = [](void* data, char const* value) {
		*static_cast<std::string*>(data) = value; };
	io.ClipboardUserData = &g_text;

	scenario();
	g_window.visible = true;
	renderFrame();
	std::printf("    [pre-engine] text len=%zu\n", g_text.size());

	ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
	tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
	tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	for (int i = 0; i < 4; ++i) renderFrame();   // let engine observe frames

	ImGuiTest* test = IM_REGISTER_TEST(engine, "pilot", "deliveryWorkflow_engine");
	test->TestFunc = deliveryWorkflowPort;
	ImGuiTestEngine_QueueTest(engine, test);

	long long frames = 0;
	while (test->Status == ImGuiTestStatus_Queued || test->Status == ImGuiTestStatus_Running)
	{
		renderFrame();
		++frames;
		if (frames > 30000) { std::printf("    STUCK\n"); break; }
	}

	std::printf("\n===== PILOT: deliveryWorkflow on the Test Engine =====\n");
	std::printf("  frames      : %lld\n", frames);
	std::printf("  assertions  : %d run, %d failed\n", g_checks, g_failures);
	std::printf("  test status : %d (0 = Success)\n", (int)test->Status);
	std::printf("  result      : %s\n",
		(g_failures == 0 && test->Status == ImGuiTestStatus_Success) ? "PASS" : "FAIL");

	ImGuiTestEngine_Stop(engine);
	ImGui::DestroyContext();
	ImGuiTestEngine_DestroyContext(engine);
	return (g_failures == 0) ? 0 : 1;
}
