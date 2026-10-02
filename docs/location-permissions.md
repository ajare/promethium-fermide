# Static Location permission requirements (#273)

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

## Scope and verification

This is the **static-authorization slice**: grants and requirements remain stable
through the journey. Reading current effective grants at initial route choice does
not implement runtime grant/requirement-change reactions, entry-time lifecycle
hardening, or a general unauthorized-occupancy policy; those are follow-up tickets
under #272. In particular, do not rely on this slice to revoke an existing Path or
interrupt an underway entry after an authorization change.

Headless coverage is owned by `pf-smoke-permissions` (`locationAuthoring*`,
`locationRouting*`, `locationAlternative*`, `locationBoundary*`) and `pf-smoke-editor`
(`permissions/locationSelectionRoom`, `permissions/locationSelectionCorridor`).
The editor checks drive the production Selection checklist with CPU-only ImGui
mouse events and restore real document-history snapshots; they open no desktop
window, native dialog, or clipboard integration.
