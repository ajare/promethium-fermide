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
- Release the key to stop. Holding both keys stops movement.
- Pause, reset, suspension, and leaving Play clear transient movement input.
- The camera centres on the controlled Agent at normal zoom and shows its Layer.
- Left/Right movement stays on the current Layer and Level. Usable open boundaries
  and fully open standalone Bulkhead Doors permit entry into adjacent Locations,
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
- Overlapping candidates prefer an admissible open doorway, then nearest centre,
  then stable traversal-resource identity. One press performs at most one action.
- Solid walls, closed/opening Bulkhead Doors, low spaces and unsupported Floor
  edges stop the Agent's leading body edge. Input never operates Buttons, boards
  transports, climbs, or automatically crouches/crawls. Only ordinary Doors between
  Locations support Up; transport-owned Doors, Windows and BoothWindows do not.
- An Agent initially in a space that cannot accommodate Standing remains
  stationary in its existing fitting pose; input does not force it through the
  ceiling.

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
`agent-playerControlDoorways`, and `agent-playerControlDoorwayRestrictions` exercise
walking, timed bidirectional Layer transitions, pause/reset, geometry, capability,
permissions, low clearance and Broken Doors through the World/coordinator seam. `agent-playerControlEditorWorkflow` covers the picker,
undo/redo and clipboard. `pf_application` covers actual Play key injection, Up repeat suppression and
suspension; `pf_application_render` covers camera following, Layer selection,
resize and the no-Player fallback.
