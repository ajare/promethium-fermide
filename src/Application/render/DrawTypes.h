#pragma once

#include <cstdint>

namespace pf::application::render {
// Application-local drawing types. Neither the command stream nor its GPU
// renderer depends on an ImGui context, header version or linked runtime.
struct Vec2 {
  float x{}, y{};
  constexpr Vec2(float x = 0, float y = 0) : x(x), y(y) {}
};
struct Vec4 {
  float x{}, y{}, z{}, w{};
  constexpr Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};
using PackedColourValue = std::uint32_t;
using DrawFlags = int;
inline constexpr DrawFlags DrawFlagsNone = 0, DrawFlagsClosed = 1;
inline constexpr int RedShift = 0, GreenShift = 8, BlueShift = 16, AlphaShift = 24;
constexpr PackedColourValue packColour(int r, int g, int b, int a = 255) {
  return (static_cast<PackedColourValue>(r) << RedShift) |
         (static_cast<PackedColourValue>(g) << GreenShift) |
         (static_cast<PackedColourValue>(b) << BlueShift) |
         (static_cast<PackedColourValue>(a) << AlphaShift);
}
inline constexpr auto White = packColour(255, 255, 255);
} // namespace pf::application::render
