#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "DrawTypes.h"

// The command stream is written in the shared drawing vocabulary this module
// owns (#996, #997): pf::render::Vec2 for positions and extents,
// pf::render::PackedColourValue for colours, pf::render::DrawFlags for the
// closed/open stroke flag. They are brought in unqualified here so the
// recording API reads the way it did when it spelled ImVec2, ImU32 and
// ImDrawFlags - but they are the shared types, not ImGui's.
using pf::render::Vec2;
using pf::render::Vec4;
using pf::render::PackedColourValue;
using pf::render::packColour;
using pf::render::DrawFlags;
using pf::render::DrawFlagsNone;
using pf::render::DrawFlagsClosed;
using pf::render::White;

// Two ImGui names remain reachable from this header, and only as opaque
// pointers, so no ImGui header version or context is needed to record a
// World:
//
//   ImFont*  the explicit-font text path, replaced by the injected font
//            metrics interface in a later step of #995;
//   ImDrawList*  the optional test adapter, which a later step of #995
//            moves out of the command stream into the headless harness.
//
// Neither is a value type carried by a command: a recorded command holds no
// ImGui value.
struct ImFont;
struct ImDrawList;

// CPU-side command stream for the World viewport. The GUI path submits these
// commands to MPP; the optional ImDrawList adapter exists only for the
// headless renderer checks and never participates in application rendering.
class WorldDrawList
{
public:
	enum class Texture
	{
		None,
		SectorAtlas,
		ObjectAtlas,
		AgentAtlas,
		FurnitureAtlas,
		FontAtlas
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

	struct Text
	{
		Vec2 position;
		PackedColourValue colour{};
		std::string value;
		ClipRectangle clip;
		// Explicit-font text can be tessellated for non-uniform stance
		// transforms; ordinary text remains on the native MPP text path.
		ImFont* font{};
		float fontSize{};
	};

	struct Barrier {};
	using Command = std::variant<Line, Triangle, Text, Barrier>;

	explicit WorldDrawList(ClipRectangle initialClip);
	explicit WorldDrawList(ImDrawList* testAdapter);

	void PushClipRect(Vec2 minimum, Vec2 maximum, bool intersectWithCurrentClip = false);
	void PopClipRect();
	Vec2 GetClipRectMin() const;
	Vec2 GetClipRectMax() const;

	void AddLine(Vec2 from, Vec2 to, PackedColourValue colour, float thickness = 1.0f);
	void AddRect(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
		float rounding = 0.0f, DrawFlags flags = DrawFlagsNone, float thickness = 1.0f);
	void AddRectFilled(Vec2 minimum, Vec2 maximum, PackedColourValue colour,
		float rounding = 0.0f, DrawFlags flags = DrawFlagsNone);
	void AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColourValue colour);
	void AddCircle(Vec2 centre, float radius, PackedColourValue colour,
		int segments = 0, float thickness = 1.0f);
	void AddCircleFilled(Vec2 centre, float radius, PackedColourValue colour, int segments = 0);
	void AddPolyline(Vec2 const* points, int count, PackedColourValue colour,
		DrawFlags flags, float thickness);
	void AddText(Vec2 position, PackedColourValue colour, char const* text);
	void AddText(ImFont* font, float fontSize, Vec2 position, PackedColourValue colour,
		char const* text, char const* textEnd = nullptr, float wrapWidth = 0.0f,
		Vec4 const* fineClipRect = nullptr);
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

	ImDrawList* mTestAdapter{};
	std::vector<ClipRectangle> mClipStack;
	std::vector<Command> mCommands;
};
