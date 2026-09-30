# Generate a populated World

`scripts/generate_new_world.cpp` is a standalone C++20 program built against this
repository's simulation core. It constructs everything through the World API;
it does **not** read or copy the existing `new-world.world`.

## Build

Use an already configured project build:

```sh
cmake --build build-windows --config Release --target pf-generate-world
```

For a fresh build, use the project's normal CMake configuration first. The target
has no GUI dependency and also works with Linux/headless configurations.

## Run

Windows (from the repository root):

```sh
# New directory: leaves the hand-authored World untouched.
build-windows/bin/x64/Release/pf-generate-world.exe \
  --output build-windows/generated/new-world.world

# Reproduce a layout/population using a printed seed.
build-windows/bin/x64/Release/pf-generate-world.exe \
  --seed 42 --output build-windows/generated/seed-42.world

# Explicitly replace the requested World AND its adjacent behaviour package.
# Close the World in the editor first.
build-windows/bin/x64/Release/pf-generate-world.exe --force
```

The examples use shell line continuations; in PowerShell/cmd, enter each command
on one line instead. On Linux, use `build/bin/x64/Release/pf-generate-world` (or
the corresponding path in your chosen build directory).

Without `--seed`, a fresh seed is generated and printed. `--seed` reproduces
layout choices, names, tag choices, and population with the same generator and
input assets. Each generated behaviour package receives a fresh UUID, so binary
files are **not byte-identical** even when their generated content matches.

Options:

| Option | Default / purpose |
|---|---|
| `--output FILE.world` | Repository `resources/test-worlds/new-world.world` |
| `--seed UINT64` | Fresh random seed |
| `--force` | Permit replacing the destination World and its entire associated behaviour directory |
| `--tags FILE.tags.yaml` | Repository `resources/test-worlds/test.tags.yaml` |
| `--behaviour FILE.lua` | Repository `resources/test-worlds/new-world.behaviours/random-marker-wander.lua` |
| `--help` | Usage |

Default asset paths are compiled from the source directory, so the CMake-built
executable may be launched from another working directory. If distributing the
executable without its source checkout, supply `--tags`, `--behaviour`, and
`--output` explicitly.

## Generated files and safety

For `example.world`, the generator writes:

- `example.world`: the binary World.
- `example.behaviours/behaviours.yaml`: a new registry, with all generated Markers
  included in the wandering behaviour's default destination list.
- `example.behaviours/random-marker-wander.lua`: a copy of the supplied Lua source.
- `test.tags.yaml`: a copy of the supplied tag registry, **only if absent**.

The tag registry must define `male` and `female`. An existing destination
`test.tags.yaml` must match the input file exactly; the generator never overwrites
it. The behaviour script must implement the `markers` list configuration used by
Random Marker Wander. Existing World/package outputs are refused without
`--force`; symlink outputs are refused even with it.

All generation, dependency validation, binary readback, routing checks, and a
four-second Lua simulation smoke test happen in a temporary sibling directory.
Only a fully validated result is published. The referencing World is installed
last; existing World/package files are backed up during publication and restored
if publication fails. A rollback failure preserves the recovery directory and
prints its path. The smoke test runs on a separate loaded copy: saved Agents
remain in their original starting positions and the World remains paused.

## Layout rules

- World: **128 × 64**, **2 Layers**. The building occupies **x=1–14**,
  **levels 3–34**, leaving levels 0–2 empty.
- Front Layer: alternating 2/3 Corridors per level, with approximately one-cell
  random width variations and continuous coverage of the building width.
- Back Layer: **8 Lifts** in four overlapping height bands; exactly two start
  at the building's base, exactly one is two cells wide. Every shaft spans at
  least six levels and serves at least half its levels. Stops are randomized,
  with missing coverage repaired. Two levels deliberately have no Lift stop
  and are served by stationary Staircases instead.
- **3 Stairwells**, respectively 2/3/4 levels high.
- **6 Staircases**, including **3 upward Escalators**, distributed across local
  levels 2–3, 5–6, 13–14, 18–19, 26–27, and 30–31 (add 3 for World levels).
- **2 fixed Ladders**, each two levels high, chosen to maximize clearance from
  existing transit footprints and separated vertically from one another.
- Remaining back-Layer space is packed with uniquely named **2–5-cell-wide
  Rooms**. Most are one level high; some are two or three. Single-cell remnants
  can remain because Rooms must be at least two cells wide.
- Room ground-level footprints overlap no more than two Corridors. This avoids
  the earlier conflict between a three-Corridor Room and a two-Door maximum,
  without needing to merge Corridors after construction.
- Each Room gets **one upward-opening Door per overlapping ground-level
  Corridor**, hence one or two Doors. About 25% (rounded to a whole Door) have
  buttons on both sides. Two Doors in the same Room always serve different
  Corridors, so there are no adjacent same-Corridor Door pairs.
- **2 Bulkhead Doors** at internal Corridor boundaries, with separate Door exits
  on both sides. The boundaries have at least one cell of clearance from other
  Door footprints and two cells from transit footprints on that level.
- **One named Marker per Room**, on its lowest level. Doorway cells are avoided
  whenever the Room has any doorway-free ground-level cell.
- **320 Agents**, ten per building level, with horizontally spaced centres at
  least one cell apart. Agents are placed in Corridors or on Room ground floors,
  not suspended on upper levels of tall Rooms.
- Each Agent receives exactly one randomly chosen `male` or `female` tag and
  the Random Marker Wander behaviour, configured with every generated Marker.

The transport height bands form a fixed backbone; corridor widths, lift stops,
room packing/names, doors/buttons, ladder tie-breaks, markers, and population vary.
Candidate layouts that violate placement rules or fail the real pathfinder's
**ground-to-every-level-and-back** check are discarded. Up to 200 attempts use
seed-derived random streams; exhaustion fails without replacing existing output.
The guarantee is reachability of every level, not every isolated Corridor or
Marker. The wandering behaviour handles unreachable destinations by trying
alternatives and eventually idling if none are possible.

## Tests

```sh
ctest --test-dir build-windows -C Release -R '^world-generator-' --output-on-failure
```

Four CTest cases generate seeds 0, 1, 42, and 2026 into separate build-directory
fixtures. Every run checks geometry bounds, transport counts, lift coverage,
room/door/button/marker rules, population spacing, tag/behaviour assignments,
round-trip level reachability, binary reopening, and live behaviour execution.
A fifth case checks CLI validation, overwrite refusal, explicit replacement,
and preservation of existing outputs when Lua validation fails.
The repository's existing `resources/test-worlds/new-world.world` is not touched
by these tests.
