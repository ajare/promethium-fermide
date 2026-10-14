#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "DrawTypes.h"

namespace pf::application::render {

// Application-owned CPU command stream. Only native geometry and atlas
// sprites: no UI adapter, fonts, text commands, or diagnostic overlays.
class WorldDrawList
{
public:
	enum class Texture
	{
		None,
		SectorAtlas,
		ObjectAtlas,
		AgentAtlas
	};

	struct ClipRectangle
	{
		Vec2 minimum;
		Vec2 maximum;
	};

	struct Line
	{
		Vec2 from;
		Vec2 to;
		PackedColourValue colour{};
		float thickness{ 1.0f };
		ClipRectangle clip;
	};

	struct Triangle
	{
		Vec2 positions[3];
		Vec2 texcoords[3];
		PackedColourValue colour{};
		Texture texture{ Texture::None };
		ClipRectangle clip;
	};


	struct Barrier {};
	using Command = std::variant<Line, Triangle, Barrier>;

	explicit WorldDrawList(ClipRectangle initialClip);

	void PushClipRect(Vec2 minimum, Vec2 maximum, bool intersectWithCurrentClip = false);
	void PopClipRect();
	Vec2 GetClipRectMin() const;
	Vec2 GetClipRectMax() const;

	void AddLine(Vec2 from, Vec2 to, PackedColourValue colour, float thickness = 1.0f);
	void AddRect(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
		float rounding = 0.0f, DrawFlags flags = 0, float thickness = 1.0f);
	void AddRectFilled(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
		float rounding = 0.0f, DrawFlags flags = 0);
	void AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColourValue colour);
	void AddCircle(Vec2 centre, float radius, PackedColourValue colour,
		int segments = 0, float thickness = 1.0f);
	void AddCircleFilled(Vec2 centre, float radius, PackedColourValue colour, int segments = 0);
	void AddPolyline(Vec2 const* points, int count, PackedColourValue colour,
		DrawFlags flags, float thickness);
	void AddImage(Texture texture, Vec2 minimum, Vec2 maximum,
		Vec2 uvMinimum, Vec2 uvMaximum, PackedColourValue colour = White);
	void AddDrawCmd();

	// Transform only geometry appended since the bookmark, retaining UVs and
	// the enclosing canvas clip. Used for bodily stance, not physical bounds.
	size_t geometryBookmark() const;
	void transformGeometrySince(size_t bookmark, Vec2 pivot, Vec2 scale,
		bool clockwiseQuarterTurn);

	std::vector<Command> const& commands() const { return mCommands; }

private:
	ClipRectangle currentClip() const;
	void addTriangle(Vec2 a, Vec2 b, Vec2 c, Vec2 uvA, Vec2 uvB,
		Vec2 uvC, PackedColourValue colour, Texture texture);

	std::vector<ClipRectangle> mClipStack;
	std::vector<Command> mCommands;
};

} // namespace pf::application::render
