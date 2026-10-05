# Access panels (#451, #452, #453, #454)

Access panels are cell-owned objects on the visible back surface of Rooms,
Corridors, and Facades. They do not occupy the aperture slot, add walls,
create traversal resources, or obstruct movement. The only type is Empty;
creation and reconstruction are Closed. Eligible Agents can Open and Close panels
through owned typed Interaction points and queryable Device operations.

## Shared authoring rules

`World::canAddAccessPanel`, `addAccessPanel`, and `configureAccessPanel` use the
same preflight, also used by construction replay and YAML/binary loading:

- The owner must be a Location. Cell X is a World X coordinate; integer Level
  offset is relative to the Location base Level. One panel per cell, including
  zero-area panels.
- Finite width, height, and bottom Y offset are in [0,1], with Y offset + height
  at most 1. Defaults are 0.5, 0.25, 0.25. Width stays cell-centred.
- Support must be Ground Floor or Walkway, not air or Force Bridge.
- Positive-area authored rectangles cannot overlap fixed wall objects in that
  Location (Buttons, Doors, Bulkhead Doors, Windows, BoothWindows, panels).
  Edge contact and zero-area rectangles are allowed. Furniture and Agents do
  not participate in this collision rule.

The cell holds a separate object reference. The existing floor-object graph
path builds its approach at X + 0.5 and the owning cell's floor Level. Geometry
editing does not replace that vertex. Inside a Furniture side-route footprint,
the approach is a destination branch on one existing route, retaining its Local
depth; it does not recreate ordinary Floor or connect front/back routes through
the panel. Deletion clears cell ownership, tombstones
the Sector object slot, and rebuilds the graph.

## Editor

Drag **Access panel** from the attached-object palette row onto a supported
cell. Object selection uses the World canvas hit-test. Selection displays
**Empty / Open** or **Empty / Closed**, cell X, Level offset, and editable **Width**, **Height**, and
**Y offset** fields. Delete through Selection or the existing Delete shortcut.
These authored actions use document snapshot history for undo/redo. Refused
requests do not add history or mark the World modified.

Closed panels render over the Location back surface, including Facades. A
zero-width or zero-height panel has a small editor-only outline indicator at
its authored centre; hit-testing uses that same indicator without modifying
its authored, persisted, or collision bounds. Wireframe passes use outlines
only.

## Agent operation (#452)

Agent Selection lists the panels in the Agent's Location. Closed panels offer
**Open**; Open delegates exposed actions to the type, and Empty offers only
**Close**, with no internal controls. Disabled actions reflect current activation,
effective Buttons Mobility, Location, Level, and stationary interaction eligibility.
The public seams are `canRequestAccessPanel` and `requestAccessPanel`; the owned
Interaction points also enforce these rules through `requestInteraction`.

Ordinary Button approach logic moves the Agent toward the graph-connected cell
centre at floor Level. Reach is 0.25 units from that vertex, independent of panel
geometry, including zero dimensions. Eligibility is checked again before activation;
changes while pending cancel the request. Buttons' Only if no other option remains
operable, and individual properties override inherited tag properties.

After the ordinary interaction scheduling interval (one tick), Open/Close is
instantaneous at activation, even before exact vertex arrival. Any eligible Agent
may Close. Panels stay Open until explicitly closed; reset reconstructs Closed.
There is no permission requirement, auto-close, animation, admission resource,
walking obstruction, bounds expansion, or topology change. Open renders a dark
interior and an inset cross, including on Facades and wireframe/zero-area indicators.
Runtime actions do not dirty the World or add history entries. Removing a panel
cancels its pending requests and removes controls/device references; panel/control
IDs are not reused on replacement, reset, or editor history reconstruction.

## Persistence and scope

World schema **51** appends Access panel placement, configuration, and deletion
construction kinds. Records store owner Sector, relative cell X and Level
offset, Empty type, and geometry. YAML and binary preserve chronology (including
edits and delete/replacement), rebuild approaches, and reconstruct Closed.
Schemas 1–50 without panel records remain supported. Malformed input is rejected
in detached reconstruction before the live World is replaced.

Surrounding structural edits also enforce these invariants (#454). Location
movement carries cell-relative panels and rebuilds their bounds, approaches, and
owned interactions. Owning-Location deletion removes the panels and all old
requests, controls, and device references; replacement identities are fresh.
Location cropping and Floor/Walkway support removal refuse retained panels rather
than deleting, clamping, or relocating them. Layer/Level deletion follows the
existing destructive Sector-deletion contract; retained Locations and their panels
compact together and reconstruct Closed.

Door/Window/BoothWindow placement, movement and resizing validate against panels
on both sides. Door height changes and Bulkhead Door placement also preflight
panel bounds. Canonical Button planning checks the complete chosen layout,
including stack heights, against panel rectangles before any reflow. Panels do
not influence the allocator's policy: a conflicting final assignment refuses the
triggering transaction. Edge contact and zero-area rules remain unchanged;
Furniture and Agents remain excluded. Detached replay validates retained panels
again after final control reflow, before live state or history is replaced.

Removed historical panels impose no support constraints on surrounding replay.
Their placement slots become ordinary object tombstones; an optional
`trimTrailing: true` tombstone replays the original removal's trailing-empty-slot
trim without deleting live objects. This backward-compatible optional field in
schema 51 preserves subsequent object indices in YAML, binary, reset and history.
Runtime Open is never serialized as an authored initial-state option.
Owned interactions are reconstructed from placement records, not serialized handles.

## Movement and clipboard (#453)

Drag a selected panel with the existing object-movement workflow. The preview
uses the panel's selection geometry (including zero-area indicators), and
`planMoveSectorObject` / `applyObjectMove` share destination validation. Panels
may move between supported cells in Rooms, Corridors, and Facades on the same
Layer, preserving Empty type, width, height, and Y offset. Source attachments
are removed and the destination floor-Level approach is rebuilt.

Copy/paste uses an authored-only `AccessPanel` payload: `panelType: Empty`,
`width`, `height`, and `yOffset`. Invalid/missing/non-finite fields and extra
runtime/ownership fields are rejected before any edit. Pasted panels have
independent identities, approaches, and owned interactions, and start Closed.

Movement uses existing detached construction replay preflight, then ownership
reconstruction, rather than copying live vertices, controls or pending requests.
It appends removal/placement records to retain geometry-edit chronology and
object tombstones, without a new persistence schema. These records rebuild
optional device ownership through ordinary panel creation. As with other object
moves, selection is replaced with the returned destination object and stale
selection references cannot edit/delete the reconstructed object. Move and paste
use document snapshot undo/redo; rejected operations add no history or modified
state. YAML/binary loading and reset reconstruct Closed destination panels.

## Headless coverage

- World: `accessPanels/authoredWorld`, `accessPanels/wallOverlap`
- Simulation: `accessPanels/approachAndInstantReach`, `accessPanels/eligibilityChanges`,
  `accessPanels/staleRequestCleanup`
- Editor: `accessPanels/selectionAndHistory`
- Persistence: `accessPanels/roundTripAndMalformedRecords`
- Render command stream: `accessPanels/closedPresentationAndIndicators` (Closed/Open)

Editor coverage includes the actual Agent action buttons, runtime history isolation,
and operable reconstructed controls. Persistence checks YAML/binary saving while Open,
Closed reconstruction, and subsequent Agent operation.

The same checks also cover movement across supported Location types, Floor and
Walkway ownership, occupied/unsupported/conflicting cells, Furniture/Agent
exclusion, degenerate hit-testing and destination rendering, malformed clipboard
payloads, fresh identities/controls, pending-request cleanup, undo/redo selection
safety, and movement/copy persistence/replay.

All use existing production seams; Editor and Render use CPU-only ImGui with no
display or dialogs. See `linux-smoke-validation.md` and `validation-recovery.md`
for the required bounded final validation lanes.

## #454 verification

Final Linux GUI-enabled default builds (including `editor`, core and headless
modules/tools) and unfiltered final CTest passed on the final source state in
Release and Debug: 110 registered tests, zero failures, one explicit optional GUI
capability skip per configuration. Displays were unset; Editor and Render used
CPU-only ImGui. The affected World/Editor/Render/Persistence/Simulation milestone,
focused panel checks, smoke ownership audit, and `git diff --check` also passed.

- Release final CTest: `d30c8089af6f4ce38293773dc9ba3882`, 132.11 s.
- Debug final CTest: `eff2571c6c8a4a87bc5ed7073df7a6d8`, 594.67 s.

Coverage includes Location carry/resize/deletion, Ground Floor and Walkway
support refusal, reverse wall-object placement/movement/resize and Door height,
final Button stack/support reflow, Layer/Level compaction/deletion, stale pending
requests/operations, document undo/redo, retired-panel chronology and stable object
indices, permitted Furniture/Agent overlap, renderer carry/removal, and YAML/binary
structural round trips and atomic conflicting/unsupported reconstruction refusal.

## #453 verification

Final GUI-enabled Linux default builds (core, headless modules/tools, and `editor`)
and unfiltered CTest passed on the final source state in Release and Debug:
110 registered tests, zero failures, one explicit optional GUI skip each. All
checks were headless; no display or dialog was used. `git diff --check` and the
smoke ownership audit passed.

- Release final CTest: `c5e7f9f597bb462eadb3d17ed2a8067f`, 129.76 s.
- Debug final CTest: `0101767323a3439b97629411488ac8bf`, 607.14 s.

## #452 verification

Final Linux GUI-enabled default builds (core, headless modules/tools, and `editor`)
and unfiltered CTest passed in Release and Debug: 110 registered tests, zero failures,
one explicit optional GUI skip per configuration. Displays were unset; Editor and
Render regressions used CPU-only ImGui. `git diff --check` and the smoke ownership
audit passed. Final CTest evidence:

- Release: `d01d95712e5d48ebab3d549a4efb4a8c`, 129.37 s.
- Debug: `950b4dff3cde46d6b5a4246e1f2e5a24`, 593.20 s.

## #451 verification

Linux GUI-enabled default builds (including core, headless modules, tools, and
`editor`) and unfiltered final CTest passed in Release and Debug: 110 registered
tests, zero failures, one explicit optional GUI skip in each. Displays were
unset. Release final run: `4beda4c4f9d44dd9a2ecc80d895c44d1` (130.56 s);
Debug final run: `d40110242ccb4271aeea453eb017138c` (598.96 s).
`git diff --check` and the exact smoke ownership audit also passed.

Schema expectation/future-version fixtures were advanced for version 51. The
validation pass additionally reconciled already-registered Agent pose and
Furniture checks with stale CLI inventories/ownership rows; their assertions
and production behavior were not changed.
