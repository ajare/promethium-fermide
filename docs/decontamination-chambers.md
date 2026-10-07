# Decontamination Chambers

Paint a **Chamber**, then choose **Decontamination Chamber** in the Selection panel's Subtype dropdown. Security Scanner remains the painting default and capacity one. Switching requires an empty, paused Chamber without threshold crossings and uses structural replay and normal undo/redo; geometry, direction, sensor distance, and authored timings are preserved.

Decontamination retains automatic directional admission, interlocked Bulkhead Doors, entry sensing, destination Location requirements, Door Mobility, pre-delay, processing duration, post-pause, and committed forward exits. It adds no contamination properties or changes to Agents.

Capacity is one Agent per horizontal cell. Admission uses a fixed boarding deadline covering entry opening plus its normal open dwell, as with Airlock batching. Agents occupy distinct cell-centre standing positions, filling from the exit towards entry. The batch starts once its reservations have boarded, every occupant is positioned, and either capacity is filled or the deadline expires. Partial batches never wait indefinitely for more arrivals. Processing starts only after both Doors close. One shared sequence releases the entire batch; exit closure waits until every occupant has left. Inactive occupants retain their slots and committed exits, as with Security Scanner journeys.

## Low threshold passage (#488)

Both owned Bulkhead Doors classify their physical opening Standing-first, then
Crawling, otherwise impossible, through the shared Chamber clearance seam. They
still reject ordinary Door Height overrides. Route planning and entry admission
require both the entrance and the committed opposite exit to fit. Waiting Agents
remain Standing; only an admitted low crossing uses Crawling, at half normal walk
speed, with no Crouching stage or preparation/recovery delay. Standing returns
immediately at the far-side threshold vertex. Interior positioning and the
shared decontamination timers do not slow; captured route motion costs double
only for low thresholds.

Width-based capacity, ticket order, the fixed boarding deadline, distinct standing
slots, the closed-door shared sequence and single crossing permit remain
authoritative. Accepted reservations finish before the batch can process, even
if their slower crossings outlast the boarding deadline. An impossible exit or
Agent envelope before admission causes Route planning/loss without consuming a
slot. After admission, cancellation, replacement intent or envelope/opening
changes cannot revoke the committed forward exit. Pause freezes motion and
processing; inactive occupants retain capacity and freeze their crossing until
reactivation. Reset clears occupants, permits and transient Crawling. Permissions,
Mobility and Broken-device behavior are unchanged.

During processing only, one white quad covers the Chamber interior and its occupants, while walls and Doors remain visible. Its alpha rises linearly from zero to fully opaque over the first **25%** of processing duration, then immediately fades linearly to zero over the remaining **75%**, with no peak hold and no repeated pulses. There are no red beams. Rendering consumes simulation progress; global pause freezes the fade, and reset/load clears transient processing.

The Selection panel uses Decontamination timing/progress labels and shows actual occupancy versus cell-width capacity. Configuration ranges and defaults match Security Scanner. Timing edits during a paused occupied sequence affect the next sequence, not its active clock.

World schema **46** persists `subtype: decontamination` and capacity equal to `cellsWide` in YAML and binary construction records. Earlier Security Scanner/Chamber documents remain readable; decontamination in an older schema or with inconsistent capacity is rejected atomically. Reset, load, and structural replay reconstruct closed, empty Chambers and the two protected owned Doors.

Headless coverage is included in existing Chamber owners:

- Simulation `decontamination/crawlingJourneys`, `crawlingLifecycle`, `crawlingGates`, and `crawlingBatches`: independent Standing/low/impossible entry and exit in both directions, exact clearance, captured costs, half-speed motion, single and full/partial/repeated batches, shared processing duration, capacity/slots/interlocks, pause/deactivation, cancellation/replacement, envelope and opening changes before/after admission, Reset, permission/Mobility gates and terminal cleanup. Fixtures use public World, graph, inspection and Shape seams without authored height overrides.
- Simulation `securityScanners/automaticJourneys`: width/direction, full and partial batches, reuse, standing slots, interlocks, and pause/edit refusal.
- Render `securityScanners/beamSweeps`: white quad bounds, draw order, alpha ratio, and absence outside processing or of red beams.
- Editor `securityScanners/selectionWorkflow`: both dropdown choices, labels, configuration-preserving switching, and undo/redo.
- Persistence `securityScanners/authoredRoundTripAndReplay`: YAML/binary, reset/replay, subtype switching, and malformed schema/capacity rejection.

#488 validation: incremental Release build including `editor` passed; all
**70/70 repository CTests** passed with unmodified Willpower submodule tests
excluded. The Simulation inventory/CLI contracts and ownership audit include
`DecontaminationCrawling.cpp`.
