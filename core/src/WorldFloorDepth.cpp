// Where a Sector's floor depth lives, and what the World says about it (#1101).
//
// A floor depth is a signed count of pixels at 1x zoom saying how deep a
// Sector's floor runs into the screen. It is artwork only: no traversal,
// capacity, device or Agent state reads it, and the Sector's own World
// coordinates are untouched by it. The one thing it changes is where the
// Layers behind draw.
//
// Three facts are stated here and nowhere else, which is the whole point of
// putting them in one file:
//
//   * the resolution of the sparse storage - a Sector with no recorded
//     depth lifts nothing, and answers zero rather than making every reader
//     remember what an absent field means. Since #1103 the resolution has a
//     middle step: a Sector with no override of its own takes the default
//     recorded for its type, and only where the type has none either does the
//     answer come back zero;
//   * the type defaults themselves (#1103) - the author's scheme, one signed
//     number per Sector type, held sparsely on the World so a type nobody set
//     has no opinion. This file is the only place that map is read or written,
//     which is what keeps the panel, the renderer and the shipped game from
//     each growing their own copy of the scheme;
//   * the accumulation rule - `offset(L) = offset(L-1) + max(depth of the
//     Sectors in Layer L-1)`, Layer 0 at zero. One statement of it, so the
//     renderer, the editor and the shipped game cannot each grow their own
//     version of the sum;
//   * the write - both halves at once, the live Sector that draws this frame
//     and the record that saves and replays, because a depth held only on
//     the live object evaporates on the first structural edit and one held
//     only on the record draws nothing until such an edit happens. The type
//     map is the exception that proves the rule: it is World state rather
//     than a projection of Sector state, so it has no second half to keep in
//     step and nothing about it can evaporate on a rebuild.
//
// The accumulation takes its maximum over every Sector in the Layer in
// front, which gives exactly one offset per Layer and a uniform shift
// across the whole screen. The alternative considered - a per-Sector offset
// derived from what sits directly ahead - was rejected because it seams
// where two adjacent back Sectors disagree about how high they sit, and
// makes a back Room slide when an unrelated Room on the far side of the
// map is edited. A Layer with no Sectors contributes a maximum of zero, so
// the recursion needs no special case at the back.
//
// Nothing here caches. `layerFloorOffsets` walks the Sectors every time it
// is asked, and the world draw pass asks it once per frame. Caching on the
// World behind an invalidation flag was rejected: the flag would have to be
// cleared by the authoring seam, by undo, by redo, by World load and by
// World rebuild, and missing one of those paths leaves the picture
// disagreeing with the panel the author is reading the number off. The walk
// is over Sectors the renderer is about to visit anyway, and the array is
// at most CORE_MAX_LAYERS entries.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Defines.h"
#include "core/Sector.h"
#include "core/World.h"

namespace core
{
	using namespace std;

	namespace
	{
		// The band a floor depth may be authored inside: one Level either way
		// (#1101). A depth is a perspective cue, not a way of moving a
		// Layer off the screen, and a mistyped 5000 would fling every Layer
		// behind the Sector that carried it out of sight and leave the author
		// hunting for the one Sector responsible.
		//
		// An alias rather than a statement of its own: the band lives on the
		// World, which is also where the load clamps a hand-edited value into
		// it (#1111). Two numbers for one band is two numbers that can disagree.
		constexpr int32_t kMaxFloorDepth = World::kFloorDepthBandPx;
	}

	int32_t World::effectiveFloorDepth(uint32_t sectorIndex) const
	{
		if (sectorIndex >= mSectors.size()) return 0;
		auto const& sector = mSectors[sectorIndex];
		// Two steps, in this order, and the order is the feature. The Sector's
		// own field wins when it has one - that is what an override means, and
		// it is what keeps an overridden Sector still while its type is moved
		// under it. Only where the Sector has no opinion of its own does the
		// type get asked, and only where the type has none either does the
		// answer come back zero (#1103).
		if (sector->hasFloorDepthOverride()) return sector->getFloorDepth();
		return sectorTypeFloorDepth(sector->getType());
	}

	bool World::hasFloorDepthOverride(uint32_t sectorIndex) const
	{
		if (sectorIndex >= mSectors.size()) return false;
		return mSectors[sectorIndex]->hasFloorDepthOverride();
	}

	int32_t World::sectorTypeFloorDepth(SectorType type) const
	{
		auto const found = mSectorTypeFloorDepths.find(type);
		// No entry is the ordinary case, not a miscount: a type nobody set has
		// no opinion, and no opinion lifts nothing.
		return found == mSectorTypeFloorDepths.end() ? 0 : found->second;
	}

	bool World::hasSectorTypeFloorDepth(SectorType type) const
	{
		return mSectorTypeFloorDepths.count(type) != 0;
	}

	vector<int32_t> World::layerFloorOffsets() const
	{
		auto const layerCount = getLayerCount();
		// The deepest floor each Layer contributes, before the running sum.
		// Held as an optional rather than seeded at zero, because zero is not
		// the bottom of the range: a Layer whose Sectors are all negative
		// contributes that negative and pushes the Layers behind it down.
		// Seeding at zero would swallow every negative depth in the World and
		// leave the picture flat, which is the one thing a negative depth is
		// authored to not be. An empty Layer stays unset and contributes
		// nothing, which is the whole of what an empty Layer means to this
		// rule and why no fallback is needed.
		vector<optional<int32_t>> deepest(layerCount);
		for (uint32_t index = 0; index < mSectors.size(); ++index)
		{
			auto const& sector = mSectors[index];
			auto const layer = sector->getLayerIndex();
			if (layer >= layerCount) continue;
			// The effective depth, not the Sector's own field: a Sector with no
			// override contributes its type's default, which is what makes a
			// type-table edit show up in the staircase without the table having
			// to walk the Sectors it governs (#1103).
			auto const depth = effectiveFloorDepth(index);
			if (!deepest[layer] || depth > *deepest[layer])
				deepest[layer] = depth;
		}

		// Layer 0 draws where it was authored; every Layer behind it draws
		// by the sum of the depths in front. The offset is a lift, so a
		// positive depth moves the Layers behind up and a negative one
		// pushes them down.
		vector<int32_t> offsets(layerCount, 0);
		int32_t running = 0;
		for (uint32_t layer = 0; layer < layerCount; ++layer)
		{
			offsets[layer] = running;
			if (deepest[layer]) running += *deepest[layer];
		}
		return offsets;
	}

	int32_t World::layerFloorOffsetPx(uint32_t layerIndex) const
	{
		// Asked through the whole staircase rather than summed on its own, so
		// the two answers cannot drift apart. A single Layer asked out of
		// order is the same sum, and building the array is the same walk over
		// the Sectors either way.
		auto const offsets = layerFloorOffsets();
		return layerIndex < offsets.size() ? offsets[layerIndex] : 0;
	}

	bool World::canSetSectorFloorDepth(uint32_t sectorIndex, int32_t depth,
		string* diagnostic) const
	{
		auto reject = [&](string text)
			{ if (diagnostic) *diagnostic = std::move(text); return false; };

		if (sectorIndex >= mSectors.size())
			return reject("There is no Sector " + to_string(sectorIndex)
				+ " in this World");

		// The band, stated once. The Sector's own name is in the refusal
		// because the author is looking at a selected Sector when this fires,
		// and "out of range" without a subject sends them looking in the wrong
		// place.
		if (depth > kMaxFloorDepth || depth < -kMaxFloorDepth)
			return reject(to_string(depth) + " px is outside the floor depth band of "
				+ to_string(kMaxFloorDepth) + " px either way, so "
				+ mSectors[sectorIndex]->getName()
				+ " would lift the Layers behind it off the screen");

		// A Sector with no creating record has nowhere for the depth to be
		// saved, so accepting it would draw today and be gone tomorrow with
		// no word said. Every Sector a World builds has a producing record,
		// so this refuses broken wiring rather than an ordinary case.
		if (!findCreatingRecord(sectorIndex))
			return reject(mSectors[sectorIndex]->getName()
				+ " was not created by a construction record, so a floor depth "
				  "recorded on it could not be saved");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::FloorDepthWrite World::setSectorFloorDepth(uint32_t sectorIndex,
		int32_t depth, string* diagnostic)
	{
		string reason;
		if (!canSetSectorFloorDepth(sectorIndex, depth, &reason))
		{
			if (diagnostic) *diagnostic = reason;
			return { false, false };
		}

		auto& record = mConstructionRecords[static_cast<size_t>(
			findCreatingRecord(sectorIndex) - mConstructionRecords.data())];
		auto& sector = mSectors[sectorIndex];

		// The override is the presence of the value, and since #1112 zero is
		// a value like any other. A Sector pinned to zero while its type
		// lifts is a real state the author asked for - "draw unlifted here
		// even though my type lifts" - and collapsing zero to nothing made
		// it unrepresentable, which is what disabled the editor's Break
		// away over a type at zero. What stays absent is the override
		// itself: `clearSectorFloorDepth` removes the field, and only that
		// puts the Sector back to following.
		auto const stored = optional<int32_t>{ depth };

		// Compared against both halves of what the write touches, the way
		// the variant write does. Either one differing is a change worth the
		// write; neither is left behind while the other is written.
		if (record.floorDepth == stored && sector->mFloorDepth == stored)
		{
			// Legal, and nothing to write: no `modify()`, and the document
			// stays as clean as it was.
			if (diagnostic) diagnostic->clear();
			return { true, false };
		}

		record.floorDepth = stored;
		sector->setFloorDepth(stored);
		modify();
		if (diagnostic) diagnostic->clear();
		return { true, true };
	}

	World::FloorDepthWrite World::clearSectorFloorDepth(uint32_t sectorIndex,
		string* diagnostic)
	{
		// Clearing deletes the override rather than writing zero (#1112).
		// It used to go through `setSectorFloorDepth(_, 0)` because the two
		// were one state; now that a zero override is expressible - a
		// Sector pinned to the unlifted position while its type lifts -
		// only removing the field puts the Sector back to following. The
		// same refusals apply, asked through the same door with a zero that
		// is trivially inside the band.
		string reason;
		if (!canSetSectorFloorDepth(sectorIndex, 0, &reason))
		{
			if (diagnostic) *diagnostic = reason;
			return { false, false };
		}

		auto& record = mConstructionRecords[static_cast<size_t>(
			findCreatingRecord(sectorIndex) - mConstructionRecords.data())];
		auto& sector = mSectors[sectorIndex];

		if (!record.floorDepth && !sector->mFloorDepth)
		{
			// Legal, and nothing to write: no `modify()`, and the document
			// stays as clean as it was.
			if (diagnostic) diagnostic->clear();
			return { true, false };
		}

		record.floorDepth.reset();
		sector->setFloorDepth(optional<int32_t>{});
		modify();
		if (diagnostic) diagnostic->clear();
		return { true, true };
	}

	bool World::canSetSectorTypeFloorDepth(SectorType type, int32_t depth,
		string* diagnostic) const
	{
		auto reject = [&](string text)
			{ if (diagnostic) *diagnostic = std::move(text); return false; };

		// The same band the per-Sector write enforces, stated once and reused
		// rather than restated with its own constant: a type default is the same
		// kind of number wearing a wider hat, and a type is exactly where a
		// mistyped value does the most damage, because it moves every Sector
		// following that type rather than one.
		if (depth > kMaxFloorDepth || depth < -kMaxFloorDepth)
			return reject(to_string(depth) + " px is outside the floor depth band of "
				+ to_string(kMaxFloorDepth) + " px either way, so a "
				+ getSectorTypeString(type) + " default would lift the Layers behind "
				+ "every " + getSectorTypeString(type) + " off the screen");

		if (diagnostic) diagnostic->clear();
		return true;
	}

	World::FloorDepthWrite World::setSectorTypeFloorDepth(SectorType type,
		int32_t depth, string* diagnostic)
	{
		string reason;
		if (!canSetSectorTypeFloorDepth(type, depth, &reason))
		{
			if (diagnostic) *diagnostic = reason;
			return { false, false };
		}

		// A type set back to zero loses its entry rather than storing the zero.
		// This is the collapse #1112 removed from the per-Sector write, and it
		// stays here because the two cases are not alike: what a type is asked
		// is "what do the Sectors following you answer", and a type with no
		// entry already answers zero for all of them. A stored zero would say
		// nothing no entry does not already say, and would cost the table the
		// ability to tell "nobody set this type" from "someone set it to 0" -
		// which is the one distinction the table's unset rows are drawn to
		// show. The Sector's zero was different: there the absence meant
		// *follow*, so collapsing zero into it destroyed the state of holding
		// still while the type lifts.
		if (depth == 0)
		{
			auto const removed = mSectorTypeFloorDepths.erase(type);
			if (removed == 0)
			{
				// Legal, and nothing to write: no `modify()`, and the document
				// stays as clean as it was.
				if (diagnostic) diagnostic->clear();
				return { true, false };
			}
			modify();
			if (diagnostic) diagnostic->clear();
			return { true, true };
		}

		auto const existing = mSectorTypeFloorDepths.find(type);
		if (existing != mSectorTypeFloorDepths.end() && existing->second == depth)
		{
			// Already carries it. Accepted, changed nothing, no undo step.
			if (diagnostic) diagnostic->clear();
			return { true, false };
		}

		mSectorTypeFloorDepths[type] = depth;
		modify();
		if (diagnostic) diagnostic->clear();
		return { true, true };
	}

	World::FloorDepthWrite World::clearSectorTypeFloorDepth(SectorType type,
		string* diagnostic)
	{
		// Same write as setting a type to zero, through the same door: two
		// implementations of "no opinion" is two chances to disagree about
		// what gets saved. Note this is *not* the per-Sector pair, where
		// setting zero and clearing are two different states and clearing is
		// the only one that ends the override (#1112). For a type the two are
		// genuinely one, because a type with no entry answers zero for every
		// Sector following it.
		return setSectorTypeFloorDepth(type, 0, diagnostic);
	}

	void World::applyFloorDepth(shared_ptr<Sector> const& sector,
		ConstructionRecord const& record)
	{
		// A record with no depth leaves the Sector it built with no depth,
		// which is what a Sector built from a document written before this
		// field existed ends up with: nothing to apply, nothing to lift.
		// A record carrying zero applies the zero override as written
		// (#1112): the Sector is pinned to the unlifted position, which
		// is not the same state as carrying nothing while its type lifts.
		if (!sector || !record.floorDepth) return;
		sector->setFloorDepth(record.floorDepth);
	}
}
