# Object usage: authoring, migration and mixed-object verification

This is the authoring/migration summary for the completed #535–#544 slices of
#534. [ADR 0019](adr/0019-script-backed-agent-types-with-live-instances-and-frozen-baselines.md)
records the decisions and lifetime policy; [Remote operation](remote-button-operation.md)
records detailed geometry and runtime rules. `CONTEXT.md` is vocabulary, not a
second specification.

## One Agent distance authority

Inside the instance returned by an API v2 Agent type's `new()`:

```lua
object_usage = "remote_control",
object_usage_distance = 3,
remote_access_panels = true,
remote_booth_window_shutters = true,
```

Keep the rest of the required physical, pose and Mobility declarations; see
[create an Agent script](create-agent-script.md) for a complete template.

| Declaration | Frozen Object usage distance |
| --- | --- |
| `object_usage = "arms"` or omitted mode | Required finite, positive distance (arm length) |
| `object_usage = "remote_control"` | Explicit finite, positive distance or 1 World unit when omitted |
| `object_usage = "none"` | Ignored; omission freezes canonical zero |
| Legacy `reach = 0.25`, omitted mode | Arms, 0.25 World units |

`reach` is only a legacy Arms **input alias**. Rename it to
`object_usage_distance`; never declare both, even with equal values. Remote
control rejects legacy `reach`. Interaction point `reach` is a different,
object-owned geometry limit and still permits zero; it is not an Agent default.
Arms obeys both effective arm length and physical point/side/height eligibility.

Each of the four values resolves independently: individual Agent property →
assigned Agent tag property → frozen script default. A mode-only override of Human
to Remote control keeps **0.25**, not 1. Author distance separately if necessary.
The two remote-category booleans default true. False means remote refusal without
Arms fallback; neither boolean changes Arms or None. No ordinary-Door, Button or
Lift boolean exists. Tag distance is concrete, never sampled. Conflicting tag
sources and invalid usable effective distances are rejected atomically.

Use the existing Agent Selection/Tags property editors. Add/remove/edit while
paused; removal reveals inheritance without deleting tag assignments. See
[Agent properties](agent-properties.md) for wire keys, provenance and history.

## Remote eligibility

All ranges are inclusive straight-line World-unit distances from Agent position,
including vertical separation. Local depth and line of sight are irrelevant.
Another Level inside the same applicable Sector is allowed; proximity never grants
operation across Sectors.

| Object/control | Authoritative centre and ownership |
| --- | --- |
| Ordinary manual Door required by the Path | Actual Door centre; either approach Sector, including low-height Doors |
| Every physical Button | Button centre, not approach vertex or controlled device centre; current Sector |
| Lift / Platform lift destination selector | Actual occupied vehicle centre and applicable Sector; never another car |
| Access panel command | Panel centre, current Sector, Remote Access panels enabled |
| Standalone BoothWindow shutter | Shutter centre, controlling back-side Sector, Remote BoothWindow shutters enabled |

Automatic Doors retain device-owned activation. Button-controlled Doors retain
Button operation/authorization. Direct Bulkhead and transport landing Door
operation, onboard Shuttle selectors and generic Interaction points are excluded.
Physical Shuttle Buttons and Buttons operating Bulkheads remain eligible. A
Dumbwaiter controls its own shutters; operate its landing Buttons instead.

Path-required activation starts at maximum range entry without walking solely to
a physical control. Normal press/device durations, Local-depth movement, crossing
alignment, boarding/disembarking, Access permissions, Permission adherence,
Mobility, Broken policy, queues, capacity and interlocks still apply. Direct
out-of-range requests refuse rather than walking. None can share already usable
passage and locally observed accepted journeys but cannot supply missing operation.
Range grants neither authorization nor remote knowledge of schedules/queues.

## Runnable laboratory

Open **RemoteActivationDemo** or
`resources/test-worlds/remote-activation.world.yaml`, then Play. Its manifest
includes the custom Agent type, tag registry and one-shot coroutine behaviour.
Reset repeats the demonstration; no dialog or interactive setup is required.

- **Script remote** inherits Remote control / 3 from `remote-operator.agent.lua`.
- **Tag remote** is Human inheriting both values from `#remote` in
  `remote-activation.tags.yaml`.
- **Individual remote** is Human with two explicit overrides.
- **Arms operator** retains Human's Arms / 0.25 defaults.
- **None observer** uses an individual None override and the separate automatic
  Door; it performs no device requests.

The four operators cross the ordinary manual Door, use physical landing Buttons,
board the capacity-one enclosed Lift, exit into the gallery, then board the
capacity-one Platform lift to **Mixed journey goal**. These are one selected Path,
not separately teleported test stages. The nearby light switch, Empty access panel
and closed BoothWindow shutter are intentionally unrelated to that Path and must
stay untouched. From the ground back Room, panel/shutter controls can be requested
explicitly; the headless mixed-control check opens/closes and cancels these via
public APIs. Their presence does not imply automatic Path intent.

For experiments, pause and change one property's source, remove a mode override
without changing range, deny Buttons Mobility, or disable one remote-category
boolean. Changing capability does not create a traversal permit. The one-shot
behaviour ends on arrival or Route loss rather than waiting indefinitely for a
failed journey.

## Persistence and lifetime audit

No new persistence schema or live Lua serialization is needed for this slice.
World YAML/binary and tag registries retain authored mode/distance/booleans,
assignments, resource references and behaviour configuration. Omitted overrides
resolve to compatible type defaults, not implicit remote capability. Both legacy
reach-only scripts and omission compatibility remain covered.

Surviving structural edits and ordinary history replay retain frozen type defaults
and live Agent instances. Creation, load, Reset, paste and restoration after
Agent deletion construct and validate fresh lifetimes. Individual overrides never
materialize script defaults. Invalid sources/documents/edits fail without partially
mutating the current World or history. Same/cross-World clipboard uses existing
resource/type resolution; it never copies another World's live Lua instance.

## Public-seam evidence map

All checks use public World ticks, object state, Agent movement, snapshots and
published outcomes; editor/lifetime checks use existing history and clipboard
workflows. No new low-level test seam is introduced.

| Acceptance evidence | Project-owned checks |
| --- | --- |
| Authored mixed Door → Lift → Platform journey, early opening, physical crossing/boarding/exits, capacity-one contention, unrelated lights, YAML/binary + Reset; capability loss after accepted Platform selection | `remoteMixedWorld` |
| Equal layouts: remote approach savings agree with objective estimate, perceived score and measured journey ticks versus Arms | `remoteMixedCosts` |
| Buttons/panels/shutters together: physical centres, inclusive/outside range, same-Sector cross-Level, persisted Local depth 7 unchanged, other-Sector refusal, paused mode cancellation, retry success and request cleanup | `remoteMixedControls` |
| Script declarations, legacy alias/omission, independent resolution and Human mode-only 0.25 | `agentTypesArmsDeclarations`, `agentTypesRemoteDeclarations`, `agentTypesObjectUsageOverrides`, `agentTypesInheritedObjectUsage` |
| Ordinary Door sides, scaled centre, range entry, unrelated/excluded categories, authorization/Mobility/Broken refusal, route reconsideration and admitted crossing preservation | `agentTypesRemoteOrdinaryDoors`, `agentTypesRemoteDoorGates`, `agentTypesObjectUsageCommittedCrossing` |
| Physical Buttons, last-resort Mobility, pending tag/range cleanup; complete Button-controlled Door/Bulkhead/Airlock/Force Bridge journeys and interlocks | `agentTypesRemoteGeometry`, `agentTypesRemotePendingEdits`, `agentTypesRemoteJourneys`, `agentTypesRemoteTightDoor` |
| Own-car selectors, inclusive/outside vehicle centre, another vehicle in range, destination/landing authorization, Buttons Mobility, committed exits; Shuttle onboard exclusion versus landing Buttons | `agentTypesRemoteOnboardJourneys`, `agentTypesRemoteLandingButtons`, `agentTypesNoneSharedJourneys`, `agentTypesNoneDirectRefusals` |
| None opportunistic passage and shared interlocked journey | `agentTypesNoneDoors`, `agentTypesNoneOtherResources`, `agentTypesNoneSharedAirlock` |
| Independent panel/shutter flags, false without fallback, Arms unaffected, generic point refusal, published authorization outcomes and paused cancellation | `agentTypesRemoteAccessPanels`, `agentTypesRemotePanelProperties`, `agentTypesRemoteBoothWindowShutters`, `agentTypesRemoteShutterProperties` |
| Registry and both-format persistence, malformed data atomic refusal, surviving edits, fresh Reset/deleted restoration, same/cross-World clipboard, undo/redo, frozen defaults | `agentTypesInheritedObjectUsageWorkflows`, `agentTypesObjectUsageOverrideWorkflows`, `agentTypesNoneClipboardAndHistory`, `agentTypesNoneLifetimes`, `agentTypesResetRevisionAndAuthoredData`, `agentTypesTopologyReplayPreservesLiveInstances`, `agentTypesHistoryPreservesSurvivorsAndReconstructsDeletedAgents` |

Focused Release commands (reuse the configured tree):

```sh
cmake --build build-linux --config Release --target pf-smoke-agent -j4
env -u DISPLAY -u WAYLAND_DISPLAY ctest --test-dir build-linux -C Release \
  -R '^object-usage-remoteMixed' --output-on-failure
```

Final validation builds the default core/headless/editor inventory and runs all
project-owned CTests, excluding unmodified submodule tests, Release only per
`AGENTS.md`. `git diff --check` supplies whitespace validation. No Windows or
Debug execution is claimed.

Final #544 Linux evidence: incremental default Release build in the existing
GUI-enabled `build-linux` tree succeeded; display-unset
`ctest --test-dir build-linux -C Release -E '^willpower_' -j4 --output-on-failure`
passed **131/131** project-owned tests (165.17 seconds), including exhaustive
module/CLI contracts, editor/history/clipboard, both-format persistence and
non-interactive Startup. The affected Agent/Simulation/Transport milestone passed
41 checks; the three focused mixed-object checks passed. The initial full pass
exposed a missing expected CLI-list update; both repaired Agent contracts passed
before the successful final matrix. `git diff --check` passed. No unmodified
submodule tests were run.

The integration closed only two evidenced numerical gaps: range-entry clipping
could round outside and stall at a Lift Button, and an enclosed Lift passenger
could retain a sub-tolerance vertical residual below its arrival Floor. Strict
request range, buffered horizontal positions and existing durations/admission
are preserved. No fallback mode, generic remote capability, cross-Sector operation,
extra category booleans or live Lua serialization was added.
