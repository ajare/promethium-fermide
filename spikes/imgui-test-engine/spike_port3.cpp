// PILOT 3: what does the Test Engine do when asked to click a DISABLED
// control? This is the semantic risk flagged before bulk-migrating the
// remaining checks: several tests click a disabled widget specifically to
// assert that nothing happens.
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include <cstdio>
#include <string>

namespace
{
	int g_clicks{ 0 };
	int g_hover_disabled{ 0 };
	int g_mouse_over_disabled{ 0 };
	int g_pressed_disabled{ 0 };
	ImRect g_disabledRect;
	std::string g_text;

	void renderFrame()
	{
		ImGui::NewFrame();
		ImGuiIO& fio = ImGui::GetIO();
		fio.SetClipboardTextFn = [](void* d, char const* v) { *static_cast<std::string*>(d) = v; };
		fio.ClipboardUserData = &g_text;
		ImGui::SetNextWindowPos({ 10, 10 });
		ImGui::SetNextWindowSize({ 420, 320 });
		ImGui::Begin("Pilot 3");
		g_text.clear();
		ImGui::LogToClipboard();
		ImGui::Text("clicks: %d", g_clicks);
		if (ImGui::Button("Enabled Button")) ++g_clicks;
		ImGui::BeginDisabled(true);
		if (ImGui::Button("Disabled Button")) ++g_clicks;
		if (ImGui::IsItemHovered()) ++g_hover_disabled;
		if (ImGui::IsItemClicked()) ++g_pressed_disabled;
		g_disabledRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		ImGui::EndDisabled();
		// Geometry-only probe: true whenever the mouse is over the disabled
		// button, independent of ImGui's disabled-hover suppression.
		if (ImGui::IsMouseHoveringRect(g_disabledRect.Min, g_disabledRect.Max))
			++g_mouse_over_disabled;
		ImGui::LogFinish();
		ImGui::End();
		ImGui::Render();
	}
}

int main()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.DisplaySize = ImVec2(800, 600);
	io.Fonts->AddFontDefault();
	io.Fonts->Build();

	renderFrame();

	ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO& tio = ImGuiTestEngine_GetIO(engine);
	tio.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
	tio.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
	tio.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;
	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	for (int i = 0; i < 4; ++i) renderFrame();

	// --- Case 1: click the ENABLED button (sanity baseline) ---
	{
		ImGuiTest* t = IM_REGISTER_TEST(engine, "p3", "enabled");
		t->TestFunc = [](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Pilot 3");
			ctx->ItemClick("Enabled Button");
		};
		ImGuiTestEngine_QueueTest(engine, t);
		long long f = 0;
		while (t->Status == ImGuiTestStatus_Queued || t->Status == ImGuiTestStatus_Running)
		{ renderFrame(); if (++f > 5000) break; }
		std::printf("CASE 1  enabled click : frames=%lld status=%d clicks=%d (expect 1)\n",
			f, (int)t->Status, g_clicks);
	}

	// --- Case 2: click the DISABLED button, default flags ---
	{
		g_clicks = 0; g_hover_disabled = 0; g_pressed_disabled = 0;
		ImGuiTest* t = IM_REGISTER_TEST(engine, "p3", "disabled_default");
		t->TestFunc = [](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Pilot 3");
			ctx->ItemClick("Disabled Button");
		};
		ImGuiTestEngine_QueueTest(engine, t);
		long long f = 0;
		while (t->Status == ImGuiTestStatus_Queued || t->Status == ImGuiTestStatus_Running)
		{ renderFrame(); if (++f > 5000) break; }
		std::printf("CASE 2  disabled click: frames=%lld status=%d clicks=%d hovered=%d pressed=%d\n",
			f, (int)t->Status, g_clicks, g_hover_disabled, g_pressed_disabled);
		std::printf("        status: 0=Success 3=Error\n");
		if (!t->TestLog.IsEmpty()) std::printf("        LOG: %s", t->TestLog.Buffer.c_str());
	}

	// --- Case 3: disabled click with NoError flag ---
	{
		g_clicks = 0; g_hover_disabled = 0; g_pressed_disabled = 0;
		ImGuiTest* t = IM_REGISTER_TEST(engine, "p3", "disabled_noerror");
		t->TestFunc = [](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Pilot 3");
			ctx->ItemClick("Disabled Button", ImGuiTestOpFlags_NoError);
		};
		ImGuiTestEngine_QueueTest(engine, t);
		long long f = 0;
		while (t->Status == ImGuiTestStatus_Queued || t->Status == ImGuiTestStatus_Running)
		{ renderFrame(); if (++f > 5000) break; }
		std::printf("CASE 3  disabled+NoError: frames=%lld status=%d clicks=%d hovered=%d pressed=%d\n",
			f, (int)t->Status, g_clicks, g_hover_disabled, g_pressed_disabled);
		if (!t->TestLog.IsEmpty()) std::printf("        LOG: %s", t->TestLog.Buffer.c_str());
	}

	// --- Case 4: faithful port - resolve rect via ItemInfo, then a REAL
	// mouse press at that coordinate. This is what the original scan helper
	// did: press at a point and let ImGui decide.
	{
		g_clicks = 0; g_hover_disabled = 0; g_pressed_disabled = 0; g_mouse_over_disabled = 0;
		ImGuiTest* t = IM_REGISTER_TEST(engine, "p3", "disabled_faithful");
		t->TestFunc = [](ImGuiTestContext* ctx)
		{
			ctx->SetRef("Pilot 3");
			ImGuiTestItemInfo* info = ctx->ItemInfo("Disabled Button");
			std::printf("        resolved rect=(%.0f,%.0f)-(%.0f,%.0f) InFlags disabled=%d\n",
				info->RectFull.Min.x, info->RectFull.Min.y,
				info->RectFull.Max.x, info->RectFull.Max.y,
				(int)((info->InFlags & ImGuiItemFlags_Disabled) != 0));
			ctx->MouseTeleportToPos(info->RectFull.GetCenter());
			ctx->MouseClick();
			ctx->Sleep(1);
		};
		ImGuiTestEngine_QueueTest(engine, t);
		long long f = 0;
		while (t->Status == ImGuiTestStatus_Queued || t->Status == ImGuiTestStatus_Running)
		{ renderFrame(); if (++f > 5000) break; }
		std::printf("CASE 4  faithful (rect+real press): frames=%lld status=%d clicks=%d mouseOverDisabled=%d\n",
			f, (int)t->Status, g_clicks, g_mouse_over_disabled);
	}

	ImGuiTestEngine_Stop(engine);
	ImGui::DestroyContext();
	ImGuiTestEngine_DestroyContext(engine);
	return 0;
}
