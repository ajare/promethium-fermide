#pragma once

#include "DrawTypes.h"

namespace pf::application::render {
// The application's drawing types are the shared renderer's (#996). They
// were written here for exactly the purpose pf-render now serves - neither
// the command stream nor its GPU renderer depends on an ImGui context,
// header version or linked runtime - so the definitions moved there and
// this header only re-exports them under the namespace the application's
// renderer already spells. Aliases, not a second definition: a type here
// and the shared one are the same type, interchangeable in either module.
using pf::render::Vec2;
using pf::render::Vec4;
using pf::render::PackedColourValue;
using pf::render::DrawFlags;
using pf::render::DrawFlagsNone;
using pf::render::DrawFlagsClosed;
using pf::render::RedShift;
using pf::render::GreenShift;
using pf::render::BlueShift;
using pf::render::AlphaShift;
using pf::render::packColour;
using pf::render::White;
} // namespace pf::application::render
