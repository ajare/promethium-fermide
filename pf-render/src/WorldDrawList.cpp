#include "WorldDrawList.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "imgui/imgui.h"

namespace
{
	// Conversions exist only where the command stream hands its recording to the
	// optional ImGui draw-list test adapter, or reads a clip back out of it.
	// The recorded commands themselves never hold an ImGui value (#997).
	ImVec2 toImGui(Vec2 value)
	{
		return ImVec2{ value.x, value.y };
	}

	Vec2 fromImGui(ImVec2 value)
	{
		return Vec2{ value.x, value.y };
	}

	// Glyph geometry comes back from the metrics relative to the text origin,
	// so the run's own position is added where it is replayed or transformed.
	Vec2 offset(Vec2 origin, Vec2 relative)
	{
		return Vec2{ origin.x + relative.x, origin.y + relative.y };
	}

	WorldDrawList::ClipRectangle intersect(
		WorldDrawList::ClipRectangle const& left,
		WorldDrawList::ClipRectangle const& right)
	{
		return {
			{ std::max(left.minimum.x, right.minimum.x),
				std::max(left.minimum.y, right.minimum.y) },
			{ std::min(left.maximum.x, right.maximum.x),
				std::min(left.maximum.y, right.maximum.y) }
		};
	}

	int circleSegments(float radius, int requested)
	{
		if (requested > 2) return requested;
		return std::clamp(static_cast<int>(std::ceil(radius * 0.75f)), 12, 64);
	}
}

WorldDrawList::WorldDrawList(ClipRectangle initialClip)
{
	mClipStack.push_back(initialClip);
}

WorldDrawList::WorldDrawList(ImDrawList* testAdapter)
	: mTestAdapter(testAdapter)
{
	if (testAdapter)
	{
		mClipStack.push_back({ fromImGui(testAdapter->GetClipRectMin()),
			fromImGui(testAdapter->GetClipRectMax()) });
	}
	else
	{
		float const limit = std::numeric_limits<float>::max();
		mClipStack.push_back({ { -limit, -limit }, { limit, limit } });
	}
}

WorldDrawList::ClipRectangle WorldDrawList::currentClip() const
{
	return mClipStack.back();
}

void WorldDrawList::PushClipRect(Vec2 minimum, Vec2 maximum,
	bool intersectWithCurrentClip)
{
	if (mTestAdapter) mTestAdapter->PushClipRect(toImGui(minimum), toImGui(maximum), intersectWithCurrentClip);
	ClipRectangle next{ minimum, maximum };
	if (intersectWithCurrentClip) next = intersect(currentClip(), next);
	mClipStack.push_back(next);
}

void WorldDrawList::PopClipRect()
{
	if (mClipStack.size() <= 1) return;
	if (mTestAdapter) mTestAdapter->PopClipRect();
	mClipStack.pop_back();
}

Vec2 WorldDrawList::GetClipRectMin() const
{
	return currentClip().minimum;
}

Vec2 WorldDrawList::GetClipRectMax() const
{
	return currentClip().maximum;
}

void WorldDrawList::AddLine(Vec2 from, Vec2 to, PackedColourValue colour, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddLine(toImGui(from), toImGui(to), colour, thickness);
		return;
	}
	mCommands.push_back(Line{ from, to, colour, thickness, currentClip() });
}

void WorldDrawList::AddRect(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
	float rounding, DrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRect(toImGui(minimum), toImGui(maximum), colour, rounding, flags, thickness);
		return;
	}
	(void)rounding;
	(void)flags;
	AddLine(minimum, { maximum.x, minimum.y }, colour, thickness);
	AddLine({ maximum.x, minimum.y }, maximum, colour, thickness);
	AddLine(maximum, { minimum.x, maximum.y }, colour, thickness);
	AddLine({ minimum.x, maximum.y }, minimum, colour, thickness);
}

void WorldDrawList::addTriangle(Vec2 a, Vec2 b, Vec2 c,
	Vec2 uvA, Vec2 uvB, Vec2 uvC, PackedColourValue colour, Texture texture)
{
	Triangle triangle;
	triangle.positions[0] = a;
	triangle.positions[1] = b;
	triangle.positions[2] = c;
	triangle.texcoords[0] = uvA;
	triangle.texcoords[1] = uvB;
	triangle.texcoords[2] = uvC;
	triangle.colour = colour;
	triangle.texture = texture;
	triangle.clip = currentClip();
	mCommands.push_back(triangle);
}

void WorldDrawList::AddRectFilled(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
	float rounding, DrawFlags flags)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRectFilled(toImGui(minimum), toImGui(maximum), colour, rounding, flags);
		return;
	}
	(void)rounding;
	(void)flags;
	addTriangle(minimum, { maximum.x, minimum.y }, maximum, {}, {}, {}, colour, Texture::None);
	addTriangle(minimum, maximum, { minimum.x, maximum.y }, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColourValue colour)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddTriangleFilled(toImGui(a), toImGui(b), toImGui(c), colour);
		return;
	}
	addTriangle(a, b, c, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddCircle(Vec2 centre, float radius, PackedColourValue colour,
	int segments, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircle(toImGui(centre), radius, colour, segments, thickness);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return Vec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddLine(point(index), point(index + 1), colour, thickness);
	}
}

void WorldDrawList::AddCircleFilled(Vec2 centre, float radius, PackedColourValue colour, int segments)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircleFilled(toImGui(centre), radius, colour, segments);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return Vec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddTriangleFilled(centre, point(index), point(index + 1), colour);
	}
}

void WorldDrawList::AddPolyline(Vec2 const* points, int count, PackedColourValue colour,
	DrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		std::vector<ImVec2> adapted;
		if (points && count > 0)
		{
			adapted.reserve(static_cast<size_t>(count));
			for (int index = 0; index < count; ++index)
				adapted.push_back(toImGui(points[index]));
		}
		mTestAdapter->AddPolyline(adapted.empty() ? nullptr : adapted.data(),
			static_cast<int>(adapted.size()), colour, flags, thickness);
		return;
	}
	if (!points || count < 2) return;
	for (int index = 1; index < count; ++index)
		AddLine(points[index - 1], points[index], colour, thickness);
	if ((flags & DrawFlagsClosed) != 0)
		AddLine(points[count - 1], points[0], colour, thickness);
}

void WorldDrawList::AddText(Vec2 position, PackedColourValue colour, char const* text)
{
	if (!text) return;
	if (mTestAdapter)
	{
		mTestAdapter->AddText(toImGui(position), colour, text);
		return;
	}
	// The implicit entry point asks for the application's body face at its own
	// size: the backend lays it out, and nothing here measures or tessellates
	// it (#999).
	mCommands.push_back(Text{ position, colour, text, currentClip() });
}

void WorldDrawList::AddText(FontRef font, Vec2 position, PackedColourValue colour,
	char const* text, char const* textEnd, float wrapWidth,
	Vec4 const* fineClipRect)
{
	if (!text) return;
	// Wrapping is a layout the backend does; the recorded command carries the
	// run as given, as it always has.
	(void)wrapWidth;
	auto value = textEnd ? std::string(text, textEnd) : std::string(text);
	if (mTestAdapter)
	{
		// The adapter has no font of its own either. Its glyphs arrive through
		// the injected metrics like everywhere else, as atlas triangles
		// written into the list against the texture it already has pushed.
		if (font.size <= 0.0f) return;
		if (fineClipRect)
			mTestAdapter->PushClipRect(toImGui(Vec2{ fineClipRect->x, fineClipRect->y }),
				toImGui(Vec2{ fineClipRect->z, fineClipRect->w }), true);
		auto const glyphs = fontMetrics().tessellate(font, value.c_str());
		if (!glyphs.empty())
		{
			auto const count = static_cast<int>(glyphs.size());
			mTestAdapter->PrimReserve(count, count);
			for (auto const& vertex : glyphs)
				mTestAdapter->PrimVtx(toImGui(offset(position, vertex.position)),
					toImGui(vertex.texcoord), colour);
		}
		if (fineClipRect) mTestAdapter->PopClipRect();
		return;
	}
	auto clip = currentClip();
	if (fineClipRect)
		clip = intersect(clip, { { fineClipRect->x, fineClipRect->y },
			{ fineClipRect->z, fineClipRect->w } });
	mCommands.push_back(Text{ position, colour, std::move(value), clip, font });
}

void WorldDrawList::AddImage(Texture texture, Vec2 minimum, Vec2 maximum,
	Vec2 uvMinimum, Vec2 uvMaximum, PackedColourValue colour)
{
	if (mTestAdapter)
	{
		// The headless compatibility adapter has no production texture. A stable
		// non-null ID preserves ImDrawList's image geometry for legacy checks.
		auto id = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture));
		mTestAdapter->AddImage(id, toImGui(minimum), toImGui(maximum),
			toImGui(uvMinimum), toImGui(uvMaximum), colour);
		return;
	}
	Vec2 const topRight{ maximum.x, minimum.y };
	Vec2 const bottomLeft{ minimum.x, maximum.y };
	Vec2 const uvTopRight{ uvMaximum.x, uvMinimum.y };
	Vec2 const uvBottomLeft{ uvMinimum.x, uvMaximum.y };
	addTriangle(minimum, topRight, maximum, uvMinimum, uvTopRight, uvMaximum,
		colour, texture);
	addTriangle(minimum, maximum, bottomLeft, uvMinimum, uvMaximum, uvBottomLeft,
		colour, texture);
}

size_t WorldDrawList::geometryBookmark() const
{
	return mTestAdapter ? static_cast<size_t>(mTestAdapter->VtxBuffer.Size) : mCommands.size();
}

void WorldDrawList::transformGeometrySince(size_t bookmark, Vec2 pivot,
	Vec2 scale, bool clockwiseQuarterTurn)
{
	auto transform = [&](Vec2 p)
	{
		float x = (p.x - pivot.x) * scale.x;
		float y = (p.y - pivot.y) * scale.y;
		return clockwiseQuarterTurn ? Vec2{pivot.x - y, pivot.y + x}
			: Vec2{pivot.x + x, pivot.y + y};
	};
	if (mTestAdapter)
	{
		for (size_t i = bookmark; i < static_cast<size_t>(mTestAdapter->VtxBuffer.Size); ++i)
		{
			auto& vertex = mTestAdapter->VtxBuffer[static_cast<int>(i)];
			auto const moved = transform(fromImGui(vertex.pos));
			vertex.pos = toImGui(moved);
		}
		return;
	}
	std::vector<Command> transformed;
	for (size_t i = bookmark; i < mCommands.size(); ++i)
	{
		auto command = mCommands[i];
		if (auto* text = std::get_if<Text>(&command); text && text->font.size > 0.0f)
		{
			// Tessellate before applying the stance, so glyph UVs and width
			// remain intact. Clipping belongs to the transformed output, not
			// the upright source glyph (a Lying body can overflow its cell).
			// The geometry comes from the injected metrics: the command stream
			// never touches a font to get it (#999).
			auto const glyphs = fontMetrics().tessellate(text->font, text->value.c_str());
			for (size_t index = 0; index + 2 < glyphs.size(); index += 3)
			{
				Triangle triangle{};
				triangle.colour = text->colour;
				triangle.texture = Texture::FontAtlas;
				triangle.clip = text->clip;
				for (int v = 0; v < 3; ++v)
				{
					auto const& vertex = glyphs[index + static_cast<size_t>(v)];
					triangle.positions[v] = transform(offset(text->position, vertex.position));
					triangle.texcoords[v] = vertex.texcoord;
				}
				transformed.push_back(triangle);
			}
			continue;
		}
		if (auto* triangle = std::get_if<Triangle>(&command))
			for (auto& p : triangle->positions) p = transform(p);
		else if (auto* line = std::get_if<Line>(&command))
		{
			line->from = transform(line->from);
			line->to = transform(line->to);
		}
		transformed.push_back(std::move(command));
	}
	mCommands.resize(bookmark);
	mCommands.insert(mCommands.end(), transformed.begin(), transformed.end());
}

void WorldDrawList::AddDrawCmd()
{
	if (mTestAdapter)
	{
		mTestAdapter->AddDrawCmd();
		return;
	}
	mCommands.push_back(Barrier{});
}
