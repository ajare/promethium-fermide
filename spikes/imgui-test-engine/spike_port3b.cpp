// PILOT 3b: the disabled-click semantic validated against the REAL
// production permission panel (renderLocationPermissionRequirements),
// not a synthetic window. Answers: can the engine port
// "click while disabled must not mutate" without going vacuous?
#include "Checks.h"
#include "EditorState.h"
#include "ImGuiContext.h"
#include "PermissionsPanel.h"
#include "DocumentEdit.h"
#include "core/World.h"
#include "core/YamlSerializer.h"
#include "imgui/imgui.h"
#include "imgui_internal.h"

#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include <cstdio>
#include <memory>
#include <string>

namespace
{
	std::shared_ptr<core::World> g_world;
	std::string g_text;
	ImGuiID g_clearButton{ 0 };
	ImGuiID g_redCheckbox{ 0 };
	core::AccessPermissionId g_red{ 0 };
	ImRect g_clearRect;
	int g_mouseOverClear{ 0 };
	int g_checks{ 0 };
	int g_failures{ 0 };

	#define P_REQUIRE(cond, msg) \
		do { ++g_checks; if (!(cond)) { ++g_failures; \
			std::printf("    FAIL: %s\n", (msg)); return; } } while (0)

	std::string saveWorld()
	{
		core::SerializationWorkData work;
		auto writer = core::YamlSerializer::toString();
		g_world->serialize(*writer, work); writer->serialize();
		return writer->getSerializedString();
	}

	void renderFrame(uint32_t sector)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(10, 10));
		ImGui::SetNextWindowSize(ImVec2(900, 650));
		ImGui::Begin("Selection", nullptr, ImGuiWindowFlags_NoSavedSettings);
		auto* window = ImGui::GetCurrentWindow();
		window->StateStorage.SetInt(window->GetID("Required Access permissions"), 1);
		ImGui::PushID("Required Access permissions");
		ImGui::PushID("location-permissions");
		ImGui::PushID(static_cast<int>(g_red.value));
		g_redCheckbox = ImGui::GetID(g_world->getAccessPermissionName(g_red).c_str());
		ImGui::PopID();
		g_clearButton = ImGui::GetID("Clear");
		ImGui::PopID();
		ImGui::PopID();
		// Buffer-based capture: sidesteps the engine's clipboard hijack entirely.
		ImGui::LogToBuffer();
		renderLocationPermissionRequirements(g_world, sector);
		g_text = GImGui->LogBuffer.c_str();
		ImGui::LogFinish();
		// Geometry probe, inside the frame: was the mouse over Clear?
		if (g_clearRect.Min.x != g_clearRect.Max.x
			&& ImGui::IsMouseHoveringRect(g_clearRect.Min, g_clearRect.Max))
			++g_mouseOverClear;
		ImGui::End();
		ImGui::Render();
	}

	// The ported disabled-click assertion.
	void disabledClickPort(ImGuiTestContext* ctx, uint32_t sector)
	{
		ctx->SetRef("Selection");
		auto const before = saveWorld();
		auto const undoBefore = gWorldDocumentHistory.undoCount();

		ImGuiTestItemInfo* clear = ctx->ItemInfo(g_clearButton);
		P_REQUIRE(!clear->IsEmpty(), "Clear button did not resolve by ID");
		g_clearRect = clear->RectFull;
		std::printf("        Clear rect=(%.0f,%.0f)-(%.0f,%.0f) disabled=%d\n",
			clear->RectFull.Min.x, clear->RectFull.Min.y,
			clear->RectFull.Max.x, clear->RectFull.Max.y,
			(int)((clear->InFlags & ImGuiItemFlags_Disabled) != 0));

		// The assertion the original only inferred: the control really is
		// disabled here.
		P_REQUIRE((clear->InFlags & ImGuiItemFlags_Disabled) != 0,
			"Clear control is not disabled while running");

		// A REAL press at the resolved coordinate - not ItemClick, which
		// would silently skip a disabled target.
		ctx->MouseTeleportToPos(clear->RectFull.GetCenter());
		ctx->MouseClick();
		ctx->Sleep(2);

		P_REQUIRE(g_mouseOverClear > 0,
			"mouse never reached the disabled Clear control (vacuous test)");
		P_REQUIRE(saveWorld() == before && gWorldDocumentHistory.undoCount() == undoBefore,
			"Disabled Clear mutated the requirement");
	}
}

int main()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr; io.LogFilename = nullptr;
	io.DisplaySize = ImVec2(1000, 700);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();

	g_world = std::make_shared<core::World>("Selection", 12, 2);
	auto sector = g_world->addRoom("Selected Room", 0, 0, 0, 4, 1);
	g_world->addFacade("Facade", 0, 0, 4, 4, 1);
	g_world->addBackground(0, 0, 8, 4, 1);
	g_world->addRoom("Landing", 0, 1, 0, 4, 1);
	g_world->addLadder(1, 0, 1, { 2, false, false, 1 });
	g_world->finishBuild();
	g_world->pauseSimulation();
	g_red = g_world->addAccessPermission("Red##literal");
	g_world->addAccessPermission("Blue");
	gWorldDocumentHistory.clear();
	gWorldDocumentHistory.markSaved();
	g_world->markSaved();

	renderFrame(sector);

	ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
	tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Warning;
	tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	for (int i = 0; i < 4; ++i) renderFrame(sector);

	// Editing is refused while running: resume so the controls go disabled.
	if (!g_world->resumeSimulation()) { std::printf("resume failed\n"); return 1; }
	renderFrame(sector);

	ImGuiTest* t = IM_REGISTER_TEST(engine, "p3b", "disabled_clear_faithful");
	t->TestFunc = [sector](ImGuiTestContext* ctx) { disabledClickPort(ctx, sector); };
	ImGuiTestEngine_QueueTest(engine, t);

	long long frames = 0;
	while (t->Status == ImGuiTestStatus_Queued || t->Status == ImGuiTestStatus_Running)
	{ renderFrame(sector); if (++frames > 5000) break; }

	std::printf("\n===== PILOT 3b: disabled click on the REAL permission panel =====\n");
	std::printf("  frames            : %lld\n", frames);
	std::printf("  mouse over Clear  : %d frames (must be > 0 for a real test)\n", g_mouseOverClear);
	std::printf("  assertions        : %d run, %d failed\n", g_checks, g_failures);
	std::printf("  result            : %s\n",
		(g_failures == 0 && t->Status == ImGuiTestStatus_Success) ? "PASS" : "FAIL");

	ImGuiTestEngine_Stop(engine);
	ImGui::DestroyContext();
	ImGuiTestEngine_DestroyContext(engine);
	return (g_failures == 0) ? 0 : 1;
}
