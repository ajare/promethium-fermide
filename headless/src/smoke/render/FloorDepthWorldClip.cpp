// The editor's World clip and scroll range follow the lifted picture (#1109).
//
// The editor used to push one clip around the whole World draw computed from
// the World's declared extent with no floor lift in it. That is right for
// the grid, which is World furniture and does not shift, and wrong for
// Sectors, which do: a Room on the top Level of a lifted Layer came off
// on a straight horizontal line at the unlifted World edge, and the
// scrollbar - sized from the same unlifted extent - could not be dragged
// far enough to bring the cut content back.
//
// The fix widens the clip to the union of the declared box and that box
// moved by every Layer's lift, and grows the scroll range at both ends by
// the same slack. This file measures both halves off the real command
// stream, the way the lift checks next door do (#1101): nothing here is
// predicted from a formula the renderer was asked to reproduce.
//
// The shape of every check is three-way. The same lifted Room is recorded
// under three clips:
//
//   * the vast clip - nothing is cut, so it states the full count the
//     Room has a right to draw;
//   * the widened clip - what the editor pushes now, computed from the
//     shared slack the editor itself reads; it must equal the full count,
//     which is the ticket's headline: the lifted picture survives the
//     editor's clip whole;
//   * the unlifted clip - what the editor used to push; it must be less
//     than the full count. This guard is what stops the check passing
//     vacuously: if the fixture ever stops crossing the World edge, the
//     guard fails and says so, rather than the widened clip passing a
//     Room that was never in danger.
//
// Beyond the headline:
//
//   * the negative case is checked too, at the other edge - a pushed-down
//     Layer must not be cut at the World's bottom either, and the widened
//     box grows downward to cover it;
//   * the grid stays exactly on the unlifted World extent under the
//     widened clip (#1105): widening a clip admits more, it never moves
//     what was already inside, so World furniture cannot have shifted;
//   * the scroll range reaches both extremes of the shifted picture and
//     keeps its resting position: scrollY = 0 still means the unlifted
//     World bottom at the canvas bottom, so adding a depth never jumps
//     the author's view;
//   * a flat World lays out exactly as it did before the slack existed -
//     the same scroll maxima, the same bar flags, a zero minimum - because
//     every new term here is added, never reordered.
//
// The viewport is pinned vast rather than the usual 4096 square: the
// renderer's own viewport clip must never be the thing that cuts, or the
// measurement would say something about the pin rather than about the
// editor's rect. The only clip allowed to bind is the one under test.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldToScreen.h"
#include "WorldViewportZoom.h"

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

	int const kCellWidth = CORE_CELL_WIDTH_PIXELS;
	int const kCellHeight = CORE_LEVEL_HEIGHT_PIXELS;

	// The synthetic sheet: one full-cell tile under each surface name the
	// Sectors below ask for, so the quads measured are real SectorAtlas
	// triangles and not the missing-tile fallback.
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
	}

	// The viewport pinned vast, with the World's bottom line parked at the
	// viewport's centre rather than at its bottom edge. The mapping ties the
	// flip line to the viewport's bottom edge, so with no offset the World
	// bottom sits exactly on that edge and a Layer pushed down by a negative
	// depth spills past it - the renderer's own viewport clip would then be
	// the thing that cuts, and the measurement would say something about
	// the pin rather than about the clip under test. Parking the World in
	// the middle leaves room below for the push and above for the lift, so
	// the only clip allowed to bind is the one handed to the draw.
	void pinVastViewport(float zoom = 1.0f)
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = -1000000.0f;
		gUISettings.worldViewportY = -1000000.0f;
		gUISettings.worldViewportWidth = 2000000.0f;
		gUISettings.worldViewportHeight = 2000000.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 1000000.0f;
		gUISettings.worldZoom = zoom;
	}

	// The World's declared box in screen pixels, mapped exactly the way
	// the editor's unlifted box is: the viewport mapping with no floor
	// lift term in it (#1105).
	WorldDrawList::ClipRectangle unliftedWorldBox(
		core::World const& world, float zoom)
	{
		auto const m = pf::render::worldToScreenAt(gUISettings, zoom);
		return {
			{ m.toScreenX(0.0f), m.toScreenY(static_cast<float>(world.getLevelsHigh())) },
			{ m.toScreenX(static_cast<float>(world.getCellsWide())), m.toScreenY(0.0f) }
		};
	}

	// The clip the editor pushes now: the declared box widened by the
	// shared slack, computed from the same two calls the editor's
	// renderWorldWindow makes (#1109). Stated here rather than replayed
	// from memory so a drift between the two is this check's to catch.
	WorldDrawList::ClipRectangle widenedWorldBox(
		core::World const& world, float zoom)
	{
		auto box = unliftedWorldBox(world, zoom);
		auto const slack = pf::render::worldFloorLiftSlack(world, zoom);
		box.minimum.y -= slack.up;
		box.maximum.y += slack.down;
		return box;
	}

	struct SurfaceCount
	{
		int triangles{};
		float top{};
		float bottom{};
	};

	// Records the lifted Layer's tile surface through the real pass under
	// one clip. The view Layer is the measured Room's own, so the Solid
	// pass draws that Room and the stream's SectorAtlas triangles are
	// its own; the lifter sits on the Layer in front and is not drawn.
	SurfaceCount recordSurfaceUnderClip(
		std::shared_ptr<core::World> const& world,
		WorldDrawList::ClipRectangle const& clip)
	{
		gUISettings.visibleLayer = 1;
		WorldDrawList commands(clip);
		renderWorld(world, &commands);

		SurfaceCount count;
		count.top = 1e30f;
		count.bottom = -1e30f;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			for (auto const& p : triangle->positions)
			{
				count.top = std::min(count.top, p.y);
				count.bottom = std::max(count.bottom, p.y);
			}
			++count.triangles;
		}
		return count;
	}

	std::string describe(SurfaceCount const& count)
	{
		return "triangles " + std::to_string(count.triangles)
			+ ", y " + std::to_string(count.top) + ".."
			+ std::to_string(count.bottom);
	}

	// The fixture: a 12-Level World with two Layers. The lifter Room sits
	// on the Layer in front and carries the depth; the measured Room sits
	// on the Layer behind, at whichever World edge this check pushes it
	// past. No Doors anywhere, so no aperture recursion enters the pass
	// and the measured stream is one Room and nothing else.
	struct EdgeFixture
	{
		std::shared_ptr<core::World> world;
		uint32_t lifter{};
		uint32_t measured{};
	};

	EdgeFixture roomsAtWorldEdge(std::string const& name, bool measuredAtTop)
	{
		auto world = std::make_shared<core::World>(name, 24, 12);
		while (world->getLayerCount() < 2) world->addLayer();

		EdgeFixture fixture;
		fixture.world = std::move(world);
		// addRoom takes (name, Layer, y, x, wide, high) - y before x.
		fixture.lifter = fixture.world->addRoom("Lifter", 0, 2, 5, 3, 2);
		// Two Levels high either way: at the top, its upper Level reaches
		// the World's top Level, so a one-Level lift puts a whole row of
		// tiles above the declared edge; at the bottom, its lower Level is
		// the World's bottom Level, so a one-Level push puts a whole row
		// below it.
		auto const measuredY = measuredAtTop
			? static_cast<int>(fixture.world->getLevelsHigh()) - 2
			: 0;
		fixture.measured = fixture.world->addRoom("Measured", 1,
			measuredY, 8, 3, 2);
		return fixture;
	}

	// The ticket's headline at the top edge: a Room lifted above the
	// World's declared top must draw whole under the widened clip, where
	// the old unlifted clip cut its upper row off.
	void checkLiftedTopSurvivesTheWidenedClip(smoke::Context const&)
	{
		installFlatAtlas();
		pinVastViewport();
		auto const fixture = roomsAtWorldEdge("Lifted at the top", true);
		require(fixture.world->setSectorFloorDepth(fixture.lifter, kCellHeight).changed,
			"the fixture's depth write was refused");

		auto const vast = recordSurfaceUnderClip(fixture.world,
			{ { -10000000.0f, -10000000.0f }, { 10000000.0f, 10000000.0f } });
		require(vast.triangles > 0,
			"The measured Room drew no tile surface at all, so there is "
			"nothing here to check");

		auto const widened = recordSurfaceUnderClip(fixture.world,
			widenedWorldBox(*fixture.world, 1.0f));
		require(widened.triangles == vast.triangles,
			"A Room lifted a Level above the World's top drew "
				+ describe(widened) + " under the widened clip against a "
				"full count of " + describe(vast)
				+ "; the widened clip must let the whole lifted picture "
				"through");

		// The guard: the old clip must actually cut this Room, or the
		// fixture stopped crossing the edge and the check above is
		// passing on a Room that was never in danger.
		auto const unlifted = recordSurfaceUnderClip(fixture.world,
			unliftedWorldBox(*fixture.world, 1.0f));
		require(unlifted.triangles < vast.triangles,
			"The unlifted clip cut nothing (" + describe(unlifted)
				+ " against a full " + describe(vast) + "); the fixture no "
				"longer crosses the World edge and this check proves nothing");
	}

	// The same at the bottom edge with a negative depth: a pushed-down
	// Layer must not be cut at the World's bottom either, and the widened
	// box grows downward to cover it.
	void checkPushedBottomSurvivesTheWidenedClip(smoke::Context const&)
	{
		installFlatAtlas();
		pinVastViewport();
		auto const fixture = roomsAtWorldEdge("Pushed at the bottom", false);
		require(fixture.world->setSectorFloorDepth(fixture.lifter, -kCellHeight).changed,
			"the fixture's negative depth write was refused");

		auto const vast = recordSurfaceUnderClip(fixture.world,
			{ { -10000000.0f, -10000000.0f }, { 10000000.0f, 10000000.0f } });
		require(vast.triangles > 0,
			"The measured Room drew no tile surface at all, so there is "
			"nothing here to check");

		auto const widened = recordSurfaceUnderClip(fixture.world,
			widenedWorldBox(*fixture.world, 1.0f));
		require(widened.triangles == vast.triangles,
			"A Room pushed a Level below the World's bottom drew "
				+ describe(widened) + " under the widened clip against a "
				"full count of " + describe(vast)
				+ "; the widened clip must grow downward as well as upward");

		auto const unlifted = recordSurfaceUnderClip(fixture.world,
			unliftedWorldBox(*fixture.world, 1.0f));
		require(unlifted.triangles < vast.triangles,
			"The unlifted clip cut nothing (" + describe(unlifted)
				+ " against a full " + describe(vast) + "); the fixture no "
				"longer crosses the World edge and this check proves nothing");
	}

	// The grid stays exactly on the unlifted World extent under the
	// widened clip (#1105). A clip admits and trims; it never moves. If
	// the grid's box under the widened clip is the declared box exactly,
	// the widening cannot have shifted World furniture along with the
	// Sectors - the other half of the ticket's acceptance, measured
	// rather than asserted from the mechanism.
	void checkTheGridStaysAtTheUnliftedExtent(smoke::Context const&)
	{
		installFlatAtlas();
		pinVastViewport();
		auto const fixture = roomsAtWorldEdge("Grid under the widened clip", true);
		require(fixture.world->setSectorFloorDepth(fixture.lifter, kCellHeight).changed,
			"the fixture's depth write was refused");

		auto const expected = unliftedWorldBox(*fixture.world, 1.0f);

		gUISettings.visibleLayer = 1;
		gUISettings.renderGrid = true;
		WorldDrawList commands(widenedWorldBox(*fixture.world, 1.0f));
		renderWorld(fixture.world, &commands);
		gUISettings.renderGrid = false;

		auto const gridColour = packColour(128, 128, 127);
		float left = 1e30f, right = -1e30f, top = 1e30f, bottom = -1e30f;
		int lines = 0;
		for (auto const& command : commands.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (!line || line->colour != gridColour) continue;
			left = std::min({ left, line->from.x, line->to.x });
			right = std::max({ right, line->from.x, line->to.x });
			top = std::min({ top, line->from.y, line->to.y });
			bottom = std::max({ bottom, line->from.y, line->to.y });
			++lines;
		}
		require(lines > 0, "The grid contributed no lines to measure");
		require(left == expected.minimum.x && right == expected.maximum.x
				&& top == expected.minimum.y && bottom == expected.maximum.y,
			"The grid drew at x " + std::to_string(left) + ".."
				+ std::to_string(right) + ", y " + std::to_string(top) + ".."
				+ std::to_string(bottom) + " under the widened clip where the "
				"declared World box is x " + std::to_string(expected.minimum.x)
				+ ".." + std::to_string(expected.maximum.x) + ", y "
				+ std::to_string(expected.minimum.y) + ".."
				+ std::to_string(expected.maximum.y) + "; the grid is World "
				"furniture and the widening must leave it exactly where it is");
	}

	// The scroll range reaches both extremes of the shifted picture while
	// keeping its resting position. Measured against the layout's own
	// canvas so the assertion is about the range's shape, not about a
	// canvas size this check made up: the top of the picture is reachable
	// at the maximum, the pushed-down bottom at the minimum, and the
	// total range is exactly the picture's overflow.
	void checkTheScrollRangeReachesTheShiftedPicture(smoke::Context const&)
	{
		float const worldWidePx = 24.0f * kCellWidth;
		float const worldHighPx = 12.0f * kCellHeight;
		float const lift = static_cast<float>(kCellHeight);

		// Lifted up: the maximum grows by the lift so the lifted top
		// comes into view; the minimum stays at rest.
		auto const up = worldViewportLayout({ 1000.0f, 1000.0f },
			{ worldWidePx, worldHighPx }, 1.0f, 20.0f, 20.0f, lift, 0.0f);
		require(up.verticalScrollbar,
			"A picture taller than the canvas raised no vertical bar");
		require(up.scrollMaximum.y == worldHighPx + lift - up.canvasSize.y,
			"The scroll maximum left the lifted top out: max "
				+ std::to_string(up.scrollMaximum.y) + " against a picture "
				"top that needs " + std::to_string(worldHighPx + lift - up.canvasSize.y));
		require(up.scrollMinimum.y == 0.0f,
			"A lift up moved the resting position off zero: min "
				+ std::to_string(up.scrollMinimum.y)
				+ "; scrollY = 0 must still mean the unlifted bottom at the "
				"canvas bottom, or adding a depth jumps the author's view");

		// Pushed down: the minimum goes negative so the pushed part is
		// reachable below the resting position.
		auto const down = worldViewportLayout({ 1000.0f, 1000.0f },
			{ worldWidePx, worldHighPx }, 1.0f, 20.0f, 20.0f, 0.0f, lift);
		require(down.verticalScrollbar,
			"A picture pushed below its box raised no vertical bar");
		require(down.scrollMinimum.y == -lift,
			"The scroll minimum did not reach below rest for the pushed-down "
				"picture: min " + std::to_string(down.scrollMinimum.y)
				+ ", expected " + std::to_string(-lift));

		// The whole shifted picture overflows by exactly the range: every
		// pixel between the lifted top and the pushed bottom is between
		// the two ends of the bar.
		auto const both = worldViewportLayout({ 1000.0f, 1000.0f },
			{ worldWidePx, worldHighPx }, 1.0f, 20.0f, 20.0f, lift, lift);
		float const pictureHeight = worldHighPx + lift + lift;
		require(both.scrollMaximum.y - both.scrollMinimum.y
				== pictureHeight - both.canvasSize.y,
			"The scroll range is " + std::to_string(both.scrollMaximum.y - both.scrollMinimum.y)
				+ "px but the picture overflows the canvas by "
				+ std::to_string(pictureHeight - both.canvasSize.y)
				+ "px; some part of the shifted picture would be unreachable");
	}

	// A picture pushed down but short enough to fit still raises the bar:
	// at the resting position the pushed part sits below the canvas and
	// nothing else can bring it back, so the bar must exist even though
	// the bare picture comparison says it fits.
	void checkAPushedPictureThatFitsStillGetsABar(smoke::Context const&)
	{
		auto const layout = worldViewportLayout({ 1000.0f, 1000.0f },
			{ 24.0f * kCellWidth, 800.0f }, 1.0f, 20.0f, 20.0f,
			0.0f, static_cast<float>(kCellHeight));
		require(layout.verticalScrollbar,
			"A picture pushed 320px below its box raised no bar even though "
				"at rest the pushed part is off-canvas and unreachable");
		require(layout.scrollMinimum.y == -static_cast<float>(kCellHeight),
			"The bar does not reach the pushed-down part: min "
				+ std::to_string(layout.scrollMinimum.y));
	}

	// A flat World lays out exactly as it did before the slack existed:
	// the same maxima, the same bar flags, a zero minimum. Every new term
	// in the layout is added, never reordered, so a World with no depth
	// anywhere cannot feel the change.
	void checkAFlatWorldKeepsTheOldScrollArithmetic(smoke::Context const&)
	{
		auto const before = worldViewportLayout({ 1000.0f, 500.0f },
			{ 24.0f * kCellWidth, 12.0f * kCellHeight }, 1.0f, 20.0f, 20.0f);
		auto const after = worldViewportLayout({ 1000.0f, 500.0f },
			{ 24.0f * kCellWidth, 12.0f * kCellHeight }, 1.0f, 20.0f, 20.0f,
			0.0f, 0.0f);
		require(after.scrollMaximum.x == before.scrollMaximum.x
				&& after.scrollMaximum.y == before.scrollMaximum.y
				&& after.scrollMinimum.x == 0.0f && after.scrollMinimum.y == 0.0f
				&& after.horizontalScrollbar == before.horizontalScrollbar
				&& after.verticalScrollbar == before.verticalScrollbar
				&& after.canvasSize.x == before.canvasSize.x
				&& after.canvasSize.y == before.canvasSize.y,
			"Passing zero slack changed the flat layout: max "
				+ std::to_string(before.scrollMaximum.x) + ","
				+ std::to_string(before.scrollMaximum.y) + " became "
				+ std::to_string(after.scrollMaximum.x) + ","
				+ std::to_string(after.scrollMaximum.y));
	}

	// The slack itself: zero for a World with no depth, the lift at the
	// zoom it was asked at, and split by sign - a positive depth widens
	// the top only, a negative the bottom only.
	void checkTheSlackIsZeroForAFlatWorldAndScalesWithZoom(smoke::Context const&)
	{
		auto const flat = std::make_shared<core::World>("Flat slack", 24, 12);
		while (flat->getLayerCount() < 2) flat->addLayer();
		auto const zero = pf::render::worldFloorLiftSlack(*flat, 1.0f);
		require(zero.up == 0.0f && zero.down == 0.0f,
			"A World with no depth anywhere gave a slack of up "
				+ std::to_string(zero.up) + ", down " + std::to_string(zero.down)
				+ "; the clip and the range must not widen at all");

		auto const deep = std::make_shared<core::World>("Deep slack", 24, 12);
		while (deep->getLayerCount() < 2) deep->addLayer();
		auto const lifter = deep->addRoom("Lifter", 0, 2, 5, 3, 2);
		require(deep->setSectorFloorDepth(lifter, 48).changed,
			"the depth write was refused");

		auto const atOne = pf::render::worldFloorLiftSlack(*deep, 1.0f);
		require(atOne.up == 48.0f && atOne.down == 0.0f,
			"A depth of 48 gave slack up " + std::to_string(atOne.up)
				+ ", down " + std::to_string(atOne.down)
				+ "; a positive depth widens the top only");

		auto const atTwo = pf::render::worldFloorLiftSlack(*deep, 2.0f);
		require(atTwo.up == 96.0f,
			"At zoom 2 the slack was " + std::to_string(atTwo.up)
				+ " rather than double the zoom-1 slack; the lift is screen "
				"pixels and must scale with the view");

		require(deep->setSectorFloorDepth(lifter, -48).changed,
			"the negative depth write was refused");
		auto const pushed = pf::render::worldFloorLiftSlack(*deep, 1.0f);
		require(pushed.up == 0.0f && pushed.down == 48.0f,
			"A depth of -48 gave slack up " + std::to_string(pushed.up)
				+ ", down " + std::to_string(pushed.down)
				+ "; a negative depth widens the bottom only");
	}
}

void render_smoke::registerFloorDepthWorldClip(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthClip/liftedContentAtTheWorldTopSurvivesTheWidenedClip",
		isolated<checkLiftedTopSurvivesTheWidenedClip> });
	checks.push_back({ "floorDepthClip/pushedContentAtTheWorldBottomSurvivesTheWidenedClip",
		isolated<checkPushedBottomSurvivesTheWidenedClip> });
	checks.push_back({ "floorDepthClip/theGridStaysAtTheUnliftedExtentUnderTheWidenedClip",
		isolated<checkTheGridStaysAtTheUnliftedExtent> });
	checks.push_back({ "floorDepthClip/theScrollRangeReachesTheShiftedPicture",
		isolated<checkTheScrollRangeReachesTheShiftedPicture> });
	checks.push_back({ "floorDepthClip/aPushedPictureThatFitsStillGetsABar",
		isolated<checkAPushedPictureThatFitsStillGetsABar> });
	checks.push_back({ "floorDepthClip/aFlatWorldKeepsTheOldScrollArithmetic",
		isolated<checkAFlatWorldKeepsTheOldScrollArithmetic> });
	checks.push_back({ "floorDepthClip/theSlackIsZeroForAFlatWorldAndScalesWithZoom",
		isolated<checkTheSlackIsZeroForAFlatWorldAndScalesWithZoom> });
}
