// The font metrics seam (#999): what a recorded Text command carries, and
// where the producer puts the text it measured. Every assertion here is on
// recorded commands and their placement - never on rasterised glyphs - and
// the numbers come from a probe text system the check owns, so they are
// exact rather than close.

#include "Checks.h"

#include "FontMetrics.h"
#include "Render.h"
#include "UISettings.h"
#include "WorldDrawList.h"

#include "imgui/IconsFontAwesome5.h"

#include "core/Agent.h"
#include "core/Marker.h"
#include "core/World.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

extern UISettings gUISettings;

namespace
{
	using pf::render::FontMetricsScope;
	using pf::render::FontRef;
	using pf::render::IFontMetrics;
	using pf::render::TessellatedGlyphs;
	using pf::render::Vec2;
	using pf::render::WorldFont;

	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	// One face of the probe text system.
	struct ProbeFace
	{
		float naturalHeight;	// the face's own pixel size
		float advance;		// every glyph advances this much at that size
	};

	// Code points, not bytes: an icon escape is three UTF-8 bytes for one
	// glyph, and a text system lays out glyphs.
	size_t glyphCount(char const* text)
	{
		size_t count = 0;
		for (auto const* p = text; *p; ++p)
			if ((static_cast<unsigned char>(*p) & 0xC0) != 0x80) ++count;
		return count;
	}

	// A text system whose numbers are known exactly, so a placement
	// assertion pins the producer's arithmetic rather than some font's
	// metrics. A sized ref scales the run linearly, as an atlas does. The
	// faces used below are deliberately unlike each other - one tall and
	// narrow, one short and wide - so a label that centres against one of
	// them and not the other cannot pass for both. Every value is a power
	// of two or a half of one, so each product is exact in binary floating
	// point and the assertions are equalities, not tolerances.
	class ProbeFontMetrics final : public IFontMetrics
	{
	public:
		ProbeFontMetrics(ProbeFace body, ProbeFace icons) : mBody(body), mIcons(icons) {}

		Vec2 measure(FontRef const& ref, char const* text) const override
		{
			++measurements;
			if (!text) return {};
			auto const& face = faceFor(ref.font);
			auto const size = ref.size > 0.0f ? ref.size : face.naturalHeight;
			return Vec2{ face.advance * static_cast<float>(glyphCount(text))
				* size / face.naturalHeight, size };
		}

		TessellatedGlyphs tessellate(FontRef const& ref, char const* text) const override
		{
			++tessellations;
			TessellatedGlyphs glyphs;
			// A run left at the default size belongs to the backend's native
			// text path: there is nothing tessellated for it.
			if (ref.size <= 0.0f || !text || !*text) return glyphs;
			auto const& face = faceFor(ref.font);
			auto const width = face.advance * ref.size / face.naturalHeight;
			auto const count = glyphCount(text);
			for (size_t index = 0; index < count; ++index)
			{
				auto const left = width * static_cast<float>(index);
				auto const right = left + width;
				glyphs.push_back({ Vec2{ left, 0.0f }, Vec2{ 0.0f, 0.0f } });
				glyphs.push_back({ Vec2{ right, 0.0f }, Vec2{ 1.0f, 0.0f } });
				glyphs.push_back({ Vec2{ right, ref.size }, Vec2{ 1.0f, 1.0f } });
				glyphs.push_back({ Vec2{ left, 0.0f }, Vec2{ 0.0f, 0.0f } });
				glyphs.push_back({ Vec2{ right, ref.size }, Vec2{ 1.0f, 1.0f } });
				glyphs.push_back({ Vec2{ left, ref.size }, Vec2{ 0.0f, 1.0f } });
			}
			return glyphs;
		}

		ProbeFace const& faceFor(WorldFont font) const
		{
			return font == WorldFont::Icons ? mIcons : mBody;
		}

		mutable int measurements{};
		mutable int tessellations{};

	private:
		ProbeFace mBody;
		ProbeFace mIcons;
	};

	WorldDrawList::Text const& onlyText(WorldDrawList const& list, char const* value)
	{
		WorldDrawList::Text const* found{};
		int hits = 0;
		for (auto const& command : list.commands())
			if (auto const* text = std::get_if<WorldDrawList::Text>(&command);
				text && text->value == value) { found = text; ++hits; }
		require(hits == 1, "Expected exactly one recorded text command, got "
			+ std::to_string(hits) + std::string(" for \"") + value + "\"");
		return *found;
	}

	// Where a recorded label sits relative to the anchor it was drawn for.
	// Each placement rule fixes a different pair of edges, so each has its
	// own reader: what must not move when the face answering the request
	// changes is the anchor, not the box.
	struct PlacedLabel
	{
		float centredOnX;
		float placedAtY;
	};

	// The Marker rule: centred on the anchor's x, hung from the anchor so
	// its bottom edge sits on it.
	PlacedLabel hungFromAnchor(WorldDrawList::Text const& text, IFontMetrics const& metrics)
	{
		auto const size = metrics.measure(text.font, text.value.c_str());
		return { text.position.x + size.x * 0.5f, text.position.y + size.y };
	}

	// The Agent badge rule: centred on both axes of the slot it fills.
	PlacedLabel centredInSlot(WorldDrawList::Text const& text, IFontMetrics const& metrics)
	{
		auto const size = metrics.measure(text.font, text.value.c_str());
		return { text.position.x + size.x * 0.5f, text.position.y + size.y * 0.5f };
	}

	// The two icon faces the Marker check renders the same Marker with. At
	// zoom 1 the tall narrow face puts the glyph in an 11x22 box and the
	// short wide one in a 22x5.5 box - the same centred x and the same
	// hanging bottom edge, or the label has moved.
	struct MarkerCase
	{
		char const* name;
		ProbeFace face;
		float expectedSize;
	};

	MarkerCase const kMarkerCases[] = {
		{ "tall", { 32.0f, 16.0f }, 22.0f },
		{ "wide", { 16.0f, 64.0f }, 5.5f }
	};

	std::shared_ptr<core::World> markerWorld()
	{
		auto world = std::make_shared<core::World>("Font metrics", 8, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->addSectorMarker(room, 0, 3.5f, "Pin");
		world->finishBuild();
		world->pauseSimulation();
		return world;
	}

	// Both text entry points, and what each records.
	void textEntryPoints(smoke::Context const&)
	{
		ProbeFontMetrics metrics{ { 16.0f, 8.0f }, kMarkerCases[0].face };
		FontMetricsScope scope{ &metrics };
		WorldDrawList list({ { -1000, -1000 }, { 1000, 1000 } });

		list.AddText(Vec2{ 10, 20 }, packColour(10, 20, 30, 255), "Q1");
		list.AddText({ WorldFont::Icons, 22.0f }, Vec2{ 30, 40 },
			packColour(40, 50, 60, 255), "X");

		require(list.commands().size() == 2, "Neither text entry point recorded a command");
		auto const& implicitText = std::get<WorldDrawList::Text>(list.commands()[0]);
		require(implicitText.value == "Q1" && implicitText.position.x == 10.0f
			&& implicitText.font.font == WorldFont::Default && implicitText.font.size == 0.0f,
			"The implicit entry point did not record the default face at its own size");
		auto const& explicitText = std::get<WorldDrawList::Text>(list.commands()[1]);
		require(explicitText.value == "X" && explicitText.position.x == 30.0f
			&& explicitText.font.font == WorldFont::Icons && explicitText.font.size == 22.0f,
			"The explicit entry point did not record the requested font reference");
		require(metrics.measurements == 0,
			"Recording a text command measured something: the stream records the request, not the glyphs");
	}

	// The Marker icon: sized by the face it asked for, and hung from the
	// same anchor whichever face answered.
	void markerCentring(smoke::Context const&)
	{
		auto const world = markerWorld();
		gUISettings.worldZoom = 1.0f;
		gUISettings.xOffset = gUISettings.yOffset = 0.0f;
		gUISettings.worldViewportX = gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 800.0f;
		gUISettings.worldViewportHeight = 600.0f;

		std::optional<PlacedLabel> anchor;
		for (auto const& testCase : kMarkerCases)
		{
			ProbeFontMetrics metrics{ { 16.0f, 8.0f }, testCase.face };
			FontMetricsScope scope{ &metrics };
			WorldDrawList drawing({ { 0, 0 }, { 800, 600 } });
			renderWorld(world, &drawing);
			// No object artwork is installed, so the Marker falls through to
			// its glyph - the only path that needs the icon face at all.
			auto const& text = onlyText(drawing, ICON_FA_MAP_MARKER_ALT);
			require(text.font.font == WorldFont::Icons,
				"The Marker asked for a face other than the icon face");
			// The producer scales the face's own pixel height by
			// MarkerIconSize and zoom, divided by the longest edge of the
			// natural run: 32 * 22 / 32 for the tall face, 16 * 22 / 64
			// for the wide one.
			require(text.font.size == testCase.expectedSize,
				std::string("The ") + testCase.name
				+ " icon face did not scale the Marker glyph as expected: got "
				+ std::to_string(text.font.size) + ", expected "
				+ std::to_string(testCase.expectedSize));
			auto const placed = hungFromAnchor(text, metrics);
			if (anchor)
			{
				require(placed.centredOnX == anchor->centredOnX,
					"Marker centring moved with the icon face: a shifted label is a regression");
				require(placed.placedAtY == anchor->placedAtY,
					"Marker baseline placement moved with the icon face");
			}
			else anchor = placed;
			require(metrics.measurements >= 2,
				"The Marker did not measure through the injected metrics interface");
		}
		require(anchor.has_value(), "The Marker glyph pass never ran");
	}

	// The Agent debug badge: centred in its slot by the body face's own
	// metrics, whichever face answered.
	void badgeCentring(smoke::Context const&)
	{
		auto world = std::make_shared<core::World>("Badge metrics", 8, 2);
		auto const room = world->addRoom("Room", 0, 0, 0, 8, 1);
		world->addSectorMarker(room, 0, 5.5f, "Goal");
		world->finishBuild();
		auto const agentId = world->createAgent("Planner", room, 0, 1.5f);
		auto* agent = world->lookupAgent(agentId).entity;
		require(world->moveAgentToMarker(agentId, world->getMarkerIds().front()).accepted(),
			"Badge fixture was not accepted");
		require(agent->getState() == core::Agent::State::RoutePlanning,
			"Badge fixture is not planning");

		gUISettings = UISettings{};
		gUISettings.renderAgentDebug = true;
		gUISettings.renderMarkers = false;
		gUISettings.xOffset = gUISettings.yOffset = 0.0f;
		gUISettings.worldViewportWidth = 800.0f;
		gUISettings.worldViewportHeight = 600.0f;

		std::optional<PlacedLabel> centre;
		for (auto const face : { ProbeFace{ 16.0f, 8.0f }, ProbeFace{ 32.0f, 16.0f } })
		{
			ProbeFontMetrics metrics{ face, kMarkerCases[0].face };
			FontMetricsScope scope{ &metrics };
			WorldDrawList drawing({ { 0, 0 }, { 800, 600 } });
			renderAgent(agent, &drawing);
			auto const& text = onlyText(drawing, "?");
			require(text.font.font == WorldFont::Default && text.font.size == 0.0f,
				"The Agent debug badge asked for a sized face instead of the body face at its own size");
			auto const placed = centredInSlot(text, metrics);
			if (centre)
				require(placed.centredOnX == centre->centredOnX
					&& placed.placedAtY == centre->placedAtY,
					"The badge drifted off its slot when the body face changed");
			else centre = placed;
			require(metrics.measurements >= 1,
				"The badge did not measure through the injected metrics interface");
		}
		require(centre.has_value(), "The badge text pass never ran");
	}

	// The stance transform: a sized run is tessellated through the metrics
	// and the resulting atlas triangles carry the stance; a run left at the
	// default size is left alone.
	void stanceTransformTessellation(smoke::Context const&)
	{
		ProbeFontMetrics metrics{ { 16.0f, 8.0f }, kMarkerCases[0].face };
		FontMetricsScope scope{ &metrics };
		WorldDrawList list({ { -1000, -1000 }, { 1000, 1000 } });
		auto const bookmark = list.geometryBookmark();
		auto const colour = packColour(200, 100, 50, 255);
		list.AddText(Vec2{ 50, 60 }, colour, "implicit");
		list.AddText({ WorldFont::Icons, 22.0f }, Vec2{ 100, 200 }, colour, "AB");
		require(list.commands().size() == 2, "The stance fixture did not record both text runs");

		// Non-uniform: twice as wide, half as tall, about the sized run's
		// own origin.
		list.transformGeometrySince(bookmark, Vec2{ 100, 200 }, Vec2{ 2.0f, 0.5f }, false);

		auto const& implicitText = std::get<WorldDrawList::Text>(list.commands()[0]);
		require(implicitText.value == "implicit" && implicitText.position.x == 50.0f
			&& implicitText.position.y == 60.0f,
			"The stance transform touched text that never asked for a size");

		// The probe lays "AB" out at 11px per glyph for a 22px icon face,
		// so the run spans x 100..122 and y 200..222. Scaled about
		// (100, 200) by (2, 0.5) that is x 100..144 and y 200..211.
		size_t glyphTriangles = 0;
		float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
		for (auto const& command : list.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::FontAtlas) continue;
			++glyphTriangles;
			require(triangle->colour == colour, "A tessellated glyph lost its colour");
			require(triangle->clip.minimum.x == -1000.0f
				&& triangle->clip.maximum.x == 1000.0f,
				"A tessellated glyph lost the canvas clip");
			for (auto const& p : triangle->positions)
			{
				minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
				minY = std::min(minY, p.y); maxY = std::max(maxY, p.y);
			}
		}
		// Two glyphs, two triangles per glyph quad.
		require(glyphTriangles == 4, "The stance transform tessellated the wrong number of glyphs");
		require(minX == 100.0f && maxX == 144.0f && minY == 200.0f && maxY == 211.0f,
			"The non-uniform stance transform did not move the tessellated glyphs as asked");
		require(metrics.tessellations == 1,
			"Glyph geometry did not come from the injected metrics interface");
	}
}

void render_smoke::registerFontMetrics(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "text/entryPoints", isolated<textEntryPoints> });
	checks.push_back({ "text/markerCentring", isolated<markerCentring> });
	checks.push_back({ "text/badgeCentring", isolated<badgeCentring> });
	checks.push_back({ "text/stanceTransformTessellation", isolated<stanceTransformTessellation> });
}
