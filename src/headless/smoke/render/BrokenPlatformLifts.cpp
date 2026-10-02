#include "Checks.h"
#include "ImGuiContext.h"
#include "BrokenPlatformLiftFixture.h"
#include "Render.h"
#include "UISettings.h"
#include "imgui/imgui.h"
#include <tuple>

extern UISettings gUISettings;

namespace
{
	void warning()
	{
		using smoke::require;
		headless::ScopedImGuiContext context;
		ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build(); ImGui::NewFrame();
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0;
		gUISettings.worldViewportWidth = 1280; gUISettings.worldViewportHeight = 720;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0; gUISettings.worldZoom = 1.0f;
		broken_platform_lift::Scene scene; scene.start();
		scene.until([&] { auto state = scene.snapshot(); return state.liftMoving && state.liftPosition > 0.5f; });
		auto draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
		auto render = [&](LayerRenderStyle style)
		{ draw->_ResetForNewFrame(); draw->PushClipRect(ImVec2(0, 0), ImVec2(1280, 720), false);
			renderSector(scene.world->getSector(scene.owner), 0, style, false, ImColor(192, 192, 255), draw); draw->PopClipRect(); };
		auto warningColour = ImU32(ImColor(255, 166, 26, 255));
		auto geometry = [&]
		{
			std::vector<std::tuple<float, float, ImU32>> vertices;
			for (auto const& v : draw->VtxBuffer)
				if ((v.col | IM_COL32_A_MASK) != warningColour
					&& (v.col | IM_COL32_A_MASK) != ImU32(ImColor(30, 20, 0, 255))) vertices.emplace_back(v.pos.x, v.pos.y, v.col);
			return vertices;
		};
		render(LayerRenderStyle::Solid); auto baseline = geometry(); require(!baseline.empty(), "Missing car geometry");
		scene.world->setLiftBroken(scene.made.traversalResource, true); scene.world->advanceTicks(600); render(LayerRenderStyle::Solid);
		require(geometry() == baseline && scene.lift->getDescription().find("Broken") != std::string::npos, "Broken altered physical geometry/missing status");
		core::Vector2 low, high; scene.lift->getCurrentShape(low, high);
		bool visible = false;
		for (auto const& vertex : draw->VtxBuffer)
			if (vertex.col == warningColour) { visible = true; require(vertex.pos.y < 720 - high.y * CORE_LEVEL_HEIGHT_PIXELS, "Warning obscured frozen car position"); }
		require(visible, "Broken warning missing");
		render(LayerRenderStyle::Wireframe);
		require(std::any_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& v) { return v.col == warningColour; }), "Wireframe warning missing");
		scene.world->setLiftBroken(scene.made.traversalResource, false); render(LayerRenderStyle::Solid);
		require(geometry() == baseline && std::none_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& v) { return v.col == warningColour; }), "Restore moved car or retained warning");
		ImGui::EndFrame();
	}
}

void render_smoke::registerBrokenPlatformLifts(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "platformLifts/brokenWarningPreservesPosition", isolated<[](smoke::Context const&) { warning(); }> });
}
