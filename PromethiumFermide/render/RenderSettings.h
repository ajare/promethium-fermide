#pragma once

namespace pf::application::render {
struct RenderSettings {
  float worldViewportX{}, worldViewportY{};
  float worldViewportWidth{}, worldViewportHeight{};
  float xOffset{}, yOffset{}, worldZoom{1.0f};
  int visibleLayer{};
};
extern RenderSettings gRenderSettings;
} // namespace pf::application::render
