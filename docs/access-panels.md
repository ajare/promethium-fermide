# Access panels (#451, #452)

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

Panel movement/clipboard and comprehensive surrounding-edit reconciliation
(including reverse wall-object conflict checks and control reflow) remain follow-up
work under #450. Runtime Open is never serialized as an authored initial-state option.
Owned interactions are reconstructed from placement records, not serialized handles.

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

All use existing production seams; Editor and Render use CPU-only ImGui with no
display or dialogs. See `linux-smoke-validation.md` and `validation-recovery.md`
for the required bounded final validation lanes.

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
