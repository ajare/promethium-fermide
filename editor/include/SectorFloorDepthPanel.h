#pragma once

// The words the Sector selection panel says about a Sector's floor depth, and
// the words the Layers table says about a Layer's share of it (#1104).
//
// A floor depth is a signed count of pixels at 1x zoom saying how deep a
// Sector's floor runs into the screen. The World holds the facts - the
// per-type defaults, the per-Sector overrides, the accumulated per-Layer
// offsets - and this header only puts those facts into sentences. Nothing
// here decides anything, and nothing here reads a widget.
//
// The sentences live in a header rather than beside the widgets for the same
// reason `LayerSectorImageSet.h` keeps its two: what the author is *told*
// about a Sector's depth is the user-visible half of the feature, and a
// check that cannot reach the wording can only prove the World's state. A
// check that re-typed the format would pass against a panel that had since
// changed its wording, which is the drift this keeps out. The editor draws
// these strings; the headless editor check asserts them.
//
// Three sentences, one per thing the author asks while looking at a selected
// Sector:
//
//   * *am I following?* - answered by naming the thing being followed and
//     the value it currently carries, so "Following Location at +24 px"
//     answers both what governs this Sector and where that thing currently
//     puts it. The name comes from the resolver below rather than from the
//     Sector's type, because the type stopped being the whole answer when
//     #1115 put a second rung underneath one type: a Corridor is of
//     `SectorType::Location` type, the type it has always had, and what it
//     follows now depends on whether the author has given Corridors a depth
//     of their own. The resolver is what knows; nothing beside it derives
//     the pair from the type a second time.
//   * *what if I have broken away?* - answered by naming the Sector's own
//     value and, beside it, the type value it is no longer taking. The
//     followed value stays on screen while overridden because it is the thing
//     the author would go back to on Reset, and because a bare "own +12 px"
//     leaves the author no way to see how far from their type this Sector
//     has drifted;
//   * *where does it actually draw?* - the Layer's accumulated offset plus
//     the Sector's own depth, both halves spelled out beside the sum so the
//     author can see which part came from where rather than trusting a
//     single number.
//
// The sign is always spelled. A depth reads the same way in every sentence
// it appears in, and "+24" against "-12" makes the direction of the lift
// readable at a glance instead of something to work out from context.
//
// Sentences that need a Layer's accumulated offset take the staircase as an
// argument rather than asking the World for it. That is a cost decision, not
// a style one: `World::layerFloorOffsets` answers only by walking every
// Sector in the World, so a helper that asked on the caller's behalf was
// harmless once and ruinous in a loop - one whole walk per table row, which
// at a large World is tens of milliseconds for one panel (#1113). The
// caller builds the staircase once for the panel and hands it down; what
// the header does with it is an index and nothing more.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/SectorType.h"
#include "core/World.h"

namespace editor
{
	// The band a floor depth may be authored inside: one Level either way.
	//
	// An alias for the band the core owns, taken from there rather than
	// restated, so the input's tooltip and the core write - and the load that
	// clamps a hand-edited value into the same band (#1111) - cannot be
	// three places that each named 320 and drifted apart. The check pins the
	// editor's number to the Level height, because a band that quietly
	// stopped being one Level is worth a failing check.
	//
	// What this is is what the editor *says* the band is. It is not a gate,
	// and there is deliberately no `floorDepthIsInBand` beside it (#1114).
	// Whether a depth can be committed is the core write's answer -
	// `World::canSetSectorFloorDepth` - and the row reports that refusal
	// rather than pre-empting it. A helper here was a second gate the panel
	// never went through: the author's input travelled to the seam whatever it
	// answered, so a check pinning it proved nothing about the path a typed
	// number takes, and routing the panel through one would have answered the
	// author with the panel's wording instead of the core's, which names the
	// Sector the refusal is about.
	inline constexpr std::int32_t kFloorDepthBandPx = core::World::kFloorDepthBandPx;

	// The name the author reads for the corridor rung.
	//
	// The rung is not a Sector type - a Corridor is of `SectorType::Location`
	// type, the same type a Room is - so there is no `getSectorTypeString`
	// answer to take it from. It is written here once so the Controls table's
	// Corridor row and the selection panel's follow sentence cannot each name
	// the same rung differently (#1116).
	inline constexpr char const* kCorridorFloorDepthLabel = "Corridor";

	// What one Sector follows for its floor depth: the name of the thing it
	// follows, and the value that thing currently carries.
	//
	// A pair rather than two separate answers because the two are always read
	// together and were always derived together, and a call site that took one
	// without the other was how the two came to disagree with the World.
	struct FloorDepthFollow
	{
		std::string label;
		std::int32_t depthPx;
	};

	// The one resolver behind "what is this Sector following, and what is it
	// worth".
	//
	// Three places used to derive that pair from the Sector's type on their
	// own, which held while the type was the only rung there was and broke the
	// moment a second rung appeared underneath one type. A Corridor is of
	// `SectorType::Location` type, so a type-derived read answered the Room's
	// value for it and called it "Location" even after the author had given
	// Corridors a depth of their own - wrong in the number and wrong in the
	// name, in three places, each of which had to be found separately (#1115).
	//
	// The corridor rung is one branch here rather than a case each call site
	// invents. A Corridor with the rung set follows the rung and says
	// "Corridor". A Corridor with the rung unset follows the `Location` type
	// default and says "Location", because that is the value it actually
	// draws at: a bare zero in its place would have said "pinned at the
	// unlifted line" about a Corridor that is in fact lifting with its type,
	// and the two only look the same when `Location` is itself at zero, which
	// is the trap ADR 0024 is about.
	inline FloorDepthFollow resolveFloorDepthFollow(core::World const& world,
		core::SectorType type, bool isCorridor)
	{
		if (isCorridor && world.hasCorridorFloorDepth())
			return FloorDepthFollow{kCorridorFloorDepthLabel,
				world.corridorFloorDepth()};
		return FloorDepthFollow{core::getSectorTypeString(type),
			world.sectorTypeFloorDepth(type)};
	}

	// The same resolver read off a live Sector.
	//
	// Corridor identity is the core's test and not a second one invented
	// here: a Sector of Location type that is not a room is a corridor and
	// nothing else, which is exactly what `World::effectiveFloorDepth`
	// resolves on. A check pins this reading against that one, because a
	// sentence about what a Corridor follows that drifted from what the World
	// actually resolves would be the exact failure this resolver was written
	// to remove, rewritten in a place no one was looking.
	inline FloorDepthFollow resolveFloorDepthFollow(core::World const& world,
		core::Sector const& sector)
	{
		return resolveFloorDepthFollow(world, sector.getType(),
			sector.getType() == core::SectorType::Location && !sector.isRoom());
	}

	// A depth with its sign always spelled: "+24 px", "0 px", "-12 px".
	//
	// The sign is written for zero as well as for both sides of it, because
	// the alternative - a bare "0" among "+24" and "-12" - reads as a value
	// whose direction was left off rather than as a value with no direction.
	inline std::string floorDepthPxText(std::int32_t depthPx)
	{
		return (depthPx < 0 ? std::string{} : std::string{ "+" })
			+ std::to_string(depthPx) + " px";
	}

	// What the row says about a Sector that follows: the thing it follows,
	// and the value that thing currently carries.
	//
	// Takes the followed label rather than a Sector type. The type cannot
	// answer this question any more - two different things a Sector can follow
	// share one type - so the wording takes the name the resolver arrived at
	// and says nothing about how it was chosen.
	inline std::string floorDepthFollowText(std::string const& followedLabel,
		std::int32_t followedDepthPx)
	{
		return "Following " + followedLabel + " at "
			+ floorDepthPxText(followedDepthPx);
	}

	// What the row says about a Sector that has broken away from what it
	// followed.
	//
	// The followed value is named alongside so the author can see the size of
	// the divergence without going to the table to look it up, and so Reset
	// is not a jump into the unknown: it is a move back to the number this
	// sentence already shows. That is worth more for a Corridor than for a
	// Room, because the thing a Corridor would go back to is a row the author
	// set for Corridors and not the one in front of them.
	inline std::string floorDepthOverrideText(std::string const& followedLabel,
		std::int32_t ownDepthPx, std::int32_t followedDepthPx)
	{
		return "Overriding " + followedLabel + " ("
			+ floorDepthPxText(followedDepthPx) + ") with own "
			+ floorDepthPxText(ownDepthPx);
	}

	// Where the Sector draws: the Layer's accumulated offset plus the
	// Sector's own depth, with both halves shown beside the sum.
	//
	// The sum is what the author asked to see - the whole depth the picture
	// carries down to this Sector's floor line - and showing the two halves
	// keeps it auditable: a Sector that looks wrong on screen can be checked
	// against the Layer it is on and its own number without recomputing the
	// staircase by hand.
	inline std::string floorDepthEffectiveText(std::int32_t layerOffsetPx,
		std::int32_t ownDepthPx)
	{
		return "Effective " + floorDepthPxText(layerOffsetPx + ownDepthPx)
			+ " (Layer " + floorDepthPxText(layerOffsetPx)
			+ " + own " + floorDepthPxText(ownDepthPx) + ")";
	}

	// The whole "Floor depth" row's state sentence for one live Sector:
	// follow wording when it carries no override of its own, override
	// wording when it does.
	//
	// Both halves come out of the one resolver. It reads the World rather
	// than the Sector alone because what a Sector follows is a fact about the
	// World - the type's current default, or the corridor rung - and the
	// Sector object can answer neither.
	inline std::string sectorFloorDepthStateText(core::World const& world,
		core::Sector const& sector)
	{
		auto const followed = resolveFloorDepthFollow(world, sector);
		if (sector.hasFloorDepthOverride())
		{
			return floorDepthOverrideText(followed.label, sector.getFloorDepth(),
				followed.depthPx);
		}
		return floorDepthFollowText(followed.label, followed.depthPx);
	}

	// One row of the Controls -> Sectors floor depth table.
	struct FloorDepthRow
	{
		std::string label;          // what the author reads down the left column
		core::SectorType type;      // the type whose default the row writes
		bool corridor;              // true: the row writes the corridor rung
		std::int32_t shownDepthPx;  // the value the row shows
	};

	// The rows the floor depth table draws, in order.
	//
	// The `Location` type is drawn as two rows because the author holds two
	// ideas where the type has one: Rooms lift, and Corridors may lift
	// differently. The Room row writes the `Location` type default - the
	// only place that value can live - and the Corridor row writes the
	// corridor rung. Every other type is one row, named with the very string
	// the type table shows down its left column, so the split is a display
	// change over the row list and not a special case the drawing code has to
	// remember (#1116).
	//
	// The Corridor row shows the `Location` default when the rung is unset
	// rather than a bare zero. A zero in that seat reads as "Corridors are
	// pinned at the unlifted line", which is a real state the author can put
	// them in on purpose and is the opposite of following for every World
	// whose Location default is not zero. Showing the followed value says
	// what the Corridors actually do, and the row's own tooltip says which of
	// the two the author is looking at.
	inline std::vector<FloorDepthRow> floorDepthSchemeRows(
		core::World const& world)
	{
		std::vector<FloorDepthRow> rows;
		for (auto const type : core::allSectorTypes())
		{
			if (type == core::SectorType::Location)
			{
				rows.push_back(FloorDepthRow{"Room", type, false,
					resolveFloorDepthFollow(world, type, false).depthPx});
				rows.push_back(FloorDepthRow{kCorridorFloorDepthLabel, type, true,
					resolveFloorDepthFollow(world, type, true).depthPx});
				continue;
			}
			rows.push_back(FloorDepthRow{core::getSectorTypeString(type), type,
				false, world.sectorTypeFloorDepth(type)});
		}
		return rows;
	}

	// What the table's Corridor row says about its own state when the author
	// hovers it.
	//
	// The unset case is the one that needs the sentence. The row is showing a
	// number the author never set, and nothing else on the row distinguishes
	// "Corridors follow the Location default of +24" from "Corridors are
	// pinned at +24 by their own rung" - which is the one distinction the
	// whole rung exists to make.
	inline std::string corridorFloorDepthRowHint(bool hasCorridorDepth,
		std::int32_t locationDepthPx)
	{
		if (!hasCorridorDepth)
		{
			return "No corridor depth set: Corridors follow the Location default ("
				+ floorDepthPxText(locationDepthPx)
				+ "). Typing a value here gives Corridors a depth of their own, so "
				"they can hold while Rooms lift.";
		}
		return "Corridors draw at their own depth instead of at the Location "
			"default (" + floorDepthPxText(locationDepthPx)
			+ "). Clearing the rung is what puts them back to following.";
	}

	// One Layer's share of a staircase that has already been built.
	//
	// A Layer past the end answers zero, which is the answer
	// `World::layerFloorOffsetPx` gives for the same question: a staircase
	// shorter than the question asked is a Layer nothing has lifted, not a
	// miscount to report.
	inline std::int32_t layerFloorOffsetAt(
		std::vector<std::int32_t> const& layerOffsets, std::uint32_t layerIndex)
	{
		return layerIndex < layerOffsets.size() ? layerOffsets[layerIndex] : 0;
	}

	// The effective readout for one live Sector, read off the World's own
	// two answers: where its Layer draws, and how deep the Sector is.
	//
	// The staircase is passed in rather than asked for here so that no
	// sentence in this header hides a walk over the World behind a call that
	// looks as cheap as the ones above it (#1113). The other half of the
	// sum - the Sector's own effective depth - is a field read and a map
	// lookup, so the walk was the whole of what this used to cost.
	inline std::string sectorFloorDepthEffectiveText(core::World const& world,
		std::vector<std::int32_t> const& layerOffsets, core::Sector const& sector)
	{
		return floorDepthEffectiveText(
			layerFloorOffsetAt(layerOffsets, sector.getLayerIndex()),
			world.effectiveFloorDepth(sector.getIndex()));
	}

	// The Layers table's read-only "Floor offset" cell: that Layer's
	// accumulated pixels, in the same signed wording every other depth
	// sentence uses.
	//
	// Reads the frame's staircase rather than the World because this cell is
	// drawn once per Layer: asked of the World it was a walk over every
	// Sector in the World, once per row, every frame the table was open
	// (#1113).
	inline std::string layerFloorOffsetText(
		std::vector<std::int32_t> const& layerOffsets, std::uint32_t layerIndex)
	{
		return floorDepthPxText(layerFloorOffsetAt(layerOffsets, layerIndex));
	}
}
