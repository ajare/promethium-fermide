#include "MppFontMetrics.h"

#include <algorithm>
#include <cmath>

namespace pf::application::render {

using pf::render::Vec2;

namespace {
	// MPP's internal font is a 16x16 grid of fixed cells, one per byte of
	// the run. The cell size comes from the atlas rather than being assumed,
	// so the metrics follow the atlas if it is ever re-authored.
	constexpr int GridCells{ 16 };

	struct Cell
	{
		int column{};
		int row{};
	};

	Cell cellOf(unsigned char glyph)
	{
		// ASCII order runs bottom-to-top through the atlas: the control range
		// sits at the bottom, the high bytes at the top. MPP's own glyph
		// table is built the same way, which is why a space really is blank.
		return { glyph % GridCells, (GridCells - 1) - (glyph / GridCells) };
	}
}

MppFontMetrics::MppFontMetrics()
{
	auto const width = mFont.getWidth();
	auto const height = mFont.getHeight();
	if (width <= 0 || height <= 0) return;
	// Flip the decoded top-to-bottom rows into the bottom-to-top layout the
	// GL texture path expects, exactly as Willpower's ImageResource does for
	// every other atlas the game draws from. The renderer copies these once
	// into GPU memory and never reads them again.
	mPixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
	auto const* source = mFont.getData();
	auto const stride = static_cast<std::size_t>(width) * 4;
	if (!source) { mPixels.clear(); return; }
	for (int row = 0; row < height; ++row)
		std::copy(source + static_cast<std::size_t>(height - row - 1) * stride,
			source + static_cast<std::size_t>(height - row) * stride,
			mPixels.begin() + static_cast<std::size_t>(row) * stride);
}

float MppFontMetrics::advanceFor(char glyph)
{
	// MPP's text renderer advances a full cell and then applies the glyph's
	// own kern. The narrow faces are the ones it carves out; everything else
	// takes the default. Matching it is what makes a measured run the width
	// that actually gets drawn.
	switch (glyph)
	{
	case 'f': return 7.0f;
	case 'i': case 'j': case 'r': return 6.0f;
	default: return 8.0f;
	}
}

pf::render::Vec2 MppFontMetrics::measure(FontRef const& ref, char const* text) const
{
	if (!text) return {};
	auto const scale = scaleFor(ref);
	float width{};
	for (auto const* p = text; *p; ++p) width += advanceFor(*p);
	return pf::render::Vec2{ width * scale, NativeLineHeight * scale };
}

pf::render::TessellatedGlyphs MppFontMetrics::tessellate(FontRef const& ref,
	char const* text) const
{
	pf::render::TessellatedGlyphs glyphs;
	// An unsized run stays on MPP's native text path: there is nothing
	// tessellated for it.
	if (ref.size <= 0.0f || !text || !*text) return glyphs;
	auto const width = mFont.getWidth();
	auto const height = mFont.getHeight();
	if (width <= 0 || height <= 0) return glyphs;

	auto const scale = scaleFor(ref);
	auto const cellWidth = static_cast<float>(width) / GridCells;
	auto const cellHeight = static_cast<float>(height) / GridCells;
	float x{};
	for (auto const* p = text; *p; ++p)
	{
		auto const cell = cellOf(static_cast<unsigned char>(*p));
		// The whole cell is the glyph quad, as MPP draws it; the visible mark
		// sits inside it with its own transparent margin.
		float const u0 = static_cast<float>(cell.column * GridCells) / width;
		float const u1 = (static_cast<float>(cell.column * GridCells) + cellWidth) / width;
		float const v0 = static_cast<float>(cell.row * GridCells) / height;
		float const v1 = (static_cast<float>(cell.row * GridCells) + cellHeight) / height;
		Vec2 const tl{ x, 0.0f };
		Vec2 const tr{ x + cellWidth * scale, 0.0f };
		Vec2 const bl{ x, cellHeight * scale };
		Vec2 const br{ x + cellWidth * scale, cellHeight * scale };
		auto vertex = [](Vec2 position, Vec2 uv)
		{
			return pf::render::GlyphVertex{ position, uv };
		};
		glyphs.push_back(vertex(tl, { u0, v0 }));
		glyphs.push_back(vertex(tr, { u1, v0 }));
		glyphs.push_back(vertex(br, { u1, v1 }));
		glyphs.push_back(vertex(tl, { u0, v0 }));
		glyphs.push_back(vertex(br, { u1, v1 }));
		glyphs.push_back(vertex(bl, { u0, v1 }));
		// The advance, not the cell, is what the next glyph starts at.
		x += advanceFor(*p) * scale;
	}
	return glyphs;
}

pf::render::FontAtlasImage MppFontMetrics::atlas() const
{
	pf::render::FontAtlasImage image;
	if (mPixels.empty()) return image;
	image.pixels = mPixels.data();
	image.width = mFont.getWidth();
	image.height = mFont.getHeight();
	return image;
}

} // namespace pf::application::render
