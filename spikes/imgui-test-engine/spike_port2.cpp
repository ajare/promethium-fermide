// PILOT 2: port of escalators/brokenControlsAndHistory from spatial-scan
// clicking to the Dear ImGui Test Engine. Different window, different panel,
// different widget set than the station pilot.
#include "Checks.h"
#include "State.h"
#include "BrokenEscalatorFixture.h"
#include "DoorPanel.h"
#include "DocumentEdit.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"

#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
	std::shared_ptr<core::World> g_world;
	std::shared_ptr<core::Staircase> g_stairs;
	std::string g_text;
	std::string g_originalYaml;
	int g_checks{ 0 };
	int g_failures{ 0 };

	#define P_REQUIRE(cond, msg) \
		do { ++g_checks; if (!(cond)) { ++g_failures; \
			std::printf("    FAIL: %s\n", (msg)); return; } } while (0)

	std::string saveWorld()
	{
		auto writer = core::YamlSerializer::toString();
		core::SerializationWorkData work; work.markSerializedUnmodified = false;
		g_world->serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	void renderFrame()
	{
		ImGui::NewFrame();
		// Re-assert our clipboard hook: the test engine replaces it during a run.
		ImGuiIO& fio = ImGui::GetIO();
		fio.SetClipboardTextFn = [](void* data, char const* value) {
			*static_cast<std::string*>(data) = value; };
		fio.ClipboardUserData = &g_text;

		ImGui::SetNextWindowPos(ImVec2(10, 10));
		ImGui::SetNextWindowSize(ImVec2(1500, 950));
		ImGui::Begin("Escalator Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
		auto depth = GImGui->DisabledStackSize;
		auto flags = GImGui->CurrentItemFlags;
		auto alpha = GImGui->Style.Alpha;
		g_text.clear();
		ImGui::LogToClipboard();
		renderEscalatorConditionPanel(g_world, g_stairs);
		ImGui::LogFinish();
		if (depth != GImGui->DisabledStackSize || flags != GImGui->CurrentItemFlags
			|| alpha != GImGui->Style.Alpha)
		{
			++g_failures;
			std::printf("    FAIL: Escalator controls leaked disabled scope\n");
		}
		ImGui::End();
		ImGui::Render();
	}

	void portedControls(ImGuiTestContext* ctx)
	{
		ctx->SetRef("Escalator Selection");

		ctx->ItemClick("Initially Broken");
		ctx->Sleep(1);
		P_REQUIRE(g_stairs->isInitiallyBroken() && g_stairs->isBroken()
			&& gWorldDocumentHistory.undoCount() == 1
			&& g_text.find("Broken: stationary Staircase") != std::string::npos,
			"Initial control/status/history failed");
		auto const authored = saveWorld();
		g_world->markSaved();

		ctx->ItemClick("Live Broken");
		ctx->Sleep(1);
		P_REQUIRE(!g_stairs->isBroken() && g_stairs->isInitiallyBroken()
			&& saveWorld() == authored && !g_world->isModified()
			&& gWorldDocumentHistory.undoCount() == 1,
			"Live control changed authored document/history");

		g_world->resumeSimulation();
		ctx->ItemClick("Live Broken");
		ctx->Sleep(1);
		P_REQUIRE(g_stairs->isBroken() && saveWorld() == authored && !g_world->isModified(),
			"Running live control failed");
		g_world->pauseSimulation();

		auto restore = [&](DocumentSnapshot const& snapshot)
		{
			auto loaded = std::make_shared<core::World>("History", 1, 1);
			core::SerializationWorkData work;
			auto reader = core::YamlSerializer::fromString(snapshot.yaml);
			reader->deserialize();
			if (!loaded->deserialize(*reader, work)) return false;
			g_world = loaded;
			g_world->pauseSimulation();
			return true;
		};
		P_REQUIRE(gWorldDocumentHistory.undo(gWorldDocumentHistory.capture(saveWorld()), restore)
			&& saveWorld() == g_originalYaml, "Authored undo failed");
		P_REQUIRE(gWorldDocumentHistory.redo(gWorldDocumentHistory.capture(saveWorld()), restore)
			&& saveWorld() == authored, "Authored redo failed");

		broken_escalator::Scene stationary(false, 0);
		g_world = stationary.world;
		g_stairs = stationary.stairs;
		ctx->Sleep(1);
		P_REQUIRE(g_text.find("Broken") == std::string::npos,
			"Stationary Staircase exposed Broken controls");
	}
}

int main()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr; io.LogFilename = nullptr;
	io.DisplaySize = ImVec2(1600, 1000);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();

	broken_escalator::Scene scene;
	g_world = scene.world;
	g_stairs = scene.stairs;
	g_world->pauseSimulation();
	g_world->markSaved();
	gWorldDocumentHistory.clear();
	gWorldDocumentHistory.markSaved();

	renderFrame();
	g_originalYaml = saveWorld();

	ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
	tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
	tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	for (int i = 0; i < 4; ++i) renderFrame();

	ImGuiTest* test = IM_REGISTER_TEST(engine, "pilot", "brokenEscalators_engine");
	test->TestFunc = portedControls;
	ImGuiTestEngine_QueueTest(engine, test);

	long long frames = 0;
	while (test->Status == ImGuiTestStatus_Queued || test->Status == ImGuiTestStatus_Running)
	{
		renderFrame();
		++frames;
		if (frames > 30000) { std::printf("    STUCK\n"); break; }
	}

	std::printf("\n===== PILOT 2: brokenEscalators on the Test Engine =====\n");
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
