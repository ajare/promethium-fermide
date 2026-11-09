#pragma once

#include <vector>

#include "DrawTypes.h"

namespace pf::render
{

// The text side of the shared renderer's vocabulary (#999).
//
// The producer names the *role* a piece of text plays and the size it wants,
// never a font implementation, atlas or typeface. Each application answers
// for those roles through IFontMetrics over its own text system - the
// editor over Dear ImGui's fonts, the game over MPP's - so recording and
// measuring a World needs no font type, no implicit current-font query and
// no process-wide icon font.

// Which face of the application's text a command asks for.
enum class WorldFont
{
	Default,	// The application's body text face.
	Icons		// Its icon face: Marker glyphs and object icons.
};

// The font request a recorded Text command carries.
//
// `size` is the glyph size in pixels the producer has chosen for itself.
// Zero means no size was chosen: the implicit text entry point records
// that, the backend renders it with its own native text sizing, and it is
// never tessellated. A positive size is one the producer measured into the
// layout - a Marker icon scaled to MarkerIconSize, a debug badge centred
// in its box - and is the only text a stance transform can tessellate.
struct FontRef
{
	WorldFont font{ WorldFont::Default };
	float size{ 0.0f };
};

// One tessellated glyph corner: a position relative to the text origin,
// and its coordinate in the application's font atlas.
struct GlyphVertex
{
	Vec2 position;
	Vec2 texcoord;
};

// The glyph geometry of a text run as triangles textured from the font
// atlas - three GlyphVertex per triangle, in layout order.
using TessellatedGlyphs = std::vector<GlyphVertex>;

// The application's font atlas, as RGBA8 pixels.
//
// A glyph that has been through a stance transform is ordinary triangle
// geometry the backend draws itself, so the backend needs the atlas sitting
// behind the UVs the tessellation handed back. The renderer uploads it once
// and never interprets it; who built the atlas, and how, stays with the
// application that supplied these metrics.
struct FontAtlasImage
{
	std::uint8_t const* pixels{};
	int width{};
	int height{};

	bool empty() const { return pixels == nullptr || width <= 0 || height <= 0; }
};

// Everything the shared renderer can ask of an application's text system.
class IFontMetrics
{
public:
	virtual ~IFontMetrics() = default;

	// Measured extents in pixels. A ref whose size is 0.0f is measured the
	// way the face itself lays the run out, so the height it returns for
	// such a ref *is* the face's pixel size - which is what a producer
	// scales from when it wants a glyph at some fraction of the face.
	virtual Vec2 measure(FontRef const& ref, char const* text) const = 0;

	// Glyph geometry for a sized ref, positioned relative to the text
	// origin. A ref left at 0.0f has no tessellated form - it belongs to
	// the backend's native text path - so nothing comes back for it.
	virtual TessellatedGlyphs tessellate(FontRef const& ref,
		char const* text) const = 0;

	// The atlas the tessellated UVs address, for a backend that has to own
	// the upload. A text system whose transformed glyphs never reach such a
	// backend returns an empty image and says so rather than lying about
	// having pixels.
	virtual FontAtlasImage atlas() const = 0;
};

// The metrics the shared renderer measures with, installed by the
// application before it records a World - the same seam convention the
// Sector, Object, Agent and Furniture tilesets already use. Nothing in the
// renderer resolves a font for itself, so with nothing installed there is
// no measurement to fall back on. Installing returns the previous metrics
// so a caller can put them back.
IFontMetrics const* installFontMetrics(IFontMetrics const* metrics);

// The installed metrics. Throws std::runtime_error when no application has
// installed any: a text pass that needs a measurement and cannot get one is
// a setup error, and silently drawing at a size nobody chose would hide it.
IFontMetrics const& requireFontMetrics();

// Installs a metrics implementation for exactly one span and restores
// whatever was installed before, including when the span is left early.
// A check uses this to drive the producer with a face whose metrics it
// knows exactly.
class FontMetricsScope
{
public:
	explicit FontMetricsScope(IFontMetrics const* metrics);
	~FontMetricsScope();

	FontMetricsScope(FontMetricsScope const&) = delete;
	FontMetricsScope& operator=(FontMetricsScope const&) = delete;
	FontMetricsScope(FontMetricsScope&&) = delete;
	FontMetricsScope& operator=(FontMetricsScope&&) = delete;

private:
	IFontMetrics const* mPrevious;
};

} // namespace pf::render
