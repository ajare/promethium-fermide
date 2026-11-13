#include "Checks.h"
#include "WorldDrawReplay.h"
#include "ImGuiContext.h"
// The real renderSector() draw-call order for a Facade hosting thresholds,
// for ticket #49.
//
// A Facade's surface used to be emitted in the type switch, after the
// BEHIND object pass - so a Door or Window authored on a Facade was drawn
// and then painted over by the opaque fill. The policy-model checks could
// not see that: they replay the surface decisions, not the z-order. This
// check executes the real renderer and reads the command stream back,
// pinning the order the viewport actually paints in:
//
//   * the Solid pass lays the Facade's own tile, and that tile lands
//     before the Door's leaf and before any aperture fill - the apertures
//     are painted over the surface, never under it;
//   * the tile covers the Facade's whole bounds, so moving it earlier did
//     not shrink or clip the surface itself;
//   * the wireframe overlay still outlines the Facade (never fills it)
//     and still lands before the thresholds' own outlines.
//
// The signal is which tile a quad sampled, not what colour it wore: a
// Facade is artwork now, and no colour identifies it.

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "imgui/imgui.h"

#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "core/World.h"
#include "core/Defines.h"
#include "core/Facade.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/Vector2.h"
#include "core/Window.h"

extern UISettings gUISettings;

namespace
{
	void require(bool condition, std::string const& message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// A sheet with one full-cell tile per surface the scene uses, each in
	// its own column, so a quad's tile is readable back out of the UVs it
	// sampled.
	SectorTileset installSceneAtlas()
	{
		SectorTileset atlas;
		atlas.family = SectorTilesetFamily::New;
		int column = 0;
		for (auto const* name : { "room", "facade" })
			atlas.surfaces.emplace(name, SectorTileRegion{
				column++ * kCellWidth, 0, kCellWidth, kCellHeight });
		atlas.width = column * kCellWidth;
		atlas.height = kCellHeight;
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic Facade draw-order atlas is not the New family");
		return atlas;
	}

	std::string tileAt(float centreU, float centreV, SectorTileset const& atlas)
	{
		for (auto const& [name, region] : atlas.surfaces)
		{
			auto const left = float(region.x) / float(atlas.width);
			auto const right = float(region.x + region.width) / float(atlas.width);
			auto const top = float(region.y) / float(atlas.height);
			auto const bottom = float(region.y + region.height) / float(atlas.height);
			if (centreU >= left && centreU < right && centreV >= top
					&& centreV < bottom)
				return name;
		}
		return {};
	}

	// One textured quad, with its position in the command stream and the
	// tile its UVs sampled.
	struct Quad
	{
		std::size_t index{ 0 };
		std::string tile{};
		WorldDrawList::Triangle const* triangle{ nullptr };
	};

	// Every SectorAtlas quad in the stream, tagged with its tile.
	std::vector<Quad> sectorQuads(WorldDrawList const& drawing,
		SectorTileset const& atlas)
	{
		std::vector<Quad> quads;
		auto const& commands = drawing.commands();
		for (std::size_t i = 0; i < commands.size(); ++i)
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&commands[i]);
			if (!triangle
					|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			float u0{ 1e9f }, u1{ -1e9f }, v0{ 1e9f }, v1{ -1e9f };
			for (auto const& uv : triangle->texcoords)
			{
				u0 = std::min(u0, uv.x); u1 = std::max(u1, uv.x);
				v0 = std::min(v0, uv.y); v1 = std::max(v1, uv.y);
			}
			quads.push_back({ i, tileAt((u0 + u1) * 0.5f, (v0 + v1) * 0.5f, atlas),
				triangle });
		}
		return quads;
	}

	// The first command drawing a filled triangle in `colour`, or nullopt.
	// With no object atlas installed the Door's leaf falls back to a filled
	// rect in its own colour, which is what this looks for.
	std::optional<std::size_t> firstFilledWith(WorldDrawList const& drawing,
		PackedColourValue colour)
	{
		auto const& commands = drawing.commands();
		for (std::size_t i = 0; i < commands.size(); ++i)
			if (auto const* t = std::get_if<WorldDrawList::Triangle>(&commands[i]);
				t && t->texture == WorldDrawList::Texture::None
				&& t->colour == colour)
				return i;
		return std::nullopt;
	}

	// The first Line command wearing `colour`, or nullopt.
	std::optional<std::size_t> firstLine(WorldDrawList const& drawing,
		PackedColourValue colour)
	{
		auto const& commands = drawing.commands();
		for (std::size_t i = 0; i < commands.size(); ++i)
			if (auto const* line = std::get_if<WorldDrawList::Line>(&commands[i]);
				line && line->colour == colour)
				return i;
		return std::nullopt;
	}

	// A two-Layer scene: a Room on Layer 1, a Facade directly in front of it
	// on Layer 0 (Layers are numbered front-to-back), carrying one Window and
	// one Door - the two threshold types the surface used to erase. Hands
	// the caller the World and the Facade's Sector index.
	template <typename Fn>
	void withFacadeHostingThresholds(Fn&& fn)
	{
		core::World world("Facade draw order", 12, 3);
		while (world.getLayerCount() < 2) world.addLayer();
		world.addRoom("Behind", 1, 0, 0, 4, 1);
		auto const facadeIndex = world.addFacade("Frontage", 0, 0, 0, 4, 2,
			CORE_ROOM_MAX_HEIGHT);
		world.finishBuild();
		world.pauseSimulation();

		std::string diagnostic;
		require(world.canAddSectorWindow(0, 0, 1, 2, 1, &diagnostic),
			("A Window on a Facade was refused: " + diagnostic).c_str());
		world.addSectorWindow(0, 0, 1, 2, 1,
			{ false, core::Window::State::Closed, core::Window::Style::Clear });
		world.addSectorDoor(0, 0, 3);
		world.finishBuild();

		fn(world, facadeIndex);
	}

	WorldDrawList renderFacadePass(core::World& world, uint32_t facadeIndex,
		LayerRenderStyle style)
	{
		WorldDrawList drawing{ WorldDrawList::ClipRectangle{
			{ -100000.0f, -100000.0f }, { 100000.0f, 100000.0f } } };
		// The Layer colour the viewport would pass down as the generic fill.
		renderSector(world.getSector(facadeIndex), 0, style, false,
			packColour(192, 192, 255), &drawing);
		return drawing;
	}
}

// The Solid pass: the Facade's own tile lands first, and the Door's leaf
// and the apertures behind the thresholds are painted over it - not under
// it. With the old ordering (surface in the type switch, after the BEHIND
// pass) both ordering assertions below fail.
void facadeSurfacePrecedesThresholdApertures()
{
	headless::ScopedImGuiContext imgui;
	auto const atlas = installSceneAtlas();

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		auto const drawing = renderFacadePass(world, facadeIndex,
			LayerRenderStyle::Solid);
		auto const quads = sectorQuads(drawing, atlas);

		std::optional<std::size_t> facade;
		for (auto const& q : quads)
			if (q.tile == "facade")
			{
				if (!facade || q.index < *facade) facade = q.index;
			}
		require(facade.has_value(), "the Solid pass laid no Facade tile at all");

		// renderDoor's leaf colour, which is what the leaf falls back to
		// with no object atlas installed behind it.
		auto const door = firstFilledWith(drawing, packColour(64, 192, 255));
		require(door.has_value(), "the Door on the Facade painted no leaf");
		require(*facade < *door,
			"the Facade surface was painted over the Door's leaf (#49 regression)");

		// The aperture behind the Window shows the Room's own tile. That it
		// lands after the Facade's is the whole of the #49 ordering rule.
		std::optional<std::size_t> aperture;
		for (auto const& q : quads)
			if (q.tile == "room")
			{
				if (!aperture || q.index < *aperture) aperture = q.index;
			}
		require(aperture.has_value(), "no aperture fill reached the Sector behind");
		require(*facade < *aperture,
			"the Facade surface was painted over the threshold apertures (#49 regression)");
	});
}

// The surface moved earlier must still be the Facade's whole surface: its
// tiles span exactly the Facade's transformed bounds.
void facadeSurfaceCoversTheWholeFacadeSurface()
{
	headless::ScopedImGuiContext imgui;
	auto const atlas = installSceneAtlas();

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		auto sector = world.getSector(facadeIndex);
		auto const drawing = renderFacadePass(world, facadeIndex,
			LayerRenderStyle::Solid);

		core::Vector2 b0, b1;
		sector->getBounds(b0, b1);

		// The same transform transformPosition() applies, recomputed here.
		auto const tx = [](float x)
		{
			return x * CORE_CELL_WIDTH_PIXELS + gUISettings.worldViewportX + gUISettings.xOffset;
		};
		auto const ty = [](float y)
		{
			return gUISettings.worldViewportY + gUISettings.worldViewportHeight
				- y * CORE_LEVEL_HEIGHT_PIXELS - gUISettings.yOffset;
		};

		float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
		int tiles = 0;
		for (auto const& q : sectorQuads(drawing, atlas))
		{
			if (q.tile != "facade") continue;
			++tiles;
			for (auto const& point : q.triangle->positions)
			{
				minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
				minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
			}
		}

		require(tiles > 0, "no Facade tiles to measure");

		constexpr float kEpsilon = 0.01f;
		require(std::abs(minX - tx(std::min(b0.x, b1.x))) < kEpsilon
			&& std::abs(maxX - tx(std::max(b0.x, b1.x))) < kEpsilon
			&& std::abs(minY - ty(std::max(b0.y, b1.y))) < kEpsilon
			&& std::abs(maxY - ty(std::min(b0.y, b1.y))) < kEpsilon,
			"the Facade surface no longer covers the Facade's whole bounds");
	});
}

// The wireframe overlay: the Facade contributes its outline - never a fill -
// and that outline lands before the thresholds' own outlines.
void wireframePassOutlinesFacadeBeforeThresholds()
{
	headless::ScopedImGuiContext imgui;
	auto const atlas = installSceneAtlas();

	withFacadeHostingThresholds([&](core::World& world, uint32_t facadeIndex)
	{
		auto const drawing = renderFacadePass(world, facadeIndex,
			LayerRenderStyle::Wireframe);

		require(sectorQuads(drawing, atlas).empty(),
			"the wireframe overlay filled the Facade instead of outlining it");

		// The Facade's outline wears the pass colour the viewport handed
		// down; the thresholds draw theirs in black.
		auto const outline = firstLine(drawing, packColour(192, 192, 255));
		require(outline.has_value(), "the wireframe overlay drew no Facade outline");

		auto const threshold = firstLine(drawing, packColour(0, 0, 0));
		require(threshold.has_value(), "the wireframe pass drew no threshold outlines");
		require(*outline < *threshold,
			"the Facade outline did not precede the threshold outlines");
	});
}


void render_smoke::registerFacadeDrawOrder(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "facadeSurfacePrecedesThresholdApertures", isolated<[](smoke::Context const&) { facadeSurfacePrecedesThresholdApertures(); }> });
	checks.push_back({ "facadeSurfaceCoversTheWholeFacadeSurface", isolated<[](smoke::Context const&) { facadeSurfaceCoversTheWholeFacadeSurface(); }> });
	checks.push_back({ "wireframePassOutlinesFacadeBeforeThresholds", isolated<[](smoke::Context const&) { wireframePassOutlinesFacadeBeforeThresholds(); }> });
}
