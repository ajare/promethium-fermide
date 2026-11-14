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
//   * Layer 0 never moves. The depth a Sector carries is a lift it hands
//     backwards and never to itself, which is why the offset of a Layer is
//     the sum of the Layers *in front* and not of its own;
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

	// Clearing is the same write as setting zero, down to the stored shape:
	// after either one, the Sector answers zero and the Layer behind stops
	// being lifted.
	void clearingAndSettingZeroAreTheSameWrite(smoke::Context const&)
	{
		auto const world = layeredWorld("Clear equals zero", { 0, 1 });
		auto const front = firstOn(world, 0);
		require(world->setSectorFloorDepth(front, 48).changed, "depth refused");
		require(world->setSectorFloorDepth(front, 0).changed,
			"Setting zero over 48 was not seen as a change");
		require(world->effectiveFloorDepth(front) == 0,
			"Setting zero left the Sector answering "
				+ std::to_string(world->effectiveFloorDepth(front)));
		require(world->layerFloorOffsetPx(1) == 0,
			"The Layer behind still draws " + std::to_string(world->layerFloorOffsetPx(1))
				+ "px off after the depth was set to zero");
		// And neither of them changes anything a second time.
		require(!world->setSectorFloorDepth(front, 0).changed,
			"Setting zero again was reported as a change");
		require(!world->clearSectorFloorDepth(front).changed,
			"Clearing an already-clear Sector was reported as a change");
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
	checks.push_back({ "floorDepth/clearingAndSettingZeroAreTheSameWrite",
		clearingAndSettingZeroAreTheSameWrite });
}
