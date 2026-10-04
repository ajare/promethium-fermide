# Route planning

Route planning is active, stationary simulation time, not a background job. Runtime
initial movement and automatic replanning retain destination intent, clear the
public active Path, release uncommitted coordination ownership, and stop at the
Agent's exact position. Committed traversals finish before planning starts. Editor
Path authoring and manual recalculation remain immediate.

The editor's **Select path destination (Ctrl+P)** picker shows and hit-tests only
World-owned Marker vertices, including Furniture usable points. Private Furniture
routing vertices, external ports, inferred floor anchors, and device/transit
vertices are not destination choices. Ordinary World graph inspection remains
unchanged. Ctrl-click path authoring and the **Path to selected Marker** action
likewise require an actual Marker destination.

Each episode uniformly samples an inclusive whole-tick interval from Minimum and
Maximum route planning time (defaults 1 and 3 seconds; each accepts 0.1–10 seconds).
Both effective endpoints round upward to ticks. A lower maximum is raised to the
minimum for consumption only. Individual properties override inherited samples;
authored endpoints and sample provenance are not rewritten. Property edits affect
the next episode, not an existing timer.

Pause and deactivation freeze the remaining ticks. At expiry, route calculation
remains **synchronous**; movement starts on the following tick. Asynchronous,
threaded, queued, or shared background route-planning services are out of scope.
The dedicated World-seed/Agent-ID episode stream is independent of behaviour,
property-sampling and Escalator randomness. Reset restarts it.

Repeated same-destination commands and environmental triggers do not restart an
episode. Replacing a destination starts a new episode. Mandatory failures report
Route loss at expiry, not at entry. Voluntary planning privately retains the old
Path and its diagnostics; current Route observations and Route persistence decide
whether to replace it. Invalidating that candidate upgrades to mandatory planning
without resampling. Queue priority is forfeited even if the old Path is resumed.
Successful same-destination automatic planning produces no behaviour callback.

## Presentation and observation

A Route-planning Agent remains active for Agent group membership/activation and
general simulation metrics, despite its stationary position. Runtime snapshots
expose destination intent and total/remaining ticks. The Selection panel displays
Route planning, destination, and simulation seconds; it does not expose the private
candidate as an active Path.

Under Agent Debug, a neutral grey box with a white `?` represents the current
Route planning state. It persists while paused or deactivated in that state and
disappears on state exit. Queue badges keep their body-relative stacking order;
entry to planning releases queue ownership, removing the old `!` and any obsolete
stack gap. There are no completed-replan sequences, timestamps, green/red result
phases, first-frame tricks, or wall-clock render-cache lifetimes.

## Compatibility contract

- World schema **29** persists the two individual timing properties and inherited
  scalar samples/provenance. Agent tag registry schema **13** persists their ranges,
  revisions and allocator state. Older documents without these fields receive the
  defaults and are not made dirty merely by loading.
- Clipboard payloads copy authored individual properties and inherited sample
  provenance. Older payloads without them retain the defaults. Existing registry
  identity and dependency validation still applies.
- Planning intent, total/remaining timer state, private candidates and episode
  random position are runtime-only: none enters World documents or clipboard
  payloads. Save/load and Reset reconstruct from authored data, not the current
  episode. Runtime topology rebuilds, unlike document reloads, preserve an episode.
- `prometheum.v1` remains supported: planning maps to `idle`, a different destination
  is refused as `agent_busy`, and superseded cancellation events are hidden.
  `prometheum.v2` exposes `route_planning`, accepts destination replacement with
  `superseded`, and reports the replaced intent's `movement_cancelled` event with
  reason `superseded`. Neither API exposes exact planning timers or random state.
  Both versions may coexist in a registry and reload independently of live Lua
  state; bundled workflows use v2. See [Agent behaviour packages](agent-behaviour-packages.md).

## Verification

See [headless integration checks](headless-build.md#route-planning-integration-258)
for repeatable commands and coverage. Existing old-format fixtures remain enabled;
new-format World, registry and clipboard round trips and mixed v1/v2 reload checks
run alongside the full coordination suite. Renderer command-stream and ImGui
panel tests exercise editor presentation without opening a window or dialog.
