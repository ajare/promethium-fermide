# Remote Button operation (#539)

Agent-type API v2 accepts `object_usage = "remote_control"`:

```lua
-- Inside the instance table returned by new(), alongside the other baselines:
object_usage = "remote_control",
object_usage_distance = 1.5,
```

Omitting `object_usage_distance` in a remote script declaration freezes 1 World
unit. Explicit usable distances must be finite, strictly positive simulation
floats; legacy `reach` is not a remote declaration. Unknown modes, invalid usable
distances and dual distance declarations are refused before publishing an Agent.
None continues to ignore its distance; Arms keeps its physical geometry rules.

Object usage and Object usage distance resolve independently as individual → tag
→ frozen script default. The individual Selection and Tags editors expose Arms,
None and Remote control. A Human changed to Remote control by mode alone keeps
its independently resolved 0.25-unit distance. Remote control has no Arms fallback.
YAML/binary Worlds, tag registries, clipboard and history persist authored values
(`remote_control`), not effective/default snapshots. Reset, load, paste and deleted
Agent restoration construct fresh script lifetimes; surviving structural/history
replay retains frozen defaults. Unsupported modes/documents reject atomically.

## Eligibility and runtime

Every **physical Button** qualifies, including light switches, stacked controls,
extensible-device controls, Airlock controls, and transport landing Buttons. The
Button identity and physical centre are authoritative, not its Interaction point
approach or the receiving device's centre/type. Generic Interaction points,
Access panels, BoothWindow shutter panels, direct manual Door operation and
onboard selectors do not acquire remote capability in this slice. Later tickets
#540–#543 extend other interactions; remote-only passengers cannot manufacture an
onboard Lift/Platform lift/Shuttle selection here, but may use accepted shared
journeys under the existing rules.

Range is inclusive Euclidean distance from Agent position to Button centre,
including vertical separation. Both must occupy the same Sector; another Level
in that Sector is allowed. Local depth and line of sight are not range dimensions.
Direct out-of-range requests refuse rather than walking. Direct requests while
pathing use the same eligibility as stationary requests.

Only controls preparing the next action-bearing resource on the selected Path
are activated automatically. A walking Agent stops at range entry for the normal
Button queue and press duration, then continues while the device operates.
Movement is clipped at range entry so a narrow interval cannot be skipped in one
tick. If approaching is necessary, traversal preparation walks only to the range
boundary on the current row; an unreachable vertical range excludes the route.
Queue standing targets do not pull an operator away during this approach. Ordinary
walking, Local-depth transitions, threshold alignment, boarding and admission
remain physical. Unrelated nearby Buttons are never implicit Path intent.

Access permissions, Permission adherence, Buttons Mobility (including last-resort
routing), Broken devices, press/device durations, FIFO queues, capacity,
reservations, interlocks and traversal permits remain their existing authorities.
Pending requests revalidate current eligibility; paused mode/range/tag edits
cancel now-ineligible work without granting new traversal admission. An Airlock's
control-only walking estimate uses the same range geometry, leaving preferences
and the remaining objective traversal/interaction costs unchanged.

## Headless verification

Release project-owned CTest checks `object-usage-agentTypesRemote*` exercise
script defaults/invalid declarations, independent overrides, exact/inside/outside
range, cross-Level/different-Sector geometry, physical centre versus approach,
direct requests during movement, pending tag edits, transport landing calls,
complete Door/Bulkhead/Airlock/Force Bridge journeys, tight-range/fallback Door
journeys, published outcomes and coordination cleanup. Existing Agent lifetime
and editor Object usage workflows additionally run with Remote control, covering
registry and YAML/binary round trips, Reset, structural replay, clipboard,
history, deletion restoration and atomic invalid authoring/reload.
