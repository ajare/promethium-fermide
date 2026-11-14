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
//   * *am I following my type?* - answered by naming the type and the value
//     being followed, so "Following Location at +24 px" answers both which
//     type governs this Sector and where that type currently puts it. The
//     type is named with `core::getSectorTypeString`, the very string the
//     Controls window's type table shows down its left column, so the row
//     and the table name the same thing the same way and a Sector's row can
//     be read straight back to the row that governs it;
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

	// What the row says about a Sector that follows its type: the type, and
	// the value that type currently carries.
	inline std::string floorDepthFollowText(core::SectorType type,
		std::int32_t followedDepthPx)
	{
		return "Following " + core::getSectorTypeString(type)
			+ " at " + floorDepthPxText(followedDepthPx);
	}

	// What the row says about a Sector that has broken away from its type.
	//
	// The type's own value is named alongside so the author can see the size
	// of the divergence without going to the type table to look it up, and
	// so Reset is not a jump into the unknown: it is a move back to the
	// number this sentence already shows.
	inline std::string floorDepthOverrideText(core::SectorType type,
		std::int32_t ownDepthPx, std::int32_t typeDepthPx)
	{
		return "Overriding " + core::getSectorTypeString(type)
			+ " (" + floorDepthPxText(typeDepthPx) + ") with own "
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
	// Reads the World rather than the Sector alone for the followed value:
	// the type's current default is a fact about the World, and the Sector
	// object has no way to answer it.
	inline std::string sectorFloorDepthStateText(core::World const& world,
		core::Sector const& sector)
	{
		auto const type = sector.getType();
		if (sector.hasFloorDepthOverride())
		{
			return floorDepthOverrideText(type, sector.getFloorDepth(),
				world.sectorTypeFloorDepth(type));
		}
		return floorDepthFollowText(type, world.sectorTypeFloorDepth(type));
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
