// A shifted Sector keeps its whole self attached (#1102).
//
// #1101 put a signed floor depth on a Sector and moved the tile surface of
// every Layer behind it. That was the surface only, and it left the picture
// half-drawn: tiles lifted out of a clip that stayed behind, doors hanging
// in the air where their wall used to be, apertures framing a Layer that
// had slid out from under them. This is the rest of it - everything a
// Sector owns travels with its tiles - and every assertion is on the command
// vector the real pass recorded, at the same CPU seam ADR 0010 names.
//
// The ticket's list, and what each check here asks:
//
//   * a Door, a Window, a BoothWindow, transit geometry, a physical
//     control and a piece of Furniture authored on a shifted Sector record
//     at the shifted position.
//     Each is measured off its own authored shape rather than a remembered
//     pixel count, so the check follows the object rather than a snapshot
//     of what the art happened to be;
//   * an aperture stays at its authored screen position regardless of the
//     back Layer's offset. A hole in a wall is measured off the front
//     Sector's own geometry while the front Layer's lift is in force. The
//     Layer behind gets no vote on where the hole in somebody else's wall
//     is cut, and changing what is behind it must not move the hole;
//   * a back Layer shifted clear of an aperture records nothing inside
//     that aperture. No clamping the back Sector to cover the hole, no
//     diagnostic fill in it - the void the canvas was cleared to simply
//     shows through;
//   * nested aperture recursion draws each deeper Sector under that
//     Sector's own Layer offset. The recursion calls the same function
//     every Sector is drawn by, and that function is what says whose
//     picture is being drawn, so nothing on the way down has to know it
//     recursed.
//
// The clip discriminator this file leans on is worth stating, because three
// of the checks turn on it. A Sector clips its own drawing to its own box,
// and an aperture narrows that clip further on the way down. So a command
// whose clip is narrower vertically than the drawing Sector's own box came
// down an aperture and belongs to the Layer behind; the front Layer's own
// wall tiles carry the wide box and are not part of the measurement. That
// is what lets these checks talk about "what the back Layer put inside the
// hole" without restating the renderer's pass structure.

#include "Checks.h"

#include "DrawTypes.h"
#include "Render.h"
#include "SectorDrawSheet.h"
#include "SectorTileset.h"
#include "UISettings.h"
#include "WorldDrawList.h"
#include "WorldToScreen.h"

#include "core/Defines.h"
#include "core/Button.h"
#include "core/ButtonSectorObject.h"
#include "core/Door.h"
#include "core/DoorSectorObject.h"
#include "core/Furniture.h"
#include "core/Ladder.h"
#include "core/LadderSectorObject.h"
#include "core/Marker.h"
#include "core/MarkerSectorObject.h"
#include "core/Sector.h"
#include "core/SectorObject.h"
#include "core/Window.h"
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

// The editor's selection. A Door's and a control's own rect is only drawn
// while it is the selected object, which is the cheapest way to get that
// rect onto the command stream at all.
extern std::shared_ptr<const core::SectorObject> gSelectedSectorObject;

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
	PackedColourValue const kSelected = packColour(255, 255, 0);

	// The synthetic sheet: one full-cell tile under each surface name the
	// Sectors below ask for, so the quads measured are real `SectorAtlas`
	// triangles and not the missing-art fallback.
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
	void pinViewport()
	{
		gUISettings = UISettings{};
		gUISettings.worldViewportX = 0.0f;
		gUISettings.worldViewportY = 0.0f;
		gUISettings.worldViewportWidth = 4096.0f;
		gUISettings.worldViewportHeight = 4096.0f;
		gUISettings.xOffset = 0.0f;
		gUISettings.yOffset = 0.0f;
		gUISettings.worldZoom = 1.0f;
	}

	// A screen-space box, and the two jobs it has here: describing what was
	// recorded, and being compared against.
	struct Rect
	{
		float left{};
		float right{};
		float top{};
		float bottom{};
	};

	std::string describe(std::string const& what, Rect const& rect)
	{
		return what + " x " + std::to_string(rect.left) + ".."
			+ std::to_string(rect.right) + ", y " + std::to_string(rect.top)
			+ ".." + std::to_string(rect.bottom);
	}

	bool empty(Rect const& rect)
	{
		return rect.right <= rect.left + 0.5f || rect.bottom <= rect.top + 0.5f;
	}

	Rect intersect(Rect const& a, Rect const& b)
	{
		return { std::max(a.left, b.left), std::min(a.right, b.right),
			std::max(a.top, b.top), std::min(a.bottom, b.bottom) };
	}

	// The mapping with a stated lift, so a check can ask where a World rect
	// lands under a Layer it names rather than under whatever lift happens to
	// be ambient in the renderer.
	pf::render::WorldToScreen mappingAt(float liftPx)
	{
		return pf::render::worldToScreenAt(gUISettings, gUISettings.worldZoom)
			.withFloorLift(liftPx);
	}

	// The screen box a World rect lands on under a stated lift.
	Rect screenOf(core::Vector2 const& min, core::Vector2 const& max,
		float liftPx)
	{
		auto const m = mappingAt(liftPx);
		auto const a = m.toScreen({ min.x, min.y });
		auto const b = m.toScreen({ max.x, max.y });
		return { std::min(a.x, b.x), std::max(a.x, b.x),
			std::min(a.y, b.y), std::max(a.y, b.y) };
	}

	Rect screenOf(core::Sector const& sector, float liftPx)
	{
		core::Vector2 a{}, b{};
		sector.getBounds(a, b);
		return screenOf(a, b, liftPx);
	}

	// The screen box of any object's authored extent, asked through the
	// shape-getter named by the caller. Every threshold, control and transit
	// here has a shape getter; none of them shares a base this file could
	// cast to, and inventing one would be a worse debt than the repetition.
	template<typename Get>
	Rect shapeRect(Get getShape, float liftPx)
	{
		core::Vector2 a{}, b{};
		getShape(a, b);
		return screenOf(a, b, liftPx);
	}

	Rect doorRect(std::shared_ptr<const core::Door> const& door, float liftPx)
	{
		return shapeRect(
			[&](core::Vector2& lo, core::Vector2& hi) { door->getFullShape(lo, hi); },
			liftPx);
	}

	Rect windowRect(std::shared_ptr<const core::Window> const& window, float liftPx)
	{
		return shapeRect(
			[&](core::Vector2& lo, core::Vector2& hi) { window->getFullShape(lo, hi); },
			liftPx);
	}

	Rect buttonRect(std::shared_ptr<const core::Button> const& button, float liftPx)
	{
		return shapeRect(
			[&](core::Vector2& lo, core::Vector2& hi) { button->getFullShape(lo, hi); },
			liftPx);
	}

	Rect ladderRect(std::shared_ptr<const core::Ladder> const& ladder, float liftPx)
	{
		return shapeRect(
			[&](core::Vector2& lo, core::Vector2& hi) { ladder->getCurrentShape(lo, hi); },
			liftPx);
	}

	// The bounding box one recorded command's geometry covers.
	Rect geometryOf(WorldDrawList::Triangle const& triangle)
	{
		Rect box{ 1e30f, -1e30f, 1e30f, -1e30f };
		for (auto const& p : triangle.positions)
		{
			box.left = std::min(box.left, p.x);
			box.right = std::max(box.right, p.x);
			box.top = std::min(box.top, p.y);
			box.bottom = std::max(box.bottom, p.y);
		}
		return box;
	}

	Rect geometryOf(WorldDrawList::Line const& line)
	{
		return { std::min(line.from.x, line.to.x), std::max(line.from.x, line.to.x),
			std::min(line.from.y, line.to.y), std::max(line.from.y, line.to.y) };
	}

	Rect clipOf(WorldDrawList::ClipRectangle const& clip)
	{
		return { clip.minimum.x, clip.maximum.x, clip.minimum.y, clip.maximum.y };
	}

	bool matches(Rect const& a, Rect const& b, float epsilon = 0.5f)
	{
		return near(a.left, b.left, epsilon) && near(a.right, b.right, epsilon)
			&& near(a.top, b.top, epsilon) && near(a.bottom, b.bottom, epsilon);
	}

	// The three edges of a hole that the front wall alone decides: both
	// sides and the head. The fourth edge, the sill, is only as far down as
	// the Sector behind reaches, so it is the one edge the back Layer is
	// allowed to move. A hole whose frame holds is a hole that stayed where
	// it was cut however far what is behind it slid.
	bool holeFrameHolds(Rect const& clip, Rect const& authored)
	{
		return near(clip.left, authored.left, 0.5f)
			&& near(clip.right, authored.right, 0.5f)
			&& near(clip.top, authored.top, 0.5f);
	}

	// The Layer offset a Sector draws under, asked of the World the same way
	// the renderer asks it. Stated here rather than remembered so a check
	// cannot drift from the accumulation rule.
	float liftOf(std::shared_ptr<core::World> const& world, uint32_t layer)
	{
		return static_cast<float>(world->layerFloorOffsetPx(layer));
	}

	// One stacked World: a Room on each of three Layers at the same
	// footprint, so a threshold on the middle Layer has a Sector behind it
	// to look through and a Layer in front of it to be shifted by.
	//
	// Three Layers is the minimum that can say anything here. Two Layers put
	// every threshold on the Layer that is not lifted, which is the case
	// that was already true before the feature existed and proves nothing.
	struct Stack
	{
		std::shared_ptr<core::World> world;
		uint32_t front{};
		uint32_t middle{};
		uint32_t back{};
	};

	Stack stackedRooms(std::string const& name, uint32_t levelsHigh = 2)
	{
		auto world = std::make_shared<core::World>(name, 24, 12);
		while (world->getLayerCount() < 3) world->addLayer();
		Stack stack;
		stack.world = std::move(world);
		// addRoom(name, layer, y, x, cellsWide, levelsHigh). Five cells wide
		// so a Door, a Window and a BoothWindow can each have their own cell
		// and no two of them can be mistaken for one another.
		stack.front = stack.world->addRoom("Front", 0, 2, 2, 5, levelsHigh);
		stack.middle = stack.world->addRoom("Middle", 1, 2, 2, 5, levelsHigh);
		stack.back = stack.world->addRoom("Back", 2, 2, 2, 5, levelsHigh);
		return stack;
	}

	// Everything one pass recorded, sorted into the two kinds this file
	// distinguishes: the drawing Sector's own surface, and the draws that
	// came down an aperture and belong to the Layer behind.
	struct Pass
	{
		Rect ownClip{};
		std::vector<WorldDrawList::Triangle const*> ownSurface;
		std::vector<WorldDrawList::Triangle const*> throughAperture;
		std::vector<WorldDrawList::Line const*> throughApertureLines;
	};

	Pass recordMiddlePass(std::shared_ptr<core::World> const& world,
		uint32_t const middleIndex,
		LayerRenderStyle style = LayerRenderStyle::Solid)
	{
		RenderWorldScope scope{ world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const middle = world->getSector(middleIndex);
		require(middle != nullptr, "The fixture Sector could not be found");
		renderSector(middle, middle->getLayerIndex(), style, false, kTint,
			&commands);

		// The widest vertical span the pass recorded is the drawing Sector's
		// own clip: the viewport and its own footprint, before any aperture
		// narrowed it. Derived off the stream rather than off the box this
		// check expects, so a Sector that drew at the wrong height is
		// reported by its own thresholds failing rather than by the
		// classifier refusing to see anything at all.
		Rect widest{ 1e30f, -1e30f, 1e30f, -1e30f };
		for (auto const& command : commands.commands())
		{
			Rect clip{};
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
				clip = clipOf(triangle->clip);
			else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
				clip = clipOf(line->clip);
			else
				continue;
			widest.left = std::min(widest.left, clip.left);
			widest.right = std::max(widest.right, clip.right);
			widest.top = std::min(widest.top, clip.top);
			widest.bottom = std::max(widest.bottom, clip.bottom);
		}
		require(widest.top < widest.bottom, "The pass recorded nothing to classify");
		auto const own = widest;
		Pass pass;
		pass.ownClip = own;
		for (auto const& command : commands.commands())
		{
			if (auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command))
			{
				if (triangle->texture != WorldDrawList::Texture::SectorAtlas) continue;
				auto const clip = clipOf(triangle->clip);
				// Narrower vertically than the Sector's own box, by more
				// than a pixel of slack: something was pushed on the way
				// down that is not this Sector's own clip.
				if (clip.top > own.top + 1.0f || clip.bottom < own.bottom - 1.0f)
					pass.throughAperture.push_back(triangle);
				else
					pass.ownSurface.push_back(triangle);
			}
			else if (auto const* line = std::get_if<WorldDrawList::Line>(&command))
			{
				auto const clip = clipOf(line->clip);
				if (clip.top > own.top + 1.0f || clip.bottom < own.bottom - 1.0f)
					pass.throughApertureLines.push_back(line);
			}
		}
		require(!pass.ownSurface.empty(),
			"The middle Room drew no surface of its own, so there is nothing "
			"here to tell an aperture draw from a wall tile");
		return pass;
	}

	// The bounding box of every selected-object stroke a pass recorded.
	Rect selectedStrokeBox(WorldDrawList const& commands)
	{
		Rect found{ 1e30f, -1e30f, 1e30f, -1e30f };
		for (auto const& command : commands.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (!line || line->colour != kSelected || line->thickness < 1.5f)
				continue;
			auto const box = geometryOf(*line);
			found.left = std::min(found.left, box.left);
			found.right = std::max(found.right, box.right);
			found.top = std::min(found.top, box.top);
			found.bottom = std::max(found.bottom, box.bottom);
		}
		return found;
	}

	int countSelectedStrokes(WorldDrawList const& commands)
	{
		int strokes = 0;
		for (auto const& command : commands.commands())
		{
			auto const* line = std::get_if<WorldDrawList::Line>(&command);
			if (line && line->colour == kSelected && line->thickness >= 1.5f)
				++strokes;
		}
		return strokes;
	}

	// ------------------------------------------------------------------
	// Criterion 1: thresholds, transit geometry and controls in a shifted
	// Sector record at the shifted position.
	// ------------------------------------------------------------------

	// A Door on the shifted Layer. Measured on the Door's own selection
	// rect, which `renderDoor` draws from `Door::getFullShape()` through
	// the renderer's transform: whatever the leaves and the frame art do,
	// that rect is the Door's authored place on screen.
	void checkADoorRecordsAtTheShiftedPosition(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("A Door travels with its Layer");
		core::World::CreateDoorOptions options{};
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto const created = stack.world->addSectorDoor(1, 2, 3, options);
		require(created.door.index != ~0u,
			"The fixture Door could not be created");
		auto const door = std::static_pointer_cast<const core::Door>(
			std::static_pointer_cast<const core::DoorSectorObject>(
				created.door.sector->getObject(created.door.index))->getDoor());
		require(door != nullptr, "The fixture Door could not be resolved");
		require(door->getFrontSector()->getLayerIndex() == 1,
			"The fixture Door was not authored on the Layer this check shifts");

		// Flat first: the Door's own rect before anything moves, so the
		// expectation is measured off the object rather than derived.
		auto const flat = doorRect(door, 0.0f);
		require(!empty(flat), "The fixture Door has no rect to move");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");
		auto const expected = doorRect(door, liftOf(stack.world, 1));
		require(near(expected.top, flat.top - 48.0f),
			"Fixture: the shifted expectation is not 48px off the flat one");

		RenderWorldScope scope{ stack.world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		gSelectedSectorObject = created.door.sector->getObject(created.door.index);
		auto const middle = stack.world->getSector(stack.middle);
		renderSector(middle, 1, LayerRenderStyle::Solid, false, kTint, &commands);
		gSelectedSectorObject.reset();

		require(countSelectedStrokes(commands) == 4,
			"The selected Door drew " + std::to_string(countSelectedStrokes(commands))
				+ " selection strokes rather than the four sides of one rect, "
				"so there is no Door rect to measure");

		auto const found = selectedStrokeBox(commands);
		require(matches(found, expected),
			"The Door on the shifted Layer recorded its rect at "
				+ describe("door", found) + " where its shifted authored place "
				"is " + describe("door", expected) + "; a Door left behind by "
				"the wall it frames hangs in the air");
	}

	// A Window on the shifted Layer, measured on the aperture clip it
	// pushes for the Layer behind. That clip is the hole in the wall, and
	// the hole is the Window's most important contribution to the picture:
	// if it does not move with the Window, the glass looks through to the
	// wrong place.
	void checkWindowsRecordAtTheShiftedPosition(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("Windows travel with their Layer");
		core::World::CreateWindowOptions options{};
		auto const created = stack.world->addSectorWindow(1, 2, 4, 1, 1, options);
		require(created.window.index != ~0u,
			"The fixture Window could not be created");
		auto const window = created.object;
		require(window != nullptr, "The fixture Window could not be resolved");
		require(window->getFrontSector()->getLayerIndex() == 1,
			"The fixture Window was not authored on the Layer this check shifts");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");

		auto const pass = recordMiddlePass(stack.world, stack.middle);
		auto const expected = windowRect(window, liftOf(stack.world, 1));

		bool seen = false;
		for (auto const* triangle : pass.throughAperture)
		{
			if (matches(clipOf(triangle->clip), expected))
			{
				seen = true;
				break;
			}
		}
		require(seen,
			"No draw through an aperture was clipped to the shifted Window's "
				"hole at " + describe("aperture", expected) + "; "
				+ std::to_string(pass.throughAperture.size())
				+ " aperture draws were recorded and none of them matched, so "
				"the glass looks through to somewhere the Window is not");
	}

	// A BoothWindow on the shifted Layer. Its aperture is the shutter's
	// inner rect rather than the frame's full shape, so the expectation is
	// computed from the frame with the same 5-pixel inset the renderer
	// reads off the 52 x 48 art.
	void checkABoothWindowRecordsAtTheShiftedPosition(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("A BoothWindow travels with its Layer");
		auto const created = stack.world->addBoothWindow(1, 2, 2,
			core::Window::State::Open);
		require(created.window.index != ~0u,
			"The fixture BoothWindow could not be created");
		auto const booth = created.object;
		require(booth != nullptr,
			"The fixture BoothWindow could not be resolved");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");

		auto const frame = windowRect(booth, liftOf(stack.world, 1));
		float const width = frame.right - frame.left;
		float const height = frame.bottom - frame.top;
		Rect const inner{ frame.left + width * 5.0f / 52.0f,
			frame.right - width * 5.0f / 52.0f,
			frame.top + height * 5.0f / 48.0f,
			frame.bottom - height * 5.0f / 48.0f };

		auto const pass = recordMiddlePass(stack.world, stack.middle);
		bool seen = false;
		for (auto const* triangle : pass.throughAperture)
		{
			if (matches(clipOf(triangle->clip), inner))
			{
				seen = true;
				break;
			}
		}
		require(seen,
			"No draw through an aperture was clipped to the shifted "
				"BoothWindow's inner rect at " + describe("inner", inner)
				+ "; the BoothWindow's shutter frames a hole that did not "
				"travel with it");
	}

	// Transit geometry on the shifted Layer. A Ladder standing in the
	// middle Room draws a filled rect off its own current shape; that rect
	// is the geometry, and it has to be where the floor the Ladder stands
	// on now is.
	void checkTransitGeometryRecordsAtTheShiftedPosition(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		// A Ladder climbs to a Walkway, so this fixture is three Levels
		// deep with a Walkway over the cell the Ladder starts on. The rest
		// of the file's two-Level stack has nowhere for a Ladder to arrive.
		Stack stack = stackedRooms("Transit geometry travels with its Layer", 3);
		stack.world->addSectorWalkway(stack.middle, 2, 1);
		auto const created = stack.world->addRoomLadder(stack.middle, 0, 1);
		require(created.ladder.index != ~0u,
			"The fixture Ladder could not be created");
		auto const ladder = std::static_pointer_cast<const core::Ladder>(
			std::static_pointer_cast<const core::LadderSectorObject>(
				created.ladder.sector->getObject(created.ladder.index))->getLadder());
		require(ladder != nullptr, "The fixture Ladder could not be resolved");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");

		RenderWorldScope scope{ stack.world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const middle = stack.world->getSector(stack.middle);
		renderSector(middle, 1, LayerRenderStyle::Solid, false, kTint, &commands);

		auto const expected = ladderRect(ladder, liftOf(stack.world, 1));
		bool seen = false;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle) continue;
			if (matches(geometryOf(*triangle), expected))
			{
				seen = true;
				break;
			}
		}
		require(seen,
			"No geometry was recorded at the shifted Ladder's rect at "
				+ describe("ladder", expected) + "; the Ladder is standing on a "
				"floor that moved and geometry that did not");
	}

	// A physical control on the shifted Layer. A Door's own button is
	// authored on the Layer its Door was, so it arrives with the fixture
	// and is measured the same way: its selection rect against its own
	// shifted full shape.
	void checkAPhysicalControlRecordsAtTheShiftedPosition(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("A control travels with its Layer");
		core::World::CreateDoorOptions options{};
		options.activationMode = core::DoorActivationMode::RemoteControlled;
		options.controls[0] = true;
		options.controls[1] = true;
		auto const created = stack.world->addSectorDoor(1, 2, 3, options);
		require(created.controls[0].index != ~0u,
			"The Door authored on the middle Layer brought no physical control "
			"with it, so there is no control to measure");

		auto const control = created.controls[0].sector->getObject(
			created.controls[0].index);
		auto const button = std::static_pointer_cast<const core::Button>(
			control->_getObject());
		require(button != nullptr,
			"The fixture control is not a Button, so it has no shape to measure");
		require(button->getThresholdLayer() == 1u,
			"The fixture control does not belong to the Layer this check shifts");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");
		auto const expected = buttonRect(button, liftOf(stack.world, 1));

		RenderWorldScope scope{ stack.world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		gSelectedSectorObject = control;
		auto const middle = stack.world->getSector(stack.middle);
		renderSector(middle, 1, LayerRenderStyle::Solid, false, kTint, &commands);
		gSelectedSectorObject.reset();

		require(countSelectedStrokes(commands) >= 4,
			"The selected control drew " + std::to_string(countSelectedStrokes(commands))
				+ " selection strokes, so there is no control rect to measure");
		auto const found = selectedStrokeBox(commands);
		require(matches(found, expected),
			"The physical control on the shifted Layer recorded at "
				+ describe("control", found) + " where its shifted authored "
				"place is " + describe("control", expected) + "; a button left "
				"behind by the door it opens is a button on nothing");
	}

	// Furniture on the shifted Layer. Furniture reaches the command stream by
	// its own door - `renderFurnitureMarkers` picks the Marker out of the
	// Sector's objects and hands it to `renderMarker` - so it is not covered
	// by any of the threshold measurements above and is asked about on its
	// own. Measured on the selection rect the Marker draws around its own
	// icon, which is the Marker's position pushed through the renderer's
	// transform: the same route the icon takes, so a lift applied to one is
	// applied to the other and a lift applied to neither shows up here.
	void checkFurnitureRecordsAtTheShiftedPosition(smoke::Context const& context)
	{
		installFlatAtlas();
		pinViewport();
		gUISettings.renderMarkers = true;
		auto const stack = stackedRooms("Furniture travels with its Layer");
		stack.world->attachFurnitureCatalogue("chair.furniture.lua",
			core::FurnitureCatalogue::load(context.fixture(
				"headless/src/smoke/fixtures/furniture/chair.furniture.lua")));
		auto const placed = stack.world->placeFurniture(stack.middle, "chair",
			2.25f, 0.0f, "Chair");
		require(placed != 0,
			"The fixture Furniture could not be placed on the middle Room");
		stack.world->finishBuild();

		// The Marker the Furniture draws through, found by asking the World
		// which Markers are Furniture's rather than by remembering which
		// index the placement happened to land on.
		auto const middle = stack.world->getSector(stack.middle);
		require(middle != nullptr, "The fixture Sector could not be found");
		std::shared_ptr<const core::SectorObject> marker;
		for (uint32_t index = 0; index < middle->getNumObjects(); ++index)
		{
			auto const candidate = std::dynamic_pointer_cast<
				const core::MarkerSectorObject>(middle->getObject(index));
			if (candidate && stack.world->isFurnitureMarker(
					candidate->getMarker()->getId()))
				marker = middle->getObject(index);
		}
		require(marker != nullptr,
			"The placed Furniture left no Furniture Marker on its Sector, so "
			"there is nothing for `renderFurnitureMarkers` to draw");

		auto const record = [&](WorldDrawList& commands) {
			RenderWorldScope scope{ stack.world };
			gSelectedSectorObject = marker;
			renderSector(middle, 1, LayerRenderStyle::Solid, false, kTint,
				&commands);
			gSelectedSectorObject.reset();
			return selectedStrokeBox(commands);
		};

		WorldDrawList flat({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const before = record(flat);
		require(before.top < before.bottom,
			"The Furniture Marker drew no selection rect on the flat Layer, so "
			"there is nothing to compare the shifted one against");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the fixture's depth write was refused");
		WorldDrawList shifted({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const found = record(shifted);

		require(near(found.top, before.top - 48.0f)
				&& near(found.bottom, before.bottom - 48.0f)
				&& near(found.left, before.left) && near(found.right, before.right),
			"The Furniture on the shifted Layer recorded its Marker at "
				+ describe("furniture", found) + " where it sat at "
				+ describe("flat", before) + "; Furniture left behind by the "
				"floor it stands on is Furniture standing on nothing");
	}

	// ------------------------------------------------------------------
	// Criterion 5: the aperture is the front Layer's, and the back Layer
	// gets no say in where it is cut.
	// ------------------------------------------------------------------

	// Changing the back Layer's offset moves what is seen through the hole
	// and must not move the hole. A depth on the middle Layer leaves the
	// middle Layer's own offset untouched - it lifts the Layer behind the
	// middle one - so the aperture is the one thing in the picture that
	// cannot be blamed on the mapping being applied twice.
	void checkTheApertureStaysWhereItWasAuthored(smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("The hole is the front wall's");
		core::World::CreateWindowOptions options{};
		auto const created = stack.world->addSectorWindow(1, 2, 4, 1, 1, options);
		require(created.window.index != ~0u,
			"The fixture Window could not be created");
		auto const window = created.object;
		require(window != nullptr, "The fixture Window could not be resolved");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the front depth write was refused");
		auto const authored = windowRect(window, liftOf(stack.world, 1));

		auto const before = recordMiddlePass(stack.world, stack.middle);
		bool seenBefore = false;
		for (auto const* triangle : before.throughAperture)
			if (holeFrameHolds(clipOf(triangle->clip), authored)) seenBefore = true;
		require(seenBefore,
			"With the back Layer unshifted, no aperture was cut at the "
				"authored hole at " + describe("aperture", authored)
				+ "; the check has nothing to hold still");

		require(stack.world->setSectorFloorDepth(stack.middle, 120).changed,
			"the middle depth write was refused");
		require(near(liftOf(stack.world, 1), 48.0f),
			"Fixture: a depth on the middle Layer moved the middle Layer's own "
				"offset, which it must not");
		require(near(liftOf(stack.world, 2), 168.0f),
			"Fixture: the back Layer did not pick up the middle Layer's depth");

		auto const after = recordMiddlePass(stack.world, stack.middle);
		int held = 0;
		Rect drifted{};
		for (auto const* triangle : after.throughAperture)
		{
			if (holeFrameHolds(clipOf(triangle->clip), authored)) ++held;
			else drifted = clipOf(triangle->clip);
		}
		require(held > 0,
			"After the back Layer was shifted 120px no hole kept the front "
				"wall's frame; the nearest recorded clip was "
				+ describe("clip", drifted) + " against the authored "
				+ describe("aperture", authored) + "; a hole in the front wall "
				"that the Layer behind gets to position is not a hole in the "
				"front wall");
	}

	// ------------------------------------------------------------------
	// Criterion 6: a back Layer shifted clear of an aperture records
	// nothing inside that aperture.
	// ------------------------------------------------------------------

	// Shift the back Layer by a full Level and it stops covering a hole cut
	// at the bottom Level of the wall in front of it. What must happen is
	// nothing: the renderer does not clamp the back Sector down to fill the
	// hole, and it does not paint a diagnostic into it. Every command that
	// came down the hole carries the intersection of the hole and the
	// shifted back Sector, and that intersection is empty.
	void checkAShiftedBackLayerRecordsNothingInsideAnApertureItClears(
		smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("A cleared hole stays empty");
		core::World::CreateWindowOptions options{};
		auto const created = stack.world->addSectorWindow(1, 2, 4, 1, 1, options);
		require(created.window.index != ~0u,
			"The fixture Window could not be created");
		auto const window = created.object;
		require(window != nullptr, "The fixture Window could not be resolved");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the front depth write was refused");
		auto const authored = windowRect(window, liftOf(stack.world, 1));

		// Baseline: the back Layer covers the hole, so something is drawn
		// through it. Without this, the empty result below would only mean
		// the fixture drew nothing at all.
		auto const covered = recordMiddlePass(stack.world, stack.middle);
		int covering = 0;
		for (auto const* triangle : covered.throughAperture)
			if (!empty(intersect(clipOf(triangle->clip), authored))) ++covering;
		require(covering > 0,
			"Baseline: nothing was drawn through the aperture even with the "
				"back Layer in place, so the empty result below proves nothing");

		require(stack.world->setSectorFloorDepth(stack.middle, 320).changed,
			"the middle depth write was refused");
		auto const backBox = screenOf(*stack.world->getSector(stack.back),
			liftOf(stack.world, 2));
		require(backBox.bottom <= authored.top + 0.5f,
			"Fixture: the shifted back Layer still overlaps the aperture ("
				+ describe("back", backBox) + " against "
				+ describe("aperture", authored)
				+ "), so this is not the cleared case");

		auto const cleared = recordMiddlePass(stack.world, stack.middle);
		int painted = 0;
		Rect worst{};
		for (auto const* triangle : cleared.throughAperture)
		{
			auto const visible = intersect(clipOf(triangle->clip), authored);
			if (!empty(visible))
			{
				++painted;
				worst = visible;
			}
		}
		for (auto const* line : cleared.throughApertureLines)
		{
			auto const visible = intersect(clipOf(line->clip), authored);
			if (!empty(visible) && !empty(intersect(geometryOf(*line), visible)))
			{
				++painted;
				worst = visible;
			}
		}
		require(painted == 0,
			"After the back Layer was shifted clear, " + std::to_string(painted)
				+ " commands still painted inside the aperture, worst at "
				+ describe("visible", worst) + "; the void is supposed to show "
				"through, not a clamped Sector or a fill standing in for one");
	}

	// ------------------------------------------------------------------
	// Criterion 7: nested aperture recursion draws each deeper Sector
	// under that Sector's own Layer offset.
	// ------------------------------------------------------------------

	// A Window in the front wall and a Window in the middle wall, one
	// behind the other. Looking through both, the back Room must draw at
	// the back Layer's offset - not at the middle Layer's, which is what a
	// lift installed once at the top of the recursion and never changed
	// would give, and not at zero, which is what a lift dropped on the way
	// down would give.
	//
	// The tile grid is anchored on the Sector's own floor line, so the
	// bottom edge of what the back Room draws through the inner hole is
	// that floor line lifted by the back Layer's offset and by nothing else.
	// It is a single number, and it is the whole criterion.
	void checkNestedAperturesDrawEachSectorAtItsOwnLayerOffset(
		smoke::Context const&)
	{
		installFlatAtlas();
		pinViewport();
		auto const stack = stackedRooms("Nested holes, own offsets");
		core::World::CreateWindowOptions options{};
		auto const outer = stack.world->addSectorWindow(0, 2, 4, 1, 1, options);
		require(outer.window.index != ~0u,
			"The outer fixture Window could not be created");
		auto const inner = stack.world->addSectorWindow(1, 2, 4, 1, 1, options);
		require(inner.window.index != ~0u,
			"The inner fixture Window could not be created");

		require(stack.world->setSectorFloorDepth(stack.front, 48).changed,
			"the front depth write was refused");
		require(stack.world->setSectorFloorDepth(stack.middle, 24).changed,
			"the middle depth write was refused");
		require(near(liftOf(stack.world, 1), 48.0f)
				&& near(liftOf(stack.world, 2), 72.0f),
			"Fixture: the staircase is not 48 then 72");

		// The inner hole, measured in the middle Layer's frame: the inner
		// Window's own shape under the middle Layer's lift, narrowed by the
		// outer hole it is being seen through and by how far the back Layer
		// reaches. Everything but the inner Window's own frame is reach, not
		// position, so the frame is what the expectation is pinned to.
		auto const outerHole = windowRect(outer.object, liftOf(stack.world, 0));
		auto const innerHole = windowRect(inner.object, liftOf(stack.world, 1));
		auto const backBox = screenOf(*stack.world->getSector(stack.back),
			liftOf(stack.world, 2));
		auto const expectedClip = intersect(intersect(innerHole, outerHole), backBox);
		require(!empty(expectedClip),
			"Fixture: the back Layer does not reach the inner hole at all, so "
				"there is no nested draw to measure");

		// Drawn from the front Layer, through both holes.
		RenderWorldScope scope{ stack.world };
		WorldDrawList commands({ { -100000.0f, -100000.0f },
			{ 100000.0f, 100000.0f } });
		auto const front = stack.world->getSector(stack.front);
		renderSector(front, 0, LayerRenderStyle::Solid, false, kTint, &commands);

		int nested = 0;
		float deepestBottom = -1e30f;
		for (auto const& command : commands.commands())
		{
			auto const* triangle = std::get_if<WorldDrawList::Triangle>(&command);
			if (!triangle
				|| triangle->texture != WorldDrawList::Texture::SectorAtlas)
				continue;
			if (!matches(clipOf(triangle->clip), expectedClip)) continue;
			++nested;
			deepestBottom = std::max(deepestBottom, geometryOf(*triangle).bottom);
		}
		require(nested > 0,
			"Nothing was drawn through the inner hole clipped to "
				+ describe("clip", expectedClip) + "; the recursion did not "
				"reach the back Layer at all");

		auto const atMiddleLayersOffset = screenOf(
			*stack.world->getSector(stack.back), liftOf(stack.world, 1)).bottom;
		require(near(deepestBottom, backBox.bottom),
			"The back Layer drew through the inner hole down to y "
				+ std::to_string(deepestBottom) + " where its own floor lifted "
				"by its own Layer offset sits at " + std::to_string(backBox.bottom)
				+ "; the middle Layer's offset would have put it at "
				+ std::to_string(atMiddleLayersOffset)
				+ ", and a deeper Sector drawing under whatever lift the "
				"recursion happened to be carrying is a Sector drawn at the "
				"wrong depth");
	}
}

void render_smoke::registerFloorDepthAttached(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepthAttached/aDoorRecordsAtTheShiftedPosition",
		isolated<checkADoorRecordsAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/windowsRecordAtTheShiftedPosition",
		isolated<checkWindowsRecordAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/aBoothWindowRecordsAtTheShiftedPosition",
		isolated<checkABoothWindowRecordsAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/transitGeometryRecordsAtTheShiftedPosition",
		isolated<checkTransitGeometryRecordsAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/aPhysicalControlRecordsAtTheShiftedPosition",
		isolated<checkAPhysicalControlRecordsAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/furnitureRecordsAtTheShiftedPosition",
		isolated<checkFurnitureRecordsAtTheShiftedPosition> });
	checks.push_back({ "floorDepthAttached/theApertureStaysWhereItWasAuthored",
		isolated<checkTheApertureStaysWhereItWasAuthored> });
	checks.push_back({ "floorDepthAttached/aShiftedBackLayerRecordsNothingInsideAnApertureItClears",
		isolated<checkAShiftedBackLayerRecordsNothingInsideAnApertureItClears> });
	checks.push_back({ "floorDepthAttached/nestedAperturesDrawEachSectorAtItsOwnLayerOffset",
		isolated<checkNestedAperturesDrawEachSectorAtItsOwnLayerOffset> });
}
