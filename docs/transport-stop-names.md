# Transport Stop short names

Every enclosed Lift, open Platform lift and Shuttle Stop has an authored short
name for display in a Player destination selector. The default is the zero-based
Stop number as a decimal string (`"0"`, `"1"`, etc.). Labels do not change Stop
identity, scheduling, routing, permissions or admission. Duplicate labels are
allowed; a destination is still selected by its Stop identity, not its name.

In the editor's selected transport panel, the **Destination permissions** table
includes a **Short name** column. Edit while paused and press Enter or leave the
field to apply. Letters typed into the field are uppercased. Labels must contain
1–3 ASCII characters from `A–Z` and `0–9`; empty labels, spaces, punctuation and
non-ASCII characters are refused. Edits support Undo/Redo.

`World::getTransportStopShortName()` exposes the label by Sector/Stop (with an
object index for a Platform lift) or Traversal resource/Stop.
`setTransportStopShortName()` validates paused authored edits. Creation options
may supply a complete `stopShortNames` vector parallel to `stopOffsets`, or omit
it to use numeric defaults. Transports allow at most 1000 Stops so every default
name fits the three-character limit.

World schema 65 persists optional `stopShortNames` arrays. Older Worlds receive
numeric defaults, malformed labels or mismatched lists are rejected atomically,
and YAML/binary round trips, Reset and document history retain authored labels.
Surviving labels follow the same structural Stop identity as destination
permissions when Stops move or are reindexed; new Stops receive numeric defaults.
Supported Platform lift clipboard payloads retain names for matching relative
Stops. Player boarding/destination-selection controls remain separate work;
this change supplies their display metadata, not new transport controls.

Release regression checks: `transport-stop-names-persistence` and
`transport-stop-names-editor`.
