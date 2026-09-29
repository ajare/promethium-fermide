# #232: indexed inferred route sources

## Structure and lifetime

`Graph` builds source indexes after assigning Vertex search slots. Sector identity
keeps Locations on different Layers separate. Each Sector has sorted exact-height
rows (queries retain the existing `abs(dy) <= 0.001` test):

- Candidates sorted by x, carrying their original Sector enumeration ordinal.
  The existing ordinary Floor-run lookup bounds the eligible x interval. Binary
  searches locate that interval and the two nearest sides; distance ties,
  including coincident topology Vertices, resolve by original ordinal.
- Ordinary same-Sector Location edges sorted by left endpoint, in an implicit
  balanced interval tree with subtree maximum right endpoints. Stabbing queries
  prune subtrees and report every strictly containing interval, including nested
  and coincident intervals. Results are sorted into the old arc enumeration
  order before seeding, preserving deterministic equal-cost behaviour.

Neither index includes Force Bridge edges as ordinary Floor. Existing Floor-run
support tests, threshold handling, blocking Marker rules, actual approach costs,
and Transit-specific source selection are unchanged. Endpoints still undergo
both original height-tolerance checks before seeding.

`Graph::build()` clears borrowed interval references before replacing topology.
There is no invalidation on ticks, profile changes, or destination changes. The
containing-result buffer is reserved for every indexed interval during build;
queries need no heap allocation, including when many intervals overlap.

Query work is binary lookup plus nearby distance ties / reported overlaps and
interval-tree traversal, rather than a Location-wide or Graph-wide scan.
Coincident candidates and overlapping intervals necessarily contribute output
work; they are not deduplicated into interchangeable topology Vertices.

## Verification

Run:

```text
cmake --build build-windows --config Release --target prometheum-fermide-headless --parallel
build-windows/bin/x64/Release/prometheum-fermide-headless.exe --routing-scale-checks
build-windows/bin/x64/Release/prometheum-fermide-headless.exe
```

Checks include:

- Independent cell-walking nearest-source oracle, now comparing exact Vertex
  identity in the original Sector order, across bundled Force Bridge, Staircase,
  and Platform lift Worlds. Samples cover integral/fractional x positions and
  positions inside, at, and outside the height tolerance.
- Independent brute-force edge enumeration compared with the complete ordered
  indexed endpoint list, including boundaries. Query scratch capacity is checked.
- Nested, crossing, coincident, and strict-boundary interval-tree cases.
- Increasing unrelated Markers/segments, equal-distance candidate order,
  coincident positions on different Layers, and selection of the farther endpoint
  when it gives the optimal Path.
- Walkway removal/restoration with topology rebuild, including replacement Sector
  identities; the unsupported fractional position loses its source and regains
  it only when Floor support returns.
- Existing threshold, isolated-sector, blocking-Marker, source-equals-target,
  unreachable, serialization, and mixed-population reset checks in the full suite.

## Observations (Windows Release)

Ten repeated inferred Paths plus ten tie-source queries per size. The other
Layer has the same number of unrelated Markers. Times are milliseconds and are
observational, not test thresholds:

| Extra Markers per Layer | Candidates examined | Interval nodes examined | Index payload bytes | Index build ms | Selection ms | Seeding ms | Arc scoring ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 30 | 60 | 5,320 | 0.0184 | 0.0044 | 0.0010 | 0.0161 |
| 128 | 30 | 80 | 17,944 | 0.0727 | 0.0061 | 0.0010 | 0.0625 |
| 512 | 30 | 100 | 87,928 | 0.3122 | 0.0075 | 0.0015 | 0.2482 |

`Graph::getSourceIndexStatistics()` exposes cumulative candidate/interval counts,
reported overlaps, builds, and separate build/selection/seeding/scoring times.
Payload bytes include row records, candidate/interval vector capacities, and the
reserved result buffer; they exclude map-node/allocator overhead and the existing
ordinary Floor-run cache. Build time covers the new indexes, not the rest of Graph
construction. Arc scoring includes cold directed-fact capture when needed.
Selection timing covers successful Location source queries; Transit fallback and
early rejected unsupported queries are not timed. Seeding timing includes lookup,
ordered results, actual approach distances, and insertion into the frontier.

The mixed 1,000-Agent / 2,040-Vertex scenario retained the exact pre-change complete
Path/cumulative-cost digest **16756504350970399886**, cold, warm, reset, and in a
fresh World. Warm throughput was about 12,338 Paths/s versus 10,342 before the
change (observational). Warm scratch capacity remained unchanged. Workspace
payload fell from 645,848 to 580,376 bytes after removing the scanned floor-arc
list; the new index payload is separately reported as 175,616 bytes.

The full Release headless suite passed, including deterministic simulation
snapshot/event digests. No graphical manual test was performed.
