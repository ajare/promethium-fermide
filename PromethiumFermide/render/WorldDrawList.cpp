#include "WorldDrawList.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pf::application::render {

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



WorldDrawList::ClipRectangle WorldDrawList::currentClip() const
{
	return mClipStack.back();
}

void WorldDrawList::PushClipRect(Vec2 minimum, Vec2 maximum,
	bool intersectWithCurrentClip)
{

	ClipRectangle next{ minimum, maximum };
	if (intersectWithCurrentClip) next = intersect(currentClip(), next);
	mClipStack.push_back(next);
}

void WorldDrawList::PopClipRect()
{
	if (mClipStack.size() <= 1) return;

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

	mCommands.push_back(Line{ from, to, colour, thickness, currentClip() });
}

void WorldDrawList::AddRect(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
	float rounding, DrawFlags flags, float thickness)
{

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

	(void)rounding;
	(void)flags;
	addTriangle(minimum, { maximum.x, minimum.y }, maximum, {}, {}, {}, colour, Texture::None);
	addTriangle(minimum, maximum, { minimum.x, maximum.y }, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColourValue colour)
{

	addTriangle(a, b, c, {}, {}, {}, colour, Texture::None);
}

void WorldDrawList::AddCircle(Vec2 centre, float radius, PackedColourValue colour,
	int segments, float thickness)
{

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

	if (!points || count < 2) return;
	for (int index = 1; index < count; ++index)
		AddLine(points[index - 1], points[index], colour, thickness);
	if ((flags & DrawFlagsClosed) != 0)
		AddLine(points[count - 1], points[0], colour, thickness);
}





void WorldDrawList::AddImage(Texture texture, Vec2 minimum, Vec2 maximum,
	Vec2 uvMinimum, Vec2 uvMaximum, PackedColourValue colour)
{

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
	return mCommands.size();
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

	std::vector<Command> transformed;
	for (size_t i = bookmark; i < mCommands.size(); ++i)
	{
		auto command = mCommands[i];

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

	mCommands.push_back(Barrier{});
}

} // namespace pf::application::render
