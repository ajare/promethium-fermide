# Broken Platform lifts (#319)

A Platform lift shares the whole-transport initial/live Broken condition with
Lifts. Breaking freezes its position and scheduler, fails pending operations,
rejects new commands and prevents new boarding/journeys. An already-claimed
virtual boarding boundary completes safely at the frozen Stop; it is not a new
admission. Passengers between Stops retain capacity and accepted journey intent,
without teleportation. Restoration resumes from that position using the normal
capacity, permissions, stop requests and safety interlocks. Pending onboard
selection is retried after restoration.

Unlike an enclosed Lift, the platform has no Doors. At an aligned Stop its
existing open-platform alighting opportunity remains available, including safe
exit after cancellation. Accepted journeys survive permission loss; this never
grants permission for a new journey. The mandatory ground Stop and selected
Walkway Stops are unchanged, and an unselected Walkway does not become a Stop.

## Discovery, controls and presentation

Agents observe from the owning Location, even while passing without the device
on their Path. They remember Broken status, physical position, Stop alignment
and open access. Remote routing uses the last local observation, never remote
live condition. Memories persist without expiry, notifications or repair-check
trips, until re-observation or reset. Unusable current Paths require Route
planning and can produce Route loss; usable alternatives/restoration use
voluntary planning and ordinary Route persistence. Boarded passengers retain
their journey until safe alighting.

Select the Platform lift object. **Initially Broken** is a paused-only, undoable
World edit; **Live Broken** works paused or running without changing the saved
initial state or history. Reset restores authored state and clears memories.
World schema 38 stores the optional Platform lift `initiallyBroken` property in
YAML and binary documents; older Worlds without it still operate normally.
The panel reports Broken status and platform y, and the amber canvas badge stays
above the physical platform in solid and wireframe views.

## Demo

Open `resources/test-worlds/broken-platform-lifts.world.yaml`:

1. Run. The remote observer discovers the unavailable Platform lift locally and
   loses its Route. No new passenger boards.
2. Select the platform and clear **Live Broken**. Send the recovery passenger to
   **Upper Walkway**. Break between Stops: the passenger remains aboard at the
   frozen position. Restore: the accepted journey resumes to level 3, passing
   the unselected level-1 Walkway without stopping.
3. Repeat, breaking on arrival at the upper Walkway; the passenger can alight,
   but a new passenger cannot board. Repeat descending to **Ground**.
4. Pause, change **Initially Broken**, undo/redo, save/reopen and reset. Live
   edits never replace the authored initial condition. An observer remaining
   in the remote room remembers stale condition until returning locally.

## Automated coverage

Simulation owns five `platformLifts/broken*` checks: frozen capacity/passengers,
Stop/selector safety, permissions/committed boarding/cancellation, individual
local memory, and mandatory/voluntary Route planning. Persistence owns
`platformLiftBrokenLifecycle` (YAML/binary, replay, reset, legacy compatibility
and demo). Editor owns `platformLifts/brokenControlsAndHistory`; Render owns
`platformLifts/brokenWarningPreservesPosition`. All checks are headless, with
CPU-only ImGui for controls and canvas inspection, no dialogs or GPU windows.
