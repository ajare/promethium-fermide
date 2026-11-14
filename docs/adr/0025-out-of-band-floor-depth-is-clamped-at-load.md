# An out-of-band floor depth is clamped at load, not refused

**Status**: accepted

The band on a floor depth - one Level either way, `World::kFloorDepthBandPx` - was
enforced on every write path and on none of the read paths. A `.world` file is
plain YAML an author edits by hand, so a depth outside the band reaches the
renderer as typed: the panel reported 5000, the Layers behind drew by 5000, and
the value the editor would have refused was on screen anyway. The parent spec
(#1099) settled what should happen and #1111 found it unimplemented. Both read
sites now clamp on the way in: the per-Sector `floorDepth` field and the
per-type `depth` field of the type map.

## Considered options

**Refuse the document.** The honest-looking option, and the one the loader takes
for almost every other complaint. But one mistyped number in a file the author
edited by hand would then cost them the whole World, and the failure mode it
protects against - a screen torn by an absurd offset - is not made safer by
losing the document. The band exists to keep the picture sane, not to police the
file.

**Clamp with a diagnostic.** Better than silence in the abstract. Rejected
because there is nowhere to put it: the serialization layer has no warning
channel, every other load-time complaint throws, and a channel invented for this
one case would be a new dependency for the whole layer to carry. And the
diagnostic is not actually missing - the author who opens the clamped World sees
the clamped number in the panel, which is the same information, in the place
they were already looking.

**Clamp at draw instead of at load.** Rejected because the draw path is not where
the value lives. Clamping there leaves the loaded World holding a number the
write path would refuse, so the next save re-litigates it, and the panel keeps
reporting what was typed rather than what is drawn.

## Consequences

The clamp lands on the record, not just on the read. The World that loads with
`floorDepth: 5000` is a World whose Sector says 320, so the save that follows
writes 320 and the mistyped value is gone from the file rather than being
re-read on every launch. The smoke cases pin that: a clamped World saves back
inside the band, and the value the load produced is one `canSetSectorFloorDepth`
accepts, so the two ends of the document cannot disagree about where the band
sits.

No schema version bump. Clamping changes no field's presence - a value outside
the band was never writable, so no document that was legal before changes
meaning, and a document we clamped and re-saved reads identically on a build
from before this change.

The band is now stated once. `World::kFloorDepthBandPx` and
`World::clampFloorDepth` are the definition; the core floor-depth helpers and
the editor's panel slider derive from them instead of each restating
`CORE_LEVEL_HEIGHT_PIXELS` in their own words.

The zero asymmetry ADR 0024 left stands, and is now pinned as coherent rather
than as two paths that happen to differ. A stored zero on a Sector is a value:
it pins that Sector at the unlifted position against a type that lifts, and it
saves back out. A stored zero in the type map is refused: an absent type entry
already answers zero for every Sector following it, so the stored row would be
a state the writer cannot produce and the delete says the same thing more
cheaply. Absence means different things on the two paths - "follow the type" on
one, "this type has no opinion" on the other - which is why one keeps its zero
and the other does not.
