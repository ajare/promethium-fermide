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
- Movement stays on the current Layer and Level. Usable open boundaries and
  fully open standalone Bulkhead Doors permit entry into adjacent Locations,
  subject to Standing fit and existing passage restrictions.
- Solid walls, closed/opening Bulkhead Doors, low spaces and unsupported Floor
  edges stop the Agent's leading body edge. Input never operates a device,
  climbs, changes Layer, or automatically crouches/crawls.
- An Agent initially in a space that cannot accommodate Standing remains
  stationary in its existing fitting pose; input does not force it through the
  ceiling.

## Editing the World used by Launcher

Launcher loads the staged World beside its executable, not the repository's
`resources/` source file. If you edit and save the source World **after** building,
refresh the assets before restarting Launcher:

```sh
cmake --build build-linux --config Release --target pf-sector-assets
```

No C++ rebuild is needed. Alternatively, edit the staged World under
`build-linux/bin/x64/Release/test-worlds/` directly, but the next build will
replace that copy with the source resources. Prefer saving source changes and
then staging them.

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
`agent-playerControlBulkheads`, and `agent-playerControlPersistence` exercise the
World/coordinator seam. `agent-playerControlEditorWorkflow` covers the picker,
undo/redo and clipboard. `pf_application` covers actual Play key injection and
suspension; `pf_application_render` covers camera following, Layer selection,
resize and the no-Player fallback.
