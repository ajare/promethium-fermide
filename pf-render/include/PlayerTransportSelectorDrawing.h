#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

// Shared, fixed-screen-size selector drawing; neither renderer needs a font atlas.
// Rect callbacks take pixel coordinates and packed RGBA (R in the low byte).
namespace pf
{
 template<class Selector, class Fill, class Outline>
 void drawPlayerTransportSelector(Selector const& selector, float x, float y, Fill fill, Outline outline)
 {
  constexpr uint32_t white = 0xffffffffu, green = 0xff60e060u;
  constexpr uint32_t amber = 0xff40bfffu, cyan = 0xffffff00u, background = 0xe0282020u;
  static constexpr std::string_view glyphs[] = {
   "111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
   "111100111001111", "111100111101111", "111001010010010", "111101111101111", "111101111001111",
   "010101111101101", "110101110101110", "111100100100111", "110101101101110", "111100110100111",
   "111100110100100", "111100101101111", "101101111101101", "111010010010111", "001001001101111",
   "101101110101101", "100100100100111", "101111111101101", "101111111111101", "111101101101111",
   "110101110100100", "111101101111001", "110101110101101", "111100111001111", "111010010010010",
   "101101101101111", "101101101101010", "101101111111101", "101101010101101", "101101010010010", "111001010100111"
  };
  auto label = [&](auto const& stop, int slot) {
   float left = x + (selector.horizontal ? slot * 38.f : 0.f);
   float top = y - (selector.horizontal ? 0.f : slot * 22.f);
   auto colour = stop.current ? green : stop.pending ? amber : white;
   fill(left, top, left + 34.f, top + 18.f, background);
   if (stop.index == selector.focus) outline(left, top, left + 34.f, top + 18.f, cyan);
   float textX = left + (34.f - stop.name.size() * 8.f + 2.f) * .5f;
   for (char c : stop.name) {
    int index = c >= '0' && c <= '9' ? c - '0' : c - 'A' + 10;
    if (index >= 0 && index < 36) for (int row = 0; row < 5; ++row)
     for (int col = 0; col < 3; ++col) if (glyphs[index][row * 3 + col] == '1')
      fill(textX + col * 2.f, top + 4.f + row * 2.f, textX + col * 2.f + 2.f, top + 6.f + row * 2.f, colour);
    textX += 8.f;
   }
  };
  int count = (int)selector.stops.size(), focus = 0;
  for (int i = 0; i < count; ++i) if (selector.stops[i].index == selector.focus) focus = i;
  int first = std::clamp(focus - 3, 0, std::max(0, count - 7));
  int end = std::min(count, first + 7);
  for (int i = first; i < end; ++i) label(selector.stops[i], i - first);
  // Overflow dots occupy their own slots. The current Stop remains visible even
  // when the navigation window is focused far away from it.
  for (auto const& stop : selector.stops) if (stop.current) {
   auto it = std::find_if(selector.stops.begin() + first, selector.stops.begin() + end,
    [&](auto const& visible) { return visible.index == stop.index; });
   if (it == selector.stops.begin() + end) label(stop, stop.index < selector.stops[first].index ? -2 : 8);
  }
  auto overflow = [&](int slot) {
   float left = x + (selector.horizontal ? slot * 38.f : 0.f);
   float top = y - (selector.horizontal ? 0.f : slot * 22.f);
   for (int i = 0; i < 3; ++i) fill(left + 9.f + i * 6.f, top + 8.f, left + 11.f + i * 6.f, top + 10.f, white);
  };
  if (first > 0) overflow(-1);
  if (end < count) overflow(7);
 }
}
