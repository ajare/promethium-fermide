# Broken Lifts (#318)

An enclosed Lift has one initial and one live Broken condition. The condition
belongs to the whole transport, not a car, landing Door or button. The current
model has one car per Lift; all its generated landing Doors share that condition.
Platform lifts and Shuttles belong to separate tickets.

Breaking freezes car position, scheduler motion and landing/car Door positions.
Pending operations fail and new commands fail. No new boarding or journeys are
admitted. Boarded passengers keep capacity ownership and their accepted journey;
a pending destination selection is reissued after restoration. At the retained
destination Stop, already-open Doors permit alighting. Closed or partially open
Doors cannot open while Broken. Restoration continues from the frozen positions
using the ordinary interlocks, manifest, capacity and scheduling rules.

Agents observe service from its landing Locations or aboard the Lift, including
when passing without a Lift on their Path. Their individual memory records the
whole-Lift condition, car position, alignment and open-door condition. Path search
uses that memory for remote service, never remote live failure state. Memories
have no expiry or remote updates. Unusable Paths require Route planning and can
produce Route loss; restored alternatives use voluntary planning and Route
persistence. Already-boarded passengers retain their journey rather than replan
inside a frozen car.

## Controls and persistence

Select the Lift shaft. **Initially Broken** is an undoable authored edit, enabled
only while paused. **Live Broken** works paused or running and never modifies the
authored document or history. Reset restores the initial condition and clears
Agent memories. World schema 37 persists the optional `initiallyBroken` Lift
field in YAML and binary documents; earlier Worlds without it remain working.
The amber warning follows the physical car without covering it, in solid and
wireframe views. The panel retains car y and the ordinary scheduler details.

## Demo

Open `resources/test-worlds/broken-lifts.world.yaml`:

1. Run: the remote-origin observer initially plans through unknown service, then
   discovers the Broken Lift from the lower landing and loses its Route. The
   recovery passenger stays outside; no car or Door moves.
2. Select the shaft and clear **Live Broken**. Send the recovery passenger to the
   upper Marker. Break the Lift while the car is between Stops: the car and its
   passenger stay where they are. Restore it: the journey resumes and alights.
3. Repeat, breaking at the upper Stop while the landing Door is closed or partly
   open, then while it is fully open. Only the fully open case permits alighting.
4. While paused change **Initially Broken**, undo/redo, save/reopen, and reset.
   The live toggle must never replace the saved initial state. An observer who
   stays in the remote room does not learn a repair until returning locally.

## Headless coverage

Simulation owns four `lifts/broken*` checks for passenger recovery, Door/selector
safety, observation/memory and Route selection/planning. Persistence owns
`liftBrokenLifecycle` (YAML/binary, legacy loading, replay/reset and demo loading).
Editor owns `lifts/brokenControlsAndHistory`; Render owns
`lifts/brokenWarningPreservesPosition`. These run in the existing module runners;
Selection and canvas checks use CPU-only ImGui, with no dialogs, windows or GPU.
