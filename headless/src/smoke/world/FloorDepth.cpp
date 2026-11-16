// What the World says about floor depth (#1101).
//
// A Sector's floor depth is one authored number; the thing the screen is
// made of is the staircase it produces. Every Layer behind a Sector draws
// by the sum of the depths in front of it, and the whole feature hangs on
// that sum being stated in exactly one place, because two statements of it
// - one in the renderer, one in the editor panel, one in the shipped game -
// is the same feature drawn three different ways.
//
// So this file pins the rule rather than the picture:
//
//   * absence is zero. A Sector with no recorded field answers 0 and lifts
//     nothing, which is what lets every World written before the field
//     existed keep drawing exactly as it drew;
//   * presence is the override, and since #1112 the value carried by that
//     presence is the depth - zero included. A zero override pins the
//     Sector to the unlifted position while its type default lifts, which
//     is a state the author asks for and which the old collapse of zero
//     into absence could not express at all;
//   * Layer 0 never moves. The depth a Sector carries is a lift it hands
//     backwards and never to itself, which is why the offset of a Layer is
//     the sum of the Layers *in front* and not of its own;
//   * the corridor rung, added by #1115 and consulted for Corridors only:
//     Room and Corridor are one key in the type table, so a single optional
//     World value is where the split lives. A Corridor with no rung falls
//     through to `Location` exactly as it always did, a Corridor with its own
//     override never reaches the rung, and a rung of zero is a value rather
//     than an absence because absence here means *follow Location*, not *lift
//     nothing*;
//   * the contribution of a Layer is the maximum depth in it, not the sum.
//     Two Rooms side by side in front, one at 48 and one at 20, lift the
//     Layer behind by 48. Summing would lift it by 68 and make the picture
//     depend on how many Rooms happened to be authored in a row, which is
//     not a thing a perspective cue should ever depend on;
//   * negatives accumulate the same way, pushing the Layers behind down;
//   * an empty Layer contributes nothing, so the staircase walks straight
//     over a gap in the Layers without a special case;
//   * the whole staircase and a single Layer's step are the same sum, so
//     the two answers cannot drift apart.
//
// The checks go through the World's own queries rather than reaching for
// the Sector objects, because the queries are the seam. The renderer and
// the editor both ask the World, and neither of them is allowed to
// reconstruct the arithmetic on its own.

#include "Checks.h"

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using smoke::require;
	using WorldPtr = std::shared_ptr<core::World>;

	// A World with enough Layers to hold the ones it is told to fill, with
	// one Room on each of those Layers. The Layers are deliberately separate
	// Rooms rather than one Room spanning them: the staircase is a fact
	// about Layers, and a fixture that put two Sectors on one Layer would
	// test the maximum rule by accident rather than on purpose.
	WorldPtr layeredWorld(std::string const& name, std::vector<uint32_t> const& layersToFill)
	{
		auto world = std::make_shared<core::World>(name, 24, 12);
		uint32_t const wanted = layersToFill.empty() ? 0u
			: *std::max_element(layersToFill.begin(), layersToFill.end()) + 1u;
		while (world->getLayerCount() < wanted) world->addLayer();
		for (auto const layer : layersToFill)
			world->addRoom("Room " + std::to_string(layer), layer, 2, 2 + layer * 4, 3, 2);
		return world;
	}

	// The index of the first Sector on a Layer. `getSectors` hands back the
	// Sectors themselves; the World's floor-depth queries are asked of the
	// index, which is what the renderer carries and what the construction
	// record is keyed by.
	uint32_t firstOn(WorldPtr const& world, uint32_t layer)
	{
		auto const sectors = world->getSectors(layer);
		require(!sectors.empty(), "Layer " + std::to_string(layer)
			+ " of the fixture holds no Sectors, so the case cannot say anything");
		return sectors.front()->getIndex();
	}

	// The depth of the first Sector on a Layer, read back through the World.
	int32_t depthOf(WorldPtr const& world, uint32_t layer)
	{
		return world->effectiveFloorDepth(firstOn(world, layer));
	}

	// A two-Layer World with one Room and one Corridor on the front Layer and
	// one Room behind, so the corridor rung has something it can move, something
	// it must not move, and something to lift (#1115).
	//
	// The two identity assertions are the premise the whole rung rests on, and
	// they are pinned rather than assumed: corridor identity is read from
	// `Sector::isRoom()`, which is already virtual and already means "a
	// Location that is neither a Corridor nor a Facade". Were that ever to stop
	// holding, every check below would resolve nothing and go quietly green
	// instead of failing, which is the worst way for a fixture to be wrong.
	struct FrontPair
	{
		WorldPtr world;
		uint32_t room;
		uint32_t corridor;
	};

	FrontPair roomAndCorridorInFront(std::string const& name)
	{
		auto world = std::make_shared<core::World>(name, 40, 12);
		while (world->getLayerCount() < 2) world->addLayer();
		auto const room = world->addRoom("Room", 0, 2, 1, 3, 2);
		auto const corridor = world->addCorridor(0, 2, 8, 3, 1);
		world->addRoom("Behind", 1, 2, 1, 3, 2);
		world->finishBuild();
		require(!world->getSector(corridor)->isRoom(),
			"The Sector built by addCorridor reports itself as a Room, so the "
			"corridor rung resolves nothing and every check built on this fixture "
			"is vacuous");
		require(world->getSector(corridor)->getType() == core::SectorType::Location,
			"A Corridor is not of Location type, so the rung's type test is wrong");
		require(world->getSector(room)->isRoom(),
			"The Sector built by addRoom does not report itself as a Room");
		return FrontPair{ std::move(world), room, corridor };
	}

	// ------------------------------------------------------------------
	// Absence.
	// ------------------------------------------------------------------

	// A World nobody lifted: every Sector answers zero, every Layer draws
	// where it was authored, and the staircase is flat.
	void aWorldWithNoDepthsAnywhereIsFlat(smoke::Context const&)
	{
		auto const world = layeredWorld("Flat", { 0, 1, 2 });
		for (uint32_t layer = 0; layer < world->getLayerCount(); ++layer)
		{
			auto const sectors = world->getSectors(layer);
			for (auto const& sector : sectors)
				require(world->effectiveFloorDepth(sector->getIndex()) == 0,
					"Sector " + std::to_string(sector->getIndex()) + " on Layer "
						+ std::to_string(layer) + " answers a floor depth of "
						+ std::to_string(world->effectiveFloorDepth(sector->getIndex()))
						+ " with no field recorded; absence must read as zero");
			require(world->layerFloorOffsetPx(layer) == 0,
				"Layer " + std::to_string(layer) + " draws "
					+ std::to_string(world->layerFloorOffsetPx(layer))
					+ "px off with every depth at zero");
		}
		auto const offsets = world->layerFloorOffsets();
		require(offsets.size() == world->getLayerCount(),
			"The staircase carries " + std::to_string(offsets.size())
				+ " steps for a World with "
				+ std::to_string(world->getLayerCount()) + " Layers");
		for (auto const offset : offsets)
			require(offset == 0, "The staircase is not flat at a zero-depth World");
	}

	// Asking about a Sector index that does not exist answers zero rather
	// than reading off the end of the Sector list.
	void anIndexOutsideTheWorldAnswersZero(smoke::Context const&)
	{
		auto const world = layeredWorld("Bounds", { 0 });
		require(world->effectiveFloorDepth(100000u) == 0,
			"effectiveFloorDepth read past the end of the Sector list");
		require(world->layerFloorOffsetPx(100000u) == 0,
			"layerFloorOffsetPx read past the end of the staircase");
	}

	// ------------------------------------------------------------------
	// The staircase.
	// ------------------------------------------------------------------

	// One Room in front at 48: the Layer behind draws 48px higher, the
	// Layer in front draws where it was authored.
	void oneDepthLiftsEveryLayerBehindIt(smoke::Context const&)
	{
		auto const world = layeredWorld("One lift", { 0, 1, 2 });
		require(world->setSectorFloorDepth(firstOn(world, 0), 48).changed,
			"Setting a floor depth of 48 on the front Room was not accepted as a change");

		require(depthOf(world, 0) == 48,
			"The front Room does not report the depth it was given");
		require(world->layerFloorOffsetPx(0) == 0,
			"Layer 0 draws " + std::to_string(world->layerFloorOffsetPx(0))
				+ "px off; a Sector's own depth must never move its own Layer");
		require(world->layerFloorOffsetPx(1) == 48,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; the Layer directly behind a Sector at 48 draws 48px higher");
		require(world->layerFloorOffsetPx(2) == 48,
			"Layer 2 draws " + std::to_string(world->layerFloorOffsetPx(2))
				+ "px off; the lift carries through every Layer behind, not only "
				  "the one directly behind");
	}

	// The staircase adds as it goes back: 48 in front of 10 is 58 two
	// Layers behind, and the running total is visible in the whole array.
	void depthsAccumulateBackwards(smoke::Context const&)
	{
		auto const world = layeredWorld("Accumulating", { 0, 1, 2 });
		require(world->setSectorFloorDepth(firstOn(world, 0), 48).changed,
			"Setting the front depth failed");
		require(world->setSectorFloorDepth(firstOn(world, 1), 10).changed,
			"Setting the middle depth failed");

		auto const offsets = world->layerFloorOffsets();
		require(offsets.size() == world->getLayerCount(),
			"The staircase is the wrong length");
		require(offsets[0] == 0 && offsets[1] == 48 && offsets[2] == 58,
			"The staircase came out as " + std::to_string(offsets[0]) + ", "
				+ std::to_string(offsets[1]) + ", " + std::to_string(offsets[2])
				+ "; 48 in front of 10 is 0, 48, 58");
	}

	// Two Sectors in front, one at 48 and one at 20: the Layer behind is
	// lifted by 48, not by 68. The contribution of a Layer is the deepest
	// floor in it, so the lift cannot depend on how many Rooms happened to
	// be authored side by side.
	void theDeepestFloorInFrontWinsNotTheSum(smoke::Context const&)
	{
		auto const world = std::make_shared<core::World>("Two in front", 24, 12);
		while (world->getLayerCount() < 2) world->addLayer();
		auto const left = world->addRoom("Left", 0, 2, 2, 3, 2);
		auto const right = world->addRoom("Right", 0, 2, 8, 3, 2);
		world->addRoom("Behind", 1, 2, 2, 3, 2);

		require(world->setSectorFloorDepth(left, 48).changed, "left depth refused");
		require(world->setSectorFloorDepth(right, 20).changed, "right depth refused");

		require(world->layerFloorOffsetPx(1) == 48,
			"The Layer behind two Sectors at 48 and 20 draws "
				+ std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; the deepest floor in front is what lifts it, so the "
				  "answer is 48 and summing would make the picture depend on how "
				  "many Rooms were authored in a row");
	}

	// A shallower Sector in front of a deeper one still lifts by the deeper
	// one's amount: the maximum is over the whole Layer in front, not over
	// what is directly ahead of each cell.
	void aShallowerSectorBehindADeeperOneDoesNotLowerTheLift(
		smoke::Context const&)
	{
		auto const world = std::make_shared<core::World>("Shallow behind deep", 24, 12);
		while (world->getLayerCount() < 3) world->addLayer();
		auto const deep = world->addRoom("Deep", 0, 2, 2, 3, 2);
		require(world->setSectorFloorDepth(deep, 64).changed, "deep depth refused");
		// Layer 1 sits behind at a shallower depth of its own; Layer 2 sits
		// behind that. Layer 1 is lifted by Layer 0's 64, and Layer 2 by the
		// deeper of Layer 0's 64 and Layer 1's own 8, which is 64 + 8.
		auto const shallow = world->addRoom("Shallow", 1, 2, 2, 3, 2);
		require(world->setSectorFloorDepth(shallow, 8).changed, "shallow depth refused");
		world->addRoom("Furthest", 2, 2, 2, 3, 2);

		require(world->layerFloorOffsetPx(1) == 64,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; it is behind a Sector at 64");
		require(world->layerFloorOffsetPx(2) == 72,
			"Layer 2 draws " + std::to_string(world->layerFloorOffsetPx(2))
				+ "px off; the staircase is 64 from Layer 0 plus the 8 Layer 1 "
				  "hands back, and a shallower depth behind never cancels a "
				  "deeper one in front");
	}

	// Negative depths accumulate the same way and push the Layers behind
	// down rather than lifting them up.
	void negativeDepthsPushTheLayersBehindDown(smoke::Context const&)
	{
		auto const world = layeredWorld("Negative", { 0, 1, 2 });
		require(world->setSectorFloorDepth(firstOn(world, 0), -30).changed,
			"Setting a floor depth of -30 was not accepted");
		require(depthOf(world, 0) == -30,
			"A negative depth does not read back as it was written");
		require(world->layerFloorOffsetPx(0) == 0,
			"Layer 0 moved under its own negative depth");
		require(world->layerFloorOffsetPx(1) == -30,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; a depth of -30 in front pushes it 30px down");
		require(world->layerFloorOffsetPx(2) == -30,
			"Layer 2 draws " + std::to_string(world->layerFloorOffsetPx(2))
				+ "px off; the push carries through as the lift does");
	}

	// A positive depth in front of a negative one can cancel out, and the
	// staircase crosses zero rather than clamping at it.
	void opposingDepthsCancelAndCross(smoke::Context const&)
	{
		auto const world = layeredWorld("Opposing", { 0, 1, 2 });
		require(world->setSectorFloorDepth(firstOn(world, 0), 40).changed,
			"front depth refused");
		require(world->setSectorFloorDepth(firstOn(world, 1), -70).changed,
			"middle depth refused");
		auto const offsets = world->layerFloorOffsets();
		require(offsets[0] == 0 && offsets[1] == 40 && offsets[2] == -30,
			"The staircase came out as " + std::to_string(offsets[0]) + ", "
				+ std::to_string(offsets[1]) + ", " + std::to_string(offsets[2])
				+ "; 40 in front of -70 is 0, 40, -30 and the sum crosses zero "
				  "rather than stopping at it");
	}

	// A Layer with nothing in it hands back nothing, so the staircase walks
	// straight over a gap in the Layers.
	void anEmptyLayerContributesNothing(smoke::Context const&)
	{
		auto const world = std::make_shared<core::World>("Gap", 24, 12);
		while (world->getLayerCount() < 3) world->addLayer();
		auto const front = world->addRoom("Front", 0, 2, 2, 3, 2);
		require(world->setSectorFloorDepth(front, 48).changed, "front depth refused");
		// Layer 1 is left empty; Layer 2 has a Room.
		world->addRoom("Furthest", 2, 2, 2, 3, 2);
		require(world->getSectors(1).empty(),
			"The fixture was supposed to leave Layer 1 empty");

		require(world->layerFloorOffsetPx(1) == 48,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; it is behind a Sector at 48");
		require(world->layerFloorOffsetPx(2) == 48,
			"Layer 2 draws " + std::to_string(world->layerFloorOffsetPx(2))
				+ "px off; the empty Layer 1 contributes a maximum of nothing, so "
				  "the lift carries over the gap unchanged");
	}

	// ------------------------------------------------------------------
	// One statement of the sum.
	// ------------------------------------------------------------------

	// The whole staircase and a single Layer's step are the same arithmetic.
	// Asked twice with no edit between, and asked both ways round, the answers
	// agree; there is no second copy of the sum anywhere that could have
	// drifted.
	void theSingleStepAndTheWholeStaircaseAgree(smoke::Context const&)
	{
		auto const world = std::make_shared<core::World>("Agreement", 24, 12);
		while (world->getLayerCount() < 4) world->addLayer();
		for (uint32_t layer = 0; layer < 4; ++layer)
		{
			auto const room = world->addRoom("Room " + std::to_string(layer), layer, 2,
				2 + layer * 3, 2, 2);
			require(world->setSectorFloorDepth(room, int32_t(layer * 13) - 22).changed,
				"depth refused on a freshly built Room");
		}
		require(world->getLayerCount() >= 4, "The fixture needs four Layers");

		auto const staircase = world->layerFloorOffsets();
		for (uint32_t layer = 0; layer < staircase.size(); ++layer)
			require(world->layerFloorOffsetPx(layer) == staircase[layer],
				"Layer " + std::to_string(layer) + " answers "
					+ std::to_string(world->layerFloorOffsetPx(layer))
					+ " asked on its own and " + std::to_string(staircase[layer])
					+ " asked as part of the whole staircase; the two must be one sum");

		// And asking again changes nothing: the answer is a function of the
		// World, not a counter or a cached value with a life of its own.
		auto const again = world->layerFloorOffsets();
		require(staircase == again,
			"The staircase changed between two asks with no edit between them");
	}

	// The staircase is derived from the World every time it is asked, not
	// remembered from the last ask. An edit between two asks shows up in the
	// second one with no invalidation step in between, which is the property
	// that keeps the renderer from needing a cache it could get wrong.
	void theStaircaseFollowsTheWorldWithNoInvalidation(smoke::Context const&)
	{
		auto const world = layeredWorld("Live", { 0, 1 });
		auto const front = firstOn(world, 0);
		require(world->layerFloorOffsetPx(1) == 0,
			"A World nobody lifted should draw flat");
		require(world->setSectorFloorDepth(front, 48).changed, "depth refused");
		require(world->layerFloorOffsetPx(1) == 48,
			"The staircase did not follow the write");
		require(world->clearSectorFloorDepth(front).changed, "clear refused");
		require(world->layerFloorOffsetPx(1) == 0,
			"The staircase did not follow the clear; a lift that outlives the "
			  "Sector's own depth is the invalidation bug this avoids");
	}

	// Setting zero and clearing are not the same write, and since #1112 that
	// distinction is the feature rather than a wart. A zero override is a
	// real state - the Sector is pinned to the unlifted position, which is
	// what an author asks for when the type default lifts and this one Room
	// should not - while clearing deletes the override and puts the Sector
	// back to following. Collapsed into one state, as they were before, a
	// Sector could never be held still under a lifting type, and the
	// editor's Break away had nothing to write over a type at zero.
	void settingZeroPinsAndClearingDeletesTheOverride(smoke::Context const&)
	{
		auto const world = layeredWorld("Zero pins, clearing deletes", { 0, 1 });
		auto const front = firstOn(world, 0);
		require(world->setSectorFloorDepth(front, 48).changed, "depth refused");

		// The pin. Zero arrives as a value like any other: the override is
		// present, and it is the zero that answers, not an absence.
		require(world->setSectorFloorDepth(front, 0).changed,
			"Setting zero over 48 was not seen as a change");
		require(world->hasFloorDepthOverride(front),
			"Setting zero left the Sector with no override; a zero is a real "
			"override since #1112, not a way of writing nothing");
		require(world->effectiveFloorDepth(front) == 0,
			"The pinned Sector answers " + std::to_string(world->effectiveFloorDepth(front))
				+ " rather than the zero it was pinned with");
		require(world->layerFloorOffsetPx(1) == 0,
			"The Layer behind still draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off after the Sector in front was pinned to zero");

		// Re-stating the pin changes nothing: the write is compared against
		// what is already there, so the author cannot dirty the document by
		// typing the number the row already shows.
		auto const again = world->setSectorFloorDepth(front, 0);
		require(again.accepted && !again.changed,
			"Setting the zero the Sector already pins was reported as a change");

		// The deletion. This is the half that used to be the same write as
		// the pin, and the half that has to be different for following to
		// mean anything.
		require(world->clearSectorFloorDepth(front).changed,
			"Clearing the zero pin was not seen as a change; deleting an "
			"override is a write of its own since #1112");
		require(!world->hasFloorDepthOverride(front),
			"Clearing left the Sector still carrying an override");
		require(world->effectiveFloorDepth(front) == 0,
			"A Sector with no override answers "
				+ std::to_string(world->effectiveFloorDepth(front))
				+ "; with nothing in front and no type lifting, absence still reads zero");

		// And clearing an already-clear Sector is legal, changes nothing,
		// and leaves no second deletion to undo.
		auto const cleared = world->clearSectorFloorDepth(front);
		require(cleared.accepted && !cleared.changed,
			"Clearing a Sector with nothing to delete was reported as a change");
	}

	// ------------------------------------------------------------------
	// The corridor rung (#1115).
	// ------------------------------------------------------------------

	// The criterion the whole rung exists for: Room and Corridor are one key in
	// the type table, so "Rooms at 24, Corridors at 8" was unreachable. One
	// corridor number and one type number reach it, and neither leaks into the
	// other.
	void aCorridorDepthMovesCorridorsAndLeavesRoomsOnTheirType(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Corridors at 8, Rooms at 24");
		auto const& world = fixture.world;
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Setting the Location type default to 24 was refused");
		require(world->setCorridorFloorDepth(8).changed,
			"Setting the corridor floor depth to 8 was refused");

		require(world->effectiveFloorDepth(fixture.corridor) == 8,
			"The Corridor answers " + std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ "; a Corridor with no override of its own takes the corridor rung, "
				  "not the Location type its type table row puts it at");
		require(world->effectiveFloorDepth(fixture.room) == 24,
			"The Room answers " + std::to_string(world->effectiveFloorDepth(fixture.room))
				+ "; the corridor rung is consulted for Corridors only, and a Room "
				  "goes straight past it to its type");
		require(world->corridorFloorDepth() == 8 && world->hasCorridorFloorDepth(),
			"The corridor rung does not read back the depth it was given");
		require(world->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"Setting the corridor depth reached into the Location type and moved it");
	}

	// No corridor rung, and the Corridor follows `Location` exactly as it did
	// before this field existed. This is the half that keeps every World written
	// before #1115 drawing the picture it drew, and it is checked with the type
	// both set and unset so the fall-through is visible at both ends of the
	// type's range.
	void aCorridorWithNoCorridorDepthFollowsTheLocationTypeDefault(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("No corridor rung");
		auto const& world = fixture.world;
		require(!world->hasCorridorFloorDepth(),
			"A World nobody set a corridor depth on reports a corridor rung");
		require(world->corridorFloorDepth() == 0,
			"An unset corridor rung answers " + std::to_string(world->corridorFloorDepth())
				+ "; absence must read as no opinion, which lifts nothing");

		// The type unset: today's flat picture, Corridor and Room both at zero.
		require(world->effectiveFloorDepth(fixture.corridor) == 0,
			"A Corridor with no rung and no type default answers "
				+ std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ "; with nothing above it the answer is zero, as it was before the "
				  "rung existed");
		require(world->effectiveFloorDepth(fixture.room) == 0,
			"A Room with no type default answers a depth it was never given");

		// The type set: the Corridor follows it, because with no rung there is
		// nothing between the Sector and its type.
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Location default refused in fixture");
		require(world->effectiveFloorDepth(fixture.corridor) == 24,
			"A Corridor with no corridor rung answers "
				+ std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ "; with no rung it falls through to the Location type default, "
				  "which is exactly what it did before #1115");
		require(world->effectiveFloorDepth(fixture.room) == 24,
			"The Room did not follow its type");
		require(world->layerFloorOffsetPx(1) == 24,
			"The Layer behind draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off with the front Layer at its type's 24 and no rung set");
	}

	// The rung is a rung and not a paint: it reaches every Corridor in the World
	// in one write, and reaches nothing that is not a Corridor - including the
	// Facade, which is not a room either and must still not take it.
	void theCorridorRungReachesEveryCorridorAndNoOtherType(smoke::Context const&)
	{
		auto world = std::make_shared<core::World>("Rung reach", 60, 12);
		while (world->getLayerCount() < 2) world->addLayer();
		auto const near = world->addCorridor(0, 2, 1, 3, 1);
		auto const far = world->addCorridor(0, 2, 8, 3, 1);
		auto const room = world->addRoom("Room", 0, 2, 15, 3, 2);
		auto const facade = world->addFacade("Facade", 0, 2, 21, 3, 2);
		auto const background = world->addBackground(0, 2, 27, 3, 2);
		world->finishBuild();

		require(!world->getSector(near)->isRoom() && !world->getSector(far)->isRoom(),
			"A built Corridor reports itself as a Room, so the fixture is vacuous");
		require(!world->getSector(facade)->isRoom(),
			"A Facade reports itself as a Room; the fixture relies on it not being "
			"one, which is what makes the rung's type test matter");

		require(world->setCorridorFloorDepth(-12).changed,
			"Setting the corridor depth to -12 was refused");
		for (auto const corridor : { near, far })
			require(world->effectiveFloorDepth(corridor) == -12,
				"Corridor " + std::to_string(corridor) + " answers "
					+ std::to_string(world->effectiveFloorDepth(corridor))
					+ "; one rung write moves every Corridor, the same way one type "
					  "write moves every Sector following that type");
		require(world->effectiveFloorDepth(room) == 0,
			"A Room picked up the corridor rung; the rung is for Corridors");
		require(world->effectiveFloorDepth(facade) == 0,
			"A Facade took the corridor rung because it is not a room either; the "
				"rung is for Corridors, which are Locations, and a Facade is its "
				"own Sector type");
		require(world->effectiveFloorDepth(background) == 0,
			"A Background took the corridor rung");
	}

	// A per-Sector override beats the corridor rung, which is what an override
	// means at any rung below it. The overridden Corridor stays where its own
	// field put it while the rung moves every other Corridor under it.
	void aSectorOverrideBeatsTheCorridorDepth(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Override over the rung");
		auto const& world = fixture.world;
		require(world->setCorridorFloorDepth(8).changed, "rung refused");
		require(world->setSectorFloorDepth(fixture.corridor, 40).changed,
			"Setting the Corridor's own depth over the rung was refused");

		require(world->effectiveFloorDepth(fixture.corridor) == 40,
			"An overridden Corridor answers "
				+ std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ " with the rung at 8; the Sector's own field is the top of the "
				  "ladder and the rung sits below it");
		require(world->hasFloorDepthOverride(fixture.corridor),
			"An overridden Corridor does not report its override");

		// Moving the rung under the overridden Corridor leaves it exactly where
		// its own field put it - the same property #1103 pins against the type,
		// asked of the rung instead.
		require(world->setCorridorFloorDepth(-30).changed, "rung move refused");
		require(world->effectiveFloorDepth(fixture.corridor) == 40,
			"The Corridor moved when its rung moved under it; an override that "
			"follows the rung is not an override");
		require(world->effectiveFloorDepth(fixture.room) == 0,
			"The Room followed the corridor rung");
	}

	// The staircase accumulates from the corridor depth where the deepest floor
	// in a Layer is a Corridor: the rung is an effective depth like any other
	// once resolved, and the single sum #1101 established does not know or care
	// which rung it came from.
	void theStaircaseAccumulatesFromTheCorridorDepth(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Corridor lifts the Layer behind");
		auto const& world = fixture.world;
		require(world->setCorridorFloorDepth(50).changed, "rung refused");
		// The Room is left on the unset Location type, so the Corridor at 50 is
		// unambiguously the deepest floor in Layer 0 and the lift can only have
		// come from the rung.
		require(world->effectiveFloorDepth(fixture.room) == 0,
			"The Room is not at zero in this fixture, so the lift below proves "
			"nothing about the corridor depth being what produced it");

		require(world->layerFloorOffsetPx(0) == 0,
			"Layer 0 moved under its own corridor rung; a depth is a lift handed "
			"backwards and never to its own Layer");
		require(world->layerFloorOffsetPx(1) == 50,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off with the deepest front floor a Corridor at its rung's 50; "
				  "the staircase takes the corridor depth the same way it takes a "
				  "Sector's own or a type's");
	}

	// The maximum rule is unchanged by the new rung: a Corridor on the rung is
	// deepest only when it actually is. A Room deeper than the rung still lifts
	// the Layer behind by its own amount, and a Corridor shallower than the Room
	// in front of it never cancels the deeper lift.
	void aCorridorOnTheRungDoesNotOutrankADeeperRoom(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Deeper room wins over the rung");
		auto const& world = fixture.world;
		require(world->setCorridorFloorDepth(8).changed, "rung refused");
		require(world->setSectorFloorDepth(fixture.room, 40).changed, "room depth refused");
		require(world->effectiveFloorDepth(fixture.corridor) == 8,
			"The Corridor did not take the rung in this fixture");

		require(world->layerFloorOffsetPx(1) == 40,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off with a Room at 40 and a Corridor on the rung at 8 in "
				  "front; the contribution of a Layer is its deepest floor, and the "
				  "rung does not make a Corridor count for more than it is");
	}

	// The rung's zero is a value, not an absence, and it is the only way to say
	// "Corridors hold at the unlifted position while Rooms lift". Absence here
	// means *follow Location*, so unlike the type map - where a zero row is
	// refused because an absent row already answers zero - the rung keeps its
	// zero. This is ADR 0024's rule read one rung up.
	void aZeroCorridorDepthHoldsCorridorsStillWhileRoomsLift(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Zero rung");
		auto const& world = fixture.world;
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Location default refused");
		require(world->setCorridorFloorDepth(0).changed,
			"Setting the corridor depth to 0 over an unset rung was not seen as a "
			"change; a zero rung is a value, not a way of writing nothing");
		require(world->hasCorridorFloorDepth(),
			"A zero corridor depth left the World with no rung; the zero would then "
			"be indistinguishable from following Location, which is a different "
			"answer and the state ADR 0024 refused to collapse");
		require(world->corridorFloorDepth() == 0,
			"The zero rung does not read back as zero");
		require(world->effectiveFloorDepth(fixture.corridor) == 0,
			"The Corridor answers " + std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ " on a zero rung with its type at 24; the rung is what holds it still");
		require(world->effectiveFloorDepth(fixture.room) == 24,
			"The zero rung pulled the Room off its type");
		require(world->layerFloorOffsetPx(1) == 24,
			"Layer 1 draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off; the Room at 24 is still the deepest floor in front, so "
				  "the held-still Corridor contributes nothing and the Room carries "
				  "the lift");

		// Re-stating the zero the rung already carries changes nothing.
		auto const again = world->setCorridorFloorDepth(0);
		require(again.accepted && !again.changed,
			"Re-setting the zero the rung already carries was reported as a change");
	}

	// Clearing the rung is a write of its own and not the same write as setting
	// it to zero: the zero holds Corridors still, the clear puts them back on
	// `Location`. Routing one through the other - as the type pair does - would
	// lose one of the two answers.
	void clearingTheCorridorRungPutsCorridorsBackOnTheirType(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Cleared rung");
		auto const& world = fixture.world;
		require(world->setSectorTypeFloorDepth(core::SectorType::Location, 24).changed,
			"Location default refused");
		require(world->setCorridorFloorDepth(8).changed, "rung refused");
		require(world->effectiveFloorDepth(fixture.corridor) == 8, "rung not in force");

		require(world->clearCorridorFloorDepth().changed,
			"Clearing a set corridor rung was not seen as a change");
		require(!world->hasCorridorFloorDepth(),
			"A cleared rung still reports itself as set");
		require(world->effectiveFloorDepth(fixture.corridor) == 24,
			"A Corridor whose rung was cleared answers "
				+ std::to_string(world->effectiveFloorDepth(fixture.corridor))
				+ "; with the rung gone it falls through to the Location type at 24, "
				  "which is not where a zero rung left it");
		require(world->sectorTypeFloorDepth(core::SectorType::Location) == 24,
			"Clearing the rung moved the Location type");

		// And clearing an already-clear rung is legal, changes nothing, and
		// leaves no second deletion to undo.
		auto const quiet = world->clearCorridorFloorDepth();
		require(quiet.accepted && !quiet.changed,
			"Clearing a rung that was never set was reported as a change");
	}

	// The same one-Level band every other floor depth write enforces, refused
	// rather than clamped, and named as the Corridor's band in the refusal
	// because the author is setting a rung under every Corridor and "out of
	// range" without a subject sends them looking for a Sector.
	void aCorridorDepthOutsideTheBandIsRefused(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Out-of-band rung");
		auto const& world = fixture.world;
		std::string diagnostic;
		require(!world->canSetCorridorFloorDepth(CORE_LEVEL_HEIGHT_PIXELS + 1, &diagnostic),
			"A corridor depth one pixel above the band was accepted");
		require(diagnostic.find("Corridor") != std::string::npos,
			"The refusal does not say which depth it is refusing: '" + diagnostic + "'");
		require(!world->canSetCorridorFloorDepth(-CORE_LEVEL_HEIGHT_PIXELS - 1, &diagnostic),
			"A corridor depth one pixel below the band was accepted");

		auto const write = world->setCorridorFloorDepth(9999, &diagnostic);
		require(!write.accepted, "A corridor depth far outside the band was accepted");
		require(!write.changed, "A refused corridor write reported a change");
		require(!world->hasCorridorFloorDepth(),
			"A refused corridor write left a rung on the World");
		require(world->effectiveFloorDepth(fixture.corridor) == 0,
			"The refused write reached the Corridor anyway");

		// The edges themselves are inside the band, both sides.
		require(world->canSetCorridorFloorDepth(CORE_LEVEL_HEIGHT_PIXELS, &diagnostic),
			"The top of the band was refused: " + diagnostic);
		require(world->canSetCorridorFloorDepth(-CORE_LEVEL_HEIGHT_PIXELS, &diagnostic),
			"The bottom of the band was refused: " + diagnostic);
		require(world->setCorridorFloorDepth(-CORE_LEVEL_HEIGHT_PIXELS).changed,
			"An edge-of-band corridor depth was not written");
		require(world->effectiveFloorDepth(fixture.corridor) == -CORE_LEVEL_HEIGHT_PIXELS,
			"The edge-of-band corridor depth did not reach the Corridor");
	}

	// Re-asking for the depth the rung already carries is legal and changes
	// nothing, which is what keeps a repeat keystroke from becoming an undo step.
	void reaskingForTheSameCorridorDepthChangesNothing(smoke::Context const&)
	{
		auto const fixture = roomAndCorridorInFront("Same rung value");
		auto const& world = fixture.world;
		require(world->setCorridorFloorDepth(33).changed, "rung refused");
		auto const again = world->setCorridorFloorDepth(33);
		require(again.accepted, "Re-asking for the same corridor depth was refused");
		require(!again.changed, "Re-asking for the corridor depth already carried "
			"was reported as a change");
		require(world->corridorFloorDepth() == 33,
			"The rung moved while being asked for the value it already had");
	}
}

void registerFloorDepth(std::vector<smoke::Check>& checks)
{
	checks.push_back({ "floorDepth/aWorldWithNoDepthsAnywhereIsFlat",
		aWorldWithNoDepthsAnywhereIsFlat });
	checks.push_back({ "floorDepth/anIndexOutsideTheWorldAnswersZero",
		anIndexOutsideTheWorldAnswersZero });
	checks.push_back({ "floorDepth/oneDepthLiftsEveryLayerBehindIt",
		oneDepthLiftsEveryLayerBehindIt });
	checks.push_back({ "floorDepth/depthsAccumulateBackwards",
		depthsAccumulateBackwards });
	checks.push_back({ "floorDepth/theDeepestFloorInFrontWinsNotTheSum",
		theDeepestFloorInFrontWinsNotTheSum });
	checks.push_back({ "floorDepth/aShallowerSectorBehindADeeperOneDoesNotLowerTheLift",
		aShallowerSectorBehindADeeperOneDoesNotLowerTheLift });
	checks.push_back({ "floorDepth/negativeDepthsPushTheLayersBehindDown",
		negativeDepthsPushTheLayersBehindDown });
	checks.push_back({ "floorDepth/opposingDepthsCancelAndCross",
		opposingDepthsCancelAndCross });
	checks.push_back({ "floorDepth/anEmptyLayerContributesNothing",
		anEmptyLayerContributesNothing });
	checks.push_back({ "floorDepth/theSingleStepAndTheWholeStaircaseAgree",
		theSingleStepAndTheWholeStaircaseAgree });
	checks.push_back({ "floorDepth/theStaircaseFollowsTheWorldWithNoInvalidation",
		theStaircaseFollowsTheWorldWithNoInvalidation });
	checks.push_back({ "floorDepth/settingZeroPinsAndClearingDeletesTheOverride",
		settingZeroPinsAndClearingDeletesTheOverride });
	checks.push_back({ "floorDepth/aCorridorDepthMovesCorridorsAndLeavesRoomsOnTheirType",
		aCorridorDepthMovesCorridorsAndLeavesRoomsOnTheirType });
	checks.push_back({ "floorDepth/aCorridorWithNoCorridorDepthFollowsTheLocationTypeDefault",
		aCorridorWithNoCorridorDepthFollowsTheLocationTypeDefault });
	checks.push_back({ "floorDepth/theCorridorRungReachesEveryCorridorAndNoOtherType",
		theCorridorRungReachesEveryCorridorAndNoOtherType });
	checks.push_back({ "floorDepth/aSectorOverrideBeatsTheCorridorDepth",
		aSectorOverrideBeatsTheCorridorDepth });
	checks.push_back({ "floorDepth/theStaircaseAccumulatesFromTheCorridorDepth",
		theStaircaseAccumulatesFromTheCorridorDepth });
	checks.push_back({ "floorDepth/aCorridorOnTheRungDoesNotOutrankADeeperRoom",
		aCorridorOnTheRungDoesNotOutrankADeeperRoom });
	checks.push_back({ "floorDepth/aZeroCorridorDepthHoldsCorridorsStillWhileRoomsLift",
		aZeroCorridorDepthHoldsCorridorsStillWhileRoomsLift });
	checks.push_back({ "floorDepth/clearingTheCorridorRungPutsCorridorsBackOnTheirType",
		clearingTheCorridorRungPutsCorridorsBackOnTheirType });
	checks.push_back({ "floorDepth/aCorridorDepthOutsideTheBandIsRefused",
		aCorridorDepthOutsideTheBandIsRefused });
	checks.push_back({ "floorDepth/reaskingForTheSameCorridorDepthChangesNothing",
		reaskingForTheSameCorridorDepthChangesNothing });
}
