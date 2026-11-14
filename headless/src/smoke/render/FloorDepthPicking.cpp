// The editor stays aimed at what it draws (#1105).
//
// A Sector's floor depth lifts the Layers behind it (#1101), and the
// renderer adds that lift on the way out. The editor converts between the
// screen and that picture on the way in - every click, drag, resize,
// surface paint and Agent drop. If the inverse does not take the same
// number off, the cursor lands on the Layer that used to be there: a Room
// selected behind the author's hand rather than under it, a drag that
// writes coordinates offset by the very shift the author is looking at,
// a variant painted onto the cell the picture says and the World says
// nothing of.
//
// Every assertion here is on the command vector the real render pass
// recorded, and on the inverse of the mapping the editor is told to use.
// The two are met head-on: the picture is drawn, its drawn box is measured,
// the measured screen point is inverted, and what comes back must be the
// Sector that was drawn there. Nothing is predicted from a formula the
// picker was asked to reproduce.
//
// The things worth pinning beyond the headline:
//
//   * the check must bite. With the lift in place, the *unlifted* inverse
//     of the same screen point is required to land somewhere else - one
//     Level away, off the Sector. Without that negative control a check
//     that "picks the right Sector" would pass on a World where nothing
//     moves at all;
//   * with every depth at zero the inverse is bit-identical to the mapping
//     from before the lift term existed (#1100), not merely close. A zero
//     that entered by a different route - a reassociation of the terms, a
//     rounding step ahead of it - would perturb every pick in the editor
//     by a hair while looking identical;
//   * the pick is zoom-invariant. The World stores document-space pixels
//     at 1x and the lift is a screen-space term; a picker that scaled the
//     depth differently from the drawer would land further off the deeper
//     the author zoomed in, which is exactly where precision is wanted;
//   * the lift belongs to the Layer being viewed, not to whatever Sector
//     the cursor happens to be over. Layer 0 is never lifted, and viewing
//     it must pick unlifted no matter what depth is recorded behind;
//   * the view Layer is clamped into the World the same way the draw pass
//     clamps it, or the two read the offset of different Layers at the
//     ends of the range;
//   * the grid and the player transport selector are world-space and do
//     not move with the Layer. They are the two things a lift that leaked
//     past the Sector pass would take with it, and a grid that slides under
//     a stationary picture is worse than no grid at all.
//
// The atlas is a synthetic sheet with one full-cell tile per surface name,
// installed the way the render stack installs it, so the boxes measured
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
#include "WorldViewportZoom.h"

#include "core/Defines.h"
#include "core/DoorSectorObject.h"
#include "core/Lift.h"
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

	// The lift this fixture puts on Layer 1 is exactly one Level. That is
	// the largest depth the World accepts and it makes the negative control
	// exact: a cursor aimed by the stale, unlifted mapping lands exactly
	// one Level away from the Sector the picture shows, which is off the
	// Sector entirely rather than merely near its edge.
	int32_t const kDepth = CORE_LEVEL_HEIGHT_PIXELS;

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
	// the flat-pick check has something from before to hold the picker to,
	// and so the negative control has a stale mapping to aim with.
	pf::render::WorldToScreen unliftedMapping()
	{
		return pf::render::worldToScreenAt(gUISettings, gUISettings.worldZoom);
	}

	// The mapping the editor is told to aim with: the viewport lifted by
	// the Layer being viewed (#1105).
	pf::render::WorldToScreen viewMapping(core::World const& world,
		uint32_t viewLayer)
	{
		return pf::render::worldToScreenForLayer(gUISettings, world, viewLayer,
			gUISettings.worldZoom);
	}

	// The World point a screen point inverts to, stated on the two scalar
	// halves so a check does not build the render module's own point type
	// at every call.
	core::Vector2 invert(pf::render::WorldToScreen const& mapping,
		core::Vector2 screen)
	{
		return { mapping.toWorldX(screen.x), mapping.toWorldY(screen.y) };
	}

	float screenY(pf::render::WorldToScreen const& mapping, float worldY)
	{
		return mapping.toScreenY(worldY);
	}

	// Two Layers, one Room on each at the same footprint, no Doors between
	// them so no aperture recursion enters the pass. The front Room is the
	// one that carries the depth; the back Room is the picture measured.
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
		// One Level tall. The lift is one Level, so the stale mapping's
		// answer is one Level off - off the Sector rather than merely near
		// its far edge, which is what makes the negative control bite.
		fixture.front = fixture.world->addRoom("Front", 0, 2, 2, 3, 1);
		fixture.behind = fixture.world->addRoom("Behind", 1, 2, 2, 3, 1);
		return fixture;
	}

	// The screen box the whole-World pass laid down for the Layer being
	// viewed. Measured from the recorded `SectorAtlas` triangles, so it is
	// where the picture actually is and not where a formula says it should
	// be.
	struct ScreenBox
	{
		float left{};
		float right{};
		float top{};
		float bottom{};
		int triangles{};

		core::Vector2 centre() const { return { (left + right) * .5f, (top + bottom) * .5f }; }
	};

	std::string describe(ScreenBox const& box)
	{
		return "x " + std::to_string(box.left) + ".." + std::to_string(box.right)
			+ ", y " + std::to_string(box.top) + ".." + std::to_string(box.bottom)
			+ ", triangles " + std::to_string(box.triangles);
	}

	ScreenBox drawnLayerBox(std::shared_ptr<core::World> const& world,
		uint32_t viewLayer)
	{
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderWorld(world, &commands);

		ScreenBox box;
		box.left = box.top = 1e30f;
		box.right = box.bottom = -1e30f;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			for (auto const& p : triangle->positions)
			{
				box.left = std::min(box.left, p.x);
				box.right = std::max(box.right, p.x);
				box.top = std::min(box.top, p.y);
				box.bottom = std::max(box.bottom, p.y);
			}
			++box.triangles;
		}
		require(box.triangles > 0,
			"renderWorld drew no SectorAtlas triangles on Layer "
				+ std::to_string(viewLayer) + ", so there is no picture to aim at");
		return box;
	}

	// The authored World-space bounds of one Sector, as a box of world
	// coordinates.
	struct WorldBox
	{
		float left{};
		float right{};
		float bottom{};
		float top{};
	};

	WorldBox authoredBounds(core::World const& world, uint32_t sectorIndex)
	{
		auto const sector = world.getSector(sectorIndex);
		require(sector != nullptr, "The fixture Sector could not be found");
		core::Vector2 lo{}, hi{};
		sector->getBounds(lo, hi);
		return { lo.x, hi.x, lo.y, hi.y };
	}

	bool contains(WorldBox const& box, core::Vector2 const& point)
	{
		return point.x >= box.left && point.x <= box.right
			&& point.y >= box.bottom && point.y <= box.top;
	}

	// The headline, in both directions: the picture is drawn lifted, and
	// every point inside the drawn box inverts back into the Sector that
	// was drawn there.
	//
	// Sampled across the box rather than at the centre alone. A mapping that
	// was off by a fraction of a Level would still return the centre
	// Sector and would let the author paint one row of cells onto the wrong
	// place; nine samples across the three cells the Room spans cannot all
	// be landed by an offset that is wrong.
	void checkTheCursorLandsOnTheSectorTheShiftedPictureShows(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Aim at the shifted picture");
		require(fixture.world->setSectorFloorDepth(fixture.front, kDepth).changed,
			"the fixture's depth write was refused");

		auto const drawn = drawnLayerBox(fixture.world, 1);
		auto const authored = authoredBounds(*fixture.world, fixture.behind);
		auto const mapping = viewMapping(*fixture.world, 1);

		// The picture really is lifted, or this check is measuring a flat
		// World and would pass without the fix.
		auto const flat = unliftedMapping();
		require(near(drawn.top, screenY(flat, authored.top) - static_cast<float>(kDepth)),
			"The Layer-1 picture was not lifted by the recorded depth, so there "
			"is no shift here to aim against: drawn box " + describe(drawn));

		int samples = 0;
		for (int ix = 1; ix <= 3; ++ix)
		{
			for (int iy = 1; iy <= 3; ++iy)
			{
				core::Vector2 screen{
					drawn.left + (drawn.right - drawn.left) * (ix / 4.0f),
					drawn.top + (drawn.bottom - drawn.top) * (iy / 4.0f) };
				auto const picked = invert(mapping, screen);
				++samples;
				require(contains(authored, picked),
					"A cursor at screen (" + std::to_string(screen.x) + ", "
						+ std::to_string(screen.y) + ") inverts to World ("
						+ std::to_string(picked.x) + ", " + std::to_string(picked.y)
						+ "), outside the Room the picture shows there (x "
						+ std::to_string(authored.left) + ".."
						+ std::to_string(authored.right) + ", y "
						+ std::to_string(authored.bottom) + ".."
						+ std::to_string(authored.top) + ")");
				auto const sector = fixture.world->getSectorAtPosition(1, picked.x,
					picked.y);
				require(sector && sector->getIndex() == fixture.behind,
					"A cursor at screen (" + std::to_string(screen.x) + ", "
						+ std::to_string(screen.y) + ") picks Sector "
						+ (sector ? std::to_string(sector->getIndex()) : "nothing")
						+ " where the shifted picture shows Room "
						+ std::to_string(fixture.behind)
						+ "; a click, a drag or a surface paint aimed here would "
						"act on the wrong ground");
			}
		}
		require(samples == 9, "The sample grid did not run");
	}

	// The negative control: the stale mapping must land somewhere else.
	//
	// Without this the check above could be satisfied by a World that never
	// moved, and the fix could be absent while every assertion still passed.
	// The depth is one whole Level, so the stale inverse is one whole Level
	// off the Room - not near its edge, but off it entirely.
	void checkTheUnliftedMappingLandsOffTheShiftedSector(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("The stale mapping misses");
		require(fixture.world->setSectorFloorDepth(fixture.front, kDepth).changed,
			"the fixture's depth write was refused");

		auto const drawn = drawnLayerBox(fixture.world, 1);
		auto const authored = authoredBounds(*fixture.world, fixture.behind);
		auto const screen = drawn.centre();

		auto const aimed = invert(viewMapping(*fixture.world, 1), screen);
		auto const stale = invert(unliftedMapping(), screen);

		require(contains(authored, aimed),
			"The lifted mapping did not land on the Room at its drawn centre");
		require(!contains(authored, stale),
			"The unlifted mapping landed at World (" + std::to_string(stale.x)
				+ ", " + std::to_string(stale.y) + "), still inside the Room at "
				"the drawn centre; the fixture does not separate the two mappings, "
				"so the checks above would pass with the fix absent");
		require(near(stale.y - aimed.y, static_cast<float>(kDepth) / kCellHeight),
			"The stale inverse is " + std::to_string(stale.y - aimed.y)
				+ " Levels off rather than the one Level the recorded depth is");

		auto const staleSector = fixture.world->getSectorAtPosition(1, stale.x,
			stale.y);
		require(!staleSector || staleSector->getIndex() != fixture.behind,
			"The stale mapping picked Sector "
				+ (staleSector ? std::to_string(staleSector->getIndex()) : "nothing")
				+ " at (" + std::to_string(stale.x) + ", " + std::to_string(stale.y)
				+ ") where the lifted mapping picked (" + std::to_string(aimed.x)
				+ ", " + std::to_string(aimed.y) + "); nothing here distinguishes "
				"the two mappings, so the checks above would pass with the fix "
				"absent");
	}

	// With every depth at zero the picker is the picker that was there
	// before the lift term existed - bit for bit, not within a tolerance.
	//
	// The lift enters the shared mapping, so a zero that came in by a
	// different route would perturb every pick in the editor by a hair
	// while looking identical on screen. A drag that lands a third of a
	// pixel off is not visible; a resize handle whose grab point is
	// consistently offset is.
	void checkAFlatWorldPicksExactlyWhereItPickedBefore(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Flat pick identity");
		require(fixture.world->layerFloorOffsetPx(1) == 0,
			"The fixture World is not flat, so this is not the case being pinned");

		auto const drawn = drawnLayerBox(fixture.world, 1);
		auto const mapping = viewMapping(*fixture.world, 1);
		require(mapping.floorLiftPx == 0.0f,
			"A flat World gave the view a non-zero lift of "
				+ std::to_string(mapping.floorLiftPx));

		for (int ix = 0; ix <= 4; ++ix)
		{
			for (int iy = 0; iy <= 4; ++iy)
			{
				core::Vector2 screen{
					drawn.left + (drawn.right - drawn.left) * (ix / 4.0f),
					drawn.top + (drawn.bottom - drawn.top) * (iy / 4.0f) };
				auto const now = invert(mapping, screen);
				auto const before = invert(unliftedMapping(), screen);
				require(now.x == before.x && now.y == before.y,
					"With every depth at zero the lifted inverse gave ("
						+ std::to_string(now.x) + ", " + std::to_string(now.y)
						+ ") where the mapping from before the lift term gave ("
						+ std::to_string(before.x) + ", " + std::to_string(before.y)
						+ "); a zero lift must be bit-identical, not merely close");
				require(fixture.world->getSectorAtPosition(1, now.x, now.y)
						== fixture.world->getSectorAtPosition(1, before.x, before.y),
					"A flat World picked a different Sector through the lifted "
					"mapping than through the mapping from before the lift term");
			}
		}
	}

	// The pick does not drift with the zoom.
	//
	// The World stores document-space pixels at 1x and the lift is a
	// screen-space term. If the picker scaled the depth differently from
	// the drawer, the error would grow with the zoom - worst exactly where
	// the author is looking closest and needs the pick to be right.
	void checkThePickIsZoomInvariant(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport(1.0f);
		auto const fixture = layeredRooms("Zoom invariant pick");
		require(fixture.world->setSectorFloorDepth(fixture.front, kDepth).changed,
			"the fixture's depth write was refused");
		auto const authored = authoredBounds(*fixture.world, fixture.behind);

		core::Vector2 atOne{};
		for (bool first : { true, false })
		{
			float const zoom = first ? 1.0f : 2.0f;
			pinViewport(zoom);
			auto const drawn = drawnLayerBox(fixture.world, 1);
			auto const picked = invert(viewMapping(*fixture.world, 1), drawn.centre());
			require(contains(authored, picked),
				"At zoom " + std::to_string(zoom) + " the drawn centre inverts to ("
					+ std::to_string(picked.x) + ", " + std::to_string(picked.y)
					+ "), outside the Room the picture shows");
			if (first) atOne = picked;
			else
			{
				require(near(picked.x, atOne.x) && near(picked.y, atOne.y),
					"The same point of the picture picks (" + std::to_string(picked.x)
						+ ", " + std::to_string(picked.y) + ") at 2x and ("
						+ std::to_string(atOne.x) + ", " + std::to_string(atOne.y)
						+ ") at 1x; the lift must scale with the view on both sides");
			}
		}

		// The lift itself is stated in screen pixels at the zoom in use.
		pinViewport(2.0f);
		require(viewMapping(*fixture.world, 1).floorLiftPx
				== static_cast<float>(kDepth) * 2.0f,
			"At 2x the view lift was "
				+ std::to_string(viewMapping(*fixture.world, 1).floorLiftPx)
				+ "px rather than the " + std::to_string(kDepth * 2)
				+ "px the drawer adds; an unscaled lift is half the lift at 2x");
	}

	// The lift belongs to the Layer being viewed, not to whatever the cursor
	// is over. Layer 0 is drawn where it was authored, so viewing it picks
	// unlifted no matter what depth is recorded in front of or behind it.
	void checkTheLiftBelongsToTheViewedLayer(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("The viewed Layer owns the lift");
		require(fixture.world->setSectorFloorDepth(fixture.front, kDepth).changed,
			"the fixture's depth write was refused");

		require(viewMapping(*fixture.world, 0).floorLiftPx == 0.0f,
			"Viewing Layer 0 carried a lift of "
				+ std::to_string(viewMapping(*fixture.world, 0).floorLiftPx)
				+ "px; the front Layer draws where it was authored");
		require(viewMapping(*fixture.world, 1).floorLiftPx
				== static_cast<float>(kDepth),
			"Viewing Layer 1 carried a lift of "
				+ std::to_string(viewMapping(*fixture.world, 1).floorLiftPx)
				+ "px rather than the " + std::to_string(kDepth)
				+ "px recorded in front of it");

		// And the pick follows: the front Room's drawn box inverts into the
		// front Room, not into a shifted version of it.
		auto const drawn = drawnLayerBox(fixture.world, 0);
		auto const authored = authoredBounds(*fixture.world, fixture.front);
		require(near(drawn.top, screenY(unliftedMapping(), authored.top))
				&& near(drawn.bottom, screenY(unliftedMapping(), authored.bottom)),
			"Layer 0's picture moved with a depth recorded on it: "
				+ describe(drawn));
		auto const picked = invert(viewMapping(*fixture.world, 0), drawn.centre());
		require(contains(authored, picked),
			"Viewing Layer 0, the drawn centre inverts to ("
				+ std::to_string(picked.x) + ", " + std::to_string(picked.y)
				+ "), outside the front Room");
		auto const sector = fixture.world->getSectorAtPosition(0, picked.x, picked.y);
		require(sector && sector->getIndex() == fixture.front,
			"Viewing Layer 0, the drawn centre picks Sector "
				+ (sector ? std::to_string(sector->getIndex()) : "nothing")
				+ " rather than the front Room");
	}

	// The view Layer is clamped into the World the same way the draw pass
	// clamps it. The setting is an `int` that can name no Layer at all; if
	// the picker clamped differently, the two would read the offset of
	// different Layers at the ends of the range and the picture and the
	// cursor would disagree by a whole Layer's lift.
	void checkTheViewLayerIsClampedIntoTheWorld(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("Clamped view Layer");
		auto const layerCount = fixture.world->getLayerCount();
		require(layerCount >= 2, "The fixture needs at least two Layers");

		require(pf::render::viewLayerFor(*fixture.world, -1) == 0,
			"A view Layer of -1 was not clamped to the front Layer");
		require(pf::render::viewLayerFor(*fixture.world, -99) == 0,
			"A view Layer far below the range was not clamped to the front Layer");
		require(pf::render::viewLayerFor(*fixture.world, 0) == 0,
			"The front Layer was clamped off itself");
		require(pf::render::viewLayerFor(*fixture.world,
				static_cast<int>(layerCount) - 1) == layerCount - 1,
			"The last Layer in the World was clamped off itself");
		require(pf::render::viewLayerFor(*fixture.world,
				static_cast<int>(layerCount) + 7) == layerCount - 1,
			"A view Layer past the last Layer was not clamped to the last Layer");

		// The clamped Layer is the Layer whose lift the mapping carries, so
		// an out-of-range setting cannot pick with a Layer that does not
		// exist.
		require(viewMapping(*fixture.world,
				pf::render::viewLayerFor(*fixture.world, static_cast<int>(layerCount) + 7))
				.floorLiftPx
				== viewMapping(*fixture.world, layerCount - 1).floorLiftPx,
			"An out-of-range view Layer carried a lift that the clamped Layer "
			"does not, so the cursor would be aimed by a Layer the draw pass "
			"never drew");
	}

	// The grid is world-space and does not move with the Layer.
	//
	// The grid is the author's ruler against the World, not against one
	// Layer's picture. A lift that leaked past the Sector pass would slide
	// the grid out from under a stationary picture, which is worse than no
	// grid: every measurement the author takes off it would be wrong by
	// the lift. Measured by rendering with the grid on, flat and lifted,
	// and requiring the recorded lines to be the same numbers.
	std::vector<std::pair<float, float>> recordedGridLines(
		std::shared_ptr<core::World> const& world, uint32_t viewLayer)
	{
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		gUISettings.renderGrid = true;
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderWorld(world, &commands);

		PackedColourValue const gridColour = packColour(128, 128, 127);
		std::vector<std::pair<float, float>> lines;
		for (auto const& command : commands.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (!line || line->colour != gridColour) continue;
			lines.emplace_back(line->from.y, line->to.y);
		}
		gUISettings.renderGrid = false;
		require(!lines.empty(),
			"The grid contributed no lines to measure, so this check would pass "
			"on an empty comparison");
		return lines;
	}

	void checkTheGridDoesNotMoveWithTheLayer(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const fixture = layeredRooms("The grid stays put");

		auto const flat = recordedGridLines(fixture.world, 1);
		// The picture before the depth is written, so the comparison below
		// is against a frame that really had not moved.
		auto const flatPicture = drawnLayerBox(fixture.world, 1);
		require(fixture.world->setSectorFloorDepth(fixture.front, kDepth).changed,
			"the fixture's depth write was refused");
		auto const liftedPicture = drawnLayerBox(fixture.world, 1);
		require(near(liftedPicture.top, flatPicture.top - static_cast<float>(kDepth)),
			"The Layer-1 picture did not lift between the two renders - "
			+ describe(flatPicture) + " against " + describe(liftedPicture)
			+ " - so the grid comparison below is not testing anything");

		auto const lifted = recordedGridLines(fixture.world, 1);
		require(lifted.size() == flat.size(),
			"The grid drew " + std::to_string(lifted.size()) + " lines with the "
			"Layer lifted and " + std::to_string(flat.size()) + " flat");
		for (size_t i = 0; i < std::min(lifted.size(), flat.size()); ++i)
		{
			require(lifted[i].first == flat[i].first
					&& lifted[i].second == flat[i].second,
				"Grid line " + std::to_string(i) + " moved with the Layer: ("
					+ std::to_string(flat[i].first) + " -> "
					+ std::to_string(flat[i].second) + ") became ("
					+ std::to_string(lifted[i].first) + " -> "
					+ std::to_string(lifted[i].second) + "); the grid is "
					"world-space and must not shift");
		}
	}

	// The player transport selector is world-space and does not move with
	// the Layer.
	//
	// The selector is drawn at the transport's real World position for the
	// boarding player. It is not part of any Layer's picture, so a Layer
	// lifting must leave it exactly where it was. Measured by boarding a
	// real lift with a real player-controlled Agent, rendering with the
	// selector up, and requiring the selector's own background rects to be
	// the same screen rectangles before and after a depth is recorded in
	// front of it.
	struct SelectorBox
	{
		float left{};
		float right{};
		float top{};
		float bottom{};
		int rects{};
	};

	SelectorBox drawnSelector(std::shared_ptr<core::World> const& world,
		uint32_t viewLayer)
	{
		gUISettings.visibleLayer = static_cast<int>(viewLayer);
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		renderWorld(world, &commands);

		// The selector's Stop plates are the only thing drawn with its
		// translucent background colour.
		constexpr PackedColourValue kSelectorBackground = 0xe0282020u;
		SelectorBox box;
		box.left = box.top = 1e30f;
		box.right = box.bottom = -1e30f;
		for (auto const& command : commands.commands())
		{
			// The selector's Stop plates are the only thing drawn with its
			// translucent background colour, and a filled rect reaches the
			// recorded stream as triangles.
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle || triangle->colour != kSelectorBackground) continue;
			for (auto const& p : triangle->positions)
			{
				box.left = std::min(box.left, p.x);
				box.right = std::max(box.right, p.x);
				box.top = std::min(box.top, p.y);
				box.bottom = std::max(box.bottom, p.y);
			}
			++box.rects;
		}
		return box;
	}

	std::string describe(SelectorBox const& box)
	{
		return "x " + std::to_string(box.left) + ".." + std::to_string(box.right)
			+ ", y " + std::to_string(box.top) + ".." + std::to_string(box.bottom)
			+ ", rects " + std::to_string(box.rects);
	}

	// A player-controlled Agent boarded onto a real lift, so the World has
	// a real transport selector up.
	std::shared_ptr<core::World> boardedWorld(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, 20, 6);
		auto const lower = world->addRoom("Origin", 0, 0, 0, 6, 1);
		world->addRoom("Middle", 0, 2, 0, 6, 1);
		world->addRoom("Top", 0, 4, 0, 6, 1);
		// A Room on the Layer the selector will be read against, so the
		// check can show that Layer's picture really moving while the
		// selector does not.
		world->addRoom("Behind", 1, 2, 8, 2, 1);
		core::World::CreateLiftOptions options;
		options.cellsWide = 2;
		options.stopOffsets = { 0, 2, 4 };
		options.minimumDwellSeconds = 5.0f;
		options.maximumBoardingSeconds = 20.0f;
		auto const created = world->addLift(1, 0, 2, options);
		auto const& firstDoor = created.doors.front();
		auto const door = std::dynamic_pointer_cast<core::DoorSectorObject>(
			firstDoor.door.sector->getObject(firstDoor.door.index))->getDoor();
		auto const point = world->lookupInteractionPoint(
			firstDoor.controls[0].interactionPoint).entity;
		world->finishBuild();
		world->pauseSimulation();
		auto const id = world->createAgent("Player", lower, 0,
			point->getPosition().x - world->getSector(lower)->getPosition().x);
		require(world->setAgentBehaviourAssignment(id, core::PlayerControlledBehaviour, 1, {}),
			"Cannot assign the Player behaviour");
		auto const actor = world->lookupAgent(id).entity;
		require(world->resumeSimulation(), "Cannot resume the simulation");
		auto const step = core::World::getFixedTimestep();
		auto const tick = [&](int frames)
			{ for (int i = 0; i < frames; ++i) world->update(step); };

		world->requestPlayerDoorAction();
		tick(60);
		float const centre = door->getPosition().x + door->getSize().x * .5f;
		int const direction = actor->getGlobalPosition().x < centre ? 1 : -1;
		world->setPlayerMovementDirection(direction);
		for (int i = 0; i < 600 && direction * (centre - actor->getGlobalPosition().x) > .05f; ++i)
			world->update(step);
		world->setPlayerMovementDirection(0);
		for (int i = 0; i < 600 && !door->isOpen(); ++i) world->update(step);
		require(door->isOpen(),
			"The lift Door never opened, so the Player cannot board to raise a "
			"selector");
		// The lift threshold is a crawl-through, so the Player has to be
		// crouching for the board to be admitted - the same posture the
		// live game requires, and without it the request does nothing.
		world->setPlayerCrouch(true);
		world->setPlayerMovementDirection(direction);
		tick(2);
		world->requestPlayerDoorAction();
		tick(2);
		world->setPlayerMovementDirection(0);
		for (int i = 0; i < 600 && actor->getSector()->getIndex() != created.lift.sector->getIndex(); ++i)
			world->update(step);
		world->setPlayerCrouch(false);
		// The simulation is left running on purpose: the selector the World
		// raises is a live boarding affordance and a paused World reports no
		// selector at all. The check below renders without ticking, so the
		// boarding window stays open across every frame it measures.
		require(actor->getSector()->getIndex() == created.lift.sector->getIndex(),
			"The Player never boarded the lift (still in Sector "
				+ std::to_string(actor->getSector()->getIndex()) + ")");
		require(world->getPlayerTransportSelector().has_value(),
			"The Player is aboard but the World raised no transport selector");
		return world;
	}

	void checkThePlayerTransportSelectorDoesNotMoveWithTheLayer(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const world = boardedWorld("The selector stays put");
		// Pausing here would hide the selector; it is paused only once every
		// render below has been taken.
		struct Unpause { std::shared_ptr<core::World> w; ~Unpause() { w->pauseSimulation(); } } const guard{ world };
		require(world->setSectorFloorDepth(0, kDepth).changed,
			"the fixture's depth write was refused");

		// The Layer behind really is lifted, or the comparison below is
		// between two frames that never moved.
		gUISettings.visibleLayer = 1;
		auto const liftedPicture = drawnLayerBox(world, 1);
		require(liftedPicture.triangles > 0, "No picture to compare against");

		auto const before = drawnSelector(world, 1);
		require(before.rects > 0,
			"The selector drew no Stop plates, so there is nothing here to pin");

		// Stated positively as well as by the comparison below: the plates
		// hang off the anchor's own unlifted screen height. A lift that
		// leaked past the Sector pass would have put them one whole Level
		// (320px) away, which nothing here would mistake for close.
		auto const anchorScreenY = screenY(unliftedMapping(),
			world->getPlayerTransportSelector()->anchor.y);
		require(std::abs(before.bottom - anchorScreenY) < 32.0f,
			"The selector drew with its lowest plate "
				+ std::to_string(before.bottom - anchorScreenY)
				+ "px from the anchor's unlifted screen height; a leaked Layer "
				"lift of " + std::to_string(kDepth) + "px would be far outside "
				"that");

		// The selector is world-space: rendering it against a Layer that
		// carries a lift must give the same rectangles as rendering it
		// against a World with no lift at all.
		require(world->clearSectorFloorDepth(0).changed, "the clear was refused");
		auto const after = drawnSelector(world, 1);
		require(after.rects == before.rects,
			"The selector drew " + std::to_string(after.rects) + " plates with "
			"the Layer flat and " + std::to_string(before.rects) + " lifted");
		require(after.left == before.left && after.right == before.right
				&& after.top == before.top && after.bottom == before.bottom,
			"The player transport selector moved with the Layer: "
				+ describe(before) + " lifted against " + describe(after)
				+ " flat; the selector is world-space and stays where the "
				"transport is");
	}
}

void render_smoke::registerFloorDepthPicking(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthPicking/theCursorLandsOnTheSectorTheShiftedPictureShows",
		isolated<checkTheCursorLandsOnTheSectorTheShiftedPictureShows> });
	checks.push_back({ "floorDepthPicking/theUnliftedMappingLandsOffTheShiftedSector",
		isolated<checkTheUnliftedMappingLandsOffTheShiftedSector> });
	checks.push_back({ "floorDepthPicking/aFlatWorldPicksExactlyWhereItPickedBefore",
		isolated<checkAFlatWorldPicksExactlyWhereItPickedBefore> });
	checks.push_back({ "floorDepthPicking/thePickIsZoomInvariant",
		isolated<checkThePickIsZoomInvariant> });
	checks.push_back({ "floorDepthPicking/theLiftBelongsToTheViewedLayer",
		isolated<checkTheLiftBelongsToTheViewedLayer> });
	checks.push_back({ "floorDepthPicking/theViewLayerIsClampedIntoTheWorld",
		isolated<checkTheViewLayerIsClampedIntoTheWorld> });
	checks.push_back({ "floorDepthPicking/theGridDoesNotMoveWithTheLayer",
		isolated<checkTheGridDoesNotMoveWithTheLayer> });
	checks.push_back({ "floorDepthPicking/thePlayerTransportSelectorDoesNotMoveWithTheLayer",
		isolated<checkThePlayerTransportSelectorDoesNotMoveWithTheLayer> });
}
