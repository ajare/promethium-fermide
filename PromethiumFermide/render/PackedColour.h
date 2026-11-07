#pragma once

#include <algorithm>
#include "DrawTypes.h"

namespace pf::application::render {
// Keep the editor's convenient colour notation without depending on an ImGui
// context or linking a second ImGui runtime into the Application DLL.
struct Colour {
  Vec4 Value;
  Colour(int r, int g, int b, int a = 255)
      : Value(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f) {}
  operator PackedColourValue() const {
    auto channel = [](float v) { return static_cast<PackedColourValue>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (channel(Value.x) << RedShift) |
           (channel(Value.y) << GreenShift) |
           (channel(Value.z) << BlueShift) |
           (channel(Value.w) << AlphaShift);
  }
};
} // namespace pf::application::render
