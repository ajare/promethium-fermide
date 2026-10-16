# Use imnodes as the station graph editor

Status: accepted

The Station Flow view becomes an authoring surface: users build a Station systems
configuration from scratch by placing nodes, dragging links between them, and
editing each node's settings. We adopted **imnodes** (Nelarius, MIT) as the
node-editor library for that surface. It is already vendored in this repository
with its context created at application startup and nothing yet using it, it is
immediate-mode with the caller owning all graph data — which matches how this
editor already draws everything — and it supplies node bodies, input/output
attributes, link drag/drop/detach, multi-selection, minimap, panning and zoom
without imposing a data model of its own.

The connection constraint layer is deliberately **ours, not the library's**.
imnodes offers no per-pin filter callback: it cannot be told which pins are
eligible. That is not a loss. The rules that decide whether two station nodes may
join are hazard-class, thermal-service-class, medium and unit agreements plus
cardinality and exclusivity, and no candidate library expresses them. They live in
`core` beside the existing configuration validator, so the editor's live pin
highlighting and the headless constraint matrix read one implementation.

## Considered options

- **ImNodeFlow** (Fattorino, MIT). Rejected: its typed pins carry `behaviour()`
  lambdas that *evaluate* the graph at runtime. This project's simulation is a
  separate deterministic core; a second execution engine inside the editor would
  either go dead or drift against it. It also renders its own links, has no
  minimap, and ships no serialization. Its `ConnectionFilter` is type-identity
  based and cannot express class or medium segregation, so the constraint layer
  would be ours anyway — with a second rendering stack added.
- **VisualNodeSystem** (Azzinoth, MIT). Rejected: it owns the node, socket and
  area model rather than borrowing ours, brings jsoncpp, GLM and stduuid, sets
  MSVC `/MT` static-runtime defaults, and serializes to **JSON** where this
  feature requires YAML. Its attractive parts — reroute nodes, sub-area nodes,
  link nodes, group comments — are graph-organization features this
  configuration model does not need.
- **Hand-rolled canvas.** Rejected: the existing read-only flow diagram already
  shows how much panning, zooming, hit-testing, edge routing and clipping a
  bespoke canvas costs, and it would still need link dragging, pin hit-testing and
  multi-selection added from scratch.

## Consequences

- The vendored imnodes context stops being dead code. Node and link identity are
  ours; imnodes ids are derived from stable configuration identity, never from
  vector position, so inserting or removing an entry cannot renumber the graph.
- Pin eligibility during a drag is our own draw pass over the pins. The link
  itself is accepted-then-rejected via the library's link-created event rather
  than blocked mid-flight; visually this is indistinguishable from blocking.
- Because the library cannot enforce constraints, it cannot be trusted to have
  produced a legal graph. Every link still passes the same `core` evaluation on
  drop that the headless matrix tests, and configuration validation remains the
  authority at apply time. The descriptor table may be more permissive than the
  validator, never less, so drift fails safe.
- Layout positions become authored presentation data, not library state. imnodes'
  own editor-state save/load (INI format) is not used for persistence; the
  Station configuration document owns positions.
- Replacing the library later means re-implementing the pin-highlighting pass and
  the link-created plumbing, but not the constraint layer — which is the part
  carrying the domain rules and was kept out of the library on purpose.

Recorded by the Station Flow system-builder spec.
