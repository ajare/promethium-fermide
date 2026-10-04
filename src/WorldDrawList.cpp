#include "WorldDrawList.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
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
		mClipStack.push_back({ testAdapter->GetClipRectMin(), testAdapter->GetClipRectMax() });
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

void WorldDrawList::PushClipRect(ImVec2 minimum, ImVec2 maximum,
	bool intersectWithCurrentClip)
{
	if (mTestAdapter) mTestAdapter->PushClipRect(minimum, maximum, intersectWithCurrentClip);
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

ImVec2 WorldDrawList::GetClipRectMin() const
{
	return currentClip().minimum;
}

ImVec2 WorldDrawList::GetClipRectMax() const
{
	return currentClip().maximum;
}

void WorldDrawList::AddLine(ImVec2 from, ImVec2 to, ImU32 colour, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddLine(from, to, colour, thickness);
		return;
	}
	mCommands.push_back(Line{ from, to, colour, thickness, currentClip() });
}

void WorldDrawList::AddRect(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
	float rounding, ImDrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRect(minimum, maximum, colour, rounding, flags, thickness);
		return;
	}
	(void)rounding;
	(void)flags;
	AddLine(minimum, { maximum.x, minimum.y }, colour, thickness);
	AddLine({ maximum.x, minimum.y }, maximum, colour, thickness);
	AddLine(maximum, { minimum.x, maximum.y }, colour, thickness);
	AddLine({ minimum.x, maximum.y }, minimum, colour, thickness);
}

void WorldDrawList::addTriangle(ImVec2 a, ImVec2 b, ImVec2 c,
	ImVec2 uvA, ImVec2 uvB, ImVec2 uvC, ImU32 colour, Texture texture)
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

void WorldDrawList::AddRectFilled(ImVec2 minimum, ImVec2 maximum, ImU32 colour,
	float rounding, ImDrawFlags flags)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddRectFilled(minimum, maximum, colour, rounding, flags);
		return;
	}
	(void)rounding;
	(void)flags;
	addTriangle(minimum, { maximum.x, minimum.y }, maximum, {}, {}, {}, colour, Texture::None);
	addTriangle(minimum, maximum, { minimum.x, maximum.y }, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddTriangleFilled(ImVec2 a, ImVec2 b, ImVec2 c, ImU32 colour)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddTriangleFilled(a, b, c, colour);
		return;
	}
	addTriangle(a, b, c, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddCircle(ImVec2 centre, float radius, ImU32 colour,
	int segments, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircle(centre, radius, colour, segments, thickness);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return ImVec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddLine(point(index), point(index + 1), colour, thickness);
	}
}

void WorldDrawList::AddCircleFilled(ImVec2 centre, float radius, ImU32 colour, int segments)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddCircleFilled(centre, radius, colour, segments);
		return;
	}
	auto const count = circleSegments(radius, segments);
	constexpr float tau = 6.28318530717958647692f;
	for (int index = 0; index < count; ++index)
	{
		auto point = [&](int i)
		{
			float const angle = tau * static_cast<float>(i % count) / static_cast<float>(count);
			return ImVec2{ centre.x + std::cos(angle) * radius,
				centre.y + std::sin(angle) * radius };
		};
		AddTriangleFilled(centre, point(index), point(index + 1), colour);
	}
}

void WorldDrawList::AddPolyline(ImVec2 const* points, int count, ImU32 colour,
	ImDrawFlags flags, float thickness)
{
	if (mTestAdapter)
	{
		mTestAdapter->AddPolyline(points, count, colour, flags, thickness);
		return;
	}
	if (!points || count < 2) return;
	for (int index = 1; index < count; ++index)
		AddLine(points[index - 1], points[index], colour, thickness);
	if ((flags & ImDrawFlags_Closed) != 0)
		AddLine(points[count - 1], points[0], colour, thickness);
}

void WorldDrawList::AddText(ImVec2 position, ImU32 colour, char const* text)
{
	if (!text) return;
	if (mTestAdapter)
	{
		mTestAdapter->AddText(position, colour, text);
		return;
	}
	mCommands.push_back(Text{ position, colour, text, currentClip() });
}

void WorldDrawList::AddText(ImFont* font, float fontSize, ImVec2 position,
	ImU32 colour, char const* text, char const* textEnd, float wrapWidth,
	ImVec4 const* fineClipRect)
{
	if (!text) return;
	if (mTestAdapter)
	{
		mTestAdapter->AddText(font, fontSize, position, colour, text, textEnd,
			wrapWidth, fineClipRect);
		return;
	}
	(void)wrapWidth;
	auto clip = currentClip();
	if (fineClipRect)
		clip = intersect(clip, { { fineClipRect->x, fineClipRect->y },
			{ fineClipRect->z, fineClipRect->w } });
	std::string value = textEnd ? std::string(text, textEnd) : std::string(text);
	mCommands.push_back(Text{ position, colour, std::move(value), clip, font, fontSize });
}

void WorldDrawList::AddImage(Texture texture, ImVec2 minimum, ImVec2 maximum,
	ImVec2 uvMinimum, ImVec2 uvMaximum, ImU32 colour)
{
	if (mTestAdapter)
	{
		// The headless compatibility adapter has no production texture. A stable
		// non-null ID preserves ImDrawList's image geometry for legacy checks.
		auto id = reinterpret_cast<ImTextureID>(static_cast<intptr_t>(texture));
		mTestAdapter->AddImage(id, minimum, maximum, uvMinimum, uvMaximum, colour);
		return;
	}
	ImVec2 const topRight{ maximum.x, minimum.y };
	ImVec2 const bottomLeft{ minimum.x, maximum.y };
	ImVec2 const uvTopRight{ uvMaximum.x, uvMinimum.y };
	ImVec2 const uvBottomLeft{ uvMinimum.x, uvMaximum.y };
	addTriangle(minimum, topRight, maximum, uvMinimum, uvTopRight, uvMaximum,
		colour, texture);
	addTriangle(minimum, maximum, bottomLeft, uvMinimum, uvMaximum, uvBottomLeft,
		colour, texture);
}

size_t WorldDrawList::geometryBookmark() const
{
	return mTestAdapter ? static_cast<size_t>(mTestAdapter->VtxBuffer.Size) : mCommands.size();
}

void WorldDrawList::transformGeometrySince(size_t bookmark, ImVec2 pivot,
	ImVec2 scale, bool clockwiseQuarterTurn)
{
	auto transform = [&](ImVec2 p)
	{
		float x = (p.x - pivot.x) * scale.x;
		float y = (p.y - pivot.y) * scale.y;
		return clockwiseQuarterTurn ? ImVec2{pivot.x - y, pivot.y + x}
			: ImVec2{pivot.x + x, pivot.y + y};
	};
	if (mTestAdapter)
	{
		for (size_t i = bookmark; i < static_cast<size_t>(mTestAdapter->VtxBuffer.Size); ++i)
			mTestAdapter->VtxBuffer[static_cast<int>(i)].pos = transform(mTestAdapter->VtxBuffer[static_cast<int>(i)].pos);
		return;
	}
	std::vector<Command> transformed;
	for (size_t i = bookmark; i < mCommands.size(); ++i)
	{
		auto command = mCommands[i];
		if (auto* text = std::get_if<Text>(&command); text && text->font)
		{
			// Tessellate before applying the stance, so glyph UVs and width
			// remain intact. Clipping belongs to the transformed output, not
			// the upright source glyph (a Lying body can overflow its cell).
			ImDrawList glyphs(ImGui::GetDrawListSharedData());
			glyphs._ResetForNewFrame();
			glyphs.PushTextureID(text->font->ContainerAtlas->TexID);
			glyphs.PushClipRectFullScreen();
			glyphs.AddText(text->font, text->fontSize, text->position, text->colour, text->value.c_str());
			for (int index = 0; index + 2 < glyphs.IdxBuffer.Size; index += 3)
			{
				Triangle triangle{};
				triangle.colour = text->colour;
				triangle.texture = Texture::FontAtlas;
				triangle.clip = text->clip;
				for (int v = 0; v < 3; ++v)
				{
					auto const& vertex = glyphs.VtxBuffer[glyphs.IdxBuffer[index + v]];
					triangle.positions[v] = transform(vertex.pos);
					triangle.texcoords[v] = vertex.uv;
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
