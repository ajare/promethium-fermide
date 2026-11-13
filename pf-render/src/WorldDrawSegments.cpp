// The CPU half of the frame plan: where a recorded stream breaks into
// batches, and which retained batch each piece lands in. See
// include/WorldDrawSegments.h for what a segment is and why the Sector
// sheet is part of its identity.
//
// This code used to sit inside `WorldRenderStack`, where it was reachable
// only from a live OpenGL context. Lifting it out is what lets a CPU-only
// check drive the same split the application runs, rather than proving a
// hand-written copy of the rule while the real one went unasserted - the
// failure shape #1030 named for the install path and for the four defects
// that hid behind it.

#include "WorldDrawSegments.h"

#include <algorithm>
#include <utility>

namespace pf::render
{
	namespace
	{
		bool same(Vec2 left, Vec2 right)
		{
			return left.x == right.x && left.y == right.y;
		}

		bool same(WorldDrawList::ClipRectangle const& left,
			WorldDrawList::ClipRectangle const& right)
		{
			return same(left.minimum, right.minimum) && same(left.maximum,
				right.maximum);
		}
	}

	std::vector<DrawSegment> buildDrawSegments(WorldDrawList const& commands)
	{
		std::vector<DrawSegment> result;
		for (auto const& command : commands.commands())
		{
			if (std::holds_alternative<WorldDrawList::Barrier>(command))
			{
				if (!result.empty()) result.push_back({});
				continue;
			}
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(
				&command))
			{
				// The sheet is part of what a batch binds, so it is part of
				// what ends a run. Two adjacent Sector triangles off two
				// different sheets are two segments here, and the two
				// batches they get are the two textures that get bound
				// (#1089); merged into one segment they would have been one
				// batch drawing both with whichever texture it happened to
				// have been created against.
				bool const append = !result.empty()
					&& result.back().kind == DrawSegment::Kind::Triangles
					&& result.back().texture == triangle->texture
					&& result.back().sectorSheet == triangle->sectorSheet
					&& same(result.back().clip, triangle->clip);
				if (!append)
				{
					DrawSegment segment;
					segment.kind = DrawSegment::Kind::Triangles;
					segment.texture = triangle->texture;
					segment.sectorSheet = triangle->sectorSheet;
					segment.clip = triangle->clip;
					result.push_back(std::move(segment));
				}
				result.back().triangles.push_back(*triangle);
			}
			else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
			{
				bool const append = !result.empty()
					&& result.back().kind == DrawSegment::Kind::Lines
					&& result.back().lineWidth == line->thickness
					&& same(result.back().clip, line->clip);
				if (!append)
				{
					DrawSegment segment;
					segment.kind = DrawSegment::Kind::Lines;
					segment.lineWidth = line->thickness;
					segment.clip = line->clip;
					result.push_back(std::move(segment));
				}
				result.back().lines.push_back(*line);
			}
			else if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
			{
				bool const append = !result.empty()
					&& result.back().kind == DrawSegment::Kind::Text
					&& same(result.back().clip, text->clip);
				if (!append)
				{
					DrawSegment segment;
					segment.kind = DrawSegment::Kind::Text;
					segment.clip = text->clip;
					result.push_back(std::move(segment));
				}
				result.back().texts.push_back(*text);
			}
		}
		std::erase_if(result, [](DrawSegment const& segment)
			{ return segment.triangles.empty() && segment.lines.empty()
				&& segment.texts.empty(); });
		return result;
	}

	WorldRenderSlotAllocator::Kind slotKindFor(DrawSegment const& segment)
	{
		using Kind = WorldRenderSlotAllocator::Kind;
		if (segment.kind == DrawSegment::Kind::Lines) return Kind::Lines;
		switch (segment.texture)
		{
		case WorldDrawList::Texture::SectorAtlas: return Kind::SectorTriangles;
		case WorldDrawList::Texture::ObjectAtlas: return Kind::ObjectTriangles;
		case WorldDrawList::Texture::AgentAtlas: return Kind::AgentTriangles;
		case WorldDrawList::Texture::FurnitureAtlas: return Kind::FurnitureTriangles;
		case WorldDrawList::Texture::FontAtlas: return Kind::FontTriangles;
		case WorldDrawList::Texture::None: break;
		}
		return Kind::SolidTriangles;
	}

	std::vector<std::size_t> planDrawSlots(
		std::vector<DrawSegment> const& segments,
		WorldRenderSlotAllocator& allocator)
	{
		allocator.beginFrame();
		std::vector<std::size_t> plan(segments.size());
		for (std::size_t index = 0; index < segments.size(); ++index)
		{
			auto const& segment = segments[index];
			if (segment.kind == DrawSegment::Kind::Text) continue;
			plan[index] = allocator.acquire(slotKindFor(segment),
				segment.sectorSheet);
		}
		return plan;
	}
}
