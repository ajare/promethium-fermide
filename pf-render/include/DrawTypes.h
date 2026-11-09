#pragma once

#include <cstdint>

namespace pf::render {
// The project's single drawing vocabulary, owned by the shared renderer.
// Neither the command stream nor any consumer of it depends on an ImGui
// context, header version or linked runtime - which is exactly why this
// set exists instead of the producer spelling its values ImVec2, ImU32,
// ImVec4 and ImDrawFlags (#995, ADR 0010).
//
// A packed colour keeps R in the low byte and A in the high byte, so it is
// the same layout MPP and ImGui both consume.
struct Vec2 {
  float x{}, y{};
  constexpr Vec2(float x = 0, float y = 0) : x(x), y(y) {}
};
struct Vec4 {
  float x{}, y{}, z{}, w{};
  constexpr Vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
};
using PackedColourValue = std::uint32_t;
// An installed texture, as an opaque handle the shared renderer never
// dereferences. A tileset asks one question of it - is artwork installed,
// or is this tileset empty and every lookup meant to fall back to magenta -
// and never more. It replaces the ImGui texture id the tilesets used to
// carry, which the renderer never looked at either; the atlas a command
// actually draws from is resolved by the MPP consumer through the resource
// manager, so no parallel texture metadata lives here (#1003, ADR 0010).
using TextureId = std::uint64_t;
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
} // namespace pf::render
