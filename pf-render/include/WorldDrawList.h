#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "DrawTypes.h"
#include "FontMetrics.h"

// The command stream is written in the shared drawing vocabulary this module
// owns (#996, #997, #999): pf::render::Vec2 for positions and extents,
// pf::render::PackedColourValue for colours, pf::render::DrawFlags for the
// closed/open stroke flag, and pf::render::FontRef for the font a text
// command asks the application for. They are brought in unqualified here so
// the recording API reads the way it did when it spelled ImVec2, ImU32,
// ImDrawFlags and ImFont* - but they are the shared types, not ImGui's.
using pf::render::Vec2;
using pf::render::Vec4;
using pf::render::PackedColourValue;
using pf::render::packColour;
using pf::render::DrawFlags;
using pf::render::DrawFlagsNone;
using pf::render::DrawFlagsClosed;
using pf::render::White;
using pf::render::FontRef;
using pf::render::WorldFont;
using pf::render::IFontMetrics;

// Nothing ImGui is reachable from this header. No recorded command holds an
// ImGui value and no ImGui type is named here. The stream used to accept a
// live ImDrawList as an optional test adapter and mirror every recording
// into it; that scaffolding is gone (#1000) - the headless harness now reads
// commands() back out and replays them itself, so the adapter lives with the
// tests that need it instead of inside production recording. A text command
// carries a FontRef and the application answers for it through the
// installed IFontMetrics (#999).

// CPU-side command stream for the World viewport. Every recording method
// writes to the command vector and to nothing else: the GUI path submits
// those commands to MPP, and a caller inspects them through commands().
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
		// The font this run asked for, as a role and a size rather than a
		// font object. A run left at the default size stays on the backend's
		// native text path; a sized run is what a non-uniform stance
		// transform tessellates through the injected metrics (#999).
		FontRef font{};
	};

	struct Barrier {};
	using Command = std::variant<Line, Triangle, Text, Barrier>;

	explicit WorldDrawList(ClipRectangle initialClip);

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
	void AddText(FontRef font, Vec2 position, PackedColourValue colour,
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

	// The font metrics the application installed for this stream to measure
	// and tessellate with (#999). Recording text with no metrics installed is
	// a setup error rather than a silent fallback, so this throws.
	static IFontMetrics const& fontMetrics()
	{
		return pf::render::requireFontMetrics();
	}

private:
	ClipRectangle currentClip() const;
	void addTriangle(Vec2 a, Vec2 b, Vec2 c, Vec2 uvA, Vec2 uvB,
		Vec2 uvC, PackedColourValue colour, Texture texture);

	std::vector<ClipRectangle> mClipStack;
	std::vector<Command> mCommands;
};
