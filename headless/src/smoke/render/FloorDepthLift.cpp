// A Sector's floor depth lifts the tile surface of the Layers behind it (#1101).
//
// This is the picture the feature is for, so every assertion here is on the
// command vector the real render pass recorded - the stable CPU seam ADR
// 0010 names - and nothing is predicted from a formula the renderer was
// asked to reproduce. What the check asks is what the ticket states: put a
// Sector on Layer 0 at depth 48, and the Layer-1 tile surface must record
// 48 pixels higher than it recorded at zero depth.
//
// The things worth pinning beyond that headline:
//
//   * the Sector carrying the depth does not move. A lift that moved the
//     lifter along with everything behind it would be a scroll, and would
//     read as the whole World jumping rather than as a floor running deep
//     behind;
//   * a flat World records the stream it recorded before the lift term
//     existed, exactly and not approximately. The lift enters the shared
//     world-to-screen mapping (#1100), so a zero that came out as `0.0f`
//     added by a different route - a reassociation of the terms, a
//     rounding step inserted ahead of it - would perturb every coordinate
//     in the game by a hair while looking identical. These assertions are
//     on float equality with the pre-change expression, not on a tolerance;
//   * the lift is in screen pixels and scales with the zoom. The World
//     stores document-space pixels at 1x; a lift that did not scale would
//     be 48 pixels at every zoom, which is 48 pixels of a two-pixel-per-
//     pixel view and not the same lift at all;
//   * the lift is recomputed from the World each frame rather than cached.
//     Two renders with a depth change between them, and no invalidation
//     call anywhere, must show the second one moved. A cache with a
//     forgotten invalidation is a lift that sticks;
//   * the Sector's own clip stays with its unlifted footprint. This slice
//     moves the tile surface and nothing else - thresholds, transit
//     geometry, objects, Agents, the selection highlight all still draw
//     where they drew. A surface lifted out of a clip that moved with it
//     would be the whole feature arriving a slice early and untested;
//   * a negative depth pushes the Layers behind down rather than up, and
//     the push is the same magnitude.
//
// The atlas is a synthetic sheet with one full-cell tile per surface name,
// installed the way the render stack installs it, so the quads measured
// here are real `SectorAtlas` triangles rather than the missing-tile
// fallback that would draw at the same place and prove nothing.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldToScreen.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// The renderer's live viewport settings, shared with every other check in
// the module.
extern UISettings gUISettings;

namespace
{
	using smoke::require;
	using namespace render_smoke;

	bool near(float a, float b, float epsilon = 0.01f)
	{
		return std::abs(a - b) < epsilon;
	}

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;
	PackedColourValue const kTint = White;

	// The synthetic sheet: one full-cell tile under each surface name the
	// Sectors below ask for.
	void installFlatAtlas()
	{
		SectorTileset atlas;
		atlas.width = kCellWidth * 4;
		atlas.height = kCellHeight * 4;
		atlas.family = SectorTilesetFamily::New;
		atlas.surfaces.emplace("room",
			SectorTileRegion{ 0, 0, kCellWidth, kCellHeight });
		atlas.surfaces.emplace("corridor",
			SectorTileRegion{ kCellWidth, 0, kCellWidth, kCellHeight });
		require(atlas.surfaces.size() == 2,
			"The synthetic sheet did not carry the two surfaces asked for");
		setSectorTileset(atlas, static_cast<TextureId>(1));
		require(sectorTilesetIsNewFamily(drawingSectorSheet()),
			"The synthetic sheet did not come up as the New family");
	}

	// The viewport, pinned. Origin and both offsets at zero and the zoom at
	// one, so a screen coordinate is the mapping's answer and nothing else.
	void pinViewport(float zoom = 1.0f)
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 4096.0f;
		gUISettings.worldViewportHeight = 4096.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = zoom;
	}

	// The mapping as it stood before the floor lift term was added (#1100):
	// the viewport's flip and the scroll, and nothing else. Stated here so
	// the flat-World check has something from before to hold the stream to.
	pf::render::WorldToScreen preChangeMapping()
	{
		return pf::render::worldToScreenAt(gUISettings, gUISettings.worldZoom);
	}

	// The bounding box one draw pass put on the screen, and the clip it was
	// recorded under.
	struct SurfaceBox
	{
		float left{};
		float right{};
		float top{};
		float bottom{};
		float clipTop{};
		float clipBottom{};
		int triangles{};
	};

	SurfaceBox boxOf(std::vector<WorldDrawList::Triangle const*> const& quads)
	{
		SurfaceBox box;
		box.left = box.top = 1e30f;
		box.right = box.bottom = -1e30f;
		box.clipTop = 1e30f;
		box.clipBottom = -1e30f;
		for (auto const* triangle : quads)
		{
			for (auto const& p : triangle->positions)
			{
				box.left = std::min(box.left, p.x);
				box.right = std::max(box.right, p.x);
				box.top = std::min(box.top, p.y);
				box.bottom = std::max(box.bottom, p.y);
			}
			box.clipTop = std::min(box.clipTop, triangle->clip.minimum.y);
			box.clipBottom = std::max(box.clipBottom, triangle->clip.maximum.y);
			++box.triangles;
		}
		return box;
	}

	std::string describe(SurfaceBox const& box)
	{
		return "x " + std::to_string(box.left) + ".." + std::to_string(box.right)
			+ ", y " + std::to_string(box.top) + ".." + std::to_string(box.bottom)
			+ ", clip y " + std::to_string(box.clipTop) + ".."
			+ std::to_string(box.clipBottom)
			+ ", triangles " + std::to_string(box.triangles);
	}

	// Records the tile surface of one Sector through the real pass.
	//
	// Only `SectorAtlas` triangles are collected: the wireframe outline,
	// the boundary strips and any procedural object art are other passes
	// and say nothing about where the surface landed.
	SurfaceBox recordSurface(std::shared_ptr<core::World> const& world,
		uint32_t const sectorIndex)
	{
		RenderWorldScope scope{ world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const sector = world->getSector(sectorIndex);
		require(sector != nullptr, "The fixture Sector could not be found");
		renderSector(sector, sector->getLayerIndex(), LayerRenderStyle::Solid,
			false, kTint, &commands);

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle && triangle->texture == WorldDrawList::Texture::SectorAtlas)
				quads.push_back(triangle);
		}
		require(!quads.empty(),
			"The Sector drew no SectorAtlas triangles, so there is no tile "
			"surface to measure and this check would pass on nothing");
		return boxOf(quads);
	}

	// Records the whole World through `renderWorld` and measures the
	// SectorAtlas triangles it laid down.
	//
	// The whole-World pass is the one that installs the per-frame staircase;
	// the direct pass above reaches it only through its own World query.
	// With the view Layer set to the back Room's, the solid surface in the
	// stream is that Room's and nothing else, so the two boxes compare.
	SurfaceBox recordSurfaceThroughRenderWorld(
		std::shared_ptr<core::World> const& world, uint32_t const viewLayer)
	{
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderWorld(world, &commands);

		std::vector<WorldDrawList::Triangle const*> quads;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (triangle && triangle->texture == WorldDrawList::Texture::SectorAtlas)
				quads.push_back(triangle);
		}
		require(!quads.empty(),
			"renderWorld drew no SectorAtlas triangles on Layer "
				+ std::to_string(viewLayer));
		return boxOf(quads);
	}

	// The top edge of the wireframe outline the same Sector draws. The
	// outline is a boundary overlay drawn outside the surface clip, so it
	// shows what the Sector's own footprint is on screen without any tile
	// surface in the way.
	float wireframeOutlineTop(std::shared_ptr<core::World> const& world,
		uint32_t const sectorIndex)
	{
		RenderWorldScope scope{ world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const sector = world->getSector(sectorIndex);
		require(sector != nullptr, "The fixture Sector could not be found");
		renderSector(sector, sector->getLayerIndex(), LayerRenderStyle::Wireframe,
			true, kTint, &commands);

		float top = 1e30f;
		int lines = 0;
		for (auto const& command : commands.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (!line) continue;
			top = std::min({ top, line->from.y, line->to.y });
			++lines;
		}
		require(lines > 0, "The wireframe pass drew no outline to measure");
		return top;
	}

	// Two Layers, one Room on each at the same footprint, no Doors between
	// them so no aperture recursion enters the pass. The front Room is the
	// one that carries the depth; the back Room is the surface measured.
	struct Fixture
	{
		std::shared_ptr<core::World> world;
		uint32_t front{};
		uint32_t behind{};
	};

	Fixture layeredRooms(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, 24, 12);
		while (world->getLayerCount() < 2) world->addLayer();
		Fixture fixture;
		fixture.world = std::move(world);
		fixture.front = fixture.world->addRoom("Front", 0, 2, 2, 3, 2);
		fixture.behind = fixture.world->addRoom("Behind", 1, 2, 2, 3, 2);
		return fixture;
	}

	// The screen box the pre-change mapping gives a Sector's World bounds,
	// with no lift term in it at all.
	SurfaceBox expectedUnlifted(core::Sector const& sector)
	{
		core::Vector2 b0, b1;
		sector.getBounds(b0, b1);
		auto const m = preChangeMapping();
		SurfaceBox box;
		box.left = m.toScreenX(b0.x);
		box.right = m.toScreenX(b1.x);
		// World y grows up; screen y grows down. The ceiling is the least y.
		box.top = m.toScreenY(b1.y);
		box.bottom = m.toScreenY(b0.y);
		return box;
	}

	// The ticket's headline, to the pixel: 48 screen pixels up.
	void checkDepthOf48LiftsTheLayerBehind(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Lift the layer behind");

		auto const flat = recordSurface(fixture.world, fixture.behind);
		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the fixture's depth write was refused");
		auto const lifted = recordSurface(fixture.world, fixture.behind);

		require(near(lifted.top, flat.top - 48.0f)
				&& near(lifted.bottom, flat.bottom - 48.0f),
			"With a Layer-0 Sector at depth 48 the Layer-1 tile surface sat at "
				+ describe(lifted) + " against a flat depth of " + describe(flat)
				+ "; it must sit 48 screen pixels higher at both edges");
		require(lifted.left == flat.left && lifted.right == flat.right,
			"The lift moved the Layer behind sideways; a floor depth is vertical");
	}

	// The Sector carrying the depth does not move with it.
	void checkTheLifterDoesNotMove(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("The lifter does not lift itself");

		auto const flat = recordSurface(fixture.world, fixture.front);
		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the fixture's depth write was refused");
		auto const lifted = recordSurface(fixture.world, fixture.front);

		require(lifted.top == flat.top && lifted.bottom == flat.bottom
				&& lifted.left == flat.left && lifted.right == flat.right,
			"The Sector carrying the depth moved with it: " + describe(flat)
				+ " became " + describe(lifted)
				+ "; a lift that moves the lifter is a scroll, not a deep floor");
	}

	// A flat World records the stream it recorded before the lift term
	// existed. Exact float equality with the pre-change mapping, not a
	// tolerance: a zero that perturbs the arithmetic by one unit in the
	// last place would move every coordinate in the game while looking the
	// same to the eye.
	void checkFlatStreamIdentity(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Flat stream identity");

		for (uint32_t const index : { fixture.front, fixture.behind })
		{
			auto const& sector = *fixture.world->getSector(index);
			auto const expected = expectedUnlifted(sector);
			auto const recorded = recordSurface(fixture.world, index);
			require(recorded.top == expected.top
					&& recorded.bottom == expected.bottom
					&& recorded.left == expected.left
					&& recorded.right == expected.right,
				"A World with every depth at zero recorded a tile surface of "
					+ describe(recorded) + " where the mapping before the lift "
					"term gave x " + std::to_string(expected.left) + ".."
					+ std::to_string(expected.right) + ", y "
					+ std::to_string(expected.top) + ".."
					+ std::to_string(expected.bottom)
					+ "; a zero lift must be bit-identical, not merely close");
		}
	}

	// The stored depth is document-space pixels at 1x; the lift on screen
	// is that count scaled by the zoom. A lift that stayed at 48 screen
	// pixels at 2x would be half the lift it claims to be.
	void checkLiftScalesWithZoom(smoke::Context const&)
	{
		installFlatAtlas();
		float const zoom = 2.0f;
		pinViewport(zoom);
		auto const fixture = layeredRooms("Zoomed lift");

		auto const flat = recordSurface(fixture.world, fixture.behind);
		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the fixture's depth write was refused");
		auto const lifted = recordSurface(fixture.world, fixture.behind);

		require(near(lifted.top, flat.top - 48.0f * zoom)
				&& near(lifted.bottom, flat.bottom - 48.0f * zoom),
			"At zoom " + std::to_string(zoom) + " a depth of 48 lifted the Layer "
				"behind by " + std::to_string(flat.top - lifted.top)
				+ " screen pixels; the stored pixels must scale with the view");
	}

	// The whole-World pass installs the staircase once per frame and every
	// Sector it draws reads from it. Measured against the direct pass,
	// which reaches the same numbers through its own World query: the two
	// routes must agree or the picture depends on how it was drawn.
	void checkRenderWorldInstallsStaircase(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Frame staircase");

		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the fixture's depth write was refused");
		auto const viaWorld = recordSurfaceThroughRenderWorld(fixture.world, 1);
		auto const direct = recordSurface(fixture.world, fixture.behind);

		require(near(viaWorld.top, direct.top) && near(viaWorld.bottom, direct.bottom),
			"The whole-World pass put the Layer-1 surface at "
				+ describe(viaWorld) + " where the direct pass gave "
				+ describe(direct)
				+ "; the staircase and the per-Sector query must agree");
	}

	// The staircase is recomputed from the World each frame, with no
	// invalidation anywhere in between. Four renders, three depth changes,
	// and each frame must show the number the World held at that moment.
	void checkLiftRecomputedPerFrame(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("No cache");

		auto const first = recordSurface(fixture.world, fixture.behind);
		require(first.top > -1e29f, "the first frame drew nothing");

		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the first depth write was refused");
		auto const second = recordSurface(fixture.world, fixture.behind);
		require(near(second.top, first.top - 48.0f),
			"The frame after a depth of 48 did not pick it up: "
				+ describe(second));

		require(fixture.world->setSectorFloorDepth(fixture.front, 12).changed,
			"the second depth write was refused");
		auto const third = recordSurface(fixture.world, fixture.behind);
		require(near(third.top, first.top - 12.0f),
			"The frame after the depth changed to 12 still drew "
				+ std::to_string(first.top - third.top)
				+ "px of lift; a cached staircase would be stuck on the old value");

		require(fixture.world->clearSectorFloorDepth(fixture.front).changed,
			"the clear was refused");
		auto const fourth = recordSurface(fixture.world, fixture.behind);
		require(fourth.top == first.top && fourth.bottom == first.bottom,
			"After the depth was cleared the Layer still draws lifted: "
				+ describe(fourth));
	}

	// A negative depth pushes the Layers behind down by the same amount.
	void checkNegativeDepthPushesDown(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Negative lift");

		auto const flat = recordSurface(fixture.world, fixture.behind);
		require(fixture.world->setSectorFloorDepth(fixture.front, -40).changed,
			"the fixture's negative depth write was refused");
		auto const pushed = recordSurface(fixture.world, fixture.behind);

		require(near(pushed.top, flat.top + 40.0f)
				&& near(pushed.bottom, flat.bottom + 40.0f),
			"A depth of -40 put the Layer behind at " + describe(pushed)
				+ " against a flat depth of " + describe(flat)
				+ "; a negative depth must push down by 40 screen pixels");
	}

	// This slice moves the tile surface and nothing else.
	//
	// Two halves, and both matter. The surface's own clip travels with it,
	// because a lift that left the clip behind would crop the top off what
	// it had just moved rather than move it. And everything that is not the
	// tile surface stays exactly where it was: the wireframe outline of the
	// same Sector, drawn outside the surface clip by the same pass, still
	// lands on the unlifted footprint. Thresholds, transit geometry,
	// objects, Agents and the selection highlight are all still on the
	// unlifted mapping, which is the declared intermediate state this
	// slice stops at.
	void checkOnlyTileSurfaceMoves(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Only the surface moves");

		auto const flat = recordSurface(fixture.world, fixture.behind);
		require(fixture.world->setSectorFloorDepth(fixture.front, 48).changed,
			"the fixture's depth write was refused");
		auto const lifted = recordSurface(fixture.world, fixture.behind);
		auto const expected = expectedUnlifted(*fixture.world->getSector(
			fixture.behind));

		// The surface's clip moved with the surface, so nothing was cropped:
		// the clip is the lifted footprint, not the one the Sector keeps.
		require(near(lifted.clipTop, expected.top - 48.0f)
				&& near(lifted.clipBottom, expected.bottom - 48.0f),
			"The lifted surface was clipped to " + std::to_string(lifted.clipTop)
				+ ".." + std::to_string(lifted.clipBottom) + " rather than its own "
				"lifted footprint of " + std::to_string(expected.top - 48.0f) + ".."
				+ std::to_string(expected.bottom - 48.0f) + "; a lift that crops the "
				"surface it moved has not moved it");

		// The wireframe outline is the same pass drawing something that is
		// not the tile surface. It stays where it was.
		float const outlineTop = wireframeOutlineTop(fixture.world, fixture.behind);
		require(outlineTop == expected.top,
			"The wireframe outline moved with the lift: it sits at "
				+ std::to_string(outlineTop) + " where the unlifted footprint gives "
				+ std::to_string(expected.top)
				+ "; only the tile surface moves in this slice");
	}
}

void render_smoke::registerFloorDepthLift(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepth/aDepthOf48LiftsTheLayerBehindBy48Pixels",
		isolated<checkDepthOf48LiftsTheLayerBehind> });
	checks.push_back({ "floorDepth/theLayerCarryingTheDepthDoesNotMove",
		isolated<checkTheLifterDoesNotMove> });
	checks.push_back({ "floorDepth/aFlatWorldRecordsTheStreamItRecordedBefore",
		isolated<checkFlatStreamIdentity> });
	checks.push_back({ "floorDepth/theLiftScalesWithZoom",
		isolated<checkLiftScalesWithZoom> });
	checks.push_back({ "floorDepth/renderWorldInstallsTheStaircaseForTheFrame",
		isolated<checkRenderWorldInstallsStaircase> });
	checks.push_back({ "floorDepth/theLiftIsRecomputedPerFrameNotCached",
		isolated<checkLiftRecomputedPerFrame> });
	checks.push_back({ "floorDepth/aNegativeDepthPushesTheLayerBehindDown",
		isolated<checkNegativeDepthPushesDown> });
	checks.push_back({ "floorDepth/onlyTheTileSurfaceMovesInThisSlice",
		isolated<checkOnlyTileSurfaceMoves> });
}
