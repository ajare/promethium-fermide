#pragma once

#include <cstdint>
#include <vector>

#include <mpp/InternalFont.h>

#include "FontMetrics.h"

namespace pf::application::render {

using pf::render::FontRef;

//
// The game's `IFontMetrics` over MPP's built-in text system (#1002).
//
// The shared renderer asks for a face by role and never sees a font object;
// this is where that role becomes the game's text system. The editor answers
// the same questions over Dear ImGui's fonts - the game has no ImGui, so it
// answers them over MPP's internal bitmap font, which is the only text the
// shipped game has: a 16x16 grid of fixed 16x16 cells, one per byte, each
// advanced by its own kern on a 16 pixel line.
//
// The producer cannot tell which of the two it is talking to. That is the
// point of the seam (#999): the game gained World text by supplying these
// three answers, not by growing a text renderer of its own.
//
class MppFontMetrics final : public pf::render::IFontMetrics
{
public:
	MppFontMetrics();

	// A run lays out one cell per glyph, less that glyph's kern, on the
	// font's fixed line height. A ref left at size 0 measures at that
	// native size - which is why the height returned for it *is* the face's
	// pixel size, exactly what the producer scales a Marker icon from. A
	// sized ref scales the whole run by size / native line height.
	pf::render::Vec2 measure(FontRef const& ref, char const* text) const override;

	// The glyph quads MPP would draw, as atlas triangles positioned
	// relative to the text origin. Only a sized ref has a tessellated form:
	// an unsized run belongs to MPP's own native text path and is never
	// tessellated, so nothing comes back for it.
	pf::render::TessellatedGlyphs tessellate(FontRef const& ref, char const* text) const override;

	// The internal font atlas as RGBA8 rows in the bottom-to-top order the
	// GL texture path expects - the same layout Willpower's ImageResource
	// hands over for every other atlas in the game, so the command stream's
	// single top-down UV convention holds across all of them rather than
	// needing a per-atlas exception.
	pf::render::FontAtlasImage atlas() const override;

	// MPP's per-byte advance: a full cell plus the glyph's own kern. The
	// narrow glyphs are the ones MPP's own renderer carves out; matching it
	// is what keeps the game's measured run the width MPP actually draws.
	static float advanceFor(char glyph);

	// The font's fixed line height in pixels, as MPP's text renderer lays
	// lines out.
	static constexpr float NativeLineHeight{ 16.0f };

private:
	float scaleFor(FontRef const& ref) const
	{
		return ref.size > 0.0f ? ref.size / NativeLineHeight : 1.0f;
	}

	mpp::InternalFont mFont;
	// The atlas, flipped into the GL row order, owned here so the pixels
	// outlive every upload the renderer makes from them.
	std::vector<std::uint8_t> mPixels;
};

} // namespace pf::application::render
