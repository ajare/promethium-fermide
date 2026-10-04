# Authored Dumbwaiters (#375)

A Dumbwaiter is a non-passenger, fixed 1-cell-wide, 2-Level-high Transit. Paint
**Dumbwaiter** on a shaft Layer behind two supported landing cells at the same x
position. Rooms, Corridors and Facades on the immediately front Layer can supply
one shared multi-Level Location or two separate Locations. Both cells need Ground
or Walkway support; Force Bridges, occupied shafts and conflicting apertures refuse
placement without changing the World. No neighbouring button cell is required.

Selection on the shaft Layer exposes **Initial Stop** (Lower by default),
**Travel time (seconds)** (2 by default, inclusive 0.1–60), and **Delete Dumbwaiter**.
Creation, configuration and whole-unit deletion participate in ordinary document
undo/redo. The car starts at the authored Stop with that shutter fully open and
the other closed. The two small landing buttons indicate here/elsewhere but are
not operational; there is no enabled press action, Agent control, passenger path,
queue, journey or traversal resource. Car geometry is procedural and reuses the
existing shaft surface and BoothWindow resources.

Owned apertures have no back-side panel and cannot be independently edited,
resized, moved, copied, deleted or toggled. Delete the complete unit first when
changing its landing Location footprints or removing Levels/Layers. Independent
owned-aperture movement is refused pending full unit movement support; unrelated
objects retain normal editing. Removing required Walkway support is transactionally
refused. Full unit movement,
clipboard and operational cycles belong to follow-up tickets, not this slice.

World schema **47** adds one `dumbwaiter` construction record carrying stable
World-owned `id`, shaft `layer`, lower `y`, `x`, `initialStop` (0/1) and
`travelSeconds`, plus the World identity high-water mark `nextDumbwaiterId`.
Children and shutter state are derived, not independently serialized. YAML and
binary use the same ordinary document validation and replay; schema-46 and older
Worlds remain supported. Loading and Reset restore the authored presentation.

Headless coverage is in World, Persistence, Editor and Render's BoothWindows
translation units, using real public World APIs, document history/Selection, and
production World draw commands. Standalone BoothWindow and passenger Lift coverage
remains in the existing suites.
