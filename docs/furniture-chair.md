# Catalogue-backed chair (#348)

This first Furniture slice supports one tile at `(0, 0)`, one usable point, and
fixed-depth-0 routing. Multi-tile layouts, Local-depth editing, explicit side
routes and catalogue migration are later tickets, not silently
approximated here. There is no sitting state or seat reservation.

## Authoring

Save a World, put a manually authored `.furniture.yaml` catalogue beside it, then
expand **Furniture** in the World panel. Enter its basename and load it, select a
definition, select a Room, Corridor or Facade on the canvas, enter the instance
name and Location-local x/supporting Level, and click **Place Furniture** while
paused. The optional snap toggle rounds x only; y must always be floor-aligned.
Refusals appear inline, without dialogs. Catalogue selection and placement use
normal document history.

`resources/test-worlds/chair.furniture.yaml` describes the format. Catalogue UUID,
definition key and usable-point key are reference identities, not display labels.
Artwork names an existing `ObjectAtlas` Image-set region; the application resource
manager resolves it and owns its atlas. The geometric placeholder chair is a full
World-tile region in `resources/Resources.yaml`. No Furniture TileSet is created.
The GUI registers user-selected catalogues dynamically as FurnitureCatalogue
resources; headless document tools use the same parser without graphics resources.

## Placement and destinations

The whole one-tile footprint must fit inside one Location and have continuous
Ground or Walkway support. Fractional x, boundary-touching chairs and elevated
Walkways are supported; gaps, overhangs, fractional y, non-finite positions,
same-depth overlap, Backgrounds and Transits are refused before mutation.

A placed chair owns one independently named World Marker. Its initial name is
`<instance name> <usable label>`, validated in the existing Marker namespace. It
appears in the ordinary behaviour destination selectors and defaults to **Blocks
pathing**. The graph connects it as a destination branch off an ordinary floor
anchor, so it remains reachable and departable without severing circulation.
Independent movement or deletion of its owned Marker is refused.

## Safe instance edits (#349)

Select an existing instance in the **Furniture instance** selector in the World
panel. While paused, change its name, Location-local x or supporting Level and
click **Apply Furniture edit**, or click **Delete Furniture**. Edits and deletion
use ordinary document undo/redo. Movement stays inside the owning Location and
rigidly translates artwork and the definition's owned point; it cannot cross a
Floor/Walkway gap, overhang the Location or overlap another chair.

Instance names generate point names only at placement. Renaming an instance does
not rewrite its Marker name. Rename the owned Marker through ordinary **Marker
Selection**; names use the same World namespace as standalone Markers, and
behaviour references keep the same identity. Markers cannot be independently
moved or deleted. Agents stay at their physical positions when Furniture moves;
existing destination intent follows the same Marker through ordinary Route
planning and Route loss handling, not a teleport or new Furniture movement API.

Deletion reports all Agent behaviour configuration references to the owned
Marker and refuses without mutation or history. Unreferenced deletion removes
both instance and destination, preserving other authored object slots and never
reusing the deleted identities. Walkway removal, Location deletion and resizing
that would crop or invalidate Furniture refuse with blocking instance names.
Delete or relocate the blocking Furniture before removing its support. Non-zero
Local-depth lifecycle behaviour remains a later topology ticket.

## Documents

World schema 43 adds a catalogue basename/expected UUID reference, instance
identity/name/placement/definition key, usable-point key and Marker identity/name/
properties, and a non-reused instance identity allocator. YAML `.world.yaml` and
binary `.world` documents use the same construction records. They resolve the
current catalogue on reopening, not an embedded definition snapshot. Worlds
without Furniture have no catalogue dependency. Move the World and its catalogue
together to preserve portable references; a missing catalogue, missing definition,
removed usable point, UUID mismatch or invalid placement is a diagnostic failure.

`resources/test-worlds/chair.world.yaml` demonstrates chairs in all three supported
Location types and an Agent visiting the reading chair. Reaching its Marker uses
ordinary movement, not a new Furniture behaviour API.

## Headless checks

- `pf-smoke-world --check furniture/chair`
- `pf-smoke-persistence --check furniture/documents`
- `pf-smoke-render --check furniture/chairCommands`
- `pf-smoke-editor --check furniture/chairActions`

The checks use the existing World/document, CPU draw-command, and production
editor-action seams, isolated temporary roots and deterministic simulation ticks.
They require no OS windows, dialogs or GPU.
