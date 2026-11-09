#pragma once

#include <cfloat>

#include "FontMetrics.h"
#include "imgui/imgui.h"

using pf::render::FontRef;
using pf::render::TessellatedGlyphs;
using pf::render::Vec2;
using pf::render::WorldFont;

//
// The editor's IFontMetrics over Dear ImGui's font atlas (#999).
//
// The shared renderer asks for a face by role and never sees a font object;
// this is where the role becomes an ImFont. The faces are supplied as
// resolvers rather than as ImFont* snapshots: the editor loads its icon
// face during start-up and the headless checks swap it, so which font
// answers for a role has to be decided at the moment of measurement, not
// at the moment the metrics were installed.
//
class ImGuiFontMetrics final : public pf::render::IFontMetrics
{
public:
	using Face = ImFont* (*)();

	ImGuiFontMetrics(Face body, Face icons) : mBody(body), mIcons(icons) {}

	// Measured with ImGui's own layout, so a ref left at the default size
	// answers at the face's own pixel size - exactly what the producer read
	// off `font->FontSize` and CalcTextSizeA before the font was injected,
	// which is why centring and measured extents are unchanged.
	Vec2 measure(FontRef const& ref, char const* text) const override
	{
		auto* font = face(ref.font);
		if (!font || !text) return {};
		auto const bounds = font->CalcTextSizeA(sizeFor(ref, font), FLT_MAX, 0.0f, text);
		return Vec2{ bounds.x, bounds.y };
	}

	// Glyph geometry is ImGui's own tessellation read back as atlas
	// triangles relative to the text origin, which is what a stance
	// transform and the draw-list test adapter both replay.
	TessellatedGlyphs tessellate(FontRef const& ref, char const* text) const override
	{
		TessellatedGlyphs glyphs;
		// A run left at the default size belongs to the backend's native
		// text path: there is nothing tessellated for it.
		if (ref.size <= 0.0f || !text || !*text) return glyphs;
		auto* font = face(ref.font);
		if (!font) return glyphs;

		ImDrawList tessellated(ImGui::GetDrawListSharedData());
		tessellated._ResetForNewFrame();
		tessellated.PushTextureID(font->ContainerAtlas->TexID);
		tessellated.PushClipRectFullScreen();
		tessellated.AddText(font, ref.size, ImVec2{ 0.0f, 0.0f }, IM_COL32_WHITE, text);
		for (int index = 0; index + 2 < tessellated.IdxBuffer.Size; index += 3)
			for (int v = 0; v < 3; ++v)
			{
				auto const& vertex = tessellated.VtxBuffer[tessellated.IdxBuffer[index + v]];
				glyphs.push_back(pf::render::GlyphVertex{
					Vec2{ vertex.pos.x, vertex.pos.y },
					Vec2{ vertex.uv.x, vertex.uv.y } });
			}
		return glyphs;
	}

private:
	ImFont* face(WorldFont font) const
	{
		auto const resolver = font == WorldFont::Icons ? mIcons : mBody;
		return resolver ? resolver() : nullptr;
	}

	float sizeFor(FontRef const& ref, ImFont* font) const
	{
		return ref.size > 0.0f ? ref.size : font->FontSize;
	}

	Face mBody;
	Face mIcons;
};
