# Access panel authored foundation (#451)

Access panels are cell-owned objects on the visible back surface of Rooms,
Corridors, and Facades. They do not occupy the aperture slot, add walls,
create traversal resources, or obstruct movement. The only type is Empty;
creation and reconstruction are Closed. Runtime operation is not delivered here.

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
**Empty / Closed**, cell X, Level offset, and editable **Width**, **Height**, and
**Y offset** fields. Delete through Selection or the existing Delete shortcut.
These authored actions use document snapshot history for undo/redo. Refused
requests do not add history or mark the World modified.

Closed panels render over the Location back surface, including Facades. A
zero-width or zero-height panel has a small editor-only outline indicator at
its authored centre; hit-testing uses that same indicator without modifying
its authored, persisted, or collision bounds. Wireframe passes use outlines
only.

## Persistence and scope

World schema **51** appends Access panel placement, configuration, and deletion
construction kinds. Records store owner Sector, relative cell X and Level
offset, Empty type, and geometry. YAML and binary preserve chronology (including
edits and delete/replacement), rebuild approaches, and reconstruct Closed.
Schemas 1–50 without panel records remain supported. Malformed input is rejected
in detached reconstruction before the live World is replaced.

Agent operation, panel movement/clipboard, and comprehensive surrounding-edit
reconciliation (including reverse wall-object conflict checks and control reflow)
remain follow-up work under #450. This slice does not introduce interactions or
an alternative framework.

## Headless coverage

- World: `accessPanels/authoredWorld`, `accessPanels/wallOverlap`
- Editor: `accessPanels/selectionAndHistory`
- Persistence: `accessPanels/roundTripAndMalformedRecords`
- Render command stream: `accessPanels/closedPresentationAndIndicators`

All use existing production seams; Editor and Render use CPU-only ImGui with no
display or dialogs. See `linux-smoke-validation.md` and `validation-recovery.md`
for the required bounded final validation lanes.

## Verification

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
