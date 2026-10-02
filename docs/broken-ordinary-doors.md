# Ordinary Doors: Broken condition (#314)

This slice covers ordinary Doors only, in all activation modes and opening styles.
Bulkhead Doors, transports, extensible devices, Escalators, Windows and buttons
have no independent Broken controls in this slice.

## Authoring and simulation

Select an ordinary Door. **Initially Broken** is a paused, undoable document edit;
**Live Broken** breaks/restores the simulated Door, even while paused, without
changing the authored setting or dirtying the document. World schema 33 stores
`initiallyBroken` on Door construction records; omission defaults to working.
Reset rebuilds authored state and discards every Agent's device memories.

Broken freezes the leaf and its timer. New operations fail, including pending
commands; no opening style, activation mode or permission requirement changes.
Only a physically 100%-open Broken Door admits new crossings. Admitted crossings
finish and release their permits and leases. Restoration retains position and
motion direction, applies ordinary obstruction/lease safety, and accepts fresh
requests. It does not force the Door shut.

The Selection panel reports Broken separately from physical opening percentage.
An amber triangle/exclamation above the canvas threshold leaves its leaf visible.

## Individual knowledge and planning

Agents observe all ordinary Doors in their current Location during intent
collection, whether approaching, passing, or idle, not just Doors on their Path.
They remember Broken status and physical opening fraction. Local observations
replace old knowledge in either direction. Remote path search uses those values
(or ordinary baseline expectations when unknown), never remote live failure or
repair state. Live toggles send no Agent notifications; memories neither expire
nor cause repair-checking trips.

A discovery invalidating the remaining Path enters mandatory Route planning and
produces Route loss if no replacement exists. Usable changes/restoration enter
voluntary planning with ordinary Route persistence. Already-admitted crossings
are excluded from invalidation; repeated observations never resample an episode.

## Demo

Open `resources/test-worlds/broken-ordinary-doors.world.yaml` and resume:

- The Door at x=2 starts Broken and closed; the local observer uses x=8 instead.
- The remote observer initially does not know about x=2. On reaching the Back
  hall it discovers the failure, pauses for Route planning, and uses x=8.
- Select x=2 and uncheck **Live Broken**. Local Agents refresh their knowledge;
  remote Agents keep their last observations. Reset restores the authored break.
- Reset, uncheck **Initially Broken**, resume and break a Door while its leaf is
  opening: the frozen partial aperture blocks new admissions. Restore it to resume.
- Break a fully open Door: Agents can still cross it. Break during an admitted
  crossing: the crossing completes safely.

The fixture is loaded and both journeys completed by the automated lifecycle
check. Manual visual verification is not required or claimed.

## Headless verification

Run `pf-smoke-simulation --check brokenDoors/frozenPositionAndCommands`,
`brokenDoors/operationsAndAdmittedCrossings`, `brokenDoors/individualLocalMemory`,
and `brokenDoors/discoveryPlanningAndPersistence` (repeat the executable and
`--check` for each). Persistence owns `ordinaryDoorBrokenLifecycle`; Editor owns
`doorpanel/checkBrokenControlsAndHistory`; Render owns
`doorOpenRight/checkBrokenWarningPreservesPosition`. All use production APIs and
CPU-only rendering; none opens a native window or dialog.

Validated on Linux with GCC: complete Release and Debug GUI-enabled builds with
`PF_HIGH_ANALYSIS=ON`; all 84 runnable CTests passed in each configuration at
`-j 6` (the optional vendored GUI capability check was skipped). Displays were
unset, controlled Startup failure checks passed, and `git diff --check` passed.
