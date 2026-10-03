# BoothWindows (#334, #335, #336)

BoothWindow is a fixed one-cell-wide, one-Level-high service aperture authored on
Layer L, with its back side on L+1. Both sides require an occupiable Room,
Corridor, or Facade and stable walkable Floor on the aperture's Level (Ground or
Walkway, not an extensible Force Bridge). Empty space, Backgrounds, Transits,
the back-most Layer, unsupported states, and malformed footprints are refused
before mutation.

The palette has a distinct BoothWindow slot. Selection offers an authored
Initially Open checkbox, with Closed as the default, and a separate runtime
Toggle shutter button; there are no resizing,
glass-style, Broken-condition, crossing, or physical control-panel controls.
Move, delete, copy/cut/paste, history, structural replay, and reset use normal
World document workflows. Edits that remove either Location or a supporting
Walkway remove the invalidated aperture and both approaches.

The two approaches are centred at x+0.5 on the walking Level, one per side,
connected only to that side's walking topology. Neither shutter state creates
a cross-Layer edge or a Traversal resource. Window base APIs reject glass states,
glass styles, and traversal configuration for BoothWindow.

## Runtime shutter operation (#335)

`BoothWindow::getDeviceId()` supplies a runtime-only typed `BoothWindowId`.
`World::submitDeviceCommand()` accepts `ToggleBoothWindow` and
`SetBoothWindowState` commands targeting that ID, without touching document
history or dirty state. The same commands can be bound to an Interaction point;
no Traversal resource is involved. Editor activation creates an activated Pending
Device operation; its next active resource-advancement phase resolves the target
and starts travel. Interaction-bound commands resolve only after physical activation,
not when their request is queued.

`World::lookupBoothWindow()` exposes Closed, Opening, Open, and Closing, physical
`getProgress()` (0 Closed to 1 Open), and `getTargetOpen()`. Toggles are never
coalesced: each activation flips the then-current target exactly once. Desired-state
commands are idempotent and may coalesce. A reversing operation cancels superseded
travel operations without jumping the device's physical position. Operation
outcomes are queryable through normal Device-operation lookups/snapshots/events;
terminal records retire at the next unreferenced tick boundary (ADR 0013).

Full travel takes 0.8 seconds at constant speed (48 ticks at 60 Hz); partial travel
is proportional. World pause and simulation time scale govern progress. Endpoints
hold indefinitely, with no sensors or automatic closing. Runtime progress, targets,
IDs, and operations are never serialized. Reset and structural replay restore the
authored initial state and retire old device references even if callers retain
object pointers; device/operation IDs are not reused within the World.

## Back-side Agent panel (#336)

`BoothWindow::getPanel()` exposes exactly one owned invisible Interaction point at
its centred back-side walking approach (x+0.5, Level), with 0.25 world-unit inclusive
reach and a one-tick press. Use `World::requestInteraction(panel, agent)` and normal
Interaction-request / Device-operation lookups, snapshots, and outcome events. The
binding is a required typed `ToggleBoothWindow`, not a traversal command or permit.
An accepted press resolves the current target only at activation; competing presses
use the normal FIFO point queue, never coalesce, and reverse moving shutters smoothly.

The Agent must be active, in the back-side Location, within reach, and permitted to
use Buttons under the normal effective Mobility profile and global interaction-state
constraints. Exact vertex arrival and crossing intent are unnecessary. Front-side,
wrong-Location, inactive, Buttons-forbidden, and out-of-reach requests return the
normal null rejected handle without allocating operations, moving the Agent, or
changing its route/target. Eligibility is checked again before pressing; departed or
newly ineligible queued Agents receive Cancelled outcomes, not auto-approach or a
late press. Other Interaction points, including Lift selectors, retain auto-approach.

The panel has no Sector object, physical Button, or renderer primitive and cannot be
independently placed/deleted. Access permission requirement authoring is deferred;
the owned panel is unrestricted by default and ineligible for direct requirements.
Construction, movement, save/load, clipboard, history, replay, and reset reconstruct
exactly one panel on the correct back side. Deletion and invalidating structural
edits clear outstanding requests and operations; request/operation handles are not
reused within the World. Only authored BoothWindow configuration persists, never
pending presses or runtime shutter state.

## Persistence

World schema **43** adds a distinct `boothWindow` construction record, appended to
the construction-kind enumeration to preserve legacy numeric kind values. YAML
and binary share this authored schema and atomic document validation:

```yaml
- type: boothWindow
  layer: 0
  y: 0
  x: 3
  cellsWide: 1
  levelsHigh: 1
  initialState: closed
```

`initialState` is `closed` or `open`. Traversability, glass style, and Broken
fields are unsupported, even when their values appear neutral. Older schema
versions cannot carry BoothWindow records. Ordinary Window and Door records
remain unchanged, and documents without BoothWindows remain compatible.

## Presentation and checks

Geometry matches an ordinary one-cell Window: 0.1 horizontal insets, 0.2 vertical
offset, and 0.3 visible height. The ObjectAtlas ImageSet supplies
`booth-window-open`, `booth-window-closed`, and the distinct `booth-window-shutter`
subregion. The Open frame stays stationary while the shutter moves upwards (opening)
or downwards (closing), clipped to the frame's inner aperture. The uncovered lower
portion reveals the back Layer through normal recursive intersecting clips; nested
Windows and BoothWindows retain their draw order. Closed omits the aperture pass.
No Button tile or physical panel is drawn.

Visual review of the supplied `objects.png`: the Open tile has a transparent centre
and narrow dark/silver frame; the Closed tile has an opaque grey, horizontally
slatted shutter. Its 42x38 inner region is suitable for vertical translation without
moving the frame. Animation uses those existing pixels, not replacement artwork.

The procedural tiles occupy the previously unused object-atlas cell [4,1].
`python3 scripts/pack_booth_window_tiles.py` (Pillow) regenerates their pixels
without changing other atlas regions; ImageSet and tileset definitions retain
their distinct rectangles.

Focused headless module checks:

- World: `boothWindows/placementAndTopology`, `boothWindows/atomicRefusal`,
  `boothWindows/lifecycle`, `boothWindows/simultaneousAdjacentPairs`.
- Simulation: `boothWindows/runtimeTimingAndReversal`, `boothWindows/runtimeLifecycle`,
  `boothWindows/typedInteractionActivation`, `boothWindows/backSideAgentPanel`,
  `boothWindows/ownedPanelLifecycle`.
- Persistence: `boothWindows/authoredRoundTripAndMalformedRecords` (also pending
  commands and mid-motion saves in both directions and both document formats).
- Render: `boothWindows/staticPresentationAndNestedClipping` (also runtime partial,
  reversed, and fully open states, with nested Window and BoothWindow clipping).
- Editor: `boothWindows/historyAndClipboard` (also the actual runtime button and
  unchanged authored snapshot/history during motion).
- Existing `sector-tileset`: verifies transparent Open and opaque Closed centres.

Use the [Linux validation procedure](linux-smoke-validation.md) for complete
Release/Debug validation. All feature checks are CPU-only and non-interactive.
