#pragma once

#include <cstdint>

namespace core
{
	// One cell of a Sector's surface wearing a variant of its type's tile
	// rather than that type's base (#1040).
	//
	// Sector-local, not World-absolute. `column` counts World cells from
	// the Sector's own left edge and `level` counts them up from its floor
	// line, which is the same frame the per-cell surface loop resolves a
	// tile in, so an entry names the cell it was authored against rather
	// than a screen position that would go stale.
	//
	// Local coordinates are also what let the entries ride the Sector's
	// construction record. Every structural edit tears the World down and
	// replays the records, and a move changes only where the record puts
	// the Sector; entries held in World coordinates would have to be
	// rewritten by that move to stay pointing at the cells they were drawn
	// for, and an entry missed by the rewrite is artwork silently lost.
	// Held Sector-local, the record replays unchanged and the entries
	// follow the Sector for free.
	//
	// A resize is the other case, and the storage does not follow for free
	// there: an edit that moves the Sector's anchor re-points every surviving
	// local key at a different World cell unless the key is re-derived. The
	// resize paths re-key through World coordinates so the artwork stays on
	// the cell it was painted on (#1047); the value stored here changes, the
	// frame it is stored in does not.
	//
	// `variant` is the variant's number within the Sector type's family, as
	// the installed tileset declares it (`-V1` .. `-VN`, #1038). It is
	// deliberately not checked against the atlas here: the domain records
	// what was authored, and the renderer decides what a number the
	// installed artwork does not carry looks like - which is the base tile,
	// not a hole (#1040).
	struct SectorSurfaceVariant
	{
		uint32_t column{ 0 };
		uint32_t level{ 0 };
		uint32_t variant{ 0 };

		// Two entries name the same recording when all three of their parts
		// agree, which is what lets a caller ask whether a Sector's entries
		// survived a rebuild or a repoint rather than comparing them field by
		// field at every use.
		friend bool operator==(SectorSurfaceVariant const& left,
			SectorSurfaceVariant const& right) = default;
	};

	// What asking for a cell's variant did (#1051).
	//
	// Two answers, kept apart on purpose.
	//
	// `accepted` is whether the request was legal, which is the question
	// #1042's hover asks and the answer that paints a cell yellow rather
	// than red. It says nothing about whether anything was written.
	//
	// `changed` is whether the write put anything down. A cell already
	// wearing what it was asked for is perfectly acceptable and changes
	// nothing, and an undo step for it is a press that visibly does
	// nothing - a hole in the author's history that has to be walked back
	// by hand, and a document dirtied by a gesture that authored no thing.
	//
	// Collapsing the two into one bool is the mistake this type exists to
	// prevent. A caller that cannot tell "refused" from "already like
	// this" cannot decide whether to show a red affordance, and cannot
	// decide whether to spend a snapshot; one bool can serve neither of
	// those decisions without lying about the other.
	struct SurfaceVariantWrite
	{
		bool accepted{ false };
		bool changed{ false };
	};
}
