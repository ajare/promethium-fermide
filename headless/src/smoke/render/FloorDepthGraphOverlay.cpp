// The graph overlay lifts with the Sectors it describes (#1110).
//
// The graph is a picture of the Layer being viewed: its vertices sit on
// Sector floors, its edges run between them, and its mount triangles mark
// where a Ladder or a Lift joins the floor. When a Sector behind runs deep
// and the Layer moves, every one of those marks moves with it or the
// overlay becomes a diagram of somewhere the author is not looking - dots
// floating off the floors they hang from by the whole lift of the Layer.
//
// `renderGraph` draws a whole Layer's Graph from outside `renderSector`, so
// the rule at the top of Render.cpp applies to it: install the lift of the
// Layer being drawn before transforming anything it draws. It did not,
// because it was the one helper that got missed.
//
// The shape of these checks matters more than usual, because the bug was
// invisible to the obvious test. The lift is ambient, and the renderer has
// two ways to ask for a Layer's number: the file-static helper that reads
// the staircase `renderWorld` installed and otherwise falls back to the
// published render World, and the shared helper asked of the World itself.
// In a headless pass that drives the Sector-level passes directly, a scope
// *is* installed and both answers are the same, so a check written that way
// passes against a still-broken editor. The editor never publishes a World
// of its own - `renderWorld` does, for the span of its own call - so by
// the time the editor calls `renderGraph` as a separate statement the
// staircase is unwound and the published World is gone, and the fallback
// answers zero for a Layer that is anything but flat.
//
// So every check here calls `renderGraph` the way the editor calls it:
// from outside the render pass, with the World handed in as a parameter and
// no scope of any kind installed around the call. Where a check needs the
// picture to compare against, it draws the World first and lets that draw
// return and unwind before the overlay is drawn, exactly as the editor's
// frame does.
//
// What is pinned beyond the headline:
//
//   * the whole overlay travels, not just the vertices. The comparison is
//     made command for command against the stream the same fixture drew at
//     zero depth, so an edge, a vertex dot or a mount triangle left behind
//     shows up as a mismatch rather than hiding in a check that only looked
//     at dots;
//   * the move is vertical only. A lift that drifted in x would be a
//     different bug and is asserted against directly;
//   * the lift is the Layer's own, read off the World handed in. Drawing
//     the World first and the overlay second must give the same overlay as
//     drawing the overlay alone - the overlay depends on the World it was
//     given, not on whichever frame last ran;
//   * a flat World draws the overlay exactly as it drew it before the lift
//     term existed, on float equality with the pre-#1100 mapping rather
//     than within a tolerance;
//   * the lift is screen pixels and scales with the zoom, and a negative
//     depth pushes the overlay down by the same magnitude;
//   * the front Layer is never lifted, so viewing it draws the overlay
//     unlifted no matter what depth is recorded behind.

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
#include "core/Graph.h"
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

	// One Level of depth. Big enough that a stale, unlifted overlay is a
	// whole Level off the floors it marks rather than near-miss close, and
	// exactly one Level makes the arithmetic in the diagnostics readable.
	int32_t const kDepth = CORE_LEVEL_HEIGHT_PIXELS;

	// The colour `renderGraph` paints a Ladder mount with. Naming it here
	// lets the mount-triangle check point at one command family instead of
	// counting everything the pass recorded.
	PackedColourValue const kMountColour = packColour(0, 192, 255);

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

	// The viewport, pinned. Origin and both offsets at zero, so a screen
	// coordinate is the mapping's answer and nothing else.
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
		gUISettings.renderGraph = true;
	}

	// The mapping as it stood before the floor lift term was added (#1100):
	// the viewport's flip and the scroll, and nothing else. Stated here so
	// the flat-overlay check has something from before to hold the pass to,
	// and so the negative control has a stale mapping to aim with.
	pf::render::WorldToScreen unliftedMapping()
	{
		return pf::render::worldToScreenAt(gUISettings, gUISettings.worldZoom);
	}

	// The mapping the lifted overlay is drawn by.
	pf::render::WorldToScreen viewMapping(core::World const& world,
		uint32_t viewLayer)
	{
		return pf::render::worldToScreenForLayer(gUISettings, world, viewLayer,
			gUISettings.worldZoom);
	}

	// Two Layers, one Room on each at the same footprint and no Door between
	// them, so nothing recurses. The front Room carries the depth; the back
	// Room is the Layer that gets viewed and whose graph gets drawn.
	struct Fixture
	{
		std::shared_ptr<core::World> world;
		uint32_t lifter{};
		uint32_t viewed{};
	};

	Fixture layeredRooms(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, 24, 12);
		while (world->getLayerCount() < 2) world->addLayer();
		Fixture fixture;
		fixture.world = std::move(world);
		fixture.lifter = fixture.world->addRoom("Lifter", 0, 2, 2, 3, 1);
		fixture.viewed = fixture.world->addRoom("Viewed", 1, 2, 2, 3, 2);
		// A lone Room with nothing in it puts no Vertex in the Graph - there
		// is no run of cells for one to join - so each Room gets a Marker to
		// stand on. Without one the front Layer's overlay would be empty and
		// the check that says it does not move would be measuring nothing.
		fixture.world->addSectorMarker(fixture.lifter, 0, 0.5f, "Front marker");
		fixture.world->addSectorMarker(fixture.viewed, 0, 2.5f, "Viewed marker");
		// A Ladder object on the viewed Room puts a LadderMount Edge in the
		// Graph whose two vertices both sit on the viewed Layer, which is
		// what makes `renderGraph` record a mount triangle for it out of the
		// mount branch of the edge loop rather than the inter-layer one.
		// Without it the overlay would be dots and lines only and the mount
		// triangles would go unmeasured.
		fixture.world->addSectorWalkway(fixture.viewed, 1, 1);
		fixture.world->addRoomLadder(fixture.viewed, 0, 1, { 0, true, false });
		fixture.world->finishBuild();
		require(fixture.world->getGraph() != nullptr,
			"The fixture World built no Graph, so the overlay has nothing to draw");
		return fixture;
	}

	// Every point the pass recorded, in the order it recorded it, with the
	// command kind carried along so a mismatch says what moved.
	struct Point
	{
		char kind{}; // 'L' line, 'T' triangle, 'X' text
		float x{};
		float y{};
	};

	std::vector<Point> recordedPositions(WorldDrawList const& list)
	{
		std::vector<Point> points;
		for (auto const& command : list.commands())
		{
			if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
			{
				points.push_back({ 'L', line->from.x, line->from.y });
				points.push_back({ 'L', line->to.x, line->to.y });
			}
			else if (auto const* triangle =
				std::get_if<WorldDrawList::Triangle>(&command))
			{
				for (auto const& p : triangle->positions)
					points.push_back({ 'T', p.x, p.y });
			}
			else if (auto const* text = std::get_if<WorldDrawList::Text>(&command))
			{
				points.push_back({ 'X', text->position.x, text->position.y });
			}
		}
		return points;
	}

	std::string describe(Point const& p)
	{
		std::string kind{ p.kind };
		return kind + " (" + std::to_string(p.x) + ", " + std::to_string(p.y) + ")";
	}

	// The overlay drawn for the viewed Layer, through the editor's own call
	// shape: no render pass in flight, no scope installed, the World handed
	// in as a parameter. This is the only way in here that distinguishes the
	// World asked from the frame left behind.
	WorldDrawList drawOverlay(Fixture const& fixture, uint32_t viewLayer)
	{
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderGraph(fixture.world->getGraph(), fixture.world, &commands);
		return commands;
	}

	// The whole overlay travels: every point the pass recorded sits exactly
	// the lift higher than the same pass recorded at zero depth, and nothing
	// drifted sideways.
	void checkTheWholeOverlayLiftsWithTheLayer(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("The overlay lifts");
		require(fixture.world->layerFloorOffsetPx(1) == 0,
			"The fixture Layer was not flat before the depth was written, so the "
			"baseline stream is not a zero-lift stream");

		auto const flat = recordedPositions(drawOverlay(fixture, 1));
		require(!flat.empty(),
			"The overlay recorded nothing at all, so there is no stream to move");

		require(fixture.world->setSectorFloorDepth(fixture.lifter, kDepth).changed,
			"the fixture's depth write was refused");
		auto const lift = pf::render::layerFloorLiftPx(*fixture.world, 1,
			gUISettings.worldZoom);
		require(lift == static_cast<float>(kDepth),
			"The viewed Layer's lift came back as " + std::to_string(lift)
				+ " rather than the " + std::to_string(kDepth)
				+ " the fixture wrote, so the checks below have nothing to bite on");

		auto const lifted = recordedPositions(drawOverlay(fixture, 1));
		require(lifted.size() == flat.size(),
			"The lifted overlay recorded " + std::to_string(lifted.size())
				+ " points against the flat overlay's " + std::to_string(flat.size())
				+ "; the depth changed what the pass drew, not only where");

		for (size_t i = 0; i < lifted.size(); ++i)
		{
			require(lifted[i].kind == flat[i].kind,
				"Command " + std::to_string(i) + " changed kind from "
					+ describe(flat[i]) + " to " + describe(lifted[i]));
			require(lifted[i].x == flat[i].x,
				"Point " + std::to_string(i) + " drifted in x from "
					+ describe(flat[i]) + " to " + describe(lifted[i])
					+ "; the floor lift is a vertical term and moves nothing sideways");
			require(near(lifted[i].y, flat[i].y - lift),
				"Point " + std::to_string(i) + " is at " + describe(lifted[i])
					+ " against the flat " + describe(flat[i]) + "; the whole overlay - "
					"vertices, edges and mount triangles alike - must sit "
					+ std::to_string(lift) + " pixels up the screen from there");
		}
	}

	// The mount triangles specifically. A vertex dot left behind is bad; a
	// mount triangle left behind is worse, because it is the mark that says
	// "a Ladder joins the floor here" and it is drawn from the edge loop
	// rather than the vertex loop, so it is a separate path through the
	// pass that has to lift on its own.
	void checkMountTrianglesLiftWithTheirVertices(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Mount triangles lift");

		gUISettings.visibleLayer = 1;
		WorldDrawList flatList({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderGraph(fixture.world->getGraph(), fixture.world, &flatList);

		require(fixture.world->setSectorFloorDepth(fixture.lifter, kDepth).changed,
			"the fixture's depth write was refused");

		WorldDrawList liftedList({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderGraph(fixture.world->getGraph(), fixture.world, &liftedList);

		auto mountApex = [](WorldDrawList const& list)
		{
			// The mount triangle's top point: the smallest y among the
			// triangles painted in the mount colour.
			float apex = 1e30f;
			int count = 0;
			for (auto const& command : list.commands())
			{
				auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
				if (!triangle || triangle->colour != kMountColour) continue;
				for (auto const& p : triangle->positions)
					apex = std::min(apex, p.y);
				++count;
			}
			return std::make_pair(apex, count);
		};

		auto const flat = mountApex(flatList);
		auto const lifted = mountApex(liftedList);
		require(flat.second > 0,
			"The fixture drew no mount triangle at all, so this check measured "
			"nothing; the Ladder object did not reach the Graph as a "
			"LadderMount Edge on the viewed Layer");
		require(lifted.second == flat.second,
			"The lifted overlay drew " + std::to_string(lifted.second)
				+ " mount triangles against the flat overlay's "
				+ std::to_string(flat.second));
		require(near(flat.first - lifted.first, static_cast<float>(kDepth)),
			"The mount triangle apex moved " + std::to_string(flat.first - lifted.first)
				+ " pixels up when the depth was written; it must move the whole "
				"lift of " + std::to_string(kDepth) + ", or the mark for where a "
				"Ladder joins the floor is floating off that floor");
	}

	// The trap, made explicit.
	//
	// The editor draws the World, `renderWorld` returns and unwinds both the
	// staircase and the published World, and only then is the overlay drawn.
	// A helper that asked the frame instead of the World would answer zero at
	// that moment. So: draw the World first, let it finish, then draw the
	// overlay, and require the overlay still lifted - and lifted to exactly
	// where the overlay drawn with no World draw before it landed, which is
	// where the picture the author is looking at says it should be.
	void checkTheOverlayLiftsAfterTheRenderPassHasUnwound(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Unwound frame");
		require(fixture.world->setSectorFloorDepth(fixture.lifter, kDepth).changed,
			"the fixture's depth write was refused");

		auto const alone = recordedPositions(drawOverlay(fixture, 1));
		require(!alone.empty(), "The overlay recorded nothing on its own");

		// The World draw installs its staircase and its published World and
		// takes both back on the way out, exactly as the editor's frame does.
		gUISettings.visibleLayer = 1;
		WorldDrawList worldCommands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderWorld(fixture.world, &worldCommands);
		require(!worldCommands.commands().empty(),
			"renderWorld drew nothing, so the frame under test never ran");

		auto const after = recordedPositions(drawOverlay(fixture, 1));
		require(after.size() == alone.size(),
			"The overlay drawn after a full World draw recorded "
				+ std::to_string(after.size()) + " points against "
				+ std::to_string(alone.size()) + " drawn alone");
		for (size_t i = 0; i < after.size(); ++i)
		{
			require(after[i].kind == alone[i].kind
				&& after[i].x == alone[i].x && after[i].y == alone[i].y,
				"The overlay drawn after the World draw put point "
					+ std::to_string(i) + " at " + describe(after[i])
					+ " rather than " + describe(alone[i])
					+ "; the overlay must read the World it was handed, not "
					"whichever frame last ran");
		}

		// And that shared landing spot is the lifted one, not the flat one
		// the unwound frame would have answered with. Take the same overlay
		// at zero depth: had the pass asked the frame instead of the World,
		// every point here would have landed on the flat stream.
		auto const lift = pf::render::layerFloorLiftPx(*fixture.world, 1,
			gUISettings.worldZoom);
		require(lift > 0.0f,
			"The viewed Layer's lift is zero, so this check cannot tell a "
			"lifted overlay from a stale one");
		require(fixture.world->setSectorFloorDepth(fixture.lifter, 0).changed,
			"the fixture's depth reset was refused");
		auto const flat = recordedPositions(drawOverlay(fixture, 1));
		require(flat.size() == after.size(),
			"The same overlay at zero depth recorded " + std::to_string(flat.size())
				+ " points against " + std::to_string(after.size()));
		for (size_t i = 0; i < after.size(); ++i)
			require(near(after[i].y, flat[i].y - lift),
				"Point " + describe(after[i]) + " is not the flat "
				+ describe(flat[i]) + " lifted by " + std::to_string(lift)
				+ "; a pass that asked the unwound frame rather than the World it "
				"was handed would have drawn exactly the flat point here");
	}

	// A flat World draws the overlay exactly where it drew it before the
	// lift term existed.
	//
	// The lift enters the same shared mapping every other drawer uses, so a
	// zero that arrived by a different route - a reassociation of the
	// terms, a rounding step ahead of it - would nudge every dot and every
	// edge in the editor by a hair while looking identical. These are
	// assertions on float equality with the pre-#1100 expression.
	void checkAFlatWorldDrawsTheOverlayExactlyAsBefore(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Flat overlay identity");
		require(fixture.world->layerFloorOffsetPx(1) == 0,
			"The fixture Layer is not flat, so this is not the case being pinned");
		require(pf::render::layerFloorLiftPx(*fixture.world, 1,
			gUISettings.worldZoom) == 0.0f,
			"A flat World gave the viewed Layer a non-zero lift");

		auto const points = recordedPositions(drawOverlay(fixture, 1));
		require(!points.empty(), "The overlay recorded nothing");

		auto const flat = unliftedMapping();
		auto const lifted = viewMapping(*fixture.world, 1);

		// Take each recorded screen point, read the world height the flat
		// mapping says it came from, and require the lifted mapping puts that
		// same world height on the very same pixel. The two mappings only
		// agree like this when the lift term contributes exactly zero: with a
		// lift of L the lifted mapping would land every one of these a full
		// L away from where the pass actually drew it.
		for (auto const& p : points)
		{
			require(near(lifted.toScreenY(flat.toWorldY(p.y)), p.y, 0.001f),
				"Point " + describe(p) + " is not where the lifted mapping puts "
				"the world height the flat mapping puts it on, so a lift term is "
				"in what should be a flat picture");
		}

		// And the mapping itself carries no lift on a flat World.
		require(lifted.floorLiftPx == 0.0f,
			"The viewed Layer's mapping carries a lift of "
				+ std::to_string(lifted.floorLiftPx) + " on a flat World");
	}

	// The lift is screen pixels at this zoom, not document pixels.
	//
	// The World stores the depth at 1x. An overlay that lifted by the
	// stored number whatever the zoom would sit further off its floors the
	// closer the author zoomed in - which is exactly where the overlay is
	// being used to aim.
	void checkTheOverlayLiftScalesWithZoom(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport(2.0f);
		auto const fixture = layeredRooms("Zoom-scaled overlay lift");
		auto const flat = recordedPositions(drawOverlay(fixture, 1));
		require(!flat.empty(), "The overlay recorded nothing at zoom 2");
		require(fixture.world->setSectorFloorDepth(fixture.lifter, kDepth).changed,
			"the fixture's depth write was refused");
		auto const lifted = recordedPositions(drawOverlay(fixture, 1));
		auto const lift = pf::render::layerFloorLiftPx(*fixture.world, 1, 2.0f);
		require(lift == static_cast<float>(kDepth) * 2.0f,
			"The Layer lift was " + std::to_string(lift) + " at zoom 2 against a "
				"stored depth of " + std::to_string(kDepth)
				+ "; it must double with the view");
		require(lifted.size() == flat.size(),
			"The depth changed how many points the overlay recorded at zoom 2");
		for (size_t i = 0; i < lifted.size(); ++i)
			require(near(lifted[i].y, flat[i].y - lift),
				"At zoom 2, point " + std::to_string(i) + " is at "
				+ describe(lifted[i]) + " against the flat " + describe(flat[i])
				+ "; the lift is screen pixels and must move the overlay by "
				+ std::to_string(lift) + " there, not by the stored "
				+ std::to_string(kDepth));
	}

	// A negative depth pushes the overlay down by the same magnitude it
	// would otherwise have lifted it up.
	void checkAPushedLayerPushesTheOverlayDown(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Pushed overlay");
		require(fixture.world->setSectorFloorDepth(fixture.lifter, -kDepth).changed,
			"the fixture's depth write was refused");
		auto const lift = pf::render::layerFloorLiftPx(*fixture.world, 1,
			gUISettings.worldZoom);
		require(lift == -static_cast<float>(kDepth),
			"A depth of " + std::to_string(-kDepth) + " gave a lift of "
				+ std::to_string(lift));

		auto const pushed = recordedPositions(drawOverlay(fixture, 1));
		require(fixture.world->setSectorFloorDepth(fixture.lifter, 0).changed,
			"the fixture's depth reset was refused");
		auto const flat = recordedPositions(drawOverlay(fixture, 1));
		require(pushed.size() == flat.size() && !flat.empty(),
			"The pushed and flat overlays recorded different numbers of points");
		for (size_t i = 0; i < pushed.size(); ++i)
			require(near(pushed[i].y, flat[i].y - lift),
				"Point " + describe(pushed[i]) + " is not "
				+ std::to_string(-lift) + " pixels below the flat "
				+ describe(flat[i]) + "; a negative depth must push the "
				"overlay down by the same magnitude");
	}

	// The front Layer is never lifted, so its overlay never moves, whatever
	// depth is recorded anywhere behind it.
	//
	// The lift is a fact about the Layer being viewed, not about the World
	// being deep. An overlay that lifted the front Layer too would detach
	// the dots from the front floors, which are the ones that never move.
	void checkTheFrontLayerOverlayIsNeverLifted(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Front overlay stays");
		require(fixture.world->setSectorFloorDepth(fixture.viewed, kDepth).changed,
			"the fixture's depth write was refused");
		require(fixture.world->layerFloorOffsetPx(0) == 0,
			"The front Layer reports a non-zero offset of "
				+ std::to_string(fixture.world->layerFloorOffsetPx(0)));

		auto const withDepthBehind = recordedPositions(drawOverlay(fixture, 0));
		require(!withDepthBehind.empty(),
			"The overlay drew nothing on the front Layer, so this check "
			"measured an empty stream");

		// A depth written on the back Layer moves that Layer and nothing
		// else, so the front overlay must be the same stream it was with
		// the World at rest.
		require(fixture.world->setSectorFloorDepth(fixture.viewed, 0).changed,
			"the fixture's depth reset was refused");
		auto const atRest = recordedPositions(drawOverlay(fixture, 0));
		require(withDepthBehind.size() == atRest.size(),
			"The front overlay changed size with a depth write");
		for (size_t i = 0; i < withDepthBehind.size(); ++i)
			require(withDepthBehind[i].x == atRest[i].x
				&& withDepthBehind[i].y == atRest[i].y,
				"The front Layer's overlay moved from " + describe(atRest[i])
					+ " to " + describe(withDepthBehind[i]) + "; the front Layer "
					"draws at zero lift and its overlay must stay on its floors");
	}
}

void render_smoke::registerFloorDepthGraphOverlay(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthGraph/theWholeOverlayLiftsWithTheLayer",
		isolated<checkTheWholeOverlayLiftsWithTheLayer> });
	checks.push_back({ "floorDepthGraph/mountTrianglesLiftWithTheirVertices",
		isolated<checkMountTrianglesLiftWithTheirVertices> });
	checks.push_back({ "floorDepthGraph/theOverlayLiftsAfterTheRenderPassHasUnwound",
		isolated<checkTheOverlayLiftsAfterTheRenderPassHasUnwound> });
	checks.push_back({ "floorDepthGraph/aFlatWorldDrawsTheOverlayExactlyAsBefore",
		isolated<checkAFlatWorldDrawsTheOverlayExactlyAsBefore> });
	checks.push_back({ "floorDepthGraph/theOverlayLiftScalesWithZoom",
		isolated<checkTheOverlayLiftScalesWithZoom> });
	checks.push_back({ "floorDepthGraph/aPushedLayerPushesTheOverlayDown",
		isolated<checkAPushedLayerPushesTheOverlayDown> });
	checks.push_back({ "floorDepthGraph/theFrontLayerOverlayIsNeverLifted",
		isolated<checkTheFrontLayerOverlayIsNeverLifted> });
}
