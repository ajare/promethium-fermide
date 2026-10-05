# Lua Furniture catalogue authoring (#461)

This scripted-actions integration slice adds native `.furniture.lua` catalogues.
Select one beside a saved World using **Select Furniture catalogue...**, then
place its definitions through the existing Furniture palette and Location plan.
Instance edits, deletion and catalogue selection use normal document history.

A catalogue returns a table, not YAML or a reference to YAML geometry:

```lua
return {
  api_version = 1,
  uuid = "1c639f04-1183-4e51-9385-86cce6b0f031",
  definitions = {
    {
      key = "desk", label = "Desk", sideRoutes = true,
      tiles = {
        {x = 0, y = 0, imageSet = "ObjectAtlas", image = "chair"},
        {x = 1, y = 0, imageSet = "ObjectAtlas", image = "chair"}
      },
      usablePoints = {},
      vertices = {
        {key = "left", x = 0, external = true},
        {key = "right", x = 2, external = true}
      },
      edges = {{from = "left", to = "right", depthOffset = 0}}
    }
  }
}
```

`src/headless/smoke/fixtures/objects.furniture.lua` is an independent integration
fixture with front/back desk routes, a non-usable table and a scripted chair.
Its desk has neither callback; an optional desk movement point remains an ordinary
owned Marker and does not imply Furniture use.

## Data and function contract

- Catalogue UUID, definition keys and usable-point keys are stable identities.
  Keys contain 1–128 ASCII letters, digits, underscores or hyphens. Definitions
  are a non-empty ordered array of at most 256 records. Lists must be dense arrays;
  unknown fields, mixed table keys, metatables and unsupported values are refused.
- Tiles have integer `x/y`, `imageSet` and `image`. They determine the complete
  artwork rectangle, including gaps. Existing tile bounds, ObjectAtlas resource
  resolution and World-tile-sized artwork requirements remain authoritative.
- `usablePoints` is required but may be empty. Points have `key`, `label`, finite
  `x`, optional floor-height `y = 0` and optional boolean `blocksPathing` (default
  true). Each explicit bound routing vertex must match its point's position.
- Optional `vertices` and `edges` retain explicit private connectivity. Vertices
  have `key`, `x`, optional `y = 0`, `usablePoint` and `external`. Edges have
  `from/to` and optional integer `depthOffset`. `sideRoutes` requires explicit
  routes. Coincidence never creates private routing connections.
- A definition may provide paired `use(agent, world, marker)` and
  `finish_use(agent, world, marker)` Lua functions, or neither. Use requires at
  least one usable point. Non-Lua functions and mutable captured values are
  refused. Stateless captured Lua helper functions may be shared. The argument
  contract is the safe [Marker Action contract](scripted-marker-actions.md), not
  mutable domain bindings. No function overrides exist on instances.

The adapter validates all returned data before accepting a catalogue and retains
an immutable accepted source snapshot, not a live VM or shared closure state.
Module evaluation uses the reusable deterministic sandbox: 256 KiB source,
2 MiB Lua heap, 100,000 instructions and bounded returned-data conversion
(16,384 values, depth 16, array indices up to 4096, strings up to 1024 bytes).
Only the versioned `prometheum.actions.v1` host import is admitted; unrestricted
imports, filesystem/process access and nondeterministic facilities are unavailable.
Caught budget exhaustion still rejects the catalogue.

**Use furniture** is derived automatically for each usable point whose definition
provides the pair. Explicit requests invoke `use` after physical arrival; finish
runs before departure or same-seat Action replacement. Default/explicit Idle does
not use Furniture. The host commits staged Pose/claim effects atomically and
ensures Standing/release cleanup even if finishing fails. See the
[Action workflow](scripted-marker-actions.md).

## Documents and reconciliation

World documents remain YAML/binary. They persist only catalogue basename/UUID and
ordinary authored instance/destination data; no source, functions, closures or VM
state are serialized. Save/reopen reconciles definition/point keys, preserving
owned Marker identities, authored names and properties. Missing dependencies,
changed UUIDs, incompatible placements and removed referenced destinations fail
loading. Existing Floor/Walkway support, Location membership, overlap, Local-depth
and route admission checks are unchanged.

Old-format loading remains **temporary integration-branch migration scaffolding**.
Lua files never fall back to YAML parsing. Bundled catalogues are converted in
#465; paused transactional reload and runtime use are implemented. Regression inputs and builders were converted in #466 and now live under
`src/headless/smoke/fixtures/furniture` (native Lua catalogues and YAML Worlds).
No regression caller uses the old format; production loader removal remains #467.
This is not a promised shipped YAML compatibility path. See
[regression conversion](scripted-actions-regression-fixtures.md).

## Verification

Public World/document check `furniture/luaObjects` exercises real Lua loading,
placement refusal, desk movement on explicit side routes, deferred callback
activation, instance edits, YAML/binary reopen, identity reconciliation, missing
and incompatible dependencies, malformed geometry/function/data contracts,
forbidden imports, source/heap/instruction budgets and stateless shared helpers.
CPU-only editor `furniture/luaWorkflow` covers selection, placement, editing,
refused edits, Undo/Redo, deletion and save/reopen. Renderer `furniture/luaCommands`
uses the existing draw-command workflow for Lua artwork, clipping, movement and
Marker visibility, including two-tile desk rendering. No low-level Lua or Vertex
test API is introduced.

### Final Linux validation

The final source state built the complete default core/headless/editor inventory
in both Release and Debug using supervised `--lane final --build-only all`.
Unfiltered final CTest ran 110 tests per configuration: **103 passed, one optional
GUI capability skipped, six failed**, exclusively for the seven unchanged legacy
implicit-Furniture-use checks documented in [Marker Actions](scripted-marker-actions.md).
All new Lua Furniture checks and affected World, persistence and editor functional,
exhaustive and CLI contracts passed. Full-suite green remains the final integration
slice's contract. `git diff --check` passed; no Windows validation is claimed.

Final build evidence: Release `c37027ff778b4d7c9a760fa022b62765`, Debug
`c6ad456959d74e7285ea2b0f404e547c`. Final unfiltered CTest evidence: Release
`d439b9d70e9c49a8b42f024812b23490`, Debug
`265a8648c1824f9fbbb3d0626b601913`. An earlier concurrent Debug run timed out in
simulation; focused verification and the final lower-concurrency Debug lane
completed with only the documented legacy failures.
