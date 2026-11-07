#include "Checks.h"
#include "ImGuiContext.h"
#include "BrokenExtensibleFixture.h"
#include "Render.h"
#include "UISettings.h"
#include "imgui/imgui.h"
#include <tuple>

extern UISettings gUISettings;

namespace
{
	void warningPreservesPosition()
	{
		using namespace broken_extensible;
		using smoke::require;
		headless::ScopedImGuiContext context;
		ImGui::GetIO().DisplaySize = ImVec2(1280, 720);
		ImGui::GetIO().Fonts->AddFontDefault(); ImGui::GetIO().Fonts->Build();
		ImGui::NewFrame();
		gUISettings.worldViewportX = 0; gUISettings.worldViewportY = 0;
		gUISettings.worldViewportWidth = 1280; gUISettings.worldViewportHeight = 720;
		gUISettings.xOffset = 0; gUISettings.yOffset = 0;
		for (auto kind : { Kind::RoomLadder, Kind::TransitLadder, Kind::Bridge })
			for (float fraction : { 0.0f, 0.5f, 1.0f })
			{
				Scene scene(kind, false); scene.device->extend(); scene.device->update(scene.device->getExtendRetractTime() * fraction);
				auto draw = ImGui::GetBackgroundDrawList(ImGui::GetMainViewport());
				auto physicalColour = kind == Kind::Bridge ? ImU32(ImColor(0, 255, 0)) : ImU32(ImColor(128, 128, 192));
				auto warningColour = ImU32(ImColor(255, 166, 26, 255));
				auto physical = [&]
				{
					std::vector<std::tuple<float, float, ImU32>> vertices;
					for (auto const& vertex : draw->VtxBuffer)
						if (vertex.col == physicalColour) vertices.emplace_back(vertex.pos.x, vertex.pos.y, vertex.col);
					return vertices;
				};
				auto render = [&](LayerRenderStyle style)
				{
					draw->_ResetForNewFrame(); draw->PushClipRect(ImVec2(0, 0), ImVec2(1280, 720), false);
					auto sector = scene.world->getSector(scene.owner);
					renderSector(sector, sector->getLayerIndex(), style, false, ImColor(192, 192, 255), draw);
					draw->PopClipRect();
				};
				render(LayerRenderStyle::Solid); auto baseline = physical();
				scene.world->setExtensibleBroken(scene.resource, true); render(LayerRenderStyle::Solid);
				require(physical() == baseline, "Warning altered frozen physical geometry");
				core::Vector2 low, high; scene.device->getFullShape(low, high);
				auto anchorY = 720.0f - high.y * CORE_LEVEL_HEIGHT_PIXELS;
				bool warning = false;
				for (auto const& vertex : draw->VtxBuffer)
					if (vertex.col == warningColour) { warning = true; require(vertex.pos.y < anchorY, "Warning obscured physical position"); }
				require(warning, "Broken canvas warning missing");
				render(LayerRenderStyle::Wireframe);
				require(std::any_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& vertex)
					{ return vertex.col == warningColour; }), "Wireframe omitted warning");
				scene.world->setExtensibleBroken(scene.resource, false); render(LayerRenderStyle::Solid);
				require(physical() == baseline && std::none_of(draw->VtxBuffer.begin(), draw->VtxBuffer.end(), [&](auto const& vertex)
					{ return vertex.col == warningColour; }), "Restore warning/position mismatch");
			}
		ImGui::EndFrame();
	}
}

void render_smoke::registerBrokenExtensibles(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "extensibles/warningPreservesPosition", isolated<[](smoke::Context const&) { warningPreservesPosition(); }> });
}
