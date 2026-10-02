# Location permission requirements (#273–#276)

Rooms and Corridors support a World-owned, all-of Location permission requirement.
New Locations, an empty requirement, and older Worlds without an authored requirement
are unrestricted. Facades, Backgrounds, and Transits do not expose this authoring
feature. See [ADR 0016](adr/0016-constrain-location-passage-with-independent-permission-requirements.md).

## Authoring and persistence

The Room and Corridor Selection panels show **Location permissions** and an explicit
**Required (all): None** when unrestricted. Expand **Required Access permissions**
to use the existing permission checklist or **Clear** the entire requirement.
Each successful checkbox change or Clear is one document edit, with undo/redo.
Inspection, dismissal without a change, repeated values, and refused changes add
no history. Controls are disabled while running; the summary remains inspectable.

Public World APIs use Sector indexes and existing `AccessPermissionId` identities:

- `isLocationPermissionEligible(index)` identifies Rooms and Corridors.
- `getLocationPermissionRequirement(index)` returns ordered permission identities;
  missing or unsupported Locations throw an actionable `invalid_argument`.
- `setLocationPermissionRequirement(index, permissions, diagnostic)` permits only
  paused authoring, validates every identity and rejects duplicates before mutation.
  The same requirement is a successful non-dirty no-op.
- `canAgentAccessLocation(sector, agent)` checks current effective direct and
  Permission set grants. All requirement members must be present.

World schema version 32 persists the requirement on the Room/Corridor construction
record, in YAML and binary documents and document-history snapshots. Missing fields
load empty. Dangling, duplicate, malformed, and unsupported-kind requirements are
rejected before replacing the loaded World.

## Static route choice

At route calculation, Location requirements are hard constraints, not costs or
preferences. An unauthorized protected Location cannot be a destination or an
intermediate Location: the check covers every owned vertex, including Markers,
Door approaches, and hosted-object vertices, as well as ordinary floor arcs and
both Mobility search passes. An unrestricted alternative is selected when available;
otherwise the simulation reports the existing `Unreachable` Route loss.

Boundary authorization belongs to the Location being entered. No permission is
required merely to leave the occupied source Location: source-owned waypoints may
lead to an exit, but a Path cannot re-enter the unauthorized source Location or
choose a destination inside it.

Permission adherence, an already-open Door, or authorization to operate a Button
cannot bypass a Location requirement. Conversely, Location authorization never
substitutes for control-operation authorization, Mobility, physical availability,
queues, capacity, reservations, or safety. Those existing checks still apply.

## Changing authorization (#274)

Losing a relevant effective grant hard-invalidates a Path that still needs to enter
the protected Location. Current direct grants and current Permission set assignments
are authoritative, including runtime overlays and changes to set membership. An
overlapping grant source preserves authorization until the final source is lost.
Adding or tightening an unsatisfied authored Location requirement has the same
mandatory Route planning behavior; satisfied and unrelated edits preserve valid Paths.

Gaining all required authorization for a Location starts voluntary Route planning.
The retained Path is replaced only when the improvement satisfies Route persistence.
Partial all-of gains, redundant sources and unrelated changes do not disturb a valid
Path. Repeated triggers do not restart an episode; invalidating a privately retained
Path upgrades that episode without resampling its interval. At expiry an authorized
alternative is selected, or established `Unreachable` Route loss is published once.

Immediately before entry starts, movement rechecks current effective grants against
the entered Location, even for a stale Path or a queue-granted request. This hard gate
applies equally to open-wall and Door boundaries, independent of Permission adherence,
Mobility, controls, physical availability, queues, capacity and safety. It does not
interrupt a crossing already underway or require permission merely to exit.
Runtime grant changes do not rewrite authored authorization; Reset restores authored
grants and reconstructed Paths using the existing simulation semantics.

## Unauthorized occupancy and placement (#276)

An unauthorized occupant may use source-owned vertices only to reach an authorized
outside destination. Internal destinations (including the current position) are
unreachable, and leaving never grants permission to re-enter. If no authorized Path
to the selected destination exists, ordinary `Unreachable` Route loss is reported;
no evacuation destination is invented. Compatible YAML and binary documents may
restore existing unauthorized occupants, who follow the same source-only rule.

Loss before physical entry begins clears the Path and starts stationary Route
planning outside. Loss during a committed crossing preserves the crossing and its
position, then starts mandatory Route planning at safe completion, before any
subsequent affected movement. Even an internal destination coincident with the
completed crossing must be resolved through planning and Route loss, not treated
as authorized arrival.

Ordinary creation, simulation spawning, authored clipboard/pegman placement and
direct relocation reject unauthorized placement before ownership, ID allocation,
position changes or document-history writes. Diagnostics name every missing Access
permission by current name and identity, using the effective union of direct and
Permission set grants. Public `createAgent` overloads accept initial direct grants
and Permission set assignments so authorized creation is atomic; granting access
after creation is not a way to enter a protected Location. Clipboard authorization
is validated both when arming a fall and at landing using current set membership;
foreign-World grants are stripped before the placement check. Drag relocation uses
the existing Agent's current runtime grants and validates before detaching it.

## Authored lifecycle (#275)

Location moves, supported resizes, and construction-record reconstruction retain
requirements with their Room or Corridor. Deleting an owner drops only its requirement;
new or recreated Locations are unrestricted. Permission rename retains stable references.
Permission deletion clears every live and persisted Location reference before releasing
the slot, so a replacement permission cannot inherit stale protection. Authoritative
usage counts scan current Location owners; the Permissions table and deletion confirmation
include those counts. Confirmed deletion is one undoable edit; cancellation is a no-op.

Schema 32 (introduced with #273) remains the authoritative requirement format. Older
supported schemas without requirements default to unrestricted; a requirement field in
a pre-32 document is rejected rather than silently discarding protection. Unsupported
owners are rejected even when the field is empty. Duplicate, dangling, out-of-range,
and malformed requirements fail before changing the destination World.

## Verification

`LocationLifecycle.cpp` adds `locationLifecycleRoom`, `locationLifecycleCorridor`,
and `locationLifecycleMalformed*` to `pf-smoke-permissions`, covering public move,
grow/shrink, deletion/reindexing, Reset reconstruction, rename/deletion, slot reuse,
complete YAML/binary round trips, older defaults and transactional malformed input.
`LocationLifecycleEditor.cpp` adds `permissions/locationLifecycle*` to `pf-smoke-editor`:
CPU-only input drives the real usage table and deletion modal (Cancel and Delete), with
complete document-history restoration for moves, resizes, rename, permission deletion,
slot reuse and Location deletion. Logging uses an in-memory callback, never the native
clipboard. Selection checks also verify Transit ineligibility.


Headless coverage is owned by `pf-smoke-permissions` (`locationAuthoring*`,
`locationRouting*`, `locationAlternative*`, `locationBoundary*`) and `pf-smoke-editor`
(`permissions/locationSelectionRoom`, `permissions/locationSelectionCorridor`).
The editor checks drive the production Selection checklist with CPU-only ImGui
mouse events and restore real document-history snapshots; they open no desktop
window, native dialog, or clipboard integration.

`LocationChanges.cpp` adds `locationLosses*`, `locationGains*`,
`locationRequirementChanges*`, `locationStaleEntry*` and `locationResetAndUnrelated*`
checks for both Rooms and Corridors. Run focused checks with
`pf-smoke-permissions --check locationStaleEntryRoom`, or the entire Permissions
module with `ctest --test-dir build-linux -R '^smoke-permissions' --output-on-failure`.
These checks assert visible Paths, stationary planning/expiry, actual entry and
movement, Reset, and exactly-once simulation events; no private invalidation helper
is tested directly.

`LocationOccupancy.cpp` adds `locationOccupancy*`, `locationCommittedEntry*` and
`locationPlacement*` checks for both Location kinds. `LocationPlacementEditor.cpp`
adds `permissions/locationPlacementRoom` and `permissions/locationPlacementCorridor`
to `pf-smoke-editor`, exercising production clipboard, pegman, drag-target and
history seams without windows, native clipboard access or dialogs.
