# Player-controlled Agents

Place an ordinary Agent in the editor, then choose **Player controlled** in its
Behaviour picker (the Agents table or selected-Agent panel). This immutable
built-in is always available, even without an Agent behaviour registry. It
replaces the previous behaviour and needs no configuration. Any Agent type and
any number of Agents can use it.

Editor simulation leaves these Agents idle. In the app, the eligible Agent with
the lowest stable Agent ID receives keyboard input; all other eligible Agents
idle. An inactive first Agent remains selected but does not move. With no
eligible Agent, Player controls are disabled and the existing fitted Layer-0
camera is used.

- Hold Left or Right to walk immediately at effective walking speed.
- Release the key to stop walking. Holding both keys stops walking; moving
  Escalator steps still carry the Agent.
- Pause, reset, suspension, and leaving Play clear transient movement input.
- The camera centres on the controlled Agent at normal zoom and shows its Layer.
- In Locations, Left/Right movement stays on the current Layer and Level. Usable open boundaries
  and standalone Bulkhead Doors whose moving leaf clears the Standing body permit entry into adjacent Locations,
  subject to Standing fit and existing passage restrictions.
- Press **Up** to cross an eligible fully open ordinary Door into its connected
  Location on the adjacent Layer, in either direction. If no crossing can start,
  Up instead opens an eligible manual Door. Press again after it finishes opening
  to cross; holding Up or keyboard repeat never queues another action.
- The Agent must be on the Door's Level in one of its connected Locations. Opening
  requires its centre inside the Door's physical X-bounds; crossing requires its
  entire Standing body inside the aperture. There is no automatic alignment.
- Player opening ignores arm length and remote activation distance, but preserves
  Object usage capability, Door Mobility, Access permissions and Broken state.
  None cannot open a Door, but can cross an already open eligible doorway. Existing
  Permission adherence and destination Location requirements still govern passage;
  a fully open Broken Door can remain passable.
- Crossing uses normal Standing Door-context speed, admission lanes and safety
  leases. Left/Right and further Up presses are ignored while crossing. Pause
  freezes an admitted crossing; it finishes safely on resume.
- Press **Up** inside a Stairwell landing doorway to enter the Stairwell sector,
  or to exit into that Level's adjacent Location. Standing body fit, Stairwell
  Mobility and destination Location permissions apply; Up preserves X and does
  not align the Agent. Open Stairwell entry does not require Object usage capability.
- Stairwells have visible horizontal side platforms at quarter-Level bends and
  on both sides of Level doorway junctions. Branches target 0.25 World units;
  bends move inward so vertices clear both walls by half the widest installed
  Agent type's Standing width. Wider types count even when not placed. Installing
  a type that changes existing platform geometry requires a paused, unoccupied
  Stairwell; a type too wide for the two-cell sector is refused.
- Left/Right walks on platforms at effective walking speed. At departure onto
  a flight, hold **Up** to ascend or **Down** to descend. Selection is sampled at
  departure, never stored in advance: neither or both keys stops at the junction.
  If the chosen flight points against horizontal input, change Left/Right to match;
  there is no automatic turn. During a departure attempt, Up selects rather than
  exits the doorway; otherwise a fresh Up press keeps its doorway-exit meaning.
- Flights use effective ascent/descent speed. On arrival, horizontal input walks
  onto the side platform rather than automatically entering another flight.
  Reverse toward the junction and hold the desired selection to depart again.
  Reversing mid-flight walks back toward its source. Releasing keys stops exactly
  where the Agent is, retaining admitted narrow-stair capacity until reaching an
  endpoint. Reversal is refused while others share the narrow span. Flat branches
  do not claim stair capacity. Pause freezes movement and clears all arrow input.
  Off-centre doorway entry still preserves X and can walk explicitly to the mount.
- Staircases and Escalators mount through their lower or upper flight endpoint, not
  a doorway. Hold **Up** to ascend from below or **Down** to descend from above,
  together with Left/Right toward the flight. Selection is checked only within
  **0.25 World units** horizontally outside the endpoint, on its Level in its
  landing Location; neither or both selection keys refuse mounting. There is no
  queued selection or automatic alignment: the Agent walks to the exact endpoint
  before entering. Eligible stair approach takes precedence over Up's ordinary
  Door action only within that zone.
- On a stationary Staircase, Left/Right follows the slope at effective directional stair
  speed. Release stops exactly in place; reverse input retraces the flight.
  Up/Down is not required once mounted. Reaching either endpoint automatically
  dismounts into its connected Location, subject to Standing fit and Location
  permissions. An open upper Room wall without Floor lands directly in the
  adjacent supported Location. Pause freezes the admitted task and clears input;
  Staircases and Escalators do not require Object usage capability or Buttons.
- On a moving Escalator, Left/Right adds effective walking speed in the selected
  direction to the signed belt speed along the slope. Either endpoint can be
  mounted, including against the belt; autonomous Agents still route one way.
  Walking against the belt makes progress only when walking speed exceeds belt
  speed, holds position when equal, and is carried backwards when slower.
  Releasing Left/Right (or holding both) rides the belt to an automatic endpoint
  exit. Broken Escalators behave as stationary Staircases, using directional
  stair speed and stopping on release. Breaking/restoring preserves position;
  resumed belt motion applies immediately. Moving Escalators use Escalator
  Mobility; Broken ones use Staircase Mobility. Pause freezes both walking and
  belt carriage, and resume with cleared keys rides a working belt.
- Fixed Ladders and fully extended extensible Ladders use contextual endpoint mounting. On the landing
  Level, walk toward the mount while holding **Up** from below or **Down** from
  above, within **0.25 World units** of its centre. Either horizontal approach
  side works; neither or both vertical keys refuses mounting. The Agent walks
  to the exact mount before entering, with no snapping or automatic alignment.
  Eligible Ladder approach suppresses Up's Door action. This supports both
  Ladders inside multi-Level Rooms and Ladder Transits behind their landings.
- Once mounted, hold **Up/Down** to climb at effective climb speed, without
  Left/Right. Release or hold both vertical keys to stop exactly; opposite
  vertical input reverses. Horizontal input does not move a mounted climber.
  Reversal is refused while another climber shares the span. Stopping retains
  admitted Ladder capacity, including for Room Ladders, until a safe endpoint.
  Reaching either connected endpoint automatically dismounts; intermediate
  Levels without a mount are not exits. Standing clearance, Ladder Mobility,
  resource admission and landing Location permissions remain authoritative.
  A refused exit allows retracing the Ladder. Pause freezes the admitted task
  and clears input; Reset restores the authored landing and discards mounted
  state. Fixed Ladders require neither Object usage capability nor Buttons.
  Extensible Ladders retain their existing Buttons Mobility, permission and
  admission gates. Retracted or extending Ladders cannot mount. Up at an
  extension Button toggles the Ladder between extended and retracted, without approaching or mounting it;
  wait for full extension, then explicitly walk to its mount. Safe retraction
  waits for admitted climbing leases to drain, including stopped Room climbers.
- A fresh **Up** press, when no higher-priority climb selection, Door opening or
  doorway crossing can occur, presses one eligible physical **Button**. Button
  pressing is always the lowest-priority Up action. Holding Up or keyboard
  repeat does not generate additional presses.
- Buttons require **Arms** Object usage, effective arm length, the Interaction
  point's geometric reach, the owning Location and Level, Buttons Mobility and
  Access permissions. Permission adherence never bypasses authorization for
  operation. None and Remote control do not gain physical Button operation.
  Reach is inclusive and uses the existing floor-Level approach point, including
  for vertically stacked Buttons; the effective limit is the smaller of arm
  length and Interaction point reach. Disabled Buttons and Broken target devices
  are not selected. Among eligible Buttons choose the nearest approach point,
  then lowest stable Interaction point ID. Each stack member retains its own
  command and permission requirement.
- Pressing uses normal Interaction queues, durations and typed Device operations.
  It never walks, snaps or aligns the Player. An unpressed queued/active request
  is cancelled if the Player leaves reach or begins crossing; capability and
  authorization changes retain existing cancellation/rejection rules. Accepted
  device operations retain their normal safe completion behavior. Physical
  controls can open controlled Doors, toggle Force Bridges/Ladders, switch lights
  or issue existing transport calls; calling a transport does not board it.
  Retraction closes new admission immediately and waits for reservations and
  occupants to clear, including stopped manual Force Bridge walkers. Autonomous
  route preparation still requests extension, never a toggle.
- Overlapping doorway candidates prefer an admissible open doorway, then nearest centre,
  then stable traversal-resource identity. One press performs at most one action.
- Solid walls, Bulkhead Door leaves overlapping the Standing body, low spaces and unsupported Floor
  edges stop the Agent's leading body edge, except at a selected open Staircase
  endpoint where the flight supplies support. Input never automatically
  crouches/crawls. Up supports ordinary Doors between Locations, open enclosed
  Lift/Shuttle Doors, Stairwell landing doorways, contextual Ladder entry/climbing
  and physical Button interaction. Windows and BoothWindows do not admit entry.
- An Agent initially in a space that cannot accommodate Standing remains
  stationary in its existing fitting pose; input does not force it through the
  ceiling.

## BoothWindow shutters

- A fresh **Up** press within reach of a standalone BoothWindow's owned **back-side
  control** toggles its shutter open/closed. The Player must be in that back-side
  Location on the control's Level, with Arms capability, effective arm length,
  Buttons Mobility and every required Access permission. Permission adherence
  does not bypass operation authorization.
- BoothWindow controls share physical Buttons' lowest Up priority, after contextual
  climbing and Door actions. Eligible controls use nearest approach point, then
  stable Interaction point ID. Holding Up or keyboard repeat never retoggles.
- Operation uses normal Interaction queues and shutter animation, without approach
  automation or alignment. Busy accepted requests retain normal deduplication;
  after completion another fresh press can toggle again. Leaving reach cancels
  an unactivated request; an accepted toggle still finishes safely.
- The front side cannot operate the shutter. BoothWindows remain non-traversable.
  Dumbwaiter-owned apertures retain their landing-button/interlock ownership and
  do not acquire independent shutter controls. Pause freezes animation; Reset
  restores the authored shutter state.

## Manual Airlocks

- A fresh **Up** press at an eligible outside Button requests its Airlock entrance
  using ordinary physical reach, capability and authorization checks. Calling
  never approaches, aligns or enters the Player.
- Hold **Left/Right** toward the entrance to enter as soon as the moving Door's
  physical bounding box clears the Standing body, even before full opening.
  This Player-only collision rule also applies to standalone and other chamber
  Bulkhead thresholds; autonomous Agents still wait for full opening.
  Admission uses the
  shared batch's capacity, reservations, active side and boarding deadline, and
  validates Door/Buttons Mobility, permissions, Standing fit through both Doors
  and a supported opposite landing. Older reserved entrants keep their turn.
- Left/Right walks inside at effective walking speed, including during closure
  and cycling. Doorway crossings use the Standing Door-context speed ratio.
  Release stops exactly, retaining admitted capacity or a partial threshold
  crossing; reverse through a partial threshold to cancel that crossing safely.
  Cancelled entry selections do not refill the current batch's admission slots.
- The existing interlock closes the entrance, cycles with both Doors closed, then
  opens the committed opposite exit. Walk through that exit explicitly; there
  is no automatic alignment or ejection. The entrance cannot bypass the cycle.
  Autonomous Agents can share the batch and leave independently.
- Pause freezes an admitted crossing or occupant. A stopped Player retains a
  usable committed exit, including after landing permissions are tightened;
  safe Standing clearance and Floor support still apply. Reset discards runtime
  occupancy and restores the authored position.

## Manual Security Scanners and Decontamination Chambers

- Hold **Left/Right** toward the authored entrance. Its sensor opens the Door;
  no Up press, Button, Arms capability or automatic approach is required.
  Walk through once the live Bulkhead leaf clears the Standing body, even before
  full opening. Wrong-side entry is refused.
- Entry claims shared capacity and commits the authored opposite exit. Older
  eligible queued entrants retain priority. Door Mobility, destination permissions,
  Standing fit through both thresholds and supported exit Floor remain mandatory.
  Security Scanners admit one occupant; Decontamination capacity equals cell width
  and retains the existing boarding deadline and autonomous batching rules.
- Left/Right walks inside, including during processing. Release stops and opposite
  input reverses. Movement remains bounded by the chamber and its controlled
  thresholds, with no slot alignment or automatic ejection. Stopped partial Door
  crossings retain their permit and provisional capacity; retracing cancels them.
- A Security Scanner waits until the Player's Standing body covers its central
  scan position. Walk there explicitly; there is no snapping. Decontamination
  batches do not require manual occupants to align to assigned capacity slots.
  Existing pre-delay, scanning/decontamination and post-pause timers run with both
  Doors closed, then the opposite Door opens. Exit explicitly with Left/Right.
- Committed exits survive tightened permissions. Pause and deactivation preserve
  admitted manual doorway safety and occupied capacity. Reset clears runtime
  ownership and restores the authored Location. Autonomous passengers retain
  their existing alignment, processing and exit behavior, including shared batches.

## Manual transport riding

- Up crosses an aligned, fully open enclosed Lift or Shuttle Door in either
  direction, without alignment or approach automation. Left/Right crosses a
  stopped Platform lift's supported open boundary. Capacity, crossing reservations,
  Mobility, Standing fit and landing permissions still apply.
- Left/Right walks within the occupied vehicle, including during travel. Movement
  remains bounded by the car or occupied Shuttle carriage. Leaving requires a
  stationary, safe open landing and explicit Player input.
- After entry completes, the Stop selector appears during the open boarding
  window before cutoff, and disappears during crossings, closure, movement,
  pause and Broken operation. For Player Lift/Shuttle visits the window starts
  once the landing Doors are fully open and remains available until the configured
  boarding cutoff; opening animation does not consume it. Reopened windows retain
  their original cutoff. Shift+Up/Down browses Lift and Platform lift Stops;
  Shift+Left/Right browses Shuttle Stops. Each fresh press advances once, clamps
  at the ends and skips unavailable destinations. Shift+cursor never also walks
  or climbs, even when no selector is available. Space confirms.
- Selection requests a service visit, not a passenger journey. Requests are
  idempotent, may coexist, survive leaving, and clear when served. Player passengers
  never automatically move to a slot or disembark. With no selection, the normal
  vehicle cycle continues; it does not wait for Player input.
- Fixed-size short-name labels are windowed around focus. Overflow dots and a
  separate current label retain context on long lists. White is normal, green is
  current (taking precedence over pending), amber is pending for any passenger,
  and a cyan outline marks focus. Reopening restores the last eligible browsed
  Stop, otherwise the nearest eligible non-current Stop.

## Play debug mode

F11 toggles a runtime-only debug mode, initially off. While enabled, pressing a
physical Lift Button selects that enclosed or Platform lift for a live MPP
built-in-font readout next to the vehicle. It displays fractional Level with
float round-trip precision, service phase, current/target Stops, dwell and boarding
timers, simulation tick/start/cutoff values, occupancy/reservations and Door/admission
state. The readout keeps updating during travel. Another Lift Button changes the
selected Lift; disabling debug clears it. Key repeat does not repeatedly toggle
F11. Observing debug data neither consumes simulation events nor operates devices.

## Editing the World used by Launcher

Launcher reads the original World at
`resources/test-worlds/room-height-test-1.world.yaml` through the repository's
`resources/Resources.yaml`. Save your changes there and restart Launcher; no
asset staging or C++ rebuild is needed. Build-directory resource copies are not
used. Generated `Game.yaml` references the repository resource directory by
absolute path, so the source checkout must remain at its configured location.

Assignment is authored and survives YAML/binary save/load, Reset, history and
cross-World clipboard placement. Key state and movement direction are runtime
input, never saved. Registry replacement, reload or detachment do not change the
built-in assignment.

The host reserves behaviour ID `18446744073709551615` with revision `1` and empty
configuration. Registry definitions cannot use that ID; clipboard data uses an
empty registry UUID for this built-in. Agent types and Installed behaviour
coroutines retain their existing contracts: this host behaviour schedules no Lua
work and does not add a new Agent type.

## Release validation

`agent-playerControlBasics`, `agent-playerControlBoundaries`,
`agent-playerControlBulkheads`, `agent-playerControlPersistence`,
`agent-playerControlDoorways`, `agent-playerControlDoorwayRestrictions`,
`agent-playerControlStairwells`, `agent-playerControlStairwellRestrictions`, and
`agent-playerControlStairwellPlatforms`, `agent-playerControlStaircases`, and
`agent-playerControlStaircaseRestrictions`, `agent-playerControlEscalators`,
`agent-playerControlLadders`, `agent-playerControlLadderRestrictions`, and
`agent-playerControlLadderSharedCapacity`, `agent-playerControlButtons`,
`agent-playerControlButtonPriority`, `agent-playerControlExtensibleLadders`, and
`agent-playerControlManualTransports` exercise
walking, timed bidirectional Layer transitions, mirrored zig-zag stair movement,
ascent/descent speed, narrow capacity, pause/reset, geometry, capability,
permissions, low clearance and Broken Doors through the World/coordinator seam. `agent-playerControlEditorWorkflow` covers the picker,
undo/redo and clipboard. `pf_application` covers actual Play key injection, Up repeat suppression and
suspension, held Up/Down platform departure and Staircase/Escalator mounting,
wrong-way walking, riding on release, fixed Ladder entry/climbing/reversal,
vertical-key cancellation, automatic dismounting, arm-length Button interaction,
repeat suppression, extension-then-climbing, manual Lift entry and Shift/Space
selector controls; `pf_application_render` covers selector windowing/colours, camera following, Layer selection,
resize and the no-Player fallback.
