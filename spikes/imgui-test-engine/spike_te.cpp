// SPIKE: measure ImGui Test Engine item resolution vs the project's
// spatial-scan click helper, against the REAL Station Systems window.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include "core/World.h"
#include "StationSystemsWindow.h"
#include "DocumentHistory.h"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	std::string g_captured;
	StationSystemsWindow* g_window{ nullptr };
	std::shared_ptr<core::World> g_world;
	DocumentHistory* g_history{ nullptr };

	void frameOnce()
	{
		ImGui::NewFrame();
		g_captured.clear();
		ImGui::LogToClipboard();
		ImGui::SetNextWindowPos({ 10, 10 });
		ImGui::SetNextWindowSize({ 950, 850 });
		g_window->render(g_world, *g_history);
		ImGui::LogFinish();
		ImGui::Render();
	}

	std::vector<const char*> widgets()
	{
		return { "Enable Station systems", "Apply settings", "Cancel edits",
			"Add store", "Add delivery" };
	}

	// ---- Baseline: the project's existing spatial-scan helper ----
	long long scanFind()
	{
		long long probes = 0, frames = 0, found = 0;
		for (auto* label : widgets())
		{
			frameOnce();
			auto* win = ImGui::FindWindowByName("Station Systems");
			if (!win) { std::printf("  scan: window absent\n"); continue; }
			auto id = win->GetID(label);
			auto& io = ImGui::GetIO();
			bool hit = false;
			for (float y = 45; y < 845 && !hit; y += 7)
				for (float x = 18; x < 600 && !hit; x += 20)
				{
					io.AddMousePosEvent(x, y); ++probes;
					frameOnce(); ++frames;
					frameOnce(); ++frames;
					if (ImGui::GetHoveredID() == id) { hit = true; ++found; }
				}
			if (!hit) std::printf("  scan MISS: %s\n", label);
		}
		std::printf("  scan: %lld found, %lld probes, %lld frames\n", found, probes, frames);
		return frames;
	}
}

int main()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(1200, 1000);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();
	io.SetClipboardTextFn = [](void* data, char const* value) {
		*static_cast<std::string*>(data) = value; };
	io.ClipboardUserData = &g_captured;

	StationSystemsWindow window;
	DocumentHistory history;
	auto world = std::make_shared<core::World>("Spike", 2, 2);
	world->pauseSimulation();
	world->finishBuild();
	g_window = &window; g_world = world; g_history = &history;
	window.visible = true;

	long long scanFrames = scanFind();

	// ---------- Test engine ----------
	ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
	tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Debug;
	tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	// The engine tracks its own FrameCount via the imgui context hook and refuses
	// to queue a test while it lags the context by more than 2 frames. Let it
	// observe the current context frame count before queueing anything.
	for (int i = 0; i < 4; ++i)
		frameOnce();

	auto runPhase = [&](char const* name, void (*body)(ImGuiTestContext*)) -> long long
	{
		ImGuiTest* test = IM_REGISTER_TEST(engine, "spike", name);
		test->TestFunc = body;
		ImGuiTestEngine_QueueTest(engine, test);
		std::printf("  %-14s: status right after QueueTest = %d\n", name, (int)test->Status);
		long long frames = 0;
		while (test->Status == ImGuiTestStatus_Queued || test->Status == ImGuiTestStatus_Running)
		{
			frameOnce();
			++frames;
			if (frames > 20000) { std::printf("  %s: STUCK\n", name); break; }
		}
		std::printf("  %-14s: %lld frames, status=%d\n", name, frames, (int)test->Status);
		if (!test->TestLog.IsEmpty())
			std::printf("    LOG:\n%s", test->TestLog.Buffer.c_str());
		return frames;
	};

	long long infoFrames = runPhase("resolve", [](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		int resolved = 0;
		for (auto* label : { "Enable Station systems", "Apply settings", "Cancel edits",
			"Add store", "Add delivery" })
		{
			ImGuiTestItemInfo* info = ctx->ItemInfo(label, ImGuiTestOpFlags_NoError);
			if (info && !info->IsEmpty()) ++resolved;
		}
		std::printf("    engine resolved %d/5 items by label\n", resolved);
	});

	long long clickFrames = runPhase("click", [](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		for (auto* label : { "Enable Station systems", "Add store", "Add delivery" })
			ctx->ItemClick(label, ImGuiTestOpFlags_NoError);
	});

	// Determinism: identical STARTING state, same test, twice. The previous
	// pair was not a valid determinism test because run #1 left the draft
	// dirty, which changes the window layout for run #2. Rebuild a fresh
	// World and window before each run so the only variable is the engine.
	auto freshState = []
	{
		g_world = std::make_shared<core::World>("Spike", 2, 2);
		g_world->pauseSimulation();
		g_world->finishBuild();
	};

	freshState();
	long long det1 = runPhase("det#1", [](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		for (auto* label : { "Enable Station systems", "Add store", "Add delivery" })
			ctx->ItemClick(label, ImGuiTestOpFlags_NoError);
	});
	freshState();
	long long det2 = runPhase("det#2", [](ImGuiTestContext* ctx)
	{
		ctx->SetRef("Station Systems");
		for (auto* label : { "Enable Station systems", "Add store", "Add delivery" })
			ctx->ItemClick(label, ImGuiTestOpFlags_NoError);
	});
	std::printf("  determinism (identical start state): %s (%lld vs %lld frames)\n",
		det1 == det2 ? "IDENTICAL" : "DIFFERS", det1, det2);

	std::printf("\n===== SPIKE RESULT (5 real Station Systems widgets) =====\n");
	std::printf("Spatial scan (find)   : %lld frames\n", scanFrames);
	std::printf("Engine ItemInfo       : %lld frames\n", infoFrames);
	std::printf("Engine ItemClick x3   : %lld frames\n", clickFrames);
	if (infoFrames > 0)
		std::printf("Resolution speedup  : %.1fx\n", (double)scanFrames / (double)infoFrames);

	ImGuiTestEngine_Stop(engine);
	ImGui::DestroyContext();
	ImGuiTestEngine_DestroyContext(engine);
	return 0;
}
