#include "Checks.h"
#include "ImGuiContext.h"
#include "BrokenEscalatorFixture.h"
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
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		broken_escalator::Scene scene;
		scene.stairs->update(0.5f);
		auto draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
		auto render = [&](LayerRenderStyle style)
		{
			draw->_ResetForNewFrame(); draw->PushClipRect(ImVec2(0, 0), ImVec2(1280, 720), false);
			renderSector(scene.world->getSector(scene.owner), 1, style, false, ImColor(192, 192, 255), draw); draw->PopClipRect();
		};
		auto geometry = [&]
		{
			std::vector<std::tuple<float, float, ImU32>> vertices;
			for (auto const& vertex : draw->VtxBuffer)
				if (vertex.col == IM_COL32(32, 32, 32, 255) || vertex.col == IM_COL32(220, 220, 220, 255))
					vertices.emplace_back(vertex.pos.x, vertex.pos.y, vertex.col);
			return vertices;
		};
		auto warningColour = ImU32(ImColor(255, 166, 26, 255));
		render(LayerRenderStyle::Solid); auto baseline = geometry(); require(!baseline.empty(), "Missing Escalator steps");
		scene.world->setEscalatorBroken(scene.owner, true); scene.stairs->update(100); render(LayerRenderStyle::Solid);
		require(geometry() == baseline && scene.stairs->getDescription().find("Broken") != std::string::npos, "Broken changed physical geometry/missing status");
		core::Vector2 low, high; scene.stairs->getFullShape(low, high);
		bool visible = false;
		for (auto const& vertex : draw->VtxBuffer)
			if (vertex.col == warningColour) { visible = true; require(vertex.pos.y < 720 - high.y * CORE_LEVEL_HEIGHT_PIXELS, "Warning obscured device position"); }
		require(visible, "Broken warning missing");
		render(LayerRenderStyle::Wireframe);
		require(std::any_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& v) { return v.col == warningColour; }), "Wireframe warning missing");
		scene.world->setEscalatorBroken(scene.owner, false); render(LayerRenderStyle::Solid);
		require(geometry() == baseline && std::none_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& v) { return v.col == warningColour; }), "Restore reset steps or retained warning");
		ImGui::EndFrame();
	}
}

void render_smoke::registerBrokenEscalators(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "escalators/brokenWarningPreservesPosition", isolated<[](smoke::Context const&) { warning(); }> });
}
